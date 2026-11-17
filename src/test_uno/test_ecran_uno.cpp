// Test minimal de l'écran ST7735 (VCC toujours sur 3,3 V)
//   ESP8266 : pio run -e test_esp8266 -t upload
//             SCL -> D5  SDA -> D7  CS -> D8  DC -> D1  RES -> D2
//   UNO :     pio run -e test_uno -t upload  (ne marche pas sans adaptation 5 V -> 3,3 V)
//             SCL -> D13  SDA -> D11  CS -> D10  DC -> D9  RES -> D8
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif

// Broches du UNO par défaut ; l'ESP8266 les définit dans platformio.ini (build_flags)
#ifndef TFT_CS
#define TFT_CS  10
#define TFT_DC   9
#define TFT_RST  8
#endif

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

void setup() {
#ifdef ESP8266
  // Coupe le Wi-Fi (allumé par défaut) : ses pics de courant font chuter le 3,3 V de l'écran
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
#endif
  // Même vitesse que les messages de démarrage de l'ESP8266, pour tout lire d'un coup
  Serial.begin(74880);
  Serial.println(F("\nDemarrage du test ecran"));

  tft.initR(INITR_BLACKTAB);      // si couleurs/bords bizarres : INITR_GREENTAB ou INITR_REDTAB
  // La librairie envoie à 32 MHz par défaut, trop vite pour le ST7735 avec des fils volants :
  // un octet déformé peut devenir une commande "reset" ou "veille" et l'écran devient blanc
  tft.setSPISpeed(8000000);
  tft.fillScreen(ST77XX_BLACK);   // remet tous les pixels à zéro au démarrage

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("Hello !");
}

void loop() {
  // rouge, vert, bleu en boucle dans un carré pour vérifier les couleurs
  uint16_t couleurs[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE};
  for (uint16_t c : couleurs) {
    tft.fillRect(20, 50, 88, 88, c);
    delay(1000);
  }
  // Message de vie : si le compteur repart de 0, la carte a redémarré
  Serial.print(F("En marche depuis "));
  Serial.print(millis() / 1000);
  Serial.println(F(" s"));
}
