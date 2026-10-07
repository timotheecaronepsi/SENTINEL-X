// Détecteur de mouvement infrarouge (PIR Grove)
// Câblage : SIG (jaune) -> GPIO27 (D27), VCC (rouge) -> 3,3 V, GND (noir) -> GND

const uint8_t  PIR_PIN         = 27;
const uint32_t PIR_WARMUP_MS   = 30000;   // le PIR se stabilise ~30 s après la mise sous tension
const uint32_t PIR_SAMPLE_MS   = 100;     // lecture toutes les 100 ms
const uint32_t PIR_MIN_HIGH_MS = 300;     // un mouvement ne compte que si le signal tient 300 ms
const uint32_t PIR_SWITCH_BLANK_MS = 3000; // PIR ignoré 3 s après chaque changement de la lumière
const uint16_t PIR_WINDOW      = 300;     // fenêtre d'activité : 300 x 100 ms = 30 s
const uint32_t MOTION_HOLD_MS  = 15000;   // présence maintenue 15 s après la dernière détection
const uint8_t  STRONG_ON_PCT   = 60;      // forte activité si détection 60 % du temps sur 30 s
const uint8_t  STRONG_OFF_PCT  = 40;      // ... et on ne la quitte que sous 40 %
const char*    MOTION_LEVEL_NAMES[] = {"clear", "warning", "critical"};

uint8_t  pirSamples[PIR_WINDOW];
uint16_t pirIdx = 0, pirFilled = 0, pirHighCount = 0;
bool     pirState = false;
uint8_t  motionLevel = 0;      // 0 = rien, 1 = présence, 2 = forte activité
uint32_t lastPirSample = 0, lastMotionMs = 0, pirDetections = 0;
bool     pirReadyAnnounced = false;
uint32_t pirHighSince = 0, pirIgnoreUntil = 0;

// Appelé par lumiere.ino : ignore les parasites créés par la commutation de la LED
void pirBlankAfterLightSwitch() { pirIgnoreUntil = millis() + PIR_SWITCH_BLANK_MS; }

bool pirReady() { return millis() >= PIR_WARMUP_MS; }
bool pirPresence() { return motionLevel >= 1; }

uint8_t pirActivityPct() {
  return pirFilled ? (uint8_t)((uint32_t)pirHighCount * 100 / pirFilled) : 0;
}

void pirSetup() {
  pinMode(PIR_PIN, INPUT_PULLDOWN);   // reste à 0 si le capteur est débranché
  Serial.printf("[PIR] Stabilisation du capteur : %lu s\n", PIR_WARMUP_MS / 1000);
}

void pirRead() {
  bool raw = digitalRead(PIR_PIN) == HIGH;
  uint32_t now = millis();
  if ((int32_t)(now - pirIgnoreUntil) < 0) raw = false;   // fenêtre de blocage

  // Filtre anti-parasite : le signal doit rester haut au moins PIR_MIN_HIGH_MS
  if (!raw) pirHighSince = 0;
  else if (pirHighSince == 0) pirHighSince = now;
  bool high = raw && (now - pirHighSince >= PIR_MIN_HIGH_MS);

  // Fenêtre glissante : proportion du temps où le PIR voit du mouvement
  if (pirFilled == PIR_WINDOW) pirHighCount -= pirSamples[pirIdx];
  else pirFilled++;
  pirSamples[pirIdx] = high ? 1 : 0;
  pirHighCount += pirSamples[pirIdx];
  pirIdx = (pirIdx + 1) % PIR_WINDOW;

  if (high && !pirState) { pirDetections++; Serial.println("[PIR] Mouvement détecté"); }
  pirState = high;
  if (high) lastMotionMs = millis();

  uint8_t act = pirActivityPct();
  uint8_t newLevel;
  if (act >= STRONG_ON_PCT || (motionLevel == 2 && act >= STRONG_OFF_PCT)) newLevel = 2;
  else if (high || millis() - lastMotionMs < MOTION_HOLD_MS) newLevel = 1;
  else newLevel = 0;

  if (newLevel != motionLevel) {
    motionLevel = newLevel;
    publishAlert("motion", MOTION_LEVEL_NAMES[motionLevel], act);
  }
}

void pirLoop() {
  if (!pirReady()) return;
  if (!pirReadyAnnounced) { pirReadyAnnounced = true; Serial.println("[PIR] Prêt"); }
  uint32_t now = millis();
  if (now - lastPirSample >= PIR_SAMPLE_MS) { lastPirSample = now; pirRead(); }
}

void pirToJson(JsonDocument& doc) {
  doc["pir"] = pirState ? 1 : 0;
  doc["motion_activity"] = pirActivityPct();
  doc["motion_level"] = motionLevel;
}
