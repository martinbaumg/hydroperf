// Test minimal de l'écran OLED 0.96" SSD1306 (I2C)
//   ESP32 :   pio run -e test_oled -t upload -t monitor
//             GND -> GND   VDD -> 3V3   SCK -> IO22   SDA -> IO21
//   ESP8266 : pio run -e test_oled_esp8266 -t upload -t monitor
//             GND -> G     VDD -> 3V3   SCK -> D1     SDA -> D2
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Broches de l'ESP32 par défaut ; l'ESP8266 les définit dans platformio.ini (build_flags)
#ifndef OLED_SDA
#define OLED_SDA 21
#define OLED_SCL 22
#endif

Adafruit_SSD1306 oled(128, 64, &Wire, -1);   // -1 : pas de broche reset

// Liste les appareils présents sur le bus I2C (l'OLED répond en 0x3C ou 0x3D)
uint8_t chercherOled() {
  uint8_t trouve = 0;
  Serial.println("Recherche sur le bus I2C...");
  for (uint8_t adr = 1; adr < 127; adr++) {
    Wire.beginTransmission(adr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  appareil trouve a l'adresse 0x%02X\n", adr);
      if (adr == 0x3C || adr == 0x3D) trouve = adr;
    }
  }
  return trouve;
}

// Le module OLED tire SDA et SCK vers VDD : une broche reliée à un écran alimenté lit HIGH
void verifierFil(int broche, const char* nom) {
#ifdef ESP8266
  // Pas de résistance de tirage vers la masse : on vide la broche à 0 V puis on la laisse libre,
  // seule la résistance de l'écran peut la faire remonter
  pinMode(broche, OUTPUT);
  digitalWrite(broche, LOW);
  delayMicroseconds(50);
  pinMode(broche, INPUT);
  delayMicroseconds(200);
#else
  pinMode(broche, INPUT_PULLDOWN);
  delay(5);
#endif
  bool relie = digitalRead(broche) == HIGH;
  Serial.printf("  GPIO%d (%s) : %s\n", broche, nom,
                relie ? "relie a l'ecran alimente" : "rien (fil absent, mauvaise broche ou ecran pas alimente)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Verification des fils :");
  verifierFil(OLED_SDA, "SDA");
  verifierFil(OLED_SCL, "SCK");

  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t adresse = chercherOled();
  if (adresse == 0) {
    Serial.println("Essai avec SDA et SCK inverses...");
#ifndef ESP8266
    Wire.end();
#endif
    Wire.begin(OLED_SCL, OLED_SDA);
    adresse = chercherOled();
    if (adresse) Serial.println("  -> SDA et SCK sont inverses : echange les deux fils");
  }
#ifndef ESP8266
  // Rien trouvé : on cherche sur quelles broches les fils SDA/SCK de l'écran sont vraiment branchés
  if (adresse == 0) {
    const int BROCHES[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
    int trouvees[4];
    int nb = 0;
    Serial.println("Recherche des fils de l'ecran sur toutes les broches...");
    for (int b : BROCHES) {
      pinMode(b, INPUT_PULLDOWN);
      delay(2);
      if (digitalRead(b) == HIGH) {
        Serial.printf("  IO%d est reliee a l'ecran alimente\n", b);
        if (nb < 4) trouvees[nb++] = b;
      }
      pinMode(b, INPUT);
    }
    if (nb == 0) Serial.println("  aucune : l'ecran n'est pas alimente (fil VDD ou GND)");
    for (int i = 0; i < nb && adresse == 0; i++) {
      for (int j = 0; j < nb && adresse == 0; j++) {
        if (i == j) continue;
        Wire.end();
        Wire.begin(trouvees[i], trouvees[j]);
        adresse = chercherOled();
        if (adresse) Serial.printf("  -> l'ecran repond avec SDA=IO%d et SCK=IO%d\n", trouvees[i], trouvees[j]);
      }
    }
  }
#endif
  if (adresse == 0 || !oled.begin(SSD1306_SWITCHCAPVCC, adresse)) {
    Serial.println("OLED introuvable : verifie les 4 fils (GND/VDD inverses ?)");
    while (true) delay(1000);
  }
  Serial.printf("OLED OK a l'adresse 0x%02X\n", adresse);

  oled.clearDisplay();               // remet tous les pixels à zéro
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.println("Hello !");
  oled.drawRect(0, 0, 128, 64, SSD1306_WHITE);   // cadre pour vérifier les bords
  oled.display();                    // rien ne s'affiche avant display()
}

void loop() {
  // Compteur pour vérifier que l'écran se met bien à jour
  oled.fillRect(10, 36, 108, 20, SSD1306_BLACK);
  oled.setCursor(10, 36);
  oled.print(millis() / 1000);
  oled.print(" s");
  oled.display();
  delay(1000);
}
