// Capteur de gaz MQ-135
// Câblage : VCC -> VIN (5 V), GND -> GND, AO -> pont 10 kΩ / 10 kΩ -> D34, DO non branché

const uint8_t  GAS_PIN       = 34;          // ADC1 obligatoire avec le Wi-Fi (GPIO32 à 39)
const uint32_t GAS_SAMPLE_MS = 200;         // une lecture toutes les 200 ms
const uint32_t WARMUP_MS     = 180000UL;    // chauffe : 3 min (30000UL pour un test rapide)
const uint32_t BASELINE_MS   = WARMUP_MS / 3; // fin de chauffe = mesure de l'air propre
const uint8_t  GAS_WINDOW    = 10;          // moyenne glissante sur 10 lectures (2 s)

// Niveaux d'alerte locale (rapport à la référence air propre), avec hystérésis
const float HIGH_ON  = 1.20f;   // normal -> high
const float HIGH_OFF = 1.10f;   // high -> normal (clear)
const float CRIT_ON  = 1.50f;   // -> critical
const float CRIT_OFF = 1.35f;   // critical -> high
const char* GAS_LEVEL_NAMES[] = {"clear", "high", "critical"};

uint16_t gasSamples[GAS_WINDOW];
uint8_t  gasIdx = 0;
bool     gasWindowFull = false;
uint16_t gasLastRaw = 0;
uint32_t gasBaselineSum = 0;
uint16_t gasBaselineCount = 0;
float    gasBaseline = 0;
uint8_t  gasLevel = 0;          // 0 = normal, 1 = high, 2 = critical
uint32_t lastGasSample = 0;

bool gasWarmedUp() { return millis() >= WARMUP_MS; }

float gasAverage() {
  uint8_t n = gasWindowFull ? GAS_WINDOW : gasIdx;
  if (n == 0) return 0;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < n; i++) sum += gasSamples[i];
  return (float)sum / n;
}

void gasSetup() {
  Serial.printf("[MQ] Chauffe du capteur : %lu s avant des valeurs fiables\n", WARMUP_MS / 1000);
}

void gasRead() {
  gasLastRaw = analogRead(GAS_PIN) >> 2;   // 12 bits -> 0-1023
  gasSamples[gasIdx] = gasLastRaw;
  gasIdx = (gasIdx + 1) % GAS_WINDOW;
  if (gasIdx == 0) gasWindowFull = true;

  if (!gasWarmedUp()) {
    if (millis() >= WARMUP_MS - BASELINE_MS) { gasBaselineSum += gasLastRaw; gasBaselineCount++; }
    return;
  }
  if (gasBaseline == 0) {
    gasBaseline = gasBaselineCount ? (float)gasBaselineSum / gasBaselineCount : gasAverage();
    if (gasBaseline < 10) gasBaseline = 10;
    Serial.printf("[MQ] Chauffe terminée, référence air propre = %.0f\n", gasBaseline);
  }

  float avg = gasAverage();
  float ratio = avg / gasBaseline;
  uint8_t newLevel = gasLevel;
  if (gasLevel == 2) {                       // en critical : on redescend seulement sous CRIT_OFF
    if (ratio < CRIT_OFF) newLevel = (ratio >= HIGH_OFF) ? 1 : 0;
  } else if (gasLevel == 1) {                // en high
    if (ratio >= CRIT_ON) newLevel = 2;
    else if (ratio < HIGH_OFF) newLevel = 0;
  } else {                                   // normal
    if (ratio >= CRIT_ON) newLevel = 2;
    else if (ratio >= HIGH_ON) newLevel = 1;
  }
  if (newLevel != gasLevel) {
    gasLevel = newLevel;
    publishAlert("gas", GAS_LEVEL_NAMES[gasLevel], avg);
  }
}

void gasLoop() {
  uint32_t now = millis();
  if (now - lastGasSample >= GAS_SAMPLE_MS) { lastGasSample = now; gasRead(); }
}

void gasToJson(JsonDocument& doc) {
  float avg = gasAverage();
  doc["gas"] = roundf(avg);
  doc["gas_raw"] = gasLastRaw;
  if (gasBaseline > 0) doc["gas_ratio"] = roundf(avg / gasBaseline * 100) / 100.0;
  doc["warm"] = gasWarmedUp();
  doc["gas_level"] = gasLevel;
}
