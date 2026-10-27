#pragma once
#include <cstdint>
#include <vector>
#include "core/memory.hpp"

// Dessine les deux ecrans de la console (256 x 192 chacun, l'un au-dessus de l'autre) a partir de
// l'etat actuel de la memoire video et des registres des moteurs 2D.
// Le resultat est un tableau de 256 x 384 pixels au format 0x00RRGGBB.
//
// Gere : fonds en tuiles (4 et 8 bits), fonds a rotation / etendus / bitmap, palettes etendues, couche 3D,
// sprites (normaux, a rotation / zoom, taille double ; normaux, semi-transparents, fenetre ou bitmap), fenetres WIN0 /
// WIN1 / sprites, effets de couleur (transparence, eclaircir, assombrir, melange de la 3D), luminosite generale, capture d'ecran.
// Pas encore gere : mosaique, affichage depuis la memoire principale.
void renderScreens(const System& sys, std::vector<uint32_t>& rgb);

// Capture d'ecran (DISPCAPCNT) : ecrit l'image du moteur A, la 3D et / ou une banque de VRAM dans une banque.
// Appelee au debut du V-Blank quand la capture a ete demandee pour l'image qui se termine.
void displayCapture(System& sys);

// Ecrit une image PNG (sans bibliotheque : donnees non compressees).
bool writePng(const char* path, const std::vector<uint32_t>& rgb, int width, int height);
