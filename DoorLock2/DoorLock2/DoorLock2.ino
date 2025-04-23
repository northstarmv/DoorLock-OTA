//finalv7 with ota updates
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <PubSubClient.h>

// ——— Async web server for OTA ———
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
// ————————————————————————————

//
// Pin Definitions
//
#define RELAY       D1
#define BUZZER      D8
#define IR_SENSOR   D2
#define RED_LED     D5
#define GREEN_LED   D6
#define DOOR_SENSOR D7

//
// Wi‑Fi Credentials
//
const char* ssid     = "DataStream_2.4";
const char* password = "armmd123!@#";

//
// Auth & WebSocket Server Config
//
const char* serverHost   = "54.255.15.253";
const int   serverPort   = 8081;
const char* authEndpoint = "/ws-auth";
const char* wsPath       = "/ws";

//
// Auth Payload
//
String apiKey = "OsC36TzcT3B4B6tRfKxKucr8veJoPux6GueO-mY0egQ=";
int branchId  = 1;
int deviceId  = 9;

//
// NTP Config
//
const char* ntpServer         = "pool.ntp.org";
const long  gmtOffset_sec     = 19800;
const int   daylightOffset_sec = 0;

//
// WebSocket & Token
//
WebSocketsClient webSocket;
String token         = "";
bool   doorUnlocking = false;

//
// Remote‑Debug MQTT (HiveMQ)
//
const char* debug_mqtt_server = "d681a6e1fecb4f01b1b5a72d2676e082.s1.eu.hivemq.cloud";
const int   debug_mqtt_port   = 8883;
const char* debug_mqtt_user   = "esplogger_2";
const char* debug_mqtt_pass   = "Esplogger2025";
const char* debugTopic        = "debug/log1";

WiFiClientSecure debugWiFiClient;
PubSubClient     debugMqttClient(debugWiFiClient);

//
// OTA Web Server (Async)
//
AsyncWebServer httpServer(80);

//
// Helpers: Remote‑Debug Logging
//
void remoteDebugLog(const String &msg) {
  Serial.println(msg);
  if (debugMqttClient.connected()) {
    debugMqttClient.publish(debugTopic, msg.c_str());
  }
}
void doorLog(const String &msg) { remoteDebugLog(msg); }

void connectToDebugMQTT() {
  debugWiFiClient.setInsecure();
  debugMqttClient.setServer(debug_mqtt_server, debug_mqtt_port);
  while (!debugMqttClient.connected()) {
    Serial.println("[DEBUG] Connecting to debug MQTT...");
    if (debugMqttClient.connect("esp8266-debug",
                                debug_mqtt_user,
                                debug_mqtt_pass)) {
      Serial.println("[DEBUG] Debug MQTT connected");
    } else {
      Serial.println("[DEBUG] Debug MQTT failed; retrying in 5s");
      delay(5000);
    }
  }
}

// Forward decls
bool   getToken();
void   startWebSocket();
void   unlockDoor();
void   lockDoorAndConfirm();
bool   isDoorClosed();
void   sendDoorOpened();
void   sendDoorClosed();
void   playBeep();
String getTimestamp();
void   webSocketEvent(WStype_t type, uint8_t *payload, size_t length);

void setup() {
  Serial.begin(115200);
  delay(500);

  // Pins
  pinMode(RELAY, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(IR_SENSOR, INPUT_PULLUP);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(DOOR_SENSOR, INPUT_PULLUP);
  digitalWrite(RELAY, HIGH);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);

  Serial.println("\n🔧 MAIN APP Booting...");

  // Wi‑Fi (renamed so you know this arrived OTA)
  Serial.printf("📡 [OTA APP] Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n✅ [OTA APP] IP: %s\n",
                WiFi.localIP().toString().c_str());

  // Debug MQTT
  connectToDebugMQTT();
  remoteDebugLog("[DEBUG] Remote debug MQTT connected");

  // NTP sync
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  remoteDebugLog("⏱ Waiting for NTP…");
  struct tm ti;
  while (!getLocalTime(&ti)) {
    remoteDebugLog("❌ NTP failed; retrying…");
    delay(1000);
  }
  remoteDebugLog("✅ NTP synced");

  // ————— ElegantOTA setup —————
  // no extra routes needed: /update is exposed by the library
  httpServer.begin();
  ElegantOTA.begin(&httpServer);
  remoteDebugLog("📡 OTA UI available: http://" +
                 WiFi.localIP().toString() + "/update");
  // ————————————————————————————

  // Token & WebSocket
  if (getToken()) {
    startWebSocket();
  } else {
    remoteDebugLog("❌ Token fetch failed; restarting");
    delay(1000);
    ESP.restart();
  }
}

void loop() {
  // Keep OTA portal alive
  ElegantOTA.loop();

  // Process WebSocket & MQTT
  webSocket.loop();
  debugMqttClient.loop();

  // IR-triggered door unlock
  if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
    doorLog("🚪 IR Sensor Triggered - Unlocking");
    doorUnlocking = true;
    unlockDoor();
    doorUnlocking = false;
  }
}

// -----------------------------------------------------
// Door Logic & Logging
// -----------------------------------------------------
bool isDoorClosed() {
  int s = digitalRead(DOOR_SENSOR);
  doorLog("🔍 Door: " + String(s) + (s==LOW? " CLOSED":" OPEN"));
  return s == LOW;
}

void unlockDoor() {
  doorLog("🔓 Unlock door");
  digitalWrite(RELAY, LOW);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  playBeep();

  // doorLog("⏳ Waiting open…");
  // unsigned long st = millis();
  // while (isDoorClosed()) {
  //   if (millis() - st > 10000) {
  //     doorLog("⌛ Timeout; re-lock");
  //     lockDoorAndConfirm();
  //     return;
  //   }
  //   delay(300);
  // }
  doorLog("⏳ Waiting open…");
  while (isDoorClosed()) {
    delay(300);  // Wait indefinitely until door is opened
  }
  doorLog("✅ Opened");
  sendDoorOpened();

  doorLog("⏳ Waiting close…");
  while (!isDoorClosed()) delay(300);
  doorLog("✅ Closed");
  lockDoorAndConfirm();
}

void lockDoorAndConfirm() {
  doorLog("🔒 Lock door");
  digitalWrite(RELAY, HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);
  sendDoorClosed();
}

void sendDoorClosed() {
  DynamicJsonDocument d(128);
  d["msgType"] = "isunlock";
  auto o = d.createNestedObject("data");
  o["isUnlock"] = false;
  o["timestamp"] = getTimestamp();
  String m; serializeJson(d, m);
  webSocket.sendTXT(m);
  doorLog("📤 Closed: " + m);
}

void sendDoorOpened() {
  DynamicJsonDocument d(128);
  d["msgType"] = "isunlock";
  auto o = d.createNestedObject("data");
  o["isUnlock"] = true;
  o["timestamp"] = getTimestamp();
  String m; serializeJson(d, m);
  webSocket.sendTXT(m);
  doorLog("📤 Opened: " + m);
}

void playBeep() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER, HIGH); delay(100);
    digitalWrite(BUZZER, LOW);  delay(100);
  }
}

// -----------------------------------------------------
// WebSocket Callback
// -----------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      doorLog("🔗 WS connected");
      break;
    case WStype_TEXT: {
      String msg; msg.reserve(length);
      for (size_t i = 0; i < length; i++) msg += (char)payload[i];
      doorLog("📩 WS msg: " + msg);
      if (msg.indexOf("door unlocked") >= 0) unlockDoor();
      break;
    }
    // case WStype_DISCONNECTED:
    //   doorLog("⚠️ WS disconnected; retry in 1m");
    //   delay(60000);
    //   if (getToken()) startWebSocket();
    //   break;
    case WStype_DISCONNECTED:
      doorLog("⚠️ WS disconnected; full reconnect in 15s");
      delay(15000); // Reconnect faster than 1 minute

      httpServer.end();                 // Fully restart the web server
      httpServer.begin();              //
      ElegantOTA.begin(&httpServer);   // Rebind OTA handler
      connectToDebugMQTT();            // Reconnect MQTT (HiveMQ)

      if (getToken()) startWebSocket(); // Re-establish WebSocket connection
      break;
    case WStype_ERROR:
      doorLog("❌ WS error");
      break;
  }
}

// -----------------------------------------------------
// HTTP Token Request
// -----------------------------------------------------
bool getToken() {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument b(256);
  b["connType"]="door-lock";
  b["id"]=deviceId;
  b["connId"]=JsonArray();
  b["apiKey"]=apiKey;
  b["branchId"]=branchId;
  String body; serializeJson(b, body);
  remoteDebugLog("🔑 Token req: " + body);

  int code = http.POST(body);
  if (code==200) {
    String r = http.getString();
    remoteDebugLog("✅ Token resp: " + r);
    DynamicJsonDocument js(256);
    if (!deserializeJson(js, r) && js.containsKey("token")) {
      token = js["token"].as<String>();
      remoteDebugLog("🔐 Token: " + token);
      http.end();
      return true;
    }
    remoteDebugLog("❌ Token parse error");
  } else {
    remoteDebugLog("❌ Token fail, code: " + String(code));
  }
  http.end();
  return false;
}

void startWebSocket() {
  String p = String(wsPath) + "?token=" + token;
  doorLog("🔌 WS connect: " + p);
  webSocket.begin(serverHost, serverPort, p);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

// -----------------------------------------------------
// ISO Timestamp Utility
// -----------------------------------------------------
String getTimestamp() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "2025-04-10T00:00:00Z";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &ti);
  return String(buf);
}




















// //otaintialcode
// #include <ESP8266WiFi.h>
// #include <ESPAsyncTCP.h>
// #include <ESPAsyncWebServer.h>
// #include <ElegantOTA.h>

// const char* ssid     = "DataStream_2.4";
// const char* password = "armmd123!@#";

// // Async server for OTA portal
// AsyncWebServer server(80);

// void setup() {
//   Serial.begin(115200);
//   delay(500);
//   Serial.println("\n🔧 OTA STUB Booting...");

//   Serial.printf("📡 [OTA STUB] WiFi: %s\n", ssid);
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ [OTA STUB] IP: %s\n",
//                 WiFi.localIP().toString().c_str());

//   // Minimal root so you know the stub is alive
//   server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
//     req->send(200, "text/plain", "OTA Stub Running");
//   });

//   server.begin();
//   Serial.println("✔ [OTA STUB] HTTP server started");

//   // Enable ElegantOTA UI
//   ElegantOTA.begin(&server);
//   Serial.println("✔ [OTA STUB] ElegantOTA → http://" 
//                  + WiFi.localIP().toString() + "/update");
// }

// void loop() {
//   // Required to keep OTA portal alive
//   ElegantOTA.loop();
// }








