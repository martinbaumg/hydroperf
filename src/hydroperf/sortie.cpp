#include "sortie.h"
#include <Arduino.h>
#include <initializer_list>
#include <vector>
#include "brochage.h"
#include "commandes.h"
#include "ecran.h"
#include "stockage.h"

// =====================================================================================
// Algorithme HydroPerf v2 (voir docs/algorithme_v2_reponses_et_synthese et ..._tour3)
//
//   Sueur    = min(S_base × R_effort × R_météo, 2,5 L/h)
//   À boire  = min(f × Sueur, plafond)       f selon la durée prévue, entre 0,5 et 0,8
//   Compteur += À boire chaque seconde ; alerte dès 2 gorgées (au moins 5 min après la
//   précédente) ou au bout de 20 min ; N = min(3, ⌊compteur / gorgée⌋) gorgées, au moins 1.
// =====================================================================================

// ---------- Constantes (origine de chaque valeur dans les documents) ----------
static const float I_REF = 0.65;                    // intensité d'une sortie longue (référence)
static const float R_EFFORT_MIN = 0.2;              // on transpire un peu même à faible effort
static const float R_EFFORT_MAX = 1 / I_REF;        // valeur atteinte à FC max
static const float K_DEFAUT = 0.03;                 // +3 % de sueur par degré (bilan de chaleur)
static const float K_MIN = 0.01, K_MAX = 0.05;      // en dehors : erreur de mesure, on garde 0,03
static const float AT_REF_DEFAUT = 24;              // température du S_base par défaut
static const float R_METEO_MIN = 0.7, R_METEO_MAX = 1.8;
static const float ECART_CALIBRATION = 8;           // °C d'écart entre deux pesées pour calculer k
static const float SUEUR_MAX = 2.5;                 // L/h : moyenne + 2 écarts-types (Barnes 2019)
static const float PERTE_VISEE = 0.015;             // finir avec au plus 1,5 % du poids perdu
static const float PART_MIN = 0.5, PART_MAX = 0.8, PART_SANS_DUREE = 0.6;
static const float PLAFOND = 0.8;                   // L/h : haut de la fourchette de l'ACSM
static const float PLAFOND_PESEES = 1.0;            // L/h : si les pesées montrent qu'il transpire beaucoup
static const int INTERVALLE_MIN = 5 * 60;           // secondes minimum entre deux alertes
static const int INTERVALLE_MAX = 20 * 60;          // rappel de sécurité
static const int GORGEES_DECLENCHEMENT = 2;
// GORGEES_MAX (3, les barres de l'écran) vient de ecran.h
static const int ATTENTE_RECUP_MAX = 15 * 60;       // on attend la récup si elle arrive dans 15 min
static const float ALPHA_SODIUM = 0.5;              // part du sodium perdu à compenser après 1 h
static const uint32_t SODIUM_APRES_S = 3600;
static const size_t MAX_PESEES = 20;

static const uint32_t SAUVEGARDE_TOUTES_LES_S = 60;
static const char* FICHIER_EN_COURS = "/en_cours.json";
static const size_t MAX_BIDONS = 3;

// ---------- Petits outils ----------

// Renvoie la valeur reçue si elle fait partie de la liste, sinon la première de la liste
static const char* choix(const char* texte, std::initializer_list<const char*> valeurs) {
  for (const char* c : valeurs) {
    if (strcmp(texte, c) == 0) return c;
  }
  return *valeurs.begin();
}

static const char* choix(JsonVariantConst v, std::initializer_list<const char*> valeurs) {
  return choix(v | "", valeurs);
}

static bool heureConnue() {
  return time(nullptr) > 1700000000;
}

static int gorgeeMl() {
  return max(5, (int)(reglages["gorgee"] | 30));
}

static float arrondi(float x, float pas) {
  return round(x / pas) * pas;
}

// ---------- Profil : équipement et références ----------

// Le vélo a ses propres références (puissance) ; trail et course partagent celles de la course
static const char* famille(const String& sport) {
  return sport == "velo" ? "velo" : "course";
}

static bool aCardio() { return profil["capteur_cardio"] | true; }
static bool aPuissance(const String& sport) { return sport == "velo" && (profil["capteur_puissance"] | false); }
// La vitesse GPS ne sert qu'en course : à vélo elle dépend du vent, en trail du dénivelé
static bool aVitesse(const String& sport) { return sport == "course" && (profil["capteur_gps"] | false); }

// Allure habituelle de sortie longue (s/km), 5'30 par défaut
static float allureRef() {
  int a = profil["allure_ref"] | 0;
  return a > 0 ? a : 330;
}
static float vitesseRef() { return 3600 / allureRef(); }  // km/h

// Puissance de référence : 0,65 × FTP (milieu de la zone endurance de Coggan),
// sinon la puissance moyenne des premières sorties, sinon 160 W
static float puissanceRef() {
  float ftp = profil["ftp"] | 0;
  if (ftp > 0) return I_REF * ftp;
  float appris = profil["p_ref"] | 0.0;
  return appris > 0 ? appris : 160;
}

// Intensité moyenne d'une séance (part de la réserve cardiaque)
//   course et trail : footing, longue, fractionne ; vélo : balade, endurance, tempo, fractionne
static float intensiteSeance(const String& seance) {
  if (seance == "footing" || seance == "balade") return 0.55;
  if (seance == "fractionne" || seance == "tempo") return 0.75;
  return I_REF;  // sortie longue, endurance
}

// Sodium de la sueur (mg/L) selon la question « traces de sel » : moyenne ± 1 écart-type
static float sodiumSueurMgL() {
  const char* sel = profil["sel"] | "nsp";
  if (strcmp(sel, "oui") == 0) return 1100;
  if (strcmp(sel, "non") == 0) return 600;
  return 830;
}

// ---------- Calibration : S_base, AT_ref et k, avec les pesées de l'utilisateur ----------

struct Calibration {
  float sBase;      // L/h à l'effort de référence et à AT_ref
  float atRef;      // °C
  float k;          // par °C
  int nbPesees;     // pesées de cette famille de sport
  bool kPersonnel;  // k calculé avec ses pesées
};

static float sBaseParDefaut(const char* fam) {
  if (strcmp(fam, "velo") == 0) {
    float ftp = profil["ftp"] | 0;
    return ftp > 0 ? 0.7 * ftp / 250 : 0.7;
  }
  return 0.9 * (float)(profil["poids"] | 70) / 70;
}

static Calibration calibration(const char* fam) {
  Calibration c{sBaseParDefaut(fam), AT_REF_DEFAUT, K_DEFAUT, 0, false};
  JsonArrayConst pesees = profil["pesees"].as<JsonArrayConst>();

  // AT_ref : ressenti de la première pesée qui en a un
  for (JsonObjectConst p : pesees) {
    if (!p["at"].isNull()) { c.atRef = p["at"]; break; }
  }

  // k : pente de ln(sueur) en fonction du ressenti, calculée dans chaque famille (vélo / course)
  float num = 0, den = 0;
  bool assezDEcart = false;
  for (const char* f : {"velo", "course"}) {
    float n = 0, sAt = 0, sLn = 0, mini = 100, maxi = -100;
    for (JsonObjectConst p : pesees) {
      if (strcmp(p["f"] | "", f) != 0 || p["at"].isNull() || !(p["fiable"] | true)) continue;
      float at = p["at"], s = p["s"];
      n++; sAt += at; sLn += log(s);
      mini = min(mini, at); maxi = max(maxi, at);
    }
    if (n < 2) continue;
    if (maxi - mini >= ECART_CALIBRATION) assezDEcart = true;
    for (JsonObjectConst p : pesees) {
      if (strcmp(p["f"] | "", f) != 0 || p["at"].isNull() || !(p["fiable"] | true)) continue;
      float dx = (float)p["at"] - sAt / n;
      num += dx * (log((float)p["s"]) - sLn / n);
      den += dx * dx;
    }
  }
  if (assezDEcart && den > 0) {
    float k = num / den;
    if (k >= K_MIN && k <= K_MAX) { c.k = k; c.kPersonnel = true; }
  }

  // S_base : moyenne des pesées de la famille, ramenées à AT_ref (une pesée moins fiable compte moitié)
  float somme = 0, poids = 0;
  for (JsonObjectConst p : pesees) {
    if (strcmp(p["f"] | "", fam) != 0) continue;
    float s = p["s"];
    if (!p["at"].isNull()) s *= exp(-c.k * ((float)p["at"] - c.atRef));
    float w = (p["fiable"] | true) ? 1.0 : 0.5;
    somme += w * s;
    poids += w;
    c.nbPesees++;
  }
  if (poids > 0) c.sBase = somme / poids;
  return c;
}

void calibrationEtat(JsonObject o) {
  for (const char* fam : {"course", "velo"}) {
    Calibration c = calibration(fam);
    JsonObject f = o[fam].to<JsonObject>();
    f["s_base"] = arrondi(c.sBase, 0.01);
    f["pesees"] = c.nbPesees;
    o["at_ref"] = arrondi(c.atRef, 0.1);
    o["k"] = arrondi(c.k, 0.001);
    o["k_personnel"] = c.kPersonnel;
  }
  o["pesees"] = profil["pesees"].size();
  o["sodium_sueur"] = sodiumSueurMgL();
  o["p_ref"] = round(puissanceRef());
}

static float rMeteo(JsonVariantConst ressenti, const Calibration& c) {
  if (ressenti.isNull()) return 1;
  return constrain(exp(c.k * ((float)ressenti - c.atRef)), R_METEO_MIN, R_METEO_MAX);
}

// Part de la sueur à compenser : finir avec au plus 1,5 % du poids perdu
static float partACompenser(float sueurLh, int dureeMin) {
  if (dureeMin <= 0 || sueurLh <= 0) return PART_SANS_DUREE;
  float poids = profil["poids"] | 70;
  float f = 1 - PERTE_VISEE * poids / (sueurLh * dureeMin / 60.0);
  return constrain(f, PART_MIN, PART_MAX);
}

static float plafond(const Calibration& c) {
  return c.nbPesees > 0 ? PLAFOND_PESEES : PLAFOND;
}

// ---------- Boissons ----------

struct Bidon {
  int volume;
  String type;     // eau, electrolyte, isotonique, maison
  bool sucre, sel;
  int sodium;      // mg/L donnés par l'utilisateur, -1 = je ne sais pas
  int glucides;    // g/L donnés par l'utilisateur, -1 = je ne sais pas
  float bu;
};

// Compositions de référence (EFSA, AIS ; maison : recette de l'appli, 1,5 g de sel par litre)
static float sodiumBoissonMgL(const Bidon& b) {
  if (b.sodium >= 0) return b.sodium;
  if (b.type == "electrolyte") return 900;
  if (b.type == "isotonique") return 600;
  if (b.type == "maison" && b.sel) return 600;
  return 0;
}

// Glucides (g/L) : isotonique 60 g/L (EFSA : 80 à 350 kcal/L), maison sucrée : recette de l'appli
static float glucidesBoissonGL(const Bidon& b) {
  if (b.glucides >= 0) return b.glucides;
  if (b.type == "isotonique" || (b.type == "maison" && b.sucre)) return 60;
  return 0;
}

static bool sucre(const Bidon& b) {
  return glucidesBoissonGL(b) > 0;
}

static TypeBoisson typeEcran(const Bidon& b) {
  if (b.type == "electrolyte") return ELECTROLYTE;
  if (b.type == "isotonique") return ISOTONIQUE;
  if (b.type == "maison") return MAISON;
  return EAU;
}

// ---------- Configuration de la sortie ----------

// Détail d'un fractionné : échauffement, répétitions (durée ou distance), récupération, retour au calme
struct Detail {
  int echauffement = 15 * 60;  // s
  int repetitions = 6;
  int effort = 180;            // s
  int recup = 120;             // s
  int calme = 10 * 60;         // s
  bool donne = false;          // détaillé par l'utilisateur
};

void normaliserConfig(JsonVariantConst recu, JsonObject config) {
  config["sport"] = choix(recu["sport"], {"course", "trail", "velo"});
  // Séances selon le sport ; une séance de l'autre liste est convertie (footing <-> balade, longue <-> endurance)
  const char* seance = recu["seance"] | "";
  bool velo = strcmp(config["sport"], "velo") == 0;
  if (velo) {
    if (strcmp(seance, "footing") == 0) seance = "balade";
    else if (strcmp(seance, "longue") == 0) seance = "endurance";
    config["seance"] = choix(seance, {"endurance", "balade", "tempo", "fractionne"});
  } else {
    if (strcmp(seance, "balade") == 0) seance = "footing";
    else if (strcmp(seance, "endurance") == 0 || strcmp(seance, "tempo") == 0) seance = "longue";
    config["seance"] = choix(seance, {"footing", "longue", "fractionne"});
  }
  config["duree"] = constrain((int)(recu["duree"] | 0), 0, 24 * 60);
  config["distance"] = round(constrain((float)(recu["distance"] | 0.0f), 0.0f, 500.0f) * 10) / 10;
  // Ressenti annoncé par la météo (°C) ; null = « je ne sais pas » (pas de correction météo)
  if (recu["ressenti"].is<float>()) config["ressenti"] = constrain((int)round((float)recu["ressenti"]), -20, 50);
  else config["ressenti"] = nullptr;
  // Allure prévue en s/km (course et trail) ou vitesse moyenne prévue en km/h (vélo), 0 = pas donnée
  config["allure"] = velo ? 0 : constrain((int)(recu["allure"] | 0), 0, 1200);
  config["vitesse"] = velo ? round(constrain((float)(recu["vitesse"] | 0.0f), 0.0f, 60.0f) * 10) / 10 : 0;
  // Dénivelé positif en m (trail)
  config["denivele"] = strcmp(config["sport"], "trail") == 0 ? constrain((int)(recu["denivele"] | 0), 0, 10000) : 0;

  JsonVariantConst d = recu["detail"];
  if (strcmp(config["seance"], "fractionne") == 0 && d.is<JsonObjectConst>()
      && ((d["effort_s"] | 0) > 0 || (d["effort_m"] | 0) > 0)) {
    JsonObject o = config["detail"].to<JsonObject>();
    o["echauffement_min"] = constrain((int)(d["echauffement_min"] | 15), 0, 90);
    o["repetitions"] = constrain((int)(d["repetitions"] | 1), 1, 50);
    o["effort_s"] = constrain((int)(d["effort_s"] | 0), 0, 3600);
    o["effort_m"] = constrain((int)(d["effort_m"] | 0), 0, 20000);
    o["allure_effort"] = constrain((int)(d["allure_effort"] | 0), 0, 1200);
    o["recup_s"] = constrain((int)(d["recup_s"] | 0), 0, 1800);
    o["retour_calme_min"] = constrain((int)(d["retour_calme_min"] | 10), 0, 90);
  }

  // Double pesée (case « Je me pèse aujourd'hui ») : poids avant en kg
  bool pesee = (recu["pesee"] | false) && (recu["poids_avant"] | 0.0f) >= 30;
  config["pesee"] = pesee;
  config["poids_avant"] = pesee ? round(constrain((float)recu["poids_avant"], 30.0f, 200.0f) * 10) / 10 : 0;

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
    o["sodium"] = constrain((int)(b["sodium"] | -1), -1, 3000);
    o["glucides"] = constrain((int)(b["glucides"] | -1), -1, 200);
  }
  if (bidons.size() == 0) {
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = 500;
    o["type"] = "eau";
    o["sucre"] = false;
    o["sel"] = false;
    o["sodium"] = -1;
    o["glucides"] = -1;
  }
}

// Vitesse prévue (km/h ; en trail : kilomètres-effort par heure). Celle donnée par l'utilisateur
// (allure ou vitesse), sinon une vitesse habituelle selon le sport et la séance.
static float vitesseKmh(JsonVariantConst config) {
  String sport = config["sport"] | "course", seance = config["seance"] | "";
  int allure = config["allure"] | 0;
  float vitesse = config["vitesse"] | 0.0;
  if (sport == "velo") {
    if (vitesse > 0) return vitesse;
    float facteur = seance == "balade" ? 0.8 : seance == "tempo" ? 1.12 : seance == "fractionne" ? 1.08 : 1.0;
    return 25 * facteur;
  }
  if (allure > 0) return 3600.0 / allure;
  float facteur = seance == "footing" ? 0.9 : seance == "fractionne" ? 1.1 : 1.0;
  return vitesseRef() * facteur * (sport == "trail" ? 0.85 : 1.0);
}

// Distance prévue ; en trail, en kilomètres-effort : 100 m de dénivelé positif comptent comme 1 km
static float kmEffort(JsonVariantConst config) {
  return (float)(config["distance"] | 0.0) + (float)(config["denivele"] | 0) / 100;
}

static Detail lireDetail(JsonVariantConst config) {
  Detail d;
  JsonVariantConst o = config["detail"];
  if (!o.is<JsonObjectConst>()) return d;
  d.donne = true;
  d.echauffement = (o["echauffement_min"] | 15) * 60;
  d.repetitions = o["repetitions"] | 1;
  d.recup = o["recup_s"] | 0;
  d.calme = (o["retour_calme_min"] | 10) * 60;
  int s = o["effort_s"] | 0;
  if (s <= 0) {
    // Effort donné en distance : durée avec l'allure de l'effort, sinon un peu plus vite que l'allure habituelle
    int allure = o["allure_effort"] | 0;
    float kmh = allure > 0 ? 3600.0 / allure : vitesseRef() * 0.85 / I_REF;
    s = round((float)(o["effort_m"] | 0) / 1000 / kmh * 3600);
  }
  d.effort = max(10, s);
  return d;
}

static int dureeDetail(const Detail& d) {
  return d.echauffement + d.repetitions * d.effort + max(0, d.repetitions - 1) * d.recup + d.calme;
}

// Durée prévue en minutes : celle du détail du fractionné, sinon celle choisie,
// sinon estimée avec la distance, sinon 0 (« on verra »)
static int dureeEstimee(JsonVariantConst config) {
  if (config["detail"].is<JsonObjectConst>()) return round(dureeDetail(lireDetail(config)) / 60.0);
  int duree = config["duree"] | 0;
  if (duree > 0) return duree;
  if ((float)(config["distance"] | 0.0) > 0) return round(kmEffort(config) / vitesseKmh(config) * 60);
  return 0;
}

// R_effort sans mesure : allure déclarée (course), sinon type de séance
static float rEffortDeclare(const String& sport, const String& seance, int allure) {
  if (sport == "course" && allure > 0) return constrain(allureRef() / allure, R_EFFORT_MIN, R_EFFORT_MAX);
  return intensiteSeance(seance) / I_REF;
}

// ---------- Plan d'alertes ----------

struct Plan {
  int intervalle;  // minutes entre deux alertes
  int gorgees;     // gorgées par alerte
};

// Rythme moyen des alertes pour un débit à boire (mL/min)
static Plan planPour(float mlParMin) {
  int g = gorgeeMl();
  // « Je calcule pour toi » désactivé : intervalle choisi dans les réglages
  if (!(reglages["auto"] | true)) {
    int intervalle = max(1, (int)(reglages["intervalle"] | 12));
    return {intervalle, constrain((int)round(mlParMin * intervalle / g), 1, 8)};
  }
  if (mlParMin <= 0) return {INTERVALLE_MAX / 60, 1};
  float t2 = GORGEES_DECLENCHEMENT * g / mlParMin;  // minutes pour atteindre 2 gorgées
  if (t2 > INTERVALLE_MAX / 60) return {INTERVALLE_MAX / 60, max(1, min(GORGEES_MAX, (int)(INTERVALLE_MAX / 60 * mlParMin / g)))};
  if (t2 >= INTERVALLE_MIN / 60) return {(int)round(t2), GORGEES_DECLENCHEMENT};
  return {INTERVALLE_MIN / 60, min(GORGEES_MAX, (int)(INTERVALLE_MIN / 60 * mlParMin / g))};
}

// ---------- Prévision (formulaire « Prépare ta sortie ») ----------

void prevoir(JsonVariantConst config, JsonVariant p) {
  String sport = config["sport"] | "course", seance = config["seance"] | "footing";
  Calibration c = calibration(famille(sport));
  float rE = rEffortDeclare(sport, seance, config["allure"] | 0);
  float rM = rMeteo(config["ressenti"], c);
  float sueur = min(c.sBase * rE * rM, SUEUR_MAX);
  int duree = dureeEstimee(config);
  float f = partACompenser(sueur, duree);
  float aBoire = min(f * sueur, plafond(c));  // L/h
  Plan plan = planPour(aBoire * 1000 / 60);

  int capacite = 0;
  for (JsonVariantConst b : config["bidons"].as<JsonArrayConst>()) capacite += b["volume"] | 0;

  p["transpiration"] = arrondi(sueur, 0.01);
  p["par_heure"] = arrondi(aBoire * 1000, 10);
  p["duree_estimee"] = duree;
  p["objectif"] = duree ? arrondi(aBoire * 1000 * duree / 60, 50) : 0;
  p["intervalle"] = plan.intervalle;
  p["gorgees"] = plan.gorgees;
  p["capacite"] = capacite;
  p["perte"] = round(sueur * 1000 * duree / 60);
  p["sodium"] = round(sueur * duree / 60 * sodiumSueurMgL());
  JsonObject calcul = p["calcul"].to<JsonObject>();
  calcul["s_base"] = arrondi(c.sBase, 0.01);
  calcul["r_effort"] = arrondi(rE, 0.01);
  calcul["r_meteo"] = arrondi(rM, 0.01);
  calcul["sueur"] = arrondi(sueur, 0.01);
  calcul["part"] = arrondi(f, 0.01);
  calcul["a_boire"] = arrondi(aBoire, 0.01);
  calcul["plafond"] = plafond(c);
  calcul["k"] = arrondi(c.k, 0.001);
  calcul["at_ref"] = c.atRef;
  calcul["effort"] = sport == "course" && (config["allure"] | 0) > 0 ? "allure" : "seance";
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

struct Gorgee {
  uint32_t t;  // secondes depuis le départ
  uint16_t ml;
  uint8_t bidon;
};

// Un point de courbe par minute (résumé de fin de sortie) : valeurs compactes pour tenir en mémoire
struct Point {
  uint32_t t;        // s
  uint8_t fc;        // bpm, 0 = pas de montre
  uint16_t v10;      // vitesse × 10 (km/h)
  uint16_t p;        // W, 0 = pas de capteur
  uint16_t sueur;    // mL/h
  uint16_t perdue;   // mL cumulés
  uint16_t aBoire;   // mL cumulés
  uint16_t bu;       // mL cumulés
  uint8_t phase;
};
static const size_t MAX_POINTS = 720;  // au-delà, on garde un point sur deux

// Phases d'un fractionné, détectées avec la puissance (vélo) ou la vitesse (course)
enum Phase : uint8_t { AUCUNE, ECHAUFFEMENT, EFFORT, RECUP, CALME };
static const char* NOMS_PHASES[] = {"", "echauffement", "effort", "recup", "calme"};

struct EtatSortie {
  bool active = false;
  bool pause = false;
  uint32_t debut = 0;  // date de départ (secondes Unix), 0 tant que l'heure est inconnue
  uint32_t duree = 0;  // secondes écoulées hors pauses
  String sport, seance;
  int dureePrevue = 0;  // minutes, 0 = on verra
  float distancePrevue = 0;
  int allure = 0;       // s/km prévue
  float vitessePrevue = 0;  // km/h prévue (vélo)
  bool ressentiConnu = false;
  float ressenti = 0;   // °C annoncés
  Detail detail;
  bool pesee = false;
  float poidsAvant = 0;
  float part = PART_SANS_DUREE;  // part de la sueur à compenser, fixée au départ
  int objectifInitial = 0;
  std::vector<Bidon> bidons;

  // Cumuls
  float buMl = 0, perdueMl = 0, aBoireMl = 0, km = 0, sodiumPerdu = 0, sodiumApporte = 0, glucidesApportes = 0;
  float fcSomme = 0, pSomme = 0, rEffortSomme = 0;
  uint32_t dureeCardio = 0, dureePuissance = 0;

  // Alertes
  float compteur = 0;          // mL « en retard »
  uint32_t derniereAlerte = 0; // secondes au moment de la dernière alerte
  int nbRappels = 0;
  bool attente = false;        // alerte retenue jusqu'à la récupération
  uint32_t debutAttente = 0;
  int alerte = 0;              // secondes d'affichage restantes de la dernière consigne
  int alerteGorgees = 0;
  int alerteBidon = -1;        // -1 : tous les bidons sont vides

  // Valeurs du moment
  float fc = 0, puissance = 0, vitesse = 0;  // 0 = pas mesuré
  float rEffort = 1, rMeteo = 1, sueur = 0, aBoire = 0;  // L/h
  const char* sourceEffort = "seance";

  // Simulation (le prototype n'a pas de vrais capteurs)
  int decalageEchauffement = 0;  // l'athlète ne suit pas son plan à la seconde
  float fcLisse = 0, pLisse = 0, vLisse = 0;
  float signal = 0;  // puissance ou vitesse moyennée sur environ 5 s, pour détecter les fractions

  // Détection des phases du fractionné
  Phase phase = AUCUNE;
  int repetition = 0;
  uint32_t debutPhase = 0;
  float moyPhase = 0;
  int nbPhase = 0;
  int conditionDepuis = 0;

  std::vector<Gorgee> gorgees;
  std::vector<Point> points;
  uint32_t pasPoints = 60;  // secondes entre deux points
};

static EtatSortie s;
static bool simulationRapide = false;  // « Simuler la course en entier » : ni écran, ni LED, ni vibreur
static unsigned long dernierTick = 0;
static unsigned long ledTestJusqua = 0;  // LED allumée par le bouton Tester

static float restant(int i) {
  return max(0.0f, s.bidons[i].volume - s.bidons[i].bu);
}

static int objectifActuel() {
  int selonEffort = arrondi(s.aBoireMl, 10);
  return s.dureePrevue ? max(s.objectifInitial, selonEffort) : selonEffort;
}

// ---------- Simulation des capteurs ----------
// L'athlète simulé suit sa séance avec une intensité « vraie » ; les capteurs qu'il a déclarés
// (profil > Mon équipement) la mesurent, chacun à sa façon.

// Intensité vraie (part de la réserve cardiaque) à l'instant t
static float intensiteVraie(uint32_t t) {
  if (s.seance == "fractionne") {
    const Detail& d = s.detail;
    int t0 = max(60, d.echauffement + s.decalageEchauffement);
    if ((int)t < t0) return 0.3 + 0.2 * min(1.0f, t / 300.0f);  // échauffement
    int dans = t - t0;
    int cycle = d.effort + d.recup;
    int rep = dans / cycle;
    if (rep < d.repetitions) {
      int pos = dans % cycle;
      if (pos < d.effort || rep == d.repetitions - 1) return pos < d.effort ? 0.85 : 0.45;
      return 0.40;
    }
    return 0.45;  // retour au calme
  }
  float cible = intensiteSeance(s.seance);
  if (s.sport == "course" && s.allure > 0) cible = constrain(I_REF * allureRef() / s.allure, 0.3f, 0.95f);
  float echauffement = min(1.0f, t / 600.0f);
  float relief = 0.04 * sin(t / 300.0);
  return 0.3 + (cible - 0.3) * echauffement + relief;
}

static void simulerCapteurs() {
  float iv = intensiteVraie(s.duree);
  float fcRepos = profil["fc_repos"] | 52, fcMax = profil["fc_max"] | 189;

  // Cardio : rejoint sa cible avec une constante de temps d'environ 1 min,
  // et dérive lentement vers le haut (plus vite quand il fait chaud)
  if (aCardio()) {
    float derive = (3 + 0.3 * max(0.0f, (s.ressentiConnu ? s.ressenti : 20) - 20)) * s.duree / 3600.0;
    float cible = fcRepos + iv * (fcMax - fcRepos) + derive;
    if (s.fcLisse == 0) s.fcLisse = fcRepos + 20;
    s.fcLisse += (cible - s.fcLisse) / 60.0;
    s.fc = min(fcMax, s.fcLisse + random(-15, 16) / 10.0f);
  } else {
    s.fc = 0;
  }

  // Puissance (vélo) et vitesse (course) : instantanées, moyennées sur environ 30 s
  float bruit = 1 + random(-50, 51) / 1000.0f;
  float brut = 0;
  if (aPuissance(s.sport)) {
    float p = puissanceRef() * iv / I_REF * bruit;
    s.pLisse = s.pLisse == 0 ? p : s.pLisse + (p - s.pLisse) / 30.0;
    s.puissance = s.pLisse;
    brut = p;
  } else {
    s.puissance = 0;
  }
  float v = vitesseRef() * iv / I_REF * bruit;
  if (s.sport == "velo") v = (s.vitessePrevue > 0 ? s.vitessePrevue : 25) * (0.7 + 0.3 * iv / I_REF);
  else if (s.sport == "trail") v = 7 * iv / I_REF;
  s.vLisse = s.vLisse == 0 ? v : s.vLisse + (v - s.vLisse) / 30.0;
  s.vitesse = aVitesse(s.sport) ? s.vLisse : 0;
  s.km += s.vLisse / 3600;
  // Signal rapide pour détecter les fractions : la moyenne sur 30 s réagirait trop lentement
  if (brut == 0 && aVitesse(s.sport)) brut = v;
  s.signal = brut == 0 ? 0 : s.signal == 0 ? brut : s.signal + (brut - s.signal) / 5.0;
}

// ---------- R_effort avec ce qui est mesuré ----------

static void calculerEffort() {
  float fcRepos = profil["fc_repos"] | 52, fcMax = profil["fc_max"] | 189;
  bool fc = s.fc > 0, p = s.puissance > 0, v = s.vitesse > 0;
  float rFc = fc ? ((s.fc - fcRepos) / (fcMax - fcRepos)) / I_REF : 0;
  float rP = p ? s.puissance / puissanceRef() : 0;
  float rV = v ? s.vitesse / vitesseRef() : 0;
  float r;
  if (p && fc) { r = 0.6 * rP + 0.4 * rFc; s.sourceEffort = "puissance+cardio"; }
  else if (p) { r = rP; s.sourceEffort = "puissance"; }
  else if (v && fc) { r = 0.5 * rFc + 0.5 * rV; s.sourceEffort = "cardio+vitesse"; }
  else if (v) { r = rV; s.sourceEffort = "vitesse"; }
  else if (fc) { r = rFc; s.sourceEffort = "cardio"; }
  else {
    r = rEffortDeclare(s.sport, s.seance, s.allure);
    s.sourceEffort = s.sport == "course" && s.allure > 0 ? "allure" : "seance";
  }
  s.rEffort = constrain(r, R_EFFORT_MIN, R_EFFORT_MAX);
  if (fc) { s.fcSomme += s.fc; s.dureeCardio++; }
  if (p) { s.pSomme += s.puissance; s.dureePuissance++; }
  s.rEffortSomme += s.rEffort;
}

// ---------- Phases du fractionné (seulement avec puissance ou vitesse mesurée) ----------

static void changerPhase(Phase p) {
  s.phase = p;
  s.debutPhase = s.duree;
  s.moyPhase = 0;
  s.nbPhase = 0;
  s.conditionDepuis = 0;
}

static void suivrePhases() {
  float signal = s.signal;
  if (s.seance != "fractionne" || signal <= 0) {
    s.phase = AUCUNE;
    return;
  }
  if (s.phase == AUCUNE) changerPhase(ECHAUFFEMENT);
  uint32_t dansPhase = s.duree - s.debutPhase;
  bool condition = false;
  // Début d'une fraction : +30 % au-dessus de la moyenne de la phase (l'échauffement monte progressivement)
  if (s.phase == ECHAUFFEMENT || s.phase == RECUP)
    condition = dansPhase >= (s.phase == ECHAUFFEMENT ? 120u : 10u) && signal > 1.3 * s.moyPhase;
  // Récupération : sous 65 % de la moyenne de la fraction
  else if (s.phase == EFFORT) condition = dansPhase >= 10 && signal < 0.65 * s.moyPhase;
  s.conditionDepuis = condition ? s.conditionDepuis + 1 : 0;

  // Changement de phase confirmé pendant 5 s
  if (s.conditionDepuis >= 5) {
    if (s.phase == EFFORT) {
      bool derniere = s.detail.donne && s.repetition >= s.detail.repetitions;
      changerPhase(derniere ? CALME : RECUP);
    } else {
      s.repetition++;
      changerPhase(EFFORT);
    }
    return;
  }
  // Moyenne du signal dans la phase
  s.nbPhase++;
  s.moyPhase += (signal - s.moyPhase) / s.nbPhase;
}

// En plein effort, et la récupération arrive dans moins de 15 min : on attend pour l'alerte
static bool attendreRecup() {
  if (s.phase != EFFORT) return false;
  int resteEffort = s.detail.effort - (int)(s.duree - s.debutPhase);
  return resteEffort <= ATTENTE_RECUP_MAX;
}

// ---------- Alertes ----------

// Bidon qui rapproche le plus « sodium apporté / sodium perdu » de α (0 la première heure, puis 0,5).
// À égalité après une heure, on préfère un bidon sucré (énergie). -1 si tous les bidons sont vides.
static int choisirBidon(float ml) {
  float alpha = s.duree >= SODIUM_APRES_S ? ALPHA_SODIUM : 0;
  int choisi = -1;
  float meilleur = 1e9;
  for (size_t i = 0; i < s.bidons.size(); i++) {
    if (restant(i) <= 0) continue;
    float apporte = s.sodiumApporte + min(ml, restant(i)) / 1000 * sodiumBoissonMgL(s.bidons[i]);
    float ecart = s.sodiumPerdu > 0 ? fabs(apporte / s.sodiumPerdu - alpha) : (apporte > 0 ? 1 : 0);
    bool mieux = choisi < 0 || ecart < meilleur - 0.02;
    bool egalite = choisi >= 0 && fabs(ecart - meilleur) <= 0.02;
    if (mieux || (egalite && alpha > 0 && sucre(s.bidons[i]) && !sucre(s.bidons[choisi]))) {
      choisi = i;
      meilleur = min(meilleur, ecart);
    }
  }
  return choisi;
}

// Message bref sur l'écran, qui s'éteint ensuite tout seul (Réglages > Durée d'affichage)
static int secondesAvantAlerte();
static void afficherEtat() {
  if (!s.active) {
    ecranMessage("HydroPerf prêt !");
  } else if (s.pause) {
    ecranMessage("En pause");
  } else {
    String texte = "Prochaine gorgée dans " + String(max(1, (secondesAvantAlerte() + 59) / 60)) + " min";
    ecranMessage(texte.c_str());
  }
}

static void declencherAlerte() {
  int g = gorgeeMl();
  int nb;
  if (reglages["auto"] | true) nb = constrain((int)(s.compteur / g), 1, GORGEES_MAX);
  else nb = constrain((int)round(s.compteur / g), 1, 8);
  s.compteur = max(0.0f, s.compteur - nb * g);
  s.derniereAlerte = s.duree;
  s.attente = false;

  int b = choisirBidon(nb * g);
  s.nbRappels++;
  s.alerte = dureeEcranS();
  s.alerteGorgees = nb;
  s.alerteBidon = b;

  if (b >= 0) {
    // On considère que l'athlète boit à chaque consigne
    float ml = min((float)nb * g, restant(b));
    s.bidons[b].bu += ml;
    s.buMl += ml;
    s.sodiumApporte += ml / 1000 * sodiumBoissonMgL(s.bidons[b]);
    s.glucidesApportes += ml / 1000 * glucidesBoissonGL(s.bidons[b]);
    if (s.gorgees.size() < 300) s.gorgees.push_back({s.duree, (uint16_t)ml, (uint8_t)b});
    if (simulationRapide) return;
    ecranConsigne(nb * g, g, typeEcran(s.bidons[b]));
    Serial.printf("Alerte : %d gorgée(s) dans le bidon %d (%.0f mL)\n", nb, b + 1, ml);
  } else {
    if (simulationRapide) return;
    ecranMessage("Bidons vides ! Pense à remplir");
    Serial.println("Alerte : tous les bidons sont vides");
  }
  ecrire(PIN_LED, true);
  signaler();
}

// L'alerte est-elle due ? (sans tenir compte de l'attente de la récupération)
static bool alerteDue() {
  uint32_t depuis = s.duree - s.derniereAlerte;
  if (!(reglages["auto"] | true)) return depuis >= (uint32_t)max(1, (int)(reglages["intervalle"] | 12)) * 60;
  return (depuis >= (uint32_t)INTERVALLE_MIN && s.compteur >= GORGEES_DECLENCHEMENT * gorgeeMl())
         || depuis >= (uint32_t)INTERVALLE_MAX;
}

// Estimation du temps avant la prochaine alerte, au débit actuel
static int secondesAvantAlerte() {
  int depuis = s.duree - s.derniereAlerte;
  if (!(reglages["auto"] | true)) return max(0, (int)(reglages["intervalle"] | 12) * 60 - depuis);
  float mlParS = s.aBoire * 1000 / 3600;
  float manque = max(0.0f, GORGEES_DECLENCHEMENT * gorgeeMl() - s.compteur);
  int t = mlParS > 0 ? (int)ceil(manque / mlParS) : INTERVALLE_MAX;
  t = max(t, INTERVALLE_MIN - depuis);
  t = min(t, INTERVALLE_MAX - depuis);
  return max(0, t);
}

// Joue une alerte complète (écran, LED, vibreur et/ou buzzer) avec les réglages actuels
void testerAlerte() {
  int g = gorgeeMl();
  ecranConsigne(2 * g, g, EAU);
  ecrire(PIN_LED, true);
  signaler();
  ledTestJusqua = millis() + dureeEcranS() * 1000UL;
}

// ---------- Sauvegarde ----------

static void ecrireBidons(JsonArray bidons) {
  for (const Bidon& b : s.bidons) {
    JsonObject o = bidons.add<JsonObject>();
    o["volume"] = b.volume;
    o["type"] = b.type;
    o["sucre"] = b.sucre;
    o["sel"] = b.sel;
    o["sodium"] = b.sodium;
    o["glucides"] = b.glucides;
    o["bu"] = round(b.bu);
  }
}

static float rEffortMoyen() {
  return s.duree ? s.rEffortSomme / s.duree : 1;
}

static int fcMoyenne() {
  return s.dureeCardio ? round(s.fcSomme / s.dureeCardio) : 0;
}

static int puissanceMoyenne() {
  return s.dureePuissance ? round(s.pSomme / s.dureePuissance) : 0;
}

static void versJson(JsonDocument& d, bool complet) {
  d["debut"] = s.debut;
  d["duree"] = s.duree;
  d["sport"] = s.sport;
  d["seance"] = s.seance;
  d["duree_prevue"] = s.dureePrevue;
  d["distance_prevue"] = s.distancePrevue;
  if (s.ressentiConnu) d["ressenti"] = s.ressenti;
  else d["ressenti"] = nullptr;
  d["km"] = round(s.km * 10) / 10;
  d["bu"] = round(s.buMl);
  d["objectif"] = objectifActuel();
  d["a_boire"] = round(s.aBoireMl);
  d["perdue"] = round(s.perdueMl);
  d["sodium_perdu"] = round(s.sodiumPerdu);
  d["sodium_apporte"] = round(s.sodiumApporte);
  d["glucides"] = round(s.glucidesApportes);
  d["fc_moy"] = fcMoyenne();
  d["p_moy"] = puissanceMoyenne();
  d["r_effort_moy"] = arrondi(rEffortMoyen(), 0.01);
  d["part"] = arrondi(s.part, 0.01);
  d["rappels"] = s.nbRappels;
  ecrireBidons(d["bidons"].to<JsonArray>());
  JsonArray liste = d["gorgees"].to<JsonArray>();
  for (const Gorgee& g : s.gorgees) {
    JsonArray item = liste.add<JsonArray>();
    item.add(g.t);
    item.add(g.ml);
    item.add(g.bidon);
  }
  if (!complet) return;
  // Ce qu'il faut en plus pour reprendre la sortie après une coupure
  d["allure"] = s.allure;
  d["vitesse_prevue"] = s.vitessePrevue;
  JsonObject det = d["detail"].to<JsonObject>();
  det["donne"] = s.detail.donne;
  det["echauffement"] = s.detail.echauffement;
  det["repetitions"] = s.detail.repetitions;
  det["effort"] = s.detail.effort;
  det["recup"] = s.detail.recup;
  det["calme"] = s.detail.calme;
  d["pesee"] = s.pesee;
  d["poids_avant"] = s.poidsAvant;
  d["objectif_initial"] = s.objectifInitial;
  d["compteur"] = s.compteur;
  d["derniere_alerte"] = s.derniereAlerte;
  d["fc_somme"] = s.fcSomme;
  d["p_somme"] = s.pSomme;
  d["r_effort_somme"] = s.rEffortSomme;
  d["duree_cardio"] = s.dureeCardio;
  d["duree_puissance"] = s.dureePuissance;
  d["decalage"] = s.decalageEchauffement;
  d["perdue_exacte"] = s.perdueMl;
  d["a_boire_exact"] = s.aBoireMl;
  d["sodium_perdu_exact"] = s.sodiumPerdu;
  d["sodium_apporte_exact"] = s.sodiumApporte;
  d["glucides_exact"] = s.glucidesApportes;
  JsonArray bidons = d["bidons"];
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
  if (!lireJson(FICHIER_EN_COURS, d) || !d["bidons"].is<JsonArray>() || d["compteur"].isNull()) {
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
  s.allure = d["allure"] | 0;
  s.vitessePrevue = d["vitesse_prevue"] | 0.0;
  s.ressentiConnu = !d["ressenti"].isNull();
  s.ressenti = d["ressenti"] | 0.0;
  JsonObjectConst det = d["detail"];
  s.detail.donne = det["donne"] | false;
  s.detail.echauffement = det["echauffement"] | 900;
  s.detail.repetitions = det["repetitions"] | 6;
  s.detail.effort = det["effort"] | 180;
  s.detail.recup = det["recup"] | 120;
  s.detail.calme = det["calme"] | 600;
  s.pesee = d["pesee"] | false;
  s.poidsAvant = d["poids_avant"] | 0.0;
  s.part = d["part"] | PART_SANS_DUREE;
  s.objectifInitial = d["objectif_initial"] | 0;
  s.km = d["km"] | 0.0;
  s.buMl = d["bu"] | 0.0;
  s.perdueMl = d["perdue_exacte"] | 0.0;
  s.aBoireMl = d["a_boire_exact"] | 0.0;
  s.sodiumPerdu = d["sodium_perdu_exact"] | 0.0;
  s.sodiumApporte = d["sodium_apporte_exact"] | 0.0;
  s.glucidesApportes = d["glucides_exact"] | 0.0;
  s.compteur = d["compteur"] | 0.0;
  s.derniereAlerte = d["derniere_alerte"] | 0;
  s.fcSomme = d["fc_somme"] | 0.0;
  s.pSomme = d["p_somme"] | 0.0;
  s.rEffortSomme = d["r_effort_somme"] | 0.0;
  s.dureeCardio = d["duree_cardio"] | 0;
  s.dureePuissance = d["duree_puissance"] | 0;
  s.decalageEchauffement = d["decalage"] | 0;
  s.nbRappels = d["rappels"] | 0;
  for (JsonVariantConst b : d["bidons"].as<JsonArrayConst>()) {
    s.bidons.push_back({b["volume"] | 500, b["type"] | "eau", b["sucre"] | false, b["sel"] | false,
                        b["sodium"] | -1, b["glucides"] | -1, b["bu"] | 0.0f});
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

  // On garde la configuration pour préremplir la prochaine sortie (la pesée reste décochée)
  JsonDocument maj;
  maj["derniere_config"] = config;
  maj["derniere_config"]["pesee"] = false;
  maj["derniere_config"]["poids_avant"] = 0;
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
  s.allure = config["allure"] | 0;
  s.vitessePrevue = config["vitesse"] | 0.0;
  s.ressentiConnu = !config["ressenti"].isNull();
  s.ressenti = config["ressenti"] | 0;
  s.detail = lireDetail(config);
  s.pesee = config["pesee"];
  s.poidsAvant = config["poids_avant"];
  s.part = p["calcul"]["part"];
  s.objectifInitial = p["objectif"];
  s.decalageEchauffement = random(-180, 181);
  for (JsonVariantConst b : config["bidons"].as<JsonArrayConst>()) {
    s.bidons.push_back({b["volume"], b["type"].as<const char*>(), b["sucre"], b["sel"], b["sodium"], b["glucides"], 0});
  }
  dernierTick = millis();
  sauvegarderEnCours();
  afficherEtat();
  Serial.printf("Départ : %s / %s, part à compenser %.2f, objectif %d mL\n", s.sport.c_str(), s.seance.c_str(),
                s.part, s.objectifInitial);
  return true;
}

void sortiePause() {
  if (!s.active) return;
  s.pause = !s.pause;
  dernierTick = millis();
  sauvegarderEnCours();
  afficherEtat();
}

// Résumé de la dernière sortie terminée, joint au retour de l'utilisateur (voir sortieResume)
static JsonDocument dernierResume;

// Puissance de référence apprise sur les 3 premières sorties à vélo, quand la FTP est inconnue
static void apprendrePuissance() {
  if ((profil["ftp"] | 0) > 0 || s.dureePuissance < 20 * 60) return;
  int n = profil["p_ref_n"] | 0;
  if (n >= 3) return;
  float p = profil["p_ref"] | 0.0;
  profil["p_ref"] = round((p * n + puissanceMoyenne()) / (n + 1));
  profil["p_ref_n"] = n + 1;
  enregistrerProfil();
}

bool sortieTerminer() {
  if (!s.active) return false;
  dernierResume.clear();
  dernierResume["debut"] = s.debut;
  dernierResume["duree"] = s.duree;
  dernierResume["sport"] = s.sport;
  dernierResume["seance"] = s.seance;
  dernierResume["bu"] = round(s.buMl);
  dernierResume["objectif"] = objectifActuel();
  dernierResume["perdue"] = round(s.perdueMl);
  dernierResume["rappels"] = s.nbRappels;
  dernierResume["fc_moy"] = fcMoyenne();
  dernierResume["r_effort_moy"] = arrondi(rEffortMoyen(), 0.01);
  if (s.ressentiConnu) dernierResume["ressenti"] = s.ressenti;
  dernierResume["pesee"] = s.pesee;
  if (s.pesee) {
    dernierResume["poids_avant"] = s.poidsAvant;
    int capacite = 0;
    for (const Bidon& b : s.bidons) capacite += b.volume;
    dernierResume["capacite"] = capacite;
  }
  // Consentement : l'historique n'est enregistré que si l'utilisateur l'accepte
  if (reglages["consent_historique"] | true) {
    JsonDocument bilan;
    versJson(bilan, false);
    ajouterHistorique(bilan);
  }
  if (s.sport == "velo") apprendrePuissance();
  supprimerFichier(FICHIER_EN_COURS);
  Serial.printf("Fin : %.0f mL bus sur %d mL\n", s.buMl, objectifActuel());
  s = EtatSortie{};
  ecrire(PIN_LED, false);
  afficherEtat();
  return true;
}

// Pesée de fin de sortie (questionnaire) : {"poids_apres":69.2, "reste":"vides|quart|moitie|pleins", "hors":false}
bool sortiePesee(JsonVariantConst recu, JsonObject resultat) {
  if (dernierResume.isNull() || !(dernierResume["pesee"] | false)) return false;
  float avant = dernierResume["poids_avant"] | 0.0, apres = recu["poids_apres"] | 0.0;
  float heures = (dernierResume["duree"] | 0) / 3600.0;
  const char* reste = choix(recu["reste"], {"vides", "quart", "moitie", "pleins"});
  float fraction = strcmp(reste, "quart") == 0 ? 0.25 : strcmp(reste, "moitie") == 0 ? 0.5
                   : strcmp(reste, "pleins") == 0 ? 1.0 : 0.0;
  float buL = (dernierResume["capacite"] | 0) / 1000.0 * (1 - fraction);
  bool hors = recu["hors"] | false;
  resultat["poids_avant"] = avant;
  resultat["poids_apres"] = apres;
  resultat["bu"] = round(buL * 1000);
  resultat["hors"] = hors;

  if (heures < 0.5 || apres < 30 || apres > 200) {
    resultat["erreur"] = "sortie trop courte ou poids invalide";
    return true;
  }
  // 1 kg perdu = 1 L d'eau
  float sueur = (avant - apres + buL) / heures;
  resultat["sueur"] = arrondi(sueur, 0.01);
  if (sueur < 0.1 || sueur > 3.5) {
    resultat["erreur"] = "valeur hors de la plage possible (0,1 à 3,5 L/h)";
    return true;
  }
  // Ramenée à l'effort de référence, avec le ressenti du jour : un point de calibration
  JsonArray pesees = profil["pesees"].as<JsonArray>();
  if (pesees.isNull()) pesees = profil["pesees"].to<JsonArray>();
  while (pesees.size() >= MAX_PESEES) pesees.remove(0);
  JsonObject p = pesees.add<JsonObject>();
  p["f"] = famille(dernierResume["sport"] | "course");
  p["s"] = arrondi(sueur / max(0.2f, (float)(dernierResume["r_effort_moy"] | 1.0)), 0.001);
  if (!dernierResume["ressenti"].isNull()) p["at"] = dernierResume["ressenti"];
  else p["at"] = nullptr;
  p["fiable"] = !hors;
  enregistrerProfil();
  resultat["enregistree"] = true;
  dernierResume["pesee"] = false;  // une seule pesée par sortie
  return true;
}

// ---------- Boucle ----------

static void avancerUneSeconde();

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
  avancerUneSeconde();
}

static void enregistrerPoint() {
  if (s.points.size() >= MAX_POINTS) {
    // Mémoire pleine (sortie très longue) : un point sur deux, et deux fois moins souvent
    for (size_t i = 0; i < s.points.size() / 2; i++) s.points[i] = s.points[2 * i + 1];
    s.points.resize(s.points.size() / 2);
    s.pasPoints *= 2;
  }
  s.points.push_back({s.duree, (uint8_t)round(s.fc), (uint16_t)round(s.vLisse * 10), (uint16_t)round(s.puissance),
                      (uint16_t)round(s.sueur * 1000), (uint16_t)min(65535.0f, round(s.perdueMl)),
                      (uint16_t)min(65535.0f, round(s.aBoireMl)), (uint16_t)min(65535.0f, round(s.buMl)), (uint8_t)s.phase});
}

static void avancerUneSeconde() {
  s.duree++;
  simulerCapteurs();
  calculerEffort();
  suivrePhases();

  // Sueur et quantité à boire de cette seconde
  Calibration c = calibration(famille(s.sport));
  s.rMeteo = s.ressentiConnu ? constrain(exp(c.k * (s.ressenti - c.atRef)), R_METEO_MIN, R_METEO_MAX) : 1;
  s.sueur = min(c.sBase * s.rEffort * s.rMeteo, SUEUR_MAX);
  s.aBoire = min(s.part * s.sueur, plafond(c));
  s.perdueMl += s.sueur * 1000 / 3600;
  s.sodiumPerdu += s.sueur / 3600 * sodiumSueurMgL();
  s.aBoireMl += s.aBoire * 1000 / 3600;
  s.compteur += s.aBoire * 1000 / 3600;

  // Fin de l'affichage de la consigne : la LED s'éteint (l'écran s'éteint tout seul)
  if (s.alerte > 0 && --s.alerte == 0) ecrire(PIN_LED, false);
  if (alerteDue()) {
    // En plein effort, on attend la récupération, mais jamais plus de 15 min
    bool attendre = attendreRecup() && (!s.attente || s.duree - s.debutAttente < (uint32_t)ATTENTE_RECUP_MAX);
    if (!attendre) declencherAlerte();
    else if (!s.attente) { s.attente = true; s.debutAttente = s.duree; }
  }
  if (s.duree % s.pasPoints == 0) enregistrerPoint();
  if (!simulationRapide && s.duree % SAUVEGARDE_TOUTES_LES_S == 0) sauvegarderEnCours();
}

void sortieEtat(JsonDocument& doc) {
  doc["active"] = s.active;
  doc["pause"] = s.pause;
  // Batterie simulée : perd 1 % toutes les 6 minutes depuis l'allumage
  doc["batterie"] = max(5, 100 - (int)(millis() / 360000));
  if (!s.active) return;

  doc["sport"] = s.sport;
  doc["seance"] = s.seance;
  doc["duree"] = s.duree;
  doc["duree_prevue"] = s.dureePrevue;
  if (s.ressentiConnu) doc["ressenti"] = s.ressenti;
  else doc["ressenti"] = nullptr;
  doc["bu"] = round(s.buMl);
  doc["objectif"] = objectifActuel();
  doc["perdue"] = round(s.perdueMl);
  doc["sodium_perdu"] = round(s.sodiumPerdu);
  doc["sodium_apporte"] = round(s.sodiumApporte);
  doc["glucides"] = round(s.glucidesApportes);
  // Capteurs (simulés) : null quand l'utilisateur n'a pas le capteur
  if (s.fc > 0) doc["fc"] = round(s.fc); else doc["fc"] = nullptr;
  if (s.puissance > 0) doc["puissance"] = round(s.puissance); else doc["puissance"] = nullptr;
  if (s.vitesse > 0) doc["vitesse"] = arrondi(s.vitesse, 0.1); else doc["vitesse"] = nullptr;
  doc["transpi"] = arrondi(s.sueur, 0.01);
  doc["km"] = round(s.km * 10) / 10;
  doc["rappel"] = secondesAvantAlerte();
  doc["rappels"] = s.nbRappels;
  doc["attente"] = s.attente;
  doc["pesee"] = s.pesee;  // le questionnaire de fin demandera le poids après
  Plan plan = planPour(s.aBoire * 1000 / 60);
  doc["plan"]["intervalle"] = plan.intervalle;
  doc["plan"]["gorgees"] = plan.gorgees;
  // D'où viennent les estimations (tout est simulé dans ce prototype)
  doc["sources"]["effort"] = s.sourceEffort;
  doc["sources"]["meteo"] = s.ressentiConnu ? "saisie" : "aucune";
  doc["sources"]["simule"] = true;
  // « Pourquoi cette consigne ? »
  Calibration c = calibration(famille(s.sport));
  JsonObject calcul = doc["calcul"].to<JsonObject>();
  calcul["s_base"] = arrondi(c.sBase, 0.01);
  calcul["r_effort"] = arrondi(s.rEffort, 0.01);
  calcul["r_meteo"] = arrondi(s.rMeteo, 0.01);
  calcul["sueur"] = arrondi(s.sueur, 0.01);
  calcul["part"] = arrondi(s.part, 0.01);
  calcul["a_boire"] = arrondi(s.aBoire, 0.01);
  calcul["plafond"] = plafond(c);
  calcul["compteur"] = round(s.compteur);
  if (s.phase != AUCUNE) {
    doc["phase"]["nom"] = NOMS_PHASES[s.phase];
    doc["phase"]["repetition"] = s.repetition;
    doc["phase"]["repetitions"] = s.detail.donne ? s.detail.repetitions : 0;
  }
  if (s.alerte > 0) {
    doc["alerte"]["restant"] = s.alerte;
    doc["alerte"]["gorgees"] = s.alerteGorgees;
    doc["alerte"]["bidon"] = s.alerteBidon;
  }
  ecrireBidons(doc["bidons"].to<JsonArray>());
}

// Résumé avec les courbes (au plus 240 points envoyés) et les alertes, pour les graphiques de l'appli
static void resumeComplet(JsonObject r) {
  float poids = profil["poids"] | 70;
  r["sport"] = s.sport;
  r["seance"] = s.seance;
  r["duree"] = s.duree;
  r["km"] = round(s.km * 10) / 10;
  if (s.ressentiConnu) r["ressenti"] = s.ressenti; else r["ressenti"] = nullptr;
  r["poids"] = poids;
  JsonObject t = r["totaux"].to<JsonObject>();
  t["perdue"] = round(s.perdueMl);
  t["bu"] = round(s.buMl);
  t["a_boire"] = round(s.aBoireMl);
  t["perte_poids"] = arrondi((s.perdueMl - s.buMl) / 1000 / poids * 100, 0.1);  // % du poids à la fin
  t["sodium_perdu"] = round(s.sodiumPerdu);
  t["sodium_apporte"] = round(s.sodiumApporte);
  t["glucides"] = round(s.glucidesApportes);
  t["rappels"] = s.nbRappels;
  t["fc_moy"] = fcMoyenne();
  t["p_moy"] = puissanceMoyenne();
  t["vitesse_moy"] = s.duree ? arrondi(s.km / (s.duree / 3600.0), 0.1) : 0;
  t["part"] = arrondi(s.part, 0.01);
  JsonObject serie = r["serie"].to<JsonObject>();
  JsonArray tt = serie["t"].to<JsonArray>(), fc = serie["fc"].to<JsonArray>(), v = serie["v"].to<JsonArray>(),
            p = serie["p"].to<JsonArray>(), sueur = serie["sueur"].to<JsonArray>(), perdue = serie["perdue"].to<JsonArray>(),
            aBoire = serie["a_boire"].to<JsonArray>(), bu = serie["bu"].to<JsonArray>(), phase = serie["phase"].to<JsonArray>();
  size_t pas = s.points.size() / 240 + 1;
  for (size_t i = 0; i < s.points.size(); i += pas) {
    const Point& pt = s.points[i];
    tt.add(pt.t); fc.add(pt.fc); v.add(pt.v10 / 10.0); p.add(pt.p); sueur.add(pt.sueur);
    perdue.add(pt.perdue); aBoire.add(pt.aBoire); bu.add(pt.bu); phase.add(NOMS_PHASES[pt.phase]);
  }
  JsonArray liste = r["gorgees"].to<JsonArray>();
  for (const Gorgee& g : s.gorgees) {
    JsonArray item = liste.add<JsonArray>();
    item.add(g.t);
    item.add(g.ml);
    item.add(g.bidon);
  }
  ecrireBidons(r["bidons"].to<JsonArray>());
}

// « Simuler la course en entier » : fait avancer la sortie jusqu'à sa durée prévue (1 h si elle
// n'en a pas), sans écran ni vibreur, la termine et renvoie le résumé. false si aucune sortie.
bool sortieSimulerEnEntier(JsonObject resume) {
  if (!s.active) return false;
  s.pause = false;
  uint32_t fin = max((uint32_t)(s.dureePrevue > 0 ? s.dureePrevue * 60 : 3600), s.duree);
  simulationRapide = true;
  while (s.duree < fin) {
    avancerUneSeconde();
    if (s.duree % 600 == 0) yield();  // laisse respirer le Wi-Fi
  }
  simulationRapide = false;
  s.alerte = 0;
  resumeComplet(resume);
  sortieTerminer();
  return true;
}

bool sortieResume(JsonObject resume) {
  if (dernierResume.isNull()) return false;
  resume.set(dernierResume.as<JsonObjectConst>());
  return true;
}
