// HydroPerf : l'ESP32 crée son Wi-Fi et sert l'application web (web/index.html)
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <sys/time.h>
#include <initializer_list>
#include "stockage.h"
#include "sortie.h"
#include "commandes.h"
#include "ecran.h"

// Réseau Wi-Fi créé par l'ESP32 (point d'accès)
const char* AP_SSID = "HydroPerf";
// Réseau ouvert (sans mot de passe) : nullptr. Sinon un mot de passe de 8 caractères minimum
const char* AP_PASSWORD = nullptr;
// Adresse de l'appli : http://hydroperf.local (en plus de http://192.168.4.1)
const char* NOM_RESEAU = "hydroperf";

// web/index.html, intégré au programme par board_build.embed_files (platformio.ini)
extern const uint8_t INDEX_HTML_DEBUT[] asm("_binary_web_index_html_start");
extern const uint8_t INDEX_HTML_FIN[] asm("_binary_web_index_html_end");

// Icônes de l'appli (web/icones), intégrées de la même façon : onglet du navigateur
// et icône sur l'écran d'accueil de l'iPhone ("Ajouter à l'écran d'accueil")
#define FICHIER_INTEGRE(nom)                                                   \
  extern const uint8_t nom##_debut[] asm("_binary_web_icones_" #nom "_start"); \
  extern const uint8_t nom##_fin[] asm("_binary_web_icones_" #nom "_end");
FICHIER_INTEGRE(favicon_ico)
FICHIER_INTEGRE(favicon_svg)
FICHIER_INTEGRE(favicon_32_png)
FICHIER_INTEGRE(apple_touch_icon_png)
FICHIER_INTEGRE(site_webmanifest)
FICHIER_INTEGRE(icon_192_png)
FICHIER_INTEGRE(icon_512_png)

struct Icone {
  const char* chemin;
  const char* type;
  const uint8_t* debut;
  const uint8_t* fin;
};
static const Icone ICONES[] = {
  {"/favicon.ico", "image/x-icon", favicon_ico_debut, favicon_ico_fin},
  {"/favicon.svg", "image/svg+xml", favicon_svg_debut, favicon_svg_fin},
  {"/favicon-32.png", "image/png", favicon_32_png_debut, favicon_32_png_fin},
  {"/apple-touch-icon.png", "image/png", apple_touch_icon_png_debut, apple_touch_icon_png_fin},
  {"/site.webmanifest", "application/manifest+json", site_webmanifest_debut, site_webmanifest_fin},
  {"/icon-192.png", "image/png", icon_192_png_debut, icon_192_png_fin},
  {"/icon-512.png", "image/png", icon_512_png_debut, icon_512_png_fin},
};

WebServer server(80);
// Les téléphones connectés au Wi-Fi HydroPerf demandent l'adresse des noms à l'ESP32 :
// il répond 192.168.4.1 pour hydroperf.local (utile pour Android, qui gère mal le mDNS)
DNSServer dns;

static void envoyerJson(const JsonDocument& doc) {
  String texte;
  serializeJson(doc, texte);
  server.send(200, "application/json", texte);
}

static void envoyerOk() {
  server.send(200, "application/json", "{\"ok\":true}");
}

// Valeur reçue si elle fait partie de la liste, sinon la première de la liste
static const char* choixParmi(JsonVariantConst v, std::initializer_list<const char*> valeurs) {
  const char* texte = v | "";
  for (const char* c : valeurs) {
    if (strcmp(texte, c) == 0) return c;
  }
  return *valeurs.begin();
}

// Texte libre limité à "max" octets, sans couper un caractère accentué en deux
static String texteCourt(JsonVariantConst v, size_t max) {
  String texte = v | "";
  texte.trim();
  if (texte.length() <= max) return texte;
  size_t fin = max;
  while (fin > 0 && ((uint8_t)texte[fin] & 0xC0) == 0x80) fin--;
  return texte.substring(0, fin);
}

// Corps JSON d'une requête POST
static bool lireCorps(JsonDocument& doc) {
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"erreur\":\"JSON invalide\"}");
    return false;
  }
  return true;
}

// Profil, réglages (avec la dernière configuration de sortie) et séances enregistrées
static void envoyerConfig() {
  JsonDocument doc;
  doc["profil"] = profil;
  doc["reglages"] = reglages;
  doc["courses"] = courses;
  calibrationEtat(doc["calibration"].to<JsonObject>());
  envoyerJson(doc);
}

// Historique et retours : le fichier contient un objet JSON par ligne, on renvoie un tableau
static void envoyerLignesJson(const char* chemin) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  File f = LittleFS.open(chemin, "r");
  bool premier = true;
  while (f && f.available()) {
    String ligne = f.readStringUntil('\n');
    ligne.trim();
    if (ligne.isEmpty()) continue;
    if (!premier) server.sendContent(",");
    server.sendContent(ligne);
    premier = false;
  }
  if (f) f.close();
  server.sendContent("]");
  server.sendContent("");  // fin de la réponse
}

// Enregistre une partie de la configuration de la prochaine sortie (clés listées),
// vérifiée par normaliserConfig, puis renvoie la configuration complète enregistrée
static void enregistrerConfig(std::initializer_list<const char*> cles) {
  JsonDocument recu;
  if (!lireCorps(recu)) return;
  JsonDocument fusion;
  fusion.set(reglages["derniere_config"]);
  for (const char* cle : cles) {
    if (!recu[cle].isNull()) fusion[cle] = recu[cle];
  }
  JsonDocument maj;
  normaliserConfig(fusion.as<JsonVariantConst>(), maj["derniere_config"].to<JsonObject>());
  modifierReglages(maj.as<JsonVariantConst>());
  JsonDocument doc;
  doc.set(reglages["derniere_config"]);
  envoyerJson(doc);
}

// L'écran s'éteint tout seul après la durée choisie dans les réglages
static void appliquerDelaiEcran() {
  ecranDelaiExtinction(constrain((int)(reglages["duree_ecran"] | 10), 3, 120) * 1000UL);
}

static void definirRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", (const char*)INDEX_HTML_DEBUT,
                  INDEX_HTML_FIN - INDEX_HTML_DEBUT);
  });

  for (const Icone& icone : ICONES) {
    server.on(icone.chemin, HTTP_GET, [&icone]() {
      // Les icônes ne changent pas : le navigateur peut les garder une semaine
      server.sendHeader("Cache-Control", "max-age=604800");
      server.send_P(200, icone.type, (const char*)icone.debut, icone.fin - icone.debut);
    });
  }

  server.on("/api/etat", HTTP_GET, []() {
    JsonDocument doc;
    sortieEtat(doc);
    commandesEtat(doc["commandes"].to<JsonObject>());
    envoyerJson(doc);
  });
  server.on("/api/config", HTTP_GET, envoyerConfig);
  server.on("/api/historique", HTTP_GET, []() { envoyerLignesJson(FICHIER_HISTORIQUE); });

  // Retours de fin de sortie, pour l'équipe : liste complète (Réglages > Pour l'équipe HydroPerf)
  server.on("/api/retours", HTTP_GET, []() { envoyerLignesJson(FICHIER_RETOURS); });
  server.on("/api/retours/effacer", HTTP_POST, []() { effacerRetours(); envoyerOk(); });

  // Formulaire de fin de sortie :
  // {"a_bu":"oui|en_partie|non", "raisons":["alerte_ratee",...], "raison_autre":"...",
  //  "dosage":"bien|trop_souvent|pas_assez|quantites", "remarques":"...",
  //  "pesee":{"poids_apres":69.2, "reste":"vides|quart|moitie|pleins", "hors":false}}  (si la case était cochée)
  server.on("/api/retour", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    JsonDocument retour;
    if (time(nullptr) > 1700000000) retour["date"] = (uint32_t)time(nullptr);
    retour["a_bu"] = choixParmi(recu["a_bu"], {"", "oui", "en_partie", "non"});  // "" : pas répondu
    JsonArray raisons = retour["raisons"].to<JsonArray>();
    for (JsonVariantConst r : recu["raisons"].as<JsonArrayConst>()) {
      const char* raison = choixParmi(r, {"", "alerte_ratee", "pas_pratique", "pas_soif", "plus_de_boisson",
                                          "trop_d_alertes", "autre"});
      if (*raison && raisons.size() < 6) raisons.add(raison);
    }
    retour["raison_autre"] = texteCourt(recu["raison_autre"], 200);
    retour["dosage"] = choixParmi(recu["dosage"], {"", "bien", "trop_souvent", "pas_assez", "quantites"});
    retour["remarques"] = texteCourt(recu["remarques"], 1000);
    sortieResume(retour["sortie"].to<JsonObject>());
    if (recu["pesee"].is<JsonObjectConst>()) {
      JsonDocument pesee;
      if (sortiePesee(recu["pesee"], pesee.to<JsonObject>())) retour["pesee"] = pesee;
    }
    if (!ajouterRetour(retour)) {
      server.send(507, "application/json", "{\"erreur\":\"memoire des retours pleine\"}");
      return;
    }
    Serial.printf("Retour enregistré : a bu \"%s\", dosage \"%s\"\n",
                  retour["a_bu"].as<const char*>(), retour["dosage"].as<const char*>());
    JsonDocument rep;
    rep["ok"] = true;
    if (!retour["pesee"].isNull()) rep["pesee"] = retour["pesee"];
    envoyerJson(rep);
  });

  server.on("/api/pesees/effacer", HTTP_POST, []() { effacerPesees(); envoyerConfig(); });

  // L'ESP32 n'a pas d'horloge : le téléphone lui donne l'heure (secondes Unix)
  server.on("/api/heure", HTTP_POST, []() {
    long t = server.arg("t").toInt();
    if (t > 1700000000) {
      timeval tv = {t, 0};
      settimeofday(&tv, nullptr);
    }
    envoyerOk();
  });

  server.on("/api/profil", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    modifierProfil(recu.as<JsonVariantConst>());
    envoyerConfig();
  });
  server.on("/api/reglages", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    modifierReglages(recu.as<JsonVariantConst>());
    appliquerDelaiEcran();
    envoyerConfig();
  });

  // Configuration de la séance : {"sport":"velo","seance":"longue","duree":120,"distance":50}
  server.on("/api/seance", HTTP_POST, []() {
    enregistrerConfig({"sport", "seance", "duree", "distance", "ressenti", "allure", "vitesse", "denivele", "detail"});
  });
  // Configuration des bidons : {"bidons":[{"volume":750,"type":"eau","sucre":false,"sel":false}, ...]}
  server.on("/api/bidons", HTTP_POST, []() { enregistrerConfig({"bidons"}); });

  // Ce qu'il faudra boire pour une configuration de sortie (formulaire "Prépare ta sortie")
  server.on("/api/prevision", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    JsonDocument config;
    normaliserConfig(recu.as<JsonVariantConst>(), config.to<JsonObject>());
    JsonDocument doc;
    prevoir(config.as<JsonVariantConst>(), doc.to<JsonObject>());
    envoyerJson(doc);
  });

  // Enregistre une séance : {"nom":"...", sport, seance, duree, distance, bidons}
  server.on("/api/courses", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    JsonDocument config;
    normaliserConfig(recu.as<JsonVariantConst>(), config.to<JsonObject>());
    ajouterCourse(recu["nom"] | "Ma séance", config.as<JsonVariantConst>());
    envoyerConfig();
  });
  server.on("/api/courses/supprimer", HTTP_POST, []() {
    supprimerCourse(server.arg("id").toInt());
    envoyerConfig();
  });

  // Démarre la sortie avec la configuration du formulaire
  server.on("/api/sortie/demarrer", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    sortieDemarrer(recu.as<JsonVariantConst>());
    envoyerOk();
  });
  server.on("/api/sortie/pause", HTTP_POST, []() { sortiePause(); envoyerOk(); });
  server.on("/api/sortie/terminer", HTTP_POST, []() { sortieTerminer(); envoyerOk(); });
  // Démo : simule la fin de la sortie d'un coup et renvoie le résumé (courbes, alertes, totaux)
  server.on("/api/sortie/simuler", HTTP_POST, []() {
    JsonDocument doc;
    if (!sortieSimulerEnEntier(doc.to<JsonObject>())) {
      server.send(409, "application/json", "{\"erreur\":\"aucune sortie en cours\"}");
      return;
    }
    envoyerJson(doc);
  });

  // Boutons de test : {"led":1,"vibreur":0,"buzzer":0} (seules les clés présentes changent)
  server.on("/api/commandes", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    commandesModifier(recu.as<JsonVariantConst>());
    JsonDocument doc;
    commandesEtat(doc.to<JsonObject>());
    envoyerJson(doc);
  });

  // Joue l'alerte avec le mode et les intensités des réglages
  server.on("/api/alerte/tester", HTTP_POST, []() { testerAlerte(); envoyerOk(); });

  // Test de l'écran : {"message":"..."}, {"gorgees":2,"boisson":1}
  // (0 eau, 1 électrolyte, 2 isotonique, 3 maison) ou {"effacer":true}
  server.on("/api/ecran", HTTP_POST, []() {
    JsonDocument recu;
    if (!lireCorps(recu)) return;
    if (recu["effacer"] | false) {
      ecranEffacer();
    } else if (recu["message"].is<const char*>()) {
      ecranMessage(recu["message"]);
    } else if (recu["gorgees"].is<int>()) {
      ecranAfficher(recu["gorgees"], (TypeBoisson)constrain(recu["boisson"] | 0, 0, (int)MAISON));
    }
    envoyerOk();
  });

  server.on("/api/historique/effacer", HTTP_POST, []() { effacerHistorique(); envoyerOk(); });

  server.onNotFound([]() { server.send(404, "text/plain", "Introuvable"); });
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  stockageInit();
  sortieInit();
  ecranInit();
  appliquerDelaiEcran();
  ecranMessage("HydroPerf prêt !");

  // Messages sur le port série quand un téléphone se connecte ou se déconnecte
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.println("Un appareil s'est connecté au Wi-Fi");
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("Un appareil s'est déconnecté du Wi-Fi (raison %d)\n", info.wifi_ap_stadisconnected.reason);
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

  if (WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    String domaine = String(NOM_RESEAU) + ".local";
    dns.start(53, domaine, WiFi.softAPIP());
    // mDNS : l'ESP32 annonce lui-même son nom (iPhone, Mac, PC)
    if (MDNS.begin(NOM_RESEAU)) MDNS.addService("http", "tcp", 80);
    Serial.printf("Wi-Fi \"%s\" créé, appli sur http://%s ou http://%s\n", AP_SSID, domaine.c_str(),
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("Erreur : le Wi-Fi n'a pas démarré (mot de passe de moins de 8 caractères ?)");
  }

  definirRoutes();
  server.begin();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  sortieTick();
  ecranTick();
}
