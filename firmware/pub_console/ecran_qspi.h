// ecran_qspi.h - ecran 3,5" IPS ST77922 en QSPI de la carte ES3C35P (320x480).
//
// L'interface du detecteur dessine toujours dans un tampon 320x240 (gfx.h).
// Ce pilote l'agrandit x4/3 (lissage bilineaire) a l'envoi : 427x320 au
// centre de l'ecran 480x320 en paysage, les deux bandes laterales (26/27 px)
// prolongent le bord de l'image. Aucun ecran de l'interface n'est a refaire.
//
// Le tactile est etalonne en coordonnees de l'interface (320x240) : il suit
// automatiquement l'agrandissement et l'orientation.
#pragma once
#include <stdint.h>

// Orientation (paysage) :
// ECRAN_RETOURNE 0 : micro a DROITE de l'ecran ; 1 : micro a gauche (retourne de 180 degres)
// ECRAN_MIROIR 1 : seulement si le texte apparait a l'envers en miroir (haut et bas inverses)
#ifndef ECRAN_RETOURNE
#define ECRAN_RETOURNE 0
#endif
#ifndef ECRAN_MIROIR
#define ECRAN_MIROIR 0
#endif
// 1 : agrandissement lisse (bilineaire) ; 0 : sans lissage, bien plus rapide
// mais traits un peu irreguliers (une colonne / ligne sur trois doublee)
#ifndef ECRAN_LISSAGE
#define ECRAN_LISSAGE 1
#endif

bool ecran_init();                                     // bus QSPI, sequence d'init du fabricant, ecran noir
void ecran_envoyer(const uint16_t *img);               // tampon 320x240 RGB565 -> ecran entier
void ecran_envoyer_gb(const uint16_t *img);            // idem, octets deja inverses (image TFT_eSprite)
void ecran_cadre(int x, int y, int l, int h, uint16_t c);   // cadre (coordonnees 320x240), retour au toucher
void ecran_remplir(uint16_t c);                        // ecran uni (message d'erreur au demarrage)
void ecran_banc(const uint16_t *img);                  // mesure chaque etape (journal), au demarrage
