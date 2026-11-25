#include "stockage.h"
#include <LittleFS.h>

JsonDocument profil;
JsonDocument reglages;
JsonDocument courses;

const char* FICHIER_HISTORIQUE = "/historique.jsonl";
static const char* FICHIER_PROFIL = "/profil.json";
static const char* FICHIER_REGLAGES = "/reglages.json";
static const char* FICHIER_COURSES = "/courses.json";

// Valeurs de départ
// sexe : "M", "F" ou "NC" (non communiqué)
// sel : réponse à « as-tu des traces de sel sur tes vêtements ? » : "oui", "non" ou "nsp" (je ne sais pas)
// capteur_* : l'équipement de l'utilisateur (dans ce prototype, ses données sont simulées)
// ftp (W) et allure_ref (s/km, sortie longue) : 0 = inconnue
// pesees_actives : « Affiner mes réglages avec des pesées » ; pesees : [{f:"velo"|"course", s:L/h ramenée
// à l'effort de référence, at:ressenti ou null, fiable}] ; p_ref : puissance apprise quand la FTP est inconnue
static const char PROFIL_DEFAUT[] = R"({"poids":68,"taille":178,"fc_repos":52,"fc_max":189,"sexe":"NC",
  "sel":"nsp","capteur_cardio":true,"capteur_puissance":false,"capteur_gps":false,"ftp":0,"allure_ref":0,
  "pesees_actives":false,"pesees":[],"p_ref":0,"p_ref_n":0})";

// Configuration d'une sortie : sport (course, trail, velo), seance (footing, longue, fractionne),
// duree en min et distance en km (0 = "on verra comment je me sens"), ressenti (°C, null = je ne sais pas),
// allure (s/km), 1 à 3 bidons de type eau, electrolyte, isotonique ou maison (sucre / sel pour la
// boisson maison, sodium en mg/L si l'utilisateur le connaît, -1 sinon). Voir normaliserConfig.
#define CONFIG_DEFAUT R"({"sport":"velo","seance":"longue","duree":120,"distance":50,"ressenti":null,"allure":0,
  "bidons":[{"volume":750,"type":"eau","sucre":false,"sel":false,"sodium":-1},
  {"volume":500,"type":"electrolyte","sucre":false,"sel":false,"sodium":-1}]})"

static const char REGLAGES_DEFAUT[] = R"({
  "fixation":"poignet",
  "alerte_vibreur":true,"alerte_buzzer":false,"intensite_vibreur":80,"intensite_buzzer":60,
  "duree_ecran":10,
  "auto":true,"intervalle":12,"gorgee":30,
  "consent_historique":true,"consent_sante":false,"consent_anonyme":false,
  "unites":"metrique","theme":"auto","notif_telephone":true,
  "derniere_config":)" CONFIG_DEFAUT "}";

static const char COURSES_DEFAUT[] = R"([
  {"id":1,"nom":"Vélo du dimanche","sport":"velo","seance":"longue","duree":120,"distance":50,"bidons":[
    {"volume":750,"type":"eau","sucre":false,"sel":false},
    {"volume":500,"type":"electrolyte","sucre":false,"sel":false}]},
  {"id":2,"nom":"Footing du midi","sport":"course","seance":"footing","duree":45,"distance":8,"bidons":[
    {"volume":500,"type":"eau","sucre":false,"sel":false}]}])";

static JsonDocument profilDefaut;
static JsonDocument reglagesDefaut;

bool lireJson(const char* chemin, JsonDocument& doc) {
  File f = LittleFS.open(chemin, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

bool ecrireJson(const char* chemin, const JsonDocument& doc) {
  File f = LittleFS.open(chemin, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

void supprimerFichier(const char* chemin) {
  if (LittleFS.exists(chemin)) LittleFS.remove(chemin);
}

// Même type JSON (nombre, booléen, texte, tableau) que la valeur par défaut
static bool memeType(JsonVariantConst a, JsonVariantConst b) {
  if (a.is<bool>()) return b.is<bool>();
  if (a.is<float>()) return b.is<float>() && !b.is<bool>();
  if (a.is<const char*>()) return b.is<const char*>();
  if (a.is<JsonArrayConst>()) return b.is<JsonArrayConst>();
  if (a.is<JsonObjectConst>()) return b.is<JsonObjectConst>();
  return false;
}

// Recopie les clés de "source" connues dans "defaut" et du bon type
static void fusionner(JsonDocument& doc, const JsonDocument& defaut, JsonVariantConst source) {
  for (JsonPairConst kv : defaut.as<JsonObjectConst>()) {
    JsonVariantConst valeur = source[kv.key()];
    if (!valeur.isNull() && memeType(kv.value(), valeur)) doc[kv.key()] = valeur;
  }
}

// Charge un fichier en complétant les clés manquantes par les valeurs par défaut
static void charger(const char* chemin, JsonDocument& doc, const JsonDocument& defaut) {
  JsonDocument lu;
  bool ok = lireJson(chemin, lu);
  doc.set(defaut);
  if (ok) fusionner(doc, defaut, lu.as<JsonVariantConst>());
}

void stockageInit() {
  // true : formate la partition au tout premier démarrage
  if (!LittleFS.begin(true)) Serial.println("Erreur : impossible de monter LittleFS");

  deserializeJson(profilDefaut, PROFIL_DEFAUT);
  deserializeJson(reglagesDefaut, REGLAGES_DEFAUT);
  charger(FICHIER_PROFIL, profil, profilDefaut);
  charger(FICHIER_REGLAGES, reglages, reglagesDefaut);

  // Séances enregistrées ; les anciennes (sans type de séance ni bidons) sont remplacées
  bool ok = lireJson(FICHIER_COURSES, courses) && courses.is<JsonArray>() && courses.size() > 0;
  for (JsonVariantConst c : courses.as<JsonArrayConst>()) {
    if (!c["seance"].is<const char*>() || !c["bidons"].is<JsonArrayConst>()) ok = false;
  }
  if (!ok) deserializeJson(courses, COURSES_DEFAUT);
}

void modifierProfil(JsonVariantConst recu) {
  // Les pesées et la puissance apprise ne viennent que du boîtier (fin de sortie)
  JsonDocument filtre;
  filtre.set(recu);
  filtre.remove("pesees");
  filtre.remove("p_ref");
  filtre.remove("p_ref_n");
  fusionner(profil, profilDefaut, filtre.as<JsonVariantConst>());
  if (strcmp(profil["sel"] | "", "oui") != 0 && strcmp(profil["sel"] | "", "non") != 0) profil["sel"] = "nsp";
  ecrireJson(FICHIER_PROFIL, profil);
}

void enregistrerProfil() {
  ecrireJson(FICHIER_PROFIL, profil);
}

void effacerPesees() {
  profil["pesees"].to<JsonArray>();
  enregistrerProfil();
}

void modifierReglages(JsonVariantConst recu) {
  fusionner(reglages, reglagesDefaut, recu);
  ecrireJson(FICHIER_REGLAGES, reglages);
}

void enregistrerCourses() {
  ecrireJson(FICHIER_COURSES, courses);
}

int ajouterCourse(const char* nom, JsonVariantConst config) {
  int id = 0;
  for (JsonVariantConst c : courses.as<JsonArrayConst>()) id = max(id, (int)(c["id"] | 0));
  id++;
  JsonObject c = courses.add<JsonObject>();
  c["id"] = id;
  c["nom"] = nom;
  for (JsonPairConst kv : config.as<JsonObjectConst>()) c[kv.key()] = kv.value();
  enregistrerCourses();
  return id;
}

bool supprimerCourse(int id) {
  JsonArray liste = courses.as<JsonArray>();
  if (liste.size() <= 1) return false;  // on garde toujours au moins une course
  for (size_t i = 0; i < liste.size(); i++) {
    if ((liste[i]["id"] | 0) == id) {
      liste.remove(i);
      enregistrerCourses();
      return true;
    }
  }
  return false;
}

void ajouterHistorique(const JsonDocument& sortie) {
  File f = LittleFS.open(FICHIER_HISTORIQUE, "a");
  if (!f) return;
  serializeJson(sortie, f);
  f.print('\n');
  f.close();
}

void effacerHistorique() {
  supprimerFichier(FICHIER_HISTORIQUE);
}

// ---------- Retours de fin de sortie ----------

const char* FICHIER_RETOURS = "/retours.jsonl";
// Environ 1 000 retours : au-delà, on n'en ajoute plus pour ne pas remplir la mémoire
static const size_t TAILLE_MAX_RETOURS = 512 * 1024;

bool ajouterRetour(const JsonDocument& retour) {
  if (LittleFS.exists(FICHIER_RETOURS)) {
    File f = LittleFS.open(FICHIER_RETOURS, "r");
    size_t taille = f ? f.size() : 0;
    if (f) f.close();
    if (taille > TAILLE_MAX_RETOURS) return false;
  }
  File f = LittleFS.open(FICHIER_RETOURS, "a");
  if (!f) return false;
  serializeJson(retour, f);
  f.print('\n');
  f.close();
  return true;
}

void effacerRetours() {
  supprimerFichier(FICHIER_RETOURS);
}
