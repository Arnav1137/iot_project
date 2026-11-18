# Pala Lite: an AI voice-notes recorder (ESP32 IoT project)

This is a simpler version of Pala Note 2.0, built from the parts you already have plus a few cheap modules.

* **Upper button (REC):** hold it and speak. When you let go, the AI turns your recording into a **task**, an **appointment** or a **memory**. While recording, tap the lower button to add a **marker**.
* **Lower button (ASK):**
  * **Hold:** ask a question about your old notes. You get a spoken answer from the Gemini native-audio model.
  * **1 click:** spoken briefing for **today**.
  * **2 clicks:** spoken briefing for **this week**.
* **OLED home screen:** time, Wi-Fi, battery, number of open tasks, the top task and the next appointment.
* **Offline recording:** without Wi-Fi, notes are saved to the SD card (or the ESP32's own flash) and uploaded later.
* **Up to 5 saved Wi-Fi networks.**
* **Web viewer:** runs on your laptop. You can search notes, play and download audio, tick off tasks, view a calendar, see past questions and answers, and try everything by typing.

## How it works

```
 ESP32 (records WAV, OLED, buttons, speaker)
        │  Wi-Fi, plain HTTP on your local network
        ▼
 Laptop: server/server.py  (Flask + SQLite + web viewer)
        │
        ├── Gemini 2.5 Flash (free)       → transcription
        ├── Gemini Flash or Cerebras      → sorting notes, writing briefings
        ├── Gemini native audio (free)    → spoken answers to spoken questions
        └── Gemini TTS                    → reading briefings aloud
```

There are three reasons a laptop server sits in the middle:

1. A plain ESP32 has only about 320 KB of RAM and no PSRAM, which is too little for HTTPS calls carrying audio and for the Live API websocket.
2. Your API keys stay off the device.
3. The web viewer comes almost for free.

---

## 1. Components

### Parts you already have
| Part | Used for |
|---|---|
| ESP32 DevKit V1 (30-pin) | Main controller |
| 0.96" OLED (SSD1306, I2C, 128×64) | Home screen |
| Speaker (4–8 Ω) | Spoken answers (needs an amplifier, see below) |
| Electret mic capsule (2-pin) | Only usable with a preamp; see the note below |
| Charging module (TP4056) | Charges the battery from USB |
| 3.7 V 2000 mAh Li-ion battery and holder | Power |
| Breadboard and jumper wires | Wiring |

### Parts you still need to buy (approx. prices on Robu.in / Amazon India)
| Part | Why | Approx. |
|---|---|---|
| **INMP441 I2S microphone module** | The 2-pin capsule's signal is only a few millivolts. The ESP32 can't read it directly without an amplifier. The INMP441 is digital, clean and needs no tuning. | ₹150–250 |
| **MAX98357A I2S amplifier module** | An ESP32 pin can't drive a speaker on its own | ₹180–300 |
| **MT3608 boost converter** (set to 5 V) | The battery gives 3.0–4.2 V, but the DevKit's regulator needs about 4.5 V+ on VIN | ₹40–80 |
| **2× push buttons** (tactile 6×6 mm) | REC and ASK | ₹10 |
| **microSD card module (SPI) + FAT32 microSD card** | Long and offline recordings. *Optional:* without it, about 60 s of notes can wait in the ESP32's internal flash (set `USE_SD_CARD false`). | ₹60 + card |
| 2× 100 kΩ resistors | Battery level reading (optional) | ₹5 |
| Slide switch | Power on/off | ₹10 |

**Cheaper alternatives** (switch them in `config.h`):
* **Mic:** a **MAX9814** module (₹150) gives analog output with auto gain. Set `MIC_TYPE MIC_ANALOG` and connect OUT to GPIO36. Your 2-pin capsule can be used the same way if you build a preamp for it (2.2 kΩ bias resistor, 1 µF capsule plus an LM358/transistor stage biased to 1.65 V). The MAX9814 is much easier.
* **Speaker amp:** a **PAM8403** (₹40) fed from the ESP32's DAC pin GPIO25. Set `SPK_TYPE SPK_DAC`. The sound is a bit lower quality but fine for speech.

---

## 2. Wiring

| From | To ESP32 |
|---|---|
| **OLED** VCC / GND / SDA / SCL | 3V3 / GND / **GPIO21** / **GPIO22** |
| **INMP441** VDD / GND / L/R | 3V3 / GND / GND |
| INMP441 SCK / WS / SD | **GPIO26** / **GPIO27** / **GPIO34** |
| **MAX98357A** VIN / GND | 5V (VIN) / GND |
| MAX98357A BCLK / LRC / DIN | **GPIO14** / **GPIO13** / **GPIO4** |
| MAX98357A speaker + / − | Speaker wires |
| **REC button** | **GPIO32** ↔ GND |
| **ASK button** | **GPIO33** ↔ GND |
| **SD module** VCC / GND | 5V (or 3V3 if the module has no regulator) / GND |
| SD SCK / MISO / MOSI / CS | **GPIO18** / **GPIO19** / **GPIO23** / **GPIO5** |
| Battery level: B+ → 100 kΩ → **GPIO35** → 100 kΩ → GND | (optional) |
| *MAX9814 OUT (if analog mic)* | *GPIO36 (VP)* |
| *PAM8403 L-in (if DAC speaker)* | *GPIO25, PAM8403 GND to GND* |

### Power
```
 USB ─► TP4056 (IN)
        TP4056 B+ / B−  ─► battery + / −
        TP4056 OUT+ ─► switch ─► MT3608 VIN+      MT3608 VOUT+ (adjust to 5.0 V!) ─► ESP32 VIN
        TP4056 OUT− ───────────► MT3608 VIN−      MT3608 VOUT− ─► ESP32 GND
```
* **Turn the MT3608's screw until a multimeter reads 5.0 V *before* you connect the ESP32.** Out of the box they can put out 12 V or more.
* Use a TP4056 board that has **6 pins (with DW01 protection)**. If yours has only 4 pins, the battery has no over-discharge protection.
* **Don't plug the ESP32's USB in while the battery switch is ON.** Turn the switch off before uploading code.
* Battery life: the ESP32 with Wi-Fi draws about 100–160 mA, so a 2000 mAh battery lasts roughly **10–14 hours** of use.

---

## 3. Server setup (laptop)

1. Install Python 3.10+.
2. Get a **free Gemini API key**: https://aistudio.google.com/apikey
3. In a terminal:
   ```
   cd server
   pip install -r requirements.txt
   copy .env.example .env        (Mac/Linux: cp .env.example .env)
   ```
4. Open `.env`, paste your `GEMINI_API_KEY`, and pick a `DEVICE_TOKEN` (any password).
5. Run `python server.py`. It prints your laptop's IP, for example `#define SERVER_HOST "192.168.1.7"`.
6. Open **http://localhost:5000** and go to **Try it**. Type a note like *"Meeting with Dr. Rana tomorrow at 3pm"* and check that it shows up as an appointment. If it works, the AI side is ready.

**Laptop firewall:** on Windows, click "Allow" when Python asks for network access. If the ESP32 can't connect, allow port 5000 in Windows Defender Firewall.

**Using Cerebras (optional):** Cerebras now gives a one-time trial credit (it needs a card) instead of a permanent free tier. If you have a key, set `TEXT_LLM=cerebras` and `CEREBRAS_API_KEY=...`. If a Cerebras call fails, the server falls back to Gemini automatically.

**Model names change.** If Google renames a model, update the `GEMINI_*_MODEL` lines in `.env`. You don't need to touch the code.

## 4. Firmware setup (ESP32)

1. Install the **Arduino IDE 2**.
2. Go to File → Preferences → *Additional boards URLs* and add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
   Then open Boards Manager and install **esp32 by Espressif (version 3.x)**.
3. In the Library Manager, install **Adafruit SSD1306**, **Adafruit GFX Library** and **ArduinoJson** (v7).
4. Open `firmware/PalaLite/PalaLite.ino` and edit **config.h**:
   * your Wi-Fi networks (the ESP32 supports **2.4 GHz only**; a phone hotspot works well),
   * `SERVER_HOST` (the IP the server printed),
   * `DEVICE_TOKEN` (the same one as in `.env`),
   * `MIC_TYPE` / `SPK_TYPE` if you use the cheaper alternatives.
5. Choose Tools → Board → **ESP32 Dev Module**.
   Then Tools → Partition Scheme → **"No OTA (2MB APP/2MB SPIFFS)"**. The firmware is about 1.23 MB, too big for the default 1.2 MB partition, and this scheme also gives 2 MB of flash for recordings.
6. Upload. Open the Serial Monitor at 115200 baud to see the IP and any errors.

The ESP32 and the laptop must be on the **same Wi-Fi network**. A phone hotspot with both connected is the easiest setup for a demo.

## 5. Using it
| Action | Result |
|---|---|
| Hold **REC**, speak, release | "Processing…", then e.g. `TASK: Submit DAA file` |
| Tap **ASK** while recording | Adds a marker (shown in the web viewer) |
| Hold **ASK**, wait for "ASK" on screen, speak, release | Spoken answer |
| Click **ASK** once | Today's briefing |
| Double-click **ASK** | This week's briefing |
| Press **REC** while it is speaking | Stops playback |

## 6. Troubleshooting
* **"Server not reachable":** check the IP in config.h, the firewall, and that both devices are on the same Wi-Fi. Try opening `http://<laptop-ip>:5000` from your phone.
* **The recording is silent:** check the INMP441 wiring. L/R must go to GND, and VDD must be 3.3 V, not 5 V.
* **Crackling or very quiet speaker:** lower `VOLUME`, check that the MAX98357A gets 5 V, and keep speaker wires short.
* **Error 401:** `DEVICE_TOKEN` doesn't match between config.h and .env.
* **Error 429 / quota in the server window:** you hit the free-tier rate limit. Wait a minute. Notes are never lost: they are saved as "Unprocessed recording", and you can click **Reprocess** in the web viewer later.
* **"Sketch too big":** choose the "No OTA (2MB APP/2MB SPIFFS)" partition (step 4.5).
* **OLED blank:** some modules use address `0x3D`. Change `OLED_ADDR`.

## Features compared with Pala Note 2.0
| Pala Note 2.0 | Pala Lite |
|---|---|
| AI transcription + auto-organisation | ✅ |
| Tasks & appointments recognised | ✅ |
| Spoken Q&A about recordings | ✅ (Gemini native audio) |
| Daily / weekly spoken briefings | ✅ |
| Home screen with tasks & appointments | ✅ (OLED) |
| Recording markers | ✅ |
| Web viewer: search, playback, downloads, tasks, calendar | ✅ (runs on the laptop instead of the device) |
| Offline recording, syncs later | ✅ |
| 5 Wi-Fi networks | ✅ |
| Interface languages / date formats | Partly: set `LANGUAGE` in .env for spoken replies |
| E-paper display, 3D case | ❌ (OLED + breadboard; a case can be added later) |
