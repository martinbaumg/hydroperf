#include "commandes.h"
#include <Arduino.h>
#include "brochage.h"

static const int PINS[] = {PIN_LED, PIN_VIBREUR, PIN_BUZZER};
static const char* NOMS[] = {"led", "vibreur", "buzzer"};
static bool allumes[] = {false, false, false};

static int indexDe(int pin) {
  for (int i = 0; i < 3; i++) {
    if (PINS[i] == pin) return i;
  }
  return -1;
}

void commandesInit() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  // Le vibreur et le buzzer passent toujours par analogWrite (PWM) pour régler leur intensité
  analogWrite(PIN_VIBREUR, 0);
  analogWrite(PIN_BUZZER, 0);
}

void ecrireIntensite(int pin, int pourcent) {
  pourcent = constrain(pourcent, 0, 100);
  if (pin == PIN_LED) {
    digitalWrite(pin, pourcent > 0 ? HIGH : LOW);
  } else {
    analogWrite(pin, pourcent * 255 / 100);
  }
  int i = indexDe(pin);
  if (i >= 0) allumes[i] = pourcent > 0;
}

void ecrire(int pin, bool allume) {
  ecrireIntensite(pin, allume ? 100 : 0);
}

void commandesEtat(JsonVariant doc) {
  for (int i = 0; i < 3; i++) doc[NOMS[i]] = allumes[i] ? 1 : 0;
}

void commandesModifier(JsonVariantConst recu) {
  for (int i = 0; i < 3; i++) {
    JsonVariantConst v = recu[NOMS[i]];
    if (!v.isNull()) ecrire(PINS[i], v.as<int>() != 0);
  }
  Serial.printf("LED %s, vibreur %s, buzzer %s\n", allumes[0] ? "ON" : "OFF",
                allumes[1] ? "ON" : "OFF", allumes[2] ? "ON" : "OFF");
}
