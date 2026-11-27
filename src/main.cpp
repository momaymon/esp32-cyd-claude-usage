// ESP32-2432S028 (CYD) - Claude usage monitor
// The PC (Claude Code status line script) POSTs usage JSON over WiFi; this board shows
// "Current session" (5-hour) and weekly limit usage with a reset countdown.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "strings.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and fill in your WiFi + token"
#endif

TFT_eSPI tft;
WebServer server(HTTP_PORT);

// Screen layout (landscape 320x240)
static const int W = 320;
static const int H = 240;
static const int PAD = 10;
static const uint16_t BG = TFT_BLACK;
static const uint16_t CARD = 0x18E3;     // dark grey
static const uint16_t ACCENT = 0xDB2A;   // warm orange
static const uint16_t MUTED = TFT_SILVER;

struct Limit {
  float pct = -1;       // % used, -1 = unknown
  uint32_t resetsAt = 0;  // unix seconds, 0 = unknown
};

struct Usage {
  bool has = false;
  Limit session;
  Limit weekly;
  char model[32] = "";
  uint32_t receivedMs = 0;
  int32_t epochOffset = 0;  // unix time - millis()/1000, from the PC clock
} usage;

static bool dirty = true;       // full redraw needed
static bool backlightOn = true;

// ---------- Helpers ----------

uint32_t nowEpoch() { return usage.epochOffset + millis() / 1000; }

uint16_t pctColor(float pct) {
  if (pct < 0) return MUTED;
  if (pct >= PCT_DANGER) return TFT_RED;
  if (pct >= PCT_WARN) return TFT_YELLOW;
  return TFT_GREEN;
}

void setLed(float pct) {
  bool r = false, g = false;
  if (pct >= PCT_DANGER) r = true;
  else if (pct >= PCT_WARN) r = g = true;
  else if (pct >= 0) g = true;
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, HIGH);
}

// "2h 13m", "3d 4h", "45s"
void formatDuration(int32_t s, char *out, size_t n) {
  if (s < 0) s = 0;
  int d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d > 0) snprintf(out, n, "%dd %dh", d, h);
  else if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
  else if (m > 0) snprintf(out, n, "%dm %02ds", m, s % 60);
  else snprintf(out, n, "%ds", s);
}

void formatReset(const Limit &l, char *out, size_t n) {
  if (!usage.has || l.resetsAt == 0) {
    snprintf(out, n, "%s", STR_NO_DATA);
    return;
  }
  int32_t left = (int32_t)(l.resetsAt - nowEpoch());
  if (left <= 0) {
    snprintf(out, n, "%s", STR_RESET_DONE);
    return;
  }
  char d[24];
  formatDuration(left, d, sizeof(d));
  snprintf(out, n, "%s %s", STR_RESETS_IN, d);
}

void drawBar(int x, int y, int w, int h, float pct) {
  tft.fillRoundRect(x, y, w, h, h / 2, TFT_DARKGREY);
  if (pct <= 0) return;
  int fw = (int)(w * (pct > 100 ? 100 : pct) / 100.0f);
  if (fw < h) fw = h;  // keep the rounded end visible
  tft.fillRoundRect(x, y, fw, h, h / 2, pctColor(pct));
}

// ---------- Screen ----------

void drawHeader() {
  tft.fillRect(0, 0, W, 28, ACCENT);
  tft.setTextColor(TFT_WHITE, ACCENT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(STR_TITLE, PAD, 14, 4);
  tft.setTextDatum(MR_DATUM);
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("offline");
  tft.drawString(ip, W - PAD, 14, 2);
}

// Session card: big number + bar. y 34..140
void drawSessionCard() {
  const int y = 34, h = 108;
  tft.fillRoundRect(PAD - 4, y, W - 2 * (PAD - 4), h, 8, CARD);
  tft.setTextColor(MUTED, CARD);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(STR_SESSION, PAD + 4, y + 8, 2);

  float pct = usage.has ? usage.session.pct : -1;
  uint16_t c = pctColor(pct);
  tft.setTextColor(c, CARD);
  tft.setTextDatum(BL_DATUM);
  int x = PAD + 4;
  if (pct >= 0) {
    char num[8];
    snprintf(num, sizeof(num), "%d", (int)(pct + 0.5f));
    x += tft.drawString(num, x, y + 80, 7);
    tft.drawString("%", x + 4, y + 78, 4);
  } else {
    tft.drawString(STR_NO_DATA, x, y + 78, 6);
  }
  drawBar(PAD + 4, y + 88, W - 2 * (PAD + 4), 14, pct);
}

// Weekly card: % + bar + countdown. y 148..210
void drawWeeklyCard() {
  const int y = 148, h = 62;
  tft.fillRoundRect(PAD - 4, y, W - 2 * (PAD - 4), h, 8, CARD);
  tft.setTextColor(MUTED, CARD);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(STR_WEEKLY, PAD + 4, y + 6, 2);

  float pct = usage.has ? usage.weekly.pct : -1;
  char buf[12];
  if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
  else snprintf(buf, sizeof(buf), "%s", STR_NO_DATA);
  tft.setTextColor(pctColor(pct), CARD);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(buf, W - PAD - 4, y + 4, 4);
  drawBar(PAD + 4, y + 30, W - 2 * (PAD + 4), 10, pct);
}

// Parts that change every second: countdowns + footer
void drawLive() {
  char buf[48];

  // session countdown, top-right of the session card
  formatReset(usage.session, buf, sizeof(buf));
  tft.setTextColor(TFT_WHITE, CARD);
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(170);
  tft.drawString(buf, W - PAD - 4, 34 + 8, 2);

  // weekly countdown, under the weekly bar
  formatReset(usage.weekly, buf, sizeof(buf));
  tft.setTextColor(MUTED, CARD);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(W - 2 * (PAD + 4));
  tft.drawString(buf, PAD + 4, 148 + 44, 2);

  // footer: model + last update age (padding overwrites old text, no flicker)
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(MUTED, BG);
  if (!usage.has) {
    tft.setTextPadding(W - 2 * PAD);
    snprintf(buf, sizeof(buf), "%s  (%s.local)", STR_WAITING, MDNS_NAME);
    tft.drawString(buf, PAD, 227, 2);
    tft.setTextPadding(0);
    return;
  }
  tft.setTextPadding(120);
  tft.drawString(usage.model, PAD, 227, 2);
  tft.setTextPadding(180);

  uint32_t age = (millis() - usage.receivedMs) / 1000;
  bool stale = age > STALE_AFTER_S;
  char d[24];
  formatDuration(age, d, sizeof(d));
  snprintf(buf, sizeof(buf), "%s%s %s %s", stale ? STR_STALE " - " : "", STR_UPDATED, d, STR_AGO);
  tft.setTextColor(stale ? TFT_ORANGE : MUTED, BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, W - PAD, 227, 2);
  tft.setTextPadding(0);
}

void drawAll() {
  tft.fillScreen(BG);
  drawHeader();
  drawSessionCard();
  drawWeeklyCard();
  drawLive();
  setLed(usage.has ? usage.session.pct : -1);
}

void showMessage(const char *msg) {
  tft.fillScreen(BG);
  drawHeader();
  tft.setTextColor(TFT_WHITE, BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(msg, W / 2, H / 2, 4);
}

// ---------- HTTP ----------

void readLimit(JsonVariantConst v, Limit &out) {
  out.pct = v["pct"].is<float>() ? v["pct"].as<float>() : -1;
  out.resetsAt = v["resets_at"].is<uint32_t>() ? v["resets_at"].as<uint32_t>() : 0;
}

void handleUsage() {
  if (server.header("X-Token") != API_TOKEN) {
    server.send(401, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  readLimit(doc["session"], usage.session);
  readLimit(doc["weekly"], usage.weekly);
  strlcpy(usage.model, doc["model"] | "", sizeof(usage.model));
  if (doc["now"].is<uint32_t>()) usage.epochOffset = doc["now"].as<uint32_t>() - millis() / 1000;
  usage.receivedMs = millis();
  usage.has = true;
  dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleState() {
  JsonDocument doc;
  doc["has_data"] = usage.has;
  doc["session"]["pct"] = usage.session.pct;
  doc["session"]["resets_at"] = usage.session.resetsAt;
  doc["weekly"]["pct"] = usage.weekly.pct;
  doc["weekly"]["resets_at"] = usage.weekly.resetsAt;
  doc["model"] = usage.model;
  doc["age_s"] = usage.has ? (millis() - usage.receivedMs) / 1000 : -1;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLed(-1);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  showMessage(STR_WIFI_CONNECTING);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    showMessage(STR_WIFI_FAIL);
    delay(5000);
    ESP.restart();
  }
  Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", HTTP_PORT);

  const char *headers[] = {"X-Token"};
  server.collectHeaders(headers, 1);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/", HTTP_GET, handleState);
  server.begin();
  dirty = true;
}

void loop() {
  server.handleClient();

  // BOOT button toggles the backlight (debounced on release)
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn == LOW && btn == HIGH) {
    backlightOn = !backlightOn;
    digitalWrite(TFT_BL, backlightOn ? HIGH : LOW);
    delay(30);
  }
  lastBtn = btn;

  static uint32_t lastDraw = 0;
  static bool wasOnline = true;
  bool online = WiFi.isConnected();
  if (online != wasOnline) dirty = true;  // refresh IP in the header
  wasOnline = online;

  if (dirty) {
    dirty = false;
    drawAll();
    lastDraw = millis();
  } else if (millis() - lastDraw >= REDRAW_INTERVAL_MS) {
    drawLive();
    lastDraw = millis();
  }
}
