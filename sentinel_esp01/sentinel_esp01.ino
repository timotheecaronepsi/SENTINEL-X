// Sentinel-X — firmware du nœud esp01 (ESP32 DevKit V1)
// Capteur de gaz MQ-135 + GPS Grove Air530 + détecteur de mouvement PIR + lumière de présence
//
// Fichiers du croquis :
//   sentinel_esp01.ino  Wi-Fi, MQTT, publication de la télémétrie (ce fichier)
//   gaz.ino             capteur de gaz MQ-135
//   gps.ino             GPS Grove Air530
//   pir.ino             détecteur de mouvement infrarouge
//   lumiere.ino         lumière allumée en cas de présence la nuit
//   secrets.h           mots de passe, hors Git (modèle : secrets.example.h)
#if !defined(ESP32)
  #error "Ce firmware vise l'ESP32 : choisir la carte DOIT ESP32 DEVKIT V1"
#endif
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <time.h>
#include <sys/time.h>
#include "secrets.h"

const uint32_t PUBLISH_MS = 5000;   // télémétrie toutes les 5 s

WiFiClient   net;
PubSubClient mqtt(net);
char topicTelemetry[64], topicAlerts[64], topicStatus[64];
uint32_t lastPublish = 0, lastMqttTry = 0;

// Horodatage Unix : 0 tant que l'heure n'est donnée ni par le GPS ni par NTP
uint32_t nowTs() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

void publishAlert(const char* type, const char* level, float value) {
  JsonDocument doc;
  doc["node"] = NODE_ID; doc["ts"] = nowTs();
  doc["type"] = type; doc["level"] = level; doc["value"] = roundf(value);
  char buf[192];
  serializeJson(doc, buf);
  Serial.printf("[ALERT] %s\n", buf);
  if (mqtt.connected()) mqtt.publish(topicAlerts, buf);
}

void publishTelemetry() {
  JsonDocument doc;
  doc["node"] = NODE_ID; doc["ts"] = nowTs();
  gasToJson(doc);
  gpsToJson(doc);
  pirToJson(doc);
  lightToJson(doc);
  if (WiFi.status() == WL_CONNECTED) doc["rssi"] = WiFi.RSSI();
  char buf[512];
  serializeJson(doc, buf);
  Serial.printf("[TELEMETRY] %s\n", buf);
  if (mqtt.connected()) mqtt.publish(topicTelemetry, buf);
}

void connectMqtt() {
  String clientId = String(NODE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connexion à %s:%d ... ", MQTT_HOST, MQTT_PORT);
  bool ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, topicStatus, 1, true, "offline");
  if (ok) { Serial.println("OK"); mqtt.publish(topicStatus, "online", true); }
  else Serial.printf("échec (code %d)\n", mqtt.state());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Sentinel-X / " NODE_ID " ===");
  snprintf(topicTelemetry, sizeof(topicTelemetry), "sentinel/%s/%s/telemetry", TABLE_ID, NODE_ID);
  snprintf(topicAlerts, sizeof(topicAlerts), "sentinel/%s/%s/alerts", TABLE_ID, NODE_ID);
  snprintf(topicStatus, sizeof(topicStatus), "sentinel/%s/%s/status", TABLE_ID, NODE_ID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, NTP_SERVER);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(2);

  gasSetup();
  gpsSetup();
  pirSetup();
  lightSetup();   // après configTime : règle le fuseau horaire de Paris
}

void loop() {
  uint32_t now = millis();
  gasLoop();
  gpsLoop();
  pirLoop();
  lightLoop();

  static bool wifiUp = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) { wifiUp = true; Serial.printf("[WIFI] Connecté, IP %s\n", WiFi.localIP().toString().c_str()); }
    if (mqtt.connected()) mqtt.loop();
    else if (now - lastMqttTry >= 5000) { lastMqttTry = now; connectMqtt(); }
  } else if (wifiUp) {
    wifiUp = false;
    Serial.println("[WIFI] Perdu, reconnexion automatique...");
  }

  if (now - lastPublish >= PUBLISH_MS) { lastPublish = now; publishTelemetry(); }
}
