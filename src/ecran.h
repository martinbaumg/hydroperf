#pragma once
#include <Arduino.h>

// Type de boisson à prendre, affiché par la couleur de la goutte
enum TypeBoisson { EAU, ELECTROLYTE, ISOTONIQUE, MAISON };

const int GORGEES_MAX = 3;

void ecranInit();
// Affiche le nombre de gorgées à boire (barres limitées à GORGEES_MAX) et le type de boisson
void ecranAfficher(int nbGorgees, TypeBoisson boisson);
// Convertit une consigne (quantité à boire en mL) en nombre de barres : 1 barre par gorgée, 1 à GORGEES_MAX
int barresPourConsigne(int consigneMl, int mlParGorgee);
// Affiche une consigne : barres (quantité) + couleur de la goutte (type de boisson)
void ecranConsigne(int consigneMl, int mlParGorgee, TypeBoisson boisson);
// Affiche un texte (UTF-8, accents français acceptés), coupé en lignes entre les mots
void ecranMessage(const char* texte);
void ecranEffacer();

// Extinction automatique pour économiser la batterie : l'écran se rallume à chaque affichage
// et s'éteint "delaiMs" plus tard (0 = reste allumé). ecranTick() est à appeler dans loop().
void ecranDelaiExtinction(uint32_t delaiMs);
void ecranTick();
void ecranEteindre();
bool ecranAllume();
