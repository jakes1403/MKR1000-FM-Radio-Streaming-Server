#include <SPI.h>
#include <WiFi101.h>
#include <Wire.h>
#include <Adafruit_VS1053.h>
#include <SD.h>
#include "ogg_plugin_encoded.h"

// ===================== Wi-Fi =====================
const char* WIFI_SSID = "Shrek";
const char* WIFI_PASS = "Portal21";

WiFiServer server(80);

static const uint16_t STREAM_CHUNK = 512;  // socket burst size

// ===================== RDA5807M FM TUNER =====================

uint16_t channel = 13;  // your station

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

// Stream Ogg Vorbis bytes from a **fresh** VS1053 recording session
void stream_ogg_to_client(WiFiClient &c) {
  uint8_t buf[STREAM_CHUNK];

  // Make sure we always start from the very beginning of a new Ogg stream
  musicPlayer.stopRecordOgg();  // in case it was already running
  delay(10);

  musicPlayer.startRecordOgg(false);  // true = use line/mic in

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

  Serial.print("Tuning to channel...");
  Wire.beginTransmission(RDA5807M_ADDRESS);
  Wire.write(tune_config, TUNE_CONFIG_LEN);
  Wire.endTransmission();
  Serial.println(" Done.");

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

  // IMPORTANT: do NOT startRecordOgg() here.
  // We will start it per-connection in stream_ogg_to_client().

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
  drain_http_headers(c);

  if (req.startsWith("GET /radio.ogg")) {
    stream_ogg_to_client(c);
  } else {
    c.println("HTTP/1.0 200 OK");
    c.println("Content-Type: text/html");
    c.println("Cache-Control: no-store");
    c.println();
    c.println("<html><body style='font-family:sans-serif'>");
    c.println("<h3>MKR1000 FM Ogg Stream</h3>");
    c.println("<p><a href='/radio.ogg'>/radio.ogg</a> (live Ogg Vorbis stream from VS1053)</p>");
    c.println("</body></html>");
    c.stop();
  }

  delay(5);
}
