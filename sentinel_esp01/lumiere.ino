// Lumière de présence : s'allume quand le PIR détecte quelqu'un ET qu'il fait nuit
// Pour l'instant : LED bleue intégrée à l'ESP32 (GPIO2), aucun câblage.
// Pour une vraie lampe plus tard : changer LIGHT_PIN, et passer par un transistor ou un relais
// (jamais une lampe directement sur une broche de l'ESP32).

const uint8_t  LIGHT_PIN    = 25;       // LED externe sur D25 (résistance 220 Ω) ; 2 = LED bleue de la carte
const uint32_t LIGHT_ON_MS  = 30000;    // reste allumée 30 s après la dernière présence

// Comment savoir s'il fait nuit :
//   0 = toujours (pour tester ou pour la démo en journée)
//   1 = selon l'heure (donnée par le GPS ou NTP), heure de Paris
//   2 = avec un capteur de lumière sur GPIO35 (si vous en obtenez un)
#define LIGHT_MODE 0   // 0 pour les tests et la démo en journée, 1 pour « la nuit seulement »
const uint8_t NIGHT_START_H = 19;       // nuit à partir de 19 h ...
const uint8_t NIGHT_END_H   = 7;        // ... jusqu'à 7 h
const uint8_t LDR_PIN       = 35;       // mode 2 : sortie analogique du capteur de lumière
const uint16_t DARK_ON      = 150;      // mode 2 : en dessous = sombre (échelle 0-1023, à ajuster)
const uint16_t DARK_OFF     = 200;      // mode 2 : au-dessus = clair (hystérésis)

bool     lightOn = false;
bool     lightEverTriggered = false;
bool     darkState = true;
uint32_t lastLightTrigger = 0;

bool isDark() {
#if LIGHT_MODE == 0
  return true;
#elif LIGHT_MODE == 1
  time_t t = time(nullptr);
  if (t < 1700000000) return true;      // heure inconnue : on considère qu'il fait nuit
  struct tm local;
  localtime_r(&t, &local);
  return local.tm_hour >= NIGHT_START_H || local.tm_hour < NIGHT_END_H;
#else
  uint16_t level = analogRead(LDR_PIN) >> 2;
  if (darkState && level > DARK_OFF) darkState = false;
  else if (!darkState && level < DARK_ON) darkState = true;
  return darkState;
#endif
}

void lightSetup() {
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);
  // Heure de Paris (gère l'heure d'été) ; l'horodatage ts reste en UTC
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  Serial.printf("[LUMIERE] Mode %d (0 = toujours, 1 = horaires %dh-%dh, 2 = capteur)\n",
                LIGHT_MODE, NIGHT_START_H, NIGHT_END_H);
}

void lightLoop() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 200) return;
  lastCheck = now;

  darkState = isDark();
  if (pirPresence() && darkState) { lastLightTrigger = now; lightEverTriggered = true; }

  bool shouldBeOn = lightEverTriggered && (now - lastLightTrigger < LIGHT_ON_MS);
  if (shouldBeOn != lightOn) {
    lightOn = shouldBeOn;
    digitalWrite(LIGHT_PIN, lightOn ? HIGH : LOW);
    pirBlankAfterLightSwitch();   // la commutation de la LED ne doit pas relancer le PIR
    Serial.println(lightOn ? "[LUMIERE] Allumée (présence la nuit)" : "[LUMIERE] Éteinte");
  }
}

void lightToJson(JsonDocument& doc) {
  doc["light"] = lightOn;
  doc["dark"] = darkState;
}
