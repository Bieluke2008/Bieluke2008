-- Procurement MCP cache on fleet Postgres (10.1.1.3, jupiter db)
-- Schema: procurement per stack law. Run once as procurement owner or fleet admin.
--   psql "postgresql://user:pass@10.1.1.3:5432/jupiter" -f migrations/001_procurement_cache.sql

CREATE SCHEMA IF NOT EXISTS procurement;

CREATE TABLE IF NOT EXISTS procurement.search_cache (
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
CREATE INDEX IF NOT EXISTS idx_search_cache_expires ON procurement.search_cache (expires_at);
-- Purge expired every hour via pg_cron or app-side TTL check; manual:
-- DELETE FROM procurement.search_cache WHERE expires_at < now();

CREATE TABLE IF NOT EXISTS procurement.search_log (
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
CREATE INDEX IF NOT EXISTS idx_search_log_created ON procurement.search_log (created_at);

-- Grants — adjust role to the procurement app user
-- GRANT USAGE ON SCHEMA procurement TO procurement;
-- GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA procurement TO procurement;
-- GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA procurement TO procurement;
