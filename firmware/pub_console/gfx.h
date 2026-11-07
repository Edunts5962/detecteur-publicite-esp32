// gfx.h - petit moteur de dessin lisse sur un tampon RGB565 320x240.
//
// Tout est dessine dans un tampon en memoire (PSRAM), puis envoye d'un bloc
// a l'ecran a chaque image : pas de scintillement, et on peut melanger les
// couleurs (transparence, halos lumineux, texte lisse).
// Ce fichier ne depend ni d'Arduino ni de TFT_eSPI : le meme code tourne
// dans le simulateur PC (outils/simulateur).
#pragma once
#include <stdint.h>
#include "polices.h"

namespace gfx {

const int L = 320;
const int H = 240;

extern uint16_t *fb;            // tampon courant (L*H pixels)

void init(uint16_t *tampon);

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t melange(uint16_t c0, uint16_t c1, uint8_t t);   // t=0 -> c0, 255 -> c1

// Zone de decoupe (tout dessin hors de cette zone est ignore)
void decoupe(int x0, int y0, int x1, int y1);              // x1/y1 exclus
void sans_decoupe();

// Primitives de base
void pixel(int x, int y, uint16_t c, uint8_t a = 255);
void pixel_ajout(int x, int y, uint16_t c, uint8_t a);     // lumiere additive
void rect(int x, int y, int l, int h, uint16_t c, uint8_t a = 255);
void degrade_v(int x, int y, int l, int h, uint16_t c0, uint16_t c1, uint8_t a = 255);
void degrade_h(int x, int y, int l, int h, uint16_t c0, uint16_t c1, uint8_t a = 255);
void rect_arrondi(int x, int y, int l, int h, int r, uint16_t c, uint8_t a = 255);
void rect_arrondi_degrade(int x, int y, int l, int h, int r, uint16_t c0, uint16_t c1, uint8_t a = 255);
void contour_arrondi(int x, int y, int l, int h, int r, float ep, uint16_t c, uint8_t a = 255);

// Formes lissees (coordonnees en virgule flottante)
void disque(float cx, float cy, float r, uint16_t c, uint8_t a = 255);
void anneau(float cx, float cy, float r, float ep, uint16_t c, uint8_t a = 255);
void arc(float cx, float cy, float r, float ep, float ang0, float ang1, uint16_t c, uint8_t a = 255);
void trait(float x0, float y0, float x1, float y1, float ep, uint16_t c, uint8_t a = 255);
void trait_pointille(float x0, float y0, float x1, float y1, float ep, float tiret, float trou,
                     uint16_t c, uint8_t a = 255);
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c, uint8_t a = 255);
void halo(float cx, float cy, float r, uint16_t c, uint8_t intensite);   // lumiere additive

// Texte lisse (UTF-8). y = haut de la ligne. Alignement : 0 gauche, 1 centre, 2 droite.
enum { GAUCHE = 0, CENTRE = 1, DROITE = 2 };
int texte(int x, int y, const char *s, const PoliceAA &p, uint16_t c, uint8_t a = 255, int align = GAUCHE);
int largeur_texte(const char *s, const PoliceAA &p);
int hauteur(const PoliceAA &p);

// Copie rapide d'un tampon complet (fond pre-calcule)
void copier(const uint16_t *source);

}  // namespace gfx
