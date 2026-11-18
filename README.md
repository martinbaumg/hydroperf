# HydroPerf

Prototype de boîtier de rappel d'hydratation pour le sport (course, trail, vélo), basé sur un ESP32.

Pendant une sortie, le boîtier estime l'eau perdue en transpirant et rappelle de boire au bon moment : il vibre et/ou bipe, allume une LED et affiche sur un petit écran le **nombre de gorgées** et la **boisson à prendre** (eau, électrolytes, isotonique ou boisson maison). On le règle depuis une application web servie par l'ESP32 lui-même : on se connecte à son Wi-Fi avec un téléphone, sans Internet.

## Sommaire

- [Matériel](#matériel)
- [Brochage](#brochage)
- [Organisation du dépôt](#organisation-du-dépôt)
- [Compiler et téléverser](#compiler-et-téléverser)
- [Utiliser HydroPerf](#utiliser-hydroperf)
- [Tester l'interface sans la carte](#tester-linterface-sans-la-carte)
- [Sprint 1](#sprint-1)
- [En cas de problème](#en-cas-de-problème)

## Matériel

| Élément | Modèle |
|---|---|
| Carte | ESP32 WROOM avec **16 Mo** de mémoire flash (carte `esp32dev` ; une FireBeetle ESP32 marche aussi) |
| Écran | OLED 0,96" SSD1306, 128×64, I2C (4 broches) |
| LED | LED rouge 3 mm + résistance |
| Vibreur | Module moteur vibrant Grove |
| Buzzer | Buzzer |

L'ancien écran couleur **ST7735 1,8"** (SPI) reste utilisable : voir [Changer d'écran](#changer-décran).

Le schéma électrique du sprint 1 et le calcul de la résistance sont dans [docs/sprint1_schema_electrique.pdf](docs/sprint1_schema_electrique.pdf).

## Brochage

| Composant | Broche de l'ESP32 |
|---|---|
| LED | GPIO 26 |
| Vibreur (SIG) | GPIO 4 |
| Buzzer | GPIO 17 |
| Écran OLED SDA | GPIO 21 |
| Écran OLED SCK | GPIO 22 |
| Écran OLED VDD | 3V3 (ou VCC sur une FireBeetle) |
| Écran OLED GND | GND |

Les broches de l'application sont définies dans `src/hydroperf/brochage.h`, et celles de l'écran en haut de `src/ecran_oled.cpp`.

<details>
<summary>Ancien écran ST7735 (SPI)</summary>

| Écran ST7735 | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL / SCK | GPIO 18 |
| SDA | GPIO 23 |
| CS | GPIO 5 |
| DC | GPIO 16 |
| RES | GPIO 25 |
| LED / BL (rétroéclairage) | GPIO 27 |

</details>

## Organisation du dépôt

```
hydroperf/
├── platformio.ini          Configuration de la compilation (cartes, bibliothèques, programmes)
├── src/                    Code qui tourne sur la carte
│   ├── hydroperf/          ★ L'application HydroPerf
│   │   ├── hydroperf.cpp     Démarrage, Wi-Fi, adresse hydroperf.local, serveur web et API
│   │   ├── sortie.h / .cpp   Algorithme d'hydratation : sortie en cours, alertes, choix du bidon,
│   │   │                     données simulées (cœur, météo, distance)
│   │   ├── stockage.h / .cpp Profil, réglages, séances et historique enregistrés dans la carte
│   │   ├── commandes.h / .cpp Pilotage de la LED, du vibreur et du buzzer
│   │   └── brochage.h        Broches utilisées
│   ├── ecran.h             Fonctions d'affichage communes aux deux écrans
│   ├── ecran_oled.cpp      ★ Affichage sur l'écran OLED (utilisé par HydroPerf)
│   ├── ecran.cpp           Affichage sur l'ancien écran ST7735
│   ├── main.cpp            Sprint 1 : LED, vibreur et buzzer pilotés depuis une page web
│   ├── diag/               Diagnostic de l'écran ST7735 (ESP32, XIAO nRF52840)
│   ├── diag_uno/           Diagnostic de l'écran ST7735 (Arduino UNO, ESP8266)
│   ├── test_uno/           Test minimal de l'écran ST7735 (Arduino UNO, ESP8266)
│   └── test_oled/          Test de l'écran OLED, avec vérification du câblage (ESP32, ESP8266)
├── web/                    Application web, intégrée au programme de la carte
│   ├── index.html          ★ L'application (Accueil, Sortie, Profil, Réglages, Test)
│   └── icones/             Favicon et icône d'écran d'accueil (iPhone, Android)
├── docs/                   Documents
│   └── sprint1_schema_electrique.pdf       Schéma électrique du sprint 1
├── outils/
│   └── faux_boitier.py     Imite la carte sur l'ordinateur pour tester l'interface
└── archives/               Anciennes versions, gardées pour mémoire (non compilées)
    ├── hydroperf_v1.html, hydroperf_avec_bilan.html   Anciennes versions de l'application
    ├── sprint1_controle_web.cpp                       Page du sprint 1 qui pilotait aussi l'écran
    └── ecran_demo.cpp                                 Démo qui fait défiler des alertes à l'écran
```

Les fichiers marqués ★ sont ceux à modifier le plus souvent.

## Compiler et téléverser

Le projet utilise [PlatformIO](https://platformio.org/), soit l'extension de VS Code, soit en ligne de commande. Sans installation, on peut aussi lancer PlatformIO avec `uvx --from platformio pio` à la place de `pio`.

```sh
pio run -t upload -t monitor              # application HydroPerf (programme par défaut)
```

`platformio.ini` contient un programme (« environnement ») par usage :

| Environnement | Carte | À quoi il sert |
|---|---|---|
| `hydroperf` | ESP32 | **L'application HydroPerf** |
| `sprint1` | ESP32 | Sprint 1 : LED, vibreur et buzzer depuis une page web |
| `test_oled` | ESP32 | Tester l'écran OLED et son câblage |
| `test_oled_esp8266` | ESP8266 | Tester l'écran OLED sur un ESP8266 |
| `diag` | ESP32 | Diagnostic de l'écran ST7735 |
| `diag_xiao` | XIAO nRF52840 | Diagnostic de l'écran ST7735 |
| `diag_uno` / `diag_esp8266` | UNO / ESP8266 | Diagnostic de l'écran ST7735 |
| `test_uno` / `test_esp8266` | UNO / ESP8266 | Test minimal de l'écran ST7735 |

Pour en lancer un : `pio run -e <environnement> -t upload -t monitor`.

La page web et les icônes sont **intégrées au programme** : après une modification de `web/`, il suffit de renvoyer le programme. Les données enregistrées (profil, réglages, historique) ne sont pas effacées par un nouvel envoi.

## Utiliser HydroPerf

1. **Allumer le boîtier.** L'ESP32 crée le réseau Wi-Fi `HydroPerf`, ouvert (sans mot de passe). Pour en ajouter un, changer `AP_PASSWORD` dans `src/hydroperf/hydroperf.cpp` (8 caractères minimum).
2. **Se connecter à ce réseau** avec un téléphone et ouvrir **http://hydroperf.local**. Si l'adresse ne répond pas (par exemple avec un « DNS privé » activé sur Android), utiliser **http://192.168.4.1**.
3. *(Facultatif)* Dans Safari sur iPhone : Partager → **Sur l'écran d'accueil**, pour avoir HydroPerf comme une application.
4. **Onglet Sortie :** choisir le sport, le type de séance, la durée et la distance (ou « On verra comment je me sens ») et les bidons (1 à 3, taille et contenu), puis **Démarrer**.

Pendant la sortie, le boîtier envoie une alerte à intervalle régulier : le vibreur et/ou le buzzer font 3 × 0,5 s, la LED s'allume, et l'écran affiche une barre par gorgée à boire (3 au maximum) avec la goutte et le type de boisson. Le programme considère que l'athlète boit à chaque alerte.

Les onglets de l'application :

| Onglet | Contenu |
|---|---|
| **Accueil** | Météo du jour, tableau de bord et séances enregistrées |
| **Sortie** | Préparer la sortie (sport, séance, durée, bidons), puis la suivre en direct |
| **Profil** | Sexe, transpiration (avec un test), sueur plus ou moins salée, sports pratiqués |
| **Réglages** | Boîtier, alertes (vibreur, buzzer), rappels, connectivité, confidentialité et RGPD, préférences |
| **Test** | Essayer le vibreur, le buzzer et une alerte complète, afficher sur l'écran, choisir la météo simulée |

**Ce qui est simulé :** la fréquence cardiaque, la météo et la distance. Le Wi-Fi du boîtier n'a pas accès à Internet et il n'y a pas encore de vraie montre.

**Ce qui est enregistré dans la carte :** le profil, les réglages, les séances et l'historique des sorties. Ils restent après un redémarrage. Une sortie en cours est sauvegardée chaque minute et reprend, en pause, après une coupure. L'historique se télécharge en CSV depuis Réglages, et son enregistrement peut être désactivé (Réglages > Confidentialité).

Les textes « Politique de confidentialité » et « Avertissement médical » sont des brouillons. Ils se trouvent en haut de la partie « Textes légaux » du script de `web/index.html`, à remplacer par ceux du Drive.

### Changer d'écran

HydroPerf utilise l'écran OLED. Pour revenir à l'écran ST7735, remplacer `+<ecran_oled.cpp>` par `+<ecran.cpp>` dans l'environnement `hydroperf` de `platformio.ini`. Les deux fichiers proposent les mêmes fonctions : le reste du code ne change pas.

## Tester l'interface sans la carte

`outils/faux_boitier.py` imite l'ESP32 sur l'ordinateur (même API, sortie simulée) :

```sh
python3 outils/faux_boitier.py web/index.html 8765
```

Puis ouvrir http://localhost:8765. Pour faire avancer la sortie en cours de 10 minutes : `POST /test/avancer?s=600`.

## Sprint 1

Premier prototype, sans écran : une page web pour allumer et éteindre la LED, le vibreur et le buzzer.

1. Envoyer le programme : `pio run -e sprint1 -t upload`.
2. Se connecter au Wi-Fi `COUAD-ESP32` (mot de passe `couad2026`).
3. Ouvrir http://192.168.4.1.

Son brochage est différent de celui de HydroPerf : LED sur GPIO 16, vibreur sur GPIO 4, buzzer sur GPIO 17.

## En cas de problème

| Problème | Cause probable |
|---|---|
| L'écran OLED reste noir | VDD ou GND mal branché. `test_oled` vérifie le câblage et cherche l'écran tout seul. |
| L'écran ST7735 reste blanc | Il ne reçoit pas les ordres : un fil mal branché, ou un Arduino UNO (5 V) branché en direct alors que l'écran fonctionne en 3,3 V. |
| L'écran devient blanc par moments | Faux contact sur un fil : souder ou raccourcir les fils. |
| L'envoi du programme échoue | Mauvais câble USB (charge seulement), ou maintenir le bouton BOOT pendant l'envoi. |
| La carte redémarre en boucle | Carte avec moins de 16 Mo de flash : adapter `board_upload.flash_size` et `board_build.partitions` dans `platformio.ini`. |
| hydroperf.local ne répond pas | « DNS privé » activé sur le téléphone : utiliser http://192.168.4.1. |
