#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from datetime import datetime, time
from typing import Any, Iterable
from zoneinfo import ZoneInfo

import holidays
import requests

try:
    import firebase_admin
    from firebase_admin import credentials, db
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "firebase-admin is required. Install dependencies with: pip install -r requirements.txt"
    ) from exc


TORONTO_TZ = ZoneInfo("America/Toronto")
MARKET_OPEN = time(9, 30)
MARKET_CLOSE = time(16, 0)
AFTER_CLOSE = time(16, 15)
TRENDLINE_RUN_TIMES = {time(8, 0), time(18, 0)}
FINNHUB_QUOTE_URL = "https://finnhub.io/api/v1/quote"
TWELVEDATA_TIME_SERIES_URL = "https://api.twelvedata.com/time_series"
SYMBOL_PATTERN = re.compile(r"^[A-Z0-9._-]{1,12}$")


@dataclass(frozen=True)
class WorkerConfig:
    database_url: str
    finnhub_api_key: str
    twelvedata_api_key: str
    service_account_path: str | None


def load_config() -> WorkerConfig:
    database_url = os.environ.get("FIREBASE_DATABASE_URL", "").strip()
    finnhub_api_key = os.environ.get("FINNHUB_API_KEY", "").strip()
    twelvedata_api_key = os.environ.get("TWELVEDATA_API_KEY", "").strip()
    service_account_path = os.environ.get("FIREBASE_SERVICE_ACCOUNT_PATH", "").strip() or None

    missing = []
    if not database_url:
        missing.append("FIREBASE_DATABASE_URL")
    if not finnhub_api_key:
        missing.append("FINNHUB_API_KEY")
    if not twelvedata_api_key:
        missing.append("TWELVEDATA_API_KEY")

    if missing:
        raise SystemExit("Missing required environment variables: " + ", ".join(missing))

    return WorkerConfig(
        database_url=database_url,
        finnhub_api_key=finnhub_api_key,
        twelvedata_api_key=twelvedata_api_key,
        service_account_path=service_account_path,
    )


def initialize_firebase(config: WorkerConfig) -> None:
    if firebase_admin._apps:
        return

    if config.service_account_path:
        cred = credentials.Certificate(config.service_account_path)
    else:
        raw_json = os.environ.get("FIREBASE_SERVICE_ACCOUNT_JSON", "").strip()
        if raw_json:
            cred = credentials.Certificate(json.loads(raw_json))
        else:
            cred = credentials.ApplicationDefault()

    firebase_admin.initialize_app(cred, {"databaseURL": config.database_url})


def now_toronto() -> datetime:
    return datetime.now(TORONTO_TZ)


def is_market_holiday(current_time: datetime) -> bool:
    ontario_holidays = holidays.country_holidays("CA", subdiv="ON")
    return current_time.date() in ontario_holidays


def should_run_quotes(current_time: datetime, force: bool) -> bool:
    if force:
        return True
    if current_time.weekday() >= 5 or is_market_holiday(current_time):
        return False

    current_clock = current_time.time().replace(second=0, microsecond=0)
    if MARKET_OPEN <= current_clock < MARKET_CLOSE:
        return True
    return current_clock == AFTER_CLOSE


def should_run_trendlines(current_time: datetime, force: bool) -> bool:
    if force:
        return True
    current_clock = current_time.time().replace(second=0, microsecond=0)
    return current_clock in TRENDLINE_RUN_TIMES


def fetch_devices() -> dict[str, Any]:
    return db.reference("/novaFrame/devices").get() or {}


def collect_stock_symbols(devices: dict[str, Any]) -> list[str]:
    return sorted({symbol for symbols in collect_device_symbols(devices).values() for symbol in symbols})


def collect_device_symbols(devices: dict[str, Any]) -> dict[str, list[str]]:
    device_symbols: dict[str, list[str]] = {}
    skipped: list[str] = []
    for device_id, device in devices.items():
        unique: set[str] = set()
        app_config = (((device or {}).get("apps") or {}).get("stocks") or {})
        raw_symbols = app_config.get("symbols") or []
        if not isinstance(raw_symbols, list):
            device_symbols[device_id] = []
            continue
        for symbol in raw_symbols:
            if not isinstance(symbol, str):
                continue
            clean = symbol.strip().upper()
            if not clean:
                continue
            if not SYMBOL_PATTERN.match(clean):
                skipped.append(clean)
                continue
            if clean:
                unique.add(clean)
        device_symbols[device_id] = sorted(unique)
    for symbol in sorted(set(skipped)):
        print(f"Skipping unsupported symbol format: {symbol}", file=sys.stderr)
    return device_symbols


def fetch_quote(session: requests.Session, symbol: str, api_key: str) -> dict[str, Any]:
    response = session.get(
        FINNHUB_QUOTE_URL,
        params={"symbol": symbol, "token": api_key},
        timeout=20,
    )
    response.raise_for_status()
    payload = response.json()

    price = payload.get("c")
    change = payload.get("d")
    change_pct = payload.get("dp")
    as_of = payload.get("t")
    if price in (None, 0):
        raise ValueError(f"Finnhub returned no usable quote for {symbol}: {payload}")

    return {
        "price": round(float(price), 2),
        "change": round(float(change or 0.0), 2),
        "changePct": round(float(change_pct or 0.0), 2),
        "asOf": int(as_of or datetime.now(TORONTO_TZ).timestamp()),
        "currency": infer_currency_for_symbol(symbol),
        "source": "finnhub",
    }


def infer_currency_for_symbol(symbol: str) -> str:
    code = symbol.strip().upper()
    if (
        code.endswith(".TO")
        or code.endswith(".V")
        or code.endswith(".NE")
        or code.endswith(".CNQ")
        or code.endswith(":CA")
    ):
        return "CAD"
    return "USD"


def normalize_trendline(values: Iterable[dict[str, Any]], points: int = 64) -> list[float]:
    closes: list[float] = []
    for item in reversed(list(values)):
        close = item.get("close")
        if close is None:
            continue
        closes.append(round(float(close), 6))

    if not closes:
        raise ValueError("No close values in TwelveData response")

    if len(closes) <= points:
        return closes

    step = (len(closes) - 1) / float(points - 1)
    sampled = []
    for index in range(points):
        source_index = round(index * step)
        sampled.append(closes[source_index])
    return sampled


def fetch_trendline(session: requests.Session, symbol: str, api_key: str) -> list[float]:
    response = session.get(
        TWELVEDATA_TIME_SERIES_URL,
        params={
            "symbol": symbol,
            "interval": "1day",
            "outputsize": 64,
            "apikey": api_key,
            "format": "JSON",
        },
        timeout=20,
    )
    response.raise_for_status()
    payload = response.json()
    if payload.get("status") == "error":
        raise ValueError(f"TwelveData error for {symbol}: {payload.get('message', payload)}")

    values = payload.get("values")
    if not isinstance(values, list) or not values:
        raise ValueError(f"TwelveData returned no values for {symbol}: {payload}")

    return normalize_trendline(values)


def write_quotes(session: requests.Session, symbols: list[str], config: WorkerConfig) -> list[str]:
    quotes_ref = db.reference("/novaFrame/cache/stocks")
    failures: list[str] = []
    for symbol in symbols:
        try:
            quote = fetch_quote(session, symbol, config.finnhub_api_key)
            quotes_ref.child(symbol).set(quote)
            print(f"Wrote quote cache for {symbol}")
        except Exception as exc:  # pragma: no cover
            failures.append(f"{symbol}: {exc}")
    return failures


def write_trendlines(session: requests.Session, symbols: list[str], config: WorkerConfig) -> list[str]:
    trend_ref = db.reference("/novaFrame/cache/trendlines")
    failures: list[str] = []
    for symbol in symbols:
        try:
            trendline = fetch_trendline(session, symbol, config.twelvedata_api_key)
            trend_ref.child(symbol).set(trendline)
            print(f"Wrote trendline cache for {symbol}")
        except Exception as exc:  # pragma: no cover
            failures.append(f"{symbol}: {exc}")
    return failures


def load_quote_cache(symbols: list[str]) -> dict[str, Any]:
    quote_root = db.reference("/novaFrame/cache/stocks")
    cache: dict[str, Any] = {}
    for symbol in symbols:
        value = quote_root.child(symbol).get()
        if value is not None:
            cache[symbol] = value
    return cache


def load_trendline_cache(symbols: list[str]) -> dict[str, Any]:
    trend_root = db.reference("/novaFrame/cache/trendlines")
    cache: dict[str, Any] = {}
    for symbol in symbols:
        value = trend_root.child(symbol).get()
        if value is not None:
            cache[symbol] = value
    return cache


def quote_is_stale(quote: dict[str, Any], current_time: datetime) -> bool:
    as_of = int(quote.get("asOf") or 0)
    if as_of <= 0:
        return True

    now_ts = int(current_time.timestamp())
    if now_ts <= as_of:
        return False

    age_seconds = now_ts - as_of
    market_hours = MARKET_OPEN <= current_time.time() < MARKET_CLOSE and current_time.weekday() < 5
    threshold_seconds = 90 * 60 if market_hours else 18 * 60 * 60
    return age_seconds > threshold_seconds


def build_runtime_snapshot(
    symbols: list[str],
    quote_cache: dict[str, Any],
    trendline_cache: dict[str, Any],
    current_time: datetime,
) -> dict[str, Any]:
    quotes: dict[str, Any] = {}
    trendlines: dict[str, Any] = {}
    state: dict[str, Any] = {}

    for symbol in symbols:
        quote_raw = quote_cache.get(symbol)
        trend_raw = trendline_cache.get(symbol)

        has_quote = isinstance(quote_raw, dict) and quote_raw.get("price") is not None
        has_trendline = isinstance(trend_raw, list) and len(trend_raw) > 0

        if has_quote:
            quote = {
                "price": float(quote_raw.get("price", 0.0)),
                "change": float(quote_raw.get("change", 0.0)),
                "changePct": float(quote_raw.get("changePct", 0.0)),
                "asOf": int(quote_raw.get("asOf", 0)),
                "currency": str(quote_raw.get("currency") or infer_currency_for_symbol(symbol)).upper(),
                "source": quote_raw.get("source", "finnhub"),
            }
            quote["stale"] = quote_is_stale(quote, current_time)
            quotes[symbol] = quote

        if has_trendline:
            trendlines[symbol] = [float(value) for value in trend_raw]

        quote_state = "ok" if has_quote else "pending"
        if has_quote and quotes[symbol].get("stale"):
            quote_state = "stale"
        trend_state = "ok" if has_trendline else "pending"
        state[symbol] = {"quoteState": quote_state, "trendState": trend_state}

    return {"symbols": symbols, "quotes": quotes, "trendlines": trendlines, "state": state}


def snapshot_payload(snapshot: dict[str, Any]) -> dict[str, Any]:
    return {
        "symbols": snapshot.get("symbols", []),
        "quotes": snapshot.get("quotes", {}),
        "trendlines": snapshot.get("trendlines", {}),
        "state": snapshot.get("state", {}),
    }


def write_runtime_snapshots(
    device_symbols: dict[str, list[str]],
    quote_cache: dict[str, Any],
    trendline_cache: dict[str, Any],
    current_time: datetime,
) -> int:
    writes = 0
    for device_id, symbols in device_symbols.items():
        runtime_ref = db.reference(f"/novaFrame/devices/{device_id}/runtime/stocks")
        payload = build_runtime_snapshot(symbols, quote_cache, trendline_cache, current_time)
        existing = runtime_ref.get() or {}
        if snapshot_payload(existing) != payload:
            snapshot = dict(payload)
            snapshot["generatedAt"] = int(current_time.timestamp())
            runtime_ref.set(snapshot)
            writes += 1
    return writes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Populate NovaFrame stock caches in Firebase.")
    parser.add_argument("--force-quotes", action="store_true", help="Run quote updates regardless of market window.")
    parser.add_argument("--force-trendlines", action="store_true", help="Run trendline updates regardless of schedule.")
    parser.add_argument("--quotes-only", action="store_true", help="Only process quotes.")
    parser.add_argument("--trendlines-only", action="store_true", help="Only process trendlines.")
    parser.add_argument("--print-symbols", action="store_true", help="Print the deduplicated symbol union and exit.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config = load_config()
    initialize_firebase(config)

    devices = fetch_devices()
    device_symbols = collect_device_symbols(devices)
    symbols = collect_stock_symbols(devices)

    if args.print_symbols:
        print("\n".join(symbols))
        return 0

    if not symbols:
        print("No stock symbols configured across devices.")
        return 0

    current_time = now_toronto()
    run_quotes = not args.trendlines_only and should_run_quotes(current_time, args.force_quotes)
    run_trendlines = not args.quotes_only and should_run_trendlines(current_time, args.force_trendlines)

    session = requests.Session()
    failures: list[str] = []
    quote_successes = 0
    trendline_successes = 0

    if run_quotes:
        quote_failures = write_quotes(session, symbols, config)
        quote_successes = len(symbols) - len(quote_failures)
        failures.extend([f"quote update failed for {failure}" for failure in quote_failures])

    if run_trendlines:
        trendline_failures = write_trendlines(session, symbols, config)
        trendline_successes = len(symbols) - len(trendline_failures)
        failures.extend([f"trendline update failed for {failure}" for failure in trendline_failures])

    quote_cache = load_quote_cache(symbols)
    trendline_cache = load_trendline_cache(symbols)
    runtime_writes = write_runtime_snapshots(device_symbols, quote_cache, trendline_cache, current_time)

    print(
        "Run summary: "
        f"symbols={len(symbols)} "
        f"quotes_written={quote_successes if run_quotes else 0} "
        f"trendlines_written={trendline_successes if run_trendlines else 0} "
        f"runtime_written={runtime_writes} "
        f"failures={len(failures)}"
    )

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
