# NovaFrame Cloudflare Stock Worker

This Worker replaces laptop-based stock cache updates with an always-on Cloudflare cron job.

It keeps the current firmware contract unchanged by writing the same Firebase paths:

- `/novaFrame/cache/stocks/{symbol}`
- `/novaFrame/cache/trendlines/{symbol}`
- `/novaFrame/devices/{deviceId}/runtime/stocks`

## What It Does

- Reads per-device symbols from `/novaFrame/devices/*/apps/stocks/symbols`
- Builds the global symbol union
- Fetches:
  - Finnhub quote data (price/change/changePct/asOf/source/currency)
  - TwelveData trendline data (numeric array)
- Writes quote/trendline cache only when payload changes
- Builds and writes per-device runtime snapshot only when payload changes

## Schedule Policy

Cron runs every minute, and the worker self-gates:

- Quotes:
  - Mon-Fri, 09:30-16:00 America/Toronto
  - One after-close refresh at 16:15
  - Skips weekends
  - Skips Canadian public holidays (best effort via Nager API)
- Trendlines:
  - 08:00 and 18:00 America/Toronto

## Prerequisites

1. Cloudflare account
2. `wrangler` authenticated (`wrangler login`)
3. Firebase service account JSON for your project
4. API keys:
   - Finnhub
   - TwelveData

## Setup

From repo root:

```bash
cd workers/cloudflare-stock-worker
npm install
```

Set secrets (never commit these):

```bash
wrangler secret put FIREBASE_DATABASE_URL
wrangler secret put FIREBASE_SERVICE_ACCOUNT_JSON
wrangler secret put FINNHUB_API_KEY
wrangler secret put TWELVEDATA_API_KEY
```

Optional:

```bash
wrangler secret put HOLIDAY_SOURCE_URL
```

`FIREBASE_SERVICE_ACCOUNT_JSON` must be the full JSON blob, not a file path.

## Deploy

```bash
npm run deploy
```

This deploys with cron trigger from `wrangler.toml`:

- `* * * * *` (every minute)

## Manual Verification

Health:

```bash
curl https://<your-worker>.workers.dev/health
```

Discover symbols:

```bash
curl https://<your-worker>.workers.dev/symbols
```

Force one full run:

```bash
curl "https://<your-worker>.workers.dev/run?forceQuotes=1&forceTrendlines=1"
```

Tail logs:

```bash
npm run tail
```

## Runtime Snapshot Contract (Unchanged)

Each device receives:

`/novaFrame/devices/{deviceId}/runtime/stocks`

Shape:

```json
{
  "generatedAt": 1778551059,
  "symbols": ["AAPL", "MSFT", "TSLA"],
  "quotes": {
    "AAPL": {
      "price": 292.68,
      "change": -0.64,
      "changePct": -0.22,
      "asOf": 1778529600,
      "currency": "USD",
      "source": "finnhub",
      "stale": false
    }
  },
  "trendlines": {
    "AAPL": [291.1, 291.3, 291.0]
  },
  "state": {
    "AAPL": {
      "quoteState": "ok",
      "trendState": "ok"
    }
  }
}
```

## Notes

- Firmware stays stream-only for stocks; no device-side `RTDB.get*` stock reads are required.
- API keys remain server-side in Cloudflare secrets.
- If the holiday API is unavailable, quote updates fail open (updates continue).

