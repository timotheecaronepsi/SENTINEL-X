#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "DHT.h"
#include "secrets.h"


// Identifiant client de base (sera rendu unique avec l'adresse MAC)
String clientIdStr = "esp-station-capteurs-";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Configuration MPU (9250 / 6500) ---
const int MPU_ADDR = 0x68;

// --- Configuration DHT22 Pro v1.3 ---
#define DHTPIN 14       // D5 = GPIO14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// --- Configuration Capteur Eau ---
#define PIN_WATER A0
const int SEUIL_DETECTION_EAU = 150;

// --- Seuils Détection Séisme / Sabotage ---
const float GRAVITE_TERRESTRE = 1.0;
const float SEUIL_VIBRATION   = 0.30;
const float SEUIL_CHOC_FORT   = 0.60;

// --- Gestion des intervalles non-bloquants (millis) ---
unsigned long precedentMillisEnvoi = 0;
const unsigned long INTERVALLE_ENVOI = 2000; // Envoi métriques toutes les 2s

unsigned long precedentMillisMPU = 0;
const unsigned long INTERVALLE_MPU = 50;     // Lecture secousses toutes les 50ms

unsigned long dernierAlerteChoc = 0;
unsigned long dernierAlerteSeisme = 0;

void setupWiFi() {
  delay(100);
  Serial.print("\nConnexion a ");
  Serial.println(WIFI_SSID);

  WiFi.disconnect(true);
  delay(200);

  WiFi.mode(WIFI_STA);
  // Désactivation impérative du mode veille Wi-Fi de l'ESP8266
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connecte !");
  Serial.print("Adresse IP ESP : ");
  Serial.println(WiFi.localIP());

  // Ajout des 4 derniers caractères MAC pour garantir un Client ID unique
  clientIdStr += WiFi.macAddress().substring(12, 14) + WiFi.macAddress().substring(15, 17);
}

void reconnectMQTT() {
  // Ne tente la reconnexion QUE si le client n'est pas déjà connecté
  if (!mqttClient.connected()) {
    Serial.print("Connexion au broker MQTT sous ID [");
    Serial.print(clientIdStr);
    Serial.print("]... ");

    // Last Will sur statut hors ligne
    if (mqttClient.connect(clientIdStr.c_str(), MQTT_USER, MQTT_PASSWORD,
                           "esp/esp-porte/status", 1, true, "offline")) {
      Serial.println("connecte !");
      mqttClient.publish("esp/esp-porte/status", "online", true);
    } else {
      Serial.print("echec, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

void initMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(4, 5); // D2 (SDA), D1 (SCL)
  initMPU();
  dht.begin();

  setupWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setKeepAlive(30); // 30 secondes de marge pour le timeout
}

void loop() {
  // Vérification et maintien du lien MQTT
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  unsigned long actuel = millis();

  // 1. Détection dynamique secousses/chocs (toutes les 50 ms sans delay bloquant)
  if (actuel - precedentMillisMPU >= INTERVALLE_MPU) {
    precedentMillisMPU = actuel;

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)6, (bool)true);

    int16_t rawX = Wire.read() << 8 | Wire.read();
    int16_t rawY = Wire.read() << 8 | Wire.read();
    int16_t rawZ = Wire.read() << 8 | Wire.read();

    float ax = rawX / 16384.0;
    float ay = rawY / 16384.0;
    float az = rawZ / 16384.0;

    float accelTotale = sqrt(sq(ax) + sq(ay) + sq(az));
    float ecart = abs(accelTotale - GRAVITE_TERRESTRE);

    // Alertes avec limitation de débit (anti-flood sans bloquer loop)
    if (ecart >= SEUIL_CHOC_FORT && (actuel - dernierAlerteChoc > 500)) {
      dernierAlerteChoc = actuel;
      Serial.println(">>> [MQTT] Envoi alerte sabotage");
      mqttClient.publish("esp/esp-porte/alarme/sabotage", "CHOC_DETECTE");
    } 
    else if (ecart >= SEUIL_VIBRATION && (actuel - dernierAlerteSeisme > 500)) {
      dernierAlerteSeisme = actuel;
      Serial.println(">>> [MQTT] Envoi avertissement seisme");
      mqttClient.publish("esp/esp-porte/alarme/seisme", "VIBRATION_DETECTEE");
    }
  }

  // 2. Publication périodique des capteurs (toutes les 2 secondes)
  if (actuel - precedentMillisEnvoi >= INTERVALLE_ENVOI) {
    precedentMillisEnvoi = actuel;

    int valeurEau = analogRead(PIN_WATER);
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    mqttClient.publish("esp/esp-porte/capteurs/eau", String(valeurEau).c_str());

    if (!isnan(t) && !isnan(h)) {
      mqttClient.publish("esp/esp-porte/capteurs/temperature", String(t, 1).c_str());
      mqttClient.publish("esp/esp-porte/capteurs/humidite", String(h, 1).c_str());
    }

    if (valeurEau >= SEUIL_DETECTION_EAU) {
      mqttClient.publish("esp/esp-porte/alarme/eau", "INONDATION");
    }

    Serial.println("[MQTT] Donnees capteurs transmises au broker.");
  }
}