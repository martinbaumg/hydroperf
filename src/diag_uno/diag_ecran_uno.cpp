// Diagnostic de l'écran ST7735 sur Arduino UNO ou ESP8266
//   UNO :     pio run -e diag_uno -t upload -t monitor
//   ESP8266 : pio run -e diag_esp8266 -t upload -t monitor
// 1. Vérifie qu'aucune broche de l'écran n'est en court-circuit ou à la masse
// 2. Essaie plusieurs réglages : chaque mode affiche son numéro sur fond rouge,
//    vert puis bleu. Le mode qui s'affiche correctement est le bon.
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// Broches du UNO par défaut ; l'ESP8266 les définit dans platformio.ini (build_flags)
#ifndef TFT_CS
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_MOSI 11
#define TFT_SCLK 13
#endif

const int PINS[] = {TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCLK};
const char* NOMS_PINS[] = {"CS", "DC", "RES", "SDA", "SCL"};
const int NB_PINS = 5;

struct Mode {
  const char* nom;
  bool spiLogiciel;
  uint8_t onglet;
};

const Mode MODES[] = {
  {"SPI materiel, BLACKTAB", false, INITR_BLACKTAB},
  {"SPI materiel, GREENTAB", false, INITR_GREENTAB},
  {"SPI materiel, REDTAB", false, INITR_REDTAB},
  {"SPI materiel, 1.44 pouce (128x128)", false, INITR_144GREENTAB},
  {"SPI materiel, 0.96 pouce (160x80)", false, INITR_MINI160x80},
  {"SPI logiciel (lent), BLACKTAB", true, INITR_BLACKTAB},
  {"SPI logiciel (lent), GREENTAB", true, INITR_GREENTAB},
};
const int NB_MODES = sizeof(MODES) / sizeof(MODES[0]);

void testerBroches() {
  Serial.println(F("\n=== Test des broches de l'ecran ==="));
  bool probleme = false;

  // Court-circuit : on met une broche à 0 V, les autres (tirées vers 5 V) ne doivent pas suivre
  for (int i = 0; i < NB_PINS; i++) {
    for (int j = 0; j < NB_PINS; j++) pinMode(PINS[j], INPUT_PULLUP);
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
    delay(5);
    for (int j = i + 1; j < NB_PINS; j++) {
      if (digitalRead(PINS[j]) == LOW) {
        Serial.print(F("  PROBLEME : court-circuit entre "));
        Serial.print(NOMS_PINS[i]);
        Serial.print(F(" et "));
        Serial.println(NOMS_PINS[j]);
        probleme = true;
      }
    }
  }

  // Broche reliée à la masse
  for (int i = 0; i < NB_PINS; i++) {
    pinMode(PINS[i], INPUT_PULLUP);
    delay(5);
    if (digitalRead(PINS[i]) == LOW) {
      Serial.print(F("  PROBLEME : "));
      Serial.print(NOMS_PINS[i]);
      Serial.println(F(" est reliee a GND"));
      probleme = true;
    }
    pinMode(PINS[i], INPUT);
  }

  if (!probleme) Serial.println(F("  OK : aucun court-circuit ni broche a la masse"));
}

void essayerMode(int n) {
  const Mode& mode = MODES[n];
  Serial.print(F("\n>>> MODE "));
  Serial.print(n);
  Serial.print(F(" : "));
  Serial.print(mode.nom);
  Serial.println(F(" -> regarde l'ecran"));

  Adafruit_ST7735* tft = mode.spiLogiciel
      ? new Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST)
      : new Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
  tft->initR(mode.onglet);

  const uint16_t FONDS[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE};
  for (uint16_t fond : FONDS) {
    tft->fillScreen(fond);
    tft->setTextColor(ST77XX_WHITE);
    tft->setTextSize(3);
    tft->setCursor(10, 20);
    tft->print(F("MODE "));
    tft->print(n);
    delay(1500);
  }

  delete tft;
  // Libère SCL/SDA pour que le mode logiciel puisse les piloter
  if (!mode.spiLogiciel) SPI.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  testerBroches();
}

void loop() {
  for (int n = 0; n < NB_MODES; n++) essayerMode(n);
  Serial.println(F("\n--- Fin du cycle, on recommence ---"));
}
