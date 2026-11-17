#include "sortie.h"
#include <Arduino.h>
#include <initializer_list>
#include <vector>
#include "brochage.h"
#include "commandes.h"
#include "ecran.h"
#include "stockage.h"

// On compense 80 % de l'eau perdue en transpirant pendant l'effort
static const float PART_A_COMPENSER = 0.8;
// Plan d'hydratation : une alerte toutes les 5 à 20 min, avec 1 à 3 gorgées (les barres de l'écran)
static const int INTERVALLE_MIN = 5;
static const int INTERVALLE_MAX = 20;
static const int GORGEES_PAR_ALERTE_MAX = 3;
static const uint32_t SAUVEGARDE_TOUTES_LES_S = 60;
static const char* FICHIER_EN_COURS = "/en_cours.json";
static const size_t MAX_BIDONS = 3;

// ---------- Petits outils ----------

// Renvoie la valeur reçue si elle fait partie de la liste, sinon la première de la liste
static const char* choix(JsonVariantConst v, std::initializer_list<const char*> valeurs) {
  const char* texte = v | "";
  for (const char* c : valeurs) {
    if (strcmp(texte, c) == 0) return c;
  }
  return *valeurs.begin();
}

static int arrondi50(float ml) {
  return round(ml / 50) * 50;
}

static bool heureConnue() {
  return time(nullptr) > 1700000000;
}

// ---------- Données simulées (montre et téléphone) ----------

struct Meteo {
  float temp;      // °C
  float humidite;  // %
  float ressenti;  // °C
};

// Météo simulée : un scénario choisi dans l'appli (onglet Test), avec de petites variations
static Meteo meteo() {
  const char* scenario = reglages["simulation_meteo"] | "auto";
  float baseT = 24, baseH = 55;
  if (strcmp(scenario, "frais") == 0) { baseT = 12; baseH = 70; }
  else if (strcmp(scenario, "doux") == 0) { baseT = 19; baseH = 55; }
  else if (strcmp(scenario, "chaud") == 0) { baseT = 29; baseH = 50; }
  else if (strcmp(scenario, "canicule") == 0) { baseT = 36; baseH = 40; }
  float t = baseT + 1.5 * sin(millis() / 3600000.0 * 2 * PI);
  float h = baseH + 10 * sin(millis() / 5400000.0 * 2 * PI);
  float r = t;
  if (t >= 27) {
    // Indice de chaleur (formule de Rothfusz en °C), valable au-dessus de 27 °C
    r = -8.78469 + 1.61139 * t + 2.33855 * h - 0.146116 * t * h - 0.0123081 * t * t
        - 0.0164248 * h * h + 0.00221173 * t * t * h + 0.00072546 * t * h * h - 0.000003582 * t * t * h * h;
  }
  return {t, h, r};
}

// On transpire plus quand il fait chaud
static float facteurChaleur(float ressenti) {
  return constrain(1 + 0.03 * (ressenti - 24), 0.8f, 1.6f);
}

static float vitesseKmh(const String& sport, const String& seance) {
  float base = sport == "velo" ? 25 : sport == "trail" ? 7 : 10;
  float facteur = seance == "footing" ? 0.9 : seance == "fractionne" ? 1.1 : 1.0;
  return base * facteur;
}

// Effort moyen de la séance, en part de la réserve cardiaque (entre FC repos et FC max)
static float intensiteSeance(const String& seance) {
  if (seance == "footing") return 0.55;
  if (seance == "fractionne") return 0.75;
  return 0.65;
}

// Sodium perdu par litre de sueur, selon le profil
static float sodiumSueurMgL() {
  const char* sel = profil["sel"] | "moyenne";
  if (strcmp(sel, "faible") == 0) return 500;
  if (strcmp(sel, "forte") == 0) return 1400;
  return 900;
}

// ---------- Vibreur et buzzer : impulsions sans bloquer le serveur web ----------

struct Motif {
  int pin;
  int restants = 0;
  int dureeOn = 0;
  int dureeOff = 0;
  int intensite = 100;
  bool allume = false;
  unsigned long prochain = 0;

  void lancer(int nb, int on, int off, int pourcent) {
    restants = nb;
    dureeOn = on;
    dureeOff = off;
    intensite = pourcent;
    prochain = millis();
  }

  void mettreAJour() {
    if ((long)(millis() - prochain) < 0) return;
    if (allume) {
      ecrireIntensite(pin, 0);
      allume = false;
      prochain = millis() + dureeOff;
    } else if (restants > 0) {
      ecrireIntensite(pin, intensite);
      allume = true;
      restants--;
      prochain = millis() + dureeOn;
    }
  }
};

static Motif vibreur{PIN_VIBREUR};
static Motif buzzer{PIN_BUZZER};

// Rythme de l'alerte, commun au vibreur et au buzzer pour qu'ils restent synchronisés :
// 3 impulsions de 0,5 s séparées de 0,3 s
static const int IMPULSIONS = 3, IMPULSION_MS = 500, PAUSE_MS = 300;

// Mode de signalement choisi dans les réglages : vibreur, buzzer ou les deux
static void signaler() {
  if (reglages["alerte_vibreur"] | true) vibreur.lancer(IMPULSIONS, IMPULSION_MS, PAUSE_MS, reglages["intensite_vibreur"] | 80);
  if (reglages["alerte_buzzer"] | false) buzzer.lancer(IMPULSIONS, IMPULSION_MS, PAUSE_MS, reglages["intensite_buzzer"] | 60);
}

// Durée d'affichage de la consigne (écran et LED), en secondes
static int dureeEcranS() {
  return constrain((int)(reglages["duree_ecran"] | 10), 3, 120);
}

// ---------- État de la sortie ----------

struct Bidon {
  int volume;
  String type;  // eau, electrolyte, isotonique, maison
  bool sucre, sel;
  float bu;
};

struct Gorgee {
  uint32_t t;  // secondes depuis le départ
  uint16_t ml;
  uint8_t bidon;
};

struct EtatSortie {
  bool active = false;
  bool pause = false;
  uint32_t debut = 0;  // date de départ (secondes Unix), 0 tant que l'heure est inconnue
  uint32_t duree = 0;  // secondes écoulées hors pauses
  String sport, seance;
  int dureePrevue = 0;  // minutes, 0 = on verra
  float distancePrevue = 0;
  int objectifInitial = 0;
  int gorgeesPrevues = 0;
  std::vector<Bidon> bidons;
  float buMl = 0, perdueMl = 0, km = 0, fcSomme = 0, sodiumPerdu = 0, sodiumApporte = 0;
  int rappel = 0;       // secondes avant la prochaine alerte
  int nbRappels = 0;
  int planIntervalle = 0, planGorgees = 0;  // plan d'hydratation en cours
  float perdueAlerte = 0;    // eau perdue (mL) au moment de la dernière alerte
  uint32_t dureeAlerte = 0;  // secondes écoulées au moment de la dernière alerte
  int alerte = 0;       // secondes d'affichage restantes de la dernière consigne
  int alerteGorgees = 0;
  int alerteBidon = -1; // -1 : tous les bidons sont vides
  float fcLisse = 0, fc = 0, transpi = 0;
  std::vector<Gorgee> gorgees;
};

static EtatSortie s;
static unsigned long dernierTick = 0;
static unsigned long ledTestJusqua = 0;  // LED allumée par le bouton Tester

static float restant(int i) {
  return max(0.0f, s.bidons[i].volume - s.bidons[i].bu);
}

static bool apporteElectrolytes(const Bidon& b) {
  return b.type == "electrolyte" || b.type == "isotonique" || (b.type == "maison" && b.sel);
}

static float sodiumBoissonMgL(const Bidon& b) {
  if (b.type == "electrolyte") return 700;
  if (b.type == "isotonique") return 450;
  if (b.type == "maison" && b.sel) return 600;
  return 0;
}

static TypeBoisson typeEcran(const Bidon& b) {
  if (b.type == "electrolyte") return ELECTROLYTE;
  if (b.type == "isotonique") return ISOTONIQUE;
  if (b.type == "maison") return MAISON;
  return EAU;
}

// ---------- Configuration et prévision ----------

void normaliserConfig(JsonVariantConst recu, JsonObject config) {
  config["sport"] = choix(recu["sport"], {"course", "trail", "velo"});
  config["seance"] = choix(recu["seance"], {"footing", "longue", "fractionne"});
  config["duree"] = constrain((int)(recu["duree"] | 0), 0, 24 * 60);
  config["distance"] = round(constrain((float)(recu["distance"] | 0.0f), 0.0f, 500.0f) * 10) / 10;

  JsonArray bidons = config["bidons"].to<JsonArray>();
  for (JsonVariantConst b : recu["bidons"].as<JsonArrayConst>()) {
    if (bidons.size() >= MAX_BIDONS) break;
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = constrain((int)(b["volume"] | 500), 100, 2000);
    const char* type = choix(b["type"], {"eau", "electrolyte", "isotonique", "maison"});
    o["type"] = type;
    bool maison = strcmp(type, "maison") == 0;
    o["sucre"] = maison && (b["sucre"] | false);
    o["sel"] = maison && (b["sel"] | false);
  }
  if (bidons.size() == 0) {
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = 500;
    o["type"] = "eau";
    o["sucre"] = false;
    o["sel"] = false;
  }
}

// Durée prévue en minutes : celle choisie, sinon estimée avec la distance, sinon 0
static int dureeEstimee(JsonVariantConst config) {
  int duree = config["duree"] | 0;
  if (duree > 0) return duree;
  float km = config["distance"] | 0.0;
  if (km > 0) return round(km / vitesseKmh(config["sport"] | "", config["seance"] | "") * 60);
  return 0;
}

// ---------- Plan d'hydratation (fréquence des alertes) ----------
// Croise la transpiration attendue (profil biométrique + effort de la séance + météo)
// avec la taille d'une gorgée : on vise la plus petite prise possible (1 à 3 gorgées)
// avec une alerte toutes les 5 à 20 minutes.
struct Plan {
  int intervalle;  // minutes entre deux alertes
  int gorgees;     // gorgées par alerte
};

static Plan calculerPlan(float transpiLh) {
  int gorgee = max(5, (int)(reglages["gorgee"] | 30));
  float besoinParHeure = max(50.0f, PART_A_COMPENSER * transpiLh * 1000);  // mL à boire par heure

  // "Je calcule pour toi" désactivé : intervalle choisi dans les réglages
  if (!(reglages["auto"] | true)) {
    int intervalle = max(1, (int)(reglages["intervalle"] | 12));
    return {intervalle, constrain((int)round(besoinParHeure * intervalle / 60 / gorgee), 1, 8)};
  }
  for (int g = 1; g <= GORGEES_PAR_ALERTE_MAX; g++) {
    float intervalle = 60.0 * g * gorgee / besoinParHeure;
    if (intervalle >= INTERVALLE_MIN) return {(int)round(min(intervalle, (float)INTERVALLE_MAX)), g};
  }
  // Très gros besoin : alerte la plus fréquente avec le maximum de gorgées
  return {INTERVALLE_MIN, GORGEES_PAR_ALERTE_MAX};
}

// Transpiration attendue (L/h) pour une séance, avec la météo du moment
static float transpirationPrevue(const char* seance) {
  float sueur = profil["sueur"] | 0.9;
  return sueur * intensiteSeance(seance) / 0.65 * facteurChaleur(meteo().ressenti);
}

void prevoir(JsonVariantConst config, JsonVariant p) {
  float transpi = transpirationPrevue(config["seance"] | "");
  Plan plan = calculerPlan(transpi);
  int intervalle = plan.intervalle;
  int parHeure = arrondi50(PART_A_COMPENSER * transpi * 1000);
  int duree = dureeEstimee(config);

  int capacite = 0;
  for (JsonVariantConst b : config["bidons"].as<JsonArrayConst>()) capacite += b["volume"] | 0;

  p["transpiration"] = round(transpi * 10) / 10;
  p["par_heure"] = parHeure;
  p["duree_estimee"] = duree;
  p["objectif"] = duree ? arrondi50(parHeure * duree / 60.0) : 0;
  p["intervalle"] = intervalle;
  p["gorgees"] = plan.gorgees;
  p["capacite"] = capacite;
  p["perte"] = round(transpi * 1000 * duree / 60);
  p["sodium"] = round(transpi * duree / 60 * sodiumSueurMgL());
}

static int objectifActuel() {
  int selonPerte = arrondi50(PART_A_COMPENSER * s.perdueMl);
  return s.dureePrevue ? max(s.objectifInitial, selonPerte) : selonPerte;
}

// ---------- Simulation de la montre ----------

static void simuler() {
  float fcRepos = profil["fc_repos"] | 52;
  float fcMax = profil["fc_max"] | 189;
  float sueur = profil["sueur"] | 0.9;

  // Fractionné : 3 min d'effort, 3 min de récupération après 10 min d'échauffement
  float cible = intensiteSeance(s.seance);
  if (s.seance == "fractionne" && s.duree > 600) cible = (s.duree / 180) % 2 ? 0.5 : 0.85;
  float echauffement = min(1.0f, s.duree / 600.0f);
  float intensite = 0.15 + (cible - 0.15) * echauffement;

  // Le cœur rejoint progressivement sa cible, avec de petites variations
  float fcCible = fcRepos + intensite * (fcMax - fcRepos);
  if (s.fcLisse == 0) s.fcLisse = fcRepos;
  s.fcLisse += (fcCible - s.fcLisse) * 0.05;
  s.fc = s.fcLisse + 3 * sin(s.duree / 240.0) + random(-20, 21) / 10.0;

  float intensiteReelle = (s.fc - fcRepos) / (fcMax - fcRepos);
  s.transpi = constrain(sueur * intensiteReelle / 0.65 * facteurChaleur(meteo().ressenti), 0.1f, 2.5f);
  s.perdueMl += s.transpi * 1000 / 3600;
  s.sodiumPerdu += s.transpi / 3600 * sodiumSueurMgL();
  s.km += vitesseKmh(s.sport, s.seance) / 3600;
  s.fcSomme += s.fc;
}

// ---------- Alertes ----------

// Transpiration moyenne (L/h) depuis la dernière alerte
static float transpirationRecente() {
  uint32_t secondes = s.duree - s.dureeAlerte;
  if (secondes < 60) return s.transpi;
  return (s.perdueMl - s.perdueAlerte) / 1000 / (secondes / 3600.0);
}

// Eau au début ; après 45 min ou quand il fait chaud, une alerte sur deux
// avec un bidon qui apporte des électrolytes. -1 si tous les bidons sont vides.
static int choisirBidon() {
  bool besoinSel = s.duree >= 45 * 60 || meteo().ressenti >= 27;
  bool tourSel = besoinSel && s.nbRappels % 2 == 1;
  int secours = -1;
  for (size_t i = 0; i < s.bidons.size(); i++) {
    if (restant(i) <= 0) continue;
    if (apporteElectrolytes(s.bidons[i]) == tourSel) return i;
    if (secours < 0) secours = i;
  }
  return secours;
}

// Message bref sur l'écran, qui s'éteint ensuite tout seul (Réglages > Durée d'affichage)
static void afficherEtat() {
  if (!s.active) {
    ecranMessage("HydroPerf prêt !");
  } else if (s.pause) {
    ecranMessage("En pause");
  } else {
    String texte = "Prochaine gorgée dans " + String(max(1, (s.rappel + 59) / 60)) + " min";
    ecranMessage(texte.c_str());
  }
}

static void declencherAlerte() {
  int gorgee = reglages["gorgee"] | 30;
  // Le plan est recalculé à chaque alerte avec la transpiration réelle (effort + météo)
  Plan plan = calculerPlan(transpirationRecente());
  s.planIntervalle = plan.intervalle;
  s.planGorgees = plan.gorgees;
  s.perdueAlerte = s.perdueMl;
  s.dureeAlerte = s.duree;

  int nb = plan.gorgees;
  int b = choisirBidon();
  s.nbRappels++;
  s.alerte = dureeEcranS();
  s.alerteGorgees = nb;
  s.alerteBidon = b;

  if (b >= 0) {
    // On considère que l'athlète boit à chaque consigne
    float ml = min((float)nb * gorgee, restant(b));
    s.bidons[b].bu += ml;
    s.buMl += ml;
    s.sodiumApporte += ml / 1000 * sodiumBoissonMgL(s.bidons[b]);
    if (s.gorgees.size() < 300) s.gorgees.push_back({s.duree, (uint16_t)ml, (uint8_t)b});
    ecranConsigne(nb * gorgee, gorgee, typeEcran(s.bidons[b]));
    Serial.printf("Alerte : %d gorgée(s) dans le bidon %d (%.0f mL), prochaine dans %d min\n",
                  nb, b + 1, ml, plan.intervalle);
  } else {
    ecranMessage("Bidons vides ! Pense à remplir");
    Serial.println("Alerte : tous les bidons sont vides");
  }
  ecrire(PIN_LED, true);
  signaler();
}

// Joue une alerte complète (écran, LED, vibreur et/ou buzzer) avec les réglages actuels
void testerAlerte() {
  int gorgee = reglages["gorgee"] | 30;
  ecranConsigne(2 * gorgee, gorgee, EAU);
  ecrire(PIN_LED, true);
  signaler();
  ledTestJusqua = millis() + dureeEcranS() * 1000UL;
}

// ---------- Sauvegarde ----------

static void versJson(JsonDocument& d, bool complet) {
  d["debut"] = s.debut;
  d["duree"] = s.duree;
  d["sport"] = s.sport;
  d["seance"] = s.seance;
  d["duree_prevue"] = s.dureePrevue;
  d["distance_prevue"] = s.distancePrevue;
  d["km"] = round(s.km * 10) / 10;
  d["bu"] = round(s.buMl);
  d["objectif"] = objectifActuel();
  d["perdue"] = round(s.perdueMl);
  d["sodium_perdu"] = round(s.sodiumPerdu);
  d["sodium_apporte"] = round(s.sodiumApporte);
  d["fc_moy"] = s.duree ? round(s.fcSomme / s.duree) : 0;
  d["rappels"] = s.nbRappels;
  JsonArray bidons = d["bidons"].to<JsonArray>();
  for (const Bidon& b : s.bidons) {
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = b.volume;
    o["type"] = b.type;
    o["sucre"] = b.sucre;
    o["sel"] = b.sel;
    o["bu"] = round(b.bu);
  }
  JsonArray liste = d["gorgees"].to<JsonArray>();
  for (const Gorgee& g : s.gorgees) {
    JsonArray item = liste.add<JsonArray>();
    item.add(g.t);
    item.add(g.ml);
    item.add(g.bidon);
  }
  if (!complet) return;
  // Ce qu'il faut en plus pour reprendre la sortie après une coupure
  d["objectif_initial"] = s.objectifInitial;
  d["gorgees_prevues"] = s.gorgeesPrevues;
  d["plan_intervalle"] = s.planIntervalle;
  d["plan_gorgees"] = s.planGorgees;
  d["perdue_alerte"] = s.perdueAlerte;
  d["duree_alerte"] = s.dureeAlerte;
  d["rappel"] = s.rappel;
  d["fc_somme"] = s.fcSomme;
  d["perdue_exacte"] = s.perdueMl;
  d["sodium_perdu_exact"] = s.sodiumPerdu;
  d["sodium_apporte_exact"] = s.sodiumApporte;
  for (size_t i = 0; i < s.bidons.size(); i++) bidons[i]["bu"] = s.bidons[i].bu;
}

static void sauvegarderEnCours() {
  JsonDocument d;
  versJson(d, true);
  ecrireJson(FICHIER_EN_COURS, d);
}

void sortieInit() {
  commandesInit();

  JsonDocument d;
  if (!lireJson(FICHIER_EN_COURS, d) || !d["bidons"].is<JsonArray>()) {
    supprimerFichier(FICHIER_EN_COURS);  // rien à reprendre (ou ancien format)
    return;
  }

  s.active = true;
  s.pause = true;  // on attend que l'utilisateur relance
  s.debut = d["debut"] | 0;
  s.duree = d["duree"] | 0;
  s.sport = d["sport"] | "course";
  s.seance = d["seance"] | "footing";
  s.dureePrevue = d["duree_prevue"] | 0;
  s.distancePrevue = d["distance_prevue"] | 0.0;
  s.km = d["km"] | 0.0;
  s.buMl = d["bu"] | 0.0;
  s.perdueMl = d["perdue_exacte"] | 0.0;
  s.sodiumPerdu = d["sodium_perdu_exact"] | 0.0;
  s.sodiumApporte = d["sodium_apporte_exact"] | 0.0;
  s.fcSomme = d["fc_somme"] | 0.0;
  s.nbRappels = d["rappels"] | 0;
  s.objectifInitial = d["objectif_initial"] | 0;
  s.gorgeesPrevues = d["gorgees_prevues"] | 3;
  s.rappel = d["rappel"] | 600;
  s.planIntervalle = d["plan_intervalle"] | 12;
  s.planGorgees = d["plan_gorgees"] | 2;
  s.perdueAlerte = d["perdue_alerte"] | 0.0;
  s.dureeAlerte = d["duree_alerte"] | 0;
  for (JsonVariantConst b : d["bidons"].as<JsonArrayConst>()) {
    s.bidons.push_back({b["volume"] | 500, b["type"] | "eau", b["sucre"] | false, b["sel"] | false, b["bu"] | 0.0f});
  }
  for (JsonArrayConst g : d["gorgees"].as<JsonArrayConst>()) {
    s.gorgees.push_back({g[0] | 0u, (uint16_t)(g[1] | 0), (uint8_t)(g[2] | 0)});
  }
  Serial.printf("Sortie reprise après une coupure (en pause), %u s écoulées\n", (unsigned)s.duree);
}

// ---------- Actions ----------

bool sortieDemarrer(JsonVariantConst recu) {
  if (s.active) return false;

  JsonDocument doc;
  JsonObject config = doc.to<JsonObject>();
  normaliserConfig(recu, config);

  // On garde la configuration pour préremplir la prochaine sortie
  JsonDocument maj;
  maj["derniere_config"] = config;
  modifierReglages(maj.as<JsonVariantConst>());

  JsonDocument p;
  prevoir(config, p.to<JsonObject>());

  s = EtatSortie{};
  s.active = true;
  s.debut = heureConnue() ? time(nullptr) : 0;
  s.sport = config["sport"].as<const char*>();
  s.seance = config["seance"].as<const char*>();
  s.dureePrevue = p["duree_estimee"];
  s.distancePrevue = config["distance"];
  s.objectifInitial = p["objectif"];
  s.gorgeesPrevues = p["gorgees"];
  s.planIntervalle = p["intervalle"];
  s.planGorgees = p["gorgees"];
  s.rappel = s.planIntervalle * 60;
  JsonArrayConst bidons = config["bidons"];
  for (JsonVariantConst b : bidons) {
    s.bidons.push_back({b["volume"], b["type"].as<const char*>(), b["sucre"], b["sel"], 0});
  }
  dernierTick = millis();
  sauvegarderEnCours();
  afficherEtat();
  Serial.printf("Départ : %s / %s, objectif %d mL\n", s.sport.c_str(), s.seance.c_str(), s.objectifInitial);
  return true;
}

void sortiePause() {
  if (!s.active) return;
  s.pause = !s.pause;
  dernierTick = millis();
  sauvegarderEnCours();
  afficherEtat();
}

bool sortieTerminer() {
  if (!s.active) return false;
  // Consentement : l'historique n'est enregistré que si l'utilisateur l'accepte
  if (reglages["consent_historique"] | true) {
    JsonDocument bilan;
    versJson(bilan, false);
    ajouterHistorique(bilan);
  }
  supprimerFichier(FICHIER_EN_COURS);
  Serial.printf("Fin : %.0f mL bus sur %d mL\n", s.buMl, objectifActuel());
  s = EtatSortie{};
  ecrire(PIN_LED, false);
  afficherEtat();
  return true;
}

// ---------- Boucle ----------

void sortieTick() {
  vibreur.mettreAJour();
  buzzer.mettreAJour();
  if (ledTestJusqua && (long)(millis() - ledTestJusqua) >= 0) {
    ledTestJusqua = 0;
    if (s.alerte == 0) ecrire(PIN_LED, false);
  }

  if (millis() - dernierTick < 1000) return;
  dernierTick += 1000;
  if (!s.active || s.pause) return;

  // L'heure arrive du téléphone après le départ : on retrouve l'heure de départ
  if (!s.debut && heureConnue()) s.debut = time(nullptr) - s.duree;

  s.duree++;
  simuler();

  // Fin de l'affichage de la consigne : la LED s'éteint (l'écran s'éteint tout seul)
  if (s.alerte > 0 && --s.alerte == 0) ecrire(PIN_LED, false);
  if (--s.rappel <= 0) {
    declencherAlerte();
    s.rappel = s.planIntervalle * 60;
  }
  if (s.duree % SAUVEGARDE_TOUTES_LES_S == 0) sauvegarderEnCours();
}

void sortieEtat(JsonDocument& doc) {
  Meteo m = meteo();
  doc["active"] = s.active;
  doc["pause"] = s.pause;
  doc["meteo"]["temp"] = round(m.temp * 10) / 10;
  doc["meteo"]["humidite"] = round(m.humidite);
  doc["meteo"]["ressenti"] = round(m.ressenti * 10) / 10;
  doc["meteo"]["scenario"] = reglages["simulation_meteo"] | "auto";
  // Batterie simulée : perd 1 % toutes les 6 minutes depuis l'allumage
  doc["batterie"] = max(5, 100 - (int)(millis() / 360000));
  if (!s.active) return;

  doc["sport"] = s.sport;
  doc["seance"] = s.seance;
  doc["duree"] = s.duree;
  doc["duree_prevue"] = s.dureePrevue;
  doc["bu"] = round(s.buMl);
  doc["objectif"] = objectifActuel();
  doc["perdue"] = round(s.perdueMl);
  doc["sodium_perdu"] = round(s.sodiumPerdu);
  doc["sodium_apporte"] = round(s.sodiumApporte);
  doc["fc"] = round(s.fc);
  doc["transpi"] = round(s.transpi * 10) / 10;
  doc["km"] = round(s.km * 10) / 10;
  doc["rappel"] = s.rappel;
  doc["rappels"] = s.nbRappels;
  doc["plan"]["intervalle"] = s.planIntervalle;
  doc["plan"]["gorgees"] = s.planGorgees;
  if (s.alerte > 0) {
    doc["alerte"]["restant"] = s.alerte;
    doc["alerte"]["gorgees"] = s.alerteGorgees;
    doc["alerte"]["bidon"] = s.alerteBidon;
  }
  JsonArray bidons = doc["bidons"].to<JsonArray>();
  for (const Bidon& b : s.bidons) {
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = b.volume;
    o["type"] = b.type;
    o["sucre"] = b.sucre;
    o["sel"] = b.sel;
    o["bu"] = round(b.bu);
  }
}
