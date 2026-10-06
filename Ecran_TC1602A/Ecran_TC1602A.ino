// ============================================================
//  Questionnaire sur écran LCD 16x2 + joystick (NodeMCU ESP8266)
//  Joystick : gauche/droite = changer de réponse, appui = valider
// ============================================================
#include <LiquidCrystal.h>

// RS, E, DB4, DB5, DB6, DB7
LiquidCrystal lcd(D1, D2, D5, D6, D7, D0);

// ---- Réglages joystick (ajuste avec tes valeurs de calibration) ----
const int SEUIL_GAUCHE = 350;   // en dessous = gauche
const int SEUIL_DROITE = 700;   // au-dessus  = droite
const int SEUIL_CLIC   = 1000;  // au-dessus  = appui

// ---- Les questions ----
// Pas d'accents : l'écran ne sait pas les afficher.
// Réponses : 13 caractères max, 4 réponses max par question.
// "bonne" = numéro de la bonne réponse (0 = la première).
struct Question {
  const char* texte;
  const char* reponses[4];
  int nbReponses;
  int bonne;
};

Question questions[] = {
  {"Capitale de l'Australie ?",          {"Sydney", "Canberra", "Melbourne"},             3, 1},
  {"Combien de pattes a une araignee ?", {"6", "8", "10"},                                3, 1},
  {"Planete la plus proche du Soleil ?", {"Venus", "Mercure", "Mars"},                    3, 1},
  {"7 x 8 = ?",                          {"56", "54", "64"},                              3, 0},
  {"Qui a peint la Joconde ?",           {"Picasso", "Monet", "De Vinci", "Van Gogh"},    4, 2},
  {"Plus long fleuve de France ?",       {"La Seine", "Le Rhone", "La Loire"},            3, 2},
  {"Annee de la prise de la Bastille ?", {"1789", "1792", "1815"},                        3, 0},
  {"Symbole chimique de l'or ?",         {"Ag", "Au", "Or", "Fe"},                        4, 1},
  {"Plus grand ocean du monde ?",        {"Atlantique", "Pacifique", "Indien", "Arctique"}, 4, 1},
  {"Joueurs par equipe au foot ?",       {"9", "10", "11", "12"},                         4, 2},
};
const int NB_QUESTIONS = sizeof(questions) / sizeof(questions[0]);

// ---- État du jeu ----
enum Etat { ACCUEIL, JEU, FIN };
Etat etat = ACCUEIL;

int ordre[NB_QUESTIONS];   // ordre (mélangé) des questions
int numQuestion = 0;
int choix = 0;
int score = 0;

int posDefil = 0;                  // défilement du texte de la question
unsigned long dernierDefil = 0;

// ------------------------------------------------------------
//  Joystick : renvoie une action seulement quand on quitte le centre
// ------------------------------------------------------------
enum Action { RIEN, GAUCHE, DROITE, CLIC };

Action lireJoystick() {
  static bool auCentre = true;
  int x = analogRead(A0);

  Action a = RIEN;
  if (x > SEUIL_CLIC)        a = CLIC;
  else if (x > SEUIL_DROITE) a = DROITE;
  else if (x < SEUIL_GAUCHE) a = GAUCHE;

  if (a == RIEN) { auCentre = true; return RIEN; }
  if (!auCentre) return RIEN;      // on attend le retour au centre
  auCentre = false;
  return a;
}

// ------------------------------------------------------------
//  Affichage
// ------------------------------------------------------------
Question& questionCourante() {
  return questions[ordre[numQuestion]];
}

void afficherQuestion() {
  String t = questionCourante().texte;
  String ligne;
  if (t.length() <= 16) {
    ligne = t;
  } else {                         // texte trop long : il défile
    String boucle = t + "    " + t;
    ligne = boucle.substring(posDefil, posDefil + 16);
  }
  while (ligne.length() < 16) ligne += ' ';
  lcd.setCursor(0, 0);
  lcd.print(ligne);
}

void afficherReponse() {
  Question& q = questionCourante();
  char buf[17];
  snprintf(buf, sizeof(buf), "%c %-13s%c",
           choix > 0 ? '<' : ' ',
           q.reponses[choix],
           choix < q.nbReponses - 1 ? '>' : ' ');
  lcd.setCursor(0, 1);
  lcd.print(buf);
}

void ecranAccueil() {
  etat = ACCUEIL;
  lcd.clear();
  lcd.print(" QUESTIONNAIRE");
  lcd.setCursor(0, 1);
  lcd.print("Clic = commencer");
}

void nouvelleQuestion() {
  lcd.clear();
  lcd.print("  Question ");
  lcd.print(numQuestion + 1);
  lcd.print("/");
  lcd.print(NB_QUESTIONS);
  delay(1200);

  lcd.clear();
  choix = 0;
  posDefil = 0;
  dernierDefil = millis();
  afficherQuestion();
  afficherReponse();
}

void afficherFin() {
  etat = FIN;
  lcd.clear();
  lcd.print("Score : ");
  lcd.print(score);
  lcd.print("/");
  lcd.print(NB_QUESTIONS);
  lcd.setCursor(0, 1);
  if (score == NB_QUESTIONS)          lcd.print("Parfait !");
  else if (score * 2 >= NB_QUESTIONS) lcd.print("Pas mal !");
  else                                lcd.print("Peut mieux faire");
  delay(3000);
  lcd.setCursor(0, 1);
  lcd.print("Clic = rejouer  ");
}

// ------------------------------------------------------------
//  Logique du jeu
// ------------------------------------------------------------
void melanger() {
  for (int i = 0; i < NB_QUESTIONS; i++) ordre[i] = i;
  for (int i = NB_QUESTIONS - 1; i > 0; i--) {
    int j = random(i + 1);
    int tmp = ordre[i]; ordre[i] = ordre[j]; ordre[j] = tmp;
  }
}

void demarrerPartie() {
  melanger();
  numQuestion = 0;
  score = 0;
  etat = JEU;
  nouvelleQuestion();
}

void valider() {
  Question& q = questionCourante();
  lcd.clear();
  if (choix == q.bonne) {
    score++;
    lcd.print("Bonne reponse !");
    lcd.setCursor(0, 1);
    lcd.print("Score : ");
    lcd.print(score);
  } else {
    lcd.print("Rate ! C'etait :");
    lcd.setCursor(0, 1);
    lcd.print(q.reponses[q.bonne]);
  }
  delay(2000);

  numQuestion++;
  if (numQuestion < NB_QUESTIONS) nouvelleQuestion();
  else                            afficherFin();
}

// ------------------------------------------------------------
void setup() {
  lcd.begin(16, 2);
  randomSeed(RANDOM_REG32);   // vrai hasard de l'ESP8266
  ecranAccueil();
}

void loop() {
  Action a = lireJoystick();

  if (etat == ACCUEIL || etat == FIN) {
    if (a == CLIC) demarrerPartie();
  } else {
    Question& q = questionCourante();

    if (a == GAUCHE && choix > 0) {
      choix--;
      afficherReponse();
    } else if (a == DROITE && choix < q.nbReponses - 1) {
      choix++;
      afficherReponse();
    } else if (a == CLIC) {
      valider();
      return;
    }

    // Défilement des questions longues (pause au début)
    unsigned long attente = (posDefil == 0) ? 1500 : 350;
    if (strlen(q.texte) > 16 && millis() - dernierDefil > attente) {
      posDefil = (posDefil + 1) % (strlen(q.texte) + 4);
      dernierDefil = millis();
      afficherQuestion();
    }
  }

  delay(20);
}
