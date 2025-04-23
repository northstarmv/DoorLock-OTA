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
String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
int branchId  = 1;
int deviceId  = 1;


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
const char* debug_mqtt_user   = "esplogger_1";
const char* debug_mqtt_pass   = "Esplogger2025";
const char* debugTopic        = "debug/log";

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
    //   doorLog("⚠️ WS disconnected; retry in 15seconds");
    //   delay(15000);
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












//ota initial code
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












//finalv6 with working debug log and other features
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>
// #include <time.h>
// #include <PubSubClient.h>  // For remote debug MQTT

// // -----------------------------------------------------
// // Pin Definitions (unchanged from finalv3)
// // -----------------------------------------------------
// #define RELAY       D1
// #define BUZZER      D8
// #define IR_SENSOR   D2
// #define RED_LED     D5
// #define GREEN_LED   D6
// #define DOOR_SENSOR D7

// // -----------------------------------------------------
// // WiFi Credentials
// // -----------------------------------------------------
// const char* ssid     = "DataStream_2.4";
// const char* password = "armmd123!@#";

// // -----------------------------------------------------
// // Server Configuration (for auth and WebSocket)
// // -----------------------------------------------------
// const char* serverHost   = "54.255.15.253";
// const int   serverPort   = 8081;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath       = "/ws";

// // -----------------------------------------------------
// // Auth Config (for token request)
// // -----------------------------------------------------
// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId  = 1;
// int deviceId  = 1;

// // -----------------------------------------------------
// // NTP Configuration
// // -----------------------------------------------------
// const char* ntpServer       = "pool.ntp.org";
// const long gmtOffset_sec    = 19800;
// const int  daylightOffset_sec = 0;

// // -----------------------------------------------------
// // WebSocket Variables (finalv3 code)
// // -----------------------------------------------------
// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;

// // -----------------------------------------------------
// // Remote Debug MQTT Configuration
// // (Using the HiveMQ broker credentials)
// // -----------------------------------------------------
// const char* debug_mqtt_server = "d681a6e1fecb4f01b1b5a72d2676e082.s1.eu.hivemq.cloud";
// const int   debug_mqtt_port   = 8883;
// const char* debug_mqtt_user   = "esplogger_1";
// const char* debug_mqtt_pass   = "Esplogger2025";
// const char* debugTopic        = "debug/log";

// WiFiClientSecure debugWiFiClient;
// PubSubClient debugMqttClient(debugWiFiClient);

// // -----------------------------------------------------
// // Remote Debug Setup and Helper Functions
// // -----------------------------------------------------
// void connectToDebugMQTT() {
//   debugWiFiClient.setInsecure(); // Accept server certificate without verification
//   debugMqttClient.setServer(debug_mqtt_server, debug_mqtt_port);
//   while (!debugMqttClient.connected()) {
//     Serial.println("[DEBUG] Connecting to debug MQTT...");
//     if (debugMqttClient.connect("esp8266-debug", debug_mqtt_user, debug_mqtt_pass)) {
//       Serial.println("[DEBUG] Debug MQTT connected");
//     } else {
//       Serial.println("[DEBUG] Debug MQTT connection failed. Retrying in 5s...");
//       delay(5000);
//     }
//   }
// }

// // This function sends a log message both to Serial and to the remote debug topic.
// void remoteDebugLog(const String &msg) {
//   Serial.println(msg);  // Print locally
//   if (debugMqttClient.connected()) {
//     bool success = debugMqttClient.publish(debugTopic, msg.c_str());
//     if (!success) {
//       Serial.println("[DEBUG] Publish to debug/log failed!");
//     }
//   }
// }

// // A helper for door and event messages.
// void doorLog(const String &msg) {
//   remoteDebugLog(msg);
// }

// // -----------------------------------------------------
// // Original finalv3 Setup with Remote Debug Integration
// // -----------------------------------------------------
// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   // Set Pin Modes
//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT_PULLUP);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");

//   // Connect to WiFi
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   // Connect to remote debug MQTT
//   connectToDebugMQTT();
//   remoteDebugLog("[DEBUG] Remote debug MQTT connected");

//   // Set up NTP time
//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   remoteDebugLog("⏱ Waiting for NTP time...");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     remoteDebugLog("❌ Failed to get time, retrying...");
//     delay(1000);
//   }
//   remoteDebugLog("✅ Time synchronized");

//   // Get token and start WebSocket
//   if (getToken()) {
//     startWebSocket();
//   } else {
//     remoteDebugLog("❌ Failed to get token. Restarting...");
//     delay(1000);
//     ESP.restart();
//   }
// }

// // -----------------------------------------------------
// // Finalv3 Loop (Door events only; Free heap messages are not published remotely)
// // -----------------------------------------------------
// void loop() {
//   webSocket.loop();
//   debugMqttClient.loop();  // Maintain debug MQTT connection

//   // Process door unlocking via IR sensor trigger
//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     doorLog("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//     doorUnlocking = false;
//   }
// }

// // -----------------------------------------------------
// // Door Control and Logging
// // -----------------------------------------------------
// bool isDoorClosed() {
//   int state = digitalRead(DOOR_SENSOR);
//   doorLog("🔍 Door Sensor State: " + String(state) + " (" + (state == LOW ? "CLOSED" : "OPEN") + ")");
//   return state == LOW;
// }

// void unlockDoor() {
//   doorLog("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();

//   doorLog("⏳ Waiting for door to open...");
//   unsigned long startTime = millis();
//   while (isDoorClosed()) {
//     if (millis() - startTime > 10000) {
//       doorLog("⌛ Timeout: Door not opened. Re-locking...");
//       lockDoorAndConfirm();
//       return;
//     }
//     delay(300);
//   }
//   doorLog("✅ Door opened (magnet separated)");
//   sendDoorOpened();

//   doorLog("⏳ Waiting for door to close...");
//   while (!isDoorClosed()) {
//     delay(300);
//   }
//   doorLog("✅ Door closed (magnet contact)");
//   lockDoorAndConfirm();
// }

// void lockDoorAndConfirm() {
//   doorLog("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);
//   sendDoorClosed();
// }

// // -----------------------------------------------------
// // JSON Messages for Door State
// // -----------------------------------------------------
// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   doorLog("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   doorLog("📤 Sent door OPENED event to server: " + msg);
// }

// // -----------------------------------------------------
// // Buzzer Beep Function
// // -----------------------------------------------------
// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// // -----------------------------------------------------
// // WebSocket Callback Function
// // -----------------------------------------------------
// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       doorLog("🔗 WebSocket connected");
//       break;
//     case WStype_TEXT:
//     {
//       String incomingMsg;
//       incomingMsg.reserve(length);
//       for (size_t i = 0; i < length; i++) {
//         incomingMsg += (char)payload[i];
//       }
//       doorLog("📩 Message: " + incomingMsg);
//       if (incomingMsg.indexOf("door unlocked") >= 0) {
//         unlockDoor();
//       }
//       break;
//     }
//     case WStype_DISCONNECTED:
//       doorLog("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
//       delay(60000);
//       if (getToken()) startWebSocket();
//       break;
//     case WStype_ERROR:
//       doorLog("❌ WebSocket Error!");
//       break;
//     default:
//       doorLog("ℹ️ WebSocket Event Type: " + String(type));
//       break;
//   }
// }

// // -----------------------------------------------------
// // HTTP Token Request
// // -----------------------------------------------------
// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   remoteDebugLog("\n🔑 Sending token request: " + requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     remoteDebugLog("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       remoteDebugLog("🔐 Parsed token: " + token);
//       http.end();
//       return true;
//     } else {
//       remoteDebugLog("❌ Token parse error");
//     }
//   } else {
//     remoteDebugLog("❌ HTTP POST failed, code: " + String(httpCode));
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   doorLog("🔌 Connecting to WebSocket at: ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// // -----------------------------------------------------
// // Get Timestamp in ISO Format
// // -----------------------------------------------------
// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     doorLog("⚠️ Failed to get local time. Using fallback.");
//     return "2025-04-10T00:00:00Z";
//   }
//   char isoTime[30];
//   strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(isoTime);
// }













//finalv5
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>
// #include <time.h>
// #include <PubSubClient.h>  // For remote debug MQTT

// // -----------------------------------------------------
// // Pin Definitions (unchanged from finalv3)
// // -----------------------------------------------------
// #define RELAY       D1
// #define BUZZER      D8
// #define IR_SENSOR   D2
// #define RED_LED     D5
// #define GREEN_LED   D6
// #define DOOR_SENSOR D7

// // -----------------------------------------------------
// // WiFi Credentials
// // -----------------------------------------------------
// const char* ssid     = "DataStream_2.4";
// const char* password = "armmd123!@#";

// // -----------------------------------------------------
// // Server Configuration (for auth and WebSocket)
// // -----------------------------------------------------
// const char* serverHost   = "54.255.15.253";
// const int   serverPort   = 8081;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath       = "/ws";

// // -----------------------------------------------------
// // Auth Config (for token request)
// // -----------------------------------------------------
// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId  = 1;
// int deviceId  = 1;

// // -----------------------------------------------------
// // NTP Configuration
// // -----------------------------------------------------
// const char* ntpServer       = "pool.ntp.org";
// const long gmtOffset_sec    = 19800;
// const int daylightOffset_sec = 0;

// // -----------------------------------------------------
// // Global Variables for WebSocket (finalv3 code)
// // -----------------------------------------------------
// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;

// // -----------------------------------------------------
// // Remote Debug MQTT Configuration
// // We use the same HiveMQ server but a separate topic "debug/log"
// // -----------------------------------------------------
// const char* debug_mqtt_server = "d681a6e1fecb4f01b1b5a72d2676e082.s1.eu.hivemq.cloud";
// const int   debug_mqtt_port   = 8883;
// const char* debug_mqtt_user   = "esplogger_1";
// const char* debug_mqtt_pass   = "Esplogger2025";
// const char* debugTopic        = "debug/log";

// WiFiClientSecure debugWiFiClient;
// PubSubClient debugMqttClient(debugWiFiClient);

// // -----------------------------------------------------
// // Global variable for remote debug free heap logging
// // -----------------------------------------------------
// unsigned long lastHeapPrint = 0;

// // -----------------------------------------------------
// // Remote Debug Functions
// // -----------------------------------------------------

// // Connect to the debug MQTT broker.
// void connectToDebugMQTT() {
//   debugWiFiClient.setInsecure(); // Accept server cert without verification
//   debugMqttClient.setServer(debug_mqtt_server, debug_mqtt_port);
//   while (!debugMqttClient.connected()) {
//     Serial.println("[DEBUG] Connecting to debug MQTT...");
//     if (debugMqttClient.connect("esp8266-debug", debug_mqtt_user, debug_mqtt_pass)) {
//       Serial.println("[DEBUG] Debug MQTT connected");
//     } else {
//       Serial.println("[DEBUG] Debug MQTT connection failed. Retrying in 5s...");
//       delay(5000);
//     }
//   }
// }

// // Remote debug log function with publish check.
// void remoteDebugLog(String msg) {
//   Serial.println(msg);
//   if (debugMqttClient.connected()) {
//     bool success = debugMqttClient.publish(debugTopic, msg.c_str());
//     if (!success) {
//       Serial.println("[DEBUG] Publish to debug/log failed!");
//     }
//   }
// }

// // -----------------------------------------------------
// // Original finalv3 Code (unchanged except for added debug calls)
// // -----------------------------------------------------

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   // Set Pin Modes
//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT_PULLUP);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");
  
//   // Connect to WiFi
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   // Connect to remote debug MQTT
//   connectToDebugMQTT();
//   remoteDebugLog("[DEBUG] Remote debug MQTT connected");

//   // Set up NTP time
//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.println("⏱ Waiting for NTP time...");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     Serial.println("❌ Failed to get time, retrying...");
//     delay(1000);
//   }
//   Serial.println("✅ Time synchronized");

//   if (getToken()) {
//     startWebSocket();
//   } else {
//     Serial.println("❌ Failed to get token. Restarting...");
//     delay(1000);
//     ESP.restart();
//   }
// }

// void loop() {
//   webSocket.loop();
//   debugMqttClient.loop(); // Maintain remote debug MQTT connection

//   // Every 5 seconds, send free heap value via remote debug
//   if (millis() - lastHeapPrint > 5000) {
//     remoteDebugLog("[DEBUG] Free heap: " + String(ESP.getFreeHeap()));
//     lastHeapPrint = millis();
//   }

//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     Serial.println("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//     doorUnlocking = false;
//   }
// }

// bool isDoorClosed() {
//   int state = digitalRead(DOOR_SENSOR);
//   Serial.printf("🔍 Door Sensor State: %d (%s)\n", state, state == LOW ? "CLOSED" : "OPEN");
//   return state == LOW;
// }

// void unlockDoor() {
//   Serial.println("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();

//   Serial.println("⏳ Waiting for door to open...");
//   unsigned long startTime = millis();
//   while (isDoorClosed()) {
//     if (millis() - startTime > 10000) {
//       Serial.println("⌛ Timeout: Door not opened. Re-locking...");
//       lockDoorAndConfirm();
//       return;
//     }
//     delay(300);
//   }
//   Serial.println("✅ Door opened (magnet separated)");
//   sendDoorOpened();

//   Serial.println("⏳ Waiting for door to close...");
//   while (!isDoorClosed()) {
//     delay(300);
//   }
//   Serial.println("✅ Door closed (magnet contact)");
//   lockDoorAndConfirm();
// }

// void lockDoorAndConfirm() {
//   Serial.println("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);
//   sendDoorClosed();
// }

// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door OPENED event to server: " + msg);
// }

// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       Serial.println("🔗 WebSocket connected");
//       break;
//     case WStype_TEXT:
//       Serial.printf("📩 Message: %s\n", (char*)payload);
//       if (String((char*)payload).indexOf("door unlocked") >= 0) {
//         unlockDoor();
//       }
//       break;
//     case WStype_DISCONNECTED:
//       Serial.println("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
//       delay(60000);
//       if (getToken()) startWebSocket();
//       break;
//     case WStype_ERROR:
//       Serial.println("❌ WebSocket Error!");
//       break;
//     default:
//       Serial.print("ℹ️ WebSocket Event Type: ");
//       Serial.println(type);
//       break;
//   }
// }

// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   Serial.print("\n🔑 Sending token request: ");
//   Serial.println(requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     Serial.println("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       Serial.println("🔐 Parsed token: " + token);
//       return true;
//     } else {
//       Serial.println("❌ Token parse error");
//     }
//   } else {
//     Serial.printf("❌ HTTP POST failed, code: %d\n", httpCode);
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   Serial.print("🔌 Connecting to WebSocket at: ");
//   Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     Serial.println("⚠️ Failed to get local time. Using fallback.");
//     return "2025-04-10T00:00:00Z";
//   }
//   char isoTime[30];
//   strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(isoTime);
// }




















//finalv4
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>
// #include <time.h>
// #include <PubSubClient.h>  // For remote debug MQTT

// // -----------------------------------------------------
// // Pin Definitions (unchanged from finalv3)
// // -----------------------------------------------------
// #define RELAY       D1
// #define BUZZER      D8
// #define IR_SENSOR   D2
// #define RED_LED     D5
// #define GREEN_LED   D6
// #define DOOR_SENSOR D7

// // -----------------------------------------------------
// // WiFi Credentials
// // -----------------------------------------------------
// const char* ssid     = "DataStream_2.4";
// const char* password = "armmd123!@#";

// // -----------------------------------------------------
// // Server Configuration (for auth and WebSocket)
// // -----------------------------------------------------
// const char* serverHost   = "54.255.15.253";
// const int   serverPort   = 8081;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath       = "/ws";

// // -----------------------------------------------------
// // Auth Config (for token request)
// // -----------------------------------------------------
// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId  = 1;
// int deviceId  = 1;

// // -----------------------------------------------------
// // NTP Configuration
// // -----------------------------------------------------
// const char* ntpServer       = "pool.ntp.org";
// const long gmtOffset_sec    = 19800;
// const int daylightOffset_sec = 0;

// // -----------------------------------------------------
// // Global Variables for WebSocket (finalv3 code)
// // -----------------------------------------------------
// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;

// // -----------------------------------------------------
// // Remote Debug MQTT Configuration
// // We use the same HiveMQ server but a separate topic "debug/log"
// // -----------------------------------------------------
// const char* debug_mqtt_server = "d681a6e1fecb4f01b1b5a72d2676e082.s1.eu.hivemq.cloud";
// const int   debug_mqtt_port   = 8883;
// const char* debug_mqtt_user   = "esplogger_1";
// const char* debug_mqtt_pass   = "Esplogger2025";
// const char* debugTopic        = "debug/log";

// WiFiClientSecure debugWiFiClient;
// PubSubClient debugMqttClient(debugWiFiClient);

// // -----------------------------------------------------
// // Global variable for remote debug free heap logging
// // -----------------------------------------------------
// unsigned long lastHeapPrint = 0;

// // -----------------------------------------------------
// // Remote Debug Functions
// // -----------------------------------------------------

// // Connect to the debug MQTT broker.
// void connectToDebugMQTT() {
//   debugWiFiClient.setInsecure(); // Accept server cert without verification (minimal overhead)
//   debugMqttClient.setServer(debug_mqtt_server, debug_mqtt_port);
//   while (!debugMqttClient.connected()) {
//     Serial.println("[DEBUG] Connecting to debug MQTT...");
//     if (debugMqttClient.connect("esp8266-debug", debug_mqtt_user, debug_mqtt_pass)) {
//       Serial.println("[DEBUG] Debug MQTT connected");
//     } else {
//       Serial.println("[DEBUG] Debug MQTT connection failed. Retrying in 5s...");
//       delay(5000);
//     }
//   }
// }

// // Publish a debug message remotely and also print locally.
// void remoteDebugLog(String msg) {
//   Serial.println(msg);
//   if (debugMqttClient.connected()) {
//     debugMqttClient.publish(debugTopic, msg.c_str());
//   }
// }

// // -----------------------------------------------------
// // Original finalv3 Code (unchanged except for added debug calls)
// // -----------------------------------------------------

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   // Set Pin Modes
//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT_PULLUP);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");
  
//   // Connect to WiFi
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   // Connect to remote debug MQTT
//   connectToDebugMQTT();
//   remoteDebugLog("[DEBUG] Remote debug MQTT connected");

//   // Set up NTP time
//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.println("⏱ Waiting for NTP time...");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     Serial.println("❌ Failed to get time, retrying...");
//     delay(1000);
//   }
//   Serial.println("✅ Time synchronized");

//   // Get token and start WebSocket
//   if (getToken()) {
//     startWebSocket();
//   } else {
//     Serial.println("❌ Failed to get token. Restarting...");
//     delay(1000);
//     ESP.restart();
//   }
// }

// void loop() {
//   webSocket.loop();
//   debugMqttClient.loop(); // Maintain remote debug MQTT connection

//   // Every 5 seconds, send free heap value via remote debug
//   if (millis() - lastHeapPrint > 5000) {
//     remoteDebugLog("[DEBUG] Free heap: " + String(ESP.getFreeHeap()));
//     lastHeapPrint = millis();
//   }

//   // Process door unlocking via IR sensor trigger
//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     Serial.println("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//     doorUnlocking = false;
//   }
// }

// bool isDoorClosed() {
//   int state = digitalRead(DOOR_SENSOR);
//   Serial.printf("🔍 Door Sensor State: %d (%s)\n", state, state == LOW ? "CLOSED" : "OPEN");
//   return state == LOW;
// }

// void unlockDoor() {
//   Serial.println("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();

//   // Wait for door to open (magnet separated)
//   Serial.println("⏳ Waiting for door to open...");
//   unsigned long startTime = millis();
//   while (isDoorClosed()) {
//     if (millis() - startTime > 10000) {
//       Serial.println("⌛ Timeout: Door not opened. Re-locking...");
//       lockDoorAndConfirm();
//       return;
//     }
//     delay(300);
//   }
//   Serial.println("✅ Door opened (magnet separated)");
//   sendDoorOpened();

//   // Wait for door to close (magnet in contact)
//   Serial.println("⏳ Waiting for door to close...");
//   while (!isDoorClosed()) {
//     delay(300);
//   }
//   Serial.println("✅ Door closed (magnet contact)");
//   lockDoorAndConfirm();
// }

// void lockDoorAndConfirm() {
//   Serial.println("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);
//   sendDoorClosed();
// }

// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door OPENED event to server: " + msg);
// }

// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       Serial.println("🔗 WebSocket connected");
//       break;
//     case WStype_TEXT:
//       Serial.printf("📩 Message: %s\n", (char*)payload);
//       if (String((char*)payload).indexOf("door unlocked") >= 0) {
//         unlockDoor();
//       }
//       break;
//     case WStype_DISCONNECTED:
//       Serial.println("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
//       delay(60000);
//       if (getToken()) startWebSocket();
//       break;
//     case WStype_ERROR:
//       Serial.println("❌ WebSocket Error!");
//       break;
//     default:
//       Serial.print("ℹ️ WebSocket Event Type: ");
//       Serial.println(type);
//       break;
//   }
// }

// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   Serial.print("\n🔑 Sending token request: ");
//   Serial.println(requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     Serial.println("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       Serial.println("🔐 Parsed token: " + token);
//       return true;
//     } else {
//       Serial.println("❌ Token parse error");
//     }
//   } else {
//     Serial.printf("❌ HTTP POST failed, code: %d\n", httpCode);
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   Serial.print("🔌 Connecting to WebSocket at: ");
//   Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     Serial.println("⚠️ Failed to get local time. Using fallback.");
//     return "2025-04-10T00:00:00Z";
//   }
//   char isoTime[30];
//   strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(isoTime);
// }










//finalv3
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>
// #include <time.h>

// // Pin Definitions
// #define RELAY D1
// #define BUZZER D8
// #define IR_SENSOR D2
// #define RED_LED D5
// #define GREEN_LED D6
// #define DOOR_SENSOR D7

// // WiFi Credentials
// const char* ssid = "DataStream_2.4";
// const char* password = "armmd123!@#";

// // Server Configuration
// const char* serverHost = "54.255.15.253";
// const int serverPort = 8081;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath = "/ws";

// // Auth Config
// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId = 1;
// int deviceId = 1;

// // NTP Configuration
// const char* ntpServer = "pool.ntp.org";
// const long gmtOffset_sec = 19800;
// const int daylightOffset_sec = 0;

// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   // Pin Modes
//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT_PULLUP);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");
//   WiFi.begin(ssid, password);
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.println("⏱ Waiting for NTP time...");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     Serial.println("❌ Failed to get time, retrying...");
//     delay(1000);
//   }
//   Serial.println("✅ Time synchronized");

//   if (getToken()) {
//     startWebSocket();
//   } else {
//     Serial.println("❌ Failed to get token. Restarting...");
//     delay(1000);
//     ESP.restart();
//   }
// }

// void loop() {
//   webSocket.loop();

//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     Serial.println("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//     doorUnlocking = false;
//   }
// }

// bool isDoorClosed() {
//   int state = digitalRead(DOOR_SENSOR);
//   Serial.printf("🔍 Door Sensor State: %d (%s)\n", state, state == LOW ? "CLOSED" : "OPEN");
//   return state == LOW;
// }

// void unlockDoor() {
//   Serial.println("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();

//   // Wait for door to open (magnet separated)
//   Serial.println("⏳ Waiting for door to open...");
//   unsigned long startTime = millis();
//   while (isDoorClosed()) {
//     if (millis() - startTime > 10000) {
//       Serial.println("⌛ Timeout: Door not opened. Re-locking...");
//       lockDoorAndConfirm();
//       return;
//     }
//     delay(300);
//   }
//   Serial.println("✅ Door opened (magnet separated)");
//   sendDoorOpened();

//   // Wait for door to close (magnet in contact)
//   Serial.println("⏳ Waiting for door to close...");
//   while (!isDoorClosed()) {
//     delay(300);
//   }
//   Serial.println("✅ Door closed (magnet contact)");
//   lockDoorAndConfirm();
// }

// void lockDoorAndConfirm() {
//   Serial.println("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);
//   sendDoorClosed();
// }

// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door OPENED event to server: " + msg);
// }

// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       Serial.println("🔗 WebSocket connected");
//       break;
//     case WStype_TEXT:
//       Serial.printf("📩 Message: %s\n", (char*)payload);
//       if (String((char*)payload).indexOf("door unlocked") >= 0) {
//         unlockDoor();
//       }
//       break;
//     case WStype_DISCONNECTED:
//       Serial.println("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
//       delay(60000);
//       if (getToken()) startWebSocket();
//       break;
//     case WStype_ERROR:
//       Serial.println("❌ WebSocket Error!");
//       break;
//     default:
//       Serial.print("ℹ️ WebSocket Event Type: ");
//       Serial.println(type);
//       break;
//   }
// }

// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   Serial.print("\n🔑 Sending token request: ");
//   Serial.println(requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     Serial.println("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       Serial.println("🔐 Parsed token: " + token);
//       return true;
//     } else {
//       Serial.println("❌ Token parse error");
//     }
//   } else {
//     Serial.printf("❌ HTTP POST failed, code: %d\n", httpCode);
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   Serial.print("🔌 Connecting to WebSocket at: ");
//   Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     Serial.println("⚠️ Failed to get local time. Using fallback.");
//     return "2025-04-10T00:00:00Z";
//   }
//   char isoTime[30];
//   strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(isoTime);
// }























//finalv2
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>
// #include <time.h>

// #define RELAY D1
// #define BUZZER D8
// #define IR_SENSOR D2
// #define RED_LED D5
// #define GREEN_LED D6
// #define DOOR_SENSOR D7

// const char* ssid = "DataStream_2.4";
// const char* password = "armmd123!@#";

// const char* serverHost = "54.255.15.253";
// const int serverPort = 8081;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath = "/ws";

// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId = 1;
// int deviceId = 1;

// // NTP config
// const char* ntpServer = "pool.ntp.org";
// const long gmtOffset_sec = 19800;      // Sri Lanka = UTC +5:30
// const int daylightOffset_sec = 0;

// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;
// bool waitingForClose = false;

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");
//   WiFi.begin(ssid, password);
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.println("⏱ Waiting for NTP time...");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     Serial.println("❌ Failed to get time, retrying...");
//     delay(1000);
//   }
//   Serial.println("✅ Time synchronized");

//   if (getToken()) {
//     startWebSocket();
//   } else {
//     Serial.println("❌ Failed to get token. Restarting...");
//     delay(1000);
//     ESP.restart();
//   }
// }

// void loop() {
//   webSocket.loop();

//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     Serial.println("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//   }

//   if (waitingForClose && digitalRead(DOOR_SENSOR) == LOW) {
//     Serial.println("✅ Door closed detected (magnetic sensor)");
//     lockDoorAndConfirm();
//   }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       Serial.println("🔗 WebSocket connected");
//       break;

//     case WStype_TEXT:
//       Serial.printf("📩 Message: %s\n", (char*)payload);
//       if (String((char*)payload).indexOf("door unlocked") >= 0) {
//         Serial.println("🚪 Unlock command received from server");
//         unlockDoor();
//       }
//       break;

//     case WStype_DISCONNECTED:
//       Serial.println("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
//       delay(60000);
//       if (getToken()) startWebSocket();
//       break;

//     case WStype_ERROR:
//       Serial.println("❌ WebSocket Error!");
//       break;

//     default:
//       Serial.print("ℹ️ WebSocket Event Type: ");
//       Serial.println(type);
//       break;
//   }
// }

// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   Serial.print("\n🔑 Sending token request: ");
//   Serial.println(requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     Serial.println("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       Serial.println("🔐 Parsed token: " + token);
//       return true;
//     } else {
//       Serial.println("❌ Token parse error");
//     }
//   } else {
//     Serial.printf("❌ HTTP POST failed, code: %d\n", httpCode);
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   Serial.print("🔌 Connecting to WebSocket at: ");
//   Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// void unlockDoor() {
//   Serial.println("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();
//   sendDoorOpened();
//   waitingForClose = true;
// }

// void lockDoorAndConfirm() {
//   Serial.println("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);

//   waitingForClose = false;
//   doorUnlocking = false;
//   sendDoorClosed();
// }

// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door OPENED event to server: " + msg);
// }

// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     Serial.println("⚠️ Failed to get local time. Using fallback.");
//     return "2025-04-10T00:00:00Z";
//   }
//   char isoTime[30];
//   strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
//   return String(isoTime);
// }


















//finalv1
// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <ESP8266HTTPClient.h>

// #define RELAY D1
// #define BUZZER D8
// #define IR_SENSOR D2
// #define RED_LED D5
// #define GREEN_LED D6
// #define DOOR_SENSOR D7

// const char* ssid = "SLT-4G_16D724";
// const char* password = "A5592AE7";

// const char* serverHost = "192.168.1.103";
// const int serverPort = 8080;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath = "/ws";

// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId = 1;
// int deviceId = 1;

// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;
// bool waitingForClose = false;

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   pinMode(RELAY, OUTPUT);
//   pinMode(BUZZER, OUTPUT);
//   pinMode(IR_SENSOR, INPUT_PULLUP);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(DOOR_SENSOR, INPUT);

//   digitalWrite(RELAY, HIGH);
//   digitalWrite(RED_LED, HIGH);
//   digitalWrite(GREEN_LED, LOW);

//   Serial.println("\n🔧 Booting...");
//   WiFi.begin(ssid, password);
//   Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

//   if (getToken()) {
//     startWebSocket();
//   } else {
//     Serial.println("❌ Failed to get token. Restarting...");
//     ESP.restart();
//   }
// }

// void loop() {
//   webSocket.loop();

//   // static unsigned long lastPing = 0;
//   // if (millis() - lastPing > 30000) {
//   //   lastPing = millis();
//   //   webSocket.sendTXT("PING");
//   //   Serial.println("📡 Sent WebSocket Keepalive Ping");
//   // }

//   if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//     Serial.println("🚪 IR Sensor Triggered - Unlocking");
//     doorUnlocking = true;
//     unlockDoor();
//   }

//   if (waitingForClose && digitalRead(DOOR_SENSOR) == LOW) {
//     Serial.println("✅ Door closed detected (magnetic sensor)");
//     lockDoorAndConfirm();
//   }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//   switch (type) {
//     case WStype_CONNECTED:
//       Serial.println("🔗 WebSocket connected");
//       break;

//     case WStype_TEXT:
//       Serial.printf("📩 Message: %s\n", (char*)payload);
//       if (String((char*)payload).indexOf("door unlocked") >= 0) {
//         Serial.println("🚪 Unlock command received from server");
//         unlockDoor();
//       }
//       break;

//     case WStype_DISCONNECTED:
//       Serial.println("⚠️ WebSocket disconnected. Trying to reconnect...");
//       startWebSocket();
//       break;

//     case WStype_ERROR:
//       Serial.println("❌ WebSocket Error!");
//       break;

//     default:
//       Serial.print("ℹ️ WebSocket Event Type: ");
//       Serial.println(type);
//       break;
//   }
// }

// bool getToken() {
//   WiFiClient client;
//   HTTPClient http;

//   String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//   http.begin(client, url);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument body(256);
//   body["connType"] = "door-lock";
//   body["id"] = deviceId;
//   body["connId"] = JsonArray();
//   body["apiKey"] = apiKey;
//   body["branchId"] = branchId;

//   String requestBody;
//   serializeJson(body, requestBody);
//   Serial.print("\n🔑 Sending token request: ");
//   Serial.println(requestBody);

//   int httpCode = http.POST(requestBody);
//   if (httpCode == 200) {
//     String response = http.getString();
//     Serial.println("✅ Token response: " + response);

//     DynamicJsonDocument responseDoc(256);
//     DeserializationError error = deserializeJson(responseDoc, response);
//     if (!error && responseDoc.containsKey("token")) {
//       token = responseDoc["token"].as<String>();
//       Serial.println("🔐 Parsed token: " + token);
//       return true;
//     } else {
//       Serial.println("❌ Token parse error");
//     }
//   } else {
//     Serial.printf("❌ HTTP POST failed, code: %d\n", httpCode);
//   }

//   http.end();
//   return false;
// }

// void startWebSocket() {
//   String fullPath = String(wsPath) + "?token=" + token;
//   Serial.print("🔌 Connecting to WebSocket at: ");
//   Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);

//   webSocket.begin(serverHost, serverPort, fullPath);
//   webSocket.onEvent(webSocketEvent);
//   webSocket.setReconnectInterval(5000);
// }

// void unlockDoor() {
//   Serial.println("🔓 Unlocking door...");
//   digitalWrite(RELAY, LOW);
//   digitalWrite(GREEN_LED, HIGH);
//   digitalWrite(RED_LED, LOW);
//   playBeep();
//   sendDoorOpened();
//   waitingForClose = true;
// }

// void lockDoorAndConfirm() {
//   Serial.println("🔒 Locking door...");
//   digitalWrite(RELAY, HIGH);
//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, HIGH);

//   waitingForClose = false;
//   doorUnlocking = false;
//   sendDoorClosed();
// }

// void playBeep() {
//   for (int i = 0; i < 3; i++) {
//     digitalWrite(BUZZER, HIGH);
//     delay(100);
//     digitalWrite(BUZZER, LOW);
//     delay(100);
//   }
// }

// void sendDoorClosed() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = false;
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door closed event to server: " + msg);
// }

// void sendDoorOpened() {
//   DynamicJsonDocument doc(128);
//   doc["msgType"] = "isunlock";
//   JsonObject data = doc.createNestedObject("data");
//   data["isUnlock"] = true;  // true means door opened
//   data["timestamp"] = getTimestamp();

//   String msg;
//   serializeJson(doc, msg);
//   webSocket.sendTXT(msg);
//   Serial.println("📤 Sent door OPENED event to server: " + msg);
// }


// String getTimestamp() {
//   return "2025-04-10T00:00:00Z"; // Replace with NTP sync later if needed
// }































// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>
// #include <WiFiClientSecure.h>
// #include <ESP8266HTTPClient.h>

// #define RELAY D1
// #define BUZZER D8
// #define IR_SENSOR D2
// #define RED_LED D5
// #define GREEN_LED D6
// #define DOOR_SENSOR D7

// const char* ssid = "SLT-4G_16D724";
// const char* password = "A5592AE7";


// const char* serverHost = "192.168.1.103";
// const int serverPort = 8080;
// const char* authEndpoint = "/ws-auth";
// const char* wsPath = "/ws";

// // Provided credentials
// String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
// int branchId = 1;
// int deviceId = 1;

// WebSocketsClient webSocket;
// String token = "";
// bool doorUnlocking = false;
// bool waitingForClose = false;

// void setup() {
//     Serial.begin(115200);

//     pinMode(RELAY, OUTPUT);
//     pinMode(BUZZER, OUTPUT);
//     pinMode(IR_SENSOR, INPUT_PULLUP);
//     pinMode(RED_LED, OUTPUT);
//     pinMode(GREEN_LED, OUTPUT);
//     pinMode(DOOR_SENSOR, INPUT);

//     digitalWrite(RELAY, HIGH);  // locked
//     digitalWrite(RED_LED, HIGH);
//     digitalWrite(GREEN_LED, LOW);

//     WiFi.begin(ssid, password);
//     Serial.print("Connecting to WiFi");
//     while (WiFi.status() != WL_CONNECTED) {
//         delay(500); Serial.print(".");
//     }
//     Serial.println("\n✅ WiFi connected");

//     if (getToken()) {
//         startWebSocket();
//     } else {
//         Serial.println("❌ Failed to get token. Restarting...");
//         ESP.restart();
//     }
// }

// void loop() {
//     webSocket.loop();

//     static unsigned long lastPing = 0;
//     if (millis() - lastPing > 30000) {
//         lastPing = millis();
//         webSocket.sendTXT("PING");
//         Serial.println("📡 Sent WebSocket Keepalive Ping");
//     }

//     if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
//         Serial.println("🚪 IR Sensor Triggered - Unlocking");
//         doorUnlocking = true;
//         unlockDoor();
//     }

//     if (waitingForClose && digitalRead(DOOR_SENSOR) == LOW) {
//         Serial.println("✅ Door closed detected");
//         lockDoorAndConfirm();
//     }
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//     if (type == WStype_CONNECTED) {
//         Serial.println("✅ WebSocket connected");

//         DynamicJsonDocument doc(128);
//         doc["msgType"] = "connect-id";
//         JsonObject data = doc.createNestedObject("data");
//         data["isUnlock"] = false;
//         data["timestamp"] = getTimestamp();

//         String message;
//         serializeJson(doc, message);
//         webSocket.sendTXT(message);
//     }

//     if (type == WStype_TEXT) {
//         String msg = String((char*)payload);
//         Serial.print("📩 Received: ");
//         Serial.println(msg);
//         if (msg.indexOf("door unlocked") >= 0) {
//             Serial.println("🚪 Unlock command received");
//             doorUnlocking = true;
//             unlockDoor();
//         }
//     }

//     if (type == WStype_DISCONNECTED) {
//         Serial.println("⚠️ WebSocket disconnected. Reconnecting...");
//         delay(5000);
//         startWebSocket();
//     }
// }

// void unlockDoor() {
//     Serial.println("🔓 Unlocking door...");
//     digitalWrite(RELAY, LOW);
//     digitalWrite(GREEN_LED, HIGH);
//     digitalWrite(RED_LED, LOW);
//     playBeep();

//     waitingForClose = true;
// }

// void lockDoorAndConfirm() {
//     Serial.println("🔒 Locking door...");
//     digitalWrite(RELAY, HIGH);
//     digitalWrite(GREEN_LED, LOW);
//     digitalWrite(RED_LED, HIGH);

//     waitingForClose = false;
//     doorUnlocking = false;

//     sendDoorClosed();
// }

// void playBeep() {
//     for (int i = 0; i < 3; i++) {
//         digitalWrite(BUZZER, HIGH);
//         delay(100);
//         digitalWrite(BUZZER, LOW);
//         delay(100);
//     }
// }




// bool getToken() {
//     WiFiClient client;
//     HTTPClient http;

//     String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;
//     http.begin(client, url);
//     http.addHeader("Content-Type", "application/json");

//     DynamicJsonDocument doc(256);
//     doc["connType"] = "door-lock";
//     doc["id"] = deviceId;
//     doc["connId"] = JsonArray();
//     doc["apiKey"] = apiKey;
//     doc["branchId"] = branchId;

//     String body;
//     serializeJson(doc, body);
//     Serial.print("🔑 Requesting token... ");

//     int httpCode = http.POST(body);
//     if (httpCode > 0) {
//         String response = http.getString();
//         Serial.println("✅ Token received: " + response);

//         // ✅ Properly extract just the token field
//         DynamicJsonDocument resDoc(256);
//         DeserializationError err = deserializeJson(resDoc, response);
//         if (err) {
//             Serial.println("❌ Failed to parse token JSON");
//             http.end();
//             return false;
//         }

//         token = resDoc["token"].as<String>();  // ✅ Only the token value
//         http.end();
//         return true;
//     } else {
//         Serial.printf("❌ HTTP POST failed: %d\n", httpCode);
//         http.end();
//         return false;
//     }
// }


// void startWebSocket() {
//     String fullPath = String(wsPath) + "?token=" + token;
//     Serial.println("🔗 Connecting to WebSocket at: ws://" + String(serverHost) + ":" + serverPort + fullPath);
//     webSocket.begin(serverHost, serverPort, fullPath);  // Changed to ws (non-SSL)
//     webSocket.onEvent(webSocketEvent);
//     webSocket.setReconnectInterval(5000);
// }



// void sendDoorClosed() {
//     DynamicJsonDocument doc(256);
//     doc["msgType"] = "isunlock";
//     JsonObject data = doc.createNestedObject("data");
//     data["isUnlock"] = false;
//     data["timestamp"] = getTimestamp();

//     String msg;
//     serializeJson(doc, msg);
//     webSocket.sendTXT(msg);
//     Serial.println("✅ Sent door closed status to server");
// }

// String getTimestamp() {
//     return "2025-04-09T15:00:00Z";  // Placeholder; you can replace with NTP sync if needed
// }
























































// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>

// #define RELAY D1
// #define BUZZER D8   
// #define SWITCH D2   
// #define RED_LED D5
// #define GREEN_LED D6
// #define BLUE_LED D7

// const char* ssid = "DataStream_2.4";       
// const char* password = "armmd123!@#";  

// const char* websocket_host = "192.168.1.10";  
// const int websocket_port = 8080;              
// const char* websocket_path = "/ws";

// WebSocketsClient webSocket;
// String gymID = "G124";  
// bool doorUnlocking = false;

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//     switch (type) {
//         case WStype_DISCONNECTED:
//             Serial.println("⚠️ WebSocket Disconnected! Attempting Reconnect...");
//             break;

//         case WStype_CONNECTED: {
//             Serial.println("✅ WebSocket Connected to Local Server!");

//             DynamicJsonDocument doc(256);
//             String jsonStr;

//             doc["type"] = "REGISTER";
//             doc["gymID"] = gymID;

//             serializeJson(doc, jsonStr);
//             Serial.print("📡 Sending JSON Gym ID: ");
//             Serial.println(jsonStr);
//             webSocket.sendTXT(jsonStr);
//             break;
//         }

//         case WStype_TEXT:
//             Serial.print("📩 Received from server: ");
//             Serial.println((char*)payload);

//             if (String((char*)payload) == "UNLOCK") {
//                 Serial.println("🚪 WebSocket Unlock Command Received - Unlocking Door...");
                
                
//                 delay(50);  

//                 unlockDoor();
//             }
//             break;

//         case WStype_ERROR:
//             Serial.println("❌ WebSocket Error!");
//             break;

//         default:
//             Serial.println("ℹ️ WebSocket Event Received.");
//             break;
//     }
// }

// void setup() {
//     Serial.begin(115200);
//     Serial.println("\n🔹 ESP8266 Booting...");

//     pinMode(RELAY, OUTPUT);
//     pinMode(BUZZER, OUTPUT);
//     pinMode(SWITCH, INPUT_PULLUP);  
//     pinMode(RED_LED, OUTPUT);
//     pinMode(GREEN_LED, OUTPUT);
//     pinMode(BLUE_LED, OUTPUT);

//     digitalWrite(RELAY, HIGH);
//     digitalWrite(RED_LED, HIGH);
//     digitalWrite(GREEN_LED, LOW);
//     digitalWrite(BUZZER, LOW);

//     // 🔹 Connect to WiFi
//     Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//     WiFi.begin(ssid, password);
//     int attempt = 0;

//     while (WiFi.status() != WL_CONNECTED) {
//         delay(500);
//         Serial.print(".");
//         attempt++;
//         if (attempt > 30) {  
//             Serial.println("\n❌ WiFi Connection Failed! Restarting ESP...");
//             ESP.restart();
//         }
//     }

//     Serial.println("\n✅ WiFi Connected!");
//     Serial.printf("🌐 IP Address: %s\n", WiFi.localIP().toString().c_str());

//     // 🔹 Connect to Local WebSocket Server
//     Serial.println("🔄 Connecting to Local WebSocket Server...");
//     webSocket.begin(websocket_host, websocket_port, websocket_path);
//     webSocket.onEvent(webSocketEvent);
//     webSocket.setReconnectInterval(5000);
// }

// void loop() {
//     webSocket.loop();

//     static unsigned long lastPing = 0;
//     if (millis() - lastPing > 30000) {  
//         lastPing = millis();
//         webSocket.sendTXT("PING");
//         Serial.println("📡 Sent WebSocket Keepalive Ping");
//     }

//     // Prevent multiple IR triggers
//     if (!doorUnlocking && digitalRead(SWITCH) == LOW) {
//         Serial.println("🚪 IR Sensor Detected - Unlocking Door...");
//         doorUnlocking = true;
//         unlockDoor();
//         delay(5000);  // Prevent multiple triggers
//         doorUnlocking = false;
//     }
// }

// void unlockDoor() {
//     Serial.println("🔓 Unlocking Door...");
//     digitalWrite(RELAY, LOW);
//     digitalWrite(GREEN_LED, HIGH);
//     digitalWrite(RED_LED, LOW);

//     playUnlockBeep();
    
//     delay(3000);

//     Serial.println("🔒 Locking Door...");
//     digitalWrite(RELAY, HIGH);
//     digitalWrite(GREEN_LED, LOW);
//     digitalWrite(RED_LED, HIGH);
// }

// void playUnlockBeep() {
//     Serial.println("🔊 Playing unlock beep...");
    
//     for (int i = 0; i < 3; i++) {
//         digitalWrite(BUZZER, HIGH);
//         delay(100);
//         digitalWrite(BUZZER, LOW);
//         delay(100);
//     }
// }
















// #include <ESP8266WiFi.h>
// #include <WebSocketsClient.h>
// #include <ArduinoJson.h>

// // Pin definitions
// #define RELAY D1
// #define BUZZER D8
// #define SWITCH D2       // IR Sensor
// #define RED_LED D5
// #define GREEN_LED D6
// #define DOOR_SENSOR_PIN D7  // Connected to GRAY wire; WHITE wire to GND

// const char* ssid = "DataStream_2.4";       
// const char* password = "armmd123!@#";  

// const char* websocket_host = "192.168.1.10";  
// const int websocket_port = 8080;              
// const char* websocket_path = "/ws";

// WebSocketsClient webSocket;
// String gymID = "G124";
// bool doorUnlocking = false;

// bool isDoorClosed() {
//     // When door is closed, GRAY wire is pulled to GND → reads LOW
//     int sensorState = digitalRead(DOOR_SENSOR_PIN);
//     Serial.print("🔍 Door Sensor State (D7): ");
//     Serial.println(sensorState == LOW ? "CLOSED (LOW)" : "OPEN (HIGH)");
//     return (sensorState == LOW);
// }

// void sendDoorStatus(const String& status) {
//     DynamicJsonDocument doc(128);
//     doc["type"] = "STATUS";
//     doc["gymID"] = gymID;
//     doc["doorStatus"] = status;

//     String jsonStr;
//     serializeJson(doc, jsonStr);
//     webSocket.sendTXT(jsonStr);
//     Serial.print("📡 Sent door status: ");
//     Serial.println(jsonStr);
// }

// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
//     switch (type) {
//         case WStype_DISCONNECTED:
//             Serial.println("⚠️ WebSocket Disconnected! Attempting reconnect...");
//             break;

//         case WStype_CONNECTED: {
//             Serial.println("✅ WebSocket Connected!");
//             DynamicJsonDocument doc(256);
//             doc["type"] = "REGISTER";
//             doc["gymID"] = gymID;
//             String jsonStr;
//             serializeJson(doc, jsonStr);
//             Serial.print("📡 Sending registration: ");
//             Serial.println(jsonStr);
//             webSocket.sendTXT(jsonStr);
//             break;
//         }

//         case WStype_TEXT:
//             Serial.print("📩 Message from server: ");
//             Serial.println((char*)payload);
//             if (String((char*)payload) == "UNLOCK") {
//                 Serial.println("🚪 UNLOCK command received!");
//                 unlockDoor();
//             }
//             break;

//         case WStype_ERROR:
//             Serial.println("❌ WebSocket Error!");
//             break;

//         default:
//             Serial.println("ℹ️ Other WebSocket Event.");
//             break;
//     }
// }

// void setup() {
//     Serial.begin(115200);
//     Serial.println("\n🔹 Booting ESP8266...");

//     pinMode(RELAY, OUTPUT);
//     pinMode(BUZZER, OUTPUT);
//     pinMode(SWITCH, INPUT_PULLUP);
//     pinMode(RED_LED, OUTPUT);
//     pinMode(GREEN_LED, OUTPUT);
//     pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);

//     digitalWrite(RELAY, HIGH);       // Lock engaged
//     digitalWrite(RED_LED, HIGH);     // Red LED ON = Locked
//     digitalWrite(GREEN_LED, LOW);
//     digitalWrite(BUZZER, LOW);

//     // Connect to WiFi
//     Serial.printf("📡 Connecting to WiFi: %s\n", ssid);
//     WiFi.begin(ssid, password);
//     int attempt = 0;
//     while (WiFi.status() != WL_CONNECTED) {
//         delay(500);
//         Serial.print(".");
//         if (++attempt > 30) {
//             Serial.println("\n❌ WiFi failed. Restarting...");
//             ESP.restart();
//         }
//     }
//     Serial.println("\n✅ WiFi Connected!");
//     Serial.print("🌐 IP Address: ");
//     Serial.println(WiFi.localIP());

//     // Connect to WebSocket server
//     webSocket.begin(websocket_host, websocket_port, websocket_path);
//     webSocket.onEvent(webSocketEvent);
//     webSocket.setReconnectInterval(5000);
// }

// void loop() {
//     webSocket.loop();

//     static unsigned long lastPing = 0;
//     if (millis() - lastPing > 30000) {
//         lastPing = millis();
//         webSocket.sendTXT("PING");
//         Serial.println("📡 Sent WebSocket PING");
//     }

//     // IR trigger
//     if (!doorUnlocking && digitalRead(SWITCH) == LOW) {
//         Serial.println("🚶 IR Sensor Triggered - Unlocking...");
//         doorUnlocking = true;
//         unlockDoor();
//         delay(5000);  // debounce
//         doorUnlocking = false;
//     }

//     // Optional live monitoring
//     isDoorClosed();
//     delay(500);
// }

// void unlockDoor() {
//     Serial.println("🔓 Unlocking Door...");
//     digitalWrite(RELAY, LOW);          // Unlock
//     digitalWrite(GREEN_LED, HIGH);
//     digitalWrite(RED_LED, LOW);
//     playUnlockBeep();

//     // Wait until the door is physically closed (or timeout)
//     unsigned long startTime = millis();
//     const unsigned long timeout = 10000;
//     while (!isDoorClosed()) {
//         Serial.println("⏳ Waiting for door to close...");
//         delay(500);
//         if (millis() - startTime > timeout) {
//             Serial.println("⌛ Timeout - Door did not close.");
//             break;
//         }
//     }

//     Serial.println("🔒 Locking Door...");
//     digitalWrite(RELAY, HIGH);         // Lock
//     digitalWrite(GREEN_LED, LOW);
//     digitalWrite(RED_LED, HIGH);

//     if (isDoorClosed()) sendDoorStatus("LOCKED");
//     else sendDoorStatus("OPEN");
// }

// void playUnlockBeep() {
//     Serial.println("🔊 Beeping...");
//     for (int i = 0; i < 3; i++) {
//         tone(BUZZER, 1000, 100);
//         delay(200);
//     }
//     noTone(BUZZER);
// }

