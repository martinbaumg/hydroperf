#pragma once

// IO21 (SDA) et IO22 (SCK) sont pris par l'écran OLED, voir src/ecran_oled.cpp
const int PIN_LED = 26;      // LED rouge : allumée tant qu'il y a des gorgées à boire
const int PIN_VIBREUR = 4;   // SIG du module vibrant Grove
const int PIN_BUZZER = 17;   // Buzzer : rappel ignoré
