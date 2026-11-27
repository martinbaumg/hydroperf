"""Faux boîtier HydroPerf : sert web/index.html avec la même API que l'ESP32, pour tester l'interface sans la carte.

    python3 outils/faux_boitier.py web/index.html 8765
    puis ouvrir http://localhost:8765

Outil de test : POST /test/avancer?s=600 fait avancer la sortie en cours de 600 secondes.
L'algorithme (v2) est le même que dans src/hydroperf/sortie.cpp : toute modification doit être faite des deux côtés.
"""
import copy
import json
import math
import random
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# ---------- Constantes de l'algorithme (mêmes valeurs que sortie.cpp) ----------
I_REF = 0.65
R_EFFORT_MIN, R_EFFORT_MAX = 0.2, 1 / I_REF
K_DEFAUT, K_MIN, K_MAX = 0.03, 0.01, 0.05
AT_REF_DEFAUT = 24
R_METEO_MIN, R_METEO_MAX = 0.7, 1.8
ECART_CALIBRATION = 8
SUEUR_MAX = 2.5
PERTE_VISEE = 0.015
PART_MIN, PART_MAX, PART_SANS_DUREE = 0.5, 0.8, 0.6
PLAFOND, PLAFOND_PESEES = 0.8, 1.0
INTERVALLE_MIN, INTERVALLE_MAX = 5 * 60, 20 * 60
GORGEES_MAX = 3
R_EFFORT_FACILE, R_EFFORT_DUR = 0.9, 1.1   # 3 gorgées visées en dessous, 1 au-dessus, 2 entre
ATTENTE_RECUP_MAX = 15 * 60
SODIUM_CIBLE = {"non": 460, "nsp": 575, "oui": 690}   # mg/L bus : fourchette de l'ACSM (20-30 mmol/L)
GLUCIDES_MAX_H, GLUCIDES_AVANCE = 60, 15
ATTENTE_RECUP_VELO = 5 * 60
MAX_PESEES = 20

CONFIG = {"sport": "velo", "seance": "endurance", "duree": 120, "distance": 50, "ressenti": None, "allure": 0,
          "vitesse": 0, "denivele": 0,
          "bidons": [{"volume": 750, "type": "eau", "sucre": False, "sel": False, "sodium": -1, "glucides": -1},
                     {"volume": 500, "type": "electrolyte", "sucre": False, "sel": False, "sodium": -1, "glucides": -1}]}
PROFIL = {"poids": 68, "taille": 178, "fc_repos": 52, "fc_max": 189, "sexe": "NC", "sel": "nsp",
          "capteur_cardio": True, "capteur_puissance": False, "capteur_gps": False, "ftp": 0, "allure_ref": 0,
          "pesees_actives": False, "pesees": [], "p_ref": 0, "p_ref_n": 0}
profil = copy.deepcopy(PROFIL)
reglages = {"fixation": "poignet", "alerte_vibreur": True, "alerte_buzzer": False, "intensite_vibreur": 80,
            "intensite_buzzer": 60, "duree_ecran": 10, "auto": True, "intervalle": 12, "gorgee": 30,
            "consent_historique": True, "consent_sante": False, "consent_anonyme": False, "unites": "metrique",
            "theme": "auto", "notif_telephone": True, "derniere_config": copy.deepcopy(CONFIG)}
courses = [dict(id=1, nom="Vélo du dimanche", **copy.deepcopy(CONFIG)),
           dict(id=2, nom="Footing du midi", sport="course", seance="footing", duree=45, distance=8,
                bidons=[{"volume": 500, "type": "eau", "sucre": False, "sel": False, "sodium": -1, "glucides": -1}])]
historique = []
retours = []          # questionnaire de fin de sortie
dernier_resume = {}   # résumé de la dernière sortie terminée, joint au retour
dernier_bilan = {}    # bilan vérifié par l'utilisateur (eau bue, distance, pesée), joint au retour
commandes = {"led": 0, "vibreur": 0, "buzzer": 0}
s = None
acceleration = 1   # démo : secondes de sortie par seconde réelle (1, 2, 4, 8 ou 16)


def borner(x, a, b):
    return max(a, min(b, x))


def arrondi(x, pas):
    return round(round(x / pas) * pas, 4)


def gorgee():
    return max(5, reglages["gorgee"])


# ---------- Profil : équipement et références ----------

def famille(sport):
    return "velo" if sport == "velo" else "course"


def a_cardio():
    return profil["capteur_cardio"]


def a_puissance(sport):
    return sport == "velo" and profil["capteur_puissance"]


def a_vitesse():
    """Capteur de vitesse (montre GPS, capteur de roue ou téléphone), pour tous les sports."""
    return profil["capteur_gps"]


def allure_ref():
    """Allure de référence du profil (s/km), 0 si inconnue."""
    return profil["allure_ref"]


def allure_habituelle():
    """Pour estimer une durée ou simuler l'athlète : l'allure du profil, sinon 5:30."""
    return allure_ref() or 330


def vitesse_ref():
    return 3600 / allure_habituelle()


def puissance_ref():
    if profil["ftp"] > 0:
        return I_REF * profil["ftp"]
    return profil["p_ref"] if profil["p_ref"] > 0 else 160


def intensite_seance(seance):
    return {"footing": 0.55, "balade": 0.55, "fractionne": 0.75, "tempo": 0.75}.get(seance, I_REF)


def sodium_sueur():
    return {"oui": 1100, "non": 600}.get(profil["sel"], 830)


def sodium_cible():
    return SODIUM_CIBLE.get(profil["sel"], SODIUM_CIBLE["nsp"])


def gorgees_visees(r_effort):
    return 3 if r_effort < R_EFFORT_FACILE else 2 if r_effort <= R_EFFORT_DUR else 1


# ---------- Calibration ----------

def s_base_defaut(fam):
    if fam == "velo":
        return 0.7 * profil["ftp"] / 250 if profil["ftp"] > 0 else 0.7
    return 0.9 * profil["poids"] / 70


def calibration(fam):
    c = {"s_base": s_base_defaut(fam), "at_ref": AT_REF_DEFAUT, "k": K_DEFAUT, "pesees": 0, "k_personnel": False}
    pesees = profil["pesees"]
    for p in pesees:
        if p.get("at") is not None:
            c["at_ref"] = p["at"]
            break
    num = den = 0
    assez = False
    for f in ("velo", "course"):
        pts = [(p["at"], math.log(p["s"])) for p in pesees
               if p["f"] == f and p.get("at") is not None and p.get("fiable", True)]
        if len(pts) < 2:
            continue
        ats = [a for a, _ in pts]
        if max(ats) - min(ats) >= ECART_CALIBRATION:
            assez = True
        ma = sum(ats) / len(pts)
        ml = sum(l for _, l in pts) / len(pts)
        num += sum((a - ma) * (l - ml) for a, l in pts)
        den += sum((a - ma) ** 2 for a, _ in pts)
    if assez and den > 0 and K_MIN <= num / den <= K_MAX:
        c["k"], c["k_personnel"] = num / den, True
    somme = poids = 0
    for p in pesees:
        if p["f"] != fam:
            continue
        sv = p["s"] * (math.exp(-c["k"] * (p["at"] - c["at_ref"])) if p.get("at") is not None else 1)
        w = 1 if p.get("fiable", True) else 0.5
        somme += w * sv
        poids += w
        c["pesees"] += 1
    if poids > 0:
        c["s_base"] = somme / poids
    return c


def calibration_etat():
    o = {}
    for fam in ("course", "velo"):
        c = calibration(fam)
        o[fam] = {"s_base": arrondi(c["s_base"], 0.01), "pesees": c["pesees"]}
        o.update(at_ref=arrondi(c["at_ref"], 0.1), k=arrondi(c["k"], 0.001), k_personnel=c["k_personnel"])
    o.update(pesees=len(profil["pesees"]), sodium_sueur=sodium_sueur(), sodium_cible=sodium_cible(), p_ref=round(puissance_ref()))
    return o


def r_meteo(ressenti, c):
    if ressenti is None:
        return 1
    return borner(math.exp(c["k"] * (ressenti - c["at_ref"])), R_METEO_MIN, R_METEO_MAX)


def part_a_compenser(sueur, duree_min):
    if duree_min <= 0 or sueur <= 0:
        return PART_SANS_DUREE
    return borner(1 - PERTE_VISEE * profil["poids"] / (sueur * duree_min / 60), PART_MIN, PART_MAX)


def plafond(c):
    return PLAFOND_PESEES if c["pesees"] > 0 else PLAFOND


# ---------- Boissons ----------

def sodium_boisson(b):
    if b.get("sodium", -1) >= 0:
        return b["sodium"]
    return {"electrolyte": 900, "isotonique": 600}.get(b["type"], 600 if b["type"] == "maison" and b["sel"] else 0)


def glucides_boisson(b):
    if b.get("glucides", -1) >= 0:
        return b["glucides"]
    return 60 if b["type"] == "isotonique" or (b["type"] == "maison" and b["sucre"]) else 0


def sucre(b):
    return glucides_boisson(b) > 0


# ---------- Configuration ----------

def entier(v, defaut=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return defaut


def normaliser(c):
    sport = c.get("sport") if c.get("sport") in ("course", "trail", "velo") else "course"
    velo = sport == "velo"
    seance = c.get("seance")
    if velo:
        seance = {"footing": "balade", "longue": "endurance"}.get(seance, seance)
        seance = seance if seance in ("endurance", "balade", "tempo", "fractionne") else "endurance"
    else:
        seance = {"balade": "footing", "endurance": "longue", "tempo": "longue"}.get(seance, seance)
        seance = seance if seance in ("footing", "longue", "fractionne") else "footing"
    r = c.get("ressenti")
    out = {"sport": sport, "seance": seance, "duree": borner(entier(c.get("duree")), 0, 1440),
           "distance": round(borner(float(c.get("distance") or 0), 0, 500), 1),
           "ressenti": borner(round(r), -20, 50) if isinstance(r, (int, float)) and not isinstance(r, bool) else None,
           "allure": 0 if velo else borner(entier(c.get("allure")), 0, 1200),
           "vitesse": round(borner(float(c.get("vitesse") or 0), 0, 60), 1) if velo else 0,
           "denivele": borner(entier(c.get("denivele")), 0, 10000) if sport == "trail" else 0}
    d = c.get("detail")
    if seance == "fractionne" and isinstance(d, dict) and (entier(d.get("effort_s")) > 0 or entier(d.get("effort_m")) > 0):
        out["detail"] = {"echauffement_min": borner(entier(d.get("echauffement_min"), 15), 0, 90),
                         "repetitions": borner(entier(d.get("repetitions"), 1), 1, 50),
                         "effort_s": borner(entier(d.get("effort_s")), 0, 3600),
                         "effort_m": borner(entier(d.get("effort_m")), 0, 20000),
                         "allure_effort": 0 if sport == "velo" else borner(entier(d.get("allure_effort")), 0, 1200),
                         "vitesse_effort": round(borner(float(d.get("vitesse_effort") or 0), 0, 80), 1) if sport == "velo" else 0,
                         "recup_s": borner(entier(d.get("recup_s")), 0, 1800),
                         "retour_calme_min": borner(entier(d.get("retour_calme_min"), 10), 0, 90)}
    pesee = bool(c.get("pesee")) and float(c.get("poids_avant") or 0) >= 30
    out["pesee"] = pesee
    out["poids_avant"] = round(borner(float(c["poids_avant"]), 30, 200), 1) if pesee else 0
    bidons = []
    for b in c.get("bidons", [])[:3]:
        t = b.get("type") if b.get("type") in ("eau", "electrolyte", "isotonique", "maison") else "eau"
        bidons.append({"volume": borner(entier(b.get("volume"), 500), 100, 2000), "type": t,
                       "sucre": t == "maison" and bool(b.get("sucre")), "sel": t == "maison" and bool(b.get("sel")),
                       "sodium": borner(entier(b.get("sodium"), -1), -1, 3000),
                       "glucides": borner(entier(b.get("glucides"), -1), -1, 200)})
    out["bidons"] = bidons or [{"volume": 500, "type": "eau", "sucre": False, "sel": False, "sodium": -1, "glucides": -1}]
    return out


def vitesse_kmh(c):
    """Vitesse prévue en km/h (en trail : kilomètres-effort par heure)."""
    if c["sport"] == "velo":
        if c["vitesse"] > 0:
            return c["vitesse"]
        return 25 * {"balade": 0.8, "tempo": 1.12, "fractionne": 1.08}.get(c["seance"], 1)
    if c["allure"] > 0:
        return 3600 / c["allure"]
    return vitesse_ref() * {"footing": 0.9, "fractionne": 1.1}.get(c["seance"], 1) * (0.85 if c["sport"] == "trail" else 1)


def km_effort(c):
    """En trail, 100 m de dénivelé positif comptent comme 1 km."""
    return c["distance"] + c["denivele"] / 100


def vitesse_effort(o):
    """km/h pendant les fractions : allure (course) ou vitesse (vélo) données, 0 sinon."""
    return 3600 / o["allure_effort"] if o.get("allure_effort") else o.get("vitesse_effort", 0)


def lire_detail(c):
    d = {"echauffement": 900, "repetitions": 6, "effort": 180, "recup": 120, "calme": 600, "donne": False,
         "vitesse_effort": 0}
    o = c.get("detail")
    if not o:
        return d
    effort = o["effort_s"]
    if effort <= 0:
        kmh = vitesse_effort(o) or vitesse_ref() * 0.85 / I_REF
        effort = round(o["effort_m"] / 1000 / kmh * 3600)
    d["vitesse_effort"] = vitesse_effort(o)
    d.update(echauffement=o["echauffement_min"] * 60, repetitions=o["repetitions"], effort=max(10, effort),
             recup=o["recup_s"], calme=o["retour_calme_min"] * 60, donne=True)
    return d


def duree_detail(d):
    return d["echauffement"] + d["repetitions"] * d["effort"] + max(0, d["repetitions"] - 1) * d["recup"] + d["calme"]


def duree_estimee(c):
    if c.get("detail"):
        return round(duree_detail(lire_detail(c)) / 60)
    if c["duree"]:
        return c["duree"]
    if c["distance"]:
        return round(km_effort(c) / vitesse_kmh(c) * 60)
    return 0


def effort_par_allure(sport, allure):
    return sport == "course" and allure > 0 and allure_ref() > 0


def r_effort_declare(sport, seance, allure):
    if effort_par_allure(sport, allure):
        return borner(allure_ref() / allure, R_EFFORT_MIN, R_EFFORT_MAX)
    return intensite_seance(seance) / I_REF


def plan_pour(ml_par_min, visees):
    g = gorgee()
    if not reglages["auto"]:
        i = max(1, reglages["intervalle"])
        return i, borner(round(ml_par_min * i / g), 1, 8)
    if ml_par_min <= 0:
        return 20, 1
    t = visees * g / ml_par_min
    if t > 20:
        return 20, max(1, min(GORGEES_MAX, int(20 * ml_par_min / g)))
    if t >= 5:
        return round(t), visees
    return 5, max(1, min(GORGEES_MAX, int(5 * ml_par_min / g)))


def prevoir(c):
    cal = calibration(famille(c["sport"]))
    r_e = r_effort_declare(c["sport"], c["seance"], c["allure"])
    r_m = r_meteo(c["ressenti"], cal)
    sueur = min(cal["s_base"] * r_e * r_m, SUEUR_MAX)
    duree = duree_estimee(c)
    f = part_a_compenser(sueur, duree)
    a_boire = min(f * sueur, plafond(cal))
    intervalle, gorgees = plan_pour(a_boire * 1000 / 60, gorgees_visees(r_e))
    return {"transpiration": arrondi(sueur, 0.01), "par_heure": arrondi(a_boire * 1000, 10), "duree_estimee": duree,
            "objectif": arrondi(a_boire * 1000 * duree / 60, 50) if duree else 0, "intervalle": intervalle,
            "gorgees": gorgees, "capacite": sum(b["volume"] for b in c["bidons"]),
            "perte": round(sueur * 1000 * duree / 60), "sodium": round(sueur * duree / 60 * sodium_sueur()),
            "calcul": {"s_base": arrondi(cal["s_base"], 0.01), "r_effort": arrondi(r_e, 0.01),
                       "r_meteo": arrondi(r_m, 0.01), "sueur": arrondi(sueur, 0.01), "part": arrondi(f, 0.01),
                       "a_boire": arrondi(a_boire, 0.01), "plafond": plafond(cal), "k": arrondi(cal["k"], 0.001),
                       "at_ref": cal["at_ref"],
                       "effort": "allure" if effort_par_allure(c["sport"], c["allure"]) else "seance"}}


# ---------- Sortie en cours ----------

def demarrer(c):
    p = prevoir(c)
    return {"debut": int(time.time()), "duree": 0, "pause": False, "sport": c["sport"], "seance": c["seance"],
            "duree_prevue": p["duree_estimee"], "allure": c["allure"], "distance_prevue": c["distance"], "vitesse_prevue": c["vitesse"],
            "ressenti": c["ressenti"], "glucides": 0,
            "detail": lire_detail(c), "pesee": c["pesee"], "poids_avant": c["poids_avant"],
            "part": p["calcul"]["part"], "objectif_initial": p["objectif"], "a_boire_prevu": p["calcul"]["a_boire"], "r_effort_prevu": p["calcul"]["r_effort"],
            "bidons": [dict(b, bu=0) for b in c["bidons"]],
            "bu": 0, "perdue": 0, "a_boire_cumul": 0, "km": 0, "sodium_perdu": 0, "sodium_apporte": 0,
            "fc_somme": 0, "p_somme": 0, "r_effort_somme": 0, "duree_cardio": 0, "duree_puissance": 0, "duree_vitesse": 0,
            "compteur": 0, "derniere_alerte": 0, "rappels": 0, "attente": False, "debut_attente": 0, "alerte": None,
            "fc": 0, "puissance": 0, "vitesse": 0, "r_effort": 1, "r_meteo": 1, "sueur": 0, "a_boire": 0,
            "source_effort": "seance", "decalage": random.randint(-180, 180), "fc_lisse": 0, "p_lisse": 0,
            "v_lisse": 0, "signal": 0, "phase": None, "repetition": 0, "debut_phase": 0, "moy_phase": 0, "nb_phase": 0,
            "condition": 0, "gorgees": [], "points": [], "pas_points": 60}


def objectif():
    """Déjà dû + reste de la sortie au débit du moment : s'ajuste pendant la sortie."""
    reste = max(0, s["duree_prevue"] * 60 - s["duree"]) if s["duree_prevue"] else 0
    debit = max(s["a_boire"], s["a_boire_prevu"]) if s["duree"] < 600 else s["a_boire"]
    return arrondi(s["a_boire_cumul"] + reste * debit * 1000 / 3600, 10)


en_effort_vrai = False


def intensite_vraie(t):
    global en_effort_vrai
    en_effort_vrai = False
    if s["seance"] == "fractionne":
        d = s["detail"]
        t0 = max(60, d["echauffement"] + s["decalage"])
        if t < t0:
            return 0.3 + 0.2 * min(1, t / 300)
        dans = t - t0
        cycle = d["effort"] + d["recup"]
        rep = dans // cycle
        if rep < d["repetitions"]:
            pos = dans % cycle
            if pos < d["effort"]:
                en_effort_vrai = True
                return 0.85
            return 0.45 if rep == d["repetitions"] - 1 else 0.40
        return 0.45
    cible = intensite_seance(s["seance"])
    if s["sport"] == "course" and s["allure"] > 0:
        cible = borner(I_REF * allure_habituelle() / s["allure"], 0.3, 0.95)
    return 0.3 + (cible - 0.3) * min(1, t / 600) + 0.04 * math.sin(t / 300)


def simuler_capteurs():
    iv = intensite_vraie(s["duree"])
    repos, fmax = profil["fc_repos"], profil["fc_max"]
    if a_cardio():
        ressenti = s["ressenti"] if s["ressenti"] is not None else 20
        derive = (3 + 0.3 * max(0, ressenti - 20)) * s["duree"] / 3600
        cible = repos + iv * (fmax - repos) + derive
        if s["fc_lisse"] == 0:
            s["fc_lisse"] = repos + 20
        s["fc_lisse"] += (cible - s["fc_lisse"]) / 60
        s["fc"] = min(fmax, s["fc_lisse"] + random.uniform(-1.5, 1.5))
    else:
        s["fc"] = 0
    bruit = 1 + random.uniform(-0.05, 0.05)
    if a_puissance(s["sport"]):
        p = puissance_ref() * iv / I_REF * bruit
        s["p_lisse"] = p if s["p_lisse"] == 0 else s["p_lisse"] + (p - s["p_lisse"]) / 30
        s["puissance"] = s["p_lisse"]
        brut = p
    else:
        s["puissance"] = 0
        brut = 0
    v = vitesse_ref() * iv / I_REF * bruit
    if s["sport"] == "velo":
        v = (s["vitesse_prevue"] or 25) * (0.7 + 0.3 * iv / I_REF)
    elif s["sport"] == "trail":
        v = 7 * iv / I_REF
    if en_effort_vrai and s["detail"]["vitesse_effort"]:
        v = s["detail"]["vitesse_effort"] * bruit
    s["v_lisse"] = v if s["v_lisse"] == 0 else s["v_lisse"] + (v - s["v_lisse"]) / 30
    s["vitesse"] = s["v_lisse"] if a_vitesse() else 0
    if s["vitesse"] > 0:
        s["duree_vitesse"] += 1
    s["km"] += s["v_lisse"] / 3600
    # Signal rapide (moyenne sur environ 5 s) pour détecter les fractions
    if not brut and a_vitesse():
        brut = v
    s["signal"] = 0 if not brut else (brut if not s["signal"] else s["signal"] + (brut - s["signal"]) / 5)


def calculer_effort():
    repos, fmax = profil["fc_repos"], profil["fc_max"]
    fc, p = s["fc"] > 0, s["puissance"] > 0
    v = s["vitesse"] > 0 and s["sport"] == "course" and allure_ref() > 0
    r_fc = (s["fc"] - repos) / (fmax - repos) / I_REF if fc else 0
    r_p = s["puissance"] / puissance_ref() if p else 0
    r_v = s["vitesse"] / (3600 / allure_ref()) if v else 0
    if p and fc:
        r, src = 0.6 * r_p + 0.4 * r_fc, "puissance+cardio"
    elif p:
        r, src = r_p, "puissance"
    elif v and fc:
        r, src = 0.5 * r_fc + 0.5 * r_v, "cardio+vitesse"
    elif v:
        r, src = r_v, "vitesse"
    elif fc:
        r, src = r_fc, "cardio"
    else:
        r = r_effort_declare(s["sport"], s["seance"], s["allure"])
        src = "allure" if effort_par_allure(s["sport"], s["allure"]) else "seance"
    s["r_effort"], s["source_effort"] = borner(r, R_EFFORT_MIN, R_EFFORT_MAX), src
    if fc:
        s["fc_somme"] += s["fc"]
        s["duree_cardio"] += 1
    if p:
        s["p_somme"] += s["puissance"]
        s["duree_puissance"] += 1
    s["r_effort_somme"] += s["r_effort"]


def changer_phase(phase):
    s.update(phase=phase, debut_phase=s["duree"], moy_phase=0, nb_phase=0, condition=0)


def phases_prevues():
    """Sans capteur de puissance ni de vitesse : les phases suivent le détail du fractionné."""
    d = s["detail"]
    t, cycle, debut, rep, p = s["duree"], d["effort"] + d["recup"], 0, 0, "echauffement"
    if t >= d["echauffement"]:
        dans = t - d["echauffement"]
        rep = min(dans // cycle, d["repetitions"] - 1)
        pos = dans - rep * cycle
        if pos < d["effort"]:
            p, debut = "effort", t - pos
        elif rep < d["repetitions"] - 1:
            p, debut = "recup", t - (pos - d["effort"])
        else:
            p, debut = "calme", d["echauffement"] + rep * cycle + d["effort"]
        rep += 1
    if p != s["phase"]:
        s.update(phase=p, debut_phase=debut, repetition=rep)


def suivre_phases():
    signal = s["signal"]
    if s["seance"] != "fractionne":
        s["phase"] = None
        return
    if signal <= 0:
        if s["detail"]["donne"]:
            phases_prevues()
        else:
            s["phase"] = None
        return
    if s["phase"] is None:
        changer_phase("echauffement")
    dans = s["duree"] - s["debut_phase"]
    if s["phase"] in ("echauffement", "recup"):
        cond = dans >= (120 if s["phase"] == "echauffement" else 10) and signal > 1.3 * s["moy_phase"]
    elif s["phase"] == "effort":
        cond = dans >= 10 and signal < 0.65 * s["moy_phase"]
    else:
        cond = False
    s["condition"] = s["condition"] + 1 if cond else 0
    if s["condition"] >= 5:
        if s["phase"] == "effort":
            derniere = s["detail"]["donne"] and s["repetition"] >= s["detail"]["repetitions"]
            changer_phase("calme" if derniere else "recup")
        else:
            s["repetition"] += 1
            changer_phase("effort")
        return
    s["nb_phase"] += 1
    s["moy_phase"] += (signal - s["moy_phase"]) / s["nb_phase"]


def attente_max_recup():
    return ATTENTE_RECUP_VELO if s["sport"] == "velo" else ATTENTE_RECUP_MAX


def attendre_recup():
    if s["phase"] != "effort":
        return False
    return s["detail"]["effort"] - (s["duree"] - s["debut_phase"]) <= attente_max_recup()


def trop_de_glucides(b, ml):
    g = s["glucides"] + min(ml, b["volume"] - b["bu"]) / 1000 * glucides_boisson(b)
    return g > GLUCIDES_MAX_H * s["duree"] / 3600 + GLUCIDES_AVANCE


def choisir_bidon(ml):
    """Rapproche le sodium apporté de « sodium visé × eau bue » ; à égalité un bidon sucré, sans dépasser 60 g/h."""
    cible = sodium_cible()
    choisi, meilleur = -1, 1e9
    sans_sucre = any(b["volume"] - b["bu"] > 0 and not sucre(b) for b in s["bidons"])
    assez = any(b["volume"] - b["bu"] >= ml for b in s["bidons"])  # sinon un bidon presque vide couperait la consigne
    for i, b in enumerate(s["bidons"]):
        reste = b["volume"] - b["bu"]
        if reste <= 0 or (assez and reste < ml) or (sans_sucre and sucre(b) and trop_de_glucides(b, ml)):
            continue
        bu = min(ml, reste)
        apporte = s["sodium_apporte"] + bu / 1000 * sodium_boisson(b)
        ecart = abs(apporte / (cible * (s["bu"] + bu) / 1000) - 1)
        mieux = choisi < 0 or ecart < meilleur - 0.02
        egalite = choisi >= 0 and abs(ecart - meilleur) <= 0.02
        if mieux or (egalite and sucre(b) and not sucre(s["bidons"][choisi])):
            choisi, meilleur = i, min(meilleur, ecart)
    return choisi


def declencher_alerte():
    g = gorgee()
    nb = borner(int(s["compteur"] / g), 1, GORGEES_MAX) if reglages["auto"] else borner(round(s["compteur"] / g), 1, 8)
    s["compteur"] = max(0, s["compteur"] - nb * g)
    s["derniere_alerte"], s["attente"] = s["duree"], False
    choix = choisir_bidon(nb * g)
    if choix >= 0:
        b = s["bidons"][choix]
        ml = min(nb * g, b["volume"] - b["bu"])
        b["bu"] += ml
        s["bu"] += ml
        s["sodium_apporte"] += ml / 1000 * sodium_boisson(b)
        s["glucides"] += ml / 1000 * glucides_boisson(b)
        s["gorgees"].append([s["duree"], round(ml), choix])
    s["alerte"] = {"restant": reglages["duree_ecran"], "gorgees": nb, "bidon": choix}
    s["rappels"] += 1


def gorgees_maintenant():
    """Pendant les 10 premières minutes, au moins l'effort prévu (l'échauffement paraît trop facile)."""
    r = max(s["r_effort"], s["r_effort_prevu"]) if s["duree"] < 600 else s["r_effort"]
    return gorgees_visees(r)


def alerte_due():
    depuis = s["duree"] - s["derniere_alerte"]
    if not reglages["auto"]:
        return depuis >= max(1, reglages["intervalle"]) * 60
    return (depuis >= INTERVALLE_MIN and s["compteur"] >= gorgees_maintenant() * gorgee()) or depuis >= INTERVALLE_MAX


def secondes_avant_alerte():
    depuis = s["duree"] - s["derniere_alerte"]
    if not reglages["auto"]:
        return max(0, reglages["intervalle"] * 60 - depuis)
    # Pendant l'échauffement, on estime avec le débit prévu
    ml_par_s = (max(s["a_boire"], s["a_boire_prevu"]) if s["duree"] < 600 else s["a_boire"]) * 1000 / 3600
    manque = max(0, gorgees_maintenant() * gorgee() - s["compteur"])
    t = math.ceil(manque / ml_par_s) if ml_par_s > 0 else INTERVALLE_MAX
    return max(0, min(max(t, INTERVALLE_MIN - depuis), INTERVALLE_MAX - depuis))


def enregistrer_point():
    """Un point de courbe par minute (comme enregistrerPoint() dans sortie.cpp)."""
    if len(s["points"]) >= 720:
        s["points"] = s["points"][1::2]
        s["pas_points"] *= 2
    s["points"].append({"t": s["duree"], "fc": round(s["fc"]), "v": round(s["v_lisse"], 1), "p": round(s["puissance"]),
                        "sueur": round(s["sueur"] * 1000), "perdue": round(s["perdue"]), "a_boire": round(s["a_boire_cumul"]),
                        "bu": round(s["bu"]), "phase": s["phase"] or ""})


def tick(secondes=1):
    for _ in range(secondes):
        if not s or s["pause"]:
            return
        s["duree"] += 1
        simuler_capteurs()
        calculer_effort()
        suivre_phases()
        cal = calibration(famille(s["sport"]))
        s["r_meteo"] = r_meteo(s["ressenti"], cal)
        s["sueur"] = min(cal["s_base"] * s["r_effort"] * s["r_meteo"], SUEUR_MAX)
        # Sortie plus longue que prévu : f se recalcule avec la durée réelle (elle ne peut que monter)
        if s["duree"] % 60 == 0 and s["duree"] >= 1800:
            s["part"] = max(s["part"], part_a_compenser(s["perdue"] / 1000 / (s["duree"] / 3600), s["duree"] // 60))
        s["a_boire"] = min(s["part"] * s["sueur"], plafond(cal))
        s["perdue"] += s["sueur"] * 1000 / 3600
        s["sodium_perdu"] += s["sueur"] / 3600 * sodium_sueur()
        s["a_boire_cumul"] += s["a_boire"] * 1000 / 3600
        s["compteur"] += s["a_boire"] * 1000 / 3600
        if alerte_due():
            attendre = attendre_recup() and (not s["attente"] or s["duree"] - s["debut_attente"] < attente_max_recup())
            if not attendre:
                declencher_alerte()
            elif not s["attente"]:
                s["attente"], s["debut_attente"] = True, s["duree"]
        if s["duree"] % s["pas_points"] == 0:
            enregistrer_point()


def etat_sortie():
    cal = calibration(famille(s["sport"]))
    intervalle, gorgees = plan_pour(s["a_boire"] * 1000 / 60, gorgees_maintenant())
    e = {k: s[k] for k in ("sport", "seance", "duree", "duree_prevue", "ressenti", "rappels", "attente", "alerte", "pesee")}
    e.update(bu=round(s["bu"]), objectif=objectif(), perdue=round(s["perdue"]), sodium_perdu=round(s["sodium_perdu"]),
             sodium_apporte=round(s["sodium_apporte"]), glucides=round(s["glucides"]), fc=round(s["fc"]) if s["fc"] > 0 else None,
             puissance=round(s["puissance"]) if s["puissance"] > 0 else None,
             vitesse=arrondi(s["vitesse"], 0.1) if s["vitesse"] > 0 else None, transpi=arrondi(s["sueur"], 0.01),
             km=round(s["km"], 1), rappel=secondes_avant_alerte(), plan={"intervalle": intervalle, "gorgees": gorgees},
             sources={"effort": s["source_effort"], "meteo": "saisie" if s["ressenti"] is not None else "aucune",
                      "simule": True},
             calcul={"s_base": arrondi(cal["s_base"], 0.01), "r_effort": arrondi(s["r_effort"], 0.01),
                     "r_meteo": arrondi(s["r_meteo"], 0.01), "sueur": arrondi(s["sueur"], 0.01),
                     "part": arrondi(s["part"], 0.01), "a_boire": arrondi(s["a_boire"], 0.01),
                     "plafond": plafond(cal), "compteur": round(s["compteur"])},
             bidons=[dict(b, bu=round(b["bu"])) for b in s["bidons"]])
    e["mesures"] = {"fc": a_cardio(), "puissance": a_puissance(s["sport"]), "vitesse": a_vitesse()}
    if s["seance"] == "fractionne":
        e["detail"] = {k: s["detail"][k] for k in ("donne", "echauffement", "repetitions", "effort", "recup", "calme")}
    if s["phase"]:
        e["phase"] = {"nom": s["phase"], "repetition": s["repetition"],
                      "repetitions": s["detail"]["repetitions"] if s["detail"]["donne"] else 0}
    return e


def seconde_reelle():
    """Une seconde réelle : la sortie avance de `acceleration` secondes, l'affichage de l'alerte d'une."""
    if not s or s["pause"]:
        return
    tick(acceleration)
    if s and s["alerte"]:
        s["alerte"]["restant"] -= 1
        if s["alerte"]["restant"] <= 0:
            s["alerte"] = None


def ajouter_point_pesee(sport, sueur, r_effort, ressenti, fiable):
    profil["pesees"] = profil["pesees"][-(MAX_PESEES - 1):] + [{
        "f": famille(sport), "s": arrondi(sueur / max(0.2, r_effort), 0.001), "at": ressenti, "fiable": fiable,
        "sueur": arrondi(sueur, 0.01), "d": int(time.time())}]


def reference_apres_pesee(sport, r):
    cal = calibration(famille(sport))
    r.update(reference=arrondi(cal["s_base"], 0.01), nb_pesees=cal["pesees"], at_ref=arrondi(cal["at_ref"], 0.1))


def pesee_manuelle(recu):
    """Même calcul que peseeManuelle() : une pesée faite avant d'avoir le boîtier."""
    c = normaliser(recu)
    lire = lambda k: float(recu.get(k) or 0)
    avant, apres, heures, bu = lire("poids_avant"), lire("poids_apres"), borner(lire("duree"), 0, 1440) / 60, borner(lire("bu"), 0, 10000) / 1000
    fc = lire("fc_moy")
    p = lire("puissance_moy") if c["sport"] == "velo" else 0
    allure = borner(entier(recu.get("allure")), 0, 1200) if c["sport"] == "course" else 0
    r = {"sport": c["sport"], "bu": round(bu * 1000)}
    if heures < 0.5 or not (30 <= avant <= 200 and 30 <= apres <= 200):
        r["erreur"] = "il faut une sortie d'au moins 30 min et des poids entre 30 et 200 kg"
        return r
    sueur = (avant - apres + bu) / heures
    r["sueur"] = arrondi(sueur, 0.01)
    if not 0.1 <= sueur <= 3.5:
        r["erreur"] = "valeur hors de la plage possible (0,1 à 3,5 L/h)"
        return r
    repos, fmax = profil["fc_repos"], profil["fc_max"]
    r_fc = (fc - repos) / (fmax - repos) / I_REF if fc > 0 else 0
    r_p = p / puissance_ref() if p > 0 else 0
    r_a = allure_ref() / allure if allure > 0 and allure_ref() > 0 else 0
    if r_p and r_fc: re = 0.6 * r_p + 0.4 * r_fc
    elif r_p: re = r_p
    elif r_a and r_fc: re = 0.5 * r_fc + 0.5 * r_a
    elif r_a: re = r_a
    elif r_fc: re = r_fc
    else: re = intensite_seance(c["seance"]) / I_REF
    re = borner(re, R_EFFORT_MIN, R_EFFORT_MAX)
    r["r_effort"] = arrondi(re, 0.01)
    ajouter_point_pesee(c["sport"], sueur, re, c["ressenti"], not recu.get("hors"))
    reference_apres_pesee(c["sport"], r)
    r["enregistree"] = True
    return r


def corriger_derniere_sortie(km, bu=-1):
    """Bilan corrigé à la fin (distance en km, eau bue en mL, < 0 = inchangé) : comme corrigerDerniereSortie()."""
    if not historique:
        return
    h = historique[-1]
    r = h.get("resume")
    if km >= 0:
        h.update(km=round(km, 1), distance_saisie=True)
        if r:
            r.update(km=round(km, 1), distance_saisie=True)
            if h["duree"]:
                r["totaux"]["vitesse_moy"] = round(km / (h["duree"] / 3600), 1)
    if bu >= 0:
        h.update(bu=round(bu), bu_saisi=True)
        if r:
            t = r["totaux"]
            t["bu"] = round(bu)
            t["perte_poids"] = round((t["perdue"] - bu) / 1000 / r.get("poids", 70) * 100, 1)
            r["bu_saisi"] = True


def resume_complet():
    """Résumé avec les courbes (au plus 240 points) et les alertes, comme resumeComplet() dans sortie.cpp."""
    pts = s["points"][::len(s["points"]) // 240 + 1]
    fc_moy = round(s["fc_somme"] / s["duree_cardio"]) if s["duree_cardio"] else 0
    p_moy = round(s["p_somme"] / s["duree_puissance"]) if s["duree_puissance"] else 0
    return {"sport": s["sport"], "seance": s["seance"], "duree": s["duree"], "km": round(s["km"], 1),
            "ressenti": s["ressenti"], "poids": profil["poids"], "debut": s["debut"],
            "allure_prevue": s["allure"], "distance_prevue": s.get("distance_prevue", 0),
            "mesures": {"fc": s["duree_cardio"] > 0, "puissance": s["duree_puissance"] > 0, "vitesse": s["duree_vitesse"] > 0},
            "totaux": {"perdue": round(s["perdue"]), "bu": round(s["bu"]), "a_boire": round(s["a_boire_cumul"]),
                       "perte_poids": arrondi((s["perdue"] - s["bu"]) / 1000 / profil["poids"] * 100, 0.1),
                       "sodium_perdu": round(s["sodium_perdu"]), "sodium_apporte": round(s["sodium_apporte"]),
                       "glucides": round(s["glucides"]), "rappels": s["rappels"], "fc_moy": fc_moy, "p_moy": p_moy,
                       "vitesse_moy": arrondi(s["km"] / (s["duree"] / 3600), 0.1) if s["duree"] else 0,
                       "part": arrondi(s["part"], 0.01)},
            "serie": {k: [pt[k] for pt in pts] for k in ("t", "fc", "v", "p", "sueur", "perdue", "a_boire", "bu", "phase")},
            "gorgees": s["gorgees"], "bidons": [dict(b, bu=round(b["bu"])) for b in s["bidons"]]}


def simuler_en_entier():
    """Fait avancer la sortie jusqu'à sa durée prévue (1 h sinon), la termine et renvoie le résumé."""
    s["pause"] = False
    fin = max(s["duree_prevue"] * 60 if s["duree_prevue"] else 3600, s["duree"])
    tick(fin - s["duree"])
    s["alerte"] = None
    r = resume_complet()
    terminer()
    return r


def terminer():
    global s
    r_moy = arrondi(s["r_effort_somme"] / s["duree"], 0.01) if s["duree"] else 1
    fc_moy = round(s["fc_somme"] / s["duree_cardio"]) if s["duree_cardio"] else 0
    p_moy = round(s["p_somme"] / s["duree_puissance"]) if s["duree_puissance"] else 0
    dernier_resume.clear()
    dernier_resume.update(debut=s["debut"], duree=s["duree"], sport=s["sport"], seance=s["seance"], bu=round(s["bu"]),
                          objectif=objectif(), perdue=round(s["perdue"]), rappels=s["rappels"], fc_moy=fc_moy,
                          r_effort_moy=r_moy, pesee=s["pesee"])
    if s["ressenti"] is not None:
        dernier_resume["ressenti"] = s["ressenti"]
    if s["pesee"]:
        dernier_resume.update(poids_avant=s["poids_avant"], capacite=sum(b["volume"] for b in s["bidons"]))
    if reglages["consent_historique"]:
        historique.append({"debut": s["debut"], "duree": s["duree"], "sport": s["sport"], "seance": s["seance"],
                           "ressenti": s["ressenti"], "km": round(s["km"], 1), "bu": round(s["bu"]),
                           "objectif": objectif(), "a_boire": round(s["a_boire_cumul"]), "perdue": round(s["perdue"]),
                           "sodium_perdu": round(s["sodium_perdu"]), "sodium_apporte": round(s["sodium_apporte"]),
                           "glucides": round(s["glucides"]), "fc_moy": fc_moy, "p_moy": p_moy, "r_effort_moy": r_moy, "part": s["part"],
                           "rappels": s["rappels"], "gorgees": s["gorgees"],
                           "bidons": [dict(b, bu=round(b["bu"])) for b in s["bidons"]], "resume": resume_complet()})
    if s["sport"] == "velo" and profil["ftp"] <= 0 and s["duree_puissance"] >= 1200 and profil["p_ref_n"] < 3:
        n = profil["p_ref_n"]
        profil["p_ref"] = round((profil["p_ref"] * n + p_moy) / (n + 1))
        profil["p_ref_n"] = n + 1
    s = None


def pesee(recu):
    """Même calcul que sortiePesee() : un point de calibration ajouté au profil."""
    if not dernier_resume.get("pesee"):
        return None
    avant, apres = dernier_resume["poids_avant"], float(recu.get("poids_apres") or 0)
    heures = dernier_resume["duree"] / 3600
    reste = recu.get("reste") if recu.get("reste") in ("vides", "quart", "moitie", "pleins") else "vides"
    fraction = {"quart": 0.25, "moitie": 0.5, "pleins": 1.0}.get(reste, 0)
    bu = dernier_resume["capacite"] / 1000 * (1 - fraction)
    if isinstance(recu.get("bu"), (int, float)):
        bu = borner(recu["bu"], 0, 10000) / 1000  # eau bue donnée en mL
    hors = bool(recu.get("hors"))
    r = {"poids_avant": avant, "poids_apres": apres, "bu": round(bu * 1000), "hors": hors}
    if heures < 0.5 or not 30 <= apres <= 200:
        r["erreur"] = "sortie trop courte ou poids invalide"
        return r
    sueur = (avant - apres + bu) / heures
    r["sueur"] = arrondi(sueur, 0.01)
    if not 0.1 <= sueur <= 3.5:
        r["erreur"] = "valeur hors de la plage possible (0,1 à 3,5 L/h)"
        return r
    ajouter_point_pesee(dernier_resume["sport"], sueur, dernier_resume["r_effort_moy"], dernier_resume.get("ressenti"), not hors)
    r["r_effort"] = dernier_resume["r_effort_moy"]
    reference_apres_pesee(dernier_resume["sport"], r)
    r["enregistree"] = True
    dernier_resume["pesee"] = False
    return r


# ---------- Serveur ----------

class Gestion(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def repondre(self, donnees, type_="application/json"):
        corps = donnees if isinstance(donnees, bytes) else json.dumps(donnees).encode()
        self.send_response(200)
        self.send_header("Content-Type", type_)
        self.send_header("Content-Length", str(len(corps)))
        self.end_headers()
        self.wfile.write(corps)

    def config(self):
        self.repondre({"profil": profil, "reglages": reglages, "courses": courses, "calibration": calibration_etat()})

    def do_GET(self):
        chemin = urlparse(self.path).path
        if chemin == "/":
            return self.repondre(PAGE.read_bytes(), "text/html; charset=utf-8")
        if chemin == "/api/config":
            return self.config()
        if chemin == "/api/historique":
            return self.repondre(historique)
        if chemin == "/api/retours":
            return self.repondre(retours)
        if chemin == "/api/etat":
            seconde_reelle()
            etat = {"active": bool(s), "pause": bool(s and s["pause"]), "batterie": 100, "commandes": commandes,
                    "acceleration": acceleration}
            if s:
                etat.update(etat_sortie())
            return self.repondre(etat)
        self.send_error(404)

    def do_POST(self):
        global s, acceleration
        url = urlparse(self.path)
        params = parse_qs(url.query)
        longueur = int(self.headers.get("Content-Length") or 0)
        corps = json.loads(self.rfile.read(longueur) or b"{}") if longueur else {}
        chemin = url.path
        if chemin == "/api/profil":
            for k, v in corps.items():
                if k in PROFIL and k not in ("pesees", "p_ref", "p_ref_n") and type(v) is type(PROFIL[k]) or (
                        k in ("ftp", "allure_ref", "poids", "taille", "fc_repos", "fc_max") and isinstance(v, (int, float))):
                    profil[k] = v
            if profil["sel"] not in ("oui", "non"):
                profil["sel"] = "nsp"
            return self.config()
        if chemin == "/api/pesee":
            return self.repondre(pesee_manuelle(corps))
        if chemin == "/api/sortie/bilan":
            dernier_bilan.clear()
            bu = corps.get("bu") if isinstance(corps.get("bu"), (int, float)) and 0 <= corps["bu"] <= 10000 else -1
            km = corps.get("distance") if isinstance(corps.get("distance"), (int, float)) and 0 <= corps["distance"] <= 500 else -1
            corriger_derniere_sortie(km, bu)
            if bu >= 0:
                dernier_bilan["bu_reel"] = round(bu)
            if km >= 0:
                dernier_bilan["distance_reelle"] = round(km, 1)
            if isinstance(corps.get("pesee"), dict):
                r = pesee(dict(corps["pesee"], **({"bu": bu} if bu >= 0 else {})))
                if r:
                    dernier_bilan["pesee"] = r
            return self.repondre(dict(dernier_bilan))
        if chemin == "/api/sortie/meteo":
            if s:
                r = corps.get("ressenti")
                s["ressenti"] = borner(round(r), -20, 50) if isinstance(r, (int, float)) and not isinstance(r, bool) else None
            return self.repondre({"ok": True})
        if chemin == "/api/sortie/vitesse":
            x = entier(corps.get("x"), 1)
            acceleration = 16 if x >= 16 else 8 if x >= 8 else 4 if x >= 4 else 2 if x >= 2 else 1
            return self.repondre({"ok": True})
        if chemin == "/api/pesees/effacer":
            profil["pesees"] = []
            return self.config()
        if chemin == "/api/reglages":
            reglages.update({k: v for k, v in corps.items() if k in reglages})
            return self.config()
        if chemin in ("/api/seance", "/api/bidons"):
            cles = ("sport", "seance", "duree", "distance", "ressenti", "allure", "vitesse", "denivele", "detail") if chemin == "/api/seance" else ("bidons",)
            fusion = dict(reglages["derniere_config"], **{k: corps[k] for k in cles if k in corps})
            reglages["derniere_config"] = normaliser(fusion)
            return self.repondre(reglages["derniere_config"])
        if chemin == "/api/prevision":
            return self.repondre(prevoir(normaliser(corps)))
        if chemin == "/api/courses":
            courses.append(dict(id=max([c["id"] for c in courses] + [0]) + 1, nom=corps.get("nom", "Ma séance"), **normaliser(corps)))
            return self.config()
        if chemin == "/api/courses/supprimer":
            if True:
                courses[:] = [c for c in courses if c["id"] != int(params["id"][0])]
            return self.config()
        if chemin == "/api/commandes":
            commandes.update({k: 1 if v else 0 for k, v in corps.items() if k in commandes})
            return self.repondre(commandes)
        if chemin == "/api/sortie/simuler":
            if not s:
                self.send_error(409)
                return
            return self.repondre(simuler_en_entier())
        if chemin == "/api/sortie/demarrer" and not s:
            c = normaliser(corps)
            reglages["derniere_config"] = dict(c, pesee=False, poids_avant=0)
            s = demarrer(c)
            acceleration = 1
        elif chemin == "/api/sortie/pause" and s:
            s["pause"] = not s["pause"]
        elif chemin == "/api/sortie/terminer" and s:
            terminer()
            acceleration = 1
        elif chemin == "/api/historique/effacer":
            historique.clear()
        elif chemin == "/api/retour":
            def choix(v, valeurs):
                return v if v in valeurs else valeurs[0]
            retour = {"date": int(time.time()),
                      "a_bu": choix(corps.get("a_bu"), ["", "oui", "en_partie", "non"]),
                      "raisons": [r for r in corps.get("raisons", []) if r in
                                  ("alerte_ratee", "pas_pratique", "pas_soif", "plus_de_boisson", "trop_d_alertes", "autre")][:6],
                      "raison_autre": str(corps.get("raison_autre", ""))[:200],
                      "dosage": choix(corps.get("dosage"), ["", "bien", "trop_souvent", "pas_assez", "quantites"]),
                      "remarques": str(corps.get("remarques", ""))[:1000]}
            if dernier_resume:
                retour["sortie"] = dict(dernier_resume)
            km = float(corps.get("distance") or 0)
            if 0 < km < 500:
                retour["distance_reelle"] = round(km, 1)
                corriger_derniere_sortie(km)
            if isinstance(corps.get("pesee"), dict):
                r = pesee(corps["pesee"])
                if r:
                    retour["pesee"] = r
            for k, v in dernier_bilan.items():
                retour.setdefault(k, v)
            dernier_bilan.clear()
            retours.append(retour)
            return self.repondre({"ok": True, **({"pesee": retour["pesee"]} if "pesee" in retour else {})})
        elif chemin == "/api/retours/effacer":
            retours.clear()
        elif chemin == "/test/avancer" and s:
            tick(int(params["s"][0]))
        return self.repondre({"ok": True})


if __name__ == "__main__":
    PAGE = Path(sys.argv[1] if len(sys.argv) > 1 else "web/index.html")
    PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    print(f"Faux boîtier sur http://localhost:{PORT}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Gestion).serve_forever()
