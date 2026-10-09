#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Keypad.h>
#include "secrets.h"

// --- Topics MQTT autorisés par l'ACL ---
const char* TOPIC_STATUS   = "esp/esp-digicode/status";
const char* TOPIC_ACCES    = "esp/esp-digicode/digicode/acces";
const char* TOPIC_CONSIGNE = "sentinel/consigne/esp-digicode";

String clientIdStr = "esp-digicode-";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Configuration des sorties ---
#define PIN_LED_VERTE 2  // D4 = GPIO2
#define PIN_HP_ROUGE  16 // D0 = GPIO16

// --- Configuration Clavier 4x4 ---
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {3, 5, 4, 0};     // RX (GPIO3), D1, D2, D3
byte colPins[COLS] = {14, 12, 13, 15}; // D5, D6, D7, D8

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- État de la consigne dynamique ---
String consignePersonne = "";
String consigneCode = "";
unsigned long consigneEcheance = 0;
bool consigneActive = false;

// --- Saisie digicode ---
String saisieUtilisateur = "";
unsigned long debutChrono = 0;
const unsigned long DELAI_MAX = 5000;

// --- Sécurité tentatives & blocage ---
byte echecsConsecutifs = 0;
unsigned long finBlocageClavier = 0;
const unsigned long DUREE_BLOCAGE = 30000; // 30 secondes après 3 échecs

// --- Timers et alertes sonores/lumineuses ---
unsigned long timerLedVerte = 0;
unsigned long timerAlerte = 0;
const unsigned long DUREE_ACCES = 3000;
const unsigned long DUREE_ALERTE = 2000;
bool alerteEnCours = false;
const unsigned int FREQUENCE_ALARME = 2000;

// --- Reconnexion MQTT non bloquante ---
unsigned long precedentMillisMQTT = 0;
const unsigned long INTERVALLE_RECONNEXION_MQTT = 5000;

void effacerConsigne() {
  consignePersonne = "";
  consigneCode = "";
  consigneEcheance = 0;
  consigneActive = false;
}

void reinitialiserSaisie() {
  saisieUtilisateur = "";
  debutChrono = 0;
}

void declencherAlerte(unsigned long depart, const String& raison) {
  alerteEnCours = true;
  timerAlerte = depart;
  digitalWrite(PIN_LED_VERTE, LOW);

  echecsConsecutifs++;
  if (echecsConsecutifs >= 3) {
    finBlocageClavier = depart + DUREE_BLOCAGE;
    echecsConsecutifs = 0;
    Serial.println(">>> 3 echecs : clavier bloque pour 30 s <<<");
  }

  if (mqttClient.connected()) {
    String payload = "REFUSE " + raison;
    mqttClient.publish(TOPIC_ACCES, payload.c_str());
  }
  Serial.print(">>> ACCES REFUSE : ");
  Serial.println(raison);
}

void arreterAlerte() {
  alerteEnCours = false;
  noTone(PIN_HP_ROUGE);
  pinMode(PIN_HP_ROUGE, OUTPUT);
  digitalWrite(PIN_HP_ROUGE, LOW);
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != TOPIC_CONSIGNE) return;

  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  // Format : "<validité_s>|<personne>|<code>"
  int sep1 = message.indexOf('|');
  if (sep1 == -1) return;

  unsigned long validiteSec = message.substring(0, sep1).toInt();
  if (validiteSec == 0) {
    effacerConsigne();
    Serial.println("[MQTT] Consigne effacee.");
    return;
  }

  int sep2 = message.indexOf('|', sep1 + 1);
  if (sep2 == -1) return;

  consignePersonne = message.substring(sep1 + 1, sep2);
  consigneCode     = message.substring(sep2 + 1);
  consigneEcheance = millis() + (validiteSec * 1000);
  consigneActive   = true;

  Serial.print("[MQTT] Nouvelle consigne recue pour : ");
  Serial.println(consignePersonne);
}

void setupWiFi() {
  delay(100);
  WiFi.disconnect(true);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connecte !");
  clientIdStr += WiFi.macAddress().substring(12, 14) + WiFi.macAddress().substring(15, 17);
}

void reconnectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Connexion MQTT sous [");
    Serial.print(clientIdStr);
    Serial.print("]... ");

    // Last Will QoS 1 retenu vers topic status
    if (mqttClient.connect(clientIdStr.c_str(), MQTT_USER, MQTT_PASSWORD,
                           TOPIC_STATUS, 1, true, "offline")) {
      Serial.println("connecte !");
      mqttClient.publish(TOPIC_STATUS, "online", true);
      mqttClient.subscribe(TOPIC_CONSIGNE, 1);
    } else {
      Serial.print("echec, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

void setup() {
  pinMode(PIN_LED_VERTE, OUTPUT);
  digitalWrite(PIN_LED_VERTE, LOW);

  pinMode(PIN_HP_ROUGE, OUTPUT);
  digitalWrite(PIN_HP_ROUGE, LOW);
  noTone(PIN_HP_ROUGE);

  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(200);

  setupWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(callbackMQTT);
  mqttClient.setKeepAlive(30);
}

void loop() {
  unsigned long actuelMillis = millis();

  // Maintien MQTT non bloquant
  if (!mqttClient.connected()) {
    if (actuelMillis - precedentMillisMQTT >= INTERVALLE_RECONNEXION_MQTT) {
      precedentMillisMQTT = actuelMillis;
      reconnectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  // Expiration automatique de la consigne
  if (consigneActive && (actuelMillis >= consigneEcheance)) {
    effacerConsigne();
    Serial.println("Consigne expiree temporellement.");
  }

  // 1. Lecture Clavier (verrouille si 3 echecs)
  if (actuelMillis < finBlocageClavier) {
    keypad.getKey(); // Vide le tampon pendant la pénalité
  } else {
    char key = keypad.getKey();

    if (key) {
      if (saisieUtilisateur.length() == 0 && key != '#' && key != 'D') {
        debutChrono = actuelMillis;
      }

      if (key == '#') {
        reinitialiserSaisie();
        digitalWrite(PIN_LED_VERTE, LOW);
        arreterAlerte();
      }
      else if (key == 'D') {
        if (saisieUtilisateur.length() > 0) {
          if (!consigneActive) {
            declencherAlerte(actuelMillis, "aucun visage reconnu");
          }
          else if (saisieUtilisateur == consigneCode) {
            arreterAlerte();
            echecsConsecutifs = 0;
            digitalWrite(PIN_LED_VERTE, HIGH);
            timerLedVerte = actuelMillis;

            String payload = "ACCORDE " + consignePersonne;
            if (mqttClient.connected()) {
              mqttClient.publish(TOPIC_ACCES, payload.c_str());
            }
            Serial.print(">>> ACCES ACCORDE A : ");
            Serial.println(consignePersonne);

            effacerConsigne(); // Usage unique
          }
          else {
            declencherAlerte(actuelMillis, "mauvais code pour " + consignePersonne);
          }
        }
        reinitialiserSaisie();
      }
      else {
        saisieUtilisateur += key;
      }
    }
  }

  // 2. Timeout de saisie (5 s)
  if (debutChrono > 0 && (actuelMillis - debutChrono > DELAI_MAX)) {
    declencherAlerte(actuelMillis, "delai depasse");
    reinitialiserSaisie();
  }

  // 3. Gestion sonore de l'alarme
  if (alerteEnCours) {
    if (actuelMillis - timerAlerte < DUREE_ALERTE) {
      bool bipActif = ((actuelMillis / 150) % 2) == 0;
      if (bipActif) {
        tone(PIN_HP_ROUGE, FREQUENCE_ALARME);
      } else {
        noTone(PIN_HP_ROUGE);
      }
    } else {
      arreterAlerte();
    }
  }

  // 4. Extinction de la LED verte
  if (timerLedVerte > 0 && (actuelMillis - timerLedVerte >= DUREE_ACCES)) {
    digitalWrite(PIN_LED_VERTE, LOW);
    timerLedVerte = 0;
  }
}