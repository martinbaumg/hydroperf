#pragma once
#include <ArduinoJson.h>

// Déroulement d'une sortie avec l'algorithme v2 : capteurs simulés (cardio, puissance, vitesse),
// sueur, quantité à boire, alertes (vibreur et/ou buzzer, LED et écran) et bilan final.
// On part du principe que l'athlète boit à chaque consigne : chaque alerte compte comme bue.

void sortieInit();   // reprend une sortie interrompue par une coupure (en pause)
void sortieTick();   // à appeler à chaque tour de loop()

// Vérifie et complète une configuration de sortie reçue de la page :
// {sport, seance (course/trail : footing, longue, fractionne ; vélo : balade, endurance, tempo, fractionne),
//  duree (min, 0 = on verra), distance (km, 0 = on verra), ressenti (°C ou null), allure (s/km), vitesse (km/h, vélo),
//  denivele (m, trail),
//  detail (fractionné : echauffement_min, repetitions, effort_s ou effort_m, allure_effort, recup_s, retour_calme_min),
//  pesee, poids_avant, bidons: [{volume, type, sucre, sel, sodium (mg/L) et glucides (g/L), -1 = je ne sais pas}]}
void normaliserConfig(JsonVariantConst recu, JsonObject config);

bool sortieDemarrer(JsonVariantConst config);  // false si une sortie est déjà en cours
void sortiePause();     // pause / reprise
bool sortieTerminer();  // enregistre le bilan dans l'historique (si l'utilisateur l'accepte)
// Résumé de la dernière sortie terminée (durée, sport, bu, objectif...) ; false s'il n'y en a pas
bool sortieResume(JsonObject resume);
void testerAlerte();    // joue l'alerte avec les réglages actuels

// État en direct envoyé à la page toutes les secondes
void sortieEtat(JsonDocument& doc);

// Ce qu'il faudra boire pour une configuration de sortie
void prevoir(JsonVariantConst config, JsonVariant prevision);

// S_base (course et vélo), AT_ref, k et nombre de pesées, pour l'affichage du profil
void calibrationEtat(JsonObject o);

// Pesée de fin de sortie (questionnaire) : {"poids_apres", "reste":"vides|quart|moitie|pleins", "hors"}
// Ajoute un point de calibration au profil. false si la dernière sortie n'avait pas de pesée.
bool sortiePesee(JsonVariantConst recu, JsonObject resultat);
