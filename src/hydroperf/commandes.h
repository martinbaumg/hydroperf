#pragma once
#include <ArduinoJson.h>

// Pilotage de la LED, du vibreur et du buzzer, en gardant leur état en mémoire
// (utilisé par les alertes de la sortie et par les boutons de l'onglet Test)

void commandesInit();
void ecrire(int pin, bool allume);
// Vibreur et buzzer : intensité de 0 (éteint) à 100 %, en PWM
void ecrireIntensite(int pin, int pourcent);

// {"led":1,"vibreur":0,"buzzer":0}
void commandesEtat(JsonVariant doc);
// Applique les clés présentes dans "recu" (led, vibreur, buzzer)
void commandesModifier(JsonVariantConst recu);
