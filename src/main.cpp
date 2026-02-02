#include "M5Unified.h"
#include <Arduino.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>

#include <WebServer.h>
#include <ESPmDNS.h>

#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>

#include "mbedtls/sha256.h"

// ================== HTTP SERVER ==================
WebServer server(8183);

// ================== UI CONFIG ==================
String MATRIX_TEXT = "M5 StickC";   // Configurable via WiFiManager
float  MAX_NET_Mbps = 100.0;        // Configurable via WiFiManager
Preferences preferences;            // namespace: "m5config"

// ================== MQTT (HiveMQ Cloud TLS 8883) ==================
static const char* MQTT_HOST = "724f4005ddac40d5a4d1586443333e56.s1.eu.hivemq.cloud";
static const uint16_t MQTT_PORT = 8883;
static const char* MQTT_USER = "client";
static const char* MQTT_PASS = "BJa938Cguzds4fx";

WiFiClientSecure tls;
PubSubClient mqtt(tls);
WiFiUDP udp;

// ================== Stateless WoL ID / Topic ==================
static String rawId;        // human-facing short id
static String deviceHash;   // sha256(rawId) hex (64)
static String TOPIC_CMD;    // wol/<deviceHash>

// Track last received MAC (for overlay display)
static bool hasLastMac = false;
static uint8_t lastMac[6] = {0};

// ================== COLORS ==================
const uint16_t COLOR_BLACK   = 0x0000;
const uint16_t COLOR_WHITE   = 0xFFFF;
const uint16_t COLOR_GRAY    = 0x8410;
const uint16_t COLOR_RED     = 0xF922;
const uint16_t COLOR_CYAN    = 0x1FFD;
const uint16_t COLOR_BAR     = 0xFD80;
const uint16_t HackerColors[2] = {0x07E0, 0xFFFF};

// Thresholds
const int   HIGH_USAGE_PERCENT = 90;
const float HIGH_SPEED_Mbps    = 45.0;
const int   GPU_TEMP_WARN      = 80;
const int   GPU_TEMP_BLINK     = 85;

// Layout
const int barHeight   = 8;
const int spacing     = 2;
const int textOffset  = 16;
const int startY      = 4;

// Burn-in protection (LCD OFF)
unsigned long lastUpdate = 0;
const unsigned long SCREEN_TIMEOUT = 15000;  // 15s no data = sleep LCD
bool screenIsOn = true;

// Burn-in animations toggles
unsigned long lastSwitch = 0;
bool showMain   = true;
bool showSengun = true;
const unsigned long MAIN_DURATION   = 20000;
const unsigned long SCREEN_DURATION = 8000;

// Matrix config
const int MATRIX_COLS  = 24;
const int MATRIX_SPEED = 6;
const int MATRIX_TRAIL = 3;
int matrixY[MATRIX_COLS];

// Timing
unsigned long lastHUD    = 0;
unsigned long lastMatrix = 0;
unsigned long lastBlink  = 0;
const unsigned long HUD_INTERVAL    = 120;
const unsigned long MATRIX_INTERVAL = 80;
const unsigned long BLINK_INTERVAL  = 500;

// Daily reboot
const unsigned long DAILY_REBOOT_MS = 86400000;
unsigned long startTime = 0;

// Blink state
bool gpuTempBlinkState = false;

// Button B long press for reset
unsigned long btnBPressStart = 0;
bool btnBPressed = false;
const unsigned long BTN_LONG_PRESS_MS = 2000;

// ================== BUTTON A: SHOW ID OVERLAY ==================
static bool showIdOverlay = false;
static unsigned long idOverlayUntil = 0;
static const unsigned long ID_OVERLAY_MS = 4000;
static bool idOverlayDrawn = false;

// Stats
int displayCPU=0, displayRAM=0, displayGPU=0, displayGPUtemp=0;
int displayUP=0,  displayDN=0;
int targetCPU=0,  targetRAM=0,  targetGPU=0,  targetGPUtemp=0;
int targetUP=0,   targetDN=0;

// Previous values
int lastCPU=-1, lastRAM=-1, lastGPU=-1, lastGPUtemp=-1, lastUP=-1, lastDN=-1;

// Clock
const char* ntpServer        = "pool.ntp.org";
const long  gmtOffset_sec    = 28800;
const int   daylightOffset_sec = 0;
bool colonVisible = true;
unsigned long lastColonToggle = 0;

static void setupClock() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ================== SHA-256 ==================
static String sha256Hex(const String& input) {
  uint8_t out[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish_ret(&ctx, out);
  mbedtls_sha256_free(&ctx);

  char buf[65];
  for (int i = 0; i < 32; i++) sprintf(buf + i * 2, "%02x", out[i]);
  buf[64] = 0;
  return String(buf);
}

// ================== MAC HELPERS ==================
static bool parseMac(const String& macStr, uint8_t out[6]) {
  String s = macStr;
  s.trim();
  int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

static String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// ================== ID OVERLAY DRAW ==================
static void drawIdOverlay() {
  M5.Display.fillScreen(COLOR_BLACK);

  M5.Display.setCursor(20, 40);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("ID: ");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.println(rawId);

  // MAC line
  M5.Display.setTextSize(1.5);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.setCursor(20, 75);

  if (hasLastMac) {
    M5.Display.print("MAC: ");
    M5.Display.setTextColor(COLOR_WHITE);
    M5.Display.print(macToString(lastMac));
  } else {
    M5.Display.print("MAC: not received");
  }

  // URL line
  M5.Display.setTextSize(1.5);
  M5.Display.setCursor(20, 120);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("https://");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.print("wol.kreaxv.top");

  M5.Power.setLed(false);
}

// ================== THREAD-SAFE MQTT EVENT MAILBOX ==================
enum MqttEventType : uint8_t {
  EVT_NONE = 0,
  EVT_WOL_MAC
};

static portMUX_TYPE g_evtMux = portMUX_INITIALIZER_UNLOCKED;
static volatile MqttEventType g_evtType = EVT_NONE;
static char g_evtPayload[64] = {0}; // MAC string

static void postEvent(MqttEventType t, const char* payloadOrNull) {
  portENTER_CRITICAL(&g_evtMux);
  g_evtType = t;
  if (payloadOrNull) {
    strncpy(g_evtPayload, payloadOrNull, sizeof(g_evtPayload) - 1);
    g_evtPayload[sizeof(g_evtPayload) - 1] = '\0';
  } else {
    g_evtPayload[0] = '\0';
  }
  portEXIT_CRITICAL(&g_evtMux);
}

static MqttEventType fetchEvent(char* outPayload, size_t outLen) {
  MqttEventType t;
  portENTER_CRITICAL(&g_evtMux);
  t = g_evtType;
  if (t != EVT_NONE && outPayload && outLen) {
    strncpy(outPayload, g_evtPayload, outLen - 1);
    outPayload[outLen - 1] = '\0';
  }
  g_evtType = EVT_NONE;
  g_evtPayload[0] = '\0';
  portEXIT_CRITICAL(&g_evtMux);
  return t;
}

// ================== WOL (stateless) ==================
static void sendWOL(const uint8_t mac[6]) {
  // Beep on main core (safe)
  if (M5.Speaker.isEnabled()) {
    M5.Speaker.tone(2000, 100);
  }

  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 6; i < 102; i += 6) memcpy(packet + i, mac, 6);

  static bool udpStarted = false;
  if (!udpStarted) {
    udp.begin(0); // ephemeral port is fine
    udpStarted = true;
  }

  IPAddress bcast = WiFi.broadcastIP();
  if (!udp.beginPacket(bcast, 9)) {
    Serial.println("udp.beginPacket failed");
    return;
  }
  udp.write(packet, sizeof(packet));
  if (!udp.endPacket()) {
    Serial.println("udp.endPacket failed");
    return;
  }

  Serial.print("WoL sent to ");
  Serial.println(macToString(mac));
}

// ================== MQTT CALLBACK (NO M5 / NO NVS HERE) ==================
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != TOPIC_CMD) return;

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  // payload must be "AA:BB:CC:DD:EE:FF"
  postEvent(EVT_WOL_MAC, msg.c_str());
}

// ================== MQTT CONNECT / HOUSEKEEPING ==================
static unsigned long lastMqttAttempt = 0;
static const unsigned long MQTT_RETRY_MS = 3000;

static void mqttInitOnce() {
  TOPIC_CMD = "wol/" + deviceHash;

  tls.setInsecure(); // MVP mode (no CA check)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);

  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static void mqttEnsureConnectedNonBlocking() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = now;

  char clientId[48];
  snprintf(clientId, sizeof(clientId), "wol-%s-%lx", deviceHash.substring(0,16).c_str(), (uint32_t)now);

  Serial.print("MQTT connecting... ");
  if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
    Serial.println("OK");
    mqtt.subscribe(TOPIC_CMD.c_str(), 0);
    Serial.print("Sub: "); Serial.println(TOPIC_CMD);
  } else {
    Serial.print("fail rc=");
    Serial.println(mqtt.state());
  }
}

static void mqttHousekeeping() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }
  mqttEnsureConnectedNonBlocking();
  if (mqtt.connected()) mqtt.loop();
}

// ================== MQTT TASK (CORE 0) ==================
static volatile bool mqttCore0Ready = false;

void mqttCore0Task(void* param) {
  mqttCore0Ready = true;
  while (true) {
    mqttHousekeeping();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ================== UTILITIES ==================
inline int smoothUpdateInt(int current,int target,int step=1){
  return (current<target) ? min(current+step,target) :
         (current>target) ? max(current-step,target) : current;
}

void drawOrangeBar(int x,int y,int w,int h,float percent,uint16_t color){
  M5.Display.fillRect(x,y,w,h,COLOR_BLACK);
  int fill = (w*percent)/100;
  M5.Display.fillRect(x,y,fill,h,color);
  M5.Display.drawRect(x,y,w,h,color);
}

void updateNumber(int x,int y,const char* text,uint16_t color){
  M5.Display.fillRect(x, y, strlen(text)*13, textOffset, COLOR_BLACK);
  M5.Display.setTextColor(color,COLOR_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(x,y);
  M5.Display.print(text);
}

// ================== HTTP PARSING ==================
int parseValue(const char* key, const char* data){
  const char* pos = strstr(data, key);
  return pos ? atoi(pos + strlen(key)) : 0;
}
float parseFloatValue(const char* key, const char* data){
  const char* pos = strstr(data, key);
  return pos ? atof(pos + strlen(key)) : 0.0;
}

void handleRoot(){
  if(!server.hasArg("text")){
    server.send(400,"text/plain","Missing 'text' parameter");
    return;
  }

  char buf[128];
  server.arg("text").toCharArray(buf, sizeof(buf));

  targetCPU     = parseValue("CPU:", buf);
  targetRAM     = parseValue("RAM:", buf);
  targetGPU     = parseValue("GPU:", buf);
  targetGPUtemp = parseValue("TEMP:", buf);
  targetUP      = (int)(parseFloatValue("UP:", buf) * 10);
  targetDN      = (int)(parseFloatValue("DN:", buf) * 10);

  lastUpdate = millis();
  server.send(200,"text/plain","Updated!");
}

// ================== HUD ==================
void displayHUD(){
  unsigned long now = millis();

  displayCPU     = smoothUpdateInt(displayCPU, targetCPU, 2);
  displayRAM     = smoothUpdateInt(displayRAM, targetRAM, 2);
  displayGPU     = smoothUpdateInt(displayGPU, targetGPU, 2);
  displayGPUtemp = smoothUpdateInt(displayGPUtemp, targetGPUtemp, 1);
  displayUP      = targetUP;
  displayDN      = targetDN;

  int yRAM = startY + textOffset + barHeight + spacing;
  int yGPU = yRAM + textOffset + barHeight + spacing;
  int yUP  = yGPU + textOffset + barHeight + spacing;
  int yDN  = yUP + textOffset + barHeight + spacing;

  char buf[32];

  uint16_t cpuColor = (displayCPU >= HIGH_USAGE_PERCENT) ? COLOR_WHITE : COLOR_BAR;
  if(displayCPU != lastCPU){
    sprintf(buf,"CPU:%d%% ",displayCPU);
    updateNumber(5,startY,buf,cpuColor);
    drawOrangeBar(5,startY+textOffset,M5.Display.width()-10,barHeight,displayCPU,cpuColor);
    lastCPU = displayCPU;
  }

  uint16_t ramColor = (displayRAM >= HIGH_USAGE_PERCENT) ? COLOR_WHITE : COLOR_BAR;
  if(displayRAM != lastRAM){
    sprintf(buf,"RAM:%d%% ",displayRAM);
    updateNumber(5,yRAM,buf,ramColor);
    drawOrangeBar(5,yRAM+textOffset,M5.Display.width()-10,barHeight,displayRAM,ramColor);
    lastRAM = displayRAM;
  }

  uint16_t gpuUsageColor = (displayGPU >= HIGH_USAGE_PERCENT) ? COLOR_WHITE : COLOR_BAR;
  if(displayGPU != lastGPU){
    sprintf(buf,"GPU:%d%% ",displayGPU);
    updateNumber(5,yGPU,buf,gpuUsageColor);
    drawOrangeBar(5,yGPU+textOffset,M5.Display.width()-10,barHeight,displayGPU,gpuUsageColor);
    lastGPU = displayGPU;
  }

  if(now - lastBlink >= BLINK_INTERVAL){
    lastBlink = now;
    if(displayGPUtemp >= GPU_TEMP_BLINK) gpuTempBlinkState = !gpuTempBlinkState;
  }

  uint16_t gpuTempColor = COLOR_BAR;
  bool blinkLED = false;

  if(displayGPUtemp >= GPU_TEMP_BLINK){
    gpuTempColor = gpuTempBlinkState ? COLOR_WHITE : COLOR_RED;
    blinkLED = gpuTempBlinkState;
  } else if(displayGPUtemp >= GPU_TEMP_WARN){
    gpuTempColor = COLOR_WHITE;
  }

  if(displayGPUtemp != lastGPUtemp){
    sprintf(buf,"Temp:%dC",displayGPUtemp);
    updateNumber(125,yGPU,buf,gpuTempColor);
    lastGPUtemp = displayGPUtemp;
  }

  float upVal = displayUP/10.0;
  uint16_t upColor = (upVal >= HIGH_SPEED_Mbps) ? COLOR_WHITE : COLOR_BAR;
  if(displayUP != lastUP){
    sprintf(buf,"UP:%.1f Mbps ",upVal);
    updateNumber(5,yUP,buf,upColor);
    drawOrangeBar(5,yUP+textOffset,M5.Display.width()-10,barHeight,(upVal/MAX_NET_Mbps)*100,upColor);
    lastUP = displayUP;
  }

  float dnVal = displayDN/10.0;
  uint16_t dnColor = (dnVal >= HIGH_SPEED_Mbps) ? COLOR_WHITE : COLOR_BAR;
  if(displayDN != lastDN){
    sprintf(buf,"DN:%.1f Mbps ",dnVal);
    updateNumber(5,yDN,buf,dnColor);
    drawOrangeBar(5,yDN+textOffset,M5.Display.width()-10,barHeight,(dnVal/MAX_NET_Mbps)*100,dnColor);
    lastDN = displayDN;
  }

  static bool lastLEDState = false;
  if(blinkLED != lastLEDState){
    M5.Power.setLed(blinkLED);
    lastLEDState = blinkLED;
  }
}

// ================== MATRIX TEXT ==================
void displaySengunScreen(){
  M5.Display.fillScreen(COLOR_BLACK);

  for(int i=0;i<MATRIX_COLS;i++){
    for(int t=0;t<MATRIX_TRAIL;t++){
      char c = 'A' + random(0,26);
      int brightness = max(60,255-t*80);
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(M5.Display.color565(0,brightness,0));
      M5.Display.setCursor(i*10,matrixY[i]-t*10);
      M5.Display.print(c);
    }
    matrixY[i]+=MATRIX_SPEED;
    if(matrixY[i]>M5.Display.height()) matrixY[i]=random(-20,0);
  }

  M5.Display.setTextSize(4);
  int startX = (M5.Display.width() - MATRIX_TEXT.length()*24)/2;
  int baseY = M5.Display.height()/2 - 12;

  for(int i=0;i<MATRIX_TEXT.length();i++){
    int randY = baseY + random(-5,6);
    uint16_t color = HackerColors[random(0,2)];
    M5.Display.setTextColor(color);
    M5.Display.setCursor(startX+i*24,randY);
    M5.Display.print(MATRIX_TEXT[i]);
  }

  M5.Power.setLed(false);
}

// ================== CLOCK ==================
void displayClockScreen() {
  M5.Display.fillScreen(COLOR_BLACK);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char buf[6];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  M5.Display.setTextSize(8);

  int charWidth = 40;
  int charHeight = 64;
  int startX = (M5.Display.width() - 5 * charWidth) / 2;
  int startY = (M5.Display.height() - charHeight) / 2;

  unsigned long now = millis();
  if (now - lastColonToggle >= 500) {
    lastColonToggle = now;
    colonVisible = !colonVisible;
  }

  for (int i = 0; i < 5; i++) {
    char c = buf[i];

    if (c == ':') {
      if (colonVisible) {
        M5.Display.setTextColor(COLOR_RED);
        M5.Display.setCursor(startX + i * charWidth, startY);
        M5.Display.print(':');
      }
      continue;
    }

    M5.Display.setCursor(startX + i * charWidth + random(-1,2), startY + random(-1,2));
    M5.Display.setTextColor(COLOR_RED);
    M5.Display.print(c);

    if (random(0, 100) < 15) {
      char glitchChar = (random(0, 100) < 50) ? '0' + random(0, 10) : 'A' + random(0, 26);
      M5.Display.setCursor(startX + i * charWidth + random(-6,7), startY + random(-6,7));
      M5.Display.setTextColor(COLOR_CYAN);
      M5.Display.print(glitchChar);
    }

    if (random(0, 100) < 5) {
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(COLOR_RED);
      M5.Display.setCursor(startX + i * charWidth + random(-20,20), startY + random(-20,20));
      M5.Display.print("HACKED");
      M5.Display.setTextSize(8);
    }
  }

  if (random(0, 100) < 10) {
    int lineY = startY + random(0, charHeight);
    M5.Display.drawFastHLine(startX, lineY, 5 * charWidth, COLOR_CYAN);
  }

  M5.Power.setLed(false);
}

// Wol auto screen show ID overlay
static void triggerIdOverlay(unsigned long now) {
  showIdOverlay = true;
  idOverlayUntil = now + ID_OVERLAY_MS;
  idOverlayDrawn = false;

  if (!screenIsOn) {
    M5.Display.wakeup();
    M5.Display.fillScreen(COLOR_BLACK);
    screenIsOn = true;
  }

  lastUpdate = now;
}

// ================== PROCESS MQTT EVENTS (MAIN CORE) ==================
static void processMqttEvents() {
  char payload[64];
  MqttEventType t = fetchEvent(payload, sizeof(payload));
  if (t == EVT_NONE) return;

  if (t == EVT_WOL_MAC) {
    String msg(payload);
    msg.trim();

    uint8_t mac[6];
    if (!parseMac(msg, mac)) {
      Serial.println("Invalid MAC payload. Expected AA:BB:CC:DD:EE:FF");
      return;
    }

    memcpy(lastMac, mac, 6);
    hasLastMac = true;

    sendWOL(mac);
    triggerIdOverlay(millis());
  }
}

// ================== SETUP ==================
void setup(){
  Serial.begin(115200);
  delay(100);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.fillScreen(COLOR_BLACK);
  M5.Display.setRotation(1);

  // Speaker
  M5.Speaker.begin();
  M5.Speaker.setVolume(128);

  // IDs
  rawId = String((uint32_t)ESP.getEfuseMac(), HEX);
  rawId.toLowerCase();
  deviceHash = sha256Hex(rawId);

  // Load saved UI settings
  preferences.begin("m5config", false);
  MATRIX_TEXT  = preferences.getString("matrixText", "M5 StickC");
  MAX_NET_Mbps = preferences.getFloat("maxNetMbps", 100.0);
  preferences.end();

  // ---- On-screen info during WiFi setup ----
  M5.Display.setTextSize(1);
  M5.Display.setCursor(75, 10);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("Hold ");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.print("^^^");
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.println(" reset");

  M5.Display.setCursor(30, 45);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("WiFi: ");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.println("M5-Setup");

  M5.Display.setCursor(55, 80);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("ID: ");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.println(rawId);

  M5.Display.setTextSize(1.5);
  M5.Display.setCursor(20, 120);
  M5.Display.setTextColor(COLOR_BAR);
  M5.Display.print("https://");
  M5.Display.setTextColor(COLOR_WHITE);
  M5.Display.print("wol.kreaxv.top");

  // ---- WiFiManager ----
  WiFiManager wm;

  wm.setTitle("M5StickC PLUS2");
  const char* menu[] = {"wifi"};
  wm.setMenu(menu, 1);

  const char* customCSS =
    "<style>"
    "input{border:2px solid #fd8020!important;}"
    "input:focus{border-color:#fd8020!important;outline:2px solid #fd8020!important;}"
    "</style>";
  wm.setCustomHeadElement(customCSS);

  char matrixTextBuf[11];
  char maxNetBuf[12];
  MATRIX_TEXT.toCharArray(matrixTextBuf, sizeof(matrixTextBuf));
  snprintf(maxNetBuf, sizeof(maxNetBuf), "%.1f", MAX_NET_Mbps);

  WiFiManagerParameter custom_matrix("matrix", "Matrix Text (max 10 chars)", matrixTextBuf, 11);
  WiFiManagerParameter custom_maxnet("maxnet", "Internet Speed Mbps", maxNetBuf, 12);

  wm.addParameter(&custom_matrix);
  wm.addParameter(&custom_maxnet);
  wm.setConfigPortalTimeout(180);
  wm.setMinimumSignalQuality(60);
  
  if (!wm.autoConnect("M5-Setup")) {
    M5.Display.fillScreen(COLOR_BLACK);
    M5.Display.setCursor(10, 50);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_RED);
    M5.Display.println("WiFi Failed");
    M5.Display.println("Restarting...");
    delay(2000);
    ESP.restart();
  }

  // Save custom parameters
  String newMatrixText = custom_matrix.getValue();
  float newMaxNet = atof(custom_maxnet.getValue());

  if (newMatrixText.length() > 0) MATRIX_TEXT = newMatrixText;
  if (newMaxNet > 0) MAX_NET_Mbps = newMaxNet;

  preferences.begin("m5config", false);
  preferences.putString("matrixText", MATRIX_TEXT);
  preferences.putFloat("maxNetMbps", MAX_NET_Mbps);
  preferences.end();

  M5.Display.fillScreen(COLOR_BLACK);

  // ---- MDNS / HTTP ----
  MDNS.begin("m5stick");
  server.on("/", handleRoot);
  server.begin();

  // ---- Random seed ----
  randomSeed(analogRead(0));
  for(int i=0;i<MATRIX_COLS;i++) matrixY[i]=random(-20, M5.Display.height());

  // ---- Clock ----
  setupClock();

  // ---- MQTT ----
  mqttInitOnce();
  mqttEnsureConnectedNonBlocking();

  xTaskCreatePinnedToCore(
    mqttCore0Task,
    "MQTT_Core0",
    8192,
    NULL,
    2,
    NULL,
    0
  );

  while (!mqttCore0Ready) delay(10);

  startTime = millis();

  Serial.print("rawId: "); Serial.println(rawId);
  Serial.print("hash : "); Serial.println(deviceHash);
  Serial.print("topic: "); Serial.println(TOPIC_CMD);
}

// ================== LOOP ==================
void loop(){
  M5.update();
  server.handleClient();

  // Handle MQTT events safely on main core
  processMqttEvents();

  unsigned long now = millis();

  // Button A: show ID overlay
  if (M5.BtnA.wasPressed()) {
    triggerIdOverlay(now);
  }

  // If overlay active: draw ONCE, then just wait
  if (showIdOverlay) {
    if ((long)(now - idOverlayUntil) < 0) {
      if (!idOverlayDrawn) {
        drawIdOverlay();
        idOverlayDrawn = true;
      }
      return;
    } else {
      showIdOverlay = false;
      idOverlayDrawn = false;

      M5.Display.fillScreen(COLOR_BLACK);
      lastCPU=lastRAM=lastGPU=lastGPUtemp=lastUP=lastDN=-1;
      lastSwitch = now;
    }
  }

  // Button B long press to reset all settings
  if (M5.BtnB.isPressed()) {
    if (!btnBPressed) {
      btnBPressed = true;
      btnBPressStart = now;
    } else if (now - btnBPressStart >= BTN_LONG_PRESS_MS) {
      M5.Display.fillScreen(COLOR_BLACK);
      M5.Display.setTextSize(2);
      M5.Display.setCursor(50, 65);
      M5.Display.setTextColor(COLOR_WHITE);
      M5.Display.println("RESETTING...");

      WiFiManager wm;
      wm.resetSettings();

      preferences.begin("m5config", false);
      preferences.clear();
      preferences.end();

      delay(1500);
      ESP.restart();
    }
  } else {
    btnBPressed = false;
  }

  // LCD SLEEP / WAKE
  if (now - lastUpdate > SCREEN_TIMEOUT) {
    if (screenIsOn) {
      M5.Display.sleep();
      screenIsOn = false;
    }
    return;
  } else {
    if (!screenIsOn) {
      M5.Display.wakeup();
      M5.Display.fillScreen(COLOR_BLACK);
      lastCPU=lastRAM=lastGPU=lastGPUtemp=lastUP=lastDN=-1;
      screenIsOn = true;
    }
  }

  // Daily reboot
  if(now - startTime >= DAILY_REBOOT_MS) ESP.restart();

  if(showMain && now - lastHUD >= HUD_INTERVAL){
    lastHUD = now;
    displayHUD();
    if(now - lastSwitch >= MAIN_DURATION){
      lastSwitch = now;
      showMain = false;
      showSengun = !showSengun;
    }
  }

  if(!showMain && now - lastMatrix >= MATRIX_INTERVAL){
    lastMatrix = now;
    if(showSengun) displaySengunScreen();
    else displayClockScreen();

    if(now - lastSwitch >= SCREEN_DURATION){
      lastSwitch = now;
      showMain = true;
      M5.Display.fillScreen(COLOR_BLACK);
      lastCPU=lastRAM=lastGPU=lastGPUtemp=lastUP=lastDN=-1;
    }
  }
}
