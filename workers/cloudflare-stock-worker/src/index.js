const MARKET_OPEN_MINUTES = 9 * 60 + 30;
const MARKET_CLOSE_MINUTES = 16 * 60;
const AFTER_CLOSE_MINUTES = 16 * 60 + 15;
const TRENDLINE_RUN_MINUTES = new Set([8 * 60, 18 * 60]);
const TORONTO_TIMEZONE = "America/Toronto";
const SYMBOL_PATTERN = /^[A-Z0-9._-]{1,12}$/;

const FINNHUB_QUOTE_URL = "https://finnhub.io/api/v1/quote";
const TWELVEDATA_TIME_SERIES_URL = "https://api.twelvedata.com/time_series";
const GOOGLE_TOKEN_URI = "https://oauth2.googleapis.com/token";
const HOLIDAY_API_URL = "https://date.nager.at/api/v3/PublicHolidays";

let accessTokenCache = null;
let holidayCacheByYear = new Map();

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      return jsonResponse({ ok: true, service: "novaframe-stocks-cache-worker" });
    }

    if (url.pathname === "/symbols") {
      try {
        const result = await discoverSymbols(env);
        return jsonResponse({
          ok: true,
          symbolCount: result.symbols.length,
          symbols: result.symbols,
          deviceCount: Object.keys(result.deviceSymbols).length,
        });
      } catch (error) {
        return jsonResponse({ ok: false, error: stringifyError(error) }, 500);
      }
    }

    if (url.pathname === "/run") {
      const opts = parseRunOptions(url.searchParams);
      try {
        const summary = await runWorker(env, opts);
        return jsonResponse({ ok: true, summary });
      } catch (error) {
        return jsonResponse({ ok: false, error: stringifyError(error) }, 500);
      }
    }

    return jsonResponse(
      {
        ok: true,
        routes: ["/health", "/symbols", "/run?forceQuotes=1&forceTrendlines=1"],
      },
      200,
    );
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(runWorker(env, { source: "scheduled", cron: event.cron }));
  },
};

function parseRunOptions(searchParams) {
  const truthy = (value) => value === "1" || value === "true" || value === "yes";
  return {
    source: "http",
    forceQuotes: truthy((searchParams.get("forceQuotes") || "").toLowerCase()),
    forceTrendlines: truthy((searchParams.get("forceTrendlines") || "").toLowerCase()),
    quotesOnly: truthy((searchParams.get("quotesOnly") || "").toLowerCase()),
    trendlinesOnly: truthy((searchParams.get("trendlinesOnly") || "").toLowerCase()),
  };
}

async function runWorker(env, options = {}) {
  validateEnv(env);

  const now = new Date();
  const toronto = getTorontoClock(now);
  const isHoliday = await isCanadianPublicHoliday(toronto, env);

  const runQuotes = !options.trendlinesOnly && shouldRunQuotes(toronto, isHoliday, !!options.forceQuotes);
  const runTrendlines =
    !options.quotesOnly && shouldRunTrendlines(toronto, !!options.forceTrendlines);

  const discovered = await discoverSymbols(env);
  const symbols = discovered.symbols;
  if (symbols.length === 0) {
    return {
      ok: true,
      source: options.source || "unknown",
      reason: "no_symbols",
      symbols: 0,
      quotesRun: false,
      trendlinesRun: false,
      runtimeWrites: 0,
    };
  }

  const failures = [];
  const quoteStats = { written: 0, unchanged: 0, attempted: 0 };
  const trendStats = { written: 0, unchanged: 0, attempted: 0 };

  if (runQuotes) {
    const result = await writeQuoteCache(env, symbols);
    quoteStats.written = result.written;
    quoteStats.unchanged = result.unchanged;
    quoteStats.attempted = result.attempted;
    failures.push(...result.failures);
  }

  if (runTrendlines) {
    const result = await writeTrendlineCache(env, symbols);
    trendStats.written = result.written;
    trendStats.unchanged = result.unchanged;
    trendStats.attempted = result.attempted;
    failures.push(...result.failures);
  }

  const quoteCache = await loadQuoteCache(env, symbols);
  const trendlineCache = await loadTrendlineCache(env, symbols);
  const runtimeWrites = await writeRuntimeSnapshots(
    env,
    discovered.deviceSymbols,
    quoteCache,
    trendlineCache,
    now,
  );

  return {
    ok: failures.length === 0,
    source: options.source || "unknown",
    cron: options.cron || null,
    torontoNow: `${toronto.dateIso} ${pad2(toronto.hour)}:${pad2(toronto.minute)}`,
    holiday: isHoliday,
    symbols: symbols.length,
    quotesRun: runQuotes,
    trendlinesRun: runTrendlines,
    quoteWrites: quoteStats.written,
    quoteUnchanged: quoteStats.unchanged,
    trendlineWrites: trendStats.written,
    trendlineUnchanged: trendStats.unchanged,
    runtimeWrites,
    failures,
  };
}

function validateEnv(env) {
  const missing = [];
  if (!env.FIREBASE_DATABASE_URL) missing.push("FIREBASE_DATABASE_URL");
  if (!env.FINNHUB_API_KEY) missing.push("FINNHUB_API_KEY");
  if (!env.TWELVEDATA_API_KEY) missing.push("TWELVEDATA_API_KEY");
  if (!env.FIREBASE_SERVICE_ACCOUNT_JSON) missing.push("FIREBASE_SERVICE_ACCOUNT_JSON");
  if (missing.length > 0) {
    throw new Error(`Missing required secrets: ${missing.join(", ")}`);
  }
}

async function discoverSymbols(env) {
  const devices = (await firebaseGet(env, "/novaFrame/devices")) || {};
  const deviceSymbols = collectDeviceSymbols(devices);
  const symbols = collectStockSymbols(deviceSymbols);
  return { symbols, deviceSymbols };
}

function collectDeviceSymbols(devices) {
  const out = {};
  for (const [deviceId, deviceNode] of Object.entries(devices || {})) {
    const unique = new Set();
    const rawSymbols = deviceNode?.apps?.stocks?.symbols;
    if (Array.isArray(rawSymbols)) {
      for (const value of rawSymbols) {
        if (typeof value !== "string") continue;
        const symbol = value.trim().toUpperCase();
        if (!symbol || !SYMBOL_PATTERN.test(symbol)) continue;
        unique.add(symbol);
      }
    }
    out[deviceId] = [...unique].sort();
  }
  return out;
}

function collectStockSymbols(deviceSymbols) {
  const union = new Set();
  for (const symbols of Object.values(deviceSymbols)) {
    for (const symbol of symbols) {
      union.add(symbol);
    }
  }
  return [...union].sort();
}

async function writeQuoteCache(env, symbols) {
  const failures = [];
  let written = 0;
  let unchanged = 0;
  let attempted = 0;

  for (const symbol of symbols) {
    attempted += 1;
    try {
      const quote = await fetchQuote(env, symbol);
      const path = `/novaFrame/cache/stocks/${symbol}`;
      const existing = await firebaseGet(env, path);
      if (!deepEqual(existing, quote)) {
        await firebasePut(env, path, quote);
        written += 1;
      } else {
        unchanged += 1;
      }
    } catch (error) {
      failures.push(`quote ${symbol}: ${stringifyError(error)}`);
    }
  }

  return { written, unchanged, attempted, failures };
}

async function writeTrendlineCache(env, symbols) {
  const failures = [];
  let written = 0;
  let unchanged = 0;
  let attempted = 0;

  for (const symbol of symbols) {
    attempted += 1;
    try {
      const trendline = await fetchTrendline(env, symbol);
      const path = `/novaFrame/cache/trendlines/${symbol}`;
      const existing = await firebaseGet(env, path);
      if (!deepEqual(existing, trendline)) {
        await firebasePut(env, path, trendline);
        written += 1;
      } else {
        unchanged += 1;
      }
    } catch (error) {
      failures.push(`trendline ${symbol}: ${stringifyError(error)}`);
    }
  }

  return { written, unchanged, attempted, failures };
}

async function loadQuoteCache(env, symbols) {
  const out = {};
  for (const symbol of symbols) {
    const value = await firebaseGet(env, `/novaFrame/cache/stocks/${symbol}`);
    if (value != null) out[symbol] = value;
  }
  return out;
}

async function loadTrendlineCache(env, symbols) {
  const out = {};
  for (const symbol of symbols) {
    const value = await firebaseGet(env, `/novaFrame/cache/trendlines/${symbol}`);
    if (value != null) out[symbol] = value;
  }
  return out;
}

async function writeRuntimeSnapshots(env, deviceSymbols, quoteCache, trendlineCache, now) {
  let writes = 0;
  const nowToronto = getTorontoClock(now);
  const nowUnix = Math.floor(now.getTime() / 1000);

  for (const [deviceId, symbols] of Object.entries(deviceSymbols)) {
    const payload = buildRuntimeSnapshot(symbols, quoteCache, trendlineCache, nowToronto, nowUnix);
    const path = `/novaFrame/devices/${deviceId}/runtime/stocks`;
    const existing = (await firebaseGet(env, path)) || {};
    if (!deepEqual(snapshotPayload(existing), payload)) {
      const next = { ...payload, generatedAt: nowUnix };
      await firebasePut(env, path, next);
      writes += 1;
    }
  }
  return writes;
}

function buildRuntimeSnapshot(symbols, quoteCache, trendlineCache, nowToronto, nowUnix) {
  const quotes = {};
  const trendlines = {};
  const state = {};

  for (const symbol of symbols) {
    const quoteRaw = quoteCache[symbol];
    const trendRaw = trendlineCache[symbol];
    const hasQuote = quoteRaw && typeof quoteRaw === "object" && quoteRaw.price != null;
    const hasTrendline = Array.isArray(trendRaw) && trendRaw.length > 0;

    if (hasQuote) {
      const quote = {
        price: toFiniteNumber(quoteRaw.price, 0),
        change: toFiniteNumber(quoteRaw.change, 0),
        changePct: toFiniteNumber(quoteRaw.changePct, 0),
        asOf: toFiniteInt(quoteRaw.asOf, 0),
        currency: String(quoteRaw.currency || inferCurrency(symbol)).toUpperCase(),
        source: quoteRaw.source || "finnhub",
      };
      quote.stale = quoteIsStale(quote, nowToronto, nowUnix);
      quotes[symbol] = quote;
    }

    if (hasTrendline) {
      trendlines[symbol] = trendRaw.map((v) => toFiniteNumber(v, 0));
    }

    let quoteState = hasQuote ? "ok" : "pending";
    if (hasQuote && quotes[symbol].stale) quoteState = "stale";
    const trendState = hasTrendline ? "ok" : "pending";
    state[symbol] = { quoteState, trendState };
  }

  return { symbols, quotes, trendlines, state };
}

function snapshotPayload(snapshot) {
  return {
    symbols: Array.isArray(snapshot?.symbols) ? snapshot.symbols : [],
    quotes: isObject(snapshot?.quotes) ? snapshot.quotes : {},
    trendlines: isObject(snapshot?.trendlines) ? snapshot.trendlines : {},
    state: isObject(snapshot?.state) ? snapshot.state : {},
  };
}

async function fetchQuote(env, symbol) {
  const url = new URL(FINNHUB_QUOTE_URL);
  url.searchParams.set("symbol", symbol);
  url.searchParams.set("token", env.FINNHUB_API_KEY);

  const response = await fetch(url.toString(), { method: "GET" });
  if (!response.ok) {
    throw new Error(`Finnhub ${response.status}: ${await response.text()}`);
  }

  const payload = await response.json();
  const price = payload?.c;
  if (price == null || Number(price) === 0) {
    throw new Error(`No usable quote payload for ${symbol}`);
  }

  return {
    price: roundTo(toFiniteNumber(payload.c, 0), 2),
    change: roundTo(toFiniteNumber(payload.d, 0), 2),
    changePct: roundTo(toFiniteNumber(payload.dp, 0), 2),
    asOf: toFiniteInt(payload.t, Math.floor(Date.now() / 1000)),
    currency: inferCurrency(symbol),
    source: "finnhub",
  };
}

async function fetchTrendline(env, symbol) {
  const url = new URL(TWELVEDATA_TIME_SERIES_URL);
  url.searchParams.set("symbol", symbol);
  url.searchParams.set("interval", "1day");
  url.searchParams.set("outputsize", "64");
  url.searchParams.set("format", "JSON");
  url.searchParams.set("apikey", env.TWELVEDATA_API_KEY);

  const response = await fetch(url.toString(), { method: "GET" });
  if (!response.ok) {
    throw new Error(`TwelveData ${response.status}: ${await response.text()}`);
  }

  const payload = await response.json();
  if (payload?.status === "error") {
    throw new Error(payload?.message || "TwelveData reported error");
  }

  const values = Array.isArray(payload?.values) ? payload.values : [];
  if (values.length === 0) {
    throw new Error("TwelveData returned no values");
  }

  const closes = [];
  for (let i = values.length - 1; i >= 0; i -= 1) {
    const close = toFiniteNumber(values[i]?.close, NaN);
    if (!Number.isFinite(close)) continue;
    closes.push(roundTo(close, 6));
  }

  if (closes.length === 0) {
    throw new Error("TwelveData had no usable close values");
  }

  if (closes.length <= 64) return closes;
  const sampled = [];
  const step = (closes.length - 1) / (64 - 1);
  for (let i = 0; i < 64; i += 1) {
    sampled.push(closes[Math.round(i * step)]);
  }
  return sampled;
}

function shouldRunQuotes(toronto, holiday, force) {
  if (force) return true;
  if (toronto.weekday === 0 || toronto.weekday === 6 || holiday) return false;
  const minutes = toronto.hour * 60 + toronto.minute;
  if (minutes >= MARKET_OPEN_MINUTES && minutes < MARKET_CLOSE_MINUTES) return true;
  return minutes === AFTER_CLOSE_MINUTES;
}

function shouldRunTrendlines(toronto, force) {
  if (force) return true;
  const minutes = toronto.hour * 60 + toronto.minute;
  return TRENDLINE_RUN_MINUTES.has(minutes);
}

function quoteIsStale(quote, toronto, nowUnix) {
  const asOf = toFiniteInt(quote.asOf, 0);
  if (asOf <= 0) return true;
  if (nowUnix <= asOf) return false;

  const ageSeconds = nowUnix - asOf;
  const marketHours =
    toronto.weekday >= 1 &&
    toronto.weekday <= 5 &&
    toronto.hour * 60 + toronto.minute >= MARKET_OPEN_MINUTES &&
    toronto.hour * 60 + toronto.minute < MARKET_CLOSE_MINUTES;
  const threshold = marketHours ? 90 * 60 : 18 * 60 * 60;
  return ageSeconds > threshold;
}

async function isCanadianPublicHoliday(toronto, env) {
  const year = toronto.year;
  const cached = holidayCacheByYear.get(year);
  if (cached && cached.expiresAt > Date.now()) {
    return cached.dates.has(toronto.dateIso);
  }

  const url = `${env.HOLIDAY_SOURCE_URL || HOLIDAY_API_URL}/${year}/CA`;
  try {
    const response = await fetch(url, { method: "GET" });
    if (!response.ok) throw new Error(`Holiday API ${response.status}`);
    const payload = await response.json();
    const dates = new Set();
    if (Array.isArray(payload)) {
      for (const item of payload) {
        if (typeof item?.date === "string" && item.date.length === 10) {
          dates.add(item.date);
        }
      }
    }
    holidayCacheByYear.set(year, {
      dates,
      expiresAt: Date.now() + 6 * 60 * 60 * 1000,
    });
    return dates.has(toronto.dateIso);
  } catch {
    // If the holiday provider is unavailable, fail open (still update quotes).
    return false;
  }
}

function inferCurrency(symbol) {
  const code = String(symbol || "").trim().toUpperCase();
  if (
    code.endsWith(".TO") ||
    code.endsWith(".V") ||
    code.endsWith(".NE") ||
    code.endsWith(".CNQ") ||
    code.endsWith(":CA")
  ) {
    return "CAD";
  }
  return "USD";
}

function getTorontoClock(date) {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: TORONTO_TIMEZONE,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    weekday: "short",
    hour12: false,
  }).formatToParts(date);

  const map = {};
  for (const part of parts) map[part.type] = part.value;
  const weekdayLookup = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 };

  return {
    year: parseInt(map.year, 10),
    month: parseInt(map.month, 10),
    day: parseInt(map.day, 10),
    hour: parseInt(map.hour, 10),
    minute: parseInt(map.minute, 10),
    weekday: weekdayLookup[map.weekday] ?? -1,
    dateIso: `${map.year}-${map.month}-${map.day}`,
  };
}

async function firebaseGet(env, path) {
  const url = await firebaseUrl(env, path);
  const response = await fetch(url, { method: "GET" });
  if (!response.ok) {
    throw new Error(`Firebase GET ${path} failed: ${response.status} ${await response.text()}`);
  }
  return response.json();
}

async function firebasePut(env, path, value) {
  const url = await firebaseUrl(env, path);
  const response = await fetch(url, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(value),
  });
  if (!response.ok) {
    throw new Error(`Firebase PUT ${path} failed: ${response.status} ${await response.text()}`);
  }
}

async function firebaseUrl(env, path) {
  const token = await getGoogleAccessToken(env);
  const base = String(env.FIREBASE_DATABASE_URL || "").replace(/\/+$/, "");
  const normalizedPath = normalizeFirebasePath(path);
  const encodedPath = normalizedPath
    .split("/")
    .filter(Boolean)
    .map((segment) => encodeURIComponent(segment))
    .join("/");
  return `${base}/${encodedPath}.json?access_token=${encodeURIComponent(token)}`;
}

function normalizeFirebasePath(path) {
  const input = String(path || "");
  if (input.startsWith("/")) return input;
  return `/${input}`;
}

async function getGoogleAccessToken(env) {
  if (accessTokenCache && accessTokenCache.expiresAt > Date.now() + 60_000) {
    return accessTokenCache.token;
  }

  const serviceAccount = JSON.parse(env.FIREBASE_SERVICE_ACCOUNT_JSON);
  const clientEmail = serviceAccount.client_email;
  const privateKeyPem = serviceAccount.private_key;
  const tokenUri = serviceAccount.token_uri || GOOGLE_TOKEN_URI;

  if (!clientEmail || !privateKeyPem) {
    throw new Error("Service account JSON must include client_email and private_key");
  }

  const now = Math.floor(Date.now() / 1000);
  const header = { alg: "RS256", typ: "JWT" };
  const claims = {
    iss: clientEmail,
    sub: clientEmail,
    aud: tokenUri,
    iat: now,
    exp: now + 3600,
    scope: "https://www.googleapis.com/auth/firebase.database https://www.googleapis.com/auth/userinfo.email",
  };

  const unsigned = `${base64UrlJson(header)}.${base64UrlJson(claims)}`;
  const signature = await signRs256(unsigned, privateKeyPem);
  const assertion = `${unsigned}.${signature}`;

  const body = new URLSearchParams();
  body.set("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer");
  body.set("assertion", assertion);

  const response = await fetch(tokenUri, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: body.toString(),
  });
  if (!response.ok) {
    throw new Error(`OAuth token exchange failed: ${response.status} ${await response.text()}`);
  }

  const payload = await response.json();
  accessTokenCache = {
    token: payload.access_token,
    expiresAt: Date.now() + (toFiniteInt(payload.expires_in, 3600) * 1000),
  };
  return accessTokenCache.token;
}

async function signRs256(input, privateKeyPem) {
  const key = await crypto.subtle.importKey(
    "pkcs8",
    pemToArrayBuffer(privateKeyPem),
    {
      name: "RSASSA-PKCS1-v1_5",
      hash: "SHA-256",
    },
    false,
    ["sign"],
  );

  const signature = await crypto.subtle.sign(
    "RSASSA-PKCS1-v1_5",
    key,
    new TextEncoder().encode(input),
  );
  return toBase64Url(signature);
}

function pemToArrayBuffer(pem) {
  const normalized = String(pem || "")
    .replace(/-----BEGIN PRIVATE KEY-----/g, "")
    .replace(/-----END PRIVATE KEY-----/g, "")
    .replace(/\s+/g, "");
  const binary = atob(normalized);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes.buffer;
}

function base64UrlJson(value) {
  return toBase64Url(new TextEncoder().encode(JSON.stringify(value)));
}

function toBase64Url(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  let binary = "";
  for (let i = 0; i < bytes.length; i += 1) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function deepEqual(a, b) {
  return stableStringify(a) === stableStringify(b);
}

function stableStringify(value) {
  if (Array.isArray(value)) {
    return `[${value.map((item) => stableStringify(item)).join(",")}]`;
  }
  if (isObject(value)) {
    const keys = Object.keys(value).sort();
    return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(value[key])}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function toFiniteNumber(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function toFiniteInt(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.trunc(number) : fallback;
}

function roundTo(value, decimals) {
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
}

function pad2(value) {
  return String(value).padStart(2, "0");
}

function stringifyError(error) {
  if (error instanceof Error) return error.message;
  try {
    return JSON.stringify(error);
  } catch {
    return String(error);
  }
}

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload, null, 2), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });
}

