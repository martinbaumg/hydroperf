"""Tests de l'algorithme HydroPerf v2 (version Python du faux boîtier, identique à sortie.cpp).

    python3 outils/test_algo.py

Chaque test reprend un exemple des documents ou une sortie type ; le script s'arrête au premier échec.
"""
import copy
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import faux_boitier as fb  # noqa: E402

PROFIL = copy.deepcopy(fb.PROFIL)


def reinitialiser(**profil):
    fb.profil.clear()
    fb.profil.update(copy.deepcopy(PROFIL), **profil)
    fb.reglages.update(auto=True, gorgee=30)
    fb.s = None
    random.seed(1)


def proche(a, b, tol=0.01):
    assert abs(a - b) <= tol, f"{a} au lieu de {b}"


def test_exemple_du_document():
    """Cycliste 70 kg, FTP 250 W, R_effort 1,18, 28 °C ressentis, sortie de 2 h : 0,93 L/h et 0,47 L/h à boire."""
    reinitialiser(poids=70, ftp=250)
    cal = fb.calibration("velo")
    proche(cal["s_base"], 0.70)
    sueur = cal["s_base"] * 1.18 * fb.r_meteo(28, cal)
    proche(sueur, 0.93)
    proche(fb.part_a_compenser(sueur, 120), 0.50)
    proche(fb.part_a_compenser(sueur, 240), 0.72)
    proche(min(fb.part_a_compenser(sueur, 120) * sueur, fb.plafond(cal)), 0.47)
    assert fb.plan_pour(0.47 * 1000 / 60) == (8, 2), fb.plan_pour(0.47 * 1000 / 60)


def test_sodium():
    for reponse, attendu in (("nsp", 830), ("non", 600), ("oui", 1100)):
        reinitialiser(sel=reponse)
        assert fb.sodium_sueur() == attendu
    assert fb.sodium_boisson({"type": "isotonique", "sel": False, "sodium": -1}) == 600
    assert fb.sodium_boisson({"type": "electrolyte", "sel": False, "sodium": -1}) == 900
    assert fb.sodium_boisson({"type": "maison", "sel": True, "sodium": -1}) == 600
    assert fb.sodium_boisson({"type": "isotonique", "sel": False, "sodium": 450}) == 450


def test_calibration_k():
    """Exemple du document : 0,80 L/h à 18 °C et 1,05 L/h à 28 °C donnent k = 0,027."""
    reinitialiser(pesees=[{"f": "course", "s": 0.80, "at": 18, "fiable": True},
                          {"f": "course", "s": 1.05, "at": 28, "fiable": True}])
    cal = fb.calibration("course")
    proche(cal["k"], 0.027, 0.001)
    assert cal["k_personnel"] and cal["at_ref"] == 18 and cal["pesees"] == 2
    proche(cal["s_base"], 0.80, 0.005)
    assert fb.plafond(cal) == 1.0
    # Moins de 8 °C d'écart : on garde 0,03
    reinitialiser(pesees=[{"f": "course", "s": 0.80, "at": 20, "fiable": True},
                          {"f": "course", "s": 0.95, "at": 25, "fiable": True}])
    assert fb.calibration("course")["k"] == 0.03


def test_sans_meteo_ni_duree():
    reinitialiser()
    cal = fb.calibration("course")
    assert fb.r_meteo(None, cal) == 1
    assert fb.part_a_compenser(0.9, 0) == fb.PART_SANS_DUREE


def sortie(config, secondes):
    fb.s = fb.demarrer(fb.normaliser(config))
    fb.tick(secondes)
    return fb.s


def test_footing_sans_capteur():
    reinitialiser(capteur_cardio=False)
    s = sortie({"sport": "course", "seance": "footing", "duree": 60, "bidons": [{"volume": 500, "type": "eau"}]}, 3600)
    assert s["source_effort"] == "seance"
    # Les alertes respectent 5 à 20 min d'écart et 1 à 3 gorgées
    temps = [g[0] for g in s["gorgees"]]
    ecarts = [b - a for a, b in zip([0] + temps, temps)]
    assert all(300 <= e <= 1200 for e in ecarts), ecarts
    assert all(1 <= g[1] / 30 <= 3 for g in s["gorgees"])
    proche(s["bu"] / 1000, s["a_boire_cumul"] / 1000, 0.07)


def test_fractionne_detecte_avec_le_gps():
    """3 × 3 000 m : les fractions sont détectées par la vitesse, malgré un échauffement plus court que prévu."""
    reinitialiser(capteur_gps=True, allure_ref=330)
    config = {"sport": "course", "seance": "fractionne", "ressenti": 20,
              "detail": {"echauffement_min": 20, "repetitions": 3, "effort_m": 3000, "allure_effort": 240,
                         "recup_s": 180, "retour_calme_min": 10},
              "bidons": [{"volume": 500, "type": "eau"}]}
    fb.s = fb.demarrer(fb.normaliser(config))
    fb.s["decalage"] = -180
    phases = []
    alertes_en_effort = 0
    for _ in range(fb.duree_detail(fb.s["detail"])):
        avant = fb.s["rappels"]
        fb.tick()
        if fb.s["phase"] and (not phases or phases[-1] != fb.s["phase"]):
            phases.append(fb.s["phase"])
        if fb.s["rappels"] > avant and fb.s["phase"] == "effort":
            alertes_en_effort += 1
    assert phases == ["echauffement", "effort", "recup", "effort", "recup", "effort", "calme"], phases
    # Les fractions durent 12 min : la récupération arrive avant 15 min, aucune alerte pendant l'effort
    assert alertes_en_effort == 0, alertes_en_effort


def test_choix_du_bidon():
    """La première heure : eau ; ensuite, le bidon salé revient régulièrement."""
    reinitialiser()
    s = sortie({"sport": "course", "seance": "longue", "duree": 150, "ressenti": 25,
                "bidons": [{"volume": 750, "type": "eau"}, {"volume": 750, "type": "isotonique"}]}, 150 * 60)
    premiere_heure = {g[2] for g in s["gorgees"] if g[0] < 3600}
    ensuite = [g[2] for g in s["gorgees"] if g[0] >= 3600]
    assert premiere_heure == {0}, premiere_heure
    assert 1 in ensuite, ensuite


def test_pesee():
    reinitialiser(poids=70)
    s = sortie({"sport": "course", "seance": "longue", "duree": 60, "ressenti": 22, "pesee": True, "poids_avant": 70,
                "bidons": [{"volume": 500, "type": "eau"}]}, 3600)
    fb.terminer()
    r = fb.pesee({"poids_apres": 69.6, "reste": "moitie", "hors": False})
    proche(r["sueur"], 0.65)
    assert r["enregistree"] and len(fb.profil["pesees"]) == 1
    assert fb.pesee({"poids_apres": 69.6}) is None  # une seule pesée par sortie


def test_seances_et_durees():
    """Séances propres au vélo, durée avec la vitesse, l'allure ou le dénivelé."""
    reinitialiser(allure_ref=330)
    c = fb.normaliser({"sport": "velo", "seance": "longue", "distance": 60, "vitesse": 30})
    assert c["seance"] == "endurance" and fb.duree_estimee(c) == 120
    assert fb.normaliser({"sport": "course", "seance": "tempo"})["seance"] == "longue"
    assert fb.duree_estimee(fb.normaliser({"sport": "course", "seance": "longue", "distance": 10, "allure": 300})) == 50
    # Trail : 20 km et 1 000 m D+ = 30 km-effort ; à 6:00 par km-effort, 3 h
    assert fb.duree_estimee(fb.normaliser({"sport": "trail", "seance": "longue", "distance": 20, "denivele": 1000,
                                           "allure": 360})) == 180
    # La durée donnée passe avant la distance
    assert fb.duree_estimee(fb.normaliser({"sport": "course", "duree": 45, "distance": 10, "allure": 300})) == 45


def test_glucides():
    assert fb.glucides_boisson({"type": "isotonique", "sucre": False, "glucides": -1}) == 60
    assert fb.glucides_boisson({"type": "isotonique", "sucre": False, "glucides": 80}) == 80
    assert fb.glucides_boisson({"type": "electrolyte", "sucre": False, "glucides": -1}) == 0


if __name__ == "__main__":
    tests = [(nom, f) for nom, f in list(globals().items()) if nom.startswith("test_")]
    for nom, f in tests:
        f()
        print(f"ok  {nom}")
    print(f"{len(tests)} tests réussis")
