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
  String liveExtra = "";
  String possessionAbbr = "";
  bool baseOccupied[3] = {false, false, false};
  bool hasBaseState = false;
  bool hasPowerPlay = false;
  bool hasPossession = false;
  int teamTimeouts = -1;
  int opponentTimeouts = -1;
  int teamFouls = -1;
  int opponentFouls = -1;
  unsigned long fetchedAt = 0;
};

std::map<String, SportsSnapshot> sportsSnapshots;
std::map<String, unsigned long> sportsFetchRetryAfter;
const unsigned long kRotateIntervalMs = 5000;
const unsigned long kRefreshIntervalMs = 60000;
const unsigned long kFetchFailureBackoffMs = 120000;

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

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

String favoriteKey(const TeamFavorite& favorite) {
  return normalizedLeague(favorite.league) + ":" + normalizedTeam(favorite.teamId);
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

Rgb leagueFallbackRgb(const String& league, bool homeRow) {
  if (league == "nba") return {static_cast<uint8_t>(homeRow ? 10 : 12), static_cast<uint8_t>(homeRow ? 24 : 14), static_cast<uint8_t>(homeRow ? 44 : 18)};
  if (league == "nfl") return {static_cast<uint8_t>(homeRow ? 8 : 10), static_cast<uint8_t>(homeRow ? 18 : 28), static_cast<uint8_t>(homeRow ? 30 : 22)};
  if (league == "mlb") return {static_cast<uint8_t>(homeRow ? 8 : 34), static_cast<uint8_t>(homeRow ? 18 : 10), static_cast<uint8_t>(homeRow ? 34 : 12)};
  return {static_cast<uint8_t>(homeRow ? 20 : 34), static_cast<uint8_t>(homeRow ? 18 : 10), static_cast<uint8_t>(homeRow ? 18 : 12)};
}

Rgb parseTeamRgb(const String& rawHex, const String& league, bool homeRow) {
  String hex = rawHex;
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() == 6) {
    uint8_t r = hexByte(hex, 0);
    uint8_t g = hexByte(hex, 2);
    uint8_t b = hexByte(hex, 4);
    if (r + g + b > 650 || r + g + b < 35) {
      return leagueFallbackRgb(league, homeRow);
    }
    return {r, g, b};
  }
  return leagueFallbackRgb(league, homeRow);
}

Rgb dimRowRgb(const Rgb& rgb) {
  return {
    static_cast<uint8_t>(max<int>(3, rgb.r / 6)),
    static_cast<uint8_t>(max<int>(3, rgb.g / 6)),
    static_cast<uint8_t>(max<int>(3, rgb.b / 6)),
  };
}

int colorDistanceSq(const Rgb& left, const Rgb& right) {
  int dr = static_cast<int>(left.r) - static_cast<int>(right.r);
  int dg = static_cast<int>(left.g) - static_cast<int>(right.g);
  int db = static_cast<int>(left.b) - static_cast<int>(right.b);
  return dr * dr + dg * dg + db * db;
}

bool dominantChannelMatches(const Rgb& left, const Rgb& right) {
  int leftMax = max<int>(left.r, max<int>(left.g, left.b));
  int leftMin = min<int>(left.r, min<int>(left.g, left.b));
  int rightMax = max<int>(right.r, max<int>(right.g, right.b));
  int rightMin = min<int>(right.r, min<int>(right.g, right.b));
  if (leftMax - leftMin < 60 || rightMax - rightMin < 60) return false;
  int leftChannel = left.r >= left.g && left.r >= left.b ? 0 : (left.g >= left.b ? 1 : 2);
  int rightChannel = right.r >= right.g && right.r >= right.b ? 0 : (right.g >= right.b ? 1 : 2);
  return leftChannel == rightChannel;
}

bool logoCollidesWithTeamColor(const String& league, const String& team, const Rgb& rawRowRgb) {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  if (!sportsLogoDominantColor(league, team, r, g, b)) return false;
  Rgb logoRgb = {r, g, b};
  return colorDistanceSq(logoRgb, rawRowRgb) < 9000 || dominantChannelMatches(logoRgb, rawRowRgb);
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

Rgb teamRowRgb(const String& colorHex, const String& league, const String& team, bool homeRow, bool colorBar) {
  if (!colorBar || team.length() == 0) {
    return {4, 4, 4};
  }
  String color = colorHex.length() > 0 ? colorHex : fallbackTeamColor(league, team);
  Rgb raw = parseTeamRgb(color, league, homeRow);
  if (logoCollidesWithTeamColor(league, team, raw)) {
    Serial.println(String("🏟️ Sports logo/bar contrast fallback: ") + league + "/" + team);
    return {5, 5, 5};
  }
  return dimRowRgb(raw);
}

uint16_t color565(const Rgb& rgb) {
  return getScaledColor(rgb.r, rgb.g, rgb.b);
}

uint16_t headerColor() {
  return getScaledColor(255, 165, 0);
}

void logSportsHeap(const String& label) {
  Serial.printf("🏟️ Sports heap %s free=%u maxAlloc=%u\n",
                label.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
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

void drawLogoOrFallback(const String& league, const String& team, int x, int y, bool drawShadow) {
  uint8_t size = sportsLogoRowSize(league, team, 12);
  int yOffset = (12 - size) / 2;
  logSportsHeap(String("before logo ") + league + "/" + team);
  if (drawShadow) {
    drawSportsLogoSilhouette(league, team, x - 1, y + yOffset, size, getScaledColor(0, 0, 0));
    drawSportsLogoSilhouette(league, team, x + 1, y + yOffset, size, getScaledColor(0, 0, 0));
    drawSportsLogoSilhouette(league, team, x, y + yOffset - 1, size, getScaledColor(0, 0, 0));
    drawSportsLogoSilhouette(league, team, x, y + yOffset + 1, size, getScaledColor(0, 0, 0));
  }
  if (drawSportsLogo(league, team, x, y + yOffset, size)) {
    logSportsHeap(String("after logo ") + league + "/" + team);
    return;
  }
  Serial.println(String("🏟️ Sports logo missing, using text fallback: ") + league + "/" + team);
  drawTinyText(compactText(team, 2), x + 1, y + 4, getScaledColor(255, 255, 255));
  logSportsHeap(String("after logo fallback ") + league + "/" + team);
}

void drawTeamRow(const String& league, const String& team, const String& label, const String& value,
                 const String& colorHex, bool homeRow, bool winner, int y, int xOffset, bool colorBar) {
  Rgb rowRgb = teamRowRgb(colorHex, league, team, homeRow, colorBar);
  matrix.fillRect(0, y, PANEL_WIDTH, 12, color565(rowRgb));
  matrix.drawFastHLine(0, y, PANEL_WIDTH, getScaledColor(16, 16, 16));
  drawLogoOrFallback(league, team, xOffset + 1, y, colorBar);

  matrix.setTextColor(winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255));
  drawTinyText(label, 16 + xOffset, y + 4, winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255));

  drawTinyRightText(value, y + 4, winner ? getScaledColor(255, 255, 0) : getScaledColor(255, 255, 255), 62 + xOffset);
}

void drawTimeoutDots(int count, int x, int y, uint16_t color) {
  if (count < 0) return;
  int visible = min<int>(count, 3);
  for (int i = 0; i < 3; ++i) {
    matrix.drawPixel(x + i * 2, y, i < visible ? color : getScaledColor(24, 24, 24));
  }
}

void drawBaseDiamond(const SportsSnapshot& snapshot, int x, int y) {
  uint16_t occupied = getScaledColor(255, 220, 40);
  uint16_t empty = getScaledColor(54, 54, 54);
  matrix.drawPixel(x + 3, y, snapshot.baseOccupied[1] ? occupied : empty);
  matrix.drawPixel(x + 5, y + 2, snapshot.baseOccupied[0] ? occupied : empty);
  matrix.drawPixel(x + 1, y + 2, snapshot.baseOccupied[2] ? occupied : empty);
}

void drawLiveExtras(const SportsSnapshot& snapshot, int xOffset) {
  if (snapshot.state != "in") return;
  int x = 25 + xOffset;
  if (snapshot.league == "mlb" && snapshot.hasBaseState) {
    drawBaseDiamond(snapshot, x + 4, 2);
    return;
  }
  if (snapshot.league == "nhl" && snapshot.hasPowerPlay) {
    drawTinyText("PP", x + 4, 1, getScaledColor(255, 220, 40));
    return;
  }
  if (snapshot.league == "nfl") {
    if (snapshot.liveExtra.length() > 0) {
      drawTinyText(compactText(snapshot.liveExtra, 4), x, 1, getScaledColor(255, 255, 255));
    }
    if (snapshot.teamTimeouts >= 0) {
      drawTimeoutDots(snapshot.teamTimeouts, x + 18, 6, getScaledColor(255, 255, 255));
    }
    return;
  }
  if (snapshot.league == "nba") {
    if (snapshot.teamFouls >= 0) {
      drawTinyText("F" + String(snapshot.teamFouls), x, 1, getScaledColor(255, 255, 255));
    }
    if (snapshot.teamTimeouts >= 0) {
      drawTimeoutDots(snapshot.teamTimeouts, x + 12, 6, getScaledColor(255, 255, 255));
    }
  }
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

int intFrom(JsonVariant value, int defaultValue = -1) {
  if (value.isNull()) return defaultValue;
  if (value.is<int>() || value.is<long>()) return value.as<int>();
  if (value.is<float>() || value.is<double>()) return static_cast<int>(value.as<float>());
  const char* raw = value.as<const char*>();
  if (raw == nullptr || strlen(raw) == 0) return defaultValue;
  return String(raw).toInt();
}

int statIntFrom(JsonObject competitor, const char* nameA, const char* nameB = nullptr) {
  JsonArray stats = competitor["statistics"].as<JsonArray>();
  if (stats.isNull()) return -1;
  for (JsonVariant v : stats) {
    JsonObject stat = v.as<JsonObject>();
    String name = safeString(stat["name"], "");
    String abbr = safeString(stat["abbreviation"], "");
    name.toLowerCase();
    abbr.toLowerCase();
    String a(nameA);
    a.toLowerCase();
    String b = nameB == nullptr ? "" : String(nameB);
    b.toLowerCase();
    if (name == a || abbr == a || (b.length() > 0 && (name == b || abbr == b))) {
      return intFrom(stat["value"], intFrom(stat["displayValue"], -1));
    }
  }
  return -1;
}

int timeoutFrom(JsonObject competitor) {
  int direct = intFrom(competitor["timeouts"], -1);
  if (direct >= 0) return direct;
  direct = intFrom(competitor["timeoutsRemaining"], -1);
  if (direct >= 0) return direct;
  return statIntFrom(competitor, "timeouts", "to");
}

int foulsFrom(JsonObject competitor) {
  int direct = intFrom(competitor["fouls"], -1);
  if (direct >= 0) return direct;
  direct = intFrom(competitor["teamFouls"], -1);
  if (direct >= 0) return direct;
  return statIntFrom(competitor, "fouls", "f");
}

void parseLiveExtras(JsonObject competition, JsonObject favorite, JsonObject opponent, SportsSnapshot& snapshot) {
  JsonObject situation = competition["situation"].as<JsonObject>();
  if (snapshot.league == "mlb") {
    if (!situation.isNull()) {
      snapshot.baseOccupied[0] = situation["onFirst"] | false;
      snapshot.baseOccupied[1] = situation["onSecond"] | false;
      snapshot.baseOccupied[2] = situation["onThird"] | false;
      snapshot.hasBaseState = situation.containsKey("onFirst") ||
                              situation.containsKey("onSecond") ||
                              situation.containsKey("onThird");
    }
    if (!snapshot.hasBaseState) {
      Serial.println(String("🏟️ ESPN MLB base occupancy unavailable for ") + snapshot.teamAbbr);
    }
  }

  if (snapshot.league == "nhl") {
    if (!situation.isNull()) {
      snapshot.hasPowerPlay = situation["powerPlay"] | false;
      if (!snapshot.hasPowerPlay) {
        snapshot.hasPowerPlay = situation["isPowerPlay"] | false;
      }
    }
    if (!snapshot.hasPowerPlay) {
      Serial.println(String("🏟️ ESPN NHL power play unavailable/inactive for ") + snapshot.teamAbbr);
    }
  }

  if (snapshot.league == "nba") {
    snapshot.teamTimeouts = timeoutFrom(favorite);
    snapshot.opponentTimeouts = timeoutFrom(opponent);
    snapshot.teamFouls = foulsFrom(favorite);
    snapshot.opponentFouls = foulsFrom(opponent);
    if (snapshot.teamTimeouts < 0 && snapshot.opponentTimeouts < 0) {
      Serial.println(String("🏟️ ESPN NBA timeouts unavailable for ") + snapshot.teamAbbr);
    }
    if (snapshot.teamFouls < 0 && snapshot.opponentFouls < 0) {
      Serial.println(String("🏟️ ESPN NBA fouls unavailable for ") + snapshot.teamAbbr);
    }
  }

  if (snapshot.league == "nfl") {
    snapshot.liveExtra = pickString(situation["shortDownDistanceText"], situation["downDistanceText"], "");
    if (snapshot.liveExtra.length() == 0) {
      String down = variantToString(situation["down"], "");
      String distance = variantToString(situation["distance"], "");
      if (down.length() > 0 && distance.length() > 0) {
        snapshot.liveExtra = down + "&" + distance;
      }
    }
    snapshot.teamTimeouts = timeoutFrom(favorite);
    snapshot.opponentTimeouts = timeoutFrom(opponent);
    String possession = normalizedTeam(safeString(situation["possession"], ""));
    if (possession.length() == 0) {
      possession = normalizedTeam(safeString(situation["possessionText"], ""));
    }
    if (possession.length() > 0) {
      snapshot.possessionAbbr = compactText(possession, 3);
      snapshot.hasPossession = true;
    }
    if (snapshot.liveExtra.length() == 0) {
      Serial.println(String("🏟️ ESPN NFL down/distance unavailable for ") + snapshot.teamAbbr);
    }
    if (snapshot.teamTimeouts < 0 && snapshot.opponentTimeouts < 0) {
      Serial.println(String("🏟️ ESPN NFL timeouts unavailable for ") + snapshot.teamAbbr);
    }
  }
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
  parseLiveExtras(competition, favorite, opponent, snapshot);

  Serial.println(String("🏟️ ESPN selected ") + snapshot.opponentAbbr + " vs " + snapshot.teamAbbr);
  Serial.println(String("🏟️ ESPN scores/status ") + snapshot.opponentScore + "-" + snapshot.teamScore + " " + snapshot.status);
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
  String key = league + ":" + teamId;
  unsigned long now = millis();
  auto retryIt = sportsFetchRetryAfter.find(key);
  if (retryIt != sportsFetchRetryAfter.end() && now < retryIt->second) {
    Serial.println(String("🏟️ ESPN backoff active for ") + key);
    return false;
  }

  String url = espnUrl(league, teamId);
  Serial.println(String("🏟️ ESPN request: ") + url);
  logSportsHeap(String("before ESPN fetch ") + key);

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(url);
  int status = http.GET();
  Serial.printf("🏟️ ESPN HTTP status: %d\n", status);
  if (status != HTTP_CODE_OK) {
    http.end();
    sportsFetchRetryAfter[key] = millis() + kFetchFailureBackoffMs;
    logSportsHeap(String("after ESPN HTTP failure ") + key);
    return false;
  }

  StaticJsonDocument<2048> filter;
  filter["events"][0]["date"] = true;
  filter["events"][0]["status"]["type"]["state"] = true;
  filter["events"][0]["status"]["type"]["shortDetail"] = true;
  filter["events"][0]["status"]["type"]["detail"] = true;
  filter["events"][0]["competitions"][0]["situation"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["homeAway"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["score"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["timeouts"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["timeoutsRemaining"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["fouls"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["teamFouls"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["abbreviation"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["shortDisplayName"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["color"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["team"]["alternateColor"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["records"][0]["summary"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["statistics"][0]["name"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["statistics"][0]["abbreviation"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["statistics"][0]["value"] = true;
  filter["events"][0]["competitions"][0]["competitors"][0]["statistics"][0]["displayValue"] = true;

  DynamicJsonDocument doc(24576);
  logSportsHeap(String("before ESPN parse ") + key);
  DeserializationError error = deserializeJson(
    doc,
    *http.getStreamPtr(),
    DeserializationOption::Filter(filter)
  );
  http.end();
  logSportsHeap(String("after ESPN parse ") + key);
  if (error) {
    Serial.println("🏟️ ESPN JSON parse failed: " + String(error.c_str()));
    sportsFetchRetryAfter[key] = millis() + kFetchFailureBackoffMs;
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
    sportsFetchRetryAfter.erase(key);
    Serial.println(String("🏟️ ESPN selected final ") + snapshot.opponentAbbr + " " + snapshot.homeAway + " " + snapshot.teamAbbr);
    Serial.println(String("🏟️ ESPN parsed scores/status ") + snapshot.opponentScore + "-" + snapshot.teamScore + " " + snapshot.status);
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
  Serial.println(String("🏟️ ESPN no matching game for ") + league + "/" + teamId);
  sportsFetchRetryAfter.erase(key);
  return true;
}

bool loadSnapshot(const TeamFavorite& favorite) {
  String key = favoriteKey(favorite);
  SportsSnapshot& snapshot = sportsSnapshots[key];
  if (snapshot.fetchedAt != 0 && millis() - snapshot.fetchedAt < kRefreshIntervalMs) {
    return true;
  }

  logSportsHeap(String("before loadSnapshot ") + key);

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
  snapshot.liveExtra = "";
  snapshot.possessionAbbr = "";
  snapshot.hasBaseState = false;
  snapshot.hasPowerPlay = false;
  snapshot.hasPossession = false;
  snapshot.teamTimeouts = -1;
  snapshot.opponentTimeouts = -1;
  snapshot.teamFouls = -1;
  snapshot.opponentFouls = -1;
  for (int i = 0; i < 3; ++i) snapshot.baseOccupied[i] = false;

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
      snapshot.liveExtra = pickString(doc["liveExtra"], doc["downDistance"], "");
      snapshot.possessionAbbr = normalizedTeam(pickString(doc["possessionAbbr"], doc["possession"], ""));
      snapshot.hasPossession = snapshot.possessionAbbr.length() > 0;
      snapshot.hasBaseState = doc["hasBaseState"] | false;
      snapshot.baseOccupied[0] = doc["onFirst"] | false;
      snapshot.baseOccupied[1] = doc["onSecond"] | false;
      snapshot.baseOccupied[2] = doc["onThird"] | false;
      snapshot.hasPowerPlay = doc["hasPowerPlay"] | false;
      snapshot.teamTimeouts = doc["teamTimeouts"] | -1;
      snapshot.opponentTimeouts = doc["opponentTimeouts"] | -1;
      snapshot.teamFouls = doc["teamFouls"] | -1;
      snapshot.opponentFouls = doc["opponentFouls"] | -1;
      loadedFromCache = snapshot.opponentAbbr.length() > 0 || snapshot.state == "no_game";
      Serial.println(String("🏟️ Sports cache loaded: ") + snapshot.league + "/" + snapshot.teamId + " " + snapshot.status);
    }
  }

  if (!loadedFromCache) {
    Serial.println(String("🏟️ Sports cache miss/no data: ") + snapshot.league + "/" + snapshot.teamId);
    fetchSnapshotFromEspn(favorite, snapshot);
  }

  snapshot.fetchedAt = millis();
  logSportsHeap(String("after loadSnapshot ") + key);
  return true;
}

void pruneSnapshots(const AppConfig& config, const String& leagueId) {
  String prefix = normalizedLeague(leagueId) + ":";
  for (auto it = sportsSnapshots.begin(); it != sportsSnapshots.end();) {
    if (!it->first.startsWith(prefix)) {
      ++it;
      continue;
    }
    bool keep = false;
    for (const TeamFavorite& favorite : config.favorites) {
      if (it->first == favoriteKey(favorite)) {
        keep = true;
        break;
      }
    }
    if (keep) {
      ++it;
    } else {
      Serial.println(String("🏟️ Sports prune snapshot: ") + it->first);
      sportsFetchRetryAfter.erase(it->first);
      it = sportsSnapshots.erase(it);
    }
  }
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
  pruneSnapshots(*config, getAppId());

  if (currentFavoriteIndex >= static_cast<int>(config->favorites.size())) {
    currentFavoriteIndex = 0;
    setNeedsRedraw(true);
  }

  if (millis() - lastRotateAt >= kRotateIntervalMs && config->favorites.size() > 1) {
    logSportsHeap(String("before rotation ") + getAppId());
    currentFavoriteIndex = (currentFavoriteIndex + 1) % config->favorites.size();
    lastRotateAt = millis();
    setNeedsRedraw(true);
    Serial.printf("🏟️ Sports rotation %s index=%d/%d\n",
                  getAppId().c_str(),
                  currentFavoriteIndex,
                  static_cast<int>(config->favorites.size()));
    logSportsHeap(String("after rotation ") + getAppId());
  }

  const TeamFavorite& favorite = config->favorites[currentFavoriteIndex];
  String key = favoriteKey(favorite);
  bool refreshDue = sportsSnapshots.find(key) == sportsSnapshots.end() ||
                    sportsSnapshots[key].fetchedAt == 0 ||
                    millis() - sportsSnapshots[key].fetchedAt >= kRefreshIntervalMs;
  loadSnapshot(favorite);
  if (refreshDue) {
    setNeedsRedraw(true);
  }
}

void SportsApp::redraw(bool force, int xOffset) {
  if (!force && !getNeedsRedraw()) return;

  logSportsHeap(String("before render ") + getAppId());
  matrix.fillScreen(0);
  const AppConfig* config = getAppConfig(getAppId());
  if (config == nullptr || config->favorites.empty()) {
    matrix.setTextSize(1);
    matrix.setTextColor(headerColor());
    matrix.setCursor(1 + xOffset, 0);
    matrix.print(leagueLabel);
    drawSmallText("Set teams", 2 + xOffset, 20);
    setNeedsRedraw(false);
    logSportsHeap(String("after render empty ") + getAppId());
    return;
  }
  bool colorBar = config->sportsDisplayStyle != "noBar";
  Serial.println(String("🏟️ Sports displayStyle ") + getAppId() + "=" + config->sportsDisplayStyle);

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
  drawLiveExtras(snapshot, xOffset);

  drawTeamRow(snapshot.league, awayTeam, awayLabel, awayValue, awayColor, false, awayWinner, 8, xOffset, colorBar);
  drawTeamRow(snapshot.league, homeTeam, homeLabel, homeValue, homeColor, true, homeWinner, 20, xOffset, colorBar);

  setNeedsRedraw(false);
  logSportsHeap(String("after render ") + getAppId());
}

void SportsApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool SportsApp::getNeedsRedraw() {
  return needsRedraw;
}
