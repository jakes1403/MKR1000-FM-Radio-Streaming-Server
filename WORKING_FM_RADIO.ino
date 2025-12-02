#include <SPI.h>
#include <WiFi101.h>
#include <Wire.h>
#include <Adafruit_VS1053.h>
#include <SD.h>
#include "ogg_plugin_encoded.h"

// ===================== Wi-Fi =====================
const char* WIFI_SSID = "MSU_IOT";
const char* WIFI_PASS = "msucowboys";

WiFiServer server(80);

static const uint16_t STREAM_CHUNK = 512;  // socket burst size

// ===================== RDA5807M FM TUNER =====================

// channel = (<desired freq in MHz> - 87.0) / 0.1
// e.g. 101.5 MHz -> channel = 10*101.5 - 870 = 145
uint16_t channel = 13;  // default station

#define RDA5807M_ADDRESS  0b0010000
#define BOOT_CONFIG_LEN 12
#define TUNE_CONFIG_LEN 4

uint8_t boot_config[] = {
  0b11000000, 0b00000011,
  0b00000000, 0b00000000,
  0b00001010, 0b00000000,
  0b10001000, 0b00001111,
  0b00000000, 0b00000000,
  0b01000010, 0b00000010,
};

uint8_t tune_config[] = {
  0b11000000,
  0b00000001,
  (uint8_t)(channel >> 2),
  (uint8_t)(((channel & 0b11) << 6) | 0b00010000)
};

// ===================== VS1053 OGG RECORDER =====================

#define RESET   2
#define CS      3
#define DCS     5
#define CARDCS  6
#define DREQ    7

Adafruit_VS1053_FilePlayer musicPlayer(RESET, CS, DCS, DREQ, CARDCS);

// ===================== Helper functions =====================

void ensure_wifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.end();
  delay(200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(200);
  }
}

void drain_http_headers(WiFiClient &c) {
  c.setTimeout(2000);
  while (c.connected()) {
    String line = c.readStringUntil('\n');
    if (!line.length()) break;
    line.trim();
    if (line.length() == 0) break;
  }
}

void send_ogg_header(WiFiClient &c) {
  c.println("HTTP/1.0 200 OK");
  c.println("Content-Type: audio/ogg");
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();
}

// Convert MHz to channel & retune the RDA5807
void set_frequency_mhz(float mhz) {
  if (mhz < 87.0f)  mhz = 87.0f;
  if (mhz > 108.0f) mhz = 108.0f;

  // channel = (<freq MHz> - 87.0) / 0.1
  // Use rounding to the nearest channel
  channel = (uint16_t)(((mhz - 87.0f) / 0.1f) + 0.5f);

  tune_config[2] = (uint8_t)(channel >> 2);
  tune_config[3] = (uint8_t)(((channel & 0x03) << 6) | 0b00010000);

  Wire.beginTransmission(RDA5807M_ADDRESS);
  Wire.write(tune_config, TUNE_CONFIG_LEN);
  Wire.endTransmission();

  Serial.print("Tuned to ");
  Serial.print(mhz, 1);
  Serial.print(" MHz (channel ");
  Serial.print(channel);
  Serial.println(")");
}

// Parse freq=NNN.N from the request line "GET /radio.ogg?freq=101.5 HTTP/1.1"
float parse_freq_from_req(const String &req) {
  int qmark = req.indexOf('?');
  if (qmark < 0) return -1.0f;

  int pos = req.indexOf("freq=", qmark);
  if (pos < 0) return -1.0f;
  pos += 5; // skip "freq="

  int endSpace = req.indexOf(' ', pos);
  int endAmp   = req.indexOf('&', pos);
  int end = -1;

  if (endSpace >= 0 && endAmp >= 0)      end = min(endSpace, endAmp);
  else if (endSpace >= 0)               end = endSpace;
  else if (endAmp >= 0)                 end = endAmp;
  else                                  end = req.length();

  String val = req.substring(pos, end);
  val.trim();
  if (!val.length()) return -1.0f;

  float f = val.toFloat();  // Arduino String -> float
  if (f <= 0.0f) return -1.0f;
  return f;
}

// Stream Ogg Vorbis bytes from a **fresh** VS1053 recording session
void stream_ogg_to_client(WiFiClient &c) {
  uint8_t buf[STREAM_CHUNK];

  // Make sure we always start from the very beginning of a new Ogg stream
  musicPlayer.stopRecordOgg();  // in case it was already running
  delay(10);

  // false = use LINE IN (for tuner audio into VS1053 line input)
  musicPlayer.startRecordOgg(false);

  // Wait for some data so we don't send empty response
  uint32_t t0 = millis();
  while (musicPlayer.recordedWordsWaiting() == 0 && c.connected()) {
    if (millis() - t0 > 2000) break;  // timeout
    delay(10);
  }

  // Now send HTTP header so the first bytes client receives
  // are the BOS Ogg page from the encoder
  send_ogg_header(c);

  while (c.connected()) {
    uint16_t wordswaiting = musicPlayer.recordedWordsWaiting();

    if (wordswaiting == 0) {
      delay(5);
      continue;
    }

    uint16_t words_to_read = wordswaiting;
    if (words_to_read * 2 > STREAM_CHUNK) {
      words_to_read = STREAM_CHUNK / 2;
    }

    uint16_t bytes = 0;
    for (uint16_t i = 0; i < words_to_read; ++i) {
      uint16_t w = musicPlayer.recordedReadWord();
      buf[bytes++] = (uint8_t)(w >> 8);
      buf[bytes++] = (uint8_t)(w & 0xFF);
    }

    int wrote = c.write(buf, bytes);
    if (wrote != bytes) {
      break;  // client closed or error
    }
  }

  // Stop recording when this client disconnects
  musicPlayer.stopRecordOgg();
  c.stop();
}

// ===================== Setup =====================

void setup() {
  Serial.begin(9600);
  delay(2000);
  Serial.println("\nFM Ogg Web Streamer starting...\n");

  // I2C for RDA5807M
  Wire.begin();

  Serial.print("Sending boot configuration to RDA5807M...");
  Wire.beginTransmission(RDA5807M_ADDRESS);
  Wire.write(boot_config, BOOT_CONFIG_LEN);
  Wire.endTransmission();
  Serial.println(" Done.");

  Serial.print("Initial tuning to channel...");
  Wire.beginTransmission(RDA5807M_ADDRESS);
  Wire.write(tune_config, TUNE_CONFIG_LEN);
  Wire.endTransmission();
  Serial.print(" Done (channel ");
  Serial.print(channel);
  Serial.println(").");

  // VS1053 init
  if (!musicPlayer.begin()) {
    Serial.println("VS1053 not found");
    while (1);
  }

  // Load Ogg plugin once
  if (!musicPlayer.prepareRecordOgg_plugin_from_memory(
          v44k1q05_img, v44k1q05_img_size)) {
    Serial.println("Couldn't load Ogg plugin!");
    while (1);
  }

  // Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(((IPAddress)WiFi.localIP()).toString());

  server.begin();
  Serial.println("HTTP server started on port 80");
}

// ===================== Main loop =====================

void loop() {
  ensure_wifi();

  WiFiClient c = server.available();
  if (!c) return;

  uint32_t t0 = millis();
  while (c.connected() && !c.available() && millis() - t0 < 2000) {
    delay(1);
  }
  if (!c.available()) {
    c.stop();
    return;
  }

  String req = c.readStringUntil('\n');
  req.trim();
  Serial.print("Request: ");
  Serial.println(req);
  drain_http_headers(c);

  if (req.startsWith("GET /radio.ogg")) {
    float freq = parse_freq_from_req(req);
    if (freq > 0.0f) {
      set_frequency_mhz(freq);
    } else {
      Serial.println("No valid freq= parameter; keeping current station.");
    }
    stream_ogg_to_client(c);
  } else {
    c.println("HTTP/1.0 200 OK");
    c.println("Content-Type: text/html");
    c.println("Cache-Control: no-store");
    c.println();
    c.println("<html><body style='font-family:sans-serif'>");
    c.println("<h3>MKR1000 FM Ogg Stream</h3>");
    c.println("<p>Example: <a href='/radio.ogg?freq=101.5'>/radio.ogg?freq=101.5</a></p>");
    c.println("<p>Frequency range: 87.0 to 108.0 MHz</p>");
    c.println("</body></html>");
    c.stop();
  }

  delay(5);
}
