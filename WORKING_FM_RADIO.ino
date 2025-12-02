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
    c.println("<!DOCTYPE html>");
    c.println("<html lang='en'>");
    c.println("<head>");
    c.println("<meta charset='UTF-8'>");
    c.println("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    c.println("<title>Radio Tuner</title>");
    c.println("<style>");
    c.println("* { margin: 0; padding: 0; box-sizing: border-box; }");
    c.println("body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: 20px; }");
    c.println(".radio-container { background: rgba(255, 255, 255, 0.1); backdrop-filter: blur(10px); border-radius: 30px; padding: 40px; box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3); border: 1px solid rgba(255, 255, 255, 0.2); max-width: 450px; width: 100%; }");
    c.println(".radio-header { text-align: center; margin-bottom: 30px; }");
    c.println(".radio-header h1 { color: white; font-size: 2em; font-weight: 300; letter-spacing: 2px; }");
    c.println(".frequency-display { background: rgba(0, 0, 0, 0.3); border-radius: 15px; padding: 20px; margin-bottom: 30px; text-align: center; box-shadow: inset 0 2px 10px rgba(0, 0, 0, 0.3); }");
    c.println(".frequency-number { font-size: 3.5em; color: #00ff88; font-weight: 300; text-shadow: 0 0 20px rgba(0, 255, 136, 0.5); letter-spacing: 3px; }");
    c.println(".frequency-unit { color: rgba(255, 255, 255, 0.6); font-size: 1.2em; margin-top: 5px; }");
    c.println(".station-name { color: white; font-size: 1.3em; margin-top: 10px; min-height: 30px; opacity: 0.9; }");
    c.println(".tuner-slider { margin: 30px 0; position: relative; }");
    c.println(".slider { -webkit-appearance: none; width: 100%; height: 8px; border-radius: 5px; background: rgba(255, 255, 255, 0.2); outline: none; transition: all 0.3s; }");
    c.println(".slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 30px; height: 30px; border-radius: 50%; background: linear-gradient(135deg, #00ff88, #00ccff); cursor: pointer; box-shadow: 0 0 20px rgba(0, 255, 136, 0.6); transition: all 0.3s; }");
    c.println(".slider::-webkit-slider-thumb:hover { transform: scale(1.2); box-shadow: 0 0 30px rgba(0, 255, 136, 0.8); }");
    c.println(".slider::-moz-range-thumb { width: 30px; height: 30px; border-radius: 50%; background: linear-gradient(135deg, #00ff88, #00ccff); cursor: pointer; border: none; box-shadow: 0 0 20px rgba(0, 255, 136, 0.6); transition: all 0.3s; }");
    c.println(".slider::-moz-range-thumb:hover { transform: scale(1.2); box-shadow: 0 0 30px rgba(0, 255, 136, 0.8); }");
    c.println(".controls { display: flex; justify-content: center; gap: 15px; margin-top: 30px; }");
    c.println(".control-btn { background: rgba(255, 255, 255, 0.2); border: 1px solid rgba(255, 255, 255, 0.3); border-radius: 50%; width: 60px; height: 60px; display: flex; align-items: center; justify-content: center; cursor: pointer; transition: all 0.3s; font-size: 1.5em; color: white; }");
    c.println(".control-btn:hover { background: rgba(255, 255, 255, 0.3); transform: scale(1.1); }");
    c.println(".control-btn:active { transform: scale(0.95); }");
    c.println(".play-btn { width: 80px; height: 80px; font-size: 2em; background: linear-gradient(135deg, #00ff88, #00ccff); border: none; box-shadow: 0 5px 20px rgba(0, 255, 136, 0.4); }");
    c.println(".play-btn:hover { box-shadow: 0 5px 30px rgba(0, 255, 136, 0.6); }");
    c.println(".signal-indicator { display: flex; gap: 3px; justify-content: center; margin-top: 15px; }");
    c.println(".signal-bar { width: 4px; height: 15px; background: rgba(255, 255, 255, 0.3); border-radius: 2px; transition: all 0.3s; }");
    c.println(".signal-bar.active { background: #00ff88; box-shadow: 0 0 10px rgba(0, 255, 136, 0.5); }");
    c.println(".frequency-input-container { margin-top: 25px; display: flex; gap: 10px; align-items: center; }");
    c.println(".frequency-input { flex: 1; background: rgba(255, 255, 255, 0.15); border: 1px solid rgba(255, 255, 255, 0.3); border-radius: 10px; padding: 15px; color: white; font-size: 1.2em; text-align: center; outline: none; transition: all 0.3s; }");
    c.println(".frequency-input::placeholder { color: rgba(255, 255, 255, 0.5); }");
    c.println(".frequency-input:focus { background: rgba(255, 255, 255, 0.25); border-color: #00ff88; box-shadow: 0 0 15px rgba(0, 255, 136, 0.3); }");
    c.println(".tune-btn { background: linear-gradient(135deg, #00ff88, #00ccff); border: none; border-radius: 10px; padding: 15px 25px; color: white; font-size: 1.1em; font-weight: 600; cursor: pointer; transition: all 0.3s; box-shadow: 0 5px 20px rgba(0, 255, 136, 0.4); }");
    c.println(".tune-btn:hover { transform: translateY(-2px); box-shadow: 0 5px 30px rgba(0, 255, 136, 0.6); }");
    c.println(".tune-btn:active { transform: translateY(0); }");
    c.println("</style>");
    c.println("</head>");
    c.println("<body>");
    c.println("<div class='radio-container'>");
    c.println("<div class='radio-header'><h1>FM RADIO</h1></div>");
    c.println("<div class='frequency-display'>");
    c.println("<div class='frequency-number' id='frequency'>98.5</div>");
    c.println("<div class='frequency-unit'>MHz</div>");
    c.println("<div class='station-name' id='station'>Enter a frequency</div>");
    c.println("<div class='signal-indicator'>");
    c.println("<div class='signal-bar'></div>");
    c.println("<div class='signal-bar'></div>");
    c.println("<div class='signal-bar'></div>");
    c.println("<div class='signal-bar'></div>");
    c.println("<div class='signal-bar'></div>");
    c.println("</div></div>");
    c.println("<div class='tuner-slider'>");
    c.println("<input type='range' min='880' max='1080' value='985' class='slider' id='tuner'>");
    c.println("</div>");
    c.println("<div class='controls'>");
    c.println("<div class='control-btn' id='prevBtn'>◄</div>");
    c.println("<div class='control-btn play-btn' id='playBtn'>▶</div>");
    c.println("<div class='control-btn' id='nextBtn'>►</div>");
    c.println("</div>");
    c.println("<div class='frequency-input-container'>");
    c.println("<input type='number' class='frequency-input' id='freqInput' placeholder='Enter frequency (88.0 - 108.0)' min='88.0' max='108.0' step='0.1'>");
    c.println("<button class='tune-btn' id='tuneBtn'>Tune</button>");
    c.println("</div></div>");
    c.println("<audio id='radioAudio' preload='none'></audio>");
    c.println("<script>");
    c.println("const tuner=document.getElementById('tuner');");
    c.println("const frequencyDisplay=document.getElementById('frequency');");
    c.println("const stationDisplay=document.getElementById('station');");
    c.println("const playBtn=document.getElementById('playBtn');");
    c.println("const prevBtn=document.getElementById('prevBtn');");
    c.println("const nextBtn=document.getElementById('nextBtn');");
    c.println("const signalBars=document.querySelectorAll('.signal-bar');");
    c.println("const freqInput=document.getElementById('freqInput');");
    c.println("const tuneBtn=document.getElementById('tuneBtn');");
    c.println("const radioAudio=document.getElementById('radioAudio');");
    c.println("let isPlaying=false;");
    c.println("let currentFrequency=null;");
    c.println("function updateFrequency(value){");
    c.println("const freq=(value/10).toFixed(1);");
    c.println("frequencyDisplay.textContent=freq;");
    c.println("tuner.value=value;");
    c.println("const decimal=freq%1;");
    c.println("if(decimal===0||decimal===0.5){updateSignal(5);stationDisplay.textContent='Station Found';}");
    c.println("else{updateSignal(3);stationDisplay.textContent='Tuning...';}}");
    c.println("function updateSignal(strength){signalBars.forEach((bar,i)=>{bar.classList.toggle('active',i<strength);});}");
    c.println("function playRadio(freq){currentFrequency=freq;");
    c.println("radioAudio.src='/radio.ogg?freq='+freq;");
    c.println("radioAudio.play().catch(err=>{console.log('Audio playback failed:',err);stationDisplay.textContent='Playback Error';});");
    c.println("isPlaying=true;playBtn.textContent='⏸';stationDisplay.textContent='Playing '+freq+' MHz';}");
    c.println("function stopRadio(){radioAudio.pause();radioAudio.src='';isPlaying=false;playBtn.textContent='▶';");
    c.println("stationDisplay.textContent=currentFrequency?'Tuned to '+currentFrequency+' MHz':'Enter a frequency';}");
    c.println("tuner.addEventListener('input',e=>{updateFrequency(e.target.value);");
    c.println("if(isPlaying&&currentFrequency){const freq=(e.target.value/10).toFixed(1);playRadio(freq);}});");
    c.println("tuneBtn.addEventListener('click',()=>{const freq=parseFloat(freqInput.value);");
    c.println("if(freq>=88.0&&freq<=108.0){const value=Math.round(freq*10);updateFrequency(value);playRadio(freq.toFixed(1));freqInput.value='';}");
    c.println("else{stationDisplay.textContent='Invalid frequency';}});");
    c.println("freqInput.addEventListener('keypress',e=>{if(e.key==='Enter'){tuneBtn.click();}});");
    c.println("playBtn.addEventListener('click',()=>{if(currentFrequency){if(isPlaying){stopRadio();}else{playRadio(currentFrequency);}}");
    c.println("else{stationDisplay.textContent='Enter a frequency first';}});");
    c.println("prevBtn.addEventListener('click',()=>{if(currentFrequency){");
    c.println("const newFreq=Math.max(88.0,parseFloat(currentFrequency)-0.1).toFixed(1);updateFrequency(Math.round(newFreq*10));");
    c.println("if(isPlaying){playRadio(newFreq);}}});");
    c.println("nextBtn.addEventListener('click',()=>{if(currentFrequency){");
    c.println("const newFreq=Math.min(108.0,parseFloat(currentFrequency)+0.1).toFixed(1);updateFrequency(Math.round(newFreq*10));");
    c.println("if(isPlaying){playRadio(newFreq);}}});");
    c.println("stationDisplay.textContent='Enter a frequency';updateSignal(0);");
    c.println("</script>");
    c.println("</body></html>");
    c.stop();
  }

  delay(5);
}
