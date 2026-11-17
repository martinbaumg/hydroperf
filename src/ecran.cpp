#include "ecran.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// Écran Joy-IT 1.8" ST7735 (160x128) en SPI
#define TFT_CS    5   // Fil JAUNE  -> IO5
#define TFT_DC   16   // Fil ORANGE -> IO16 (RS/DC)
#define TFT_RST  25   // RES -> IO25 (IO17 est pris par le buzzer)
#define TFT_MOSI 23   // SDA
#define TFT_SCLK 18   // SCK
// Rétroéclairage : brancher la broche LED de l'écran sur IO27 (au lieu de 3V3)
// pour que l'extinction automatique coupe vraiment la lumière et économise la batterie
#define TFT_BL   27

// SPI logiciel : plus lent que le SPI matériel (32 MHz) mais fiable avec les longs fils de la breadboard.
// Pour revenir au SPI matériel : Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST)
static Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

static const int LARGEUR = 160;
static const int HAUTEUR = 128;
static const uint16_t GRIS = 0x4208;

// Couleur et nom affichés pour chaque type de boisson
static const uint16_t COULEURS[] = {ST77XX_BLUE, ST77XX_RED, ST77XX_YELLOW, ST77XX_GREEN};
static const char* NOMS[] = {"EAU", "ELECTROLYTE", "ISOTONIQUE", "MAISON"};

// Goutte : pointe en (x, y), partie ronde de rayon 12 en dessous
static void dessinerGoutte(int x, int y, uint16_t couleur) {
  tft.fillTriangle(x, y, x - 12, y + 24, x + 12, y + 24, couleur);
  tft.fillCircle(x, y + 24, 12, couleur);
  // Reflet blanc pour le relief
  tft.fillCircle(x - 4, y + 20, 3, ST77XX_WHITE);
}

// Écrit un texte centré horizontalement sur centreX
static void texteCentre(const char* texte, int centreX, int y, uint8_t taille, uint16_t couleur) {
  int16_t x1, y1;
  uint16_t l, h;
  tft.setTextSize(taille);
  tft.getTextBounds(texte, 0, 0, &x1, &y1, &l, &h);
  tft.setCursor(centreX - l / 2, y);
  tft.setTextColor(couleur);
  tft.print(texte);
}

// ---------- Extinction automatique ----------

static uint32_t delaiExtinction = 0;
static unsigned long extinctionA = 0;
static bool allume = false;

static void allumer() {
  digitalWrite(TFT_BL, HIGH);
  allume = true;
  if (delaiExtinction) extinctionA = millis() + delaiExtinction;
}

void ecranEteindre() {
  tft.fillScreen(ST77XX_BLACK);
  digitalWrite(TFT_BL, LOW);
  allume = false;
}

bool ecranAllume() {
  return allume;
}

void ecranDelaiExtinction(uint32_t delaiMs) {
  delaiExtinction = delaiMs;
  if (allume && delaiMs) extinctionA = millis() + delaiMs;
}

void ecranTick() {
  if (allume && delaiExtinction && (long)(millis() - extinctionA) >= 0) ecranEteindre();
}

void ecranInit() {
  pinMode(TFT_BL, OUTPUT);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);  // Paysage : 160 de large, 128 de haut
  tft.fillScreen(ST77XX_BLACK);
  allumer();
}

// ---------- Consignes ----------

int barresPourConsigne(int consigneMl, int mlParGorgee) {
  if (consigneMl <= 0) return 0;
  int gorgees = (consigneMl + mlParGorgee / 2) / max(1, mlParGorgee);
  return constrain(gorgees, 1, GORGEES_MAX);
}

void ecranConsigne(int consigneMl, int mlParGorgee, TypeBoisson boisson) {
  ecranAfficher(barresPourConsigne(consigneMl, mlParGorgee), boisson);
}

void ecranAfficher(int nbGorgees, TypeBoisson boisson) {
  allumer();
  nbGorgees = max(0, nbGorgees);
  boisson = (TypeBoisson)constrain((int)boisson, 0, (int)MAISON);
  tft.fillScreen(ST77XX_BLACK);

  // Barres à gauche : toujours GORGEES_MAX emplacements pour que rien ne bouge,
  // pleines pour les gorgées restant à boire, en contour gris pour les autres
  const int barreX = 14, barreLargeur = 65, barreHauteur = 14, espacement = 10;
  const int hauteurTotale = GORGEES_MAX * barreHauteur + (GORGEES_MAX - 1) * espacement;
  const int startY = (105 - hauteurTotale) / 2;
  for (int i = 0; i < GORGEES_MAX; i++) {
    int y = startY + i * (barreHauteur + espacement);
    if (i < nbGorgees) {
      tft.fillRoundRect(barreX, y, barreLargeur, barreHauteur, 3, ST77XX_WHITE);
    } else {
      tft.drawRoundRect(barreX, y, barreLargeur, barreHauteur, 3, GRIS);
    }
  }

  // Goutte à droite, grisée quand il n'y a rien à boire
  uint16_t couleur = nbGorgees > 0 ? COULEURS[boisson] : GRIS;
  dessinerGoutte(122, 22, couleur);
  texteCentre(NOMS[boisson], 122, 66, 1, couleur);

  // Message en bas
  char message[16];
  if (nbGorgees == 0) {
    strcpy(message, "BIEN JOUE !");
  } else {
    snprintf(message, sizeof(message), "%d GORGEE%s", nbGorgees, nbGorgees > 1 ? "S" : "");
  }
  texteCentre(message, LARGEUR / 2, HAUTEUR - 22, 2, nbGorgees > 0 ? ST77XX_WHITE : ST77XX_GREEN);
}

void ecranEffacer() {
  ecranEteindre();
}

// La police de l'écran utilise la table de caractères CP437 :
// on convertit les lettres accentuées UTF-8 les plus courantes en français
static String versCp437(const char* texte) {
  static const struct { const char* utf8; char cp437; } ACCENTS[] = {
    {"é", (char)0x82}, {"è", (char)0x8A}, {"ê", (char)0x88}, {"ë", (char)0x89}, {"à", (char)0x85},
    {"â", (char)0x83}, {"ç", (char)0x87}, {"î", (char)0x8C}, {"ï", (char)0x8B}, {"ô", (char)0x93},
    {"ù", (char)0x97}, {"û", (char)0x96}, {"ü", (char)0x81}, {"É", (char)0x90}, {"°", (char)0xF8},
  };
  String resultat;
  for (const char* p = texte; *p;) {
    if ((uint8_t)*p < 0x80) {
      resultat += *p++;
      continue;
    }
    bool trouve = false;
    for (const auto& a : ACCENTS) {
      size_t n = strlen(a.utf8);
      if (strncmp(p, a.utf8, n) == 0) {
        resultat += a.cp437;
        p += n;
        trouve = true;
        break;
      }
    }
    if (!trouve) {
      resultat += '?';
      p++;
      while (((uint8_t)*p & 0xC0) == 0x80) p++;  // saute la suite du caractère UTF-8
    }
  }
  return resultat;
}

void ecranMessage(const char* texte) {
  const int TAILLE = 2;                         // caractères de 12 x 16 pixels
  const int PAR_LIGNE = LARGEUR / (6 * TAILLE);  // 13 caractères
  const int MAX_LIGNES = HAUTEUR / (8 * TAILLE + 4);

  // Découpe en lignes sans couper les mots (sauf s'ils sont trop longs)
  allumer();
  String reste = versCp437(texte);
  reste.trim();
  String lignes[MAX_LIGNES];
  int nb = 0;
  while (reste.length() && nb < MAX_LIGNES) {
    int coupe = reste.length() <= PAR_LIGNE ? reste.length() : reste.lastIndexOf(' ', PAR_LIGNE);
    if (coupe <= 0) coupe = min((int)reste.length(), PAR_LIGNE);
    lignes[nb++] = reste.substring(0, coupe);
    reste = reste.substring(coupe);
    reste.trim();
  }

  tft.fillScreen(ST77XX_BLACK);
  tft.cp437(true);
  tft.setTextWrap(false);
  tft.setTextSize(TAILLE);
  tft.setTextColor(ST77XX_WHITE);
  const int hauteurLigne = 8 * TAILLE + 4;
  int y = (HAUTEUR - nb * hauteurLigne) / 2;
  for (int i = 0; i < nb; i++) {
    tft.setCursor((LARGEUR - lignes[i].length() * 6 * TAILLE) / 2, y + i * hauteurLigne);
    tft.print(lignes[i]);
  }
}
