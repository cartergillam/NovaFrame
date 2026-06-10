const GOOGLE_TOKEN_URI = "https://oauth2.googleapis.com/token";
const TORONTO_TIMEZONE = "America/Toronto";

const LEAGUES = {
  mlb: { sport: "baseball", label: "MLB" },
  nba: { sport: "basketball", label: "NBA" },
  nfl: { sport: "football", label: "NFL" },
  nhl: { sport: "hockey", label: "NHL" },
};

let accessTokenCache = null;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      return jsonResponse({ ok: true, service: "novaframe-sports-cache-worker" });
    }

    if (url.pathname === "/favorites") {
      try {
        const favorites = await discoverFavorites(env);
        return jsonResponse({ ok: true, favorites });
      } catch (error) {
        return jsonResponse({ ok: false, error: stringifyError(error) }, 500);
      }
    }

    if (url.pathname === "/run") {
      try {
        const summary = await runWorker(env, { source: "http", force: truthy(url.searchParams.get("force")) });
        return jsonResponse({ ok: true, summary });
      } catch (error) {
        return jsonResponse({ ok: false, error: stringifyError(error) }, 500);
      }
    }

    return jsonResponse({ ok: true, routes: ["/health", "/favorites", "/run?force=1"] });
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(runWorker(env, { source: "scheduled", cron: event.cron }));
  },
};

async function runWorker(env, options = {}) {
  validateEnv(env);
  const favorites = await discoverFavorites(env);
  const failures = [];
  const writes = [];

  for (const [league, teams] of Object.entries(favorites)) {
    if (!LEAGUES[league] || teams.length === 0) continue;
    try {
      const events = await fetchLeagueEvents(league);
      for (const teamId of teams) {
        try {
          const snapshot = buildTeamSnapshot(league, teamId, events);
          const path = `/novaFrame/cache/sports/${league}/${teamId}`;
          const existing = await firebaseGet(env, path);
          if (!deepEqual(snapshotComparable(existing), snapshotComparable(snapshot))) {
            await firebasePut(env, path, snapshot);
            writes.push(`${league}:${teamId}`);
          }
        } catch (error) {
          failures.push(`${league}:${teamId}: ${stringifyError(error)}`);
        }
      }
    } catch (error) {
      failures.push(`${league}: ${stringifyError(error)}`);
    }
  }

  return {
    ok: failures.length === 0,
    source: options.source || "unknown",
    cron: options.cron || null,
    leagues: Object.fromEntries(Object.entries(favorites).map(([league, teams]) => [league, teams.length])),
    writes,
    failures,
    generatedAt: Date.now(),
  };
}

function validateEnv(env) {
  const missing = [];
  if (!env.FIREBASE_DATABASE_URL) missing.push("FIREBASE_DATABASE_URL");
  if (!env.FIREBASE_SERVICE_ACCOUNT_JSON) missing.push("FIREBASE_SERVICE_ACCOUNT_JSON");
  if (missing.length > 0) {
    throw new Error(`Missing required secrets: ${missing.join(", ")}`);
  }
}

async function discoverFavorites(env) {
  const devices = (await firebaseGet(env, "/novaFrame/devices")) || {};
  const favorites = { mlb: new Set(), nba: new Set(), nfl: new Set(), nhl: new Set() };

  for (const device of Object.values(devices || {})) {
    const apps = device?.apps || {};
    for (const league of Object.keys(LEAGUES)) {
      collectAppFavorites(favorites, league, apps[league]);
    }
    collectAppFavorites(favorites, "", apps.sports);
  }

  return Object.fromEntries(
    Object.entries(favorites).map(([league, teams]) => [league, [...teams].sort()]),
  );
}

function collectAppFavorites(out, fallbackLeague, app) {
  const rawFavorites = app?.favorites;
  if (!Array.isArray(rawFavorites)) return;
  for (const value of rawFavorites) {
    const league = normalizeLeague(value?.league || fallbackLeague);
    const teamId = normalizeTeamId(value?.teamId || value?.team || value);
    if (!out[league] || !teamId) continue;
    out[league].add(teamId);
  }
}

async function fetchLeagueEvents(league) {
  const config = LEAGUES[league];
  const now = new Date();
  const start = addDays(now, -1);
  const end = addDays(now, 7);
  const dates = `${formatDateParam(start)}-${formatDateParam(end)}`;
  const url = new URL(`https://site.api.espn.com/apis/site/v2/sports/${config.sport}/${league}/scoreboard`);
  url.searchParams.set("limit", "100");
  url.searchParams.set("dates", dates);

  const response = await fetch(url.toString(), {
    method: "GET",
    headers: { "accept": "application/json", "user-agent": "NovaFrame sports cache" },
  });
  if (!response.ok) {
    throw new Error(`ESPN ${league} ${response.status}: ${await response.text()}`);
  }
  const payload = await response.json();
  return Array.isArray(payload?.events) ? payload.events : [];
}

function buildTeamSnapshot(league, teamId, events) {
  const normalizedTeam = normalizeTeamId(teamId);
  const candidates = [];
  for (const event of events) {
    const competition = event?.competitions?.[0];
    const competitors = Array.isArray(competition?.competitors) ? competition.competitors : [];
    const teamCompetitor = competitors.find((competitor) => normalizeTeamId(competitor?.team?.abbreviation) === normalizedTeam);
    if (!teamCompetitor) continue;
    const opponent = competitors.find((competitor) => competitor !== teamCompetitor);
    candidates.push({ event, competition, teamCompetitor, opponent, rank: eventRank(event) });
  }

  if (candidates.length === 0) {
    return noGameSnapshot(league, normalizedTeam);
  }

  candidates.sort((a, b) => {
    if (a.rank !== b.rank) return a.rank - b.rank;
    return new Date(a.event?.date || 0).getTime() - new Date(b.event?.date || 0).getTime();
  });

  return eventSnapshot(league, normalizedTeam, candidates[0]);
}

function eventRank(event) {
  const state = String(event?.status?.type?.state || "").toLowerCase();
  if (state === "in") return 0;
  if (state === "pre") return 1;
  if (state === "post") return 2;
  return 3;
}

function eventSnapshot(league, teamId, candidate) {
  const { event, teamCompetitor, opponent } = candidate;
  const state = normalizeState(event?.status?.type?.state);
  const team = teamCompetitor?.team || {};
  const opp = opponent?.team || {};
  const teamScore = String(teamCompetitor?.score ?? "");
  const opponentScore = String(opponent?.score ?? "");
  const favoriteHomeAway = String(teamCompetitor?.homeAway || "");
  const homeAway = favoriteHomeAway === "away" ? "@" : "vs";
  const gameDate = event?.date ? new Date(event.date) : null;

  return {
    league,
    teamId,
    teamAbbr: normalizeTeamId(team.abbreviation || teamId),
    teamName: String(team.shortDisplayName || team.displayName || team.name || teamId),
    opponentAbbr: normalizeTeamId(opp.abbreviation || ""),
    opponentName: String(opp.shortDisplayName || opp.displayName || opp.name || ""),
    homeAway,
    state,
    status: statusText(event, state, gameDate),
    detail: detailText(event, state, gameDate),
    teamScore: state === "pre" ? "" : teamScore,
    opponentScore: state === "pre" ? "" : opponentScore,
    teamRecord: recordSummary(teamCompetitor),
    opponentRecord: recordSummary(opponent),
    teamColor: normalizeHexColor(team.color),
    opponentColor: normalizeHexColor(opp.color),
    gameTimeUtc: gameDate ? gameDate.toISOString() : "",
    updatedAt: Date.now(),
  };
}

function noGameSnapshot(league, teamId) {
  return {
    league,
    teamId,
    teamAbbr: teamId,
    teamName: teamId,
    opponentAbbr: "",
    opponentName: "",
    homeAway: "",
    state: "no_game",
    status: "NO GAME",
    detail: "NO GAME",
    teamScore: "",
    opponentScore: "",
    teamRecord: "",
    opponentRecord: "",
    teamColor: "",
    opponentColor: "",
    gameTimeUtc: "",
    updatedAt: Date.now(),
  };
}

function normalizeState(state) {
  const value = String(state || "").toLowerCase();
  if (value === "in") return "in";
  if (value === "post") return "post";
  if (value === "pre") return "pre";
  return "unknown";
}

function statusText(event, state, gameDate) {
  if (state === "pre" && gameDate) return formatMonthDay(gameDate);
  const shortDetail = String(event?.status?.type?.shortDetail || event?.status?.type?.detail || "");
  if (state === "post") return shortenStatus(shortDetail || "FINAL");
  if (state === "in") return shortenStatus(shortDetail || "LIVE");
  return shortenStatus(shortDetail || "GAME");
}

function detailText(event, state, gameDate) {
  if (state === "pre" && gameDate) return formatGameTime(gameDate);
  const shortDetail = String(event?.status?.type?.shortDetail || event?.status?.type?.detail || "");
  return shortenStatus(shortDetail || statusText(event, state, gameDate));
}

function shortenStatus(value) {
  return String(value || "")
    .replace(/\bFinal\b/g, "F")
    .replace(/\bFinal\/OT\b/g, "F/OT")
    .replace(/\b1st Period\b/g, "P1")
    .replace(/\b2nd Period\b/g, "P2")
    .replace(/\b3rd Period\b/g, "P3")
    .replace(/\b1st Quarter\b/g, "Q1")
    .replace(/\b2nd Quarter\b/g, "Q2")
    .replace(/\b3rd Quarter\b/g, "Q3")
    .replace(/\b4th Quarter\b/g, "Q4")
    .replace(/\bHalftime\b/g, "HALF")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 12);
}

function recordSummary(competitor) {
  const records = Array.isArray(competitor?.records) ? competitor.records : [];
  const overall = records.find((record) => record?.type === "total" || record?.name === "overall") || records[0];
  return String(overall?.summary || competitor?.record || "");
}

function normalizeHexColor(value) {
  const hex = String(value || "").replace(/^#/, "").trim();
  return /^[0-9a-fA-F]{6}$/.test(hex) ? hex.toUpperCase() : "";
}

function normalizeLeague(value) {
  const league = String(value || "").trim().toLowerCase();
  return LEAGUES[league] ? league : "";
}

function normalizeTeamId(value) {
  return String(value || "").trim().toUpperCase().replace(/[^A-Z0-9]/g, "");
}

function addDays(date, days) {
  const next = new Date(date);
  next.setUTCDate(next.getUTCDate() + days);
  return next;
}

function formatDateParam(date) {
  return `${date.getUTCFullYear()}${pad2(date.getUTCMonth() + 1)}${pad2(date.getUTCDate())}`;
}

function formatMonthDay(date) {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TORONTO_TIMEZONE,
    month: "short",
    day: "numeric",
  }).formatToParts(date);
  const month = parts.find((part) => part.type === "month")?.value || "";
  const day = parts.find((part) => part.type === "day")?.value || "";
  return `${month.toUpperCase()} ${day}`;
}

function formatGameTime(date) {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TORONTO_TIMEZONE,
    hour: "numeric",
    minute: "2-digit",
    hour12: true,
  }).formatToParts(date);
  const hour = parts.find((part) => part.type === "hour")?.value || "";
  const minute = parts.find((part) => part.type === "minute")?.value || "00";
  const dayPeriod = parts.find((part) => part.type === "dayPeriod")?.value || "";
  return `${hour}:${minute}${dayPeriod.slice(0, 1).toUpperCase()}`;
}

function pad2(value) {
  return String(value).padStart(2, "0");
}

function truthy(value) {
  const normalized = String(value || "").toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes";
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
  const encodedPath = String(path || "")
    .split("/")
    .filter(Boolean)
    .map((segment) => encodeURIComponent(segment))
    .join("/");
  return `${base}/${encodedPath}.json?access_token=${encodeURIComponent(token)}`;
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
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
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
  let binary = "";
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function toFiniteInt(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.trunc(number) : fallback;
}

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload, null, 2), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });
}

function stringifyError(error) {
  if (error instanceof Error) return error.message;
  return String(error);
}

function deepEqual(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function snapshotComparable(snapshot) {
  if (!snapshot || typeof snapshot !== "object") return {};
  const { updatedAt, ...rest } = snapshot;
  return rest;
}
