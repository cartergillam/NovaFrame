# NovaFrame Sports Cache Worker

Fetches ESPN scoreboard JSON for MLB, NBA, NFL, and NHL, normalizes favorite-team
snapshots, and writes them to Firebase for the device to read.

## Routes

- `GET /health`
- `GET /favorites`
- `GET /run?force=1`

## Required Secrets

Use the same Firebase secrets as the stock cache worker:

```sh
wrangler secret put FIREBASE_DATABASE_URL
wrangler secret put FIREBASE_SERVICE_ACCOUNT_JSON
```

## Cache Output

Snapshots are written to:

```txt
/novaFrame/cache/sports/{league}/{teamId}
```

The firmware reads that path directly from each league-specific sports app.
