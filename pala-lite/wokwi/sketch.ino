/*
  Pala Lite – hardware test. Upload this BEFORE the main firmware and add
  parts one at a time (see the build guide). Same pins as PalaLite/config.h.

  OLED status page shows:  SD card, battery, Wi-Fi, buttons
  Hold REC  -> live microphone level bar (speak into the INMP441)
  Click ASK -> two-tone beep on the MAX98357A + speaker

  Libraries: Adafruit SSD1306, Adafruit GFX. Board: ESP32 Dev Module.
  Also runs in the Wokwi simulator (mic/amp are placeholders there).
*/
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#if __has_include(<ESP_I2S.h>)
  #include <ESP_I2S.h>
  #define HAVE_I2S 1
#else
  #define HAVE_I2S 0
#endif

// ---- pins (same as config.h) ----
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define BTN_REC 32
#define BTN_ASK 33
#define MIC_SCK 26
#define MIC_WS 27
#define MIC_SD 34
#define AMP_BCLK 14
#define AMP_LRC 13
#define AMP_DIN 4
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5
#define BAT_PIN 35

// Optional Wi-Fi check. In Wokwi use "Wokwi-GUEST" with an empty password.
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool sdOk = false;
uint64_t sdMB = 0;

float batteryVolts() { return analogReadMilliVolts(BAT_PIN) * 2.0f / 1000.0f; }

void statusPage() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  oled.print("PALA HARDWARE TEST");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  oled.setCursor(0, 14);
  if (sdOk) { oled.print("SD: OK "); oled.print((uint32_t)sdMB); oled.print(" MB"); }
  else oled.print("SD: none (flash used)");
  oled.setCursor(0, 24);
  float v = batteryVolts();
  if (v < 2.5) oled.print("Bat: USB / not wired");
  else { oled.print("Bat: "); oled.print(v, 2); oled.print(" V"); }
  oled.setCursor(0, 34);
  if (WiFi.status() == WL_CONNECTED) { oled.print("WiFi: "); oled.print(WiFi.localIP()); }
  else oled.print("WiFi: connecting...");
  oled.setCursor(0, 46);
  oled.print(digitalRead(BTN_REC) == LOW ? "REC DOWN " : "REC up   ");
  oled.print(digitalRead(BTN_ASK) == LOW ? "ASK DOWN" : "ASK up");
  oled.setCursor(0, 56);
  oled.print("Hold REC=mic ASK=beep");
  oled.display();
}

// ---------------- microphone level meter ----------------
void micTest() {
#if HAVE_I2S
  I2SClass mic;
  mic.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Mic I2S init failed");
    return;
  }
  mic.setTimeout(100);
  static int32_t raw[512];
  while (digitalRead(BTN_REC) == LOW) {
    size_t n = mic.readBytes((char*)raw, sizeof(raw)) / 4;
    int peak = 0;
    for (size_t i = 0; i < n; i++) peak = max(peak, (int)abs(raw[i] >> 14));
    peak = min(peak, 20000);
    oled.clearDisplay();
    oled.setCursor(0, 0);  oled.print("MIC TEST - speak!");
    oled.setCursor(0, 16); oled.print("Peak: "); oled.print(peak);
    oled.drawRect(0, 34, 128, 16, SSD1306_WHITE);
    oled.fillRect(0, 34, map(peak, 0, 20000, 0, 128), 16, SSD1306_WHITE);
    oled.setCursor(0, 56); oled.print(n == 0 ? "No data: check wiring" : "Release REC to exit");
    oled.display();
    Serial.printf("mic peak %d\n", peak);
  }
  mic.end();
#else
  oled.clearDisplay(); oled.setCursor(0, 20); oled.print("Needs ESP32 core 3.x"); oled.display(); delay(1500);
#endif
}

// ---------------- speaker beep ----------------
void beepTest() {
  oled.clearDisplay(); oled.setCursor(0, 24); oled.print("BEEP! (speaker test)"); oled.display();
#if HAVE_I2S
  I2SClass amp;
  amp.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  if (!amp.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("Amp I2S init failed");
    return;
  }
  int16_t buf[160];
  const int freqs[2] = {880, 1320};
  for (int f = 0; f < 2; f++) {
    for (int block = 0; block < 40; block++) {          // 40 x 10 ms = 0.4 s per tone
      for (int i = 0; i < 160; i++) {
        float t = (block * 160 + i) / 16000.0f;
        buf[i] = (int16_t)(8000 * sinf(2 * PI * freqs[f] * t));
      }
      amp.write((uint8_t*)buf, sizeof(buf));
    }
  }
  amp.end();
#endif
  Serial.println("beep done");
  delay(300);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Pala test starting");
  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_ASK, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("OLED NOT FOUND - check wiring / address");
  else Serial.println("OLED OK");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS);
  if (sdOk) sdMB = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD: %s\n", sdOk ? "OK" : "not found");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("Hold REC = mic test, click ASK = beep test");
}

void loop() {
  static bool wifiPrinted = false;
  if (!wifiPrinted && WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP "); Serial.println(WiFi.localIP());
    wifiPrinted = true;
  }
  if (digitalRead(BTN_REC) == LOW) { delay(30); if (digitalRead(BTN_REC) == LOW) micTest(); }
  if (digitalRead(BTN_ASK) == LOW) {
    delay(30);
    if (digitalRead(BTN_ASK) == LOW) { while (digitalRead(BTN_ASK) == LOW) delay(5); beepTest(); }
  }
  statusPage();
  delay(100);
}
