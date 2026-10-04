"""Federated procurement MCP server — integration-only.

No scrapers are built here. Every marketplace is reached via a hosted API
you call with an API key. Chinese sources via TMAPI / AliExpress Affiliate,
Australian sources via eBay Browse, Amazon PA-API, and hosted scraper-APIs
for Gumtree / Facebook (SociaVault etc.) — all consumed as HTTP APIs.

Database: optional Postgres on 10.1.1.3 (fleet store, jupiter db). When
DATABASE_URL is set, federated results are cached and searches are logged.
When unset, the server runs stateless (in-memory only).

Run:
  uv sync
  cp .env.example .env  # add keys for io+agent@jupiter.au + DATABASE_URL
  uv run mcp dev server.py            # Inspector
  uv run python server.py             # stdio (for Claude Desktop)
  uv run mcp install server.py        # install into Claude Desktop
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import logging
import os
import re
import time
import xml.etree.ElementTree as ET
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Annotated, Any, Literal

import httpx
from dotenv import load_dotenv
from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field

load_dotenv()

# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------


class Shipping(BaseModel):
    domestic_cny: float | None = Field(default=None, description="Domestic leg in CNY if known")
    intl_estimate_aud: float | None = Field(default=None, description="Estimated international shipping in AUD")
    eta_days: list[int] | None = Field(default=None, description="ETA window [min, max] days")
    method: str | None = None


class Seller(BaseModel):
    shop_name: str | None = None
    shop_id: str | None = None
    rating: float | None = None
    verified: bool | None = None


class AuctionInfo(BaseModel):
    """Live-auction metadata for eBay AUCTION-format listings.

    price_aud on the parent Offer carries the CURRENT BID (converted), not a
    fixed price — this block makes that explicit and adds the bidding state.
    """
    current_bid_aud: float | None = Field(default=None, description="Current bid in AUD (price_aud mirror)")
    bid_count: int | None = Field(default=None, description="Number of bids so far")
    end_time: str | None = Field(default=None, description="Auction end ISO datetime (UTC)")
    time_left: str | None = Field(default=None, description="Humanised time remaining, e.g. '2d 4h'")
    reserve_met: bool | None = Field(default=None, description="Whether the reserve price has been met, if eBay reports it")


class CommunitySignal(BaseModel):
    """Deal-site community metadata — OzBargain / Cheapies style.

    Populated from RSS ozb:meta / Apify deal scrapes. Distinct from seller
    trust: this is *crowd vetting of the price itself* (votes, clicks), not
    merchant reputation.
    """
    votes_pos: int | None = None
    votes_neg: int | None = None
    comment_count: int | None = None
    click_count: int | None = None
    expiry: str | None = Field(default=None, description="Deal expiry ISO datetime, if the source sets one")
    brand: str | None = None
    tags: list[str] | None = None


class Offer(BaseModel):
    offer_id: str = Field(description="Stable id marketplace:product:sku, e.g. 1688:123456:sku147")
    marketplace: str = Field(description="aliexpress | taobao | tmall | 1688 | jd | pdd | ebay_au | amazon_au | gumtree | facebook | ozbargain | cheapies")
    title: str
    title_en: str | None = None
    url: str | None = None
    image: str | None = None
    price_cny: float | None = None
    price_aud: float | None = None
    currency: str = Field(default="AUD", description="price_aud currency")
    moq: int | None = Field(default=None, description="Minimum order quantity — 1688 wholesale")
    unit: str | None = None
    stock: int | None = None
    regular_price_aud: float | None = Field(default=None, description="Pre-special regular price (grocery specials) — price_aud carries the effective/discounted price")
    unit_price_aud: float | None = Field(default=None, description="Unit price in AUD per the unit below (grocery: $/100g, $/L …) — the true cheapest comparator")
    shipping: Shipping = Field(default_factory=Shipping)
    seller: Seller = Field(default_factory=Seller)
    community: CommunitySignal | None = Field(default=None, description="Deal-site votes/expiry/brand — OzBargain, Cheapies")
    auction: AuctionInfo | None = Field(default=None, description="Live-auction metadata when the listing is AUCTION format (price_aud = current bid)")
    seller_country: str | None = Field(default=None, description="Item/seller country code (eBay itemLocationCountry) when known — global searches populate this")
    price_history: dict[str, Any] | None = Field(default=None, description="Keepa/sold-comps price-history summary")
    source_reliability: Literal["authoritative", "aggregator", "best_effort"] = "aggregator"
    raw: dict[str, Any] | None = Field(default=None, description="Raw marketplace payload for debugging")


# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

# Chinese / aggregator
TMAPI_TOKEN = os.getenv("TMAPI_TOKEN") or os.getenv("TMAPI_API_TOKEN")
ELIM_API_TOKEN = os.getenv("ELIM_API_TOKEN")
OTCOMMERCE_RAPIDAPI_KEY = os.getenv("OTCOMMERCE_RAPIDAPI_KEY") or os.getenv("RAPIDAPI_KEY")

# AliExpress Affiliate (official)
ALIEXPRESS_APP_KEY = os.getenv("ALIEXPRESS_APP_KEY")
ALIEXPRESS_APP_SECRET = os.getenv("ALIEXPRESS_APP_SECRET")

# Australian
EBAY_APP_ID = os.getenv("EBAY_APP_ID") or os.getenv("EBAY_CLIENT_ID")
EBAY_CERT_ID = os.getenv("EBAY_CERT_ID") or os.getenv("EBAY_CLIENT_SECRET")
EBAY_OAUTH_TOKEN = os.getenv("EBAY_OAUTH_TOKEN")  # optional: pre-fetched bearer
AMAZON_PAAPI_ACCESS_KEY = os.getenv("AMAZON_PAAPI_ACCESS_KEY")
AMAZON_PAAPI_SECRET_KEY = os.getenv("AMAZON_PAAPI_SECRET_KEY")
AMAZON_PAAPI_PARTNER_TAG = os.getenv("AMAZON_PAAPI_PARTNER_TAG")  # e.g. jupiter-22

# Hosted scraper-APIs (you don't build these — you call them)
SOCIAVAULT_API_KEY = os.getenv("SOCIAVAULT_API_KEY")
APIFY_TOKEN = os.getenv("APIFY_TOKEN")  # for Gumtree / JD / OzBargain / Amazon / eBay-sold actors
GUMTREE_APIFY_ACTOR = os.getenv("GUMTREE_APIFY_ACTOR", "crawlerbros/gumtree-scraper")
# eBay sold-price comps (supports ebay.com.au) and Amazon AU fallback (no
# PA-API/SP-API credentials needed — hosted actors).
EBAY_SOLD_APIFY_ACTOR = os.getenv("EBAY_SOLD_APIFY_ACTOR", "caffein.dev/ebay-sold-listings")
AMAZON_APIFY_ACTOR = os.getenv("AMAZON_APIFY_ACTOR", "junglee/free-amazon-product-scraper")
# Reverse image search across 1688/Alibaba/AliExpress/global — sourcing by photo.
IMAGE_SEARCH_APIFY_ACTOR = os.getenv("IMAGE_SEARCH_APIFY_ACTOR", "dev00/alibaba-1688-aliexpress-reverse-image-search-api")
# AU grocery (dromb series: search op, unit pricing, specials). Costco
# deliberately EXCLUDED — every costco actor on the store targets costco.com
# (US, USD, US shipping); no AU-domain support, so it would pollute the
# federation with non-comparable US prices. Revisit if an AU Costco actor
# ever appears.
# Chinese keyword search — zen-studio hosted actors (TMAPI has no keyword
# search except 1688; these cover taobao/tmall and jd). Verified live
# 2026-09-02: taobao ¥2.68 spatula, jd Logitech keyboard rows.
# NOTE: maxItems must be >= 10 per actor input validation.
TAOBAO_APIFY_ACTOR = os.getenv("TAOBAO_APIFY_ACTOR", "zen-studio/taobao-search-scraper")
JD_APIFY_ACTOR = os.getenv("JD_APIFY_ACTOR", "zen-studio/jd-com-search-scraper")
# 1688 wholesale fallback when TMAPI is unsubscribed (verified actor from research):
ALI1688_APIFY_ACTOR = os.getenv("ALI1688_APIFY_ACTOR", "zen-studio/1688-wholesale-scraper")
WOOLWORTHS_APIFY_ACTOR = os.getenv("WOOLWORTHS_APIFY_ACTOR", "dromb/woolworths-au-product-search-catalog-unofficial")
COLES_APIFY_ACTOR = os.getenv("COLES_APIFY_ACTOR", "dromb/coles-au-product-search-specials-stores-unofficial")
ALDI_APIFY_ACTOR = os.getenv("ALDI_APIFY_ACTOR", "dromb/aldi-au-product-search-catalog-unofficial")

# Deal feeds — no keys, official RSS (OzBargain/Cheapies publish these)
OZBARGAIN_FEED_URL = os.getenv("OZBARGAIN_FEED_URL", "https://www.ozbargain.com.au/deals/feed")
CHEAPIES_FEED_URL = os.getenv("CHEAPIES_FEED_URL", "https://www.cheapies.nz/deals/feed")

# Keepa — official paid API for Amazon price history (covers amazon.com.au)
KEEPA_API_KEY = os.getenv("KEEPA_API_KEY")

# eBay marketplace account deletion/closure notifications compliance.
# A NEW production keyset stays DISABLED (token endpoint → 401 invalid_client)
# until this flow is completed in the developer portal. The "subscribe" path
# needs a public HTTPS endpoint that answers the GET challenge and ACKs the
# POST notifications — both implemented below as FastMCP custom routes.
# Configure in the portal (Application Keys → Production → notifications):
#   Endpoint URL:        https://procurement.jupiter.au/ebay/notifications
#   Verification token:  the EBAY_DELETION_TOKEN value from sops/env
EBAY_DELETION_TOKEN = os.getenv("EBAY_DELETION_TOKEN")
# The EXACT public URL registered with eBay — the SHA-256 hashes the endpoint
# string itself, so it must match byte-for-byte what's in the portal.
EBAY_DELETION_ENDPOINT = os.getenv("EBAY_DELETION_ENDPOINT", "https://procurement.jupiter.au/ebay/notifications")

# Postgres — fleet store on callisto 10.1.1.3, jupiter db per stack law
DATABASE_URL = os.getenv("DATABASE_URL")  # e.g. postgresql://procurement:***@10.1.1.3:5432/jupiter
PG_SCHEMA = os.getenv("PROCUREMENT_PG_SCHEMA", "procurement")

# Behaviour
CNY_TO_AUD = float(os.getenv("CNY_TO_AUD", "0.215"))
DEFAULT_TIMEOUT_S = float(os.getenv("PROCUREMENT_TIMEOUT_S", "8"))
CACHE_TTL_S = int(os.getenv("PROCUREMENT_CACHE_TTL_S", "3600"))

# ---------------------------------------------------------------------------
# Postgres cache (optional — stateless fallback if DATABASE_URL unset)
# ---------------------------------------------------------------------------

_pool: Any | None = None  # asyncpg.Pool | None
_db_enabled: bool = False
_db_error: str | None = None

DDL = """
CREATE SCHEMA IF NOT EXISTS {schema};
CREATE TABLE IF NOT EXISTS {schema}.search_cache (
  cache_key TEXT PRIMARY KEY,
  query TEXT NOT NULL,
  marketplaces TEXT[],
  sort TEXT,
  qty INT,
  ship_to TEXT,
  offers JSONB NOT NULL,
  created_at TIMESTAMPTZ DEFAULT now(),
  expires_at TIMESTAMPTZ NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_search_cache_expires ON {schema}.search_cache (expires_at);
CREATE TABLE IF NOT EXISTS {schema}.search_log (
  id BIGSERIAL PRIMARY KEY,
  query TEXT NOT NULL,
  marketplaces TEXT[],
  sort TEXT,
  qty INT,
  ship_to TEXT,
  result_count INT,
  elapsed_ms INT,
  created_at TIMESTAMPTZ DEFAULT now()
);
"""


def _cache_key(query: str, marketplaces: list[str], sort: str, qty: int, ship_to: str, max_results: int, ebay_auctions: bool = False, ebay_seller_scope: str = "domestic") -> str:
    raw = json.dumps({"q": query.strip().lower(), "mps": sorted(marketplaces), "sort": sort, "qty": qty, "ship_to": ship_to, "max": max_results, "ebay_auction": ebay_auctions, "ebay_scope": ebay_seller_scope}, sort_keys=True)
    return hashlib.sha256(raw.encode()).hexdigest()[:32]


async def _init_db() -> None:
    global _pool, _db_enabled, _db_error
    if not DATABASE_URL:
        _db_enabled = False
        return
    try:
        import asyncpg  # type: ignore[import-untyped]

        _pool = await asyncpg.create_pool(DATABASE_URL, min_size=1, max_size=5, timeout=5)
        assert _pool is not None
        ddl = DDL.format(schema=PG_SCHEMA)
        async with _pool.acquire() as conn:
            await conn.execute(ddl)
        _db_enabled = True
        _db_error = None
    except Exception as e:
        _db_enabled = False
        _db_error = str(e)


async def _close_db() -> None:
    global _pool
    if _pool is not None:
        try:
            await _pool.close()
        except Exception:
            pass
        _pool = None


async def _db_get_cached(cache_key: str) -> list[Offer] | None:
    if not _db_enabled or _pool is None:
        return None
    try:
        async with _pool.acquire() as conn:
            row = await conn.fetchrow(
                f"SELECT offers FROM {PG_SCHEMA}.search_cache WHERE cache_key=$1 AND expires_at > now()", cache_key
            )
            if not row:
                return None
            data = row["offers"]
            if isinstance(data, str):
                data = json.loads(data)
            return [Offer.model_validate(o) for o in data]
    except Exception:
        return None


async def _db_set_cached(cache_key: str, query: str, marketplaces: list[str], sort: str, qty: int, ship_to: str, offers: list[Offer]) -> None:
    if not _db_enabled or _pool is None:
        return
    try:
        payload = json.dumps([o.model_dump() for o in offers])
        async with _pool.acquire() as conn:
            await conn.execute(
                f"""
                INSERT INTO {PG_SCHEMA}.search_cache (cache_key, query, marketplaces, sort, qty, ship_to, offers, expires_at)
                VALUES ($1,$2,$3,$4,$5,$6,$7::jsonb, now() + make_interval(secs => $8))
                ON CONFLICT (cache_key) DO UPDATE SET offers=EXCLUDED.offers, expires_at=EXCLUDED.expires_at, created_at=now()
                """,
                cache_key,
                query,
                marketplaces,
                sort,
                qty,
                ship_to,
                payload,
                CACHE_TTL_S,
            )
    except Exception:
        # Never block a search on cache failure — but leave a breadcrumb.
        logging.getLogger("procurement").warning("search_cache put failed", exc_info=True)


async def _db_log_search(query: str, marketplaces: list[str], sort: str, qty: int, ship_to: str, result_count: int, elapsed_ms: int) -> None:
    if not _db_enabled or _pool is None:
        return
    try:
        async with _pool.acquire() as conn:
            await conn.execute(
                f"INSERT INTO {PG_SCHEMA}.search_log (query, marketplaces, sort, qty, ship_to, result_count, elapsed_ms) VALUES ($1,$2,$3,$4,$5,$6,$7)",
                query,
                marketplaces,
                sort,
                qty,
                ship_to,
                result_count,
                elapsed_ms,
            )
    except Exception:
        pass


# ---------------------------------------------------------------------------
# FastMCP with lifespan (DB init / close)
# ---------------------------------------------------------------------------


@asynccontextmanager
async def _lifespan(_server: FastMCP) -> AsyncIterator[None]:
    await _init_db()
    try:
        yield
    finally:
        await _close_db()


mcp = FastMCP(
    "procurement-search",
    instructions=(
        "Federated procurement search across Chinese (AliExpress, Taobao/Tmall, 1688, JD, PDD) "
        "and Australian (eBay AU, Amazon AU, Gumtree, Facebook Marketplace) sources. "
        "All sources are reached via hosted APIs — no scrapers are built in this server. "
        "Use search_offers for 'cheapest / fastest / best value <item>' queries. "
        "Results are cached in Postgres on 10.1.1.3 when DATABASE_URL is set."
    ),
    lifespan=_lifespan,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _has_any_key(keys: list[str | None]) -> bool:
    return any(k for k in keys if k)


def _cny_to_aud(cny: float | None) -> float | None:
    if cny is None:
        return None
    return round(cny * CNY_TO_AUD, 2)


# TMAPI real endpoint map (verified against tmapi.top doc tree 2026-09-02):
#   alibaba/1688: /alibaba/search/items (keyword search!) + /alibaba/item_detail_by_url
#   taobao/tmall: /taobao/item_detail (by id) — NO keyword search on TMAPI
#   pdd:         /pdd/item_detail (by id) — NO keyword search
#   jd:          /jd/item_detail (by id) — NO keyword search
# Chinese keyword search therefore runs ONLY for 1688; the other lanes are
# detail-only (used by get_offer_detail when we hold an item id/url).
_TMAPI_DETAIL_PATH = {
    "taobao": "taobao/item_detail",
    "tmall": "taobao/item_detail",
    "1688": "alibaba/item_detail_by_url",
    "pdd": "pdd/item_detail",
    "jd": "jd/item_detail",
}


async def _fetch_tmapi(item_id: str, marketplace: str) -> dict[str, Any] | None:
    if not TMAPI_TOKEN:
        return None
    mp_path = _TMAPI_DETAIL_PATH.get(marketplace)
    if not mp_path:
        return None
    url = f"https://api.tmapi.top/{mp_path}"
    async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
        r = await client.get(url, params={"apiToken": TMAPI_TOKEN, "item_id": item_id})
        r.raise_for_status()
        return r.json()


# ---------------------------------------------------------------------------
# Shared Apify plumbing
# ---------------------------------------------------------------------------

_PRODUCT_ID_KEYS = ("itemId", "skuId", "asin", "id", "sku", "barcode", "productId", "url", "productUrl")


def _split_envelopes(items: Any) -> tuple[list[dict], str | None]:
    """Separate Apify actor error envelopes from real product items.

    Quota/billing notices arrive as {"error": ..., "message": ...} dicts
    with no product id (observed live: free_tier_exhausted from
    zen-studio/taobao-search-scraper after its 10 free runs). Returns
    (clean_items, first_error_message). Callers: no clean items + a message
    → one honest diagnostic offer; envelopes mixed with real items → skip.
    """
    if not isinstance(items, list):
        return [], None
    clean: list[dict] = []
    first_err: str | None = None
    for it in items:
        if isinstance(it, dict) and "error" in it and not any(it.get(k) for k in _PRODUCT_ID_KEYS):
            if first_err is None:
                first_err = str(it.get("message") or it.get("error"))[:200]
            continue
        clean.append(it)
    return clean, first_err


async def _search_tmapi(query: str, marketplace: str, max_results: int) -> list[Offer]:
    """Chinese search via TMAPI. REALITY (per tmapi.top docs): only 1688
    (alibaba) has a keyword-search endpoint; taobao/tmall/jd/pdd are
    detail-only. Those lanes surface an honest skip note instead of
    pretending to search. 4013 = API not subscribed in the console
    (https://console.tmapi.io → APIs List → Subscribe) — surfaced verbatim.
    """
    if not TMAPI_TOKEN:
        return []
    if marketplace not in ("1688", "alibaba"):
        return [
            Offer(
                offer_id=f"{marketplace}:unsupported",
                marketplace=marketplace,
                title=f"[{marketplace}] keyword search not available on TMAPI (detail-only API) — use get_offer_detail with an item id",
                source_reliability="aggregator",
                raw={"reason": "TMAPI has no keyword-search endpoint for this marketplace; only 1688/alibaba does"},
            )
        ]
    url = "https://api.tmapi.top/alibaba/search/items"
    try:
        async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
            r = await client.get(url, params={"apiToken": TMAPI_TOKEN, "keywords": query, "page": 1, "sort": "relevance"})
            if r.status_code == 401:
                return [Offer(offer_id="1688:unsubscribed", marketplace="1688", title="[1688] TMAPI error 4013: API not subscribed — subscribe at console.tmapi.io → APIs List", source_reliability="aggregator", raw={"error": r.text[:200]})]
            r.raise_for_status()
            data = r.json()
            items = (data.get("data") or {}).get("products") or (data.get("data") or {}).get("items") or []
            offers: list[Offer] = []
            for it in items[:max_results]:
                # alibaba search item shape (from docs): product title/subject,
                # price ranges, trade info, supplier company
                price_cny = None
                for k in ("price", "min_price", "price_range_min"):
                    v = it.get(k)
                    if v is not None:
                        try:
                            price_cny = round(float(str(v).replace("¥", "").strip() or 0), 2) or None
                            if price_cny:
                                break
                        except ValueError:
                            continue
                offers.append(
                    Offer(
                        offer_id=f"1688:{it.get('product_id') or it.get('id') or 'unknown'}",
                        marketplace="1688",
                        title=str(it.get("title") or it.get("subject") or it.get("product_name") or query),
                        url=it.get("detail_url") or it.get("product_url") or it.get("url"),
                        image=it.get("image") or it.get("pic") or (it.get("images") or [None])[0],
                        price_cny=price_cny,
                        price_aud=_cny_to_aud(price_cny),
                        moq=it.get("min_order") or it.get("moq"),
                        seller=Seller(shop_name=it.get("company_name") or it.get("supplier_name")),
                        source_reliability="aggregator",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [
            Offer(
                offer_id=f"{marketplace}:error",
                marketplace=marketplace,
                title=f"[{marketplace}] search error: {e}",
                price_aud=None,
                source_reliability="aggregator",
                raw={"error": str(e)},
            )
        ]


# Cached eBay application token: (token, expiry_epoch). Module-level so the
# global in _get_ebay_token resolves on FIRST call (a refactor dropped this
# declaration once — NameError killed the whole lane — never remove it).
_EBAY_TOKEN_CACHE: tuple[str, float] | None = None


async def _get_ebay_token() -> str | None:
    global _EBAY_TOKEN_CACHE
    if EBAY_OAUTH_TOKEN:
        return EBAY_OAUTH_TOKEN
    if not _has_any_key([EBAY_APP_ID, EBAY_CERT_ID]):
        return None
    # cached token still valid?
    if _EBAY_TOKEN_CACHE and _EBAY_TOKEN_CACHE[1] > time.time():
        return _EBAY_TOKEN_CACHE[0]
    # NOTE: the endpoint is /identity/v1/oauth2/token (the legacy
    # /oauth/token path 404s). Token cached until expiry minus 60s margin
    # so each search doesn't re-fetch.
    creds = base64.b64encode(f"{EBAY_APP_ID}:{EBAY_CERT_ID}".encode()).decode()
    async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
        r = await client.post(
            "https://api.ebay.com/identity/v1/oauth2/token",
            headers={"Authorization": f"Basic {creds}", "Content-Type": "application/x-www-form-urlencoded"},
            data={"grant_type": "client_credentials", "scope": "https://api.ebay.com/oauth/api_scope"},
        )
        r.raise_for_status()
        tok = r.json().get("access_token")
        exp = r.json().get("expires_in") or 0
        if tok and exp:
            _EBAY_TOKEN_CACHE = (tok, time.time() + int(exp) - 60)
        return tok


async def _search_ebay_au(query: str, max_results: int, sort: str, auctions: bool = False, seller_scope: str = "domestic") -> list[Offer]:
    """eBay AU Browse API search.

    auctions:   AUCTION-format listings only (current bid, bid count, end time).
                Without it: FIXED_PRICE listings (the historical behaviour).
    seller_scope:
      "domestic" — items located in Australia (itemLocationCountry:AU; the
                   historical buyer-context behaviour).
      "global"   — no location filter: world-wide sellers shipping into the
                   AU marketplace (GBP/USD/EUR sellers with converted prices).
                   Omitting the AU end-user context widens the pool ~19x
                   (verified: 23 → 2707 for "vintage synthesizer").
      "auto"     — eBay default (no explicit filter, AU context header set).
    """
    token = await _get_ebay_token() if _has_any_key([EBAY_APP_ID, EBAY_CERT_ID, EBAY_OAUTH_TOKEN]) else None
    if not token:
        return []
    ebay_sort = {"cheapest": "price", "fastest": "price", "best_value": "price"}.get(sort, "price")
    url = "https://api.ebay.com/buy/browse/v1/item_summary/search"
    headers = {
        "Authorization": f"Bearer {token}",
        "X-EBAY-C-MARKETPLACE-ID": "EBAY_AU",
    }
    if seller_scope != "global":
        headers["X-EBAY-C-ENDUSERCTX"] = "contextualLocation=country=AU,zip=2000"

    filters: list[str] = []
    if auctions:
        filters.append("buyingOptions:{AUCTION}")
    if seller_scope == "domestic":
        filters.append("itemLocationCountry:AU")

    params: dict[str, Any] = {"q": query, "limit": str(max_results), "sort": ebay_sort}
    if filters:
        params["filter"] = ",".join(filters)
    try:
        async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
            r = await client.get(url, headers=headers, params=params)
            r.raise_for_status()
            data = r.json()
            offers: list[Offer] = []
            for it in (data.get("itemSummaries") or [])[:max_results]:
                is_auction = "AUCTION" in (it.get("buyingOptions") or [])
                bid = it.get("currentBidPrice") or {}
                price = it.get("price") or {}
                # Auctions carry currentBidPrice; fixed-price carry price.
                src = (bid if is_auction and bid.get("value") else price) or {}
                aud_price = None
                try:
                    if src.get("value"):
                        aud_price = float(src["value"])
                except Exception:
                    aud_price = None
                # Time left for auctions
                time_left = None
                if is_auction and it.get("itemEndDate"):
                    try:
                        end = datetime.fromisoformat(it["itemEndDate"].replace("Z", "+00:00"))
                        delta = end - datetime.now(timezone.utc)
                        if delta.total_seconds() > 0:
                            d, rem = divmod(int(delta.total_seconds()), 86400)
                            h, _m = divmod(rem, 3600)
                            time_left = f"{d}d {h}h" if d else f"{h}h"
                    except Exception:
                        time_left = None
                shipping = it.get("shippingOptions") or [{}]
                item_country = (it.get("itemLocation") or {}).get("country") or \
                    ((it.get("shipToLocations") or {}).get("countryCodes") or [None])[0]
                offers.append(
                    Offer(
                        offer_id=f"ebay_au:{it.get('itemId') or it.get('legacyItemId') or 'unknown'}",
                        marketplace="ebay_au",
                        title=it.get("title") or query,
                        url=it.get("itemWebUrl") or it.get("itemHref"),
                        image=(it.get("image") or {}).get("imageUrl") or (it.get("thumbnailImages", [{}])[0].get("imageUrl") if it.get("thumbnailImages") else None),
                        price_aud=aud_price,
                        currency=src.get("currency") or "AUD",
                        shipping=Shipping(
                            intl_estimate_aud=0 if any(s.get("shippingCost") is None for s in shipping) else None,
                            eta_days=[3, 7],
                            method="eBay AU" + (" (auction)" if is_auction else ""),
                        ),
                        seller=Seller(shop_name=(it.get("seller") or {}).get("username")),
                        auction=AuctionInfo(
                            current_bid_aud=aud_price if is_auction else None,
                            bid_count=it.get("bidCount"),
                            end_time=it.get("itemEndDate"),
                            time_left=time_left,
                        ) if is_auction else None,
                        seller_country=item_country,
                        source_reliability="authoritative",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="ebay_au:error", marketplace="ebay_au", title=f"[ebay_au] error: {e}", source_reliability="authoritative", raw={"error": str(e)})]


async def _search_sociavault_facebook(query: str, lat: float = -33.8688, lng: float = 151.2093, max_results: int = 10) -> list[Offer]:
    if not SOCIAVAULT_API_KEY:
        return []
    url = "https://api.sociavault.com/v1/scrape/facebook-marketplace/search"
    try:
        async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
            r = await client.get(url, headers={"x-api-key": SOCIAVAULT_API_KEY}, params={"query": query, "lat": lat, "lng": lng, "limit": max_results})
            r.raise_for_status()
            data = r.json()
            listings = data.get("listings") or (data.get("data") or {}).get("listings") or []
            # Sociavault returns listings as a LIST or as a DICT keyed by
            # index strings ("0", "1", …) depending on response path —
            # normalise before slicing (slicing a dict is a TypeError,
            # which presented as 'slice(None, 15, None)' errors).
            if isinstance(listings, dict):
                listings = [listings[k] for k in sorted(listings, key=lambda x: int(x) if x.isdigit() else 0)]
            elif not isinstance(listings, list):
                listings = []
            offers: list[Offer] = []
            for it in listings[:max_results]:
                price = it.get("price") or {}
                aud = price.get("amount") if isinstance(price, dict) else it.get("price_aud")
                try:
                    aud = float(aud) if aud is not None else None
                except Exception:
                    aud = None
                offers.append(
                    Offer(
                        offer_id=f"facebook:{it.get('id') or it.get('listing_id') or 'unknown'}",
                        marketplace="facebook",
                        title=it.get("title") or query,
                        url=it.get("url") or it.get("listing_url"),
                        image=(lambda ph: ph.get("url") if isinstance(ph, dict) else ph)(it.get("primary_photo")) or it.get("image"),
                        price_aud=aud,
                        shipping=Shipping(eta_days=[1, 4], method=it.get("delivery_types") and ", ".join(it["delivery_types"]) or "Local pickup / shipping"),
                        seller=Seller(shop_name=it.get("seller_name") or str(it.get("seller_id") or "")),
                        source_reliability="best_effort",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="facebook:error", marketplace="facebook", title=f"[facebook] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]


async def _search_apify_gumtree(query: str, max_results: int = 10) -> list[Offer]:
    """Gumtree AU via hosted actor.

    KNOWN DEGRADED 2026-09-02: Gumtree AU sits behind Akamai Bot Manager;
    the crawlerbros actor requires RESIDENTIAL/UNBLOCKER proxies, which the
    FREE Apify plan has 0 of (verified live: actor runs SUCCEED but return
    0 items; memo23 actor FAILED outright). Recovery options: (a) upgrade
    the Apify plan for residential proxies, (b) export Gumtree cookies from
    a real browser session into GUMTREE_COOKIES (the actor's documented
    Akamai bypass without proxies).
    """
    if not APIFY_TOKEN:
        return []
    if not os.getenv("GUMTREE_COOKIES"):
        return [
            Offer(
                offer_id="gumtree:degraded",
                marketplace="gumtree",
                title="[gumtree] AU is Akamai-protected; needs residential proxies (Apify plan upgrade) or GUMTREE_COOKIES (EditThisCookie export) — see lane docs",
                source_reliability="best_effort",
                raw={"degraded": True, "reason": "Akamai Bot Manager; FREE plan has 0 residential/UNBLOCKER proxies; actors verified returning 0 items"},
            )
        ]
    actor = os.getenv("GUMTREE_APIFY_ACTOR", "crawlerbros/gumtree-scraper")
    # NOTE: the v2 acts API resolves names ONLY with the '~' separator
    # (user~actor); the '/' form 404s — same fix the ozbargain lane needed.
    url = f"https://api.apify.com/v2/acts/{actor.replace('/', '~', 1)}/runs"
    try:
        async with httpx.AsyncClient(timeout=30) as client:
            r = await client.post(
                url,
                params={"token": APIFY_TOKEN},
                json={"startUrls": [{"url": f"https://www.gumtree.com.au/s-{query.replace(' ', '-')}/k0q0?sortByName=dateDesc"}], "maxItems": max_results, "includeListingDetails": False},
            )
            r.raise_for_status()
            run = r.json().get("data") or {}
            run_id = run.get("id")
            dataset_id = run.get("defaultDatasetId")
            if not run_id:
                return []
            for _ in range(12):
                await asyncio.sleep(2)
                s = await client.get(f"https://api.apify.com/v2/actor-runs/{run_id}", params={"token": APIFY_TOKEN})
                s.raise_for_status()
                status = s.json().get("data", {}).get("status")
                dataset_id = s.json().get("data", {}).get("defaultDatasetId") or dataset_id
                if status in ("SUCCEEDED", "FAILED", "TIMED-OUT", "ABORTED"):
                    break
            if not dataset_id:
                return []
            d = await client.get(f"https://api.apify.com/v2/datasets/{dataset_id}/items", params={"token": APIFY_TOKEN, "limit": max_results})
            d.raise_for_status()
            items = d.json() if isinstance(d.json(), list) else d.json().get("items", [])
            offers: list[Offer] = []
            for it in items[:max_results]:
                try:
                    price_aud = float(str(it.get("price") or it.get("price_aud") or "0").replace("$", "").replace(",", "").strip() or 0) or None
                except Exception:
                    price_aud = None
                offers.append(
                    Offer(
                        offer_id=f"gumtree:{it.get('id') or it.get('url') or 'unknown'}",
                        marketplace="gumtree",
                        title=it.get("title") or query,
                        url=it.get("url"),
                        image=(it.get("images") or [None])[0] if isinstance(it.get("images"), list) else it.get("image"),
                        price_aud=price_aud,
                        seller=Seller(shop_name=it.get("seller_name") or it.get("seller")),
                        shipping=Shipping(eta_days=[2, 5], method="Gumtree local"),
                        source_reliability="best_effort",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="gumtree:error", marketplace="gumtree", title=f"[gumtree] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]


# ---------------------------------------------------------------------------
# Deal-feed lanes — OzBargain / Cheapies (official RSS, no key required)
# ---------------------------------------------------------------------------

# The ozb: namespace both sites share (same platform software)
_OZB_NS = {"ozb": "https://www.ozbargain.com.au", "media": "http://search.yahoo.com/mrss/"}

_PRICE_RE = re.compile(r"\$([0-9][0-9,]*(?:\.[0-9]{1,2})?)")


def _title_price_aud(title: str) -> float | None:
    """OzBargain embeds the price in the title, e.g. '... $267.75 Delivered @ Amazon AU'.

    First $amount wins — the site's convention puts the headline price first.
    """
    m = _PRICE_RE.search(title)
    if not m:
        return None
    try:
        return round(float(m.group(1).replace(",", "")), 2)
    except ValueError:
        return None


def _query_relevant(query: str, *texts: str | None) -> bool:
    """Loose relevance gate for search lanes whose upstream search can drift
    (grocery catalogues fuzzy-match, Amazon pads generic terms).

    Every whitespace term of the query must appear in at least ONE of the
    candidate texts (title/brand/category). 'full cream milk' accepts
    'Full Cream Milk 2L' and 'A2 Full Cream Milk' but rejects
    'Sunflower Oil' or 'Mega Roulette'. A bare single term ('milk') just
    needs that substring present. Case-insensitive substring, not
    token-exact — grocery titles carry pack sizes and descriptors.
    """
    if not query:
        return True
    terms = [t.lower() for t in query.split() if t]
    if not terms:
        return True
    haystack = " ".join((t or "").lower() for t in texts)
    return all(t in haystack for t in terms)


def _parse_deal_feed(xml_text: str, marketplace: str, query: str | None, max_results: int) -> list[Offer]:
    """Parse an OzBargain/Cheapies RSS feed into Offers.

    No scraping — this is the sites' own published feed. ozb:meta carries
    votes/clicks/expiry; <category> carries cat/tag/brand/product taxonomy.
    When `query` is set, offers are filtered client-side on title/category
    match (feeds are ~100 items; filtering locally is cheap and ToS-clean).
    """
    root = ET.fromstring(xml_text)
    offers: list[Offer] = []
    terms = [t for t in (query or "").lower().split()] if query else None

    for item in root.iter("item"):
        title = (item.findtext("title") or "").strip()
        link = (item.findtext("link") or "").strip()
        description = (item.findtext("description") or "").strip()

        cats: list[str] = []
        brand: str | None = None
        tags: list[str] = []
        for cat in item.findall("category"):
            text = (cat.text or "").strip()
            domain = cat.get("domain") or ""
            cats.append(text)
            if "/brand/" in domain:
                brand = text
            elif "/tag/" in domain or "/cat/" in domain:
                tags.append(text)

        meta = item.find("ozb:meta", _OZB_NS)
        votes_pos = votes_neg = comment_count = click_count = None
        expiry = None
        image = None
        goto = None
        if meta is not None:
            def _int(attr: str) -> int | None:
                v = meta.get(attr)  # type: ignore[union-attr]
                if v is None:
                    return None
                try:
                    return int(v)
                except ValueError:
                    return None

            votes_pos = _int("votes-pos")
            votes_neg = _int("votes-neg")
            comment_count = _int("comment-count")
            click_count = _int("click-count")
            expiry = meta.get("expiry")  # type: ignore[union-attr]
            image = meta.get("image")  # type: ignore[union-attr]
            goto = meta.get("link")  # type: ignore[union-attr]  # affiliate /goto/ link

        if image is None:
            thumb = item.find("media:thumbnail", _OZB_NS)
            if thumb is not None:
                image = thumb.get("url")

        # guid isPermaLink=false carries the node id, e.g. '973515 at https://...'
        guid = (item.findtext("guid") or "").split(" at ")[0].strip()

        # client-side filter: title/categories/brand/tags must contain a term
        if terms:
            haystack = " ".join([title, *cats, brand or "", *(tags or [])]).lower()
            if not any(t in haystack for t in terms):
                continue

        offers.append(
            Offer(
                offer_id=f"{marketplace}:{guid or link}",
                marketplace=marketplace,
                title=title,
                url=link,
                image=image,
                price_aud=_title_price_aud(title),
                currency="AUD" if marketplace == "ozbargain" else "NZD",
                shipping=Shipping(eta_days=[0, 0], method="Deal listing — see merchant"),
                seller=Seller(shop_name=(item.findtext("{http://purl.org/dc/elements/1.1/}creator") or None)),
                community=CommunitySignal(
                    votes_pos=votes_pos,
                    votes_neg=votes_neg,
                    comment_count=comment_count,
                    click_count=click_count,
                    expiry=expiry,
                    brand=brand,
                    tags=tags or None,
                ),
                source_reliability="authoritative",  # the site's own published feed
                raw={"description": description, "goto": goto} if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
            )
        )
        if len(offers) >= max_results:
            break
    return offers


async def _search_deal_feed(feed_url: str, marketplace: str, query: str | None, max_results: int) -> list[Offer]:
    """Fetch an OzBargain/Cheapies RSS feed and parse it. Query=None returns the
    latest deals unfiltered (the 'what's hot' browse)."""
    try:
        async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S, follow_redirects=True) as client:
            r = await client.get(feed_url, headers={"User-Agent": "procurement-mcp/0.1 (+https://jupiter.au)"})
            r.raise_for_status()
            return _parse_deal_feed(r.text, marketplace, query, max_results)
    except Exception as e:
        return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] feed error: {e}", source_reliability="authoritative", raw={"error": str(e)})]


# ---------------------------------------------------------------------------
# Keepa — official Amazon price-history API (optional key; covers amazon.com.au)
# ---------------------------------------------------------------------------


async def _enrich_with_keepa(offers: list[Offer]) -> list[Offer]:
    """Attach Amazon AU price history to amazon_au offers via Keepa.

    Keepa domain 12 == amazon.com.au. Rate-limited to one batched call per
    search (up to 20 ASINs); offers without an ASIN-shaped id are skipped.
    Enrichment is best-effort: on any error the offer passes through bare.
    """
    if not KEEPA_API_KEY:
        return offers
    asin_re = re.compile(r"^B[0-9A-Z]{9}$")
    targets = [
        (i, o) for i, o in enumerate(offers)
        if o.marketplace == "amazon_au"
        and asin_re.match((o.offer_id.split(":", 1)[1] if ":" in o.offer_id else "").strip())
    ]
    if not targets:
        return offers
    asins = [o.offer_id.split(":", 1)[1].strip() for _, o in targets[:20]]
    try:
        async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S + 2) as client:
            r = await client.get(
                "https://api.keepa.com/product",
                params={"key": KEEPA_API_KEY, "domain": "12", "asin": ",".join(asins), "stats": "90"},
            )
            r.raise_for_status()
            products = {p.get("asin"): p for p in (r.json().get("products") or [])}
            stats_by_asin: dict[str, dict[str, Any]] = {}
            current_by_asin: dict[str, float | None] = {}
            for asin, p in products.items():
                sv = p.get("csv") or []
                # csv[0] == amazon price series (Keepa format: pairs of time,price)
                stats_by_asin[asin] = p.get("stats") or {}
                current_by_asin[asin] = (p.get("amazonPrice") or None)
            for idx, offer in targets:
                asin = offer.offer_id.split(":", 1)[1].strip()
                st = stats_by_asin.get(asin)
                if not st:
                    continue
                offer.price_history = {
                    "source": "keepa",
                    "domain": "amazon.com.au",
                    "current_aud": current_by_asin.get(asin),
                    "avg_90d_aud": (st.get("avg90") or [None] * 30)[0] if isinstance(st.get("avg90"), list) else st.get("avg90"),
                    "min_90d_aud": st.get("min") or (st.get("min90") or [None])[0] if isinstance(st.get("min90"), list) else st.get("min90"),
                }
        return offers
    except Exception:
        return offers


# ---------------------------------------------------------------------------
# Chinese keyword search — zen-studio hosted actors (taobao/tmall, jd, 1688)
# ---------------------------------------------------------------------------


async def _search_apify_chinese(actor: str, marketplace: str, query: str, max_results: int) -> list[Offer]:
    """Shared zen-studio Chinese search lane.

    Verified output shapes (2026-09-02 live runs):
      taobao: itemId, url, title/titleOriginal, price (CNY str), priceCurrency
      jd:     skuId/itemId (jd- prefixed), url, title, price (CNY float)
    maxItems floor is 10 per actor input validation.
    """
    if not APIFY_TOKEN:
        return []
    # Per-actor input schema (each zen-studio actor differs — the 1688
    # wholesale scraper takes keywords[] + maxResults, while taobao/jd take
    # keyword + maxItems floor 10; sending the wrong shape silently returns
    # nothing or TIMED-OUT):
    is_1688 = "1688" in actor
    if is_1688:
        run_input = {"keywords": [query], "maxResults": max_results, "includeSkuDetails": False, "includeDescriptionHtml": False, "includeSupplierIntelligence": False}
    else:
        run_input = {"keyword": query, "maxItems": max(max_results, 10), "enrichWithDetails": False}
    try:
        async with httpx.AsyncClient(timeout=90) as client:
            r = await client.post(
                f"https://api.apify.com/v2/acts/{actor.replace('/', '~', 1)}/run-sync-get-dataset-items",
                params={"token": APIFY_TOKEN, "timeout": 85},
                json=run_input,
            )
            if r.status_code not in (200, 201):
                return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] actor error {r.status_code}", source_reliability="best_effort", raw={"error": r.text[:300]})]
            items = r.json() if isinstance(r.json(), list) else r.json().get("items", [])
            items, actor_err = _split_envelopes(items)
            if not items and actor_err:
                return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] actor error: {actor_err}", source_reliability="best_effort", raw={"error": actor_err})]
            offers: list[Offer] = []
            for it in items[:max_results]:
                raw_price = it.get("price")
                if isinstance(raw_price, dict):
                    # 1688 wholesale shape: {min, max, currency}
                    raw_price = raw_price.get("min") or raw_price.get("max")
                price_cny = _float_or_none(raw_price or it.get("priceFromSearch"))
                item_id = str(it.get("itemId") or it.get("skuId") or "unknown")
                offers.append(
                    Offer(
                        offer_id=f"{marketplace}:{item_id}",
                        marketplace=marketplace,
                        title=str(it.get("title") or it.get("titleOriginal") or query),
                        title_en=it.get("titleEn"),
                        url=it.get("url") or it.get("productUrl") or it.get("detailUrl"),
                        image=it.get("image") or it.get("imageUrl") or ((it.get("images") or {}).get("main") if isinstance(it.get("images"), dict) else None),
                        price_cny=price_cny,
                        price_aud=_cny_to_aud(price_cny),
                        currency="CNY",
                        stock=it.get("stock") if isinstance(it.get("stock"), int) else None,
                        shipping=Shipping(eta_days=[10, 25], method="China consolidated"),
                        seller=Seller(shop_name=it.get("shopName") or (it.get("shop") or {}).get("name") if isinstance(it.get("shop"), dict) else it.get("shopName")),
                        source_reliability="best_effort",  # hosted scrape, not official API
                        raw={"is_tmall": it.get("isTmall"), "sales": it.get("salesSignal") or it.get("sales")} if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]


# ---------------------------------------------------------------------------
# eBay sold-price comps — caffein.dev/ebay-sold-listings (supports ebay.com.au)
# ---------------------------------------------------------------------------


async def _search_ebay_sold_au(query: str, max_results: int = 10, days: int = 30) -> list[Offer]:
    """Real eBay AU sold prices (what items ACTUALLY sold for, not asking).

    Output per the actor's schema: soldPrice, totalPrice (price+shipping),
    endedAt, itemCondition, sellerPositivePercent. These are comps, not
    buyable offers — source_reliability=aggregator, eta 0 (already ended).
    """
    if not APIFY_TOKEN:
        return []
    try:
        async with httpx.AsyncClient(timeout=30) as client:
            r = await client.post(
                f"https://api.apify.com/v2/acts/{EBAY_SOLD_APIFY_ACTOR.replace('/', '~', 1)}/run-sync-get-dataset-items",
                params={"token": APIFY_TOKEN, "timeout": 28},
                json={"keywords": [query], "ebaySite": "ebay.com.au", "daysToScrape": days, "count": max_results},
            )
            if r.status_code not in (200, 201):
                return [Offer(offer_id="ebay_sold:error", marketplace="ebay_sold", title=f"[ebay_sold] actor error {r.status_code}", source_reliability="aggregator", raw={"error": r.text[:300]})]
            items = r.json() if isinstance(r.json(), list) else r.json().get("items", [])
            items, actor_err = _split_envelopes(items)
            if not items and actor_err:
                return [Offer(offer_id="ebay_sold:error", marketplace="ebay_sold", title=f"[ebay_sold] actor error: {actor_err}", source_reliability="aggregator", raw={"error": actor_err})]
            offers: list[Offer] = []
            for it in items[:max_results]:
                try:
                    sold = round(float(it.get("soldPrice") or 0), 2) or None
                except (TypeError, ValueError):
                    sold = None
                offers.append(
                    Offer(
                        offer_id=f"ebay_sold:{it.get('itemId') or 'unknown'}",
                        marketplace="ebay_sold",
                        title=it.get("title") or query,
                        url=it.get("url"),
                        image=it.get("thumbnailUrl"),
                        price_aud=sold,
                        currency=it.get("soldCurrency") or "AUD",
                        stock=0,  # sold — no longer available
                        shipping=Shipping(eta_days=[0, 0], method="Sold comp — listing ended"),
                        seller=Seller(
                            shop_name=it.get("sellerUsername"),
                            rating=round(float(it["sellerPositivePercent"]), 1) if it.get("sellerPositivePercent") else None,
                        ),
                        price_history={
                            "source": "ebay_sold_comps",
                            "sold_price": sold,
                            "total_with_shipping": it.get("totalPrice"),
                            "ended_at": it.get("endedAt"),
                            "condition": it.get("condition"),
                            "listing_type": it.get("listingType"),
                        },
                        source_reliability="aggregator",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="ebay_sold:error", marketplace="ebay_sold", title=f"[ebay_sold] error: {e}", source_reliability="aggregator", raw={"error": str(e)})]


# ---------------------------------------------------------------------------
# Amazon AU via junglee/free-amazon-product-scraper — no PA-API credentials
# ---------------------------------------------------------------------------


async def _search_apify_amazon_au(query: str, max_results: int = 10) -> list[Offer]:
    """Amazon AU search via Apify's first-party hosted actor.

    Unblocks the amazon_au marketplace with zero credential wait (PA-API
    needs an Associates account with sales history; SP-API needs a Pro
    seller). Input is amazon.com.au search URLs; output carries asin (feeds
    the Keepa enrichment), price, stars, reviewsCount. NOTE: the actor needs
    ~40-60s per run (full page scrape) — it runs on its own longer timeout,
    NOT the per-lane DEFAULT_TIMEOUT_S.
    """
    if not APIFY_TOKEN:
        return []
    try:
        async with httpx.AsyncClient(timeout=75) as client:
            r = await client.post(
                f"https://api.apify.com/v2/acts/{AMAZON_APIFY_ACTOR.replace('/', '~', 1)}/run-sync-get-dataset-items",
                params={"token": APIFY_TOKEN, "timeout": 70},
                json={
                    "categoryUrls": [{"url": f"https://www.amazon.com.au/s?k={query.replace(' ', '+')}"}],
                    "maxItemsPerStartUrl": max_results,
                    "maxSearchPagesPerStartUrl": 1,
                    "scrapeProductDetails": False,  # quick category data — fast lane
                },
            )
            if r.status_code not in (200, 201):
                return [Offer(offer_id="amazon_au:error", marketplace="amazon_au", title=f"[amazon_au] actor error {r.status_code}", source_reliability="best_effort", raw={"error": r.text[:300]})]
            items = r.json() if isinstance(r.json(), list) else r.json().get("items", [])
            items, actor_err = _split_envelopes(items)
            if not items and actor_err:
                return [Offer(offer_id="amazon_au:error", marketplace="amazon_au", title=f"[amazon_au] actor error: {actor_err}", source_reliability="best_effort", raw={"error": actor_err})]
            offers: list[Offer] = []
            for it in items[:max_results]:
                # live-verified field shape: price{value,currency}, imageUrl,
                # stars, reviewsCount, asin — no listPrice at this scrape level
                price = it.get("price") or {}
                try:
                    p = round(float(price.get("value")), 2) if price.get("value") else None
                except (TypeError, ValueError):
                    p = None
                asin = it.get("asin")
                # relevance gate — Amazon pads generic searches with sponsored drift
                if not _query_relevant(query, it.get("title")):
                    continue
                offers.append(
                    Offer(
                        offer_id=f"amazon_au:{asin}" if asin else f"amazon_au:{it.get('url') or 'unknown'}",
                        marketplace="amazon_au",
                        title=it.get("title") or query,
                        url=f"https://www.amazon.com.au/dp/{asin}" if asin else it.get("url"),
                        image=it.get("imageUrl"),
                        price_aud=p,
                        stock=None,
                        shipping=Shipping(eta_days=[2, 7], method="Amazon AU"),
                        seller=Seller(shop_name="Amazon AU", rating=it.get("stars")),
                        source_reliability="best_effort",
                        raw={"reviews": it.get("reviewsCount"), "sponsored": it.get("isSponsored")} if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="amazon_au:error", marketplace="amazon_au", title=f"[amazon_au] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]


# ---------------------------------------------------------------------------
# AU grocery trio — dromb series (search op, unit pricing, specials)
# ---------------------------------------------------------------------------


def _float_or_none(v: Any) -> float | None:
    try:
        return round(float(str(v).replace("$", "").replace(",", "").strip()), 2) or None
    except (TypeError, ValueError):
        return None


async def _search_grocery_actor(actor: str, marketplace: str, query: str, max_results: int) -> list[Offer]:
    """Shared dromb-series grocery search (woolworths/coles/aldi).

    Output: price (regular), discount_price (special), unit_price + unit,
    barcode, stock_status, brand. price_aud carries the effective price
    (discount when present); regular_price_aud keeps the anchor so
    best_value can reward genuine specials.
    """
    if not APIFY_TOKEN:
        return []
    try:
        async with httpx.AsyncClient(timeout=30) as client:
            r = await client.post(
                f"https://api.apify.com/v2/acts/{actor.replace('/', '~', 1)}/run-sync-get-dataset-items",
                params={"token": APIFY_TOKEN, "timeout": 28},
                json={"operation": "search", "query": query, "page": 1, "includeRaw": False},
            )
            if r.status_code not in (200, 201):
                return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] actor error {r.status_code}", source_reliability="best_effort", raw={"error": r.text[:300]})]
            items = r.json() if isinstance(r.json(), list) else r.json().get("items", [])
            items, actor_err = _split_envelopes(items)
            if not items and actor_err:
                return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] actor error: {actor_err}", source_reliability="best_effort", raw={"error": actor_err})]
            offers: list[Offer] = []
            for it in items:
                # relevance gate — grocery catalogues fuzzy-match and drift
                # (search 'milk' at ALDI returns oil and lollipops)
                if not _query_relevant(query, it.get("name"), it.get("brand"), *(it.get("attributes") or {}).get("categories", []) if isinstance(it.get("attributes"), dict) else []):
                    continue
                regular = _float_or_none(it.get("price"))
                special = _float_or_none(it.get("discount_price"))
                effective = special if (special and regular and special < regular) else regular
                unit_price = _float_or_none(it.get("unit_price"))
                offers.append(
                    Offer(
                        offer_id=f"{marketplace}:{it.get('id') or it.get('sku') or it.get('barcode') or 'unknown'}",
                        marketplace=marketplace,
                        title=it.get("name") or query,
                        url=it.get("source_url"),
                        image=it.get("image"),
                        price_aud=effective,
                        regular_price_aud=regular if (regular and special and regular > special) else None,
                        unit_price_aud=unit_price,
                        unit=it.get("unit") or (it.get("unit_price_attributes") or {}).get("unit") if isinstance(it.get("unit_price_attributes"), dict) else it.get("unit"),
                        stock=1 if it.get("is_available") in (True, "true") else (0 if it.get("stock_status") == "out_of_stock" else None),
                        shipping=Shipping(eta_days=[1, 5], method="Grocery pickup/delivery"),
                        seller=Seller(shop_name=(it.get("brand") or marketplace.title())),
                        source_reliability="best_effort",
                        raw={"barcode": it.get("barcode"), "size": it.get("size")} if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
                if len(offers) >= max_results:
                    break
            return offers
    except Exception as e:
        return [Offer(offer_id=f"{marketplace}:error", marketplace=marketplace, title=f"[{marketplace}] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]



def _rank_offers(offers: list[Offer], sort: str, qty: int) -> list[Offer]:
    if sort == "cheapest":

        def landed(o: Offer) -> float:
            base = o.price_aud if o.price_aud is not None else (o.price_cny * CNY_TO_AUD if o.price_cny else 1e9)
            ship = o.shipping.intl_estimate_aud or 0
            if o.moq and qty < o.moq:
                return 1e12
            return base * qty + ship

        # Grocery offers with unit pricing sort on unit economics — a $4.50
        # 2L beats a $3.10 1L and item price can't express that.
        def unit_or_landed(o: Offer) -> float:
            if o.unit_price_aud is not None:
                # normalise: unit price IS the per-unit landed cost already
                return o.unit_price_aud
            return landed(o)

        return sorted(offers, key=unit_or_landed)
    if sort == "fastest":

        def eta(o: Offer) -> float:
            if o.shipping.eta_days:
                return o.shipping.eta_days[1]
            return 99

        return sorted(offers, key=lambda o: (eta(o), o.price_aud or 1e9))

    def score(o: Offer) -> float:
        price_norm = 1 - min(1, (o.price_aud or 50) / 200)
        eta_norm = 1 - min(1, (o.shipping.eta_days[1] if o.shipping.eta_days else 14) / 30)
        trust = 0.5
        # Community vetting of the *price* (OzBargain-style votes) is the
        # strongest trust signal we have — a +5-voted deal outranks a
        # heuristic 4.8-star seller. Net votes, saturating at ±10.
        if o.community and (o.community.votes_pos or o.community.votes_neg):
            net = (o.community.votes_pos or 0) - (o.community.votes_neg or 0)
            trust = max(0.0, min(1.0, 0.5 + net / 20))
        elif o.seller.rating:
            trust = min(1, o.seller.rating / 5)
        elif o.source_reliability == "authoritative":
            trust = 0.85
        elif o.source_reliability == "aggregator":
            trust = 0.7
        return 0.5 * price_norm + 0.25 * eta_norm + 0.25 * trust

    return sorted(offers, key=lambda o: score(o), reverse=True)


# ---------------------------------------------------------------------------
# MCP tools
# ---------------------------------------------------------------------------


@mcp.tool()
async def search_offers(
    query: Annotated[str, Field(description="Product keyword. Chinese yields best recall for Chinese sources (e.g. 硅胶厨具). English is auto-translated for TMAPI call.")],
    marketplaces: Annotated[
        list[str] | None,
        Field(description="Subset to search. Defaults to all. Valid: aliexpress, taobao, tmall, 1688, jd, pdd, ebay_au, ebay_sold (real AU sold prices), amazon_au, gumtree, facebook, woolworths, coles, aldi (grocery with unit pricing), ozbargain, cheapies. Unknown values ignored."),
    ] = None,
    sort: Annotated[Literal["cheapest", "fastest", "best_value"], Field(description="Ranking intent")] = "best_value",
    qty: Annotated[int, Field(ge=1, description="Requested quantity — used for MOQ check on 1688 wholesale")] = 1,
    ship_to: Annotated[str, Field(description="Destination country code, e.g. AU")] = "AU",
    max_results: Annotated[int, Field(ge=1, le=50, description="Per-marketplace cap, then ranked globally")] = 10,
    ebay_auctions: Annotated[bool, Field(description="eBay lane: AUCTION-format listings only — current bid, bid count, end time. Default false = fixed-price listings.")] = False,
    ebay_seller_scope: Annotated[Literal["domestic", "global", "auto"], Field(description="eBay lane seller scope. 'domestic' (default): items located in AU. 'global': world-wide sellers into the AU marketplace (~19x pool, converted prices). 'auto': eBay default ranking.")] = "domestic",
) -> list[Offer]:
    """Federated procurement search — `find me the cheapest / fastest / best value <item>` across Chinese + AU sources.

    Every marketplace is reached via a hosted API (no scrapers built here).
    Missing API keys are skipped gracefully. A query with no matches returns an empty
    list — never synthetic data. When DATABASE_URL (Postgres on 10.1.1.3) is configured,
    results are cached for CACHE_TTL_S.
    """
    t0 = time.time()
    all_mps = [
        "taobao", "tmall", "1688", "jd", "pdd", "aliexpress",
        "ebay_au", "ebay_sold", "amazon_au", "gumtree", "facebook",
        "woolworths", "coles", "aldi",
        "ozbargain", "cheapies",
    ]
    wanted = [m.lower().strip() for m in (marketplaces or all_mps)]
    wanted = [m for m in wanted if m in all_mps]

    # Postgres cache hit?
    cache_key = _cache_key(query, wanted, sort, qty, ship_to, max_results, ebay_auctions=ebay_auctions, ebay_seller_scope=ebay_seller_scope)
    cached = await _db_get_cached(cache_key)
    if cached is not None:
        return cached

    tasks: dict[str, Any] = {}
    # Chinese keyword search — zen-studio hosted actors (TMAPI has search only
    # for 1688; these carry taobao/tmall + jd). 1688 prefers TMAPI (official
    # API) when subscribed, else falls back to the wholesale actor.
    if "taobao" in wanted or "tmall" in wanted:
        tasks["taobao"] = asyncio.create_task(_search_apify_chinese(TAOBAO_APIFY_ACTOR, "taobao", query, max_results))
    if "jd" in wanted:
        tasks["jd"] = asyncio.create_task(_search_apify_chinese(JD_APIFY_ACTOR, "jd", query, max_results))
    if "1688" in wanted:
        async def search_1688() -> list[Offer]:
            offers = await _search_tmapi(query, "1688", max_results)
            # honest-unsubscribed / empty → hosted actor fallback
            if not offers or all(o.offer_id.endswith((":error", ":unsubscribed")) for o in offers):
                fb = await _search_apify_chinese(ALI1688_APIFY_ACTOR, "1688", query, max_results)
                real = [o for o in fb if not o.offer_id.endswith(":error")]
                if real:
                    return real
            return offers
        tasks["1688"] = asyncio.create_task(search_1688())
    if "pdd" in wanted:
        # No hosted keyword-search actor for PDD on the store (checked 2026-09-02);
        # honest skip — TMAPI detail-only + no actor.
        tasks["pdd"] = asyncio.create_task(_search_tmapi(query, "pdd", max_results))
    if "ebay_au" in wanted:
        tasks["ebay_au"] = asyncio.create_task(_search_ebay_au(query, max_results, sort, auctions=ebay_auctions, seller_scope=ebay_seller_scope))
    if "facebook" in wanted:
        tasks["facebook"] = asyncio.create_task(_search_sociavault_facebook(query, max_results=max_results))
    if "gumtree" in wanted:
        tasks["gumtree"] = asyncio.create_task(_search_apify_gumtree(query, max_results))
    # eBay sold comps — real transaction prices (ebay.com.au). Heavier than a
    # normal search (Apify credits per run) so it's opt-in-ish: runs when
    # explicitly requested OR when ebay_au is searched (comps for its offers).
    if "ebay_sold" in wanted:
        tasks["ebay_sold"] = asyncio.create_task(_search_ebay_sold_au(query, max_results))
    # Amazon AU via hosted actor — no credentials required
    if "amazon_au" in wanted and APIFY_TOKEN:
        tasks["amazon_au"] = asyncio.create_task(_search_apify_amazon_au(query, max_results))
    # AU grocery trio — dromb series, unit pricing + specials
    for mp, actor in [("woolworths", WOOLWORTHS_APIFY_ACTOR), ("coles", COLES_APIFY_ACTOR), ("aldi", ALDI_APIFY_ACTOR)]:
        if mp in wanted:
            tasks[mp] = asyncio.create_task(_search_grocery_actor(actor, mp, query, max_results))
    # Deal feeds — official RSS, no key. ozbargain/cheapies filter the feed
    # client-side on the query. (ozbargain_search actor REMOVED 2026-09-01:
    # junk echo of the query with worse data than the free RSS lane.)
    if "ozbargain" in wanted:
        tasks["ozbargain"] = asyncio.create_task(_search_deal_feed(OZBARGAIN_FEED_URL, "ozbargain", query, max_results))
    if "cheapies" in wanted:
        tasks["cheapies"] = asyncio.create_task(_search_deal_feed(CHEAPIES_FEED_URL, "cheapies", query, max_results))

    results: list[Offer] = []
    if tasks:
        # Heavy hosted-actor lanes scrape full search pages (30-90s); the
        # fast lanes (RSS, eBay official API) ride the normal timeout.
        HEAVY_LANES = {"amazon_au", "taobao", "tmall", "jd", "1688", "ebay_sold"}
        for mp, task in tasks.items():
            lane_timeout = 95 if mp in HEAVY_LANES else DEFAULT_TIMEOUT_S + 2
            try:
                chunk = await asyncio.wait_for(task, timeout=lane_timeout)
                results.extend(chunk)
            except TimeoutError:
                results.append(Offer(offer_id=f"{mp}:timeout", marketplace=mp, title=f"[{mp}] timeout after {lane_timeout}s", source_reliability="aggregator"))
            except Exception as e:
                results.append(Offer(offer_id=f"{mp}:error", marketplace=mp, title=f"[{mp}] error: {e}", source_reliability="aggregator"))

    # Honest results: never fabricate. A no-match query returns []; lanes
    # that errored surface their error Offers (searchable diagnostics), but
    # synthetic data is never presented as real (mock fallback REMOVED
    # 2026-09-01 — it fired on legitimate no-match queries, presenting
    # synthetic offers as if they were results).

    ranked = _rank_offers(results, sort, qty)[: max_results * 2]
    # Keepa price-history enrichment for amazon_au offers (no-op without key)
    ranked = await _enrich_with_keepa(ranked)
    await _db_set_cached(cache_key, query, wanted, sort, qty, ship_to, ranked)
    await _db_log_search(query, wanted, sort, qty, ship_to, len(ranked), int((time.time() - t0) * 1000))
    return ranked


@mcp.tool()
async def get_offer_detail(offer_id: Annotated[str, Field(description="Offer id from search_offers, e.g. taobao:123456 or ebay_au:1234")]) -> Offer | dict[str, Any]:
    """Fetch full detail for a single offer by id."""
    if ":" in offer_id:
        mp, item_id = offer_id.split(":", 1)
        if mp in ("taobao", "tmall", "1688", "jd", "pdd") and not item_id.endswith(":error"):
            detail = await _fetch_tmapi(item_id, mp)
            if detail:
                data = detail.get("data") or detail
                price_cny = None
                try:
                    price_cny = float(str(data.get("price") or 0).replace("¥", "").strip() or 0) or None
                except Exception:
                    pass
                return Offer(
                    offer_id=offer_id,
                    marketplace=mp,
                    title=str(data.get("title") or data.get("subject") or offer_id),
                    url=data.get("url") or data.get("detail_url"),
                    image=data.get("pic"),
                    price_cny=price_cny,
                    price_aud=_cny_to_aud(price_cny),
                    moq=data.get("moq"),
                    stock=data.get("stock"),
                    seller=Seller(shop_name=data.get("shop_name")),
                    source_reliability="aggregator",
                    raw=data if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                )
    if offer_id.startswith("ebay_au:"):
        token = await _get_ebay_token()
        if token:
            item_id = offer_id.split(":", 1)[1]
            async with httpx.AsyncClient(timeout=DEFAULT_TIMEOUT_S) as client:
                r = await client.get(
                    f"https://api.ebay.com/buy/browse/v1/item/{item_id}",
                    headers={"Authorization": f"Bearer {token}", "X-EBAY-C-MARKETPLACE-ID": "EBAY_AU"},
                )
                if r.status_code == 200:
                    it = r.json()
                    price = it.get("price") or {}
                    return Offer(
                        offer_id=offer_id,
                        marketplace="ebay_au",
                        title=it.get("title") or offer_id,
                        url=it.get("itemWebUrl"),
                        image=(it.get("image") or {}).get("imageUrl"),
                        price_aud=float(price["value"]) if price.get("value") else None,
                        seller=Seller(shop_name=(it.get("seller") or {}).get("username")),
                        source_reliability="authoritative",
                        raw=it if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
    return {"error": "offer not found or no credentials for detail fetch", "offer_id": offer_id}


@mcp.tool()
async def search_by_image(
    image_url: Annotated[str, Field(description="Public URL of a product photo to reverse-search (JPG/PNG)")],
    destination: Annotated[Literal["1688", "alibaba", "aliexpress", "global"], Field(description="Where to search: 1688 = wholesale factories, alibaba = B2B, aliexpress = retail, global = Amazon/Walmart/eBay and thousands of retail sites")] = "1688",
    max_results: Annotated[int, Field(ge=1, le=50, description="Cap on matches returned")] = 10,
) -> list[Offer]:
    """Sourcing by photo — reverse image search across 1688/Alibaba/AliExpress/global.

    The most procurement-native capability: hand it a photo (e.g. from a
    Gumtree or Facebook listing) and get matching supplier listings with
    price, MOQ, supplier name and location. Powered by Alibaba's own visual
    search technology via a hosted Apify actor ($0.10/search).
    """
    if not APIFY_TOKEN:
        return [Offer(offer_id="image_search:error", marketplace="1688", title="[image_search] APIFY_TOKEN not configured", source_reliability="best_effort")]
    try:
        async with httpx.AsyncClient(timeout=30) as client:
            r = await client.post(
                f"https://api.apify.com/v2/acts/{IMAGE_SEARCH_APIFY_ACTOR.replace('/', '~', 1)}/run-sync-get-dataset-items",
                params={"token": APIFY_TOKEN, "timeout": 28},
                json={"imageUrl": image_url, "destination": destination, "language": "en", "currency": "AUD"},
            )
            if r.status_code not in (200, 201):
                return [Offer(offer_id="image_search:error", marketplace=destination, title=f"[image_search] actor error {r.status_code}", source_reliability="best_effort", raw={"error": r.text[:300]})]
            items = r.json() if isinstance(r.json(), list) else r.json().get("items", [])
            items, actor_err = _split_envelopes(items)
            if not items and actor_err:
                return [Offer(offer_id="image_search:error", marketplace=destination, title=f"[image_search] actor error: {actor_err}", source_reliability="best_effort", raw={"error": actor_err})]
            offers: list[Offer] = []
            for it in items[:max_results]:
                price = _float_or_none(it.get("price"))
                offers.append(
                    Offer(
                        offer_id=f"{destination}:{it.get('productUrl') or it.get('title') or 'unknown'}",
                        marketplace=destination,
                        title=it.get("title") or "image match",
                        url=it.get("productUrl"),
                        image=it.get("imageUrl"),
                        price_aud=price,
                        currency=it.get("currency") or "AUD",
                        moq=it.get("minOrderQty"),
                        seller=Seller(
                            shop_name=it.get("supplierName"),
                            verified=bool(it.get("supplierUrl")),
                        ),
                        shipping=Shipping(eta_days=[10, 25], method="China consolidated"),
                        source_reliability="aggregator",
                        raw={"supplier_url": it.get("supplierUrl"), "location": it.get("location"), "trade_count": it.get("tradeCount")} if os.getenv("PROCUREMENT_INCLUDE_RAW") == "1" else None,
                    )
                )
            return offers
    except Exception as e:
        return [Offer(offer_id="image_search:error", marketplace=destination, title=f"[image_search] error: {e}", source_reliability="best_effort", raw={"error": str(e)})]


@mcp.tool()
def health() -> dict[str, Any]:
    """Return which integrations are configured (no secrets) and DB status."""
    return {
        "status": "ok",
        "email": "io+agent@jupiter.au",
        "integrations": {
            "tmapi": bool(TMAPI_TOKEN),
            "elim": bool(ELIM_API_TOKEN),
            "aliexpress": bool(ALIEXPRESS_APP_KEY and ALIEXPRESS_APP_SECRET),
            "ebay_au": bool(EBAY_APP_ID and EBAY_CERT_ID or EBAY_OAUTH_TOKEN),
            "amazon_paapi": bool(AMAZON_PAAPI_ACCESS_KEY),
            "sociavault_facebook": bool(SOCIAVAULT_API_KEY),
            "apify_gumtree": bool(APIFY_TOKEN),
            "ozbargain_rss": True,  # official feed, no key
            "cheapies_rss": True,  # official feed, no key
            "ebay_sold_comps": bool(APIFY_TOKEN),  # caffein.dev actor (ebay.com.au)
            "amazon_au_actor": bool(APIFY_TOKEN),  # junglee actor — no PA-API needed
            "grocery": {"woolworths": bool(APIFY_TOKEN), "coles": bool(APIFY_TOKEN), "aldi": bool(APIFY_TOKEN)},
            "image_search": bool(APIFY_TOKEN),  # dev00 reverse image search ($0.10/search)
            "keepa_price_history": bool(KEEPA_API_KEY),
        },
        "any_key_configured": any([TMAPI_TOKEN, EBAY_APP_ID, EBAY_OAUTH_TOKEN, SOCIAVAULT_API_KEY, APIFY_TOKEN]),
        "cny_to_aud": CNY_TO_AUD,
        "database": {
            "configured": bool(DATABASE_URL),
            "host": "10.1.1.3",
            "schema": PG_SCHEMA,
            "enabled": _db_enabled,
            "error": _db_error,
            "cache_ttl_s": CACHE_TTL_S,
        },
    }


@mcp.prompt()
def procurement_brief(query: str) -> str:
    return (
        f"You are a procurement assistant for {query}. Use search_offers(query='{query}', sort='best_value') "
        "then compare landed cost (price_aud * qty + shipping), ETA and seller rating. "
        "Explain cheapest vs fastest vs best value and flag MOQ issues for 1688 wholesale."
    )


# ---------------------------------------------------------------------------
# eBay marketplace account deletion/closure notifications — compliance routes
# ---------------------------------------------------------------------------
# Contract (developer.ebay.com/develop/guides/sell/marketplace-user-account-deletion):
#   GET  <endpoint>?challenge_code=<code> → 200 OK, application/json,
#        {"challengeResponse": "<sha256_hex(challengeCode + token + endpoint)>"}
#        Hash order is EXACT: challengeCode + verificationToken + endpoint.
#   POST <endpoint> — deletion notification payload → any 2xx ACK
#        (200/201/202/204). We log it and 204.
# Plain Starlette routes mounted on the FastMCP streamable-HTTP app — served
# whenever MCP_HTTP_PORT is set (the callisto systemd unit / tunnel path).

from starlette.requests import Request  # noqa: E402
from starlette.responses import JSONResponse, Response  # noqa: E402


@mcp.custom_route("/ebay/notifications", methods=["GET"])
async def ebay_deletion_challenge(request: Request) -> JSONResponse:
    """eBay subscription verification: hash challenge + token + endpoint."""
    challenge = request.query_params.get("challenge_code")
    if not EBAY_DELETION_TOKEN:
        return JSONResponse({"error": "EBAY_DELETION_TOKEN not configured"}, status_code=500)
    if not challenge:
        return JSONResponse({"error": "challenge_code query param required"}, status_code=400)
    # EXACT hash order per eBay's contract: challengeCode + verificationToken + endpoint
    digest = hashlib.sha256(
        (challenge + EBAY_DELETION_TOKEN + EBAY_DELETION_ENDPOINT).encode("utf-8")
    ).hexdigest()
    return JSONResponse({"challengeResponse": digest})


@mcp.custom_route("/ebay/notifications", methods=["POST"])
async def ebay_deletion_notify(request: Request) -> Response:
    """eBay deletion/closure notification: acknowledge with 2xx."""
    try:
        body = await request.json()
    except Exception:
        body = None
    # Log-only audit trail — this app holds no marketplace user data to
    # delete (search-only, no user tokens persisted), so ACK is the whole
    # obligation. Keep the payload in the journal for the audit trail.
    print(f"[ebay-deletion] notification received: {json.dumps(body)[:500] if body else '(empty)'}", flush=True)
    return Response(status_code=204)


def main() -> None:
    # Two transports: opencode spawns this file per-session over stdio
    # (mcp.procurement in opencode.json); the callisto systemd unit sets
    # MCP_HTTP_PORT instead, serving streamable-HTTP on loopback for
    # remote-type MCP clients (http://127.0.0.1:<port>/mcp).
    port = os.getenv("MCP_HTTP_PORT")
    if port:
        mcp.settings.host = "127.0.0.1"
        mcp.settings.port = int(port)
        mcp.run(transport="streamable-http")
    else:
        mcp.run()


if __name__ == "__main__":
    main()
