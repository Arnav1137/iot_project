/*
  Pala Lite – AI voice notes recorder for ESP32 (30-pin DevKit) + SSD1306 OLED
  ---------------------------------------------------------------------------
  Upper button (REC)  : hold to record a thought. Release -> AI turns it into a
                        task / appointment / memory. While recording, tap the
                        lower button to drop a marker.
  Lower button (ASK)  : hold to ask a question about your notes -> spoken answer
                        1 click  -> spoken briefing for today
                        2 clicks -> spoken briefing for this week
  No Wi-Fi?  Notes are saved to SD card / flash and uploaded automatically later.

  Board  : "ESP32 Dev Module"  (esp32 by Espressif, core 3.x)
  Partition Scheme: "No OTA (2MB APP/2MB SPIFFS)"  -> about 2 MB of flash for
           recordings when there is no SD card (~60 s of audio waiting to sync).
  Libs   : Adafruit SSD1306, Adafruit GFX, ArduinoJson (v7)
  Edit config.h first.
*/

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <vector>
#include <time.h>
#include "config.h"

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiMulti wifiMulti;
fs::FS* store = nullptr;
bool usingSD = false;

// Home-screen data fetched from the server
int    openTasks = -1;
String topTask, nextAppt, nextWhen;
int    queueCount = 0;
uint32_t lastHomeFetch = 0, lastWifiCheck = 0, lastDraw = 0;
bool   everConnected = false;

// =====================================================================
//  Small helpers
// =====================================================================
String fit(const String& s, int maxLen) {          // ASCII only + truncate for OLED
  String o;
  for (size_t i = 0; i < s.length() && (int)o.length() < maxLen; i++) {
    char c = s[i];
    if (c >= 32 && c < 127) o += c;
  }
  return o;
}

String serverUrl(const String& path) {
  return String("http://") + SERVER_HOST + ":" + SERVER_PORT + path;
}

int batteryPercent() {
  float mv = analogReadMilliVolts(BAT_PIN) * BAT_DIVIDER;
  if (mv < 2500) return -1;                        // not wired / on USB
  int p = (int)((mv - 3300.0f) * 100.0f / (4200.0f - 3300.0f));
  return constrain(p, 0, 100);
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

void writeWavHeader(File& f, uint32_t rate, uint32_t dataLen) {
  uint8_t h[44];
  memcpy(h, "RIFF", 4);      put32(h + 4, 36 + dataLen);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);         put16(h + 20, 1);      put16(h + 22, 1);
  put32(h + 24, rate);       put32(h + 28, rate * 2);
  put16(h + 32, 2);          put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, dataLen);
  f.write(h, 44);
}

// =====================================================================
//  Display
// =====================================================================
void drawHeader() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  struct tm t;
  if (getLocalTime(&t, 0)) {
    char b[16]; strftime(b, sizeof b, "%H:%M %d/%m", &t); oled.print(b);
  } else oled.print("--:--");
  oled.setCursor(72, 0);
  oled.print(WiFi.status() == WL_CONNECTED ? "W" : "x");
  if (queueCount > 0) { oled.print(" Q"); oled.print(queueCount); }
  int b = batteryPercent();
  oled.setCursor(104, 0);
  if (b >= 0) { oled.print(b); oled.print("%"); } else oled.print("USB");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void showMessage(const String& a, const String& b = "", const String& c = "", const String& d = "") {
  oled.clearDisplay();
  drawHeader();
  oled.setCursor(0, 16); oled.print(fit(a, 21));
  oled.setCursor(0, 28); oled.print(fit(b, 21));
  oled.setCursor(0, 40); oled.print(fit(c, 21));
  oled.setCursor(0, 52); oled.print(fit(d, 21));
  oled.display();
}

void drawHome() {
  oled.clearDisplay();
  drawHeader();
  oled.setCursor(0, 14);
  if (openTasks < 0) {
    oled.print(WiFi.status() == WL_CONNECTED ? "Server not reachable" : "Offline - recording OK");
  } else {
    oled.print("Open tasks: "); oled.print(openTasks);
    oled.setCursor(0, 24); oled.print("> "); oled.print(fit(topTask, 19));
    oled.setCursor(0, 36); oled.print("Next appointment:");
    oled.setCursor(0, 46); oled.print(fit(nextAppt.length() ? nextAppt : String("none"), 21));
    oled.setCursor(0, 56); oled.print(fit(nextWhen, 21));
  }
  oled.display();
}

void drawRecording(const char* label, uint32_t seconds, int peak, int markers) {
  oled.clearDisplay();
  drawHeader();
  oled.setTextSize(2);
  oled.setCursor(0, 16);
  oled.print(label);
  oled.setCursor(0, 34);
  char b[12]; snprintf(b, sizeof b, "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  oled.print(b);
  oled.setTextSize(1);
  if (markers > 0) { oled.setCursor(80, 38); oled.print("M:"); oled.print(markers); }
  int w = map(constrain(peak, 0, 20000), 0, 20000, 0, 127);   // level meter
  oled.drawRect(0, 56, 128, 8, SSD1306_WHITE);
  oled.fillRect(0, 56, w, 8, SSD1306_WHITE);
  oled.display();
}

// =====================================================================
//  Storage: microSD if present, otherwise internal flash (LittleFS)
// =====================================================================
void initStorage() {
  usingSD = false;
#if USE_SD_CARD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS)) { store = &SD; usingSD = true; }
#endif
  if (!usingSD) {
    if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed - check Partition Scheme");
    store = &LittleFS;
  }
  if (!store->exists("/queue")) store->mkdir("/queue");
}

std::vector<String> listQueue() {
  std::vector<String> out;
  File dir = store->open("/queue");
  if (!dir) return out;
  File e;
  while ((e = dir.openNextFile())) {
    String p = e.path();
    if (!e.isDirectory() && p.endsWith(".wav")) out.push_back(p);
    e.close();
  }
  dir.close();
  return out;
}

void refreshQueueCount() { queueCount = listQueue().size(); }

uint64_t freeBytes() {
  if (usingSD) return SD.totalBytes() - SD.usedBytes();
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}

// =====================================================================
//  Microphone
// =====================================================================
#if MIC_TYPE == MIC_I2S
I2SClass micI2S;
float hpX = 0, hpY = 0;

void micBegin() {
  micI2S.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  // INMP441 sends 24-bit samples inside 32-bit slots. Read stereo and keep
  // whichever channel actually has the mic (works whatever the L/R pin is).
  if (!micI2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Mic I2S init failed");
  }
  hpX = hpY = 0;
}
void micEnd() { micI2S.end(); }

size_t micRead(int16_t* out, size_t maxSamples) {
  static int32_t raw[512];                       // 256 stereo frames
  size_t frames = min(maxSamples, (size_t)256);
  size_t got = micI2S.readBytes((char*)raw, frames * 8) / 8;
  int64_t sl = 0, sr = 0;
  for (size_t i = 0; i < got; i++) { sl += abs(raw[2 * i] >> 16); sr += abs(raw[2 * i + 1] >> 16); }
  int ch = (sl >= sr) ? 0 : 1;
  for (size_t i = 0; i < got; i++) {
    float x = (float)(raw[2 * i + ch] >> 14);    // 24-bit -> 16-bit with 4x gain
    float y = x - hpX + 0.995f * hpY;            // remove DC offset
    hpX = x; hpY = y;
    out[i] = (int16_t)constrain((int)y, -32768, 32767);
  }
  return got;
}

#else   // ---------------- analog mic (MAX9814) ----------------
static const uint32_t RING = 8192;               // power of two
static int16_t ring[RING];
static volatile uint32_t wIdx = 0;
static uint32_t rIdx = 0;
static volatile bool sampling = false;

void samplerTask(void*) {
  const int64_t period = 1000000LL / SAMPLE_RATE;
  int64_t next = esp_timer_get_time();
  float dc = 2048;
  while (sampling) {
    next += period;
    while (esp_timer_get_time() < next) { }
    int v = analogRead(MIC_ANALOG_PIN);
    dc += (v - dc) * 0.001f;
    int s = (int)((v - dc) * 16.0f * MIC_ANALOG_GAIN);
    ring[wIdx & (RING - 1)] = (int16_t)constrain(s, -32768, 32767);
    wIdx++;
  }
  vTaskDelete(NULL);
}

void micBegin() {
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_ANALOG_PIN, ADC_11db);
  wIdx = rIdx = 0;
  sampling = true;
  disableCore0WDT();
  xTaskCreatePinnedToCore(samplerTask, "mic", 4096, NULL, 5, NULL, 0);
}
void micEnd() { sampling = false; delay(20); enableCore0WDT(); }

size_t micRead(int16_t* out, size_t maxSamples) {
  while (wIdx == rIdx) delay(1);
  uint32_t w = wIdx;
  if (w - rIdx > RING) rIdx = w - RING;          // overrun: skip ahead
  size_t n = 0;
  while (rIdx != w && n < maxSamples) out[n++] = ring[(rIdx++) & (RING - 1)];
  return n;
}
#endif

// Record while `pin` is held. Writes a WAV file. Returns false if too short.
bool recordWhileHeld(int pin, const String& path, const char* label, bool allowMarkers) {
  File f = store->open(path, FILE_WRITE);
  if (!f) { showMessage("Storage error", usingSD ? "SD card" : "Flash"); delay(1500); return false; }
  writeWavHeader(f, SAMPLE_RATE, 0);

  // Never record more than the free storage space (important without an SD card)
  uint32_t maxBytes = (uint32_t)SAMPLE_RATE * 2 * MAX_RECORD_SEC;
  uint64_t freeB = freeBytes();
  freeB = (freeB > 24576) ? freeB - 24576 : 0;          // keep 24 KB spare
  if (freeB < maxBytes) maxBytes = (uint32_t)freeB;
  if (maxBytes < (uint32_t)SAMPLE_RATE * 2) {           // less than 1 s left
    f.close(); store->remove(path);
    showMessage("Storage full!", "Connect to Wi-Fi", "to sync old notes");
    delay(2000); return false;
  }

  micBegin();
  int16_t buf[256];
  uint32_t bytes = 0;
  uint32_t lastUi = 0, releasedAt = 0;
  int peak = 0, markerCount = 0;
  bool askWasDown = (digitalRead(BTN_ASK) == LOW);
  String markers;

  while (bytes < maxBytes) {
    if (digitalRead(pin) == HIGH) {              // released (with 150 ms debounce)
      if (!releasedAt) releasedAt = millis();
      if (millis() - releasedAt > 150) break;
    } else releasedAt = 0;

    size_t n = micRead(buf, 256);
    if (n) { f.write((uint8_t*)buf, n * 2); bytes += n * 2; }
    for (size_t i = 0; i < n; i++) peak = max(peak, abs((int)buf[i]));

    if (allowMarkers) {                          // tap ASK while recording = marker
      bool askDown = (digitalRead(BTN_ASK) == LOW);
      if (askDown && !askWasDown) {
        if (markers.length()) markers += ",";
        markers += String(bytes / 2.0f / SAMPLE_RATE, 1);
        markerCount++;
      }
      askWasDown = askDown;
    }
    if (millis() - lastUi > 400) {
      drawRecording(label, bytes / 2 / SAMPLE_RATE, peak, markerCount);
      peak = 0; lastUi = millis();
    }
  }
  micEnd();
  f.close();

  if (bytes < SAMPLE_RATE) {                     // under 0.5 s -> ignore
    store->remove(path);
    return false;
  }
  File h = store->open(path, "r+");              // patch the real length into the header
  if (h) { h.seek(0); writeWavHeader(h, SAMPLE_RATE, bytes); h.close(); }

  if (markers.length()) {
    String mp = path.substring(0, path.length() - 4) + ".mrk";
    File m = store->open(mp, FILE_WRITE);
    if (m) { m.print(markers); m.close(); }
  }
  return true;
}

// =====================================================================
//  Speaker
// =====================================================================
#if SPK_TYPE == SPK_I2S
I2SClass spkI2S;
#endif

// Read exactly n bytes from a file or network stream (false on timeout/end).
static bool readExact(Stream& s, uint8_t* dst, size_t n) {
  return s.readBytes((char*)dst, n) == n;
}
static bool skipBytes(Stream& s, uint32_t n) {
  uint8_t tmp[64];
  while (n) { size_t k = min(n, (uint32_t)sizeof(tmp)); if (!readExact(s, tmp, k)) return false; n -= k; }
  return true;
}

// Plays a WAV coming from any Stream: a file, or the HTTP response directly
// (so spoken answers never need to be stored - good when there is no SD card).
bool playWavStream(Stream& s) {
  s.setTimeout(8000);
  uint8_t hdr[12];
  if (!readExact(s, hdr, 12) || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return false;
  uint32_t rate = 16000, dataLen = 0;
  while (true) {                                 // walk chunks until "data"
    uint8_t ch[8];
    if (!readExact(s, ch, 8)) return false;
    uint32_t sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t fmt[16];
      if (!readExact(s, fmt, 16)) return false;
      rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      if (sz > 16 && !skipBytes(s, sz - 16)) return false;
    } else if (!memcmp(ch, "data", 4)) { dataLen = sz; break; }
    else if (!skipBytes(s, sz)) return false;
  }

  int16_t buf[512];
#if SPK_TYPE == SPK_I2S
  spkI2S.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  if (!spkI2S.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) return false;
#else
  const int64_t period = 1000000LL / rate;
  int64_t next = esp_timer_get_time();
#endif

  uint32_t left = dataLen ? dataLen : 0xFFFFFFFF;
  while (left > 0) {
    if (digitalRead(BTN_REC) == LOW) break;      // press REC to stop playback
    size_t want = min((uint32_t)sizeof(buf), left);
    size_t got = s.readBytes((char*)buf, want);
    if (got == 0) break;                         // end of stream / timeout
    if ((got & 1) && got < want) got += s.readBytes(((char*)buf) + got, 1);   // keep 16-bit alignment
    left -= got;
    size_t n = got / 2;
    for (size_t i = 0; i < n; i++) buf[i] = (int16_t)((int32_t)buf[i] * VOLUME / 100);
#if SPK_TYPE == SPK_I2S
    spkI2S.write((uint8_t*)buf, n * 2);
#else
    for (size_t i = 0; i < n; i++) {
      next += period;
      while (esp_timer_get_time() < next) { }
      dacWrite(SPK_DAC_PIN, (uint8_t)((buf[i] >> 8) + 128));
    }
#endif
  }
#if SPK_TYPE == SPK_I2S
  spkI2S.end();
#else
  dacWrite(SPK_DAC_PIN, 128);
#endif
  return true;
}

bool playWav(const char* path) {                 // play a WAV file from storage
  File f = store->open(path);
  if (!f) return false;
  bool ok = playWavStream(f);
  f.close();
  return ok;
}

// =====================================================================
//  Server communication
// =====================================================================
bool fetchHome() {
  HTTPClient http;
  http.begin(serverUrl("/api/home"));
  http.addHeader("X-Token", DEVICE_TOKEN);
  http.setConnectTimeout(3000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      openTasks = doc["open_tasks"] | 0;
      topTask   = (const char*)(doc["top_task"] | "");
      nextAppt  = (const char*)(doc["next_appt"] | "");
      nextWhen  = (const char*)(doc["next_when"] | "");
      ok = true;
    }
  } else openTasks = -1;
  http.end();
  lastHomeFetch = millis();
  return ok;
}

// Uploads one queued note. On success fills `summary` and deletes the files.
bool uploadNote(const String& path, String& summary) {
  File f = store->open(path);
  if (!f) return false;
  String name = path.substring(path.lastIndexOf('/') + 1);   // note_<epoch>.wav
  String mrkPath = path.substring(0, path.length() - 4) + ".mrk";
  String markers;
  if (store->exists(mrkPath)) { File m = store->open(mrkPath); markers = m.readString(); m.close(); }

  HTTPClient http;
  http.begin(serverUrl("/api/note"));
  http.setConnectTimeout(5000);
  http.setTimeout(65000);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Token", DEVICE_TOKEN);
  http.addHeader("X-Filename", name);
  if (markers.length()) http.addHeader("X-Markers", markers);
  int code = http.sendRequest("POST", &f, f.size());
  f.close();

  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray items = doc["items"].as<JsonArray>();
      summary = "";
      for (JsonObject it : items) {
        String t = it["type"] | "memory"; t.toUpperCase();
        if (summary.length()) summary += " | ";
        summary += t + ": " + String((const char*)(it["title"] | ""));
      }
    }
    ok = true;
    store->remove(path);
    if (store->exists(mrkPath)) store->remove(mrkPath);
  } else Serial.printf("Upload failed: %d\n", code);
  http.end();
  return ok;
}

void uploadQueue() {
  std::vector<String> files = listQueue();
  for (size_t i = 0; i < files.size(); i++) {
    if (WiFi.status() != WL_CONNECTED) break;
    showMessage("Syncing offline notes", String(i + 1) + " of " + files.size());
    String s;
    if (!uploadNote(files[i], s)) break;         // server down -> try again later
  }
  refreshQueueCount();
}

// POST (audio) or GET a URL and play the WAV reply straight from the network.
// Nothing is saved, so answers of any length work even without an SD card.
// Returns false if the server didn't answer with audio.
bool requestAndPlay(const String& url, const String* uploadPath) {
  HTTPClient http;
  http.begin(url);
  http.setConnectTimeout(5000);
  http.setTimeout(65000);
  http.addHeader("X-Token", DEVICE_TOKEN);
  const char* keys[] = {"X-Answer-Text"};
  http.collectHeaders(keys, 1);
  int code;
  if (uploadPath) {
    File f = store->open(*uploadPath);
    http.addHeader("Content-Type", "audio/wav");
    code = http.sendRequest("POST", &f, f.size());
    f.close();
  } else code = http.GET();

  bool ok = false;
  if (code == 200) {
    String text = http.header("X-Answer-Text");
    showMessage("Speaking...", text.substring(0, 21), text.substring(21, 42), "(REC = stop)");
    WiFiClient* s = http.getStreamPtr();
    if (s) ok = playWavStream(*s);
  } else Serial.printf("Audio request failed: %d\n", code);
  http.end();
  if (!ok) { showMessage("Sorry, no answer", "Check server / key"); delay(2000); }
  return ok;
}

// =====================================================================
//  Actions
// =====================================================================
void doNote() {
  String path = "/queue/note_";
  time_t now = time(nullptr);
  if (now > 1700000000) path += String((uint32_t)now);
  else path += "m" + String(esp_random() % 100000);          // clock not synced yet
  path += ".wav";

  if (!recordWhileHeld(BTN_REC, path, "REC", true)) {
    showMessage("Too short!", "Hold the upper button", "while you speak.");
    delay(1500); return;
  }
  refreshQueueCount();
  if (WiFi.status() == WL_CONNECTED) {
    showMessage("Processing...", "AI is organising", "your note");
    String summary;
    if (uploadNote(path, summary)) {
      refreshQueueCount();
      showMessage("Saved:", summary.substring(0, 21), summary.substring(21, 42), summary.substring(42, 63));
      fetchHome();
    } else showMessage("Saved offline", "Server not reachable", "Will sync later");
  } else showMessage("Saved offline", "Will sync when", "Wi-Fi is back");
  delay(2500);
}

void doAsk() {
  String path = "/ask.wav";
  if (!recordWhileHeld(BTN_ASK, path, "ASK", false)) {
    showMessage("Too short!", "Hold the lower button", "and ask a question.");
    delay(1500); return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    showMessage("Offline", "Questions need Wi-Fi");
    store->remove(path); delay(2000); return;
  }
  showMessage("Thinking...");
  requestAndPlay(serverUrl("/api/ask"), &path);
  store->remove(path);
}

void doBriefing(const char* kind) {
  if (WiFi.status() != WL_CONNECTED) { showMessage("Offline", "Briefings need Wi-Fi"); delay(2000); return; }
  showMessage(strcmp(kind, "week") == 0 ? "Weekly briefing..." : "Daily briefing...", "Preparing audio");
  requestAndPlay(serverUrl(String("/api/briefing?kind=") + kind), nullptr);
  fetchHome();
}

// Lower button: hold > 0.4 s = ask, otherwise count clicks.
void handleAskButton() {
  uint32_t t0 = millis();
  while (digitalRead(BTN_ASK) == LOW) {
    if (millis() - t0 > 400) { doAsk(); return; }
    delay(5);
  }
  int clicks = 1;
  uint32_t t1 = millis();
  while (millis() - t1 < 450) {
    if (digitalRead(BTN_ASK) == LOW) {
      delay(30);
      while (digitalRead(BTN_ASK) == LOW) delay(5);
      clicks++; t1 = millis();
    }
    delay(5);
  }
  doBriefing(clicks >= 2 ? "week" : "day");
}

void handleWifi() {
  uint32_t interval = (WiFi.status() == WL_CONNECTED) ? 10000 : 30000;
  if (lastWifiCheck && millis() - lastWifiCheck < interval) return;
  lastWifiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) wifiMulti.run(4000);
  if (WiFi.status() == WL_CONNECTED) {
    if (!everConnected) { everConnected = true; Serial.print("IP: "); Serial.println(WiFi.localIP()); }
    if (queueCount > 0) uploadQueue();
    if (!lastHomeFetch || millis() - lastHomeFetch > 60000) fetchHome();
  } else openTasks = -1;
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_ASK, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("OLED not found");
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(10, 12); oled.print("Pala Lite");
  oled.setTextSize(1); oled.setCursor(10, 40); oled.print("starting...");
  oled.display();

  initStorage();
  refreshQueueCount();
  Serial.printf("Storage: %s, free %u KB, queued notes: %d\n",
                usingSD ? "SD card" : "internal flash", (unsigned)(freeBytes() / 1024), queueCount);

  WiFi.mode(WIFI_STA);
  for (auto& w : WIFI_LIST) wifiMulti.addAP(w.ssid, w.pass);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  wifiMulti.run(8000);
  handleWifi();
  drawHome();
}

void loop() {
  handleWifi();
  if (digitalRead(BTN_REC) == LOW) {
    delay(30);
    if (digitalRead(BTN_REC) == LOW) { doNote(); lastDraw = 0; }
  }
  if (digitalRead(BTN_ASK) == LOW) {
    delay(30);
    if (digitalRead(BTN_ASK) == LOW) { handleAskButton(); lastDraw = 0; }
  }
  if (!lastDraw || millis() - lastDraw > 1000) { drawHome(); lastDraw = millis(); }
  delay(10);
}
