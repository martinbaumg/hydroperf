"""Faux boîtier HydroPerf : sert web/index.html avec la même API que l'ESP32, pour tester l'interface sans la carte.

    python3 outils/faux_boitier.py web/index.html 8765
    puis ouvrir http://localhost:8765

Outil de test : POST /test/avancer?s=600 fait avancer la sortie en cours de 600 secondes.
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

PAGE = Path(sys.argv[1] if len(sys.argv) > 1 else "web/index.html")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765

CONFIG = {"sport": "velo", "seance": "longue", "duree": 120, "distance": 50,
          "bidons": [{"volume": 750, "type": "eau", "sucre": False, "sel": False},
                     {"volume": 500, "type": "electrolyte", "sucre": False, "sel": False}]}
profil = {"poids": 68, "taille": 178, "fc_repos": 52, "fc_max": 189, "sueur": 0.8, "sueur_mesuree": False,
          "sexe": "NC", "sel": "moyenne"}
reglages = {"fixation": "poignet", "alerte_vibreur": True, "alerte_buzzer": False, "intensite_vibreur": 80,
            "intensite_buzzer": 60, "duree_ecran": 10, "simulation_meteo": "auto", "source_cardio": "simule",
            "auto": True, "intervalle": 12, "gorgee": 30, "consent_historique": True, "consent_sante": False,
            "consent_anonyme": False, "unites": "metrique", "theme": "auto", "notif_telephone": True,
            "derniere_config": copy.deepcopy(CONFIG)}
courses = [dict(id=1, nom="Vélo du dimanche", **copy.deepcopy(CONFIG)),
           dict(id=2, nom="Footing du midi", sport="course", seance="footing", duree=45, distance=8,
                bidons=[{"volume": 500, "type": "eau", "sucre": False, "sel": False}])]
historique = []
retours = []          # questionnaire de fin de sortie
dernier_resume = {}   # résumé de la dernière sortie terminée, joint au retour
commandes ={"led": 0, "vibreur": 0, "buzzer": 0}
s = None

INTENSITE = {"footing": 0.55, "longue": 0.65, "fractionne": 0.75}
SODIUM_SUEUR = {"faible": 500, "moyenne": 900, "forte": 1400}
METEO = {"auto": (24, 55), "frais": (12, 70), "doux": (19, 55), "chaud": (29, 50), "canicule": (36, 40)}


def meteo_disponible():
    return reglages["simulation_meteo"] != "aucune"


def cardio_disponible():
    return reglages["source_cardio"] != "aucun"


def meteo():
    """Météo simulée, ou None quand il n'y a pas de météo."""
    if not meteo_disponible():
        return None
    t, h = METEO.get(reglages["simulation_meteo"], METEO["auto"])
    r = t
    if t >= 27:
        r = (-8.78469 + 1.61139 * t + 2.33855 * h - 0.146116 * t * h - 0.0123081 * t * t - 0.0164248 * h * h
             + 0.00221173 * t * t * h + 0.00072546 * t * h * h - 0.000003582 * t * t * h * h)
    return {"temp": t, "humidite": h, "ressenti": round(r, 1), "scenario": reglages["simulation_meteo"]}


def chaleur():
    """Effet de la météo sur la sueur : 1 (aucun effet) sans météo."""
    m = meteo()
    return min(1.6, max(0.8, 1 + 0.03 * (m["ressenti"] - 24))) if m else 1


def estimer_sueur():
    facteur = {"F": 0.85, "M": 1.0}.get(profil["sexe"], 0.93)
    return round(0.9 * profil["poids"] / 70 * facteur, 1)


def plan(transpi):
    """Même algorithme que calculerPlan() dans src/hydroperf/sortie.cpp."""
    gorgee = reglages["gorgee"]
    besoin = max(50, 0.8 * transpi * 1000)
    if not reglages["auto"]:
        i = reglages["intervalle"]
        return i, max(1, min(8, round(besoin * i / 60 / gorgee)))
    for g in (1, 2, 3):
        i = 60 * g * gorgee / besoin
        if i >= 5:
            return round(min(i, 20)), g
    return 5, 3


def vitesse(sport, seance):
    return {"velo": 25, "trail": 7}.get(sport, 10) * {"footing": 0.9, "fractionne": 1.1}.get(seance, 1)


def sodium_boisson(b):
    return {"electrolyte": 700, "isotonique": 450}.get(b["type"], 600 if b["type"] == "maison" and b["sel"] else 0)


def normaliser(c):
    bidons = [{"volume": max(100, min(2000, int(b.get("volume", 500)))), "type": b.get("type", "eau"),
               "sucre": bool(b.get("sucre")), "sel": bool(b.get("sel"))} for b in c.get("bidons", [])[:3]]
    return {"sport": c.get("sport", "course"), "seance": c.get("seance", "footing"), "duree": int(c.get("duree", 0)),
            "distance": float(c.get("distance", 0)),
            "bidons": bidons or [{"volume": 500, "type": "eau", "sucre": False, "sel": False}]}


def prevoir(c):
    transpi = profil["sueur"] * INTENSITE[c["seance"]] / 0.65 * chaleur()
    par_heure = round(0.8 * transpi * 1000 / 50) * 50
    duree = c["duree"] or (round(c["distance"] / vitesse(c["sport"], c["seance"]) * 60) if c["distance"] else 0)
    intervalle, gorgees = plan(transpi)
    return {"transpiration": round(transpi, 1), "par_heure": par_heure, "duree_estimee": duree,
            "objectif": round(par_heure * duree / 60 / 50) * 50 if duree else 0, "intervalle": intervalle,
            "gorgees": gorgees, "capacite": sum(b["volume"] for b in c["bidons"]),
            "perte": round(transpi * 1000 * duree / 60), "sodium": round(transpi * duree / 60 * SODIUM_SUEUR[profil["sel"]])}


def fc_moyenne():
    """Fréquence cardiaque moyenne sur le temps passé avec la montre (0 sans montre)."""
    return round(s["fc_somme"] / s["duree_cardio"]) if s["duree_cardio"] else 0


def objectif():
    perte = round(0.8 * s["perdue"] / 50) * 50
    return max(s["objectif_initial"], perte) if s["duree_prevue"] else perte


def tick(secondes=1):
    for _ in range(secondes):
        if not s or s["pause"]:
            return
        s["duree"] += 1
        cible = INTENSITE[s["seance"]]
        intensite = 0.15 + (cible - 0.15) * min(1, s["duree"] / 600)
        if cardio_disponible():
            # Montre simulée : effort mesuré par le cœur
            s["fc"] = profil["fc_repos"] + intensite * (profil["fc_max"] - profil["fc_repos"]) + random.uniform(-2, 2)
            intensite = (s["fc"] - profil["fc_repos"]) / (profil["fc_max"] - profil["fc_repos"])
            s["fc_somme"] += s["fc"]
            s["duree_cardio"] += 1
        else:
            # Pas de montre : effort estimé avec le type de séance
            s["fc"] = None
        s["transpi"] = max(0.1, profil["sueur"] * intensite / 0.65 * chaleur())
        s["perdue"] += s["transpi"] * 1000 / 3600
        s["sodium_perdu"] += s["transpi"] / 3600 * SODIUM_SUEUR[profil["sel"]]
        s["km"] += vitesse(s["sport"], s["seance"]) / 3600
        if s["alerte"]:
            s["alerte"]["restant"] -= 1
            if s["alerte"]["restant"] <= 0:
                s["alerte"] = None
        s["rappel"] -= 1
        if s["rappel"] <= 0:
            secondes_depuis = s["duree"] - s["duree_alerte"]
            transpi = (s["perdue"] - s["perdue_alerte"]) / 1000 / (secondes_depuis / 3600) if secondes_depuis >= 60 else s["transpi"]
            s["plan"]["intervalle"], s["plan"]["gorgees"] = plan(transpi)
            s["perdue_alerte"], s["duree_alerte"] = s["perdue"], s["duree"]
            nb = s["plan"]["gorgees"]
            m = meteo()
            sel = (s["duree"] >= 2700 or (m is not None and m["ressenti"] >= 27)) and s["rappels"] % 2 == 1
            libres = [i for i, b in enumerate(s["bidons"]) if b["volume"] - b["bu"] > 0]
            choix = next((i for i in libres if (s["bidons"][i]["type"] != "eau") == sel), libres[0] if libres else -1)
            if choix >= 0:
                b = s["bidons"][choix]
                ml = min(nb * reglages["gorgee"], b["volume"] - b["bu"])
                b["bu"] += ml
                s["bu"] += ml
                s["sodium_apporte"] += ml / 1000 * sodium_boisson(b)
                s["gorgees"].append([s["duree"], ml, choix])
            s["alerte"] = {"restant": reglages["duree_ecran"], "gorgees": nb, "bidon": choix}
            s["rappels"] += 1
            s["rappel"] = s["plan"]["intervalle"] * 60


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
        self.repondre({"profil": profil, "reglages": reglages, "courses": courses})

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
            tick()
            etat = {"active": bool(s), "pause": bool(s and s["pause"]), "meteo": meteo(), "batterie": 100,
                    "commandes": commandes,
                    "sources": {"effort": "cardio" if cardio_disponible() else "seance",
                                "meteo": "simulee" if meteo_disponible() else "aucune"}}
            if s:
                etat.update({k: s[k] for k in ("sport", "seance", "duree", "duree_prevue", "rappel", "rappels", "alerte", "plan")})
                etat.update(bu=round(s["bu"]), objectif=objectif(), perdue=round(s["perdue"]), sodium_perdu=round(s["sodium_perdu"]),
                            sodium_apporte=round(s["sodium_apporte"]), fc=round(s["fc"]) if s["fc"] is not None else None, transpi=round(s["transpi"], 1),
                            km=round(s["km"], 1), bidons=[dict(b, bu=round(b["bu"])) for b in s["bidons"]])
            return self.repondre(etat)
        self.send_error(404)

    def do_POST(self):
        global s
        url = urlparse(self.path)
        params = parse_qs(url.query)
        longueur = int(self.headers.get("Content-Length") or 0)
        corps = json.loads(self.rfile.read(longueur) or b"{}") if longueur else {}
        chemin = url.path
        if chemin == "/api/profil":
            profil.update({k: v for k, v in corps.items() if k in profil})
            if "sueur" in corps:
                profil["sueur_mesuree"] = True
            if not profil["sueur_mesuree"]:
                profil["sueur"] = estimer_sueur()
            return self.config()
        if chemin == "/api/reglages":
            reglages.update({k: v for k, v in corps.items() if k in reglages})
            return self.config()
        if chemin in ("/api/seance", "/api/bidons"):
            cles = ("sport", "seance", "duree", "distance") if chemin == "/api/seance" else ("bidons",)
            fusion = dict(reglages["derniere_config"], **{k: corps[k] for k in cles if k in corps})
            reglages["derniere_config"] = normaliser(fusion)
            return self.repondre(reglages["derniere_config"])
        if chemin == "/api/prevision":
            return self.repondre(prevoir(normaliser(corps)))
        if chemin == "/api/courses":
            courses.append(dict(id=max([c["id"] for c in courses] + [0]) + 1, nom=corps.get("nom", "Ma séance"), **normaliser(corps)))
            return self.config()
        if chemin == "/api/courses/supprimer":
            if len(courses) > 1:
                courses[:] = [c for c in courses if c["id"] != int(params["id"][0])]
            return self.config()
        if chemin == "/api/commandes":
            commandes.update({k: 1 if v else 0 for k, v in corps.items() if k in commandes})
            return self.repondre(commandes)
        if chemin == "/api/sortie/demarrer" and not s:
            c = normaliser(corps)
            reglages["derniere_config"] = c
            p = prevoir(c)
            s = {"debut": int(time.time()), "duree": 0, "sport": c["sport"], "seance": c["seance"], "pause": False,
                 "duree_prevue": p["duree_estimee"], "objectif_initial": p["objectif"], "bu": 0, "perdue": 0,
                 "sodium_perdu": 0, "sodium_apporte": 0, "km": 0, "fc": None, "fc_somme": 0, "duree_cardio": 0, "transpi": 0,
                 "rappel": p["intervalle"] * 60, "rappels": 0, "alerte": None, "gorgees": [],
                 "plan": {"intervalle": p["intervalle"], "gorgees": p["gorgees"]}, "perdue_alerte": 0, "duree_alerte": 0,
                 "bidons": [dict(b, bu=0) for b in c["bidons"]]}
        elif chemin == "/api/sortie/pause" and s:
            s["pause"] = not s["pause"]
        elif chemin == "/api/sortie/terminer" and s:
            dernier_resume.clear()
            dernier_resume.update(debut=s["debut"], duree=s["duree"], sport=s["sport"], seance=s["seance"],
                                  bu=round(s["bu"]), objectif=objectif(), perdue=round(s["perdue"]),
                                  rappels=s["rappels"], fc_moy=fc_moyenne())
            if reglages["consent_historique"]:
                historique.append({"debut": s["debut"], "duree": s["duree"], "sport": s["sport"], "seance": s["seance"],
                                   "km": round(s["km"], 1), "bu": round(s["bu"]), "objectif": objectif(),
                                   "perdue": round(s["perdue"]), "sodium_perdu": round(s["sodium_perdu"]),
                                   "sodium_apporte": round(s["sodium_apporte"]), "fc_moy": fc_moyenne(), "rappels": s["rappels"],
                                   "gorgees": s["gorgees"], "bidons": s["bidons"]})
            s = None
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
            retours.append(retour)
        elif chemin == "/api/retours/effacer":
            retours.clear()
        elif chemin == "/test/avancer" and s:
            tick(int(params["s"][0]))
        return self.repondre({"ok": True})


print(f"Faux boîtier sur http://localhost:{PORT}", flush=True)
ThreadingHTTPServer(("127.0.0.1", PORT), Gestion).serve_forever()
