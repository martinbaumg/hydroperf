// Écran OLED 0.96" SSD1306 (128x64, une couleur) en I2C
// Même fonctions que ecran.cpp (version ST7735) : le reste de HydroPerf ne change pas.
// Branchement : GND -> GND   VDD -> 3V3 ou VCC   SCK -> IO22   SDA -> IO21
#include "ecran.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_SDA 21
#define OLED_SCL 22

static const int LARGEUR = 128;
static const int HAUTEUR = 64;

static Adafruit_SSD1306 oled(LARGEUR, HAUTEUR, &Wire, -1);  // -1 : pas de broche reset

// L'écran n'a qu'une couleur : le type de boisson est écrit sous la goutte
static const char* NOMS[] = {"EAU", "ELECTRO", "ISO", "MAISON"};

// Goutte : pointe en (x, y), partie ronde de rayon r en dessous (hauteur totale 3r).
// Pleine quand il faut boire, en contour quand il n'y a rien à boire.
static void dessinerGoutte(int x, int y, int r, bool pleine) {
  if (pleine) {
    oled.fillTriangle(x, y, x - r, y + 2 * r, x + r, y + 2 * r, SSD1306_WHITE);
    oled.fillCircle(x, y + 2 * r, r, SSD1306_WHITE);
    oled.fillCircle(x - r / 3, y + 2 * r - r / 4, r / 5, SSD1306_BLACK);  // reflet
  } else {
    oled.drawCircle(x, y + 2 * r, r, SSD1306_WHITE);
    oled.fillRect(x - r + 1, y + r, 2 * r - 1, r, SSD1306_BLACK);  // efface le haut du cercle, remplacé par la pointe
    oled.drawLine(x, y, x - r, y + 2 * r, SSD1306_WHITE);
    oled.drawLine(x, y, x + r, y + 2 * r, SSD1306_WHITE);
  }
}

// Écrit un texte centré horizontalement sur centreX
static void texteCentre(const char* texte, int centreX, int y, uint8_t taille) {
  int16_t x1, y1;
  uint16_t l, h;
  oled.setTextSize(taille);
  oled.getTextBounds(texte, 0, 0, &x1, &y1, &l, &h);
  oled.setCursor(centreX - l / 2, y);
  oled.print(texte);
}

// ---------- Extinction automatique ----------

static uint32_t delaiExtinction = 0;
static unsigned long extinctionA = 0;
static bool allume = false;

static void allumer() {
  // Un OLED n'a pas de rétroéclairage : on rallume directement l'affichage
  if (!allume) oled.ssd1306_command(SSD1306_DISPLAYON);
  allume = true;
  if (delaiExtinction) extinctionA = millis() + delaiExtinction;
}

void ecranEteindre() {
  oled.clearDisplay();
  oled.display();
  oled.ssd1306_command(SSD1306_DISPLAYOFF);  // l'écran ne consomme presque plus rien
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
  Wire.begin(OLED_SDA, OLED_SCL);
  // L'écran répond en 0x3C (parfois 0x3D selon les modèles)
  uint8_t adresse = 0x3C;
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(0x3D);
    if (Wire.endTransmission() == 0) adresse = 0x3D;
    else Serial.println("Ecran OLED introuvable : verifie les 4 fils (SDA -> IO21, SCK -> IO22)");
  }
  oled.begin(SSD1306_SWITCHCAPVCC, adresse);
  oled.setTextColor(SSD1306_WHITE);
  oled.cp437(true);
  oled.setTextWrap(false);
  oled.clearDisplay();  // remet tous les pixels à zéro au démarrage
  oled.display();
  allume = false;
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
  oled.clearDisplay();

  // Barres à gauche sur toute la hauteur : toujours GORGEES_MAX emplacements pour que rien ne bouge,
  // pleines pour les gorgées restant à boire, en contour pour les autres
  const int barreX = 4, barreLargeur = 58, barreHauteur = 16, espacement = 6;
  const int startY = (HAUTEUR - (GORGEES_MAX * barreHauteur + (GORGEES_MAX - 1) * espacement)) / 2;
  for (int i = 0; i < GORGEES_MAX; i++) {
    int y = startY + i * (barreHauteur + espacement);
    if (i < nbGorgees) {
      oled.fillRoundRect(barreX, y, barreLargeur, barreHauteur, 3, SSD1306_WHITE);
    } else {
      oled.drawRoundRect(barreX, y, barreLargeur, barreHauteur, 3, SSD1306_WHITE);
    }
  }

  // Grande goutte à droite (hauteur 45 px) avec le type de boisson en dessous
  dessinerGoutte(97, 1, 15, nbGorgees > 0);
  texteCentre(NOMS[boisson], 97, 55, 1);
  oled.display();
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

// Découpe le texte en lignes de "parLigne" caractères sans couper les mots (sauf s'ils sont trop longs).
// Renvoie le nombre de lignes, ou maxLignes + 1 si le texte ne tient pas.
static int decouper(String reste, int parLigne, String lignes[], int maxLignes) {
  int nb = 0;
  reste.trim();
  while (reste.length()) {
    if (nb == maxLignes) return maxLignes + 1;
    int coupe = (int)reste.length() <= parLigne ? reste.length() : reste.lastIndexOf(' ', parLigne);
    if (coupe <= 0) coupe = min((int)reste.length(), parLigne);
    lignes[nb++] = reste.substring(0, coupe);
    reste = reste.substring(coupe);
    reste.trim();
  }
  return nb;
}

void ecranMessage(const char* texte) {
  allumer();
  String converti = versCp437(texte);

  // En gros (10 caractères x 3 lignes) si le texte tient, sinon en petit (21 x 7)
  const int MAX_LIGNES = 7;
  String lignes[MAX_LIGNES];
  int taille = 2;
  int nb = decouper(converti, LARGEUR / 12, lignes, HAUTEUR / 18);
  if (nb > HAUTEUR / 18) {
    taille = 1;
    nb = min(decouper(converti, LARGEUR / 6, lignes, MAX_LIGNES), MAX_LIGNES);
  }

  oled.clearDisplay();
  oled.setTextSize(taille);
  const int hauteurLigne = taille == 2 ? 18 : 9;
  int y = (HAUTEUR - nb * hauteurLigne) / 2;
  for (int i = 0; i < nb; i++) {
    oled.setCursor((LARGEUR - lignes[i].length() * 6 * taille) / 2, y + i * hauteurLigne);
    oled.print(lignes[i]);
  }
  oled.display();
}
