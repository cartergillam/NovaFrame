# Stock Cache Workers

NovaFrame now has two worker paths:

1. `cloudflare-stock-worker` (recommended production)
2. `stocks_cache_worker.py` (local/manual fallback)

Both publish the same Firebase contract used by firmware.

## Firebase Paths (Shared Contract)

Workers read symbol config from:

- `/novaFrame/devices/*/apps/stocks/symbols`

Workers write:

- `/novaFrame/cache/stocks/{symbol}`
- `/novaFrame/cache/trendlines/{symbol}`
- `/novaFrame/devices/{deviceId}/runtime/stocks`

## Recommended: Cloudflare Worker + Cron

Use:

- [workers/cloudflare-stock-worker/README.md](/Users/carter/Documents/NovaFrame/workers/cloudflare-stock-worker/README.md)

That path is always-on and removes the requirement for a running laptop.

## Local Python Worker (Fallback)

Use only for local testing/manual backfill.

## Environment

Set these environment variables before running:

- `FIREBASE_DATABASE_URL`
- `FINNHUB_API_KEY`
- `TWELVEDATA_API_KEY`

Firebase credentials can be provided with either:

- `FIREBASE_SERVICE_ACCOUNT_PATH`
- `FIREBASE_SERVICE_ACCOUNT_JSON`
- or Application Default Credentials via `GOOGLE_APPLICATION_CREDENTIALS`

## Install

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

Dry-run symbol discovery:

```bash
python3 workers/stocks_cache_worker.py --print-symbols
```

Force quotes:

```bash
python3 workers/stocks_cache_worker.py --force-quotes --quotes-only
```

Force trendlines:

```bash
python3 workers/stocks_cache_worker.py --force-trendlines --trendlines-only
```

Normal scheduled run:

```bash
python3 workers/stocks_cache_worker.py
```

The worker prints a run summary:

- `symbols`
- `quotes_written`
- `trendlines_written`
- `runtime_written`
- `failures`

## Schedule Behavior

The worker uses America/Toronto time.

- Quotes run Monday-Friday during `09:30` to `16:00` every minute.
- One after-close quote refresh is allowed at `16:15`.
- Quotes do not run on weekends.
- Trendlines run twice daily at `08:00` and `18:00`.
- Runtime snapshots are written only when payload data changes.

## Simple Scheduler (Local Mac)

Every minute with cron (worker self-gates market hours):

```bash
* * * * * cd /Users/carter/Documents/NovaFrame && /Users/carter/Documents/NovaFrame/.venv/bin/python workers/stocks_cache_worker.py >> /tmp/novaframe-stocks-worker.log 2>&1
```
