# projet-couad

Prototype d'un rappel d'hydratation sur ESP32 : le boîtier signale qu'il faut boire (LED, vibreur, buzzer) et affiche sur un petit écran le nombre de gorgées à prendre et le type de boisson (eau, effort, énergie).

## Matériel

- ESP32 WROOM (carte `esp32dev`)
- LED rouge 3 mm + résistance
- Module moteur vibrant Grove
- Buzzer
- Écran Joy-IT 1,8" ST7735 (160×128, SPI)

Le schéma électrique et le calcul de la résistance sont dans [docs/sprint1_schema_electrique.pdf](docs/sprint1_schema_electrique.pdf).

## Organisation du code

```
web/
  index.html            Application HydroPerf (5 écrans), intégrée au programme de l'ESP32
src/
  hydroperf/            Application HydroPerf
    hydroperf.cpp         Wi-Fi, serveur web et API
    sortie.h / .cpp       Déroulement d'une sortie, données simulées, rappels (LED, vibreur, buzzer)
    stockage.h / .cpp     Profil, réglages, courses et historique enregistrés en flash
    brochage.h            Broches utilisées
  main.cpp              Sprint 1 : contrôle de la LED, du vibreur et du buzzer depuis une page web
  ecran.h / ecran.cpp   Affichage des gorgées et du type de boisson sur l'écran ST7735
  diag/diag_ecran.cpp   Programme de diagnostic de l'écran (câblage, mode SPI)
archives/
  ecran_demo.cpp        Démo qui fait défiler des alertes sur l'écran
  sprint1_controle_web.cpp  Version de la page web qui pilote aussi l'écran
```

## Compiler et téléverser

Avec [PlatformIO](https://platformio.org/) :

```sh
pio run -t upload -t monitor              # application HydroPerf (par défaut)
pio run -e sprint1 -t upload -t monitor   # sprint 1 : LED, vibreur, buzzer
pio run -e diag -t upload -t monitor      # diagnostic de l'écran
```

Après une modification de `web/index.html`, il suffit de renvoyer le programme : la page est intégrée dedans.

## Utilisation (HydroPerf)

1. Alimenter l'ESP32 : il crée le réseau Wi-Fi `HydroPerf`, ouvert (sans mot de passe). Pour en remettre un, changer `AP_PASSWORD` dans `src/hydroperf/hydroperf.cpp` (8 caractères minimum).
2. Se connecter à ce réseau depuis un téléphone et ouvrir http://hydroperf.local (ou http://192.168.4.1 si l'adresse ne répond pas, par exemple avec un « DNS privé » activé sur Android).
3. Onglet **Sortie** : choisir le sport, le type de séance, la durée et la distance (ou « On verra comment je me sens ») et les bidons (1 à 3, taille et contenu), puis « Démarrer ».

Pendant la sortie, l'ESP32 envoie une alerte à intervalle régulier (vibreur et/ou buzzer selon les réglages, LED, écran) et considère que l'athlète boit à chaque consigne. Les données de la montre et du téléphone (cœur, météo, distance) sont simulées : le Wi-Fi du boîtier n'a pas accès à Internet. L'eau et le sodium perdus sont estimés à partir du profil (transpiration, sueur plus ou moins salée), de la séance et de la météo.

L'onglet **Test** permet d'allumer le vibreur et le buzzer et d'afficher sur l'écran une alerte (1 à 3 barres, type de boisson) ou un message.

Le profil, les réglages, les séances enregistrées et l'historique des sorties sont enregistrés dans la flash de l'ESP32 : ils restent après un redémarrage et quand on renvoie le programme. Une sortie en cours est sauvegardée chaque minute et reprend (en pause) après une coupure. L'historique se télécharge en CSV depuis Réglages ; son enregistrement peut être désactivé (Réglages > Confidentialité).

Les textes « Politique de confidentialité » et « Avertissement médical » sont des brouillons : ils sont en haut de la partie « Textes légaux » du script de `web/index.html`, à remplacer par ceux du Drive.

## Utilisation (Sprint 1)

1. Envoyer le programme du sprint 1 (`pio run -e sprint1 -t upload`), puis alimenter l'ESP32 : il crée le réseau Wi-Fi `COUAD-ESP32` (mot de passe `couad2026`).
2. Se connecter à ce réseau depuis un téléphone ou un PC.
3. Ouvrir http://192.168.4.1 pour allumer ou éteindre la LED, le vibreur et le buzzer.

## Brochage

| Composant | Sprint 1 | HydroPerf |
|---|---|---|
| LED | GPIO 16 | GPIO 26 |
| Vibreur (SIG) | GPIO 4 | GPIO 4 |
| Buzzer | GPIO 17 | GPIO 17 |
| Écran CS | — | GPIO 5 |
| Écran DC | — | GPIO 16 |
| Écran RST | — | GPIO 25 |
| Écran SCK | — | GPIO 18 |
| Écran SDA/MOSI | — | GPIO 23 |
