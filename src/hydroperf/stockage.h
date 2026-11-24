#pragma once
#include <ArduinoJson.h>

// Données enregistrées dans la flash (LittleFS) : elles restent après un redémarrage
// et ne sont pas effacées quand on renvoie le programme.
extern JsonDocument profil;    // poids, taille, fc_repos, fc_max, sueur (L/h)
extern JsonDocument reglages;  // alertes, rappels, consentements, préférences, dernière configuration
extern JsonDocument courses;   // séances enregistrées : [{id, nom, sport, seance, duree, distance, bidons}]

void stockageInit();

// Recopie dans profil / reglages les clés connues présentes dans "recu", puis enregistre
void modifierProfil(JsonVariantConst recu);
void modifierReglages(JsonVariantConst recu);

// Séances enregistrées ("config" déjà vérifiée par normaliserConfig, voir sortie.h)
int ajouterCourse(const char* nom, JsonVariantConst config);
bool supprimerCourse(int id);
void enregistrerCourses();

// Historique : une ligne JSON par sortie terminée dans /historique.jsonl
void ajouterHistorique(const JsonDocument& sortie);
void effacerHistorique();
extern const char* FICHIER_HISTORIQUE;

// Retours des utilisateurs en fin de sortie (pour l'équipe) : une ligne JSON par retour dans /retours.jsonl
bool ajouterRetour(const JsonDocument& retour);  // false si la mémoire réservée aux retours est pleine
void effacerRetours();
extern const char* FICHIER_RETOURS;

// Sortie en cours, sauvegardée régulièrement pour survivre à une coupure
bool lireJson(const char* chemin, JsonDocument& doc);
bool ecrireJson(const char* chemin, const JsonDocument& doc);
void supprimerFichier(const char* chemin);
