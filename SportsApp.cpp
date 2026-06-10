#include "SportsApp.h"

#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <map>
#include <time.h>

#include "DeviceConfig.h"
#include "DisplayHelpers.h"
#include "SportsLogos.h"

namespace {

FirebaseData sportsFbdo;

struct SportsSnapshot {
  String league = "";
  String teamId = "";
  String teamAbbr = "";
  String teamName = "";
  String opponentAbbr = "";
  String opponentName = "";
  String homeAway = "";
  String state = "";
  String status = "";
  String detail = "";
  String teamScore = "--";
  String opponentScore = "--";
  String teamRecord = "";
  String opponentRecord = "";
  String teamColor = "";
  String opponentColor = "";
  String gameTimeUtc = "";
  unsigned long fetchedAt = 0;
};

std::map<String, SportsSnapshot> sportsSnapshots;
const unsigned long kRotateIntervalMs = 5000;
const unsigned long kRefreshIntervalMs = 60000;

struct LeagueEndpoint {
  const char* sport;
  const char* league;
};

LeagueEndpoint endpointForLeague(const String& league) {
  if (league == "mlb") return {"baseball", "mlb"};
  if (league == "nba") return {"basketball", "nba"};
  if (league == "nfl") return {"football", "nfl"};
  return {"hockey", "nhl"};
}

String favoriteKey(const TeamFavorite& favorite) {
  return favorite.league + ":" + favorite.teamId;
}

String variantToString(JsonVariant value, const String& defaultValue = "") {
  if (value.isNull()) return defaultValue;
  if (value.is<long>() || value.is<int>()) return String(value.as<long>());
  if (value.is<float>() || value.is<double>()) return String(value.as<float>(), 0);
  const char* raw = value.as<const char*>();
  if (raw != nullptr) return String(raw);
  return defaultValue;
}

String normalizedLeague(const String& league) {
  String out = league;
  out.trim();
  out.toLowerCase();
  return out;
}

String normalizedTeam(const String& team) {
  String out = team;
  out.trim();
  out.toUpperCase();
  return out;
}

String safeString(JsonVariant value, const String& defaultValue = "") {
  const char* raw = value.as<const char*>();
  return raw == nullptr ? defaultValue : String(raw);
}

String pickString(JsonVariant primary, JsonVariant fallback, const String& defaultValue = "") {
  String primaryValue = variantToString(primary, "");
  if (primaryValue.length() > 0) return primaryValue;
  String fallbackValue = variantToString(fallback, "");
  if (fallbackValue.length() > 0) return fallbackValue;
  return defaultValue;
}

uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

uint8_t hexByte(const String& hex, int offset) {
  return (hexNibble(hex[offset]) << 4) | hexNibble(hex[offset + 1]);
}

uint16_t rowColor(const String& rawHex, const String& league, bool homeRow) {
  String hex = rawHex;
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() == 6) {
    uint8_t r = hexByte(hex, 0);
    uint8_t g = hexByte(hex, 2);
    uint8_t b = hexByte(hex, 4);
    if (r + g + b > 650 || r + g + b < 35) {
      return getScaledColor(homeRow ? 32 : 24, homeRow ? 64 : 48, homeRow ? 112 : 80);
    }
    return getScaledColor(r / 2, g / 2, b / 2);
  }
  if (league == "nba") return getScaledColor(homeRow ? 28 : 34, homeRow ? 76 : 42, homeRow ? 142 : 52);
  if (league == "nfl") return getScaledColor(homeRow ? 12 : 18, homeRow ? 38 : 82, homeRow ? 76 : 58);
  if (league == "mlb") return getScaledColor(homeRow ? 12 : 96, homeRow ? 42 : 18, homeRow ? 90 : 24);
  return getScaledColor(homeRow ? 34 : 92, homeRow ? 38 : 18, homeRow ? 38 : 34);
}

String fallbackTeamColor(const String& league, const String& team) {
  String key = normalizedLeague(league) + ":" + normalizedTeam(team);
  if (key == "mlb:TOR") return "134A8E";
  if (key == "mlb:NYY") return "003087";
  if (key == "mlb:BOS") return "BD3039";
  if (key == "mlb:LAD") return "005A9C";
  if (key == "mlb:SEA") return "005C5C";
  if (key == "nba:TOR") return "CE1141";
  if (key == "nba:NY") return "006BB6";
  if (key == "nba:BOS") return "007A33";
  if (key == "nba:LAL") return "552583";
  if (key == "nba:GS") return "1D428A";
  if (key == "nhl:TOR") return "00205B";
  if (key == "nhl:MTL") return "AF1E2D";
  if (key == "nhl:BOS") return "FFB81C";
  if (key == "nhl:NYR") return "0038A8";
  if (key == "nfl:BUF") return "00338D";
  if (key == "nfl:NYJ") return "125740";
  if (key == "nfl:DAL") return "003594";
  return "";
}

uint16_t teamRowColor(const String& colorHex, const String& league, const String& team, bool homeRow) {
  String color = colorHex.length() > 0 ? colorHex : fallbackTeamColor(league, team);
  return rowColor(color, league, homeRow);
}

uint16_t headerColor() {
  return getScaledColor(255, 165, 0);
}

String compactText(const String& value, int maxChars) {
  if (value.length() <= maxChars) return value;
  if (maxChars <= 1) return value.substring(0, maxChars);
  return value.substring(0, maxChars - 1) + "_";
}

String teamDisplay(const String& abbr, const String& fallback) {
  if (abbr.length() > 0) return compactText(abbr, 4);
  return compactText(fallback, 4);
}

String scoreDisplay(const SportsSnapshot& snapshot, bool favoriteRow) {
  String score = favoriteRow ? snapshot.teamScore : snapshot.opponentScore;
  String record = favoriteRow ? snapshot.teamRecord : snapshot.opponentRecord;
  if (snapshot.state == "pre" || snapshot.state == "no_game") {
    return compactText(record.length() > 0 ? record : "--", 6);
  }
  return compactText(score.length() > 0 ? score : "--", 4);
}

uint16_t tinyGlyph(char c) {
  switch (c) {
    case '0': return 0b111101101101111;
    case '1': return 0b010110010010111;
    case '2': return 0b111001111100111;
    case '3': return 0b111001111001111;
    case '4': return 0b101101111001001;
    case '5': return 0b111100111001111;
    case '6': return 0b111100111101111;
    case '7': return 0b111001010010010;
    case '8': return 0b111101111101111;
    case '9': return 0b111101111001111;
    case 'A': return 0b010101111101101;
    case 'B': return 0b110101110101110;
    case 'C': return 0b111100100100111;
    case 'D': return 0b110101101101110;
    case 'E': return 0b111100110100111;
    case 'F': return 0b111100110100100;
    case 'G': return 0b111100101101111;
    case 'H': return 0b101101111101101;
    case 'I': return 0b111010010010111;
    case 'J': return 0b001001001101111;
    case 'K': return 0b101101110101101;
    case 'L': return 0b100100100100111;
    case 'M': return 0b101111111101101;
    case 'N': return 0b110101101101101;
    case 'O': return 0b111101101101111;
    case 'P': return 0b111101111100100;
    case 'Q': return 0b111101101111001;
    case 'R': return 0b110101110101101;
    case 'S': return 0b111100111001111;
    case 'T': return 0b111010010010010;
    case 'U': return 0b101101101101111;
    case 'V': return 0b101101101101010;
    case 'W': return 0b101101111111101;
    case 'X': return 0b101101010101101;
    case 'Y': return 0b101101010010010;
    case 'Z': return 0b111001010100111;
    case '-': return 0b000000111000000;
    case '/': return 0b001001010100100;
    case ':': return 0b000010000010000;
    case '.': return 0b000000000000010;
    case '@': return 0b111101111100111;
    default: return 0;
  }
}

int tinyTextWidth(const String& text) {
  return text.length() == 0 ? 0 : static_cast<int>(text.length()) * 4 - 1;
}

void drawTinyText(const String& rawText, int x, int y, uint16_t color) {
  String text = rawText;
  text.toUpperCase();
  for (int i = 0; i < static_cast<int>(text.length()); ++i) {
    char c = text[i];
    if (c == ' ') continue;
    uint16_t glyph = tinyGlyph(c);
    for (int row = 0; row < 5; ++row) {
      for (int col = 0; col < 3; ++col) {
        int bit = 14 - (row * 3 + col);
        if (glyph & (1 << bit)) {
          matrix.drawPixel(x + i * 4 + col, y + row, color);
        }
      }
    }
  }
}

void drawTinyRightText(const String& text, int y, uint16_t color, int rightX) {
  drawTinyText(text, rightX - tinyTextWidth(text), y, color);
}

void drawLogoOrFallback(const String& league, const String& team, int x, int y) {
  uint8_t size = sportsLogoRowSize(league, team, 12);
  int yOffset = (12 - size) / 2;
  if (drawSportsLogo(league, team, x, y + yOffset, size)) return;
  drawTinyText(compactText(team, 2), x + 1, y + 4, getScaledColor(255, 255, 255));
}

void drawTeamRow(const String& league, const String& team, const String& label, const String& value,
                 const String& colorHex, bool homeRow, bool winner, int y, int xOffset) {
  matrix.fillRect(0, y, PANEL_WIDTH, 12, teamRowColor(colorHex, league, team, homeRow));
  matrix.drawFastHLine(0, y, PANEL_WIDTH, getScaledColor(16, 16, 16));
  drawLogoOrFallback(league, team, xOffset + 1, y);

  matrix.setTextColor(winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255));
  drawTinyText(label, 16 + xOffset, y + 4, winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255));

  drawTinyRightText(value, y + 4, winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255), 62 + xOffset);
}

String espnUrl(const String& league, const String& teamId) {
  LeagueEndpoint endpoint = endpointForLeague(league);
  String url = "https://site.api.espn.com/apis/site/v2/sports/";
  url += endpoint.sport;
  url += "/";
  url += endpoint.league;
  url += "/scoreboard?limit=100";
  if (teamId.length() > 0) {
    url += "&teams=";
    url += teamId;
  }
  time_t now = time(nullptr);
  if (now > 1700000000) {
    char startBuf[9];
    char endBuf[9];
    time_t start = now - 86400;
    time_t end = now + 7 * 86400;
    strftime(startBuf, sizeof(startBuf), "%Y%m%d", gmtime(&start));
    strftime(endBuf, sizeof(endBuf), "%Y%m%d", gmtime(&end));
    url += "&dates=";
    url += startBuf;
    url += "-";
    url += endBuf;
  }
  return url;
}

String teamColorFrom(JsonObject team) {
  return pickString(team["color"], team["alternateColor"], "");
}

String recordFrom(JsonObject competitor) {
  JsonArray records = competitor["records"].as<JsonArray>();
  if (!records.isNull() && records.size() > 0) {
    return safeString(records[0]["summary"], "");
  }
  return "";
}

String shortStatus(JsonObject event, const String& state) {
  String text = pickString(event["status"]["type"]["shortDetail"], event["status"]["type"]["detail"], "");
  text.replace("Final", "F");
  text.replace("1st Period", "P1");
  text.replace("2nd Period", "P2");
  text.replace("3rd Period", "P3");
  text.replace("1st Quarter", "Q1");
  text.replace("2nd Quarter", "Q2");
  text.replace("3rd Quarter", "Q3");
  text.replace("4th Quarter", "Q4");
  if (text.length() == 0) {
    if (state == "pre") return "PRE";
    if (state == "in") return "LIVE";
    if (state == "post") return "F";
  }
  return compactText(text, 10);
}

bool parseEspnEvent(JsonObject event, const String& league, const String& teamId, SportsSnapshot& snapshot) {
  JsonArray competitions = event["competitions"].as<JsonArray>();
  if (competitions.isNull() || competitions.size() == 0) return false;
  JsonObject competition = competitions[0].as<JsonObject>();
  JsonArray competitors = competition["competitors"].as<JsonArray>();
  if (competitors.isNull() || competitors.size() < 2) return false;

  JsonObject favorite;
  JsonObject opponent;
  for (JsonVariant v : competitors) {
    JsonObject competitor = v.as<JsonObject>();
    String abbr = normalizedTeam(safeString(competitor["team"]["abbreviation"], ""));
    if (abbr == teamId) {
      favorite = competitor;
    } else {
      opponent = competitor;
    }
  }
  if (favorite.isNull() || opponent.isNull()) return false;

  String state = safeString(event["status"]["type"]["state"], "pre");
  state.toLowerCase();
  snapshot.league = league;
  snapshot.teamId = teamId;
  snapshot.teamAbbr = normalizedTeam(safeString(favorite["team"]["abbreviation"], teamId));
  snapshot.teamName = pickString(favorite["team"]["shortDisplayName"], favorite["team"]["displayName"], snapshot.teamAbbr);
  snapshot.opponentAbbr = normalizedTeam(safeString(opponent["team"]["abbreviation"], ""));
  snapshot.opponentName = pickString(opponent["team"]["shortDisplayName"], opponent["team"]["displayName"], snapshot.opponentAbbr);
  snapshot.homeAway = safeString(favorite["homeAway"], "home") == "away" ? "@" : "vs";
  snapshot.state = state;
  snapshot.status = shortStatus(event, state);
  snapshot.detail = snapshot.status;
  snapshot.teamScore = state == "pre" ? "" : variantToString(favorite["score"], "");
  snapshot.opponentScore = state == "pre" ? "" : variantToString(opponent["score"], "");
  snapshot.teamRecord = recordFrom(favorite);
  snapshot.opponentRecord = recordFrom(opponent);
  snapshot.teamColor = teamColorFrom(favorite["team"].as<JsonObject>());
  snapshot.opponentColor = teamColorFrom(opponent["team"].as<JsonObject>());
  snapshot.gameTimeUtc = safeString(event["date"], "");

  Serial.println("🏟️ ESPN selected " + snapshot.opponentAbbr + " vs " + snapshot.teamAbbr);
  Serial.println("🏟️ ESPN scores/status " + snapshot.opponentScore + "-" + snapshot.teamScore + " " + snapshot.status);
  return true;
}

int snapshotRank(const SportsSnapshot& snapshot) {
  if (snapshot.state == "in") return 0;
  if (snapshot.state == "pre") return 1;
  if (snapshot.state == "post") return 2;
  return 3;
}

bool fetchSnapshotFromEspn(const TeamFavorite& favorite, SportsSnapshot& snapshot) {
  String league = normalizedLeague(favorite.league);
  String teamId = normalizedTeam(favorite.teamId);
  String url = espnUrl(league, teamId);
  Serial.println("🏟️ ESPN request: " + url);

  HTTPClient http;
  http.begin(url);
  int status = http.GET();
  Serial.printf("🏟️ ESPN HTTP status: %d\n", status);
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    Serial.println("🏟️ ESPN JSON parse failed: " + String(error.c_str()));
    return false;
  }

  JsonArray events = doc["events"].as<JsonArray>();
  int gameCount = events.isNull() ? 0 : events.size();
  Serial.printf("🏟️ ESPN games found: %d\n", gameCount);
  if (events.isNull()) return false;

  bool found = false;
  SportsSnapshot best;
  int bestRank = 99;
  for (JsonVariant v : events) {
    SportsSnapshot candidate;
    if (!parseEspnEvent(v.as<JsonObject>(), league, teamId, candidate)) continue;
    int rank = snapshotRank(candidate);
    if (!found || rank < bestRank) {
      best = candidate;
      bestRank = rank;
      found = true;
    }
  }

  if (found) {
    snapshot = best;
    Serial.println("🏟️ ESPN selected final " + snapshot.opponentAbbr + " " + snapshot.homeAway + " " + snapshot.teamAbbr);
    Serial.println("🏟️ ESPN parsed scores/status " + snapshot.opponentScore + "-" + snapshot.teamScore + " " + snapshot.status);
    return true;
  }
  snapshot.league = league;
  snapshot.teamId = teamId;
  snapshot.teamAbbr = teamId;
  snapshot.teamName = teamId;
  snapshot.state = "no_game";
  snapshot.status = "NO GAME";
  snapshot.detail = "NO GAME";
  snapshot.teamColor = fallbackTeamColor(league, teamId);
  Serial.println("🏟️ ESPN no matching game for " + league + "/" + teamId);
  return true;
}

bool loadSnapshot(const TeamFavorite& favorite) {
  String key = favoriteKey(favorite);
  SportsSnapshot& snapshot = sportsSnapshots[key];
  if (snapshot.fetchedAt != 0 && millis() - snapshot.fetchedAt < kRefreshIntervalMs) {
    return true;
  }

  snapshot.league = normalizedLeague(favorite.league);
  snapshot.teamId = normalizedTeam(favorite.teamId);
  snapshot.teamAbbr = snapshot.teamId;
  snapshot.teamName = snapshot.teamId;
  snapshot.opponentAbbr = "";
  snapshot.opponentName = "";
  snapshot.homeAway = "";
  snapshot.state = "pending";
  snapshot.status = "NO DATA";
  snapshot.detail = "";
  snapshot.teamScore = "--";
  snapshot.opponentScore = "--";
  snapshot.teamRecord = "";
  snapshot.opponentRecord = "";
  snapshot.teamColor = "";
  snapshot.opponentColor = "";
  snapshot.gameTimeUtc = "";

  bool loadedFromCache = false;
  String path = "/novaFrame/cache/sports/" + snapshot.league + "/" + snapshot.teamId;
  if (Firebase.RTDB.getJSON(&sportsFbdo, path.c_str())) {
    String jsonStr;
    sportsFbdo.jsonObject().toString(jsonStr, true);
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, jsonStr)) {
      snapshot.league = pickString(doc["league"], doc["sport"], snapshot.league);
      snapshot.teamAbbr = pickString(doc["teamAbbr"], doc["teamId"], snapshot.teamId);
      snapshot.teamName = pickString(doc["teamName"], doc["teamAbbr"], snapshot.teamAbbr);
      snapshot.opponentAbbr = pickString(doc["opponentAbbr"], doc["opponent"], "");
      snapshot.opponentName = pickString(doc["opponentName"], doc["opponentAbbr"], snapshot.opponentAbbr);
      snapshot.homeAway = pickString(doc["homeAway"], doc["venue"], "");
      snapshot.state = pickString(doc["state"], doc["gameState"], "pending");
      snapshot.status = pickString(doc["status"], doc["state"], "NO DATA");
      snapshot.detail = pickString(doc["detail"], doc["gameTime"], "");
      snapshot.teamScore = pickString(doc["teamScore"], doc["score"], "--");
      snapshot.opponentScore = pickString(doc["opponentScore"], doc["opponentPoints"], "--");
      snapshot.teamRecord = pickString(doc["teamRecord"], doc["record"], "");
      snapshot.opponentRecord = pickString(doc["opponentRecord"], doc["opponentRecord"], "");
      snapshot.teamColor = pickString(doc["teamColor"], doc["primaryColor"], "");
      snapshot.opponentColor = pickString(doc["opponentColor"], doc["opponentPrimaryColor"], "");
      snapshot.gameTimeUtc = pickString(doc["gameTimeUtc"], doc["startTime"], "");
      loadedFromCache = snapshot.opponentAbbr.length() > 0 || snapshot.state == "no_game";
      Serial.println("🏟️ Sports cache loaded: " + snapshot.league + "/" + snapshot.teamId + " " + snapshot.status);
    }
  }

  if (!loadedFromCache) {
    Serial.println("🏟️ Sports cache miss/no data: " + snapshot.league + "/" + snapshot.teamId);
    fetchSnapshotFromEspn(favorite, snapshot);
  }

  snapshot.fetchedAt = millis();
  return true;
}

}  // namespace

SportsApp::SportsApp(const String& leagueId, const String& leagueLabel)
  : leagueId(normalizedLeague(leagueId)), leagueLabel(leagueLabel) {}

void SportsApp::init() {
  currentFavoriteIndex = 0;
  lastRotateAt = millis();
  setNeedsRedraw(true);
}

void SportsApp::loop() {
  const AppConfig* config = getAppConfig(getAppId());
  if (config == nullptr || config->favorites.empty()) return;

  if (currentFavoriteIndex >= static_cast<int>(config->favorites.size())) {
    currentFavoriteIndex = 0;
    setNeedsRedraw(true);
  }

  if (millis() - lastRotateAt >= kRotateIntervalMs && config->favorites.size() > 1) {
    currentFavoriteIndex = (currentFavoriteIndex + 1) % config->favorites.size();
    lastRotateAt = millis();
    setNeedsRedraw(true);
  }

  loadSnapshot(config->favorites[currentFavoriteIndex]);
}

void SportsApp::redraw(bool force, int xOffset) {
  if (!force && !getNeedsRedraw()) return;

  matrix.fillScreen(0);
  const AppConfig* config = getAppConfig(getAppId());
  if (config == nullptr || config->favorites.empty()) {
    matrix.setTextSize(1);
    matrix.setTextColor(headerColor());
    matrix.setCursor(1 + xOffset, 0);
    matrix.print(leagueLabel);
    drawSmallText("Set teams", 2 + xOffset, 20);
    setNeedsRedraw(false);
    return;
  }

  const TeamFavorite& favorite = config->favorites[currentFavoriteIndex];
  loadSnapshot(favorite);
  const SportsSnapshot& snapshot = sportsSnapshots[favoriteKey(favorite)];

  String awayTeam;
  String awayLabel;
  String awayValue;
  String awayColor;
  bool awayFavorite = snapshot.homeAway == "@";
  if (awayFavorite) {
    awayTeam = snapshot.teamAbbr;
    awayLabel = teamDisplay(snapshot.teamAbbr, snapshot.teamName);
    awayValue = scoreDisplay(snapshot, true);
    awayColor = snapshot.teamColor;
  } else {
    awayTeam = snapshot.opponentAbbr;
    awayLabel = teamDisplay(snapshot.opponentAbbr, snapshot.opponentName);
    awayValue = scoreDisplay(snapshot, false);
    awayColor = snapshot.opponentColor;
  }

  String homeTeam;
  String homeLabel;
  String homeValue;
  String homeColor;
  bool homeFavorite = !awayFavorite;
  if (homeFavorite) {
    homeTeam = snapshot.teamAbbr;
    homeLabel = teamDisplay(snapshot.teamAbbr, snapshot.teamName);
    homeValue = scoreDisplay(snapshot, true);
    homeColor = snapshot.teamColor;
  } else {
    homeTeam = snapshot.opponentAbbr;
    homeLabel = teamDisplay(snapshot.opponentAbbr, snapshot.opponentName);
    homeValue = scoreDisplay(snapshot, false);
    homeColor = snapshot.opponentColor;
  }

  if (snapshot.state == "no_game" || snapshot.opponentAbbr.length() == 0) {
    awayTeam = snapshot.teamAbbr;
    awayLabel = teamDisplay(snapshot.teamAbbr, snapshot.teamName);
    awayValue = snapshot.teamRecord.length() > 0 ? compactText(snapshot.teamRecord, 6) : "--";
    awayColor = snapshot.teamColor;
    homeTeam = "";
    homeLabel = "NO";
    homeValue = "GAME";
    homeColor = "";
  }

  bool teamWon = snapshot.state == "post" && snapshot.teamScore.toInt() > snapshot.opponentScore.toInt();
  bool oppWon = snapshot.state == "post" && snapshot.opponentScore.toInt() > snapshot.teamScore.toInt();
  bool awayWinner = (awayFavorite && teamWon) || (!awayFavorite && oppWon);
  bool homeWinner = (homeFavorite && teamWon) || (!homeFavorite && oppWon);

  matrix.fillRect(0, 0, PANEL_WIDTH, 8, 0);
  drawTinyText(leagueLabel, 1 + xOffset, 1, headerColor());

  String header = snapshot.detail.length() > 0 ? snapshot.detail : snapshot.status;
  drawTinyRightText(compactText(header, 10), 1, getScaledColor(255, 255, 255), 62 + xOffset);

  drawTeamRow(snapshot.league, awayTeam, awayLabel, awayValue, awayColor, false, awayWinner, 8, xOffset);
  drawTeamRow(snapshot.league, homeTeam, homeLabel, homeValue, homeColor, true, homeWinner, 20, xOffset);

  setNeedsRedraw(false);
}

void SportsApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool SportsApp::getNeedsRedraw() {
  return needsRedraw;
}
