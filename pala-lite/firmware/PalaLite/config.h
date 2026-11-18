#pragma once
// =====================================================================
//  Pala Lite – configuration. Edit this file, then upload the sketch.
// =====================================================================

// ---------- Wi-Fi: up to 5 networks (phone hotspot, home, college) ----------
struct WifiCred { const char* ssid; const char* pass; };
static const WifiCred WIFI_LIST[] = {
  {"MyPhoneHotspot", "hotspot-password"},
  {"HomeWiFi",       "home-password"},
  // {"CollegeWiFi", "password"},
  // {"Network4",    "password"},
  // {"Network5",    "password"},
};

// ---------- AI server (the laptop running server/server.py) ----------
#define SERVER_HOST  "192.168.1.50"   // laptop's IP on the SAME Wi-Fi (see README)
#define SERVER_PORT  5000
#define DEVICE_TOKEN "change-me"      // must match DEVICE_TOKEN in server/.env

// ---------- Time ----------
#define TZ_INFO "IST-5:30"            // India. POSIX TZ string.

// ---------- Which microphone are you using? ----------
#define MIC_I2S     1   // INMP441 digital I2S mic   (recommended, best quality)
#define MIC_ANALOG  2   // MAX9814 module / electret + preamp on an ADC pin
#define MIC_TYPE    MIC_I2S

// ---------- Which speaker amplifier are you using? ----------
#define SPK_I2S     1   // MAX98357A I2S amplifier  (recommended)
#define SPK_DAC     2   // PAM8403 / LM386 fed from ESP32 DAC pin GPIO25
#define SPK_TYPE    SPK_I2S

#define VOLUME      70  // 0-100

// ---------- Pins (ESP32 DevKit V1, 30-pin) ----------
// OLED SSD1306 128x64, I2C
#define OLED_SDA     21
#define OLED_SCL     22
#define OLED_ADDR    0x3C   // some modules use 0x3D

// Buttons (other leg to GND, internal pull-up used)
#define BTN_REC      32     // upper button: hold = record a note
#define BTN_ASK      33     // lower button: hold = ask, 1 click = today, 2 clicks = week

// INMP441 I2S microphone (L/R pin -> GND, VDD -> 3.3V)
#define MIC_SCK      26
#define MIC_WS       27
#define MIC_SD       34

// Analog microphone (MAX9814 OUT) – only used when MIC_TYPE == MIC_ANALOG
#define MIC_ANALOG_PIN  36  // "VP" pin
#define MIC_ANALOG_GAIN 1.0

// MAX98357A I2S amplifier
#define AMP_BCLK     14
#define AMP_LRC      13
#define AMP_DIN      4

// DAC output to PAM8403/LM386 – only used when SPK_TYPE == SPK_DAC
#define SPK_DAC_PIN  25

// microSD card module (SPI). Set USE_SD_CARD to false if you have no SD module:
// recordings are then kept in the ESP32's internal flash (LittleFS) until synced.
#define USE_SD_CARD  false
#define SD_SCK       18
#define SD_MISO      19
#define SD_MOSI      23
#define SD_CS        5

// Battery voltage through a 100k/100k divider (optional)
#define BAT_PIN      35
#define BAT_DIVIDER  2.0f

// ---------- Audio ----------
#define SAMPLE_RATE     16000   // 16 kHz, 16-bit mono (what Gemini expects)
#define MAX_RECORD_SEC  120     // safety limit per recording
