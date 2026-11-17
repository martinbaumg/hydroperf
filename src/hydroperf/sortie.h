#pragma once
#include <ArduinoJson.h>

// Déroulement d'une sortie : chrono, données simulées (cœur, météo, transpiration, électrolytes),
// alertes pour boire (vibreur et/ou buzzer, LED et écran) et bilan final.
// On part du principe que l'athlète boit à chaque consigne : chaque alerte compte comme bue.

void sortieInit();   // reprend une sortie interrompue par une coupure (en pause)
void sortieTick();   // à appeler à chaque tour de loop()

// Vérifie et complète une configuration de sortie reçue de la page :
// {sport, seance, duree (min, 0 = on verra), distance (km, 0 = on verra), bidons: [{volume, type, sucre, sel}]}
void normaliserConfig(JsonVariantConst recu, JsonObject config);

bool sortieDemarrer(JsonVariantConst config);  // false si une sortie est déjà en cours
void sortiePause();     // pause / reprise
bool sortieTerminer();  // enregistre le bilan dans l'historique (si l'utilisateur l'accepte)
void testerAlerte();    // joue l'alerte avec les réglages actuels

// État en direct envoyé à la page toutes les secondes
void sortieEtat(JsonDocument& doc);

// Ce qu'il faudra boire pour une configuration de sortie
void prevoir(JsonVariantConst config, JsonVariant prevision);
