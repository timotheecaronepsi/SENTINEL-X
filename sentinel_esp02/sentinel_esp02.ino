// Sentinel-X — firmware du nœud esp02 (ESP32 DevKit V1)
// Radar (HC-SR04 sur servo SG90) + lumière de présence
// (le gaz et le GPS sont sur l'autre ESP : voir sentinel_esp01)
//
// Fichiers du croquis :
//   sentinel_esp02.ino  Wi-Fi, MQTT, publication de la télémétrie (ce fichier)
//   radar.ino           radar à ultrasons qui balaie la pièce
//   lumiere.ino         lumière allumée en cas de présence
//   secrets.h           mots de passe, hors Git (modèle : secrets.example.h)
//
// Bibliothèques : PubSubClient, ArduinoJson, ESP32Servo
#if !defined(ESP32)
  #error "Ce firmware vise l'ESP32 : choisir la carte DOIT ESP32 DEVKIT V1"
#endif
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

const uint32_t PUBLISH_MS = 5000;   // télémétrie toutes les 5 s

WiFiClient   net;
PubSubClient mqtt(net);
char topicTelemetry[64], topicAlerts[64], topicStatus[64];
uint32_t lastPublish = 0, lastMqttTry = 0;

// Horodatage Unix : 0 tant que l'heure n'est pas donnée par NTP
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
  radarToJson(doc);
  lightToJson(doc);
  if (WiFi.status() == WL_CONNECTED) doc["rssi"] = WiFi.RSSI();
  char buf[256];
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
  Serial.println("\n=== Sentinel-X / " NODE_ID " (radar + lumière) ===");
  snprintf(topicTelemetry, sizeof(topicTelemetry), "sentinel/%s/%s/telemetry", TABLE_ID, NODE_ID);
  snprintf(topicAlerts, sizeof(topicAlerts), "sentinel/%s/%s/alerts", TABLE_ID, NODE_ID);
  snprintf(topicStatus, sizeof(topicStatus), "sentinel/%s/%s/status", TABLE_ID, NODE_ID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  // Réduit les pics de courant du Wi-Fi (alimentation USB partagée avec le servo) :
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  configTime(0, 0, NTP_SERVER);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(2);

  radarSetup();
  lightSetup();   // après configTime : règle le fuseau horaire de Paris
}

void loop() {
  uint32_t now = millis();
  radarLoop();
  lightLoop();

  static bool wifiUp = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) { wifiUp = true; Serial.printf("[WIFI] Connecté, IP %s\n", WiFi.localIP().toString().c_str()); }
    if (mqtt.connected()) mqtt.loop();
    else if (now - lastMqttTry >= 5000) { lastMqttTry = now; connectMqtt(); }
  } else {
    if (wifiUp) { wifiUp = false; Serial.println("[WIFI] Perdu, reconnexion automatique..."); }
    static uint32_t lastWifiReport = 0;
    if (now - lastWifiReport >= 10000) {        // diagnostic toutes les 10 s
      lastWifiReport = now;
      wl_status_t st = WiFi.status();
      const char* why =
        st == WL_NO_SSID_AVAIL   ? "réseau introuvable (nom faux, point d'accès éteint ou en 5 GHz)" :
        st == WL_CONNECT_FAILED  ? "connexion refusée (mot de passe faux ?)" :
        st == WL_CONNECTION_LOST ? "connexion perdue" :
                                   "tentative en cours";
      Serial.printf("[WIFI] Pas connecté à \"%s\" : %s\n", WIFI_SSID, why);
      if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) { WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    }
  }

  if (now - lastPublish >= PUBLISH_MS) { lastPublish = now; publishTelemetry(); }
}
