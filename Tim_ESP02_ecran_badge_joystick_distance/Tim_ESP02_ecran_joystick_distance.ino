// ============================================================
//  BORNE SANS LECTEUR DE BADGE - Workshop SENTINEL-X
//  NodeMCU ESP8266 + écran Grove LCD RGB Backlight (I2C)
//  + capteur de distance HC-SR04 + joystick Grove
//
//  Fonctionnement :
//   1. VEILLE        : "Bienvenue !" + distance mesurée (publiée au serveur)
//   2. DEMARRAGE     : le questionnaire démarre directement quand la borne
//                      reçoit l'un de ces messages du serveur :
//                       - une consigne "60|pierre|Super|..." -> "Bonjour pierre"
//                       - une étape "BADGE|<personne>"       -> "Code OK"
//                      (plus de lecteur de badge, donc plus d'étape badge)
//   3. QUESTIONNAIRE : gauche/droite = choisir, appui = valider
//                      (30 s sans action = session annulée)
//   4. FIN           : résultats envoyés en JSON au serveur MQTT,
//                      puis attente du verdict du serveur (6 s max) :
//                      ACCES ACCORDE / ACCES REFUSE / Pas de reponse
//
//  Contrat MQTT (compte "esp-ecran") :
//   Publie  esp/esp-ecran/status     "online" / "offline" (Last Will), retenu
//           esp/esp-ecran/distance   {"distance_cm":42.5} ou {"distance_cm":null}
//           esp/esp-ecran/resultats  récap JSON de la session
//                                    ("badge" et "type_badge" valent null)
//   Lit     sentinel/consigne/esp-ecran
//           "<validite_s>|<personne>|<rep1>|<rep2>|<rep3>|<rep4>|<rep5>"
//           "0|" = effacer la consigne
//           sentinel/consigne/esp-ecran/etape
//           "BADGE|<personne>"  (digicode validé)
//           sentinel/consigne/esp-ecran/verdict
//           après le questionnaire : "ACCORDE|<personne>" ou "REFUSE|<raison>"
//
//  Bibliothèques : Grove - LCD RGB Backlight par Seeed Studio
//                  PubSubClient par Nick O'Leary (MQTT)
// ============================================================
#include <Wire.h>
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
const char* TOPIC_DISTANCE  = "esp/esp-ecran/distance";
const char* TOPIC_RESULTATS = "esp/esp-ecran/resultats";
const char* TOPIC_CONSIGNE  = "sentinel/consigne/esp-ecran";
const char* TOPIC_ETAPE     = "sentinel/consigne/esp-ecran/etape";
const char* TOPIC_VERDICT   = "sentinel/consigne/esp-ecran/verdict";

const unsigned long DELAI_VERDICT_MS        = 6000;   // attente max du verdict du serveur
const unsigned long DUREE_AFFICHAGE_FIN_MS  = 5000;   // durée d'affichage du verdict final

String clientId;   // "esp-ecran-" + identifiant de la puce en hexadécimal

const unsigned long INTERVALLE_ENVOI_DISTANCE_MS = 2000;   // au plus une mesure toutes les 2 s
const unsigned long INTERVALLE_RECONNEXION_MS    = 5000;   // un essai MQTT toutes les 5 s

// ------------------------------------------------------------
//  Broches (voir le schéma de câblage)
// ------------------------------------------------------------
//  Écran Grove (I2C)  : SCL -> D1, SDA -> D2  (broches I2C par défaut)
//  HC-SR04            : Trig -> D4, Echo -> D0
//  Joystick           : X -> A0
//  Libres : D3, D5, D6, D7, D8, RX, TX
const uint8_t PIN_TRIG = D4;
const uint8_t PIN_ECHO = D0;
const uint8_t PIN_JOY  = A0;

// ------------------------------------------------------------
//  Réglages
// ------------------------------------------------------------
const int SEUIL_GAUCHE = 350;    // joystick : en dessous = gauche
const int SEUIL_DROITE = 700;    // au-dessus = droite
const int SEUIL_CLIC   = 1000;   // au-dessus = appui

const unsigned long DELAI_INACTIVITE_MS = 30000;  // 30 s sans toucher au joystick

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

enum Etat { VEILLE, QUESTIONNAIRE };
Etat etat = VEILLE;

// Types déclarés ici, avant toute fonction,
// sinon l'IDE Arduino ne les connaît pas à temps
enum Action { RIEN, GAUCHE, DROITE, CLIC };

// Session en cours
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
bool          consigneNouvelle = false;   // une consigne valide vient d'arriver -> lancer le questionnaire

// Copie figée de la consigne pour la session en cours (usage unique)
bool   sessionAvecConsigne = false;
String sessionPersonne = "";

// Étape "digicode validé" reçue du serveur (traitée dans loop())
bool   etapeCodeRecue = false;
String etapePersonne  = "";

// Verdict final du serveur : accepté seulement pendant l'attente,
// ouverte juste après l'envoi des résultats
bool   attenteVerdict  = false;
bool   verdictRecu     = false;
String verdictDecision = "";   // ACCORDE ou REFUSE
String verdictTexte    = "";   // personne ou raison

// Minuteries
unsigned long derniereMesure = 0;
unsigned long derniereAction = 0;

// Veille
bool veilleAvecConsigne = false;   // ce que l'écran de veille affiche actuellement

// Défilement des questions longues
int posDefil = 0;
unsigned long dernierDefil = 0;

// ============================================================
//  Écran
// ============================================================
// Couleurs du rétroéclairage selon l'étape (rouge, vert, bleu : 0 à 255)
void couleur(uint8_t r, uint8_t g, uint8_t b) { lcd.setRGB(r, g, b); }
void couleurVeille()   { couleur(255, 255, 255); }  // blanc
void couleurAttente()  { couleur(0,   80,  255); }  // bleu  : vérification
void couleurSucces()   { couleur(0,   255, 0);   }  // vert  : code OK, accès accordé
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
  consigneNouvelle = true;

  Serial.print("[CONSIGNE] "); Serial.print(consignePersonne);
  Serial.print(", valable "); Serial.print(validite); Serial.println(" s");
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
  etapePersonne  = texte;
  etapeCodeRecue = true;
  Serial.print("[ETAPE] Code valide pour "); Serial.println(etapePersonne);
}

// Verdict final : "ACCORDE|<personne>" / "REFUSE|<raison>"
void traiterVerdict(const String& message) {
  if (!attenteVerdict) {
    Serial.println("[VERDICT] Ignore (aucune attente de verdict en cours)");
    return;
  }

  String decision, texte;
  decouper(message, decision, texte);

  if (decision != "ACCORDE" && decision != "REFUSE") {
    Serial.print("[VERDICT] Inattendu, ignore : "); Serial.println(message);
    return;
  }
  if (decision == "REFUSE" && texte.length() > 16) texte = texte.substring(0, 16);  // raison : 16 car. max

  verdictDecision = decision;
  verdictTexte    = texte;
  verdictRecu     = true;
  attenteVerdict  = false;
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

// Ouvre l'attente du verdict et attend (DELAI_VERDICT_MS max).
// À appeler juste après l'envoi : le callback MQTT ne s'exécute que dans
// mqtt.loop(), donc aucun verdict ne peut arriver entre l'envoi et ici.
void attendreVerdict() {
  verdictRecu     = false;
  verdictDecision = "";
  verdictTexte    = "";
  attenteVerdict  = true;

  unsigned long debut = millis();
  while (!verdictRecu && millis() - debut < DELAI_VERDICT_MS) {
    if (mqtt.connected()) mqtt.loop();
    delay(10);
  }
  attenteVerdict = false;   // plus aucun verdict accepté après le délai
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
//  Étape 1 : veille (on attend le code validé par le serveur)
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
  Serial.println("[VEILLE] En attente du code...");
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
//  Étape 2 : consigne ou code reçu -> questionnaire directement
// ============================================================
void lancerSession(const String& titre, const String& personne) {
  Serial.print("[SESSION] "); Serial.print(titre);
  Serial.print(" "); Serial.println(personne);

  // Distance de la personne au démarrage (pour les résultats)
  distanceArrivee = mesurerDistance();

  couleurSucces();
  lcdLigne(0, titre);
  lcdLigne(1, personne);
  attendre(1500);

  demarrerQuestionnaire();
}

// ============================================================
//  Étape 3 : questionnaire
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
  etapeCodeRecue   = false;   // un message arrivé pendant l'affichage ne relance rien
  consigneNouvelle = false;

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
//  Étape 4 : fin, envoi des résultats et verdict du serveur
//  (c'est le serveur qui vérifie les réponses)
// ============================================================
bool envoyerResultats(bool complet) {
  // Même format qu'avant : "badge" et "type_badge" valent null (plus de lecteur),
  // "verifie" vaut null (la vérification est faite par le serveur).
  String json = "{";
  json += "\"badge\":null,";
  json += "\"type_badge\":null,";
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
    Serial.println("[VERDICT] En attente du serveur...");
    attendreVerdict();

    if (verdictRecu && verdictDecision == "ACCORDE") {
      couleurSucces();
      lcdLigne(0, " ACCES ACCORDE");
      lcdLigne(1, "Bienvenue " + verdictTexte);
    } else if (verdictRecu) {   // REFUSE
      couleurErreur();
      lcdLigne(0, " ACCES REFUSE");
      lcdLigne(1, verdictTexte);
    } else {
      Serial.println("[VERDICT] Pas de reponse du serveur");
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
  Serial.println("   version sans lecteur de badge");
  Serial.println("==================================");

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);

  lcd.begin(16, 2);              // démarre aussi l'I2C (SDA = D2, SCL = D1)
  couleurVeille();
  lcdLigne(0, "  SENTINEL-X");
  lcdLigne(1, "Demarrage...");
  delay(1500);

  demarrerReseau();
  passerEnVeille();
}

void loop() {
  gererReseau();
  Action a = lireJoystick();

  // Consigne reçue ("60|pierre|Super|...") -> questionnaire directement
  if (consigneNouvelle) {
    consigneNouvelle = false;
    if (etat == VEILLE && consigneValide()) { lancerSession("Bonjour", consignePersonne); return; }
    if (etat != VEILLE) Serial.println("[CONSIGNE] Ignoree pour le demarrage : questionnaire deja en cours");
  }

  // Digicode validé ("BADGE|<personne>") -> questionnaire directement
  if (etapeCodeRecue) {
    etapeCodeRecue = false;
    if (etat == VEILLE) { lancerSession("Code OK", etapePersonne); return; }
    Serial.println("[ETAPE] Ignoree : questionnaire en cours");
  }

  switch (etat) {
    case VEILLE:        gererVeille();         break;
    case QUESTIONNAIRE: gererQuestionnaire(a); break;
  }

  attendre(20);
}
