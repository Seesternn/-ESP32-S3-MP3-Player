// ============================================================
//  ESP32 MP3 Oynatıcı  –  Gelişmiş Sürüm
//  Özellikler:
//    • INA226 ile pil voltaj / akım / süre tahmini (I2C1: 16,17)
//    • OLED SSD1306 128x64  (I2C0: 38,39)
//    • SD Kart üzerinden MP3 çalma (Audio / I2S)
//    • 6-öğeli kaydırmalı ana menü
//    • Now Playing: progress bar + ▶/⏸ ikonu + önceki/sonraki
//    • Ses Ayarları menüsü
//    • Batarya Bilgisi menüsü (900 mAh 1S Li-Ion)
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <INA226_WE.h>
#include "esp_sleep.h"

// ===== OLED =====
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_SDA       39
#define OLED_SCL       38
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== INA226 (Wire1 – I2C Bus 1) =====
#define INA226_SDA    16
#define INA226_SCL    17
#define INA226_ADDR   0x40
#define SHUNT_OHMS    0.10f      // 100 mΩ shunt direnci
#define MAX_CURRENT_A  1.30f     // Maksimum ölçüm aralığı (A)
#define BATT_CAPACITY 900.0f     // mAh  – 1S Li-Ion
#define BATT_MAX_V     4.20f     // Tam şarj voltajı
#define BATT_MIN_V     3.00f     // Boşalma voltajı
INA226_WE ina226(&Wire1, INA226_ADDR);

// ===== I2S =====
#define I2S_BCLK  20
#define I2S_LRC   21
#define I2S_DOUT  18

// ===== SD Kart =====
#define SD_CS   10
#define SD_SCK  12
#define SD_MISO 13
#define SD_MOSI 11

// ===== Butonlar =====
#define BTN_UP    45
#define BTN_DOWN  46
#define BTN_OK    47
#define BTN_BACK  48

Audio audio;

// ===== Menü Durumları =====
enum MenuState {
  MAIN_MENU,
  NOW_PLAYING,
  PLAYLIST,
  AUDIO_SETTINGS,
  BATTERY_INFO,
  SETTINGS
};
MenuState currentMenu = MAIN_MENU;

// ===== Genel Değişkenler =====
String songs[500];
int    songCount      = 0;
int    currentSong    = 0;

int    mainCursor     = 0;   // Ana menü imleci
int    mainScroll     = 0;   // Ana menü kaydırma
int    playlistCursor = 0;

int    volume         = 12;
bool   isPlaying      = false;
bool   isShuffle      = false;
bool   sdOk           = false;
bool   autoNextFlag   = false;
bool   needsUpdate    = true;

// INA226 ölçüm verileri
float battVoltage = 0.0f;   // V
float battCurrent = 0.0f;   // mA (+ = deşarj, – = şarj)
float battPercent = 0.0f;   // %

unsigned long lastInaRead      = 0;
unsigned long lastProgressTick = 0;
#define INA_INTERVAL 2000   // ms

// Ana menü öğe listesi (6 adet)
const char* MENU_ITEMS[] = {
  "Su An Caliyor",
  "Sarki Listesi",
  "Ses Ayarlari",
  "Batarya",
  "Ayarlar",
  "Kapat"
};
const int MENU_COUNT   = 6;
const int VISIBLE_ROWS = 4;   // Ekranda aynı anda görünen satır

// ============================================================
//  YARDIMCI FONKSİYONLAR
// ============================================================
bool readBtn(int pin)          { return digitalRead(pin) == LOW; }
void requestDisplayUpdate()    { needsUpdate = true; }
void audio_eof_mp3(const char*){ autoNextFlag = true; }

float voltToPercent(float v) {
  float p = (v - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V) * 100.0f;
  return constrain(p, 0.0f, 100.0f);
}

void readINA226() {
  if (millis() - lastInaRead < INA_INTERVAL) return;
  lastInaRead = millis();

  float busV   = ina226.getBusVoltage_V();     // VBUS – GND arası
  float shuntV = ina226.getShuntVoltage_mV();  // Shunt üzerindeki düşüm (mV)

  // Shunt IN- tarafındaysa gerçek pil voltajı = busV + (shuntV / 1000)
  battVoltage = busV + (shuntV / 1000.0f);

  // busV sıfır geliyorsa minimum değer ata (ekranda 0% görünmesin)
  if (battVoltage < 0.5f) battVoltage = BATT_MIN_V;

  battCurrent = ina226.getCurrent_mA();  // + deşarj, – şarj
  battPercent = voltToPercent(battVoltage);

  // Seri monitör debug çıktısı (sorun giderme için)
  Serial.printf("[INA226] VBUS=%.3fV  Shunt=%.3fmV  I=%.2fmA  Pct=%.1f%%\n",
                busV, shuntV, battCurrent, battPercent);
}

// ============================================================
//  SD / SES FONKSİYONLARI
// ============================================================
void loadSongs() {
  songCount = 0;
  File root = SD.open("/music");
  if (!root || !root.isDirectory()) { sdOk = false; return; }
  sdOk = true;
  File f = root.openNextFile();
  while (f && songCount < 500) {
    String name = f.name();
    if (name.endsWith(".mp3")) songs[songCount++] = name;
    f.close();
    f = root.openNextFile();
  }
}

void playSong(int idx) {
  if (songCount <= 0) return;
  audio.stopSong();
  String path = "/music/" + songs[idx];
  audio.connecttoFS(SD, path.c_str());
  isPlaying = true;
  requestDisplayUpdate();
}

void nextSong() {
  if (isShuffle) currentSong = random(0, songCount);
  else           currentSong = (currentSong + 1) % songCount;
  playSong(currentSong);
}

void prevSong() {
  if (isShuffle) currentSong = random(0, songCount);
  else           currentSong = (currentSong - 1 + songCount) % songCount;
  playSong(currentSong);
}

// ============================================================
//  GRAFİK YARDIMCILARI
// ============================================================

// Batarya simgesi – x,y: sol üst köşe, 16×8 piksel
void drawBattIcon(int x, int y, float pct) {
  display.drawRect(x, y, 14, 7, SSD1306_WHITE);
  display.drawLine(x + 14, y + 2, x + 14, y + 4, SSD1306_WHITE); // pil ucu
  int fw = constrain((int)(12.0f * pct / 100.0f), 0, 12);
  if (fw > 0) {
    display.fillRect(x + 1, y + 1, fw, 5, SSD1306_WHITE);
  }
}

// ▶ Oynat ikonu – cx,cy: merkez koordinatı
void drawPlayIcon(int cx, int cy) {
  display.fillTriangle(
    cx - 5, cy - 6,
    cx - 5, cy + 6,
    cx + 6, cy,
    SSD1306_WHITE
  );
}

// ⏸ Duraklat ikonu – cx,cy: merkez koordinatı
void drawPauseIcon(int cx, int cy) {
  display.fillRect(cx - 5, cy - 6, 4, 12, SSD1306_WHITE);
  display.fillRect(cx + 1, cy - 6, 4, 12, SSD1306_WHITE);
}

// Dikey kaydırma çubuğu – x: sol kenar, y: başlangıç, h: toplam yükseklik
void drawScrollBar(int x, int y, int h, int total, int visible, int offset) {
  if (total <= visible) return;
  display.drawRect(x, y, 3, h, SSD1306_WHITE);
  int indH = max(4, h * visible / total);
  int indY = y + (h - indH) * offset / max(1, total - visible);
  display.fillRect(x, indY, 3, indH, SSD1306_WHITE);
}

// Yatay dolum çubuğu
void drawProgressBar(int x, int y, int w, int h, float pct) {
  display.drawRoundRect(x, y, w, h, 2, SSD1306_WHITE);
  int fw = constrain((int)((w - 2) * pct), 0, w - 2);
  if (fw > 0) display.fillRect(x + 1, y + 1, fw, h - 2, SSD1306_WHITE);
}

// ============================================================
//  DURUM ÇUBUĞU  (y: 0‒11, ayırıcı: y=12)
// ============================================================
void drawStatusBar() {
  // --- Batarya simgesi (sağ üst) ---
  drawBattIcon(110, 2, battPercent);

  // Yüzde metni
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(80, 3);
  display.printf("%3.0f%%", battPercent);

  // --- SD kart simgesi ---
  if (sdOk) {
    display.drawRect(66, 1, 9, 9, SSD1306_WHITE);
    display.drawPixel(66, 1, SSD1306_BLACK);
    display.drawPixel(67, 1, SSD1306_BLACK);
    display.drawPixel(66, 2, SSD1306_BLACK);
    display.drawLine(68, 1, 66, 3, SSD1306_WHITE);
  }

  // --- Ayırıcı çizgi ---
  display.drawLine(0, 12, 127, 12, SSD1306_WHITE);
}

// ============================================================
//  UI ÇİZİMİ
// ============================================================
void drawUI() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  drawStatusBar();

  /* ─── ANA MENÜ ─────────────────────────────────────────── */
  if (currentMenu == MAIN_MENU) {

    display.setCursor(2, 2);
    display.print(F("MENU"));

    // Scroll sınırlarını güncelle
    if (mainCursor < mainScroll) mainScroll = mainCursor;
    if (mainCursor >= mainScroll + VISIBLE_ROWS)
      mainScroll = mainCursor - VISIBLE_ROWS + 1;

    for (int i = 0; i < VISIBLE_ROWS; i++) {
      int idx = mainScroll + i;
      if (idx >= MENU_COUNT) break;
      int y   = 14 + i * 12;
      bool sel = (idx == mainCursor);

      if (sel) {
        display.fillRoundRect(0, y, 121, 12, 2, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        display.setCursor(4, y + 2);
        display.print(F(">"));
      } else {
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(4, y + 2);
        display.print(F(" "));
      }
      display.setCursor(14, y + 2);
      display.print(MENU_ITEMS[idx]);
      display.setTextColor(SSD1306_WHITE);
    }

    drawScrollBar(123, 14, VISIBLE_ROWS * 12, MENU_COUNT, VISIBLE_ROWS, mainScroll);
  }

  /* ─── ŞU AN ÇALIYOR ─────────────────────────────────────── */
  else if (currentMenu == NOW_PLAYING) {

    display.setCursor(2, 2);
    display.print(F("CALIYOR"));
    if (isShuffle) {
      display.setCursor(70, 2);
      display.print(F("[KARISIK]"));
    }

    // Şarkı adı (uzatma eki kırpılmış)
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 15);
    if (songCount > 0) {
      String name = songs[currentSong];
      if (name.endsWith(".mp3")) name = name.substring(0, name.length() - 4);
      if ((int)name.length() > 21) name = name.substring(0, 21);
      display.print(name);
    } else {
      display.print(F("-- Dosya Yok --"));
    }

    // Şarkı numarası (sağ üst)
    display.setCursor(90, 15);
    if (songCount > 0)
      display.printf("%d/%d", currentSong + 1, songCount);

    // ── Progress bar ──
    uint32_t cur   = audio.getAudioCurrentTime();
    uint32_t total = audio.getAudioFileDuration();
    float    frac  = (total > 0) ? (float)cur / (float)total : 0.0f;

    display.drawRoundRect(0, 27, 128, 7, 2, SSD1306_WHITE);
    int fw = constrain((int)(126.0f * frac), 0, 126);
    if (fw > 0) display.fillRect(1, 28, fw, 5, SSD1306_WHITE);

    // Süre etiketleri
    display.setCursor(0, 37);
    display.printf("%02d:%02d", cur / 60, cur % 60);
    if (total > 0) {
      display.setCursor(96, 37);
      display.printf("%02d:%02d", total / 60, total % 60);
    }

    // ── ▶ / ⏸ ikonu ──
    int cx = 64, cy = 54;
    if (isPlaying) drawPauseIcon(cx, cy);
    else           drawPlayIcon(cx, cy);

    // Navigasyon ipuçları
    display.setCursor(0, 57);
    display.print(F("|<"));
    display.setCursor(113, 57);
    display.print(F(">|"));
  }

  /* ─── ŞARKI LİSTESİ ─────────────────────────────────────── */
  else if (currentMenu == PLAYLIST) {

    display.setCursor(2, 2);
    display.printf("LISTE  %d sarki", songCount);

    int startIdx = max(0, playlistCursor - 2);
    if (songCount > VISIBLE_ROWS && startIdx > songCount - VISIBLE_ROWS)
      startIdx = songCount - VISIBLE_ROWS;

    for (int i = 0; i < VISIBLE_ROWS && (startIdx + i) < songCount; i++) {
      int  idx = startIdx + i;
      int  y   = 14 + i * 12;
      bool sel = (idx == playlistCursor);
      bool cur = (idx == currentSong);

      if (sel) {
        display.fillRoundRect(0, y, 121, 12, 2, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setCursor(2, y + 2);
      display.print(cur ? F("*") : F(" "));

      String name = songs[idx];
      if (name.endsWith(".mp3")) name = name.substring(0, name.length() - 4);
      if ((int)name.length() > 18) name = name.substring(0, 18);
      display.print(name);

      display.setTextColor(SSD1306_WHITE);
    }

    drawScrollBar(123, 14, VISIBLE_ROWS * 12, songCount, VISIBLE_ROWS, startIdx);
  }

  /* ─── SES AYARLARI ──────────────────────────────────────── */
  else if (currentMenu == AUDIO_SETTINGS) {

    display.setCursor(2, 2);
    display.print(F("SES AYARLARI"));

    // Seviye yazısı
    display.setCursor(0, 15);
    display.printf("Seviye: %d / 21", volume);

    // Görsel ses çubuğu
    float vPct = (float)volume / 21.0f;
    drawProgressBar(0, 26, 128, 10, vPct);

    // Yüzde etiketi ortada
    display.setCursor(52, 39);
    display.printf("%%%d", (int)(vPct * 100.0f));

    // Ses simgesi (sağda)
    // Hoparlör çizgisi
    display.drawRect(100, 16, 4, 8, SSD1306_WHITE);
    display.fillRect(100, 17, 4, 6, SSD1306_WHITE);
    // Ses dalgaları
    int bars = (int)(vPct * 3.0f) + 1; // 1..4
    for (int b = 0; b < bars && b < 4; b++) {
      int bx = 106 + b * 4;
      int bh = 4 + b * 2;
      int by = 20 - b;
      display.drawLine(bx, by + bh, bx, by, SSD1306_WHITE);
    }

    display.setCursor(0, 52);
    display.print(F("Y/A: Ses  OK: Cik"));
  }

  /* ─── BATARYA BİLGİSİ ───────────────────────────────────── */
  else if (currentMenu == BATTERY_INFO) {

    display.setCursor(2, 2);
    display.print(F("BATARYA"));

    // Büyük pil grafiği (40×16)
    display.drawRect(0, 14, 40, 16, SSD1306_WHITE);
    display.drawLine(40, 18, 40, 26, SSD1306_WHITE); // uç
    int fw = constrain((int)(38.0f * battPercent / 100.0f), 0, 38);
    if (fw > 0) display.fillRect(1, 15, fw, 14, SSD1306_WHITE);

    // Yüzde etiketi pil üstünde
    display.setTextColor((battPercent > 30) ? SSD1306_BLACK : SSD1306_WHITE);
    if (fw < 20) display.setTextColor(SSD1306_WHITE); // taşmayı önle
    display.setCursor(4, 19);
    display.printf("%.0f%%", battPercent);
    display.setTextColor(SSD1306_WHITE);

    // Voltaj + akım (sağ)
    display.setCursor(48, 15);
    display.printf("%.2f V", battVoltage);
    display.setCursor(48, 25);
    display.printf("%.1f mA", fabsf(battCurrent));

    // Durum çizgisi
    display.drawLine(0, 33, 127, 33, SSD1306_WHITE);

    // Tahmini kalan süre
    display.setCursor(0, 37);
    if (battCurrent > 5.0f) {
      // Deşarj oluyor
      float remMah = (battPercent / 100.0f) * BATT_CAPACITY;
      float hLeft  = remMah / battCurrent;
      int   hh     = (int)hLeft;
      int   mm     = (int)((hLeft - hh) * 60.0f);
      display.printf("Kalan: %dsa %ddk", hh, mm);
    } else if (battCurrent < -5.0f) {
      display.print(F("Durum: Sarj ediliyor"));
    } else {
      display.print(F("Durum: Bekleme"));
    }

    display.setCursor(0, 49);
    display.printf("Kapasite: %.0f mAh", BATT_CAPACITY);
    display.setCursor(0, 57);
    display.print(F("Guncelleme: 2 sn"));
  }

  /* ─── AYARLAR ───────────────────────────────────────────── */
  else if (currentMenu == SETTINGS) {

    display.setCursor(2, 2);
    display.print(F("AYARLAR"));

    // Karışık çalma toggle kutusu
    if (isShuffle) {
      display.fillRoundRect(0, 15, 128, 14, 3, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.drawRoundRect(0, 15, 128, 14, 3, SSD1306_WHITE);
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, 19);
    display.printf("Karisik Cal: %s", isShuffle ? "[ ACIK  ]" : "[ KAPALI]");
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 36);
    display.print(F("Y/A: Degistir"));
    display.setCursor(0, 47);
    display.print(F("Geri: Ana Menu"));
  }

  display.display();
}

// ============================================================
//  KURULUM
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed(analogRead(0));

  // Butonlar
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  // ── OLED (Wire / I2C-0) ──
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 bulunamadi!"));
  }
  display.clearDisplay();
  display.display();

  // ── INA226 (Wire1 / I2C-1) ──
  Wire1.begin(INA226_SDA, INA226_SCL);
  if (!ina226.init()) {
    Serial.println(F("INA226 bulunamadi! Adres: 0x40"));
  } else {
    ina226.setResistorRange(SHUNT_OHMS, MAX_CURRENT_A);
    ina226.setCorrectionFactor(1.0f);
    ina226.setAverage(INA226_AVERAGE_16);               // 16 örnek ortalaması
    ina226.setConversionTime(INA226_CONV_TIME_1100,     // Voltaj ölçüm süresi
                             INA226_CONV_TIME_1100);    // Akım ölçüm süresi
    Serial.println(F("INA226 hazir."));
  }
  readINA226();  // İlk ölçüm

  // ── SD Kart ──
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    sdOk = false;
    Serial.println(F("SD Kart Hatasi!"));
  } else {
    sdOk = true;
    loadSongs();
    Serial.printf("Bulunan sarki: %d\n", songCount);
  }

  // ── Ses ──
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);
  if (songCount > 0) {
    playSong(currentSong);
    isPlaying = false;
    audio.pauseResume();  // Başlangıçta duraklatılmış
  }

  requestDisplayUpdate();
}

// ============================================================
//  ANA DÖNGÜ
// ============================================================
void loop() {
  audio.loop();
  readINA226();

  // ── Otomatik sonraki şarkı ──
  if (autoNextFlag) {
    autoNextFlag = false;
    if (songCount > 0) nextSong();
  }

  // ── Now Playing periyodik güncelleme (progress bar için) ──
  if (currentMenu == NOW_PLAYING && isPlaying) {
    if (millis() - lastProgressTick >= 1000) {
      lastProgressTick = millis();
      needsUpdate = true;
    }
  }

  // ── Buton okuma + debounce (200 ms) ──
  static unsigned long lastBtnRead = 0;
  if (millis() - lastBtnRead > 200) {

    bool pressed = false;

    /* YUKARI */
    if (readBtn(BTN_UP)) {
      switch (currentMenu) {
        case MAIN_MENU:
          mainCursor = (mainCursor - 1 + MENU_COUNT) % MENU_COUNT;
          break;
        case PLAYLIST:
          playlistCursor = (playlistCursor - 1 + max(1, songCount)) % max(1, songCount);
          break;
        case NOW_PLAYING:
          if (songCount > 0) prevSong();
          break;
        case AUDIO_SETTINGS:
          if (volume < 21) { volume++; audio.setVolume(volume); }
          break;
        case SETTINGS:
          isShuffle = !isShuffle;
          break;
        default: break;
      }
      pressed = true;
    }

    /* AŞAĞI */
    if (readBtn(BTN_DOWN)) {
      switch (currentMenu) {
        case MAIN_MENU:
          mainCursor = (mainCursor + 1) % MENU_COUNT;
          break;
        case PLAYLIST:
          playlistCursor = (playlistCursor + 1) % max(1, songCount);
          break;
        case NOW_PLAYING:
          if (songCount > 0) nextSong();
          break;
        case AUDIO_SETTINGS:
          if (volume > 0) { volume--; audio.setVolume(volume); }
          break;
        case SETTINGS:
          isShuffle = !isShuffle;
          break;
        default: break;
      }
      pressed = true;
    }

    /* OK */
    if (readBtn(BTN_OK)) {
      switch (currentMenu) {
        case MAIN_MENU:
          switch (mainCursor) {
            case 0: currentMenu = NOW_PLAYING;     break;
            case 1: currentMenu = PLAYLIST;         break;
            case 2: currentMenu = AUDIO_SETTINGS;   break;
            case 3: currentMenu = BATTERY_INFO;     break;
            case 4: currentMenu = SETTINGS;         break;
            case 5:
              // Derin uyku – önce her şeyi düzgün kapat
              audio.stopSong();   // I2S + DMA tamponlarını boşalt
              delay(100);
              SD.end();           // SD kartı güvenli şekilde ayır
              SPI.end();          // SPI veri yolunu serbest bırak
              display.clearDisplay();
              display.setTextSize(1);
              display.setTextColor(SSD1306_WHITE);
              display.setCursor(28, 28);
              display.print(F("UYUYOR..."));
              display.display();
              delay(600);
              display.ssd1306_command(SSD1306_DISPLAYOFF); // OLED'i de kapat
              esp_deep_sleep_start();
              break;
          }
          break;

        case PLAYLIST:
          currentSong = playlistCursor;
          playSong(currentSong);
          currentMenu = NOW_PLAYING;
          break;

        case NOW_PLAYING:
          audio.pauseResume();
          isPlaying = !isPlaying;
          break;

        case AUDIO_SETTINGS:
          currentMenu = MAIN_MENU;
          break;

        case BATTERY_INFO:
          currentMenu = MAIN_MENU;
          break;

        default: break;
      }
      pressed = true;
    }

    /* GERİ */
    if (readBtn(BTN_BACK)) {
      if (currentMenu != MAIN_MENU) currentMenu = MAIN_MENU;
      pressed = true;
    }

    if (pressed) {
      lastBtnRead = millis();
      requestDisplayUpdate();
    }
  }

  // ── Ekranı yalnızca gerektiğinde çiz ──
  if (needsUpdate) {
    drawUI();
    needsUpdate = false;
  }
}
