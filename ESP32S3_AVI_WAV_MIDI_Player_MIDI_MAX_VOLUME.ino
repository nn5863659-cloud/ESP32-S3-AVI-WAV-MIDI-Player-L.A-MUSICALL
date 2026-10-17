// ============================================================
// ESP32-S3 AVI PLAYER + WAV MUSIC PLAYER + MIDI PLAYER
// ST7789 240x240 / microSD / MAX98357A / 6 BUTTONS
// JPEGDEC 1.8.4
//
// AVI: MJPEG 240x240 / 20 FPS; PCM 22050Hz 16-bit mono
// WAV: PCM 22050Hz 16-bit mono
// MIDI: Type 0/1, format-0/1 tracks, Note On/Off, tempo,
//       running status, up to 8 synthesized voices
//
// NOTE: AVI playback and WAV playback routines are retained
// from the supplied sketch. MIDI uses a separate synthesizer.
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <JPEGDEC.h>
#include "driver/i2s.h"
#include <math.h>

// ============================================================
// 1. TFT
// ============================================================
#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_CS   7
#define TFT_DC   8
#define TFT_RST  10
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ============================================================
// 2. SD
// ============================================================
#define SD_MISO 5
#define SD_CS   9
#define SD_SPEED 10000000

// ============================================================
// 3. MAX98357A
// ============================================================
#define I2S_PORT I2S_NUM_0
#define I2S_BCLK 12
#define I2S_LRC  13
#define I2S_DIN  14
#define MAX_SD 11
#define AUDIO_SAMPLE_RATE 22050

// ============================================================
// 4. BUTTONS
// ============================================================
#define BTN_PREV       15
#define BTN_PLAY       16
#define BTN_NEXT       17
#define BTN_STOP       18
#define BTN_VOL_DOWN    1
#define BTN_VOL_UP     21

// ============================================================
// 5. VIDEO
// ============================================================
#define VIDEO_WIDTH 240
#define VIDEO_HEIGHT 240
#define VIDEO_FPS 20
#define FRAME_TIME_MS 50

// ============================================================
// 6. AVI AUDIO OLA (UNCHANGED)
// ============================================================
#define OLA_WINDOW 882
#define OLA_HOP 441
int16_t olaInput[OLA_WINDOW];
int16_t olaAccum[OLA_WINDOW];
int16_t audioStereo[OLA_HOP * 2];
int olaInputCount = 0;

// ============================================================
// 7. BUFFERS
// ============================================================
#define JPEG_BUFFER_SIZE (96 * 1024)
uint8_t *jpegBuffer = NULL;
#define AUDIO_READ_BUFFER 2048
uint8_t audioReadBuffer[AUDIO_READ_BUFFER];

// ============================================================
// 8. AVI FILES
// ============================================================
#define MAX_FILES 50
String aviFiles[MAX_FILES];
int fileCount = 0;
int selectedFile = 0;

// ============================================================
// 9. WAV FILES
// ============================================================
#define MAX_MUSIC_FILES 50
String musicFiles[MAX_MUSIC_FILES];
int musicFileCount = 0;
int selectedMusicFile = 0;

// ============================================================
// 10. MIDI FILES / SYNTH
// ============================================================
#define MAX_MIDI_FILES 50
#define MIDI_MAX_VOICES 8
#define MIDI_BLOCK_SAMPLES 128

String midiFiles[MAX_MIDI_FILES];
int midiFileCount = 0;
int selectedMidiFile = 0;

struct MIDIVoice {
  bool active;
  uint8_t note;
  uint8_t velocity;
  uint8_t channel;
  float phase;
  float phaseStep;
};
MIDIVoice midiVoices[MIDI_MAX_VOICES];
int16_t midiMono[MIDI_BLOCK_SAMPLES];
int16_t midiStereo[MIDI_BLOCK_SAMPLES * 2];

// ============================================================
// 11. PLAYER MODE
// ============================================================
enum PlayerMode {
  MODE_MENU_AVI,
  MODE_MENU_MUSIC,
  MODE_MENU_MIDI,
  MODE_PLAY_AVI,
  MODE_PLAY_MUSIC,
  MODE_PLAY_MIDI
};
PlayerMode playerMode = MODE_MENU_AVI;

// ============================================================
// 12. EVENTS
// ============================================================
bool stopRequested = false;
bool pauseRequested = false;
bool nextRequested = false;
bool prevRequested = false;
bool playRequested = false;

// ============================================================
// 13. VOLUME
// ============================================================
int volumeLevel = 80;

// ============================================================
// 14. BUTTON STATE
// ============================================================
bool oldPrevState = HIGH;
bool oldPlayState = HIGH;
bool oldNextState = HIGH;
bool oldStopState = HIGH;
bool oldVolDownState = HIGH;
bool oldVolUpState = HIGH;

// ============================================================
// 15. DEBOUNCE
// ============================================================
uint32_t lastPrevTime = 0;
uint32_t lastPlayTime = 0;
uint32_t lastNextTime = 0;
uint32_t lastStopTime = 0;
uint32_t lastVolDownTime = 0;
uint32_t lastVolUpTime = 0;
#define BUTTON_DEBOUNCE 100

// ============================================================
// 16. GLOBALS
// ============================================================
JPEGDEC jpeg;
bool volumeDisplayRequest = false;

// ============================================================
// 17. POLL BUTTONS
// ============================================================
void pollButtons() {
  uint32_t now = millis();
  bool s = digitalRead(BTN_PREV);
  if (s == LOW && oldPrevState == HIGH &&
      now - lastPrevTime >= BUTTON_DEBOUNCE) {
    prevRequested = true; lastPrevTime = now;
  }
  oldPrevState = s;

  s = digitalRead(BTN_PLAY);
  if (s == LOW && oldPlayState == HIGH &&
      now - lastPlayTime >= BUTTON_DEBOUNCE) {
    playRequested = true; lastPlayTime = now;
  }
  oldPlayState = s;

  s = digitalRead(BTN_NEXT);
  if (s == LOW && oldNextState == HIGH &&
      now - lastNextTime >= BUTTON_DEBOUNCE) {
    nextRequested = true; lastNextTime = now;
  }
  oldNextState = s;

  s = digitalRead(BTN_STOP);
  if (s == LOW && oldStopState == HIGH &&
      now - lastStopTime >= BUTTON_DEBOUNCE) {
    stopRequested = true; lastStopTime = now;
  }
  oldStopState = s;

  s = digitalRead(BTN_VOL_DOWN);
  if (s == LOW && oldVolDownState == HIGH &&
      now - lastVolDownTime >= BUTTON_DEBOUNCE) {
    volumeLevel = (volumeLevel >= 5) ? volumeLevel - 5 : 0;
    volumeDisplayRequest = true;
    Serial.printf("VOL DOWN -> %d\n", volumeLevel);
    lastVolDownTime = now;
  }
  oldVolDownState = s;

  s = digitalRead(BTN_VOL_UP);
  if (s == LOW && oldVolUpState == HIGH &&
      now - lastVolUpTime >= BUTTON_DEBOUNCE) {
    volumeLevel = (volumeLevel <= 95) ? volumeLevel + 5 : 100;
    volumeDisplayRequest = true;
    Serial.printf("VOL UP -> %d\n", volumeLevel);
    lastVolUpTime = now;
  }
  oldVolUpState = s;
}

// ============================================================
// 18. WAIT WHILE PAUSED
// ============================================================
bool waitWhilePaused() {
  while (pauseRequested && !stopRequested) {
    pollButtons();
    if (playRequested) {
      playRequested = false;
      pauseRequested = false;
      tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
      tft.setCursor(5, 200);
      tft.print("PLAYING         ");
    }
    if (stopRequested) return false;
    if (volumeDisplayRequest) {
      volumeDisplayRequest = false;
      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(5, 35);
      tft.printf("VOL %d%%   ", volumeLevel);
    }
    delay(5);
  }
  return !stopRequested;
}

// ============================================================
// 19. JPEG DRAW
// ============================================================
int JPEGDraw(JPEGDRAW *pDraw) {
  pollButtons();
  if (stopRequested || pDraw == NULL || pDraw->pPixels == NULL) return 0;
  int x = pDraw->x, y = pDraw->y, w = pDraw->iWidth, h = pDraw->iHeight;
  if (x < 0 || y < 0 || x >= VIDEO_WIDTH || y >= VIDEO_HEIGHT) return 1;
  if (x + w > VIDEO_WIDTH) w = VIDEO_WIDTH - x;
  if (y + h > VIDEO_HEIGHT) h = VIDEO_HEIGHT - y;
  if (w <= 0 || h <= 0) return 1;
  tft.drawRGBBitmap(x, y, pDraw->pPixels, w, h);
  return 1;
}

// ============================================================
// 20. READ HELPERS
// ============================================================
uint32_t readLE32(File &file) {
  uint8_t b[4];
  if (file.readBytes((char *)b, 4) != 4) return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
uint16_t readLE16(File &file) {
  uint8_t b[2];
  if (file.readBytes((char *)b, 2) != 2) return 0;
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
bool idEquals(const uint8_t *id, char a, char b, char c, char d) {
  return id[0] == a && id[1] == b && id[2] == c && id[3] == d;
}

// ============================================================
// 21. I2S SETUP
// ============================================================
void setupI2S() {
  Serial.println("I2S init...");
  i2s_config_t config;
  memset(&config, 0, sizeof(config));
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = AUDIO_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;
  esp_err_t err = i2s_driver_install(I2S_PORT, &config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S driver error: %d\n", (int)err);
    return;
  }
  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRC;
  pins.data_out_num = I2S_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("I2S pin error: %d\n", (int)err);
    return;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  pinMode(MAX_SD, OUTPUT);
  digitalWrite(MAX_SD, HIGH);
  Serial.println("I2S OK");
}

// ============================================================
// 22. AUDIO RESET / VOLUME
// ============================================================
void audioReset() {
  memset(olaInput, 0, sizeof(olaInput));
  memset(olaAccum, 0, sizeof(olaAccum));
  memset(audioStereo, 0, sizeof(audioStereo));
  olaInputCount = 0;
  i2s_zero_dma_buffer(I2S_PORT);
}
void updateVolumeDisplay() {
  if (!volumeDisplayRequest) return;
  volumeDisplayRequest = false;
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(5, 35);
  tft.printf("VOL %d%%   ", volumeLevel);
}

// ============================================================
// 23. AUDIO OUTPUT / OLA (AVI + WAV PATH RETAINED)
// ============================================================
void audioWriteMono(const int16_t *samples, int count) {
  if (count <= 0 || stopRequested || !waitWhilePaused()) return;
  pollButtons();
  updateVolumeDisplay();
  for (int i = 0; i < count; i++) {
    int32_t s = (int32_t)samples[i] * volumeLevel / 100;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    audioStereo[i * 2] = (int16_t)s;
    audioStereo[i * 2 + 1] = (int16_t)s;
  }
  size_t written = 0;
  i2s_write(I2S_PORT, audioStereo, count * 4, &written, pdMS_TO_TICKS(100));
  pollButtons();
}
void processOLA() {
  if (stopRequested) return;
  for (int i = 0; i < OLA_WINDOW; i++) {
    float phase = (2.0f * PI * i) / (OLA_WINDOW - 1);
    float window = 0.5f - 0.5f * cosf(phase);
    int32_t v = (int32_t)((float)olaInput[i] * window);
    int32_t sum = (int32_t)olaAccum[i] + v;
    if (sum > 32767) sum = 32767;
    if (sum < -32768) sum = -32768;
    olaAccum[i] = (int16_t)sum;
  }
  audioWriteMono(olaAccum, OLA_HOP);
  memmove(olaAccum, olaAccum + OLA_HOP, (OLA_WINDOW - OLA_HOP) * sizeof(int16_t));
  memset(olaAccum + (OLA_WINDOW - OLA_HOP), 0, OLA_HOP * sizeof(int16_t));
  memmove(olaInput, olaInput + OLA_HOP, (OLA_WINDOW - OLA_HOP) * sizeof(int16_t));
  olaInputCount = OLA_WINDOW - OLA_HOP;
}
void audioFeedSamples(const int16_t *samples, int count) {
  int pos = 0;
  while (pos < count && !stopRequested) {
    pollButtons();
    if (!waitWhilePaused()) return;
    updateVolumeDisplay();
    int room = OLA_WINDOW - olaInputCount;
    int n = count - pos;
    if (n > room) n = room;
    memcpy(olaInput + olaInputCount, samples + pos, n * sizeof(int16_t));
    olaInputCount += n;
    pos += n;
    if (olaInputCount >= OLA_WINDOW) processOLA();
  }
}

// ============================================================
// 24. AVI AUDIO CHUNK (RETAINED)
// ============================================================
bool playAudioChunk(File &file, uint32_t dataPos, uint32_t chunkSize) {
  if (stopRequested) return false;
  if (chunkSize == 0) return true;
  file.seek(dataPos);
  uint32_t remaining = chunkSize;
  while (remaining > 0 && !stopRequested) {
    pollButtons();
    if (!waitWhilePaused()) return false;
    updateVolumeDisplay();
    uint32_t n = remaining;
    if (n > AUDIO_READ_BUFFER) n = AUDIO_READ_BUFFER;
    size_t got = file.readBytes((char *)audioReadBuffer, n);
    if (got == 0) return false;
    int samples = got / 2;
    int16_t *pcm = (int16_t *)audioReadBuffer;
    audioFeedSamples(pcm, samples);
    remaining -= got;
  }
  return !stopRequested;
}

// ============================================================
// 25. AVI VIDEO CHUNK (RETAINED)
// ============================================================
bool playVideoChunk(File &file, uint32_t dataPos, uint32_t chunkSize) {
  if (stopRequested) return false;
  if (chunkSize == 0) return true;
  if (chunkSize > JPEG_BUFFER_SIZE) {
    Serial.printf("JPEG qua lon: %u\n", chunkSize);
    file.seek(dataPos + chunkSize);
    return true;
  }
  file.seek(dataPos);
  size_t got = file.readBytes((char *)jpegBuffer, chunkSize);
  if (got != chunkSize) {
    Serial.println("Doc JPEG loi");
    return false;
  }
  pollButtons();
  if (stopRequested || !waitWhilePaused()) return false;
  int opened = jpeg.openRAM(jpegBuffer, got, JPEGDraw);
  if (!opened) {
    Serial.printf("JPEG open loi: %d\n", jpeg.getLastError());
    return true;
  }
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  int result = jpeg.decode(0, 0, 0);
  jpeg.close();
  if (!result) Serial.printf("JPEG decode loi: %d\n", jpeg.getLastError());
  return true;
}

// ============================================================
// 26. FIND MOVI (RETAINED)
// ============================================================
bool findMovi(File &file, uint32_t &moviStart, uint32_t &moviEnd) {
  file.seek(0);
  uint8_t riff[4];
  if (file.readBytes((char *)riff, 4) != 4 || !idEquals(riff, 'R','I','F','F')) return false;
  readLE32(file);
  uint8_t aviId[4];
  if (file.readBytes((char *)aviId, 4) != 4 || !idEquals(aviId, 'A','V','I',' ')) return false;
  uint32_t fileSize = file.size();
  while (file.position() + 8 <= fileSize) {
    uint8_t id[4];
    if (file.readBytes((char *)id, 4) != 4) break;
    uint32_t size = readLE32(file);
    uint32_t dataStart = file.position();
    uint32_t dataEnd = dataStart + size;
    if (dataEnd > fileSize) dataEnd = fileSize;
    if (idEquals(id, 'L','I','S','T')) {
      if (size >= 4) {
        uint8_t listType[4];
        file.readBytes((char *)listType, 4);
        if (idEquals(listType, 'm','o','v','i')) {
          moviStart = dataStart + 4;
          moviEnd = dataStart + size;
          if (moviEnd > fileSize) moviEnd = fileSize;
          return true;
        }
      }
    }
    uint32_t next = dataStart + size;
    if (next & 1) next++;
    if (next <= dataStart) break;
    file.seek(next);
  }
  return false;
}

// ============================================================
// 27. PLAY MOVI RANGE (RETAINED)
// ============================================================
bool playMoviRange(File &file, uint32_t startPos, uint32_t endPos) {
  file.seek(startPos);
  while (file.position() + 8 <= endPos) {
    pollButtons();
    if (stopRequested || !waitWhilePaused()) return false;
    uint8_t id[4];
    if (file.readBytes((char *)id, 4) != 4) return false;
    uint32_t size = readLE32(file);
    uint32_t dataStart = file.position();
    uint32_t dataEnd = dataStart + size;
    if (dataEnd > endPos) dataEnd = endPos;

    if (idEquals(id, 'L','I','S','T') || idEquals(id, 'r','e','c',' ')) {
      if (size >= 4) {
        file.seek(dataStart + 4);
        if (!playMoviRange(file, dataStart + 4, dataEnd)) return false;
      }
    } else if (idEquals(id, '0','0','d','c')) {
      uint32_t frameStart = millis();
      if (!playVideoChunk(file, dataStart, size)) return false;
      uint32_t elapsed = millis() - frameStart;
      if (elapsed < FRAME_TIME_MS) {
        uint32_t waitTime = FRAME_TIME_MS - elapsed;
        uint32_t waitStart = millis();
        while (millis() - waitStart < waitTime) {
          pollButtons(); updateVolumeDisplay();
          if (stopRequested || !waitWhilePaused()) return false;
          delay(1);
        }
      }
    } else if (idEquals(id, '0','1','w','b')) {
      if (!playAudioChunk(file, dataStart, size)) return false;
    }

    if (file.position() < dataEnd) file.seek(dataEnd);
    if (file.position() & 1) file.seek(file.position() + 1);
  }
  return true;
}

// ============================================================
// 28. PLAY AVI (RETAINED)
// ============================================================
bool playAVI(const char *filename) {
  Serial.printf("PLAY: %s\n", filename);
  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.println("Khong mo duoc AVI");
    return false;
  }
  uint32_t moviStart = 0, moviEnd = 0;
  if (!findMovi(file, moviStart, moviEnd)) {
    Serial.println("Khong tim thay MOVI");
    file.close();
    return false;
  }
  Serial.printf("MOVI START: %u\nMOVI END: %u\n", moviStart, moviEnd);
  audioReset();
  tft.fillScreen(ST77XX_BLACK);
  bool result = playMoviRange(file, moviStart, moviEnd);
  file.close();
  i2s_zero_dma_buffer(I2S_PORT);
  return result;
}

// ============================================================
// 29. SCAN FILE LISTS
// ============================================================
void scanAVI() {
  fileCount = 0;
  File root = SD.open("/");
  if (!root) { Serial.println("Khong mo duoc root"); return; }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = entry.name(), lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".avi") && fileCount < MAX_FILES) {
        if (!name.startsWith("/")) name = "/" + name;
        aviFiles[fileCount++] = name;
        Serial.printf("AVI: %s\n", name.c_str());
      }
    }
    entry.close();
  }
  root.close();
  Serial.printf("TOTAL AVI: %d\n", fileCount);
}
void scanMusic() {
  musicFileCount = 0;
  File root = SD.open("/");
  if (!root) { Serial.println("Khong mo duoc root"); return; }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = entry.name(), lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".wav") && musicFileCount < MAX_MUSIC_FILES) {
        if (!name.startsWith("/")) name = "/" + name;
        musicFiles[musicFileCount++] = name;
        Serial.printf("WAV: %s\n", name.c_str());
      }
    }
    entry.close();
  }
  root.close();
  Serial.printf("TOTAL WAV: %d\n", musicFileCount);
}
void scanMIDI() {
  midiFileCount = 0;
  File root = SD.open("/");
  if (!root) { Serial.println("MIDI: root open failed"); return; }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = entry.name(), lower = name;
      lower.toLowerCase();
      if ((lower.endsWith(".mid") || lower.endsWith(".midi")) &&
          midiFileCount < MAX_MIDI_FILES) {
        if (!name.startsWith("/")) name = "/" + name;
        midiFiles[midiFileCount++] = name;
        Serial.printf("MIDI: %s\n", name.c_str());
      }
    }
    entry.close();
  }
  root.close();
  Serial.printf("TOTAL MIDI: %d\n", midiFileCount);
}

// ============================================================
// 30. MENUS
// ============================================================
void drawFileMenu(const char *title, const char *emptyText,
                  String *files, int count, int selected,
                  uint16_t titleColor, const char *footer) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(titleColor);
  tft.setCursor(8, 5);
  tft.println(title);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 30);
  tft.printf("FILES: %d", count);
  tft.setCursor(125, 30);
  tft.printf("VOL: %d%%", volumeLevel);

  if (count == 0) {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_RED);
    tft.setCursor(15, 100);
    tft.println(emptyText);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(15, 135);
    tft.println("Put files into SD root");
    tft.setCursor(15, 160);
    tft.println(footer);
    return;
  }

  int first = selected - 3;
  if (first < 0) first = 0;
  if (first > count - 7) first = count - 7;
  if (first < 0) first = 0;
  for (int i = 0; i < 7; i++) {
    int idx = first + i;
    if (idx >= count) break;
    int y = 55 + i * 23;
    if (idx == selected) tft.fillRect(3, y - 2, 234, 20, ST77XX_BLUE);
    String name = files[idx];
    if (name.startsWith("/")) name = name.substring(1);
    if (name.length() > 27) name = name.substring(0, 27);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(7, y);
    tft.printf("%d. ", idx + 1);
    tft.print(name);
  }
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 225);
  tft.print(footer);
}
void showMenu() {
  drawFileMenu("AVI PLAYER", "NO AVI FILE", aviFiles, fileCount,
               selectedFile, ST77XX_CYAN, "PREV/NEXT PLAY STOP=MUSIC");
}
void showMusicMenu() {
  drawFileMenu("MUSIC PLAYER", "NO WAV FILE", musicFiles, musicFileCount,
               selectedMusicFile, ST77XX_MAGENTA, "PREV/NEXT PLAY STOP=MIDI");
}
void showMIDIMenu() {
  drawFileMenu("MIDI PLAYER", "NO MIDI FILE", midiFiles, midiFileCount,
               selectedMidiFile, ST77XX_YELLOW, "PREV/NEXT PLAY STOP=AVI");
}
void showPlayingScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(5, 5); tft.println("PLAYING AVI");
  String name = aviFiles[selectedFile];
  if (name.startsWith("/")) name = name.substring(1);
  if (name.length() > 32) name = name.substring(0, 32);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 20); tft.println(name);
  tft.setCursor(5, 35); tft.printf("VOL %d%%", volumeLevel);
  tft.setCursor(5, 200); tft.setTextColor(ST77XX_GREEN); tft.print("PLAYING");
}
void showMusicPlayingScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setCursor(5, 5); tft.println("PLAYING MUSIC");
  String name = musicFiles[selectedMusicFile];
  if (name.startsWith("/")) name = name.substring(1);
  if (name.length() > 32) name = name.substring(0, 32);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 20); tft.println(name);
  tft.setCursor(5, 35); tft.printf("VOL %d%%", volumeLevel);
  tft.setCursor(5, 200); tft.setTextColor(ST77XX_GREEN); tft.print("PLAYING");
}
void showMIDIPlayingScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 5); tft.println("PLAYING MIDI");
  String name = midiFiles[selectedMidiFile];
  if (name.startsWith("/")) name = name.substring(1);
  if (name.length() > 32) name = name.substring(0, 32);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 20); tft.println(name);
  tft.setCursor(5, 35); tft.printf("VOL %d%%", volumeLevel);
  tft.setCursor(5, 200); tft.setTextColor(ST77XX_GREEN); tft.print("PLAYING");
}

// ============================================================
// 31. MENU BUTTON PROCESSING
// STOP cycles AVI -> WAV -> MIDI -> AVI
// ============================================================
void processMenu() {
  pollButtons();
  if (prevRequested) {
    prevRequested = false;
    if (fileCount > 0) { selectedFile = (selectedFile - 1 + fileCount) % fileCount; showMenu(); }
  }
  if (nextRequested) {
    nextRequested = false;
    if (fileCount > 0) { selectedFile = (selectedFile + 1) % fileCount; showMenu(); }
  }
  if (volumeDisplayRequest) { volumeDisplayRequest = false; showMenu(); }
  if (playRequested) {
    playRequested = false;
    if (fileCount > 0) playerMode = MODE_PLAY_AVI;
  }
  if (stopRequested) {
    stopRequested = false; nextRequested = prevRequested = playRequested = false;
    playerMode = MODE_MENU_MUSIC; showMusicMenu();
  }
}
void processMusicMenu() {
  pollButtons();
  if (prevRequested) {
    prevRequested = false;
    if (musicFileCount > 0) { selectedMusicFile = (selectedMusicFile - 1 + musicFileCount) % musicFileCount; showMusicMenu(); }
  }
  if (nextRequested) {
    nextRequested = false;
    if (musicFileCount > 0) { selectedMusicFile = (selectedMusicFile + 1) % musicFileCount; showMusicMenu(); }
  }
  if (volumeDisplayRequest) { volumeDisplayRequest = false; showMusicMenu(); }
  if (playRequested) {
    playRequested = false;
    if (musicFileCount > 0) playerMode = MODE_PLAY_MUSIC;
  }
  if (stopRequested) {
    stopRequested = false; nextRequested = prevRequested = playRequested = false;
    playerMode = MODE_MENU_MIDI; showMIDIMenu();
  }
}
void processMIDIMenu() {
  pollButtons();
  if (prevRequested) {
    prevRequested = false;
    if (midiFileCount > 0) { selectedMidiFile = (selectedMidiFile - 1 + midiFileCount) % midiFileCount; showMIDIMenu(); }
  }
  if (nextRequested) {
    nextRequested = false;
    if (midiFileCount > 0) { selectedMidiFile = (selectedMidiFile + 1) % midiFileCount; showMIDIMenu(); }
  }
  if (volumeDisplayRequest) { volumeDisplayRequest = false; showMIDIMenu(); }
  if (playRequested) {
    playRequested = false;
    if (midiFileCount > 0) playerMode = MODE_PLAY_MIDI;
  }
  if (stopRequested) {
    stopRequested = false; nextRequested = prevRequested = playRequested = false;
    playerMode = MODE_MENU_AVI; showMenu();
  }
}

// ============================================================
// 32. WAV PARSER / PLAYER (RETAINED)
// ============================================================
bool findWavData(File &file, uint32_t &dataStart, uint32_t &dataSize) {
  file.seek(0);
  uint8_t riff[4];
  if (file.readBytes((char *)riff, 4) != 4 || !idEquals(riff, 'R','I','F','F')) {
    Serial.println("WAV: KHONG PHAI RIFF"); return false;
  }
  readLE32(file);
  uint8_t wave[4];
  if (file.readBytes((char *)wave, 4) != 4 || !idEquals(wave, 'W','A','V','E')) {
    Serial.println("WAV: KHONG PHAI WAVE"); return false;
  }
  bool fmtFound = false, dataFound = false;
  uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0, fileSize = file.size();
  while (file.position() + 8 <= fileSize) {
    uint8_t id[4];
    if (file.readBytes((char *)id, 4) != 4) break;
    uint32_t size = readLE32(file), chunkStart = file.position();
    uint32_t chunkEnd = chunkStart + size;
    if (chunkEnd > fileSize) chunkEnd = fileSize;
    if (idEquals(id, 'f','m','t',' ')) {
      if (size < 16) return false;
      audioFormat = readLE16(file);
      channels = readLE16(file);
      sampleRate = readLE32(file);
      readLE32(file); readLE16(file);
      bitsPerSample = readLE16(file);
      fmtFound = true;
    } else if (idEquals(id, 'd','a','t','a')) {
      dataStart = chunkStart; dataSize = size;
      if (dataStart + dataSize > fileSize) dataSize = fileSize - dataStart;
      dataFound = true;
    }
    uint32_t next = chunkStart + size;
    if (next & 1) next++;
    if (next <= chunkStart) break;
    file.seek(next);
    if (fmtFound && dataFound) break;
  }
  Serial.printf("WAV FORMAT: %u\nWAV CHANNELS: %u\nWAV RATE: %u\nWAV BITS: %u\n",
                audioFormat, channels, sampleRate, bitsPerSample);
  if (!fmtFound || !dataFound || audioFormat != 1 || channels != 1 ||
      sampleRate != AUDIO_SAMPLE_RATE || bitsPerSample != 16) {
    Serial.println("WAV FORMAT KHONG HO TRO"); return false;
  }
  return true;
}
bool playWAV(const char *filename) {
  Serial.printf("PLAY WAV: %s\n", filename);
  File file = SD.open(filename, FILE_READ);
  if (!file) { Serial.println("Khong mo duoc WAV"); return false; }
  uint32_t dataStart = 0, dataSize = 0;
  if (!findWavData(file, dataStart, dataSize)) { file.close(); return false; }
  Serial.printf("WAV DATA START: %u\nWAV DATA SIZE: %u\n", dataStart, dataSize);
  audioReset();
  tft.fillScreen(ST77XX_BLACK);
  showMusicPlayingScreen();
  file.seek(dataStart);
  uint32_t remaining = dataSize;
  while (remaining > 0 && !stopRequested) {
    pollButtons();
    if (!waitWhilePaused()) { file.close(); i2s_zero_dma_buffer(I2S_PORT); return false; }
    updateVolumeDisplay();
    if (nextRequested || prevRequested) { file.close(); i2s_zero_dma_buffer(I2S_PORT); return true; }
    uint32_t n = remaining;
    if (n > AUDIO_READ_BUFFER) n = AUDIO_READ_BUFFER;
    n &= ~1UL;
    if (n == 0) break;
    size_t got = file.readBytes((char *)audioReadBuffer, n);
    if (got == 0) { Serial.println("WAV READ ERROR"); break; }
    audioFeedSamples((int16_t *)audioReadBuffer, got / 2);
    remaining -= got;
    if (nextRequested || prevRequested) break;
  }
  if (!stopRequested && !nextRequested && !prevRequested && olaInputCount > 0) {
    while (olaInputCount < OLA_WINDOW) { olaInput[olaInputCount] = 0; olaInputCount++; }
    processOLA();
  }
  file.close();
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}
void processPlayButtons() {
  pollButtons();
  if (playRequested) {
    playRequested = false;
    pauseRequested = !pauseRequested;
    tft.setTextColor(pauseRequested ? ST77XX_YELLOW : ST77XX_GREEN, ST77XX_BLACK);
    tft.setCursor(5, 200);
    tft.print(pauseRequested ? "PAUSED          " : "PLAYING         ");
  }
  updateVolumeDisplay();
}
void processMusicPlayButtons() { processPlayButtons(); }

// ============================================================
// 33. MIDI READER HELPERS
// ============================================================
uint16_t readBE16(File &f) {
  int a = f.read(), b = f.read();
  if (a < 0 || b < 0) return 0;
  return ((uint16_t)a << 8) | (uint16_t)b;
}
uint32_t readBE32(File &f) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    int b = f.read();
    if (b < 0) return 0;
    v = (v << 8) | (uint8_t)b;
  }
  return v;
}
uint32_t readMIDIvar(File &f, bool &ok) {
  uint32_t value = 0;
  ok = false;
  for (int i = 0; i < 4; i++) {
    int c = f.read();
    if (c < 0) return 0;
    value = (value << 7) | (uint32_t)(c & 0x7F);
    if (!(c & 0x80)) { ok = true; return value; }
  }
  return value;
}
void silenceMIDIVoices() {
  for (int i = 0; i < MIDI_MAX_VOICES; i++) midiVoices[i].active = false;
}
float midiNoteFrequency(uint8_t note) {
  return 440.0f * powf(2.0f, ((int)note - 69) / 12.0f);
}
void midiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (velocity == 0) {
    for (int i = 0; i < MIDI_MAX_VOICES; i++)
      if (midiVoices[i].active && midiVoices[i].channel == channel && midiVoices[i].note == note)
        midiVoices[i].active = false;
    return;
  }
  int slot = -1;
  for (int i = 0; i < MIDI_MAX_VOICES; i++) {
    if (midiVoices[i].active && midiVoices[i].channel == channel && midiVoices[i].note == note) { slot = i; break; }
    if (!midiVoices[i].active && slot < 0) slot = i;
  }
  if (slot < 0) slot = 0; // voice stealing when all 8 are occupied
  midiVoices[slot].active = true;
  midiVoices[slot].note = note;
  midiVoices[slot].velocity = velocity;
  midiVoices[slot].channel = channel;
  midiVoices[slot].phase = 0.0f;
  midiVoices[slot].phaseStep = 2.0f * PI * midiNoteFrequency(note) / AUDIO_SAMPLE_RATE;
}
void midiNoteOff(uint8_t channel, uint8_t note) {
  for (int i = 0; i < MIDI_MAX_VOICES; i++)
    if (midiVoices[i].active && midiVoices[i].channel == channel && midiVoices[i].note == note)
      midiVoices[i].active = false;
}
void renderMIDIAudio(uint32_t samples) {
  while (samples > 0 && !stopRequested) {
    pollButtons();
    if (!waitWhilePaused()) return;
    updateVolumeDisplay();
    uint32_t n = samples > MIDI_BLOCK_SAMPLES ? MIDI_BLOCK_SAMPLES : samples;
    for (uint32_t j = 0; j < n; j++) {
      float mix = 0.0f;
      int activeCount = 0;
      for (int v = 0; v < MIDI_MAX_VOICES; v++) {
        if (!midiVoices[v].active) continue;
        float wave = sinf(midiVoices[v].phase);
        float gain = (midiVoices[v].velocity / 127.0f) * 30000.0f; // MIDI gain near digital maximum
        mix += wave * gain;
        midiVoices[v].phase += midiVoices[v].phaseStep;
        if (midiVoices[v].phase >= 2.0f * PI) midiVoices[v].phase -= 2.0f * PI;
        activeCount++;
      }
      if (activeCount > 1) mix /= sqrtf((float)activeCount);
      mix *= volumeLevel / 100.0f;
      if (mix > 32767) mix = 32767;
      if (mix < -32768) mix = -32768;
      midiMono[j] = (int16_t)mix;
      midiStereo[j * 2] = midiMono[j];
      midiStereo[j * 2 + 1] = midiMono[j];
    }
    size_t written = 0;
    i2s_write(I2S_PORT, midiStereo, n * 4, &written, portMAX_DELAY);
    samples -= n;
  }
}

// ============================================================
// 34. PLAY MIDI
// Supports SMF type 0/1, tracks merged by absolute tick time.
// MIDI synth is sine-wave, 8 voices, no GM instrument samples.
// ============================================================
bool playMIDI(const char *filename) {
  Serial.printf("PLAY MIDI: %s\n", filename);
  File f = SD.open(filename, FILE_READ);
  if (!f) { Serial.println("Cannot open MIDI"); return false; }

  uint8_t hdr[4];
  if (f.readBytes((char *)hdr, 4) != 4 || !idEquals(hdr, 'M','T','h','d')) {
    Serial.println("MIDI: invalid MThd"); f.close(); return false;
  }
  uint32_t headerLen = readBE32(f);
  if (headerLen < 6) { f.close(); return false; }
  uint16_t format = readBE16(f);
  uint16_t trackCount = readBE16(f);
  uint16_t division = readBE16(f);
  if (headerLen > 6) f.seek(f.position() + headerLen - 6);
  if (format > 1 || trackCount == 0 || division == 0 || (division & 0x8000)) {
    Serial.println("MIDI: unsupported format/time division"); f.close(); return false;
  }
  if (trackCount > 16) {
    Serial.println("MIDI: max 16 tracks supported"); f.close(); return false;
  }

  struct TrackState {
    uint32_t pos;
    uint32_t end;
    uint64_t nextTick;
    uint8_t runningStatus;
    bool active;
    uint8_t eventStatus;
    uint8_t data1;
    uint8_t data2;
    uint8_t dataCount;
    uint32_t tempo;
    uint32_t eventTempo;
  };
  TrackState tr[16];
  memset(tr, 0, sizeof(tr));
  uint32_t foundTracks = 0;

  // Locate tracks and prime their first delta/event.
  while (f.position() + 8 <= f.size() && foundTracks < trackCount) {
    uint8_t id[4];
    if (f.readBytes((char *)id, 4) != 4) break;
    uint32_t len = readBE32(f);
    uint32_t start = f.position();
    uint32_t end = start + len;
    if (end > f.size()) { f.close(); return false; }
    if (idEquals(id, 'M','T','r','k')) {
      TrackState &t = tr[foundTracks++];
      t.pos = start; t.end = end; t.active = true; t.runningStatus = 0; t.tempo = 500000;
    }
    f.seek(end);
  }
  if (foundTracks == 0) { Serial.println("MIDI: no tracks"); f.close(); return false; }

  // Per-track event parser, scheduling next event using absolute ticks.
  auto loadNextEvent = [&](int ti) -> bool {
    TrackState &t = tr[ti];
    if (!t.active || t.pos >= t.end) { t.active = false; return false; }
    f.seek(t.pos);
    bool ok = false;
    uint32_t delta = readMIDIvar(f, ok);
    if (!ok || f.position() >= t.end) { t.active = false; return false; }
    t.nextTick += delta;
    int first = f.read();
    if (first < 0) { t.active = false; return false; }
    uint8_t status;
    int d1 = -1;
    if (first & 0x80) {
      status = (uint8_t)first;
      if (status < 0xF0) t.runningStatus = status;
    } else {
      if (t.runningStatus == 0) { t.active = false; return false; }
      status = t.runningStatus;
      d1 = first;
    }
    t.eventStatus = status;
    t.eventTempo = 0;
    t.data1 = 0; t.data2 = 0; t.dataCount = 0;
    if (status == 0xFF) {
      int type = f.read();
      if (type < 0) { t.active = false; return false; }
      uint32_t mlen = readMIDIvar(f, ok);
      if (!ok || f.position() + mlen > t.end) { t.active = false; return false; }
      if (type == 0x51 && mlen == 3) {
        uint8_t b[3];
        f.readBytes((char *)b, 3);
        t.tempo = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        t.eventTempo = t.tempo;
      } else if (type == 0x2F) {
        t.active = false;
      } else {
        f.seek(f.position() + mlen);
      }
    } else if (status == 0xF0 || status == 0xF7) {
      uint32_t syxLen = readMIDIvar(f, ok);
      if (!ok || f.position() + syxLen > t.end) { t.active = false; return false; }
      f.seek(f.position() + syxLen);
    } else {
      uint8_t hi = status & 0xF0;
      int need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      if (d1 >= 0) {
        t.data1 = (uint8_t)d1; t.dataCount = 1;
      } else {
        int b = f.read();
        if (b < 0) { t.active = false; return false; }
        t.data1 = (uint8_t)b; t.dataCount = 1;
      }
      if (need == 2) {
        int b = f.read();
        if (b < 0) { t.active = false; return false; }
        t.data2 = (uint8_t)b; t.dataCount = 2;
      }
    }
    t.pos = f.position();
    return true;
  };

  for (uint32_t i = 0; i < foundTracks; i++) loadNextEvent(i);
  for (int i = 0; i < MIDI_MAX_VOICES; i++) midiVoices[i].active = false;

  audioReset();
  tft.fillScreen(ST77XX_BLACK);
  showMIDIPlayingScreen();

  uint64_t currentTick = 0;
  uint32_t currentTempo = 500000;
  uint64_t fractionalMicros = 0;
  uint32_t safetyEvents = 0;

  while (!stopRequested && safetyEvents < 2000000UL) {
    pollButtons();
    if (!waitWhilePaused()) break;
    updateVolumeDisplay();

    int best = -1;
    uint64_t nextTick = UINT64_MAX;
    for (uint32_t i = 0; i < foundTracks; i++) {
      if (tr[i].active && tr[i].nextTick < nextTick) {
        nextTick = tr[i].nextTick; best = (int)i;
      }
    }
    if (best < 0) break;

    // Render elapsed time until next event. Use current tempo and PPQN.
    if (nextTick > currentTick) {
      uint64_t deltaTicks = nextTick - currentTick;
      uint64_t us = (deltaTicks * currentTempo) / division;
      uint64_t totalUs = us + fractionalMicros;
      uint64_t sampleCount = (totalUs * AUDIO_SAMPLE_RATE) / 1000000ULL;
      fractionalMicros = totalUs - (sampleCount * 1000000ULL) / AUDIO_SAMPLE_RATE;
      if (sampleCount > 0) renderMIDIAudio((uint32_t)min((uint64_t)sampleCount, (uint64_t)3000000));
      currentTick = nextTick;
      if (stopRequested) break;
    }

    TrackState &t = tr[best];
    uint8_t status = t.eventStatus;
    uint8_t hi = status & 0xF0;
    uint8_t channel = status & 0x0F;
    if (status < 0xF0) {
      if (hi == 0x90) midiNoteOn(channel, t.data1, t.data2);
      else if (hi == 0x80) midiNoteOff(channel, t.data1);
      else if (hi == 0xB0 && t.data1 == 123) {
        for (int v = 0; v < MIDI_MAX_VOICES; v++)
          if (midiVoices[v].active && midiVoices[v].channel == channel)
            midiVoices[v].active = false;
      }
    }
    // Tempo event is global in SMF format 0/1.
    if (t.eventTempo >= 10000 && t.eventTempo <= 10000000)
      currentTempo = t.eventTempo;
    loadNextEvent(best);
    safetyEvents++;
    if (nextRequested || prevRequested) break;
  }

  silenceMIDIVoices();
  i2s_zero_dma_buffer(I2S_PORT);
  f.close();
  if (safetyEvents >= 2000000UL) Serial.println("MIDI event safety limit reached");
  return true;
}

// ============================================================
// 35. SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32-S3 AVI + WAV + MIDI");
  Serial.println("==============================");

  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);
  pinMode(BTN_VOL_DOWN, INPUT_PULLUP);
  pinMode(BTN_VOL_UP, INPUT_PULLUP);

  oldPrevState = digitalRead(BTN_PREV);
  oldPlayState = digitalRead(BTN_PLAY);
  oldNextState = digitalRead(BTN_NEXT);
  oldStopState = digitalRead(BTN_STOP);
  oldVolDownState = digitalRead(BTN_VOL_DOWN);
  oldVolUpState = digitalRead(BTN_VOL_UP);

  SPI.begin(TFT_SCLK, SD_MISO, TFT_MOSI, TFT_CS);
  delay(100);

  tft.init(240, 240);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 80);
  tft.println("AVI+WAV+MIDI");
  tft.setTextSize(1);
  tft.setCursor(25, 115);
  tft.println("Starting...");

  Serial.println("Init SD...");
  if (!SD.begin(SD_CS, SPI, SD_SPEED)) {
    Serial.println("SD FAILED");
    tft.fillScreen(ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(30, 100);
    tft.println("SD ERROR");
    while (true) delay(1000);
  }
  Serial.println("SD OK");

  jpegBuffer = (uint8_t *)malloc(JPEG_BUFFER_SIZE);
  if (jpegBuffer == NULL) {
    Serial.println("JPEG BUFFER ERROR");
    tft.fillScreen(ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 100);
    tft.println("JPEG BUFFER ERROR");
    while (true) delay(1000);
  }
  Serial.printf("JPEG BUFFER: %u KB\n", JPEG_BUFFER_SIZE / 1024);

  setupI2S();
  scanAVI();
  scanMusic();
  scanMIDI();
  showMenu();
  Serial.println("READY");
}

// ============================================================
// 36. MAIN LOOP
// ============================================================
void loop() {
  if (playerMode == MODE_MENU_AVI) {
    processMenu();
    delay(5);
    return;
  }
  if (playerMode == MODE_MENU_MUSIC) {
    processMusicMenu();
    delay(5);
    return;
  }
  if (playerMode == MODE_MENU_MIDI) {
    processMIDIMenu();
    delay(5);
    return;
  }

  if (playerMode == MODE_PLAY_AVI) {
    if (fileCount <= 0) { playerMode = MODE_MENU_AVI; showMenu(); return; }
    stopRequested = pauseRequested = false;
    nextRequested = prevRequested = playRequested = false;
    volumeDisplayRequest = false;
    showPlayingScreen();
    playAVI(aviFiles[selectedFile].c_str());
    i2s_zero_dma_buffer(I2S_PORT);

    if (stopRequested) {
      stopRequested = pauseRequested = false;
      nextRequested = prevRequested = playRequested = false;
      volumeDisplayRequest = false;
      oldStopState = digitalRead(BTN_STOP);
      playerMode = MODE_MENU_AVI; showMenu(); delay(150); return;
    }
    if (nextRequested) {
      nextRequested = false; selectedFile = (selectedFile + 1) % fileCount; return;
    }
    if (prevRequested) {
      prevRequested = false; selectedFile = (selectedFile - 1 + fileCount) % fileCount; return;
    }
    selectedFile = (selectedFile + 1) % fileCount;
    playerMode = MODE_MENU_AVI;
    pauseRequested = false;
    showMenu();
    delay(100);
    return;
  }

  if (playerMode == MODE_PLAY_MUSIC) {
    if (musicFileCount <= 0) { playerMode = MODE_MENU_MUSIC; showMusicMenu(); return; }
    stopRequested = pauseRequested = false;
    nextRequested = prevRequested = playRequested = false;
    volumeDisplayRequest = false;
    showMusicPlayingScreen();
    playWAV(musicFiles[selectedMusicFile].c_str());
    i2s_zero_dma_buffer(I2S_PORT);

    if (stopRequested) {
      stopRequested = pauseRequested = false;
      nextRequested = prevRequested = playRequested = false;
      volumeDisplayRequest = false;
      oldStopState = digitalRead(BTN_STOP);
      playerMode = MODE_MENU_MUSIC; showMusicMenu(); delay(150); return;
    }
    if (nextRequested) {
      nextRequested = false; selectedMusicFile = (selectedMusicFile + 1) % musicFileCount; return;
    }
    if (prevRequested) {
      prevRequested = false; selectedMusicFile = (selectedMusicFile - 1 + musicFileCount) % musicFileCount; return;
    }
    selectedMusicFile = (selectedMusicFile + 1) % musicFileCount;
    playerMode = MODE_PLAY_MUSIC;
    pauseRequested = false;
    delay(50);
    return;
  }

  if (playerMode == MODE_PLAY_MIDI) {
    if (midiFileCount <= 0) { playerMode = MODE_MENU_MIDI; showMIDIMenu(); return; }
    stopRequested = pauseRequested = false;
    nextRequested = prevRequested = playRequested = false;
    volumeDisplayRequest = false;
    showMIDIPlayingScreen();
    playMIDI(midiFiles[selectedMidiFile].c_str());
    i2s_zero_dma_buffer(I2S_PORT);

    if (stopRequested) {
      stopRequested = pauseRequested = false;
      nextRequested = prevRequested = playRequested = false;
      volumeDisplayRequest = false;
      oldStopState = digitalRead(BTN_STOP);
      playerMode = MODE_MENU_MIDI; showMIDIMenu(); delay(150); return;
    }
    if (nextRequested) {
      nextRequested = false; selectedMidiFile = (selectedMidiFile + 1) % midiFileCount; return;
    }
    if (prevRequested) {
      prevRequested = false; selectedMidiFile = (selectedMidiFile - 1 + midiFileCount) % midiFileCount; return;
    }
    selectedMidiFile = (selectedMidiFile + 1) % midiFileCount;
    playerMode = MODE_PLAY_MIDI;
    pauseRequested = false;
    delay(50);
    return;
  }
}
