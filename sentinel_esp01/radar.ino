// Radar : capteur à ultrasons HC-SR04 monté sur un servo SG90 qui balaie de 10° à 170°
// Bibliothèque : "ESP32Servo" (Kevin Harrington)
// Câblage : servo orange -> D26, rouge -> 5 V, marron -> GND
//           HC-SR04 Trig -> D27, Echo -> pont 3 x 220 Ω -> D14, Vcc -> 5 V, Gnd -> GND
//
// Principe : au démarrage, le radar apprend la pièce vide (distance habituelle à chaque angle).
// Ensuite, un objet nettement plus proche que d'habitude, vu sur 2 balayages de suite,
// déclenche une alerte « warning » (présence) ; s'il change de place, « critical » (mouvement).
#include <ESP32Servo.h>

const int RADAR_SERVO_PIN = 26;
const int RADAR_TRIG_PIN  = 27;
const int RADAR_ECHO_PIN  = 14;

const int   R_ANGLE_MIN  = 10;        // le SG90 force en butée à 0° et 180° : on garde une marge
const int   R_ANGLE_MAX  = 170;
const int   R_ANGLE_STEP = 10;
const int   R_N          = (R_ANGLE_MAX - R_ANGLE_MIN) / R_ANGLE_STEP + 1;   // 17 angles
const uint32_t R_SETTLE_MS = 120;     // temps pour que le servo arrive avant la mesure
const float R_MAX_CM     = 400;       // pas d'écho = « rien jusqu'à 4 m »
const int   R_CALIB      = 3;         // balayages d'apprentissage de la pièce vide
const float R_MARGIN_CM  = 30;        // un objet doit être au moins 30 cm plus proche que d'habitude...
const float R_MARGIN_RATIO = 0.8;     // ... et à moins de 80 % de la distance habituelle
const int   R_MOVE_DEG   = 20;        // déplacé d'au moins 20° ...
const float R_MOVE_CM    = 25;        // ... ou de 25 cm d'un balayage à l'autre = en mouvement
const int   R_CLEAR_SWEEPS = 3;       // plus rien pendant 3 balayages = fin d'alerte
const char* RADAR_LEVEL_NAMES[] = {"clear", "warning", "critical"};

Servo    radarServo;
float    rBaseline[R_N], rSweep[R_N];
int      rIdx = 0, rDir = 1, rCalibDone = 0, rMissCount = 0;
bool     rHitLast = false;
uint32_t rLastMove = 0;
uint8_t  radarLevel = 0;                 // 0 = rien, 1 = présence, 2 = mouvement
int      rLastAngle = -1;  float rLastDist = 0;     // dernière détection confirmée
int      rPrevAngle = -1;  float rPrevDist = 0;
char     topicRadar[64];

bool radarReady()    { return rCalibDone >= R_CALIB; }
bool radarPresence() { return radarLevel >= 1; }

float radarReadCm() {
  digitalWrite(RADAR_TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(RADAR_TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(RADAR_TRIG_PIN, LOW);
  unsigned long us = pulseIn(RADAR_ECHO_PIN, HIGH, 25000);   // 25 ms max ~ 4,3 m
  if (us == 0) return R_MAX_CM;
  float d = us * 0.0343f / 2.0f;
  return d > R_MAX_CM ? R_MAX_CM : d;
}

void radarSetup() {
  pinMode(RADAR_TRIG_PIN, OUTPUT);
  pinMode(RADAR_ECHO_PIN, INPUT);
  radarServo.setPeriodHertz(50);
  radarServo.attach(RADAR_SERVO_PIN, 500, 2400);   // impulsions du SG90 : 0,5 à 2,4 ms
  radarServo.write(R_ANGLE_MIN);
  rLastMove = millis();
  snprintf(topicRadar, sizeof(topicRadar), "sentinel/%s/%s/radar", TABLE_ID, NODE_ID);
  Serial.println("[RADAR] Apprentissage de la pièce vide : ne pas passer devant pendant 3 balayages");
}

void radarSetLevel(uint8_t level, float value) {
  if (level == radarLevel) return;
  radarLevel = level;
  publishAlert("radar", RADAR_LEVEL_NAMES[level], value);
}

// Carte complète du balayage, pour l'écran radar du dashboard et pour l'IA
void radarPublishSweep() {
  JsonDocument doc;
  doc["node"] = NODE_ID; doc["ts"] = nowTs();
  doc["angle_min"] = R_ANGLE_MIN; doc["angle_step"] = R_ANGLE_STEP;
  JsonArray arr = doc["dist"].to<JsonArray>();
  for (int i = 0; i < R_N; i++) arr.add((int)roundf(rSweep[i]));
  char buf[256];
  serializeJson(doc, buf);
  if (mqtt.connected()) mqtt.publish(topicRadar, buf);
}

void radarProcessSweep() {
  radarPublishSweep();

  if (rCalibDone < R_CALIB) {            // apprentissage : distance la plus grande vue à chaque angle
    for (int i = 0; i < R_N; i++)
      rBaseline[i] = rCalibDone == 0 ? rSweep[i] : max(rBaseline[i], rSweep[i]);
    rCalibDone++;
    Serial.printf("[RADAR] Apprentissage %d/%d\n", rCalibDone, R_CALIB);
    return;
  }

  int best = -1;                          // objet le plus proche, nettement plus près que d'habitude
  for (int i = 0; i < R_N; i++) {
    bool closer = rSweep[i] < rBaseline[i] - R_MARGIN_CM && rSweep[i] < rBaseline[i] * R_MARGIN_RATIO;
    if (closer && (best < 0 || rSweep[i] < rSweep[best])) best = i;
  }

  if (best >= 0 && rHitLast) {            // confirmé sur 2 balayages : pas un faux écho
    rMissCount = 0;
    rPrevAngle = rLastAngle; rPrevDist = rLastDist;
    rLastAngle = R_ANGLE_MIN + best * R_ANGLE_STEP;
    rLastDist  = rSweep[best];
    bool moved = rPrevAngle >= 0 &&
                 (abs(rLastAngle - rPrevAngle) >= R_MOVE_DEG || fabsf(rLastDist - rPrevDist) >= R_MOVE_CM);
    Serial.printf("[RADAR] Objet à %d° : %.0f cm (habituellement %.0f cm)%s\n",
                  rLastAngle, rLastDist, rBaseline[best], moved ? " — en mouvement" : "");
    radarSetLevel(moved ? 2 : max(radarLevel, (uint8_t)1), rLastDist);
  } else if (best < 0) {
    if (++rMissCount >= R_CLEAR_SWEEPS && radarLevel != 0) {
      rLastAngle = rPrevAngle = -1;
      radarSetLevel(0, 0);
    }
  }
  rHitLast = best >= 0;
}

void radarLoop() {
  if (millis() - rLastMove < R_SETTLE_MS) return;   // le servo n'est pas encore arrivé
  rSweep[rIdx] = radarReadCm();
  rIdx += rDir;
  if (rIdx >= R_N || rIdx < 0) {                    // fin du balayage : on repart dans l'autre sens
    rDir = -rDir;
    rIdx += rDir;
    radarProcessSweep();
  }
  radarServo.write(R_ANGLE_MIN + rIdx * R_ANGLE_STEP);
  rLastMove = millis();
}

void radarToJson(JsonDocument& doc) {
  doc["radar_ready"] = radarReady();
  doc["radar_level"] = radarLevel;
  if (rLastAngle >= 0) {
    doc["radar_angle"] = rLastAngle;
    doc["radar_dist"] = roundf(rLastDist);
  }
}
