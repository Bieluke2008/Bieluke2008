# Procurement MCP — federated search across Chinese + Australian marketplaces

> Integration-only. Every source is a hosted API you call with a key — **no scrapers are built here**. Chinese sources via aggregators (TMAPI), Australian sources via official APIs where they exist (eBay AU) and hosted scraper-APIs where they don't (Gumtree, Facebook Marketplace).

Natural-language procurement: *find me the cheapest / fastest shipped / best value `<item>`* → normalised `Offer[]` ranked by landed cost, ETA and seller trust.

Identity for sign-ups: **`io+agent@jupiter.au`** (approved).

## Quick start

```bash
uv sync
cp .env.example .env   # add keys
uv run mcp dev server.py          # Inspector at http://localhost:5173
uv run python server.py           # stdio — for Claude Desktop
uv run mcp install server.py      # install into Claude Desktop
```

Mock fallback means the server is **demo-able with zero keys** — `search_offers` returns synthetic offers and `health` reports `mock_fallback: true`.

## Tools

| Tool | Purpose |
|---|---|
| `search_offers(query, marketplaces?, sort=best_value, qty=1, ship_to=AU, max_results=10)` | Federated parallel search. `marketplaces` subset of `taobao`, `tmall`, `1688`, `jd`, `pdd`, `aliexpress`, `ebay_au`, `ebay_sold` (real AU sold prices), `amazon_au`, `gumtree`, `facebook`, `woolworths`, `coles`, `aldi`, `ozbargain`, `cheapies`, `ozbargain_search`. Sort `cheapest` = landed cost (unit price for grocery), `fastest` = ETA, `best_value` = 0.5 price + 0.25 speed + 0.25 trust (community votes outrank seller ratings). MOQ-aware for 1688 wholesale. |
| `search_by_image(image_url, destination, max_results)` | Reverse image search — photo to matching 1688/Alibaba/AliExpress/global supplier listings with price, MOQ, supplier. |
| `get_offer_detail(offer_id)` | Detail for `marketplace:item_id` (TMAPI) or `ebay_au:itemId` (Browse `getItem`). |
| `health()` | Which integrations are configured (no secrets). |
| Prompt `procurement_brief` | System prompt for procurement reasoning. |

All tools run concurrent `asyncio` fan-out with per-marketplace timeout (`PROCUREMENT_TIMEOUT_S`, default 8s) and never fail the whole search if one marketplace errors.

## Deal-signal lanes (no marketplace required)

Beyond marketplaces, the federation carries **deal-vetting signals**:

* **`ozbargain` / `cheapies`** — the sites' own official RSS feeds (`/deals/feed`), parsed with stdlib `xml.etree` — no key, no ToS risk. Each offer carries `community` (votes ±, comments, clicks, expiry, brand, tags) parsed from the `ozb:meta` extension and `<category>` taxonomy, plus price-from-title and the affiliate `goto` link. The `best_value` trust term uses net community votes — a +5-voted deal outranks a heuristic 4.8-star seller.
* **`ozbargain_search`** — true keyword search over OzBargain via the hosted Apify actor `parseforge/ozbargain-australia-scraper` (reuses `APIFY_TOKEN`; costs Apify credits).
* **`ebay_sold`** — real eBay AU **sold** prices (what items actually sold for, not asking) via `caffein.dev/ebay-sold-listings` with `ebaySite: ebay.com.au`. Sold comps land in `price_history` — a ranking correction no official API offers.
* **`keepa` price history** — when `KEEPA_API_KEY` is set, `amazon_au` offers get a `price_history` block (90-day avg/min via Keepa's official API, domain 12 = amazon.com.au).

## Grocery lanes — unit economics

`woolworths`, `coles`, `aldi` via the dromb Apify series ($0.30/1k):
`operation: search` with unit pricing, specials and barcodes. Grocery offers carry `unit_price_aud` + `unit` + `regular_price_aud` — `price_aud` holds the **effective** price (special when present). The `cheapest` ranker sorts on **unit price** when available, because a $4.50 2L milk beats a $3.10 1L and item price cannot express that. Costco deliberately excluded — every store actor targets costco.com (US/USD); revisit if an AU actor appears.

`search_offers` gates grocery and Amazon results with a relevance filter (every query term must appear in title/brand/category) — ALDI's catalogue search fuzzes "milk" into oil and lollipops without it.

## `search_by_image(image_url, destination, max_results)`

Sourcing by photo — reverse image search across `1688` (wholesale factories), `alibaba` (B2B), `aliexpress` (retail), `global` (Amazon/Walmart/eBay and thousands of retail sites). Hand it a photo from any listing and get matching supplier listings with price, MOQ, supplier name and location. Alibaba's own visual-search tech, $0.10/search.

## API keys — where to sign up as `io+agent@jupiter.au`

### Recommended primary (full Chinese width in one key)

* **TMAPI** — https://console.tmapi.io/user/signup → Account Center → `apiToken`. Covers taobao/tmall/1688/pdd/jd with `item_search` + `item_detail`. Free 200/mo, paid from ~$8/mo. Set `TMAPI_TOKEN`.
* **AliExpress Affiliate** (official, moderate barrier) — https://openservice.aliexpress.com → UAC account with `io+agent@jupiter.au` → join Affiliate Program → create app (1–2 day approval) → `AppKey`/`AppSecret` → set `ALIEXPRESS_APP_KEY`/`ALIEXPRESS_APP_SECRET`.

### Australian

* **eBay AU** — https://developer.ebay.com → Create app with `io+agent@jupiter.au` → obtain `AppID` (`Client ID`) + `CertID` (`Client Secret`) → set `EBAY_APP_ID`/`EBAY_CERT_ID`. Server does client-credentials flow for `https://api.ebay.com/identity/v1/oauth/token` scope `https://api.ebay.com/oauth/api_scope` and calls `buy/browse/v1/item_summary/search` with `X-EBAY-C-MARKETPLACE-ID: EBAY_AU`.
* **Amazon AU** — Seller Central Professional + https://developer.amazonservices.com → SP-API Far East endpoint `https://sellingpartnerapi-fe.amazon.com` (for catalogue/pricing if you are a seller) or Product Advertising API `PA-API 5.0` via `AMAZON_PAAPI_*` + `PARTNER_TAG=jupiter-22`.

### Hosted scraper-APIs (you call, they run browsers)

* **SociaVault** — https://sociavault.com → Facebook Marketplace `search` + `item` (`SOCIAVAULT_API_KEY`). Supports AU lat/lng. Pay-per-request, real-time.
* **Apify** — https://console.apify.com/settings/integrations → `APIFY_TOKEN` → actor `crawlerbros/gumtree-scraper` for Gumtree AU (residential + Akamai bypass handled by Apify).

Optional: `ELIM_API_TOKEN` (https://elim.asia) and `RAPIDAPI_KEY` for Otcommerce JD fallback.

## Behaviour and ranking

* **Price normalisation** — `price_cny` → `price_aud` via `CNY_TO_AUD` (default 0.215). Landed cost = `price_aud * qty + shipping.intl_estimate_aud`. `qty < moq` (1688) pushes to end and is flagged.
* **ETA** — `shipping.eta_days` `[min, max]`.
* **Trust** — `seller.rating` / `source_reliability` (`authoritative` > `aggregator` > `best_effort`).
* **Sorts** — `cheapest` by landed cost, `fastest` by ETA, `best_value` composite `0.5 price + 0.25 speed + 0.25 trust`.

## Project layout

```
pyproject.toml
server.py          # FastMCP server + all integrations
.env.example
research/          # hyperresearch vault (light run procurement-mcp-51d4f3)
```

## Database — Postgres on 10.1.1.3

When `DATABASE_URL` is set, the server caches federated results and logs searches in the fleet Postgres (`10.1.1.3:5432`, `jupiter` db, schema `procurement` per stack law). When unset it runs stateless.

```bash
# Example — set in .env
DATABASE_URL=postgresql://procurement:__SET_ME__@10.1.1.3:5432/jupiter
PROCUREMENT_PG_SCHEMA=procurement
# Apply once
psql "$DATABASE_URL" -f migrations/001_procurement_cache.sql
```

`health()` reports `database.enabled` / `database.error` without leaking secrets. Cache TTL is `PROCUREMENT_CACHE_TTL_S` (default 3600s). Tables: `procurement.search_cache` (keyed by query+marketplaces+sort+qty+ship_to) and `procurement.search_log`.

## Development

```bash
# Use the hyperresearch venv that already has deps, or create your own
/home/io/projects/hyperresearch-opencode/.venv/bin/python -m py_compile server.py
/home/io/projects/hyperresearch-opencode/.venv/bin/ruff check server.py
/home/io/projects/hyperresearch-opencode/.venv/bin/python -c "from server import health; print(health())"
```
