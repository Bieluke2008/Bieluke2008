# MCP "procurement"

Servidor MCP de búsqueda federada de ofertas (Taobao/Tmall, 1688, JD, eBay AU,
Amazon AU, OzBargain…), vendorizado desde
[belikh/procurement](https://github.com/belikh/procurement) @ `7a41264`.

## Cómo funciona

- `/.mcp.json` registra el servidor stdio `procurement`. Claude Code lo lanza con
  `uv run --directory tools/procurement python server.py`; uv crea el `.venv` e
  instala las dependencias fijadas en `tools/procurement/uv.lock` la primera vez.
- `/.claude/settings.json` lo aprueba (`enabledMcpjsonServers`) para que las
  sesiones en la nube no se queden esperando confirmación.
- Herramientas: `search_offers`, `get_offer_detail`, `search_by_image`, `health`.
- Sin claves no hay datos inventados: los carriles sin clave se omiten y
  `search_offers` devuelve `[]`. `health()` indica qué integraciones están activas.

Prueba rápida:

```bash
uv run --directory tools/procurement python -c "from server import health; print(health())"
```

## Variables de entorno

Configúralas en el entorno de Claude Code en la nube, nunca en un fichero del repo.

| Variable | Para qué |
|---|---|
| `TMAPI_TOKEN` | 1688 por palabra clave y `get_offer_detail` (taobao/tmall/1688/jd/pdd). Se pasa vía `${TMAPI_TOKEN}` en `.mcp.json`. |
| `APIFY_TOKEN` | **Necesario para buscar en Taobao/Tmall y JD por palabra clave** (TMAPI no tiene búsqueda para esos), fallback de 1688, Amazon AU, eBay sold, supermercados, búsqueda por imagen. |
| `EBAY_APP_ID` + `EBAY_CERT_ID` | eBay AU (opcional). |
| `SOCIAVAULT_API_KEY`, `KEEPA_API_KEY` | Facebook Marketplace y precio histórico de Amazon (opcionales). |

El proceso hereda el entorno de la sesión, así que estas variables llegan al
servidor aunque solo `TMAPI_TOKEN` esté listada en `.mcp.json`. Lista completa en
`tools/procurement/.env.example`. No pongas `DATABASE_URL` (apunta a una red privada
del autor); sin ella el servidor funciona sin estado.

## Dominios de red permitidos

- Siempre: `pypi.org`, `files.pythonhosted.org` (uv instala dependencias).
- TMAPI: `api.tmapi.top`
- Apify: `api.apify.com`
- Opcionales: `api.ebay.com`, `api.sociavault.com`, `api.keepa.com`,
  `www.ozbargain.com.au`, `www.cheapies.nz`

## Cambios locales respecto a upstream

`pyproject.toml`: `pyright>=1.18.0` → `>=1.1.380` (la versión no existía y rompía
la resolución), `mcp<2` (mcp 2.x eliminó `FastMCP`) y `[tool.uv] package = false`
(es un script, no un paquete). Añadido `uv.lock` y `.venv/` al `.gitignore`.
