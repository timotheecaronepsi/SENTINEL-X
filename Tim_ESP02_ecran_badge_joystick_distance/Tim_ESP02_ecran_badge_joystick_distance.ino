// ============================================================
//  BORNE COMPLETE - Workshop SENTINEL-X
//  NodeMCU ESP8266 + écran Grove LCD RGB Backlight (I2C)
//  + lecteur de badge RC522 + capteur de distance HC-SR04
//  + joystick Grove
//
//  Fonctionnement :
//   1. VEILLE        : "Bienvenue !" + distance mesurée (publiée au serveur)
//                      Si la caméra a reconnu quelqu'un (consigne) :
//                      "Bonjour <personne>" / "Tapez le code"
//   2. CODE OK       : le serveur envoie "BADGE|<personne>" quand le
//                      digicode est validé -> "Code OK <personne>" /
//                      "Passez un badge" (20 s max, sinon veille)
//   3. BADGE         : à chaque badge scanné (même en veille), envoi au
//                      serveur puis "Verification..." (6 s max) :
//                      BADGE_OK -> questionnaire, REFUSE -> ACCES REFUSE
//   4. QUESTIONNAIRE : gauche/droite = choisir, appui = valider
//                      (30 s sans action = session annulée)
//   5. FIN           : résultats envoyés en JSON au serveur MQTT,
//                      puis attente du verdict du serveur (6 s max) :
//                      ACCES ACCORDE / ACCES REFUSE / Pas de reponse
//
//  Contrat MQTT (compte "esp-ecran") :
//   Publie  esp/esp-ecran/status     "online" / "offline" (Last Will), retenu
//           esp/esp-ecran/badge      {"badge":"3AF2910C","type_badge":"MIFARE 1KB"}
//           esp/esp-ecran/distance   {"distance_cm":42.5} ou {"distance_cm":null}
//           esp/esp-ecran/resultats  récap JSON de la session
//   Lit     sentinel/consigne/esp-ecran
//           "<validite_s>|<personne>|<rep1>|<rep2>|<rep3>|<rep4>|<rep5>"
//           "0|" = effacer la consigne
//           sentinel/consigne/esp-ecran/etape
//           "BADGE|<personne>"  (digicode validé)
//           sentinel/consigne/esp-ecran/verdict
//           après le badge         : "BADGE_OK|<personne>" ou "REFUSE|<raison>"
//           après le questionnaire : "ACCORDE|<personne>"  ou "REFUSE|<raison>"
//
//  Bibliothèques : Grove - LCD RGB Backlight par Seeed Studio
//                  MFRC522 par GithubCommunity
//                  PubSubClient par Nick O'Leary (MQTT)
// ============================================================
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <rgb_lcd.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// ------------------------------------------------------------
//  Paramètres Réseau & MQTT : dans secrets.h (jamais sur Git)
//  WIFI_SSID, WIFI_PASSWORD, MQTT_HOST (192.168.137.1),
//  MQTT_PORT (1883), MQTT_USER ("esp-ecran"), MQTT_PASSWORD
//  Voir secrets.example.h pour le modèle.
// ------------------------------------------------------------
#include "secrets.h"

// ------------------------------------------------------------
//  Topics MQTT
//  Ce compte ne peut écrire QUE sous esp/esp-ecran/
//  et lire sous sentinel/consigne/esp-ecran
// ------------------------------------------------------------
const char* TOPIC_STATUS    = "esp/esp-ecran/status";
const char* TOPIC_BADGE     = "esp/esp-ecran/badge";
const char* TOPIC_DISTANCE  = "esp/esp-ecran/distance";
const char* TOPIC_RESULTATS = "esp/esp-ecran/resultats";
const char* TOPIC_CONSIGNE  = "sentinel/consigne/esp-ecran";
const char* TOPIC_ETAPE     = "sentinel/consigne/esp-ecran/etape";
const char* TOPIC_VERDICT   = "sentinel/consigne/esp-ecran/verdict";

const unsigned long DELAI_VERDICT_MS        = 6000;   // attente max d'un verdict du serveur
const unsigned long DUREE_AFFICHAGE_FIN_MS  = 5000;   // durée d'affichage du verdict final
const unsigned long DUREE_REFUS_BADGE_MS    = 4000;   // durée d'affichage d'un badge refusé

String clientId;   // "esp-ecran-" + identifiant de la puce en hexadécimal

const unsigned long INTERVALLE_ENVOI_DISTANCE_MS = 2000;   // au plus une mesure toutes les 2 s
const unsigned long INTERVALLE_RECONNEXION_MS    = 5000;   // un essai MQTT toutes les 5 s

// ------------------------------------------------------------
//  Broches (voir le schéma de câblage)
// ------------------------------------------------------------
//  Écran Grove (I2C)  : SCL -> D1, SDA -> D2  (broches I2C par défaut)
//  RC522 (SPI)        : SCK -> D5, MISO -> D6, MOSI -> D7, SDA -> D3
//                       RST -> 3,3 V
//  Libres pour la suite : D8, RX, TX
//  (SDA du RC522 sur D3 et pas D8 : sur D8, l'ESP8266 ne démarre plus / ne se téléverse plus)
const uint8_t PIN_RFID_SS = D3;  // SDA du RC522
const uint8_t PIN_TRIG    = D4;
const uint8_t PIN_ECHO    = D0;
const uint8_t PIN_JOY     = A0;

// ------------------------------------------------------------
//  Réglages
// ------------------------------------------------------------
const int SEUIL_GAUCHE = 350;    // joystick : en dessous = gauche
const int SEUIL_DROITE = 700;    // au-dessus = droite
const int SEUIL_CLIC   = 1000;   // au-dessus = appui

const unsigned long DELAI_ATTENTE_BADGE_MS = 20000;  // 20 s pour badger après le code
const unsigned long DELAI_INACTIVITE_MS    = 30000;  // 30 s sans toucher au joystick

// ------------------------------------------------------------
//  Questions (pas d'accents, pas de guillemets)
//  Réponses : 13 caractères max, 6 réponses max par question
//  La consigne du serveur doit donner les réponses dans cet ordre,
//  écrites exactement comme ici.
// ------------------------------------------------------------
struct Question {
  const char* texte;
  const char* reponses[6];
  int nbReponses;
};

Question questions[] = {
  {"Comment tu te sens aujourd'hui ?",    {"Super", "Bien", "Bof", "Pas top"},                 4},
  {"Ta saison preferee ?",                {"Printemps", "Ete", "Automne", "Hiver"},            4},
  {"Chat ou chien ?",                     {"Chat", "Chien", "Les deux", "Aucun"},              4},
  {"Ton repas prefere ?",                 {"Pizza", "Burger", "Sushi", "Pates", "Autre"},      5},
  {"Tu aimes l'electronique ?",           {"Oui !", "Un peu", "Pas encore"},                   3},
};
const int NB_QUESTIONS = sizeof(questions) / sizeof(questions[0]);

// ------------------------------------------------------------
//  Objets et état
// ------------------------------------------------------------
rgb_lcd lcd;
MFRC522 rfid(PIN_RFID_SS, MFRC522::UNUSED_PIN);

enum Etat { VEILLE, ATTENTE_BADGE, QUESTIONNAIRE };
Etat etat = VEILLE;

// Types déclarés ici, avant toute fonction,
// sinon l'IDE Arduino ne les connaît pas à temps
enum Action { RIEN, GAUCHE, DROITE, CLIC };
enum PhaseVerdict { PHASE_AUCUNE, PHASE_BADGE, PHASE_FINALE };

// Session en cours
String badgeUid = "";
String badgeType = "";
float distanceArrivee = -1;
unsigned long debutSession = 0;
int reponsesDonnees[NB_QUESTIONS];
int numQuestion = 0;
int choix = 0;

// Consigne reçue du serveur (caméra) : valable jusqu'à consigneEcheance
bool          consigneActive = false;
String        consignePersonne = "";
String        consigneReponses[NB_QUESTIONS];
unsigned long consigneEcheance = 0;
bool          consigneModifiee = false;   // pour rafraîchir l'écran de veille

// Copie figée de la consigne pour la session en cours (usage unique)
bool   sessionAvecConsigne = false;
String sessionPersonne = "";

// Étape "digicode validé" reçue du serveur (traitée dans loop())
bool   etapeBadgeRecue = false;
String etapePersonne   = "";

// Verdict du serveur : accepté seulement pendant une phase d'attente,
// ouverte juste après l'envoi du badge ou des résultats
PhaseVerdict phaseVerdict = PHASE_AUCUNE;
bool   verdictRecu     = false;
String verdictDecision = "";   // BADGE_OK, ACCORDE ou REFUSE
String verdictTexte    = "";   // personne ou raison

// Minuteries
unsigned long derniereMesure = 0;
unsigned long debutAttenteBadge = 0;
unsigned long derniereAction = 0;

// Veille
bool  veilleAvecConsigne = false;   // ce que l'écran de veille affiche actuellement

// Défilement des questions longues
int posDefil = 0;
unsigned long dernierDefil = 0;

// ============================================================
//  Écran
// ============================================================
// Couleurs du rétroéclairage selon l'étape (rouge, vert, bleu : 0 à 255)
void couleur(uint8_t r, uint8_t g, uint8_t b) { lcd.setRGB(r, g, b); }
void couleurVeille()   { couleur(255, 255, 255); }  // blanc
void couleurAttente()  { couleur(0,   80,  255); }  // bleu  : passez un badge, vérification
void couleurSucces()   { couleur(0,   255, 0);   }  // vert  : accès accordé, badge OK
void couleurQuestion() { couleur(0,   200, 200); }  // cyan  : questionnaire
void couleurErreur()   { couleur(255, 0,   0);   }  // rouge : accès refusé, erreur
void couleurAlerte()   { couleur(255, 120, 0);   }  // orange: session annulée, pas de réponse

// L'écran ne connaît pas les accents : on remplace les plus courants (UTF-8)
String pourLcd(const String& s) {
  String out = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) { out += (char)c; continue; }
    if (c == 0xC3 && i + 1 < s.length()) {
      uint8_t n = (uint8_t)s[++i];
      char r = '?';
      if      (n >= 0xA0 && n <= 0xA5) r = 'a';
      else if (n == 0xA7)              r = 'c';
      else if (n >= 0xA8 && n <= 0xAB) r = 'e';
      else if (n >= 0xAC && n <= 0xAF) r = 'i';
      else if (n >= 0xB2 && n <= 0xB6) r = 'o';
      else if (n >= 0xB9 && n <= 0xBC) r = 'u';
      else if (n >= 0x80 && n <= 0x85) r = 'A';
      else if (n == 0x87)              r = 'C';
      else if (n >= 0x88 && n <= 0x8B) r = 'E';
      else if (n >= 0x8C && n <= 0x8F) r = 'I';
      else if (n >= 0x92 && n <= 0x96) r = 'O';
      else if (n >= 0x99 && n <= 0x9C) r = 'U';
      out += r;
      continue;
    }
    // Autre caractère multi-octets : on saute ses octets de suite
    while (i + 1 < s.length() && (((uint8_t)s[i + 1]) & 0xC0) == 0x80) i++;
    out += '?';
  }
  return out;
}

void lcdLigne(uint8_t ligne, String texte) {
  texte = pourLcd(texte);
  if (texte.length() > 16) texte = texte.substring(0, 16);
  while (texte.length() < 16) texte += ' ';
  lcd.setCursor(0, ligne);
  lcd.print(texte);
}

// ============================================================
//  Outils JSON
// ============================================================
String jsonTexte(const String& s) {
  String out = "\"";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) {
      char buf[7];
      snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
      out += buf;
    }
    else out += c;
  }
  out += "\"";
  return out;
}

String jsonDistance(float d) {
  return (d < 0) ? String("null") : String(d, 1);
}

// ============================================================
//  Messages reçus du serveur
// ============================================================
bool consigneValide() {
  return consigneActive && (long)(consigneEcheance - millis()) > 0;
}

void effacerConsigne() {
  consigneActive = false;
  consignePersonne = "";
  for (int i = 0; i < NB_QUESTIONS; i++) consigneReponses[i] = "";
}

// Consigne : "<validite_s>|<personne>|<rep1>|...|<repN>"   ("0|" = effacer)
void traiterConsigne(const String& message) {
  String champs[2 + NB_QUESTIONS];
  int nbChamps = 0;
  int debut = 0;
  while (nbChamps < 2 + NB_QUESTIONS) {
    int sep = message.indexOf('|', debut);
    if (sep < 0) { champs[nbChamps++] = message.substring(debut); break; }
    champs[nbChamps++] = message.substring(debut, sep);
    debut = sep + 1;
  }
  for (int i = 0; i < nbChamps; i++) champs[i].trim();

  long validite = champs[0].toInt();
  if (validite <= 0) {
    effacerConsigne();
    consigneModifiee = true;
    Serial.println("[CONSIGNE] Effacee");
    return;
  }

  effacerConsigne();
  consigneActive   = true;
  consignePersonne = (nbChamps > 1) ? champs[1] : "";
  for (int i = 0; i < NB_QUESTIONS; i++) {
    consigneReponses[i] = (2 + i < nbChamps) ? champs[2 + i] : "";
  }
  consigneEcheance = millis() + (unsigned long)validite * 1000UL;
  consigneModifiee = true;

  Serial.print("[CONSIGNE] "); Serial.print(consignePersonne);
  Serial.print(", valable "); Serial.print(validite); Serial.println(" s");
  if (nbChamps < 2 + NB_QUESTIONS) {
    Serial.print("[CONSIGNE] Attention : seulement "); Serial.print(nbChamps - 2);
    Serial.print(" reponse(s) sur "); Serial.println(NB_QUESTIONS);
  }
}

// Découpe "MOT|texte" en deux morceaux
void decouper(const String& message, String& mot, String& texte) {
  int sep = message.indexOf('|');
  mot   = (sep < 0) ? message : message.substring(0, sep);
  texte = (sep < 0) ? String("") : message.substring(sep + 1);
  mot.trim();
  texte.trim();
}

// Étape : "BADGE|<personne>" = digicode validé (traité dans loop())
void traiterEtape(const String& message) {
  String mot, texte;
  decouper(message, mot, texte);
  if (mot != "BADGE") {
    Serial.print("[ETAPE] Format inconnu, ignore : "); Serial.println(message);
    return;
  }
  etapePersonne   = texte;
  etapeBadgeRecue = true;
  Serial.print("[ETAPE] Code valide pour "); Serial.println(etapePersonne);
}

// Verdict : après le badge "BADGE_OK|<personne>" / "REFUSE|<raison>",
//           après le questionnaire "ACCORDE|<personne>" / "REFUSE|<raison>"
void traiterVerdict(const String& message) {
  if (phaseVerdict == PHASE_AUCUNE) {
    Serial.println("[VERDICT] Ignore (aucune attente de verdict en cours)");
    return;
  }

  String decision, texte;
  decouper(message, decision, texte);

  bool attendu =
    (phaseVerdict == PHASE_BADGE  && (decision == "BADGE_OK" || decision == "REFUSE")) ||
    (phaseVerdict == PHASE_FINALE && (decision == "ACCORDE"  || decision == "REFUSE"));
  if (!attendu) {
    Serial.print("[VERDICT] Inattendu a cette etape, ignore : "); Serial.println(message);
    return;
  }

  if (decision == "REFUSE" && texte.length() > 16) texte = texte.substring(0, 16);  // raison : 16 car. max

  verdictDecision = decision;
  verdictTexte    = texte;
  verdictRecu     = true;
  phaseVerdict    = PHASE_AUCUNE;
  Serial.print("[VERDICT] "); Serial.print(decision);
  Serial.print(" : ");       Serial.println(verdictTexte);
}

void recevoirMqtt(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("[MQTT] Recu sur "); Serial.print(topic);
  Serial.print(" : ");              Serial.println(message);

  if      (strcmp(topic, TOPIC_CONSIGNE) == 0) traiterConsigne(message);
  else if (strcmp(topic, TOPIC_ETAPE)    == 0) traiterEtape(message);
  else if (strcmp(topic, TOPIC_VERDICT)  == 0) traiterVerdict(message);
}

// ============================================================
//  Wi-Fi & MQTT
//  Non bloquant : si le réseau ou le serveur est absent, la borne
//  continue de fonctionner et réessaie toute seule toutes les 5 s.
// ============================================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long dernierEssaiMqtt = 0;
bool premierEssaiMqtt = true;
bool wifiAnnonce = false;

void demarrerReseau() {
  clientId = "esp-ecran-" + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(recevoirMqtt);
  mqtt.setBufferSize(1024);     // nos messages JSON dépassent les 256 octets par défaut
  mqtt.setSocketTimeout(2);     // ne pas bloquer la borne si le serveur ne répond pas
  Serial.print("[WIFI] Connexion a ");
  Serial.println(WIFI_SSID);
}

void abonner(const char* topic) {
  if (mqtt.subscribe(topic, 1)) {
    Serial.print("[MQTT] Abonne a "); Serial.println(topic);
  } else {
    Serial.print("[MQTT] ECHEC abonnement a "); Serial.println(topic);
  }
}

void gererReseau() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiAnnonce = false;
    return;                      // l'ESP se reconnecte tout seul au Wi-Fi
  }
  if (!wifiAnnonce) {
    wifiAnnonce = true;
    Serial.print("[WIFI] Connecte, IP : ");
    Serial.println(WiFi.localIP());
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  if (!premierEssaiMqtt && millis() - dernierEssaiMqtt < INTERVALLE_RECONNEXION_MS) return;
  premierEssaiMqtt = false;
  dernierEssaiMqtt = millis();

  Serial.print("[MQTT] Connexion a ");
  Serial.print(MQTT_HOST); Serial.print(":"); Serial.print(MQTT_PORT);
  Serial.print(" (id ");   Serial.print(clientId); Serial.print(")... ");

  // Last Will : si la borne se coupe, le serveur publie "offline" (QoS 1, retenu)
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                   TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    abonner(TOPIC_CONSIGNE);
    abonner(TOPIC_ETAPE);
    abonner(TOPIC_VERDICT);
  } else {
    Serial.print("echec, code ");
    Serial.println(mqtt.state());   // -2 = serveur injoignable, 4 = mauvais identifiants, 5 = non autorisé
  }
}

bool publierMqtt(const char* topic, const String& message) {
  if (!mqtt.connected()) {
    gererReseau();                 // une tentative si les 5 s sont écoulées
  }
  if (mqtt.connected() && mqtt.publish(topic, message.c_str())) {
    Serial.print("[MQTT] "); Serial.print(topic);
    Serial.print(" -> ");    Serial.println(message);
    return true;
  }
  Serial.print("[MQTT] ECHEC d'envoi sur "); Serial.println(topic);
  return false;
}

// Remplace les délais : la connexion MQTT reste vivante pendant l'attente
void attendre(unsigned long ms) {
  unsigned long debut = millis();
  while (millis() - debut < ms) {
    if (mqtt.connected()) mqtt.loop();
    delay(10);
  }
}

// Ouvre une phase d'attente de verdict et attend (DELAI_VERDICT_MS max).
// À appeler juste après l'envoi : le callback MQTT ne s'exécute que dans
// mqtt.loop(), donc aucun verdict ne peut arriver entre l'envoi et ici.
void attendreVerdict(PhaseVerdict phase) {
  verdictRecu     = false;
  verdictDecision = "";
  verdictTexte    = "";
  phaseVerdict    = phase;

  unsigned long debut = millis();
  while (!verdictRecu && millis() - debut < DELAI_VERDICT_MS) {
    if (mqtt.connected()) mqtt.loop();
    delay(10);
  }
  phaseVerdict = PHASE_AUCUNE;   // plus aucun verdict accepté après le délai
}

// ============================================================
//  Capteur de distance
// ============================================================
float mesurerDistance() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duree = pulseIn(PIN_ECHO, HIGH, 30000);   // 30 ms max (~5 m)
  if (duree == 0) return -1;                     // rien détecté
  return duree * 0.0343 / 2;
}

// ============================================================
//  Lecteur de badge : surveillance
//  Un pic de courant (démarrage du Wi-Fi, reconnexion) peut faire
//  redémarrer le RC522 tout seul : il répond encore, mais son antenne
//  reste éteinte et plus aucune carte n'est détectée.
//  Toutes les 2 s, on vérifie et on le relance si besoin.
// ============================================================
void initLecteur() {
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);   // antenne à pleine puissance (meilleure portée)
}

void surveillerLecteur() {
  static unsigned long derniereVerif = 0;
  if (millis() - derniereVerif < 2000) return;
  derniereVerif = millis();

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  byte antenne = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
  bool antenneOn = (antenne & 0x03) == 0x03;
  if (version == 0x00 || version == 0xFF || !antenneOn) {
    Serial.printf("[LECTEUR] Relance (version 0x%02X, antenne %s)\n",
                  version, antenneOn ? "allumee" : "ETEINTE");
    initLecteur();
  }
}

// ============================================================
//  Joystick : une action seulement quand on quitte le centre
// ============================================================
Action lireJoystick() {
  static bool auCentre = true;
  int x = analogRead(PIN_JOY);

  Action a = RIEN;
  if (x > SEUIL_CLIC)        a = CLIC;
  else if (x > SEUIL_DROITE) a = DROITE;
  else if (x < SEUIL_GAUCHE) a = GAUCHE;

  if (a == RIEN) { auCentre = true; return RIEN; }
  if (!auCentre) return RIEN;
  auCentre = false;
  return a;
}

// ============================================================
//  Étape 1 : veille
//  Ne démarre plus rien toute seule : on attend le code (étape
//  envoyée par le serveur) ou un badge.
// ============================================================
void afficherVeille() {
  couleurVeille();
  veilleAvecConsigne = consigneValide();
  if (veilleAvecConsigne) {
    lcdLigne(0, "Bonjour " + consignePersonne);
    lcdLigne(1, "Tapez le code");
  } else {
    lcdLigne(0, "  Bienvenue !");
    lcdLigne(1, "");
  }
}

void passerEnVeille() {
  etat = VEILLE;
  derniereMesure = 0;
  consigneModifiee = false;
  afficherVeille();
  Serial.println("[VEILLE] En attente du code ou d'un badge...");
}

void gererVeille() {
  // Consigne reçue, effacée ou expirée : on met l'écran à jour
  if (consigneModifiee || consigneValide() != veilleAvecConsigne) {
    consigneModifiee = false;
    afficherVeille();
  }

  if (millis() - derniereMesure < 300) return;
  derniereMesure = millis();

  float d = mesurerDistance();
  if (!veilleAvecConsigne) {
    if (d < 0) lcdLigne(1, "Approchez-vous");
    else       lcdLigne(1, "Distance " + String(d, 0) + " cm");
  }

  // Envoi de la distance au serveur (au plus toutes les 2 s)
  static unsigned long dernierEnvoiDistance = 0;
  static bool dejaEnvoye = false;
  if (mqtt.connected() &&
      (!dejaEnvoye || millis() - dernierEnvoiDistance >= INTERVALLE_ENVOI_DISTANCE_MS)) {
    dejaEnvoye = true;
    dernierEnvoiDistance = millis();
    publierMqtt(TOPIC_DISTANCE, "{\"distance_cm\":" + jsonDistance(d) + "}");
  }
}

// ============================================================
//  Étape 2 : code validé -> attente du badge
// ============================================================
void passerAttenteBadge(const String& personne) {
  etat = ATTENTE_BADGE;
  debutAttenteBadge = millis();
  couleurAttente();
  lcdLigne(0, "Code OK " + personne);
  lcdLigne(1, "Passez un badge");
  Serial.print("[CODE] Valide pour "); Serial.print(personne);
  Serial.println(", en attente du badge");
}

void gererAttenteBadge() {
  if (millis() - debutAttenteBadge > DELAI_ATTENTE_BADGE_MS) {
    Serial.println("[BADGE] Aucun badge, retour en veille");
    passerEnVeille();
  }
}

// ============================================================
//  Étape 3 : badge scanné (en veille ou après le code)
// ============================================================
void verifierBadge() {
  // UID en texte, ex : 3AF2910C
  badgeUid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) badgeUid += "0";
    badgeUid += String(rfid.uid.uidByte[i], HEX);
  }
  badgeUid.toUpperCase();

  MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);
  badgeType = String(rfid.PICC_GetTypeName(type));
  byte sak = rfid.uid.sak;
  byte taille = rfid.uid.size;

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.println("----------------------------------");
  Serial.println("[BADGE] Badge lu");
  Serial.print("  UID    : "); Serial.println(badgeUid);
  Serial.print("  Taille : "); Serial.print(taille); Serial.println(" octets");
  Serial.print("  Type   : "); Serial.println(badgeType);
  Serial.print("  SAK    : 0x"); Serial.println(sak, HEX);
  Serial.println("----------------------------------");

  // Distance de la personne au moment du badge (pour les résultats)
  distanceArrivee = mesurerDistance();

  // Envoi du badge au serveur
  bool envoye = publierMqtt(TOPIC_BADGE,
              "{\"badge\":" + jsonTexte(badgeUid) + ",\"type_badge\":" + jsonTexte(badgeType) + "}");

  if (!envoye) {
    couleurErreur();
    lcdLigne(0, "Badge lu");
    lcdLigne(1, "Envoi impossible");
    attendre(DUREE_REFUS_BADGE_MS);
    passerEnVeille();
    return;
  }

  // Attente de la réponse du serveur
  couleurAttente();
  lcdLigne(0, "Verification...");
  lcdLigne(1, "");
  Serial.println("[VERDICT] Badge : en attente du serveur...");
  attendreVerdict(PHASE_BADGE);

  if (verdictRecu && verdictDecision == "BADGE_OK") {
    couleurSucces();
    lcdLigne(0, "Badge OK");
    lcdLigne(1, verdictTexte);
    attendre(1500);
    demarrerQuestionnaire();
  } else if (verdictRecu) {   // REFUSE
    couleurErreur();
    lcdLigne(0, " ACCES REFUSE");
    lcdLigne(1, verdictTexte);
    attendre(DUREE_REFUS_BADGE_MS);
    passerEnVeille();
  } else {
    Serial.println("[VERDICT] Badge : pas de reponse du serveur");
    couleurAlerte();
    lcdLigne(0, "Pas de reponse");
    lcdLigne(1, "du serveur");
    attendre(DUREE_REFUS_BADGE_MS);
    passerEnVeille();
  }
}

// ============================================================
//  Étape 4 : questionnaire
// ============================================================
void afficherQuestion() {
  String t = questions[numQuestion].texte;
  if (t.length() <= 16) {
    lcdLigne(0, t);
  } else {
    String boucle = t + "    " + t;
    lcdLigne(0, boucle.substring(posDefil, posDefil + 16));
  }
}

void afficherReponse() {
  Question& q = questions[numQuestion];
  char buf[17];
  snprintf(buf, sizeof(buf), "%c %-13s%c",
           choix > 0 ? '<' : ' ',
           q.reponses[choix],
           choix < q.nbReponses - 1 ? '>' : ' ');
  lcdLigne(1, buf);
}

void nouvelleQuestion() {
  couleurQuestion();
  lcdLigne(0, "  Question " + String(numQuestion + 1) + "/" + String(NB_QUESTIONS));
  lcdLigne(1, "");
  attendre(1000);
  choix = 0;
  posDefil = 0;
  dernierDefil = millis();
  derniereAction = millis();
  afficherQuestion();
  afficherReponse();
}

void demarrerQuestionnaire() {
  etat = QUESTIONNAIRE;
  numQuestion = 0;
  debutSession = millis();
  etapeBadgeRecue = false;   // un code tapé pendant la vérification ne compte plus

  // Copie figée de la consigne pour toute la session, puis effacement (usage unique)
  sessionAvecConsigne = consigneValide();
  sessionPersonne = sessionAvecConsigne ? consignePersonne : "";
  effacerConsigne();
  consigneModifiee = false;

  Serial.print("[QUESTIONNAIRE] Debut");
  if (sessionAvecConsigne) { Serial.print(", personne attendue : "); Serial.print(sessionPersonne); }
  Serial.println();
  nouvelleQuestion();
}

void gererQuestionnaire(Action a) {
  Question& q = questions[numQuestion];

  if (a != RIEN) derniereAction = millis();

  if (a == GAUCHE && choix > 0) {
    choix--;
    afficherReponse();
  } else if (a == DROITE && choix < q.nbReponses - 1) {
    choix++;
    afficherReponse();
  } else if (a == CLIC) {
    reponsesDonnees[numQuestion] = choix;
    Serial.print("  Q"); Serial.print(numQuestion + 1);
    Serial.print(" : "); Serial.print(q.texte);
    Serial.print(" -> "); Serial.println(q.reponses[choix]);

    lcdLigne(0, "Reponse notee :");
    lcdLigne(1, q.reponses[choix]);
    attendre(1000);

    numQuestion++;
    if (numQuestion < NB_QUESTIONS) nouvelleQuestion();
    else                            terminerSession(true);
    return;
  }

  // Personne n'a touché au joystick depuis trop longtemps
  if (millis() - derniereAction > DELAI_INACTIVITE_MS) {
    terminerSession(false);
    return;
  }

  // Défilement des questions longues
  unsigned long attente = (posDefil == 0) ? 1500 : 350;
  if (strlen(q.texte) > 16 && millis() - dernierDefil > attente) {
    posDefil = (posDefil + 1) % (strlen(q.texte) + 4);
    dernierDefil = millis();
    afficherQuestion();
  }
}

// ============================================================
//  Étape 5 : fin, envoi des résultats et verdict du serveur
//  (c'est le serveur qui vérifie les réponses, plus l'ESP)
// ============================================================
bool envoyerResultats(bool complet) {
  // "verifie" reste dans le JSON pour garder le même format, mais vaut
  // toujours null : la vérification est faite par le serveur.
  String json = "{";
  json += "\"badge\":";       json += jsonTexte(badgeUid);  json += ",";
  json += "\"type_badge\":";  json += jsonTexte(badgeType); json += ",";
  json += "\"distance_cm\":"; json += jsonDistance(distanceArrivee); json += ",";
  json += "\"complet\":";     json += complet ? "true" : "false"; json += ",";
  json += "\"duree_s\":";     json += String((millis() - debutSession) / 1000); json += ",";
  json += "\"personne\":";    json += sessionAvecConsigne ? jsonTexte(sessionPersonne) : String("null"); json += ",";
  json += "\"verifie\":null,";
  json += "\"reponses\":[";
  for (int i = 0; i < numQuestion; i++) {
    if (i > 0) json += ",";
    json += "{\"question\":"; json += jsonTexte(questions[i].texte);
    json += ",\"reponse\":";  json += jsonTexte(questions[i].reponses[reponsesDonnees[i]]);
    json += "}";
  }
  json += "]}";

  return publierMqtt(TOPIC_RESULTATS, json);
}

void terminerSession(bool complet) {
  Serial.println(complet ? "[QUESTIONNAIRE] Termine" : "[QUESTIONNAIRE] Annule (inactivite)");

  bool envoye = envoyerResultats(complet);

  if (complet && envoye) {
    couleurAttente();
    lcdLigne(0, "Verification...");
    lcdLigne(1, "");
    Serial.println("[VERDICT] Final : en attente du serveur...");
    attendreVerdict(PHASE_FINALE);

    if (verdictRecu && verdictDecision == "ACCORDE") {
      couleurSucces();
      lcdLigne(0, " ACCES ACCORDE");
      lcdLigne(1, "Bienvenue " + verdictTexte);
    } else if (verdictRecu) {   // REFUSE
      couleurErreur();
      lcdLigne(0, " ACCES REFUSE");
      lcdLigne(1, verdictTexte);
    } else {
      Serial.println("[VERDICT] Final : pas de reponse du serveur");
      couleurAlerte();
      lcdLigne(0, "Pas de reponse");
      lcdLigne(1, "du serveur");
    }
    attendre(DUREE_AFFICHAGE_FIN_MS);
  } else if (complet) {
    // Résultats non envoyés : inutile d'attendre un verdict
    couleurErreur();
    lcdLigne(0, "Merci !");
    lcdLigne(1, "Envoi impossible");
    attendre(DUREE_AFFICHAGE_FIN_MS);
  } else {
    couleurAlerte();
    lcdLigne(0, "Temps ecoule");
    lcdLigne(1, "Session annulee");
    attendre(3000);
  }

  sessionAvecConsigne = false;
  sessionPersonne = "";
  passerEnVeille();
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("==================================");
  Serial.println("   BORNE SENTINEL-X (esp-ecran)");
  Serial.println("==================================");

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);

  lcd.begin(16, 2);              // démarre aussi l'I2C (SDA = D2, SCL = D1)
  couleurVeille();
  lcdLigne(0, "  SENTINEL-X");
  lcdLigne(1, "Demarrage...");

  SPI.begin();
  initLecteur();
  delay(50);
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("Lecteur RC522, version 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    Serial.println("ERREUR : lecteur de badge non detecte, verifie le cablage !");
    couleurErreur();
    lcdLigne(1, "ERREUR lecteur");
  } else {
    lcdLigne(1, "Lecteur badge OK");
  }
  delay(1500);

  demarrerReseau();
  passerEnVeille();
}

void loop() {
  gererReseau();
  Action a = lireJoystick();

  // Digicode validé (message "BADGE|<personne>" du serveur)
  if (etapeBadgeRecue) {
    etapeBadgeRecue = false;
    if (etat == VEILLE || etat == ATTENTE_BADGE) passerAttenteBadge(etapePersonne);
    else Serial.println("[ETAPE] Ignoree : questionnaire en cours");
  }

  // Badge scanné : accepté en veille comme après le code
  if (etat == VEILLE || etat == ATTENTE_BADGE) surveillerLecteur();
  if ((etat == VEILLE || etat == ATTENTE_BADGE) &&
      rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    verifierBadge();
    return;
  }

  switch (etat) {
    case VEILLE:        gererVeille();         break;
    case ATTENTE_BADGE: gererAttenteBadge();   break;
    case QUESTIONNAIRE: gererQuestionnaire(a); break;
  }

  attendre(20);
}
