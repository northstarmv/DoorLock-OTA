//finalv4
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <time.h>

// Pin Definitions
#define RELAY D1
#define BUZZER D8
#define IR_SENSOR D2
#define RED_LED D5
#define GREEN_LED D6
#define DOOR_SENSOR D7

// WiFi Credentials
const char* ssid = "DataStream_2.4";
const char* password = "armmd123!@#";

// Server Configuration
const char* serverHost = "54.255.15.253";
const int serverPort = 8081;
const char* authEndpoint = "/ws-auth";
const char* wsPath = "/ws";

// Auth Config
String apiKey = "K05uCc5QVyD2iA2WpU3kAaR6PLLFi6X4aBzdiit3xkQ=";
int branchId = 1;
int deviceId = 1;

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

// OTA Configuration
String currentVersion = "1.0.0";
String versionJsonURL = "https://northstarmv.github.io/DoorLock-OTA/firmware/version.json";

WebSocketsClient webSocket;
String token = "";
bool doorUnlocking = false;

// Timer for OTA
unsigned long lastOTACheck = 0;

// Choose one:
const unsigned long otaInterval = 300000;    // every 5 minutes
// const unsigned long otaInterval = 10800000;  // every 3 hours
//const unsigned long otaInterval = 86400000;     // every 24 hours (midnight)

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(IR_SENSOR, INPUT_PULLUP);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(DOOR_SENSOR, INPUT_PULLUP);

  digitalWrite(RELAY, HIGH);
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);

  Serial.println("\n🔧 Booting...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.printf("\n✅ Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  while (!getLocalTime(nullptr)) {
    Serial.println("⏱ Waiting for time...");
    delay(1000);
  }

  if (getToken()) startWebSocket();
  else ESP.restart();
}

void loop() {
  webSocket.loop();

  if (!doorUnlocking && digitalRead(IR_SENSOR) == LOW) {
    Serial.println("🚪 IR Sensor Triggered - Unlocking");
    doorUnlocking = true;
    unlockDoor();
    doorUnlocking = false;
  }

  if (millis() - lastOTACheck > otaInterval) {
    lastOTACheck = millis();
    checkForOTAUpdate();
  }
}

bool isDoorClosed() {
  int state = digitalRead(DOOR_SENSOR);
  Serial.printf("🔍 Door Sensor State: %d (%s)\n", state, state == LOW ? "CLOSED" : "OPEN");
  return state == LOW;
}

void unlockDoor() {
  Serial.println("🔓 Unlocking door...");
  digitalWrite(RELAY, LOW);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  playBeep();

  Serial.println("⏳ Waiting for door to open...");
  unsigned long startTime = millis();
  while (isDoorClosed()) {
    if (millis() - startTime > 10000) {
      Serial.println("⌛ Timeout: Door not opened. Re-locking...");
      lockDoorAndConfirm();
      return;
    }
    delay(300);
  }

  Serial.println("✅ Door opened (magnet separated)");
  sendDoorOpened();

  Serial.println("⏳ Waiting for door to close...");
  while (!isDoorClosed()) delay(300);
  Serial.println("✅ Door closed (magnet contact)");
  lockDoorAndConfirm();
}

void lockDoorAndConfirm() {
  Serial.println("🔒 Locking door...");
  digitalWrite(RELAY, HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);
  sendDoorClosed();
}

void playBeep() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER, HIGH); delay(100);
    digitalWrite(BUZZER, LOW); delay(100);
  }
}

void sendDoorClosed() {
  DynamicJsonDocument doc(128);
  doc["msgType"] = "isunlock";
  JsonObject data = doc.createNestedObject("data");
  data["isUnlock"] = false;
  data["timestamp"] = getTimestamp();

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  Serial.println("📤 Sent door closed event to server: " + msg);
}

void sendDoorOpened() {
  DynamicJsonDocument doc(128);
  doc["msgType"] = "isunlock";
  JsonObject data = doc.createNestedObject("data");
  data["isUnlock"] = true;
  data["timestamp"] = getTimestamp();

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  Serial.println("📤 Sent door OPENED event to server: " + msg);
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "2025-04-10T00:00:00Z";
  char isoTime[30];
  strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(isoTime);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) Serial.println("🔗 WebSocket connected");
  else if (type == WStype_TEXT) {
    Serial.printf("📩 Message: %s\n", (char*)payload);
    if (String((char*)payload).indexOf("door unlocked") >= 0) unlockDoor();
  }
  else if (type == WStype_DISCONNECTED) {
    Serial.println("⚠️ WebSocket disconnected. Waiting 1 min to retry...");
    delay(60000);
    if (getToken()) startWebSocket();
  }
  else if (type == WStype_ERROR) Serial.println("❌ WebSocket Error!");
}

bool getToken() {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + serverHost + ":" + serverPort + authEndpoint;

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument body(256);
  body["connType"] = "door-lock";
  body["id"] = deviceId;
  body["connId"] = JsonArray();
  body["apiKey"] = apiKey;
  body["branchId"] = branchId;

  String requestBody;
  serializeJson(body, requestBody);
  Serial.print("\n🔑 Sending token request: ");
  Serial.println(requestBody);

  int httpCode = http.POST(requestBody);
  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(256);
    deserializeJson(responseDoc, response);
    if (responseDoc.containsKey("token")) {
      token = responseDoc["token"].as<String>();
      Serial.println("🔐 Parsed token: " + token);
      return true;
    }
  }
  return false;
}

void startWebSocket() {
  String fullPath = String(wsPath) + "?token=" + token;
  Serial.print("🔌 Connecting to WebSocket at: ");
  Serial.println("ws://" + String(serverHost) + ":" + String(serverPort) + fullPath);
  webSocket.begin(serverHost, serverPort, fullPath);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void checkForOTAUpdate() {
  Serial.println("🛠️ Checking for OTA update...");
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, versionJsonURL)) {
    Serial.println("❌ Failed to connect to version.json");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("❌ HTTP GET failed: %d\n", httpCode);
    return;
  }

  String payload = http.getString();
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.println("❌ Failed to parse JSON");
    return;
  }

  String latestVersion = doc["version"];
  String binURL = doc["bin"];

  Serial.println("🔍 Current version: " + currentVersion + ", Latest: " + latestVersion);

  if (latestVersion != currentVersion) {
    Serial.println("⬇️ New version available. Starting OTA...");
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, binURL, currentVersion);
    switch (ret) {
      case HTTP_UPDATE_FAILED:
        Serial.printf("❌ OTA Failed: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
        break;
      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("ℹ️ No new updates.");
        break;
      case HTTP_UPDATE_OK:
        Serial.println("✅ OTA Update successful. Restarting...");
        break;
    }
  } else {
    Serial.println("✅ Already on latest version.");
  }

  http.end();
}






















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

