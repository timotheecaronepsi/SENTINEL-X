// ============================================================
//  Scanneur de badges RFID RC522 + NodeMCU ESP8266
//  Affiche le détail de chaque badge dans le Moniteur série
//  (115200 bauds)
//
//  Câblage : SDA->D8  SCK->D5  MOSI->D7  MISO->D6
//            IRQ->rien  GND->G  RST->D3  3.3V->3V
//
//  Bibliothèque : "MFRC522" par GithubCommunity
// ============================================================
#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN  D8   // broche SDA du module
#define RST_PIN D3

MFRC522 rfid(SS_PIN, RST_PIN);

// Mettre à true pour afficher TOUT le contenu de la carte
// (secteurs, blocs...). Plus long, mais très détaillé.
const bool DUMP_COMPLET = false;

unsigned long nbScans = 0;
byte dernierUid[10];
byte tailleDernierUid = 0;

// ------------------------------------------------------------
void afficherUid(byte* uid, byte taille) {
  for (byte i = 0; i < taille; i++) {
    if (uid[i] < 0x10) Serial.print("0");
    Serial.print(uid[i], HEX);
    if (i < taille - 1) Serial.print(":");
  }
}

bool memeBadgeQueAvant() {
  if (rfid.uid.size != tailleDernierUid) return false;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] != dernierUid[i]) return false;
  }
  return true;
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  SPI.begin();
  rfid.PCD_Init();
  delay(50);

  Serial.println();
  Serial.println("==================================");
  Serial.println("   Scanneur de badges RC522");
  Serial.println("==================================");

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("Version du lecteur : 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    Serial.println("ERREUR : lecteur non detecte, verifie le cablage !");
  } else {
    Serial.println("Pret. Approche un badge...");
  }
}

// ------------------------------------------------------------
void loop() {
  // Attend qu'un nouveau badge soit présenté
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  nbScans++;
  MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);

  Serial.println();
  Serial.println("----------------------------------");
  Serial.print("Scan #");
  Serial.print(nbScans);
  Serial.print("   (");
  Serial.print(millis() / 1000);
  Serial.println(" s apres demarrage)");

  Serial.print("UID (hex)     : ");
  afficherUid(rfid.uid.uidByte, rfid.uid.size);
  Serial.println();

  // UID en décimal (utile pour les badges de 4 octets)
  if (rfid.uid.size <= 4) {
    unsigned long dec = 0;
    for (byte i = 0; i < rfid.uid.size; i++) {
      dec = (dec << 8) | rfid.uid.uidByte[i];
    }
    Serial.print("UID (decimal) : ");
    Serial.println(dec);
  }

  Serial.print("Taille UID    : ");
  Serial.print(rfid.uid.size);
  Serial.println(" octets");

  Serial.print("Type de carte : ");
  Serial.println(rfid.PICC_GetTypeName(type));

  Serial.print("SAK           : 0x");
  if (rfid.uid.sak < 0x10) Serial.print("0");
  Serial.println(rfid.uid.sak, HEX);

  Serial.print("Statut        : ");
  if (memeBadgeQueAvant()) Serial.println("meme badge que le precedent");
  else                     Serial.println("nouveau badge");

  // Mémorise ce badge
  tailleDernierUid = rfid.uid.size;
  memcpy(dernierUid, rfid.uid.uidByte, rfid.uid.size);

  if (DUMP_COMPLET) {
    Serial.println("Contenu complet de la carte :");
    rfid.PICC_DumpToSerial(&rfid.uid);   // met aussi la carte en veille
  } else {
    rfid.PICC_HaltA();        // met la carte en veille
    rfid.PCD_StopCrypto1();
  }

  Serial.println("----------------------------------");
  Serial.println("Approche un badge...");
}
