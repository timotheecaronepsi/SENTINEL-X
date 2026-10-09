// ============================================================
//  ESP-ECRAN - Workshop SENTINEL-X
//  NodeMCU ESP8266 + écran Grove LCD RGB Backlight (I2C)
//  + capteur de distance HC-SR04 + joystick Grove
//
//  L'écran affiche les étapes du parcours SENTINEL-X.
//  LE SERVEUR DÉCIDE DE TOUT : l'écran ne lance jamais les questions
//  de lui-même (ni distance, ni joystick, ni consigne).
//  Le badge est lu par un autre ESP (esp-carte).
//
//  Parcours affiché :
//   Accueil       : "Bienvenue !" + distance (publiée au serveur)
//   VISAGE|Pierre : vert 1,5 s "1/4 Visage OK" / "Bonjour Pierre"
//                   puis bleu "2/4 Pierre" / "Tapez le code"   (60 s max)
//   CODE|Pierre   : vert 1,5 s "2/4 Code correct" / "Pierre"
//                   puis bleu "3/4 Pierre" / "Passez la carte" (25 s max)
//   QUESTIONS|P.  : vert 1,5 s "3/4 Carte valide" / "4/4 Questions..."
//                   puis questionnaire au joystick
//   ERREUR|texte  : rouge 2,5 s "REFUSE" / texte, puis retour à l'étape en cours
//   VEILLE|       : retour immédiat à l'accueil
//   Fin           : résultats envoyés, puis verdict ACCORDE / REFUSE (6 s max)
//
//  Contrat MQTT (compte "esp-ecran") :
//   Publie  esp/esp-ecran/status     "online" / "offline" (Last Will), retenu
//           esp/esp-ecran/distance   {"distance_cm":42.5} ou {"distance_cm":null}
//           esp/esp-ecran/resultats  récap JSON du questionnaire
//   Lit     sentinel/consigne/esp-ecran/etape    "TYPE|valeur" (voir ci-dessus)
//           sentinel/consigne/esp-ecran/verdict  "ACCORDE|<personne>" ou "REFUSE|<raison>"
//           sentinel/consigne/esp-ecran          ignoré complètement (même "0|")
//
//  Le callback MQTT ne bloque jamais : il range seulement les messages
//  dans une file, et tout l'affichage est fait dans loop().
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
// ------------------------------------------------------------
const char* TOPIC_STATUS    = "esp/esp-ecran/status";
const char* TOPIC_DISTANCE  = "esp/esp-ecran/distance";
const char* TOPIC_RESULTATS = "esp/esp-ecran/resultats";
const char* TOPIC_CONSIGNE  = "sentinel/consigne/esp-ecran";
const char* TOPIC_ETAPE     = "sentinel/consigne/esp-ecran/etape";
const char* TOPIC_VERDICT   = "sentinel/consigne/esp-ecran/verdict";

String clientId;   // "esp-ecran-" + identifiant de la puce en hexadécimal

// ------------------------------------------------------------
//  Durées
// ------------------------------------------------------------
const unsigned long DUREE_FLASH_VERT_MS      = 1500;    // écran vert de validation
const unsigned long DUREE_ERREUR_MS          = 2500;    // écran rouge "REFUSE"
const unsigned long DUREE_ETAPE_CODE_MS      = 60000;   // "Tapez le code" : 60 s max
const unsigned long DUREE_ETAPE_CARTE_MS     = 25000;   // "Passez la carte" : 25 s max
const unsigned long DELAI_VERDICT_MS         = 6000;    // attente max du verdict final
const unsigned long DUREE_AFFICHAGE_FIN_MS   = 5000;    // affichage du verdict final
const unsigned long DELAI_INACTIVITE_MS      = 30000;   // 30 s sans toucher au joystick
const unsigned long INTERVALLE_ENVOI_DISTANCE_MS = 2000; // au plus une mesure toutes les 2 s
const unsigned long INTERVALLE_RECONNEXION_MS    = 5000; // un essai MQTT toutes les 5 s

// ------------------------------------------------------------
//  Broches
// ------------------------------------------------------------
//  Écran Grove (I2C)  : SCL -> D1, SDA -> D2
//  HC-SR04            : Trig -> D4, Echo -> D0
//  Joystick           : X -> A0
//  Libres : D3, D5, D6, D7, D8, RX, TX
const uint8_t PIN_TRIG = D4;
const uint8_t PIN_ECHO = D0;
const uint8_t PIN_JOY  = A0;

// ------------------------------------------------------------
//  Joystick
// ------------------------------------------------------------
const int SEUIL_GAUCHE = 350;    // en dessous = gauche
const int SEUIL_DROITE = 700;    // au-dessus = droite
const int SEUIL_CLIC   = 1000;   // au-dessus = appui

// ------------------------------------------------------------
//  Questions (pas d'accents, pas de guillemets)
//  Réponses : 13 caractères max, 6 réponses max par question
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
//  Types (déclarés avant toute fonction pour l'IDE Arduino)
// ------------------------------------------------------------
enum Etape  { ACCUEIL, ATTENTE_CODE, ATTENTE_CARTE, QUESTIONNAIRE };
enum Action { RIEN, GAUCHE, DROITE, CLIC };

// ------------------------------------------------------------
//  État
// ------------------------------------------------------------
rgb_lcd lcd;

Etape         etape = ACCUEIL;
String        personne = "";            // personne de l'étape en cours
bool          etapeAvecEcheance = false;
unsigned long etapeEcheance = 0;        // retour à l'accueil à cette date (millis)

// Écran temporaire (vert de validation ou rouge d'erreur) : à la fin,
// on réaffiche l'écran de l'étape en cours
bool          ecranTemporaire = false;
unsigned long finEcranTemporaire = 0;

// Questionnaire
bool          questionnaireDemarre = false;
float         distanceArrivee = -1;
unsigned long debutSession = 0;
int           reponsesDonnees[NB_QUESTIONS];
int           numQuestion = 0;
int           choix = 0;
unsigned long derniereAction = 0;
int           posDefil = 0;
unsigned long dernierDefil = 0;

// Verdict final : accepté seulement pendant l'attente
bool   attenteVerdict  = false;
bool   verdictRecu     = false;
String verdictDecision = "";   // ACCORDE ou REFUSE
String verdictTexte    = "";

// Accueil : mesure de distance
unsigned long derniereMesure = 0;

// File des messages d'étape reçus (remplie par le callback, vidée par loop())
const int TAILLE_FILE = 8;
String fileMessages[TAILLE_FILE];
int    fileDebut  = 0;
int    fileTaille = 0;

// ============================================================
//  Écran
// ============================================================
void couleur(uint8_t r, uint8_t g, uint8_t b) { lcd.setRGB(r, g, b); }
void couleurAccueil()  { couleur(255, 255, 255); }  // blanc
void couleurEtape()    { couleur(0,   80,  255); }  // bleu  : étape en cours, vérification
void couleurSucces()   { couleur(0,   255, 0);   }  // vert  : validation, accès accordé
void couleurQuestion() { couleur(0,   200, 200); }  // cyan  : questionnaire
void couleurErreur()   { couleur(255, 0,   0);   }  // rouge : refus
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

// Affiche un écran temporaire ; à la fin, loop() réaffiche l'étape en cours
void ecranFlash(bool erreur, const String& ligne1, const String& ligne2, unsigned long duree) {
  if (erreur) couleurErreur(); else couleurSucces();
  lcdLigne(0, ligne1);
  lcdLigne(1, ligne2);
  ecranTemporaire = true;
  finEcranTemporaire = millis() + duree;
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

// Découpe "TYPE|valeur" en deux morceaux
void decouper(const String& message, String& type, String& valeur) {
  int sep = message.indexOf('|');
  type   = (sep < 0) ? message : message.substring(0, sep);
  valeur = (sep < 0) ? String("") : message.substring(sep + 1);
  type.trim();
  valeur.trim();
}

// ============================================================
//  File des messages d'étape
// ============================================================
void empiler(const String& message) {
  if (fileTaille == TAILLE_FILE) {          // file pleine : on perd le plus ancien
    fileDebut = (fileDebut + 1) % TAILLE_FILE;
    fileTaille--;
    Serial.println("[ETAPE] File pleine, message le plus ancien perdu");
  }
  fileMessages[(fileDebut + fileTaille) % TAILLE_FILE] = message;
  fileTaille++;
}

bool depiler(String& message) {
  if (fileTaille == 0) return false;
  message = fileMessages[fileDebut];
  fileMessages[fileDebut] = "";
  fileDebut = (fileDebut + 1) % TAILLE_FILE;
  fileTaille--;
  return true;
}

// ============================================================
//  Callback MQTT : ne bloque jamais, ne touche pas à l'écran
// ============================================================
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
  if (decision == "REFUSE" && texte.length() > 16) texte = texte.substring(0, 16);
  verdictDecision = decision;
  verdictTexte    = texte;
  verdictRecu     = true;
  attenteVerdict  = false;
}

void recevoirMqtt(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("[MQTT] Recu sur "); Serial.print(topic);
  Serial.print(" : ");              Serial.println(message);

  if (strcmp(topic, TOPIC_ETAPE) == 0) {
    empiler(message);
  } else if (strcmp(topic, TOPIC_VERDICT) == 0) {
    traiterVerdict(message);
  } else if (strcmp(topic, TOPIC_CONSIGNE) == 0) {
    // Consigne totalement ignorée (y compris "0|") : elle ne change rien à l'écran.
    // Pour revenir à l'accueil, le serveur envoie "VEILLE|" sur .../etape.
    Serial.println("[CONSIGNE] Ignoree");
  }
}

// ============================================================
//  Wi-Fi & MQTT (non bloquant, reconnexion toutes les 5 s)
// ============================================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long dernierEssaiMqtt = 0;
bool premierEssaiMqtt = true;
bool wifiAnnonce = false;

void demarrerReseau() {
  clientId = "esp-ecran-" + String(ESP.getChipId(), HEX);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // pas de mise en veille du Wi-Fi : évite les décrochages
  WiFi.setAutoReconnect(true);          // reconnexion Wi-Fi automatique
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(recevoirMqtt);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(2);
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
    return;
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

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                   TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    abonner(TOPIC_ETAPE);
    abonner(TOPIC_VERDICT);
    abonner(TOPIC_CONSIGNE);
  } else {
    Serial.print("echec, code ");
    Serial.println(mqtt.state());   // -2 = serveur injoignable, 4 = mauvais identifiants, 5 = non autorisé
  }
}

bool publierMqtt(const char* topic, const String& message) {
  if (!mqtt.connected()) gererReseau();
  if (mqtt.connected() && mqtt.publish(topic, message.c_str())) {
    Serial.print("[MQTT] "); Serial.print(topic);
    Serial.print(" -> ");    Serial.println(message);
    return true;
  }
  Serial.print("[MQTT] ECHEC d'envoi sur "); Serial.print(topic);
  Serial.print(" (connecte : "); Serial.print(mqtt.connected() ? "oui" : "non");
  Serial.print(", taille "); Serial.print(message.length()); Serial.println(" octets)");
  return false;
}

// Envoi important (résultats) : si la connexion vient de tomber, on se
// reconnecte tout de suite (sans attendre les 5 s) et on réessaie,
// pendant 5 s maximum.
bool publierFiable(const char* topic, const String& message) {
  unsigned long debut = millis();
  for (int essai = 1; ; essai++) {
    if (!mqtt.connected()) {
      premierEssaiMqtt = true;   // force une tentative de reconnexion immédiate
      gererReseau();
    }
    if (publierMqtt(topic, message)) return true;
    if (millis() - debut > 5000) return false;
    Serial.print("[MQTT] Nouvel essai d'envoi ("); Serial.print(essai + 1); Serial.println(")...");
    attendre(500);
  }
}

// Pause qui garde MQTT vivant (les messages reçus sont mis en file)
void attendre(unsigned long ms) {
  unsigned long debut = millis();
  while (millis() - debut < ms) {
    if (mqtt.connected()) mqtt.loop();
    delay(10);
  }
}

// ============================================================
//  Capteur de distance & joystick
// ============================================================
float mesurerDistance() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duree = pulseIn(PIN_ECHO, HIGH, 30000);   // 30 ms max (~5 m)
  if (duree == 0) return -1;
  return duree * 0.0343 / 2;
}

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
//  Questionnaire
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

void reafficherQuestion() {
  couleurQuestion();
  posDefil = 0;
  dernierDefil = millis();
  derniereAction = millis();
  afficherQuestion();
  afficherReponse();
}

void nouvelleQuestion() {
  couleurQuestion();
  lcdLigne(0, "  Question " + String(numQuestion + 1) + "/" + String(NB_QUESTIONS));
  lcdLigne(1, "");
  attendre(1000);
  choix = 0;
  reafficherQuestion();
}

void demarrerQuestionnaire() {
  questionnaireDemarre = true;
  numQuestion = 0;
  debutSession = millis();
  Serial.print("[QUESTIONNAIRE] Debut pour "); Serial.println(personne);
  nouvelleQuestion();
}

// ============================================================
//  Écrans des étapes
// ============================================================
void afficherEtape() {
  switch (etape) {
    case ACCUEIL:
      couleurAccueil();
      lcdLigne(0, "  Bienvenue !");
      lcdLigne(1, "");
      derniereMesure = 0;   // la distance s'affiche tout de suite en ligne 2
      break;
    case ATTENTE_CODE:
      couleurEtape();
      lcdLigne(0, "2/4 " + personne);
      lcdLigne(1, "Tapez le code");
      break;
    case ATTENTE_CARTE:
      couleurEtape();
      lcdLigne(0, "3/4 " + personne);
      lcdLigne(1, "Passez la carte");
      break;
    case QUESTIONNAIRE:
      if (!questionnaireDemarre) demarrerQuestionnaire();
      else                       reafficherQuestion();
      break;
  }
}

void passerAccueil() {
  etape = ACCUEIL;
  personne = "";
  etapeAvecEcheance = false;
  ecranTemporaire = false;
  questionnaireDemarre = false;
  afficherEtape();
  Serial.println("[ETAPE] Accueil");
}

void changerEtape(Etape nouvelle, const String& qui, unsigned long duree) {
  etape = nouvelle;
  personne = qui;
  etapeAvecEcheance = (duree > 0);
  etapeEcheance = millis() + duree;
}

// Un message "TYPE|valeur" reçu sur .../etape
void traiterMessageEtape(const String& message) {
  String type, valeur;
  decouper(message, type, valeur);
  Serial.print("[ETAPE] "); Serial.print(type); Serial.print(" : "); Serial.println(valeur);

  if (type == "VEILLE") {
    passerAccueil();

  } else if (type == "VISAGE") {
    if (etape == QUESTIONNAIRE) {
      Serial.println("[ETAPE] VISAGE ignore : questionnaire en cours");
    } else if (etape == ATTENTE_CARTE && valeur == personne) {
      Serial.println("[ETAPE] VISAGE ignore : cette personne en est deja a la carte");
    } else if (etape == ATTENTE_CODE && valeur == personne) {
      etapeEcheance = millis() + DUREE_ETAPE_CODE_MS;   // renvoi périodique : on prolonge seulement
      Serial.println("[ETAPE] VISAGE renouvele : 60 s de plus");
    } else {
      changerEtape(ATTENTE_CODE, valeur, DUREE_ETAPE_CODE_MS);
      ecranFlash(false, "1/4 Visage OK", "Bonjour " + valeur, DUREE_FLASH_VERT_MS);
    }

  } else if (type == "CODE") {
    if (etape == QUESTIONNAIRE) {
      Serial.println("[ETAPE] CODE ignore : questionnaire en cours");
    } else {
      changerEtape(ATTENTE_CARTE, valeur, DUREE_ETAPE_CARTE_MS);
      ecranFlash(false, "2/4 Code correct", valeur, DUREE_FLASH_VERT_MS);
    }

  } else if (type == "QUESTIONS") {
    if (etape == QUESTIONNAIRE) {
      Serial.println("[ETAPE] QUESTIONS ignore : questionnaire deja en cours");
    } else {
      changerEtape(QUESTIONNAIRE, valeur, 0);
      questionnaireDemarre = false;
      distanceArrivee = mesurerDistance();
      ecranFlash(false, "3/4 Carte valide", "4/4 Questions...", DUREE_FLASH_VERT_MS);
    }

  } else if (type == "ERREUR") {
    // Rouge 2,5 s, puis retour à l'écran de l'étape en cours (sans toucher aux délais)
    ecranFlash(true, "REFUSE", valeur, DUREE_ERREUR_MS);

  } else {
    Serial.print("[ETAPE] Type inconnu, ignore : "); Serial.println(message);
  }
}

// ============================================================
//  Accueil : distance affichée et publiée
// ============================================================
void gererAccueil() {
  if (millis() - derniereMesure < 300) return;
  derniereMesure = millis();

  float d = mesurerDistance();
  if (d < 0) lcdLigne(1, "Approchez-vous");
  else       lcdLigne(1, "Distance " + String(d, 0) + " cm");

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
//  Fin du questionnaire : résultats puis verdict final
// ============================================================
bool envoyerResultats(bool complet) {
  // Même format qu'avant. "badge"/"type_badge" : null (lus par esp-carte),
  // "verifie" : null (c'est le serveur qui vérifie).
  String json = "{";
  json += "\"badge\":null,";
  json += "\"type_badge\":null,";
  json += "\"distance_cm\":"; json += jsonDistance(distanceArrivee); json += ",";
  json += "\"complet\":";     json += complet ? "true" : "false"; json += ",";
  json += "\"duree_s\":";     json += String((millis() - debutSession) / 1000); json += ",";
  json += "\"personne\":";    json += (personne.length() > 0) ? jsonTexte(personne) : String("null"); json += ",";
  json += "\"verifie\":null,";
  json += "\"reponses\":[";
  for (int i = 0; i < numQuestion; i++) {
    if (i > 0) json += ",";
    json += "{\"question\":"; json += jsonTexte(questions[i].texte);
    json += ",\"reponse\":";  json += jsonTexte(questions[i].reponses[reponsesDonnees[i]]);
    json += "}";
  }
  json += "]}";
  bool ok = publierFiable(TOPIC_RESULTATS, json);
  Serial.println(ok ? "[MQTT] Resultats envoyes" : "[MQTT] ECHEC envoi resultats");
  return ok;
}

// Ouvre l'attente du verdict juste après l'envoi et attend (6 s max)
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
  attenteVerdict = false;
}

void terminerSession(bool complet) {
  Serial.println(complet ? "[QUESTIONNAIRE] Termine" : "[QUESTIONNAIRE] Annule (inactivite)");
  bool envoye = envoyerResultats(complet);

  if (complet && envoye) {
    couleurEtape();
    lcdLigne(0, "Verification...");
    lcdLigne(1, "");
    attendreVerdict();

    if (verdictRecu && verdictDecision == "ACCORDE") {
      couleurSucces();
      lcdLigne(0, " ACCES ACCORDE");
      lcdLigne(1, "Bienvenue " + verdictTexte);
    } else if (verdictRecu) {
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

  passerAccueil();
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

  if (millis() - derniereAction > DELAI_INACTIVITE_MS) {
    terminerSession(false);
    return;
  }

  unsigned long attente = (posDefil == 0) ? 1500 : 350;
  if (strlen(q.texte) > 16 && millis() - dernierDefil > attente) {
    posDefil = (posDefil + 1) % (strlen(q.texte) + 4);
    dernierDefil = millis();
    afficherQuestion();
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("==================================");
  Serial.println("   SENTINEL-X : esp-ecran");
  Serial.println("==================================");

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);

  lcd.begin(16, 2);              // I2C : SDA = D2, SCL = D1
  couleurAccueil();
  lcdLigne(0, "  SENTINEL-X");
  lcdLigne(1, "Demarrage...");
  delay(1500);

  demarrerReseau();
  passerAccueil();
}

void loop() {
  gererReseau();
  Action a = lireJoystick();

  // 1. Messages d'étape reçus du serveur (dans l'ordre d'arrivée)
  String message;
  while (depiler(message)) traiterMessageEtape(message);

  // 2. Délai de l'étape écoulé -> accueil
  if (etapeAvecEcheance && (long)(millis() - etapeEcheance) >= 0) {
    Serial.println("[ETAPE] Delai ecoule");
    passerAccueil();
  }

  // 3. Fin d'un écran temporaire -> écran de l'étape en cours
  if (ecranTemporaire && (long)(millis() - finEcranTemporaire) >= 0) {
    ecranTemporaire = false;
    afficherEtape();
  }

  // 4. Étape en cours (rien pendant un écran temporaire)
  if (!ecranTemporaire) {
    if (etape == ACCUEIL)                                gererAccueil();
    else if (etape == QUESTIONNAIRE && questionnaireDemarre) gererQuestionnaire(a);
  }

  attendre(20);
}
