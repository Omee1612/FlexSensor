/*
  Sign-language reader — DEBUG BUILD
  ESP32-S3-CAM (Freenove-style) + LCD + NS4168 + Python server
  ------------------------------------------------------------------
  Button:  short press -> capture + decode sign     long press (>0.8 s) -> speak sentence

  Serial Monitor: 115200 baud, line ending "Newline" (any setting works).
  Type a letter + Enter to test each part WITHOUT the button:
     t  = play 1 kHz test tone (checks amp + I2S only)
     p  = ping server /health   (checks WiFi + PC server)
     c  = capture + decode       (same as short press)
     s  = speak the buffer       (same as long press)
     h  = speak the word "hello" (checks /speak + download + play; keeps buffer)
     i  = status info (WiFi, memory, buffer)
     x  = clear buffer

  Photos: every capture is saved by the server; view the latest at
  http://<PC-IP>:3000/last  and use CAM_VFLIP / CAM_HMIRROR / CAM_BRIGHTNESS below.

  Nothing printed at all? In Tools, "USB CDC On Boot" must be ENABLED if you
  plug into the ESP32-S3's native USB port, DISABLED if you use the USB-UART port.

  Arduino IDE (Tools): Board ESP32S3 Dev Module, PSRAM "OPI PSRAM".
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP_I2S.h>

// ---------------- CONFIG ----------------
const char* ssid        = "SAMEER";
const char* password    = "recentissue";
const char* SERVER_HOST = "10.200.15.95";   // your PC's LAN IP (run ipconfig on the PC)
const int   SERVER_PORT = 3000;

#define BUTTON_PIN  14
#define LCD_SDA      41
#define LCD_SCL      42
#define LCD_ADDR  0x27
#define I2S_BCLK    47
#define I2S_LRCLK   48
#define I2S_DOUT    45
#define AMP_CTRL    21   // NS4168 CTRL: HIGH = on

const unsigned long LONG_PRESS_MS  = 800;
const unsigned long DEBOUNCE_MS    = 30;
const int    VOLUME_PCT      = 70;
const size_t MAX_AUDIO_BYTES = 1024 * 1024;

// Camera image fixes: check the photo at http://<PC-IP>:3000/last and adjust
#define CAM_VFLIP       0   // 1 = flip upside down
#define CAM_HMIRROR     0   // 1 = mirror left/right
#define CAM_BRIGHTNESS  0   // -2 .. 2  (raise if photos are dark)
// -----------------------------------------

// Freenove ESP32-S3 WROOM CAM / ESP32-S3-EYE camera pins
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y2_GPIO_NUM    11
#define Y3_GPIO_NUM    9
#define Y4_GPIO_NUM    8
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    12
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    16
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// Every log line starts with the time since boot
#define LOG(fmt, ...) Serial.printf("[%7lu ms] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
I2SClass I2S;

String   sentenceBuffer = "";
uint32_t i2sRate  = 0;
bool     cameraOK = false;
bool     i2sOK    = false;
bool     lcdOK    = false;

// ---------- helpers ----------
const char* wifiStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (wrong name / out of range / 5 GHz only?)";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "OTHER";
  }
}

void logMemory(const char* where) {
  LOG("MEM %-14s heap free %u  | PSRAM free %u of %u", where,
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
}

void showLCD(const String& l1, const String& l2 = "") {
  LOG("LCD | %-16s | %-16s |", l1.substring(0, 16).c_str(), l2.substring(0, 16).c_str());
  if (!lcdOK) return;
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(l2.substring(0, 16));
}

void showReady() {
  if (sentenceBuffer.length()) showLCD("Buffer:", sentenceBuffer);
  else                         showLCD("Ready", "Press button");
}

String urlEncode(const String& s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) out += c;
    else { sprintf(buf, "%%%02X", (unsigned char)c); out += buf; }
  }
  return out;
}

String serverUrl(const String& path) {
  return String("http://") + SERVER_HOST + ":" + SERVER_PORT + path;
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  LOG("WiFi: not connected (status %d %s), reconnecting...",
      WiFi.status(), wifiStatusName(WiFi.status()));
  showLCD("WiFi lost", "Reconnecting...");
  WiFi.reconnect();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(200);
  LOG("WiFi: status now %d %s", WiFi.status(), wifiStatusName(WiFi.status()));
  return WiFi.status() == WL_CONNECTED;
}

void i2cScan() {
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      LOG("I2C: device at 0x%02X%s", a, a == LCD_ADDR ? "  <- LCD" : "");
      found++;
    }
  }
  if (!found) LOG("I2C: NO devices on SDA=%d SCL=%d (check LCD wiring / power)", LCD_SDA, LCD_SCL);
}

// ---------- Camera ----------
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count     = 1;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    LOG("CAMERA: init FAILED, error 0x%x (%s)", err, esp_err_to_name(err));
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
    s->set_brightness(s, CAM_BRIGHTNESS);
  }
  LOG("CAMERA: init OK, sensor PID 0x%x, vflip=%d hmirror=%d brightness=%d",
      s ? s->id.PID : 0, CAM_VFLIP, CAM_HMIRROR, CAM_BRIGHTNESS);
  return true;
}

// ---------- I2S / audio ----------
bool startI2S(uint32_t rate) {
  if (rate == i2sRate) return true;
  if (i2sRate) I2S.end();
  I2S.setPins(I2S_BCLK, I2S_LRCLK, I2S_DOUT, -1, -1);
  if (!I2S.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_BOTH)) {
    LOG("I2S: begin(%lu Hz) FAILED, lastError=%d", (unsigned long)rate, I2S.lastError());
    i2sRate = 0;
    return false;
  }
  i2sRate = rate;
  LOG("I2S: running at %lu Hz, 16-bit, mono, both slots (BCLK %d, LRCLK %d, DOUT %d)",
      (unsigned long)rate, I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  return true;
}

// Write 16-bit samples with volume; returns bytes accepted by I2S
size_t writeSamples(const int16_t* src, size_t total) {
  int16_t buf[256];
  size_t sent = 0;
  for (size_t i = 0; i < total; i += 256) {
    size_t n = (total - i < 256) ? (total - i) : 256;
    for (size_t k = 0; k < n; k++) buf[k] = (int16_t)((int32_t)src[i + k] * VOLUME_PCT / 100);
    sent += I2S.write((uint8_t*)buf, n * 2);
  }
  return sent;
}

void playTestTone() {
  if (!startI2S(i2sRate ? i2sRate : 16000)) return;
  const uint32_t rate = i2sRate;
  const size_t   total = rate / 2;                 // 0.5 s
  static int16_t toneBuf[256];
  float phase = 0, inc = 2.0f * PI * 1000.0f / rate;
  size_t sent = 0;
  unsigned long t0 = millis();
  LOG("TONE: playing 1 kHz for 0.5 s at %lu Hz", (unsigned long)rate);
  for (size_t i = 0; i < total; i += 256) {
    size_t n = (total - i < 256) ? (total - i) : 256;
    for (size_t k = 0; k < n; k++) { toneBuf[k] = (int16_t)(8000 * sinf(phase)); phase += inc; }
    sent += I2S.write((uint8_t*)toneBuf, n * 2);
  }
  LOG("TONE: sent %u of %u bytes in %lu ms (CTRL pin reads %s)", (unsigned)sent,
      (unsigned)(total * 2), millis() - t0, digitalRead(AMP_CTRL) ? "HIGH" : "LOW");
}

static uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool playWavBuffer(const uint8_t* w, size_t len) {
  if (len < 12 || memcmp(w, "RIFF", 4) != 0 || memcmp(w + 8, "WAVE", 4) != 0) {
    LOG("PLAY: data is NOT a WAV. First bytes:");
    for (size_t i = 0; i < len && i < 32; i++) Serial.printf("%02X ", w[i]);
    Serial.println();
    return false;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t* pcm = nullptr;
  size_t pcmLen = 0;

  size_t p = 12;
  while (p + 8 <= len) {
    const uint8_t* body = w + p + 8;
    size_t size = rd32(w + p + 4);
    size_t room = len - p - 8;
    LOG("PLAY: chunk '%c%c%c%c' size %u", w[p], w[p + 1], w[p + 2], w[p + 3], (unsigned)size);
    if (memcmp(w + p, "fmt ", 4) == 0 && size >= 16) {
      format = rd16(body); channels = rd16(body + 2);
      rate = rd32(body + 4); bits = rd16(body + 14);
    } else if (memcmp(w + p, "data", 4) == 0) {
      pcm = body;
      pcmLen = (size > room) ? room : size;
      break;
    }
    if (size > room) break;
    p += 8 + size + (size & 1);
  }

  LOG("PLAY: WAV fmt=%u ch=%u rate=%lu bits=%u pcm=%u bytes (%.2f s)",
      format, channels, (unsigned long)rate, bits, (unsigned)pcmLen,
      rate ? (float)pcmLen / (rate * 2) : 0.0f);
  if (!pcm || format != 1 || channels != 1 || bits != 16 || rate < 8000 || rate > 96000) {
    LOG("PLAY: unsupported WAV (need 16-bit mono PCM)");
    return false;
  }
  if (!startI2S(rate)) return false;

  unsigned long t0 = millis();
  size_t sent = writeSamples((const int16_t*)pcm, pcmLen / 2);
  unsigned long ms = millis() - t0;
  LOG("PLAY: sent %u of %u bytes in %lu ms (expected ~%lu ms)",
      (unsigned)sent, (unsigned)pcmLen, ms, (unsigned long)(pcmLen * 1000UL / (rate * 2)));
  if (sent != pcmLen) LOG("PLAY: WARNING short write, I2S lastError=%d", I2S.lastError());
  return sent == pcmLen;
}

// ---------- server ping ----------
bool pingServer() {
  if (!ensureWiFi()) return false;
  HTTPClient http;
  String url = serverUrl("/health");
  LOG("PING: GET %s", url.c_str());
  http.begin(url);
  http.setTimeout(3000);
  unsigned long t0 = millis();
  int code = http.GET();
  if (code > 0) LOG("PING: HTTP %d '%s' in %lu ms", code, http.getString().c_str(), millis() - t0);
  else LOG("PING: FAILED %d (%s) -> is server.py running? same WiFi? firewall allows port %d?",
           code, HTTPClient::errorToString(code).c_str(), SERVER_PORT);
  http.end();
  return code == 200;
}

// ---------- capture + decode ----------
void captureAndAppend() {
  LOG("=== CAPTURE ===");
  if (!cameraOK) { LOG("CAPTURE: camera not initialised, skipping"); showLCD("Camera FAIL"); return; }
  if (!ensureWiFi()) { showLCD("No WiFi"); return; }

  showLCD("Capturing...");
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);          // discard stale frame
  fb = esp_camera_fb_get();
  if (!fb) { LOG("CAPTURE: esp_camera_fb_get() returned NULL"); showLCD("Capture failed", sentenceBuffer); return; }
  LOG("CAPTURE: got JPEG %u bytes, %ux%u", (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height);

  showLCD("Decoding...");
  HTTPClient http;
  String url = serverUrl("/decode");
  LOG("DECODE: POST %s", url.c_str());
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(20000);
  unsigned long t0 = millis();
  int code = http.POST(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  LOG("DECODE: HTTP %d (%s) after %lu ms", code,
      code > 0 ? "ok" : HTTPClient::errorToString(code).c_str(), millis() - t0);

  if (code == 200) {
    String sign = http.getString();
    sign.trim();
    LOG("DECODE: server says '%s'", sign.c_str());
    if (sign.length() > 0 && sign != "UNCLEAR" && sign != "ERROR") {
      if (sentenceBuffer.length() > 0) sentenceBuffer += " ";
      sentenceBuffer += sign;
      LOG("BUFFER: '%s'", sentenceBuffer.c_str());
      showLCD("Buffer:", sentenceBuffer);
    } else {
      LOG("DECODE: not added. Check the photo at http://%s:%d/last", SERVER_HOST, SERVER_PORT);
      showLCD("Unclear, retry", sentenceBuffer);
    }
  } else {
    if (code > 0) LOG("DECODE: body: %s", http.getString().c_str());
    showLCD("Server error", String(code));
  }
  http.end();
}

// ---------- download WAV for text and play it ----------
bool fetchAndPlay(const String& text) {
  LOG("=== SPEAK '%s' ===", text.c_str());
  if (!ensureWiFi()) { showLCD("No WiFi", text); return false; }

  showLCD("Getting audio", text);
  HTTPClient http;
  String url = serverUrl("/speak?text=" + urlEncode(text));
  LOG("SPEAK: GET %s", url.c_str());
  http.begin(url);
  http.setTimeout(20000);
  unsigned long t0 = millis();
  int code = http.GET();
  LOG("SPEAK: HTTP %d (%s) after %lu ms", code,
      code > 0 ? "ok" : HTTPClient::errorToString(code).c_str(), millis() - t0);
  if (code != 200) {
    if (code > 0) LOG("SPEAK: body: %s", http.getString().c_str());
    showLCD("TTS error", String(code));
    http.end();
    return false;
  }

  int len = http.getSize();
  LOG("SPEAK: Content-Length %d", len);
  if (len <= 44 || (size_t)len > MAX_AUDIO_BYTES) {
    LOG("SPEAK: bad size (need 45..%u bytes). -1 means server sent no Content-Length",
        (unsigned)MAX_AUDIO_BYTES);
    showLCD("Bad audio size", String(len));
    http.end();
    return false;
  }

  uint8_t* wav = (uint8_t*)ps_malloc(len);
  if (!wav) {
    LOG("SPEAK: ps_malloc(%d) FAILED", len);
    logMemory("after malloc");
    showLCD("No PSRAM mem");
    http.end();
    return false;
  }

  Stream* stream = http.getStreamPtr();
  stream->setTimeout(5000);
  size_t got = 0, nextReport = 32768;
  t0 = millis();
  while (got < (size_t)len) {
    size_t n = stream->readBytes(wav + got, len - got);
    if (n == 0) break;
    got += n;
    if (got >= nextReport) { LOG("SPEAK: downloaded %u / %d", (unsigned)got, len); nextReport += 32768; }
  }
  http.end();
  LOG("SPEAK: download done %u / %d bytes in %lu ms", (unsigned)got, len, millis() - t0);

  bool ok = false;
  if (got == (size_t)len) {
    showLCD("Speaking:", text);
    ok = playWavBuffer(wav, got);
    if (!ok) showLCD("Play error", text);
  } else {
    LOG("SPEAK: download incomplete (timeout or connection closed)");
    showLCD("Download failed", text);
  }
  free(wav);
  return ok;
}

void speakBuffer() {
  if (sentenceBuffer.length() == 0) {
    LOG("SPEAK: buffer is empty, nothing to say (UNCLEAR/ERROR results are not added). Type h to test the speaker.");
    showLCD("Buffer empty");
    delay(800);
    showReady();
    return;
  }
  if (fetchAndPlay(sentenceBuffer)) {
    sentenceBuffer = "";
    LOG("BUFFER: cleared after speaking");
  }
  delay(300);
  showReady();
}

void printStatus() {
  LOG("STATUS: WiFi %d %s, IP %s, RSSI %d dBm", WiFi.status(), wifiStatusName(WiFi.status()),
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  LOG("STATUS: server %s:%d  camera %s  i2s %s (%lu Hz)  lcd %s",
      SERVER_HOST, SERVER_PORT, cameraOK ? "OK" : "FAIL", i2sOK ? "OK" : "FAIL",
      (unsigned long)i2sRate, lcdOK ? "OK" : "NOT FOUND");
  LOG("STATUS: button GPIO%d reads %s, buffer '%s'", BUTTON_PIN,
      digitalRead(BUTTON_PIN) == LOW ? "PRESSED" : "released", sentenceBuffer.c_str());
  logMemory("now");
}

void handleSerial() {
  while (Serial.available()) {
    char c = tolower(Serial.read());
    if (c == '\n' || c == '\r' || c == ' ') continue;
    LOG("CMD: '%c'", c);
    switch (c) {
      case 't': playTestTone(); break;
      case 'p': pingServer(); break;
      case 'c': captureAndAppend(); break;
      case 's': speakBuffer(); break;
      case 'h': fetchAndPlay("hello"); showReady(); break;
      case 'i': printStatus(); break;
      case 'x': sentenceBuffer = ""; LOG("BUFFER: cleared"); showReady(); break;
      default:  LOG("CMD: unknown. Use t p c s h i x"); break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);                               // time to open the Serial Monitor
  Serial.println();
  LOG("================ SIGN READER DEBUG BOOT ================");
  LOG("Chip %s rev %d, CPU %lu MHz, flash %u bytes",
      ESP.getChipModel(), ESP.getChipRevision(), (unsigned long)ESP.getCpuFreqMHz(),
      (unsigned)ESP.getFlashChipSize());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(AMP_CTRL, OUTPUT);
  digitalWrite(AMP_CTRL, HIGH);
  LOG("PINS: button GPIO%d (reads %s), amp CTRL GPIO%d set HIGH", BUTTON_PIN,
      digitalRead(BUTTON_PIN) == LOW ? "LOW = pressed/shorted?!" : "HIGH ok", AMP_CTRL);

  // [1/5] LCD
  LOG("[1/5] LCD / I2C on SDA=%d SCL=%d", LCD_SDA, LCD_SCL);
  Wire.begin(LCD_SDA, LCD_SCL);
  i2cScan();
  Wire.beginTransmission(LCD_ADDR);
  lcdOK = (Wire.endTransmission() == 0);
  if (lcdOK) { lcd.init(); lcd.backlight(); }
  else LOG("LCD: nothing at 0x%02X. If the scan found 0x3F, change LCD_ADDR", LCD_ADDR);

  // [2/5] PSRAM
  LOG("[2/5] PSRAM");
  if (!psramFound()) LOG("PSRAM: NOT FOUND -> Tools > PSRAM > OPI PSRAM. Camera + speech will fail.");
  logMemory("boot");

  // [3/5] Camera
  LOG("[3/5] Camera");
  showLCD("Init camera...");
  cameraOK = initCamera();
  if (!cameraOK) showLCD("Camera FAIL", "see Serial");

  // [4/5] I2S
  LOG("[4/5] I2S / amp");
  i2sOK = startI2S(16000);
  if (i2sOK) playTestTone();                 // short beep = audio path alive
  else showLCD("I2S FAIL", "see Serial");

  // [5/5] WiFi + server
  LOG("[5/5] WiFi '%s'", ssid);
  showLCD("Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  int lastStatus = -1;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    if (WiFi.status() != lastStatus) {
      lastStatus = WiFi.status();
      LOG("WiFi: status %d %s", lastStatus, wifiStatusName(lastStatus));
    }
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    LOG("WiFi: CONNECTED in %lu ms, IP %s, RSSI %d dBm", millis() - t0,
        WiFi.localIP().toString().c_str(), WiFi.RSSI());
    pingServer();
  } else {
    LOG("WiFi: FAILED after 20 s, status %d %s (will retry on each request)",
        WiFi.status(), wifiStatusName(WiFi.status()));
    showLCD("WiFi FAIL", "see Serial");
    delay(1500);
  }

  LOG("BOOT DONE. Commands: t=tone p=ping c=capture s=speak h=hello i=info x=clear");
  showReady();
}

void loop() {
  handleSerial();

  static bool stable = false, lastRaw = false;
  static unsigned long changedAt = 0, pressStart = 0;

  bool raw = (digitalRead(BUTTON_PIN) == LOW);
  if (raw != lastRaw) { lastRaw = raw; changedAt = millis(); }

  if (millis() - changedAt > DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) {
      pressStart = millis();
      LOG("BUTTON: pressed");
    } else {
      unsigned long heldFor = millis() - pressStart;
      LOG("BUTTON: released after %lu ms -> %s", heldFor,
          heldFor >= LONG_PRESS_MS ? "LONG (speak)" : "SHORT (capture)");
      if (heldFor >= LONG_PRESS_MS) speakBuffer();
      else                          captureAndAppend();
    }
  }
  delay(5);
}