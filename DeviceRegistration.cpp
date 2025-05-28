#include "DeviceRegistration.h"
#include "DisplayHelpers.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "secrets.h"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String units = "metric";
String deviceID = "";
int timeFormatPreference = 0;
int lastTimeFormat = timeFormatPreference;
float storedLat = 0.0;
float storedLon = 0.0;

String getSanitizedMac() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac;
}

void initializeWiFi() {
  wm.setConnectTimeout(WIFI_TIMEOUT);
  setupCustomWiFiManager(wm);

  Serial.println("Attempting connection using saved credentials...");
  showConnectingToWiFi();

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected successfully.");
    showWifiInfo();
    delay(2000);
    
    // ✅ Always set deviceID before returning!
    deviceID = getSanitizedMac();
    Serial.println("📟 deviceID set to: " + deviceID);
    return;
  }

  Serial.println("⚠️ Saved WiFi failed. Launching setup portal...");
  showWifiNotSetNotice();
  delay(2000);
  showJoinInstructions();
  delay(3000);

  bool connected = wm.startConfigPortal("NovaFrame-Setup");
  if (connected) {
    Serial.println("✅ Connected via portal.");
    matrix.fillScreen(0);
    showCenteredText("Connected!", 10, matrix.color565(0, 255, 0));
    matrix.show();
    delay(1500);
    showWifiInfo();
    delay(2000);
  } else {
    Serial.println("❌ Portal failed. Resetting credentials.");
    wm.resetSettings();
    matrix.fillScreen(0);
    showCenteredText("WiFi Fail", 10, matrix.color565(255, 0, 0));
    matrix.show();
    delay(2000);
  }

  // ✅ Always set deviceID no matter what
  deviceID = getSanitizedMac();
  Serial.println("📟 deviceID set to: " + deviceID);
}

void initializeFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_HOST;
  auth.user.email = FIREBASE_EMAIL;
  auth.user.password = FIREBASE_PASSWORD;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("Waiting for Firebase token...");
  while (!Firebase.ready()) delay(100);
}

void fetchAndStoreTimezone(float lat, float lon) {
  HTTPClient http;
  String url = "https://api.ipgeolocation.io/timezone?apiKey=" + String(IP_GEO_LOCATION_API_KEY) +
               "&lat=" + String(lat, 4) + "&long=" + String(lon, 4);
  http.begin(url);
  Serial.println("Time zone query: " + url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    String timezone = doc["timezone"];
    Serial.println("✅ Timezone detected: " + timezone);
    String path = "/novaFrame/devices/" + deviceID + "/settings/timezone";
    Firebase.RTDB.setString(&fbdo, path.c_str(), timezone);
  } else {
    Serial.println("❌ Failed to fetch timezone: " + http.errorToString(httpCode));
  }
  http.end();
}

void registerDeviceInFirebase() {
  Serial.println("📝 Registering device in Firebase...");

  deviceID = auth.token.uid.c_str();
  String devicePath = "/novaFrame/devices/" + deviceID;
  String settingsPath = devicePath + "/settings";
  String appSeqPath = settingsPath + "/appSequence";
  String appsPath = devicePath + "/apps";

  // ✅ Create settings node if missing
  if (!Firebase.RTDB.getJSON(&fbdo, settingsPath.c_str())) {
    Serial.println("📁 Settings node missing, creating empty shell.");
    FirebaseJson patch;
    patch.set("placeholder", true);  // harmless dummy key
    if (!Firebase.RTDB.updateNode(&fbdo, settingsPath.c_str(), &patch)) {
      Serial.printf("❌ Failed to create settings node: %s\n", fbdo.errorReason().c_str());
    } else {
      Serial.println("✅ Created empty settings node.");
    }
  }

  // 📦 Ensure at least one app is defined in the apps block
  FirebaseJson defaultAppConfig;
  defaultAppConfig.set("duration", 10000);
  defaultAppConfig.set("enabled", true);

  String clockAppPath = appsPath + "/clock";
  if (!Firebase.RTDB.getJSON(&fbdo, clockAppPath.c_str())) {
    Serial.println("⚙️ clock app not defined — auto-creating in /apps.");
    if (!Firebase.RTDB.setJSON(&fbdo, clockAppPath.c_str(), &defaultAppConfig)) {
      Serial.printf("❌ Failed to create clock app: %s\n", fbdo.errorReason().c_str());
    } else {
      Serial.println("✅ clock app added to Firebase.");
    }
  } else {
    Serial.println("📦 clock app already defined in Firebase.");
  }

  // 🔁 Validate or create appSequence
  bool hasValidSequence = false;
  if (Firebase.RTDB.getArray(&fbdo, appSeqPath.c_str())) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, fbdo.payload().c_str());
    if (!err && doc.is<JsonArray>() && doc.size() > 0) {
      hasValidSequence = true;
      Serial.printf("✅ appSequence is a valid array with %d items.\n", doc.size());
    } else {
      Serial.println("❌ appSequence deserialization failed or is empty.");
    }
  } else {
    Serial.println("⚠️ Failed to fetch appSequence.");
    Serial.printf("❌ Error: %s\n", fbdo.errorReason().c_str());
  }

  if (!hasValidSequence) {
    Serial.println("⚠️ appSequence missing or invalid. Writing default...");

    FirebaseJsonArray defaultSeq;
    defaultSeq.add("clock");

    FirebaseJson json;
    json.set("appSequence", defaultSeq);

    // Write the full settings object, merging with what's already there
    if (!Firebase.RTDB.updateNode(&fbdo, settingsPath.c_str(), &json)) {
      Serial.printf("❌ Failed to write default appSequence: %s\n", fbdo.errorReason().c_str());
    } else {
      Serial.println("✅ Default appSequence written.");
    }
  }

  // 🌡 Optional settings
  String unitsPath = settingsPath + "/units";
  String timeFmtPath = settingsPath + "/timeFormat";
  String brightnessPath = settingsPath + "/brightness";

  if (Firebase.RTDB.getString(&fbdo, unitsPath.c_str())) {
    units = fbdo.stringData();
    Serial.println("📏 Units loaded: " + units);
  }

  if (Firebase.RTDB.getInt(&fbdo, timeFmtPath.c_str())) {
    int format = fbdo.intData();
    timeFormatPreference = constrain(format, 0, 2);
    Serial.printf("🕒 Time format loaded: %d\n", timeFormatPreference);
  } else {
    timeFormatPreference = 1; // default to 12hr with AM/PM
    if (Firebase.RTDB.setInt(&fbdo, timeFmtPath.c_str(), timeFormatPreference)) {
      Serial.println("🕒 Default time format set to 12hr (1)");
    } else {
      Serial.printf("❌ Failed to set default time format: %s\n", fbdo.errorReason().c_str());
    }
  }

  int brightness = 7;
  if (Firebase.RTDB.getInt(&fbdo, brightnessPath.c_str())) {
    brightness = fbdo.intData();
    Serial.printf("💡 Brightness loaded: %d\n", brightness);
  } else {
    Firebase.RTDB.setInt(&fbdo, brightnessPath.c_str(), brightness);
    Serial.println("⚠️ Brightness fallback set to 7");
  }

  brightnessLevel = constrain(brightness, 1, 10);

  // 🌍 GeoIP
  HTTPClient geoHttp;
  geoHttp.begin("http://ip-api.com/json");
  int code = geoHttp.GET();
  if (code == 200) {
    String payload = geoHttp.getString();
    DynamicJsonDocument geoDoc(1024);
    deserializeJson(geoDoc, payload);
    storedLat = geoDoc["lat"];
    storedLon = geoDoc["lon"];
    String city = geoDoc["city"];
    String region = geoDoc["region"];

    Firebase.RTDB.setString(&fbdo, (settingsPath + "/weatherLocation").c_str(), city + "," + region);
    Firebase.RTDB.setFloat(&fbdo, (settingsPath + "/lat").c_str(), storedLat);
    Firebase.RTDB.setFloat(&fbdo, (settingsPath + "/lon").c_str(), storedLon);
    fetchAndStoreTimezone(storedLat, storedLon);
  } else {
    Serial.println("❌ GeoIP failed: " + geoHttp.errorToString(code));
  }
  geoHttp.end();

  Serial.println("📍 Settings initialized or updated.");
}