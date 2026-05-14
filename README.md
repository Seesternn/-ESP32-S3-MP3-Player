# 🎵 ESP32-S3 MP3 Player

A feature-rich, open-source portable MP3 player built on the **ESP32-S3-N16R8** microcontroller. It plays MP3 files from a MicroSD card, displays a clean OLED interface, monitors battery health in real time via an INA226 power sensor, and manages power through a high-efficiency TPS63030 Buck-Boost converter.

---


## ✨ Features

- 🎵 MP3 playback from MicroSD card (`/music` folder) via I2S
- 📺 128×64 SSD1306 OLED interface with animated menus
- 🔋 Real-time battery monitoring — voltage, current draw, and estimated remaining time
- 🔊 Separate volume control menu (0–21 levels)
- 🔀 Shuffle mode
- ⏮⏭ Previous / Next track navigation from Now Playing screen
- ⏸ Play / Pause with graphical ▶ ⏸ icons
- 📊 Song progress bar with elapsed / total time display
- 💤 Deep sleep mode with safe SD card unmounting
- 🎚️ Scrollable playlist with currently-playing indicator
- 📡 Dual I2C buses — OLED and INA226 run completely independently

---

## ⚠️ Hardware Requirements — Read Carefully

### Microcontroller: ESP32-S3-N16R8 (MANDATORY)

This project is specifically designed and tested on the **ESP32-S3-N16R8** variant. Do **not** substitute with a standard ESP32, ESP32-S2, or a different ESP32-S3 variant without significant rework.

**Why N16R8 specifically?**

| Feature | Requirement | Why It Matters |
|---|---|---|
| Flash | 16 MB (N16) | Arduino partition `16M Flash (3MB APP/9.9MB FATFS)` is required for the compiled firmware + audio buffers |
| PSRAM | 8 MB OPI (R8) | Audio decoding, SD buffering, and OLED framebuffer all compete for RAM; without 8 MB OPI PSRAM the audio library will crash or stutter |
| CPU | Dual-core 240 MHz | Audio decoding runs on one core, UI + sensor polling runs on the other |
| GPIO count | 45 usable GPIOs | SPI + dual I2C + I2S + 4 buttons consume many pins; the N16R8 dev module exposes enough |

**Arduino IDE Board Settings:**

```
Board:         ESP32S3 Dev Module
PSRAM:         OPI PSRAM
Flash Mode:    QIO 80MHz
Flash Size:    16MB (128Mb)
Partition:     16M Flash (3MB APP/9.9MB FATFS)
CPU Speed:     240MHz (WiFi)
Upload Speed:  921600
```

---

### Power Supply: TPS63030 Buck-Boost Converter (Critical)

Powering an ESP32-S3 + I2S amplifier + OLED + SD card from a single-cell Li-Ion battery is **electrically demanding**. A low-quality regulator will cause:

- Audio crackling and dropouts (voltage sag during decode peaks)
- SD card read errors (SPI voltage instability)
- Random resets under load
- OLED flickering

**This project uses the [Boardoza TPS63030 Buck-Boost Converter Breakout Board](https://boardoza.com).**

The TPS63030 is a high-efficiency (up to 96%) buck-boost converter that accepts an input range of **2.0 V – 5.5 V** and delivers a stable **3.3 V output** — even as a 1S Li-Ion cell discharges from 4.2 V down to 3.0 V. This seamless transition through the crossover point (when V_in ≈ V_out ≈ 3.3 V) is what makes it ideal for 1S Li-Ion applications.

**Why a quality converter matters:**

| Regulator Type | Problem |
|---|---|
| Cheap LDO (e.g. AMS1117) | Drops out below ~3.6V input; last 30% of battery is unusable |
| Cheap switching module | High ripple → audio noise, SD errors |
| TPS63030 | Stable 3.3V from 2.0V to 5.5V, ultra-low ripple, high efficiency |

**Wiring the TPS63030 Breakout:**

```
Li-Ion Cell (+) ──→ VIN
Li-Ion Cell (–) ──→ GND
VOUT            ──→ ESP32-S3 3V3 pin
GND             ──→ ESP32-S3 GND
```

Set the output voltage to **3.3 V** using the trimmer or solder jumper on the breakout board before connecting to the ESP32.

---

### Battery

- **Type:** 1S Li-Ion (single cell, 3.7 V nominal)
- **Capacity:** 900 mAh (configured in firmware — change `BATT_CAPACITY` to match your cell)
- **Voltage range:** 3.0 V (empty) — 4.2 V (full)
- **Connector:** JST-PH 2.0 recommended

The INA226 shunt resistor (100 mΩ) is placed in series with the positive battery line, before the TPS63030 input, so it measures **total system current draw** accurately.

---

## 🔌 Wiring & Pin Assignments

### Complete Pin Map

| Signal | GPIO | Direction | Notes |
|---|---|---|---|
| OLED SDA | 39 | Bidirectional | I2C Bus 0 (Wire) |
| OLED SCL | 38 | Output | I2C Bus 0 (Wire) |
| INA226 SDA | 16 | Bidirectional | I2C Bus 1 (Wire1) |
| INA226 SCL | 17 | Output | I2C Bus 1 (Wire1) |
| I2S BCLK | 20 | Output | Bit clock to amplifier |
| I2S LRC | 21 | Output | Left/Right word select |
| I2S DOUT | 18 | Output | Serial audio data |
| SD CS | 10 | Output | SPI chip select |
| SD SCK | 12 | Output | SPI clock |
| SD MISO | 13 | Input | SPI data from SD |
| SD MOSI | 11 | Output | SPI data to SD |
| BTN UP | 45 | Input | Pull-up, active LOW |
| BTN DOWN | 46 | Input | Pull-up, active LOW |
| BTN OK | 47 | Input | Pull-up, active LOW |
| BTN BACK | 48 | Input | Pull-up, active LOW |

---

### Pin Selection Rationale

#### OLED — GPIO 38 (SCL), GPIO 39 (SDA)
GPIO 38 and 39 are fully capable I2C pins on the ESP32-S3-N16R8 and are placed physically adjacent on most dev module breakouts, making routing clean. They are assigned to **I2C Bus 0 (`Wire`)**, which is the default bus the Adafruit SSD1306 library uses. These pins have no ADC or touch-sensor functions that could interfere.

#### INA226 — GPIO 16 (SDA), GPIO 17 (SCL)
The INA226 is deliberately placed on a **separate I2C bus** (`Wire1`, I2C Bus 1). Both the OLED (0x3C) and INA226 (0x40) are slave devices with fixed, close addresses — sharing a bus is possible but introduces complexity and potential conflicts if the INA226 I2C address jumper is ever moved. Running `Wire1` on GPIO 16/17 keeps the buses completely independent, eliminates address collision risk, and means either peripheral can be re-initialized without disturbing the other.

#### I2S — GPIO 18 (DOUT), GPIO 20 (BCLK), GPIO 21 (LRC)
The ESP32-S3 routes I2S peripherals to almost any GPIO via its GPIO matrix. GPIO 18, 20, and 21 were chosen because:
- They are **not** part of the SPI2 (SD card) pin group
- They do **not** share functions with the JTAG debugger pins (GPIO 39–42 on some sub-variants)
- They sit in a contiguous physical region on the dev module, simplifying wiring

#### SD Card SPI — GPIO 10 (CS), GPIO 11 (MOSI), GPIO 12 (SCK), GPIO 13 (MISO)
These are the **default SPI2 (HSPI) pins** on the ESP32-S3. Using the hardware SPI peripheral (rather than bit-banging) is essential for reliable, high-speed SD card reads during audio decoding. Hardware SPI on these pins uses DMA, which offloads transfer work from the CPU and prevents audio buffer underruns.

#### Buttons — GPIO 45, 46, 47, 48
These are **input-only safe** GPIOs in the upper range of the ESP32-S3-N16R8's exposed pins. They have no boot-strapping functions (unlike GPIO 0, 3, 45, 46 on older ESP32 variants — note the ESP32-**S3** handles 45/46 differently), no ADC channels that would waste analog resources, and no SPI/I2C/I2S assignments. They are configured with `INPUT_PULLUP`, so buttons simply connect pin to GND — no external resistors needed. A 200 ms software debounce is applied in the main loop.

---

## 🔊 I2S Amplifier Module — Back Pad Reference (H1L / H2L / H3L / H4L)

Most inexpensive I2S amplifier breakout boards (MAX98357A, PCM5102A, etc.) expose solder-bridge pads on the **back of the PCB** labeled in the format `HxL`. These are configuration pads that determine the behaviour of the chip's control pins.

### What HxL Means

The naming convention works as follows:

- **`H`** = the pad's default state is **HIGH** (pulled up internally or via a resistor to VCC)
- **`x`** = pad/jumper number (1 through 4)
- **`L`** = **bridging this pad** with solder pulls it **LOW**

In other words: leave the pad open → pin is HIGH. Bridge the pad with a blob of solder → pin is pulled LOW.

> ⚠️ The SCK pin must be connected to GND; otherwise, there may be no sound.

### Pad Functions (MAX98357A-based modules)

| Pad | Pin it controls | Open (HIGH) | Bridged (LOW) |
|---|---|---|---|
| **H1L** | `GAIN` configuration bit 0 | Part of gain ladder resistor network | Pulls GAIN toward GND — typically sets **12 dB** gain |
| **H2L** | `GAIN` configuration bit 1 | Part of gain ladder | Pulls GAIN toward VDD — typically sets **15 dB** gain |
| **H3L** | `SD_MODE` (Shutdown / Enable) | Normal operation (chip active) | Forces chip into **shutdown** (silent, low power) |
| **H4L** | Channel select (`LR` or `LRCLK`-based) | **Left** channel output | **Right** channel output (or mono mix, depending on variant) |

> ⚠️ Exact pad-to-function mapping varies between manufacturers. Always cross-reference with your specific module's silk screen and the MAX98357A datasheet Table 1 (GAIN pin configurations) and Table 2 (SD_MODE states).

### Gain Reference Table (MAX98357A)

| GAIN Pin Condition | Output Gain |
|---|---|
| GAIN left open (floating) | **9 dB** (default — most modules ship this way) |
| GAIN tied to GND via 100 kΩ | **12 dB** |
| GAIN tied to VDD via 100 kΩ | **15 dB** |
| GAIN tied directly to GND | **6 dB** |
| GAIN tied directly to VDD | Mute / over-voltage protection |

For this project, the default 9 dB gain is used (all pads left open). If your speaker is quiet, bridge **H1L** to increase to 12 dB.

### SD_MODE (H3L) Caution

**Do not bridge H3L in normal use.** This shuts the amplifier down completely. It is only useful if you want to add hardware mute control. The firmware currently uses the Audio library's software volume (`audio.setVolume(0)`) rather than hardware shutdown.

---

## 📦 Libraries Required

Install all of the following via **Arduino IDE → Library Manager** or **PlatformIO**:

| Library | Author | Purpose |
|---|---|---|
| `ESP32-audioI2S` | schreibfaul1 | MP3 decoding + I2S output |
| `Adafruit SSD1306` | Adafruit | OLED display driver |
| `Adafruit GFX Library` | Adafruit | Graphics primitives |
| `INA226_WE` | Wolfgang Ewald | INA226 power sensor driver |
| `SD` (ESP32 built-in) | Espressif | SD card filesystem |
| `SPI` (ESP32 built-in) | Espressif | SPI hardware driver |
| `Wire` (ESP32 built-in) | Espressif | I2C hardware driver |

---

## 📁 MicroSD Card Setup

1. Format the card as **FAT32**
2. Create a folder named exactly `/music` in the root
3. Copy your `.mp3` files into `/music`
4. The player supports up to **500 tracks** (increase the `songs[500]` array if needed)

```
SD Card Root
└── music/
    ├── 01 - Track One.mp3
    ├── 02 - Track Two.mp3
    └── ...
```

File names are displayed truncated to fit the 128-pixel screen width. Shorter, descriptive names are recommended.

---

## 📋 Menu Structure

```
MAIN MENU
├── Su An Caliyor  →  NOW PLAYING
│                       Song name (truncated)
│                       [========------] progress bar
│                       00:43 / 03:21
│                         ▶ / ⏸  (play/pause icon)
│                       ↑ = Previous track
│                       ↓ = Next track
│                       OK = Play / Pause
│
├── Sarki Listesi  →  PLAYLIST
│                       Scrollable song list
│                       * = currently playing
│                       OK = Play selected song
│
├── Ses Ayarlari   →  AUDIO SETTINGS
│                       Volume bar (0–21)
│                       ↑/↓ = adjust volume
│                       OK = return to menu
│
├── Batarya        →  BATTERY INFO
│                       Graphical charge indicator
│                       Voltage (V), Current (mA)
│                       Estimated remaining time
│
├── Ayarlar        →  SETTINGS
│                       Shuffle On / Off toggle
│
└── Kapat          →  Deep Sleep
                        (SD safely unmounted first)
```

---

## 🔋 Battery Monitoring Details

The **INA226** is a precision power monitor IC connected in series with the battery's positive rail.

### Shunt Resistor

A **100 mΩ (0.1 Ω)** shunt resistor is placed between the battery positive terminal and the TPS63030 VIN. The INA226 measures the tiny voltage drop across it to calculate current:

```
I (A) = V_shunt (V) / R_shunt (Ω)
I (A) = V_shunt / 0.1
```

For a 900 mAh battery drawing 150 mA of system current, the shunt drop is only **15 mV** — imperceptible to system performance.

### State of Charge Estimation

Battery percentage is derived from terminal voltage using a linear approximation between the defined empty and full thresholds:

```
% = (V_measured - V_min) / (V_max - V_min) × 100
  = (V_measured - 3.0) / (4.2 - 3.0) × 100
```

This is an approximation. Li-Ion discharge curves are non-linear, so accuracy is ±10% in the mid-range and improves near full/empty. For a more accurate coulomb-counting approach, the INA226's continuous current integration can be added in a future version.

### Remaining Time Calculation

```
Remaining capacity (mAh) = (% / 100) × 900 mAh
Remaining time (hours)   = Remaining capacity / I_current (mA)
```

This is an **instantaneous estimate** based on current draw at the moment of reading. It updates every 2 seconds. If the device is idle or volume is low, the estimate will be longer; during heavy decode or high volume, shorter.

### INA226 Configuration

| Parameter | Value | Reason |
|---|---|---|
| Averaging | 16 samples | Smooths out noise from switching regulator and I2S DMA spikes |
| Conversion time | 1.1 ms (bus + shunt) | Balances measurement speed vs. accuracy |
| Max current range | 1.3 A | Covers worst-case ESP32-S3 peak (WiFi off, audio at max volume) |
| Update interval | 2 seconds | Sufficient for a battery gauge; avoids I2C bus contention |

---

## 🔧 Firmware Configuration Constants

All user-tunable parameters are at the top of `music_player.ino`:

```cpp
// Power
#define SHUNT_OHMS     0.10f   // Your shunt resistor value in Ohms
#define MAX_CURRENT_A  1.30f   // Maximum expected current draw
#define BATT_CAPACITY  900.0f  // Your battery capacity in mAh
#define BATT_MAX_V     4.20f   // Full charge voltage
#define BATT_MIN_V     3.00f   // Empty voltage (cutoff)

// INA226
#define INA226_ADDR    0x40    // Default I2C address (ADDR pin → GND)
                               // 0x41 if ADDR → VCC
                               // 0x44 if ADDR → SDA
                               // 0x45 if ADDR → SCL

// Audio
int volume = 12;               // Startup volume (0–21)

// Playback
String songs[50];              // Increase if you have more than 50 tracks
```

---

## 🛠️ Build & Upload

1. Install **Arduino IDE 1.8.x or 2.x**
2. Add ESP32 board support:
   `File → Preferences → Additional Boards Manager URLs`:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Install **ESP32 by Espressif Systems** (tested on v3.x)
4. Install all libraries listed in the Libraries section
5. Select board settings as described in the Hardware Requirements section
6. Connect ESP32-S3 via USB, select the correct COM port
7. Click **Upload**

---

## 🐛 Troubleshooting

| Symptom | Likely Cause | Fix |
|---|---|---|
| No audio / crackling | Power supply instability | Check TPS63030 output is exactly 3.3V under load |
| SD card not detected | SPI wiring or card format | Reformat as FAT32; check GPIO 10/11/12/13 connections |
| OLED blank | I2C address mismatch | Scan I2C bus; try `0x3D` instead of `0x3C` |
| INA226 shows 0V | VS+ (VBUS) pin not connected | Bridge INA226 `IN+` to `VS+` on the module |
| Battery % wrong | Wrong min/max voltage constants | Measure your actual cell voltage at full and empty, adjust `BATT_MIN_V`/`BATT_MAX_V` |
| Firmware too large | Wrong partition scheme | Select `16M Flash (3MB APP/9.9MB FATFS)` in board settings |
| Audio stutters | Insufficient PSRAM | Confirm your module is **N16R8** (OPI PSRAM), not N16 without R |

---

## 📄 License

MIT License. See [LICENSE](LICENSE) for details.

---

## 🙏 Acknowledgements

- [schreibfaul1](https://github.com/schreibfaul1/ESP32-audioI2S) — ESP32-audioI2S library
- [Adafruit](https://github.com/adafruit/Adafruit_SSD1306) — SSD1306 OLED library
- [Wolfgang Ewald](https://github.com/wollewald/INA226_WE) — INA226_WE library
- [Espressif Systems](https://github.com/espressif/arduino-esp32) — ESP32 Arduino core
- [Boardoza](https://boardoza.com) — TPS63030 Buck-Boost Breakout Board
