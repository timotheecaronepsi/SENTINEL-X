// GPS Grove Air530 (bibliothèque TinyGPSPlus)
// Câblage : jaune (TX du GPS) -> RX2 / GPIO16, blanc (RX du GPS) -> TX2 / GPIO17,
//           rouge -> 3V3 (surtout pas VIN), noir -> GND

const int      GPS_RX_PIN         = 16;
const int      GPS_TX_PIN         = 17;
const uint32_t GPS_BAUD           = 9600;
const uint32_t GPS_FIX_MAX_AGE_MS = 10000;     // position plus vieille que 10 s = fix perdu
const uint32_t GPS_CLOCK_SYNC_MS  = 600000UL;  // recale l'horloge toutes les 10 min
const uint32_t GPS_REPORT_MS      = 30000;     // état du GPS dans le moniteur toutes les 30 s
const time_t   GPS_ROLLOVER_S     = (time_t)1024 * 7 * 86400;  // 1024 semaines
const time_t   TS_2024            = 1704067200;  // 1er janvier 2024

HardwareSerial gpsSerial(2);
TinyGPSPlus    gps;
bool     gpsHadFix = false;
bool     gpsClockSynced = false;
uint32_t lastClockSync = 0, lastGpsReport = 0;

bool gpsHasFix() {
  return gps.location.isValid() && gps.location.age() < GPS_FIX_MAX_AGE_MS;
}

// Nombre de jours depuis le 1er janvier 1970 (algorithme de H. Hinnant)
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Met l'horloge de l'ESP32 à l'heure du GPS (uniquement avec une position valide :
// avant le fix, la date envoyée par le module est fausse)
void gpsSyncClock() {
  if (!gpsHasFix() || !gps.date.isValid() || !gps.time.isValid() || gps.time.age() > 1500) return;
  if (gpsClockSynced && millis() - lastClockSync < GPS_CLOCK_SYNC_MS) return;

  int64_t days = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day());
  time_t t = (time_t)(days * 86400 + gps.time.hour() * 3600 + gps.time.minute() * 60 + gps.time.second());
  while (t < TS_2024) t += GPS_ROLLOVER_S;   // bug du rollover : le module annonce 2007 au lieu de 2026

  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  lastClockSync = millis();
  if (!gpsClockSynced) {
    gpsClockSynced = true;
    struct tm utc;
    gmtime_r(&t, &utc);
    Serial.printf("[GPS] Horloge réglée : %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  }
}

void gpsSetup() {
  gpsSerial.setRxBufferSize(1024);   // grand tampon : évite de perdre des trames
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] Démarré, recherche des satellites (antenne vers le ciel)");
}

void gpsLoop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  gpsSyncClock();

  bool fix = gpsHasFix();
  if (fix != gpsHadFix) {
    gpsHadFix = fix;
    publishAlert("gps", fix ? "fix" : "lost", gps.satellites.value());
  }

  if (millis() - lastGpsReport >= GPS_REPORT_MS) {
    lastGpsReport = millis();
    if (gps.charsProcessed() < 10)
      Serial.println("[GPS] Aucune donnée reçue : vérifier jaune -> RX2 (16), rouge -> 3V3, noir -> GND");
    else
      Serial.printf("[GPS] fix=%s sats=%lu hdop=%.1f trames ok=%lu rejetées=%lu\n",
                    fix ? "oui" : "non", gps.satellites.value(), gps.hdop.hdop(),
                    gps.passedChecksum(), gps.failedChecksum());
  }
}

void gpsToJson(JsonDocument& doc) {
  bool fix = gpsHasFix();
  doc["gps_fix"] = fix;
  doc["sats"] = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (fix) {
    doc["lat"] = serialized(String(gps.location.lat(), 6));
    doc["lon"] = serialized(String(gps.location.lng(), 6));
    doc["hdop"] = roundf(gps.hdop.hdop() * 10) / 10.0;
    if (gps.altitude.isValid()) doc["alt"] = roundf(gps.altitude.meters());
  }
}
