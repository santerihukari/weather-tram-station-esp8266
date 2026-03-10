// ---------------- USER CONFIG ----------------
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <TZ.h>

// ---------------- USER CONFIG ----------------
const char* WIFI_SSID = "<your wifi ssd here>";
const char* WIFI_PASS = "<your wifi password here>";
const char* DIGITRANSIT_API_KEY = "<your digitransit api key here>";

// Hervantajarvi A
const char* TARGET_STOP_CODE = "0839";

// Tampere coordinates for weather
const char* WEATHER_LAT = "61.4981";
const char* WEATHER_LON = "23.7608";

// NodeMCU / LoLin ESP8266 I2C pins
const uint8_t PIN_SDA = D2;   // GPIO4
const uint8_t PIN_SCL = D1;   // GPIO5

const unsigned long CLOCK_INTERVAL_MS   = 1000;
const unsigned long WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL;
const unsigned long TRAM_INTERVAL_MS    = 30UL * 1000UL;
// --------------------------------------------

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

enum WeatherIcon {
  ICON_SUNNY,
  ICON_CLOUDY,
  ICON_RAINY
};

struct Departure {
  String route;
  String hhmm;
  int minutes;
  time_t epoch;
  bool valid;
};

String timeStr = "--:--";
String dateStr = "--.--.----";
String tempStr = "--.-C";
WeatherIcon weatherIcon = ICON_CLOUDY;

Departure deps[5];

unsigned long lastClockUpdate   = 0;
unsigned long lastWeatherUpdate = 0;
unsigned long lastTramUpdate    = 0;

String formatEpochToHHMM(time_t epochLocal) {
  struct tm* t = localtime(&epochLocal);
  if (!t) return "--:--";

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(buf);
}

void clearDepartures() {
  for (int i = 0; i < 5; ++i) {
    deps[i].route = "-";
    deps[i].hhmm = "--:--";
    deps[i].minutes = -1;
    deps[i].epoch = 0;
    deps[i].valid = false;
  }
}

void drawBoot(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 16, msg);
  u8g2.sendBuffer();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  drawBoot("Connecting WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
}

void initClock() {
  // Correct Tampere / Helsinki local time with automatic DST handling
  configTime(TZ_Europe_Helsinki, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  while (now < 100000) {
    delay(200);
    now = time(nullptr);
  }
}

void updateClockStrings() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    timeStr = "--:--";
    dateStr = "--.--.----";
    return;
  }

  char tbuf[6];
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  timeStr = String(tbuf);

  char dbuf[11];
  snprintf(dbuf, sizeof(dbuf), "%02d.%02d.%04d",
           timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
  dateStr = String(dbuf);
}

bool httpsGet(const String& url, String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  https.useHTTP10(true);

  if (!https.begin(*client, url)) {
    return false;
  }

  int code = https.GET();
  if (code != 200) {
    https.end();
    return false;
  }

  payload = https.getString();
  https.end();
  return true;
}

bool httpsPostJson(const String& url, const String& body, String& payload, const char* apiKey) {
  if (WiFi.status() != WL_CONNECTED) return false;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  https.useHTTP10(true);

  if (!https.begin(*client, url)) {
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("digitransit-subscription-key", apiKey);

  int code = https.POST(body);
  if (code != 200) {
    https.end();
    return false;
  }

  payload = https.getString();
  https.end();
  return true;
}

WeatherIcon weatherCodeToIcon(int code) {
  if (code == 0) return ICON_SUNNY;

  if (code == 1 || code == 2 || code == 3 || code == 45 || code == 48) {
    return ICON_CLOUDY;
  }

  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code == 95 || code == 96 || code == 99) {
    return ICON_RAINY;
  }

  return ICON_CLOUDY;
}

void updateWeather() {
  String url =
    "https://api.open-meteo.com/v1/forecast?latitude=" + String(WEATHER_LAT) +
    "&longitude=" + String(WEATHER_LON) +
    "&current=temperature_2m,weather_code,is_day" +
    "&timezone=Europe%2FHelsinki";

  String payload;
  if (!httpsGet(url, payload)) {
    tempStr = "--.-C";
    weatherIcon = ICON_CLOUDY;
    return;
  }

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    tempStr = "--.-C";
    weatherIcon = ICON_CLOUDY;
    return;
  }

  float temp = doc["current"]["temperature_2m"] | NAN;
  int weatherCode = doc["current"]["weather_code"] | 3;

  if (isnan(temp)) {
    tempStr = "--.-C";
  } else {
    tempStr = String(temp, 1) + "C";
  }

  weatherIcon = weatherCodeToIcon(weatherCode);
}

void insertDeparture(const String& route, time_t depEpoch, time_t nowEpoch) {
  if (depEpoch < nowEpoch - 60) return;

  Departure cand;
  cand.route = route;
  cand.hhmm = formatEpochToHHMM(depEpoch);

  long diffSec = (long)(depEpoch - nowEpoch);
  if (diffSec < 0) diffSec = 0;
  cand.minutes = (diffSec + 59) / 60;
  cand.epoch = depEpoch;
  cand.valid = true;

  for (int i = 0; i < 5; ++i) {
    if (!deps[i].valid || cand.epoch < deps[i].epoch) {
      for (int j = 4; j > i; --j) {
        deps[j] = deps[j - 1];
      }
      deps[i] = cand;
      return;
    }
  }
}

void updateNextTrams() {
  clearDepartures();

  const String endpoint = "https://api.digitransit.fi/routing/v2/waltti/gtfs/v1";

  const String gql =
    "{\"query\":\"{ stops(name:\\\"Hervantaj\\u00e4rvi\\\") { "
    "name code platformCode "
    "stoptimesWithoutPatterns(numberOfDepartures:12) { "
    "serviceDay scheduledDeparture realtimeDeparture realtime "
    "trip { route { shortName mode } } "
    "} } }\"}";

  String payload;
  if (!httpsPostJson(endpoint, gql, payload, DIGITRANSIT_API_KEY)) {
    return;
  }

  DynamicJsonDocument doc(12288);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return;
  }

  JsonArray stops = doc["data"]["stops"].as<JsonArray>();
  if (stops.isNull()) return;

  time_t nowEpoch = time(nullptr);

  for (JsonObject stop : stops) {
    const char* code = stop["code"] | "";
    if (String(code) != TARGET_STOP_CODE) continue;

    JsonArray times = stop["stoptimesWithoutPatterns"].as<JsonArray>();
    if (times.isNull()) continue;

    for (JsonObject row : times) {
      const char* mode = row["trip"]["route"]["mode"] | "";
      if (String(mode) != "TRAM") continue;

      const char* shortName = row["trip"]["route"]["shortName"] | "?";
      long serviceDay = row["serviceDay"] | 0;
      bool realtime = row["realtime"] | false;
      long depSec = realtime ? (row["realtimeDeparture"] | 0)
                             : (row["scheduledDeparture"] | 0);

      time_t depEpoch = (time_t)(serviceDay + depSec);
      insertDeparture(String(shortName), depEpoch, nowEpoch);
    }

    break; // exact stop code found, no need to inspect further stops
  }
}

void drawSunIcon(int x, int y) {
  u8g2.drawCircle(x, y, 7);
  u8g2.drawLine(x, y - 11, x, y - 15);
  u8g2.drawLine(x, y + 11, x, y + 15);
  u8g2.drawLine(x - 11, y, x - 15, y);
  u8g2.drawLine(x + 11, y, x + 15, y);
  u8g2.drawLine(x - 8, y - 8, x - 11, y - 11);
  u8g2.drawLine(x + 8, y - 8, x + 11, y - 11);
  u8g2.drawLine(x - 8, y + 8, x - 11, y + 11);
  u8g2.drawLine(x + 8, y + 8, x + 11, y + 11);
}

void drawCloudIcon(int x, int y) {
  u8g2.drawDisc(x - 8, y, 5);
  u8g2.drawDisc(x, y - 3, 7);
  u8g2.drawDisc(x + 9, y, 5);
  u8g2.drawBox(x - 13, y, 27, 7);
}

void drawRainIcon(int x, int y) {
  drawCloudIcon(x, y - 3);
  u8g2.drawLine(x - 8, y + 9, x - 10, y + 14);
  u8g2.drawLine(x,     y + 9, x - 2,  y + 14);
  u8g2.drawLine(x + 8, y + 9, x + 6,  y + 14);
}

void drawWeatherIcon(int x, int y) {
  if (weatherIcon == ICON_SUNNY) {
    drawSunIcon(x, y);
  } else if (weatherIcon == ICON_RAINY) {
    drawRainIcon(x, y);
  } else {
    drawCloudIcon(x, y);
  }
}

void drawTramIcon(int x, int y) {
  // simple 14x12 tram icon
  u8g2.drawFrame(x, y, 14, 10);
  u8g2.drawBox(x + 2, y + 2, 3, 3);
  u8g2.drawBox(x + 6, y + 2, 3, 3);
  u8g2.drawBox(x + 10, y + 2, 2, 3);
  u8g2.drawDisc(x + 3, y + 11, 1);
  u8g2.drawDisc(x + 11, y + 11, 1);
  u8g2.drawLine(x + 5, y - 2, x + 7, y);
  u8g2.drawLine(x + 9, y - 2, x + 7, y);
}

void drawScreen() {
  u8g2.clearBuffer();

  const int dividerX = 54;

  // Divider
  u8g2.drawVLine(dividerX, 0, 64);

  // Left side: time, date, weather
  u8g2.setFont(u8g2_font_logisoso16_tf);
  u8g2.drawStr(0, 18, timeStr.c_str());

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 29, dateStr.c_str());

  drawWeatherIcon(16, 48);

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(30, 51, tempStr.c_str());

  // Right side: header + next 5 trams
  drawTramIcon(dividerX + 3, 2);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(dividerX + 20, 10, "Next trams");

  for (int i = 0; i < 5; ++i) {
    int y = 20 + i * 9;

    if (deps[i].valid) {
      char line[24];
      snprintf(line, sizeof(line), "%s %s +%d",
               deps[i].route.c_str(),
               deps[i].hhmm.c_str(),
               deps[i].minutes);
      u8g2.drawStr(dividerX + 3, y, line);
    } else {
      u8g2.drawStr(dividerX + 3, y, "- --:--");
    }
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  u8g2.begin();
  clearDepartures();

  drawBoot("Booting...");
  connectWiFi();
  initClock();

  updateClockStrings();
  updateWeather();
  updateNextTrams();
  drawScreen();

  lastClockUpdate = millis();
  lastWeatherUpdate = millis();
  lastTramUpdate = millis();
}

void loop() {
  unsigned long nowMs = millis();

  if (nowMs - lastClockUpdate >= CLOCK_INTERVAL_MS) {
    updateClockStrings();
    lastClockUpdate = nowMs;
  }

  if (nowMs - lastWeatherUpdate >= WEATHER_INTERVAL_MS) {
    updateWeather();
    lastWeatherUpdate = nowMs;
  }

  if (nowMs - lastTramUpdate >= TRAM_INTERVAL_MS) {
    updateNextTrams();
    lastTramUpdate = nowMs;
  }

  drawScreen();
  delay(50);
}
