// VERSION DE CE FICHIER : v1.1 (2026-09-22)
// Dernier changement : ajout de ft6336_touch_save_calibration() et
// ft6336_touch_load_calibration(), pour conserver l'etalonnage tactile en
// memoire flash (NVS) d'un demarrage a l'autre, au lieu de le refaire
// systematiquement (voir setup() dans le .ino, et le nouveau bouton de
// re-etalonnage a la demande).
#ifndef FT6336_TOUCH_H
#define FT6336_TOUCH_H

#include <Arduino.h>
#include "driver/i2c_master.h"

// Adresse I2C standard du FT6336 (verifie dans la doc de ton module si la
// lecture echoue systematiquement - certains modules cablent une adresse
// alternative).
#define FT6336_I2C_ADDR 0x38

// IMPORTANT (compatibilite multitache / multi-bus) :
// La bibliotheque FT6336.h d'origine pilote le tactile via Wire (le driver
// I2C "historique" d'Arduino). Le pilote es8311.cpp fourni pilote lui le
// codec audio via la NOUVELLE API i2c_master d'ESP-IDF (i2c_master_bus_...).
// Ces deux API ne peuvent pas configurer le meme peripherique I2C materiel
// (I2C_NUM_0) en meme temps : la seconde installation echoue.
// Solution retenue ici : un seul bus i2c_master (cree une fois dans le
// .ino), sur lequel on attache DEUX peripheriques - le codec ES8311 et le
// tactile FT6336 - chacun avec sa propre adresse. C'est la facon standard
// de partager un bus I2C entre plusieurs puces. Cela tombe bien : les deux
// projets d'origine utilisaient deja les memes broches SDA=16 / SCL=15.
bool ft6336_touch_init(i2c_master_bus_handle_t bus, gpio_num_t rst_pin,
                        uint16_t screen_w, uint16_t screen_h);

// Lecture BRUTE (non etalonnee) du capteur : utilisee uniquement pendant
// l'etalonnage (voir run_touch_calibration() dans le .ino). Renvoie true
// si un doigt est actuellement detecte, avec ses coordonnees telles que
// rapportees par la puce, sans aucune mise a l'echelle ni conversion.
bool ft6336_touch_get_raw(int *raw_x, int *raw_y);

// Enregistre la transformation affine (echelle + decalage + permutation
// eventuelle des axes) qui convertit une position brute du capteur en
// coordonnees ecran, a partir de 3 points de reference (ecran connu, brut
// mesure) non alignes. C'est la methode standard d'etalonnage d'ecran
// tactile a 3 points : elle retrouve automatiquement toute permutation
// X/Y et toute inversion d'axe due au montage physique du capteur, sans
// avoir besoin de la deviner a l'avance (contrairement a la version
// precedente qui supposait une orientation fixe en fonction de la
// rotation ecran - hypothese qui s'est revelee fausse sur le materiel
// reel). Voir run_touch_calibration() dans le .ino, qui fournit ces 3
// points en demandant a l'utilisateur de toucher 3 croix affichees a
// l'ecran au demarrage.
// v2.1-BETA-LVGL : renvoie desormais false (et garde l'etalonnage en
// cours) si les 3 points sont alignes ou confondus - les appels existants
// qui ignorent la valeur de retour restent valides.
bool ft6336_touch_set_calibration(int raw_x0, int raw_y0, int screen_x0, int screen_y0,
                                   int raw_x1, int raw_y1, int screen_x1, int screen_y1,
                                   int raw_x2, int raw_y2, int screen_x2, int screen_y2);

// v2.1-BETA-LVGL - AJOUT : lecture/ecriture directe des 6 coefficients
// (a, b, c, d, e, f) de la transformation affine en cours, pour pouvoir
// revenir a l'etalonnage precedent si un nouvel etalonnage est annule ou
// juge imprecis (ecran d'etalonnage de ui_lvgl.cpp). N'ecrit PAS en NVS.
void ft6336_touch_get_coeffs(float out[6]);
void ft6336_touch_set_coeffs(const float in[6]);

// v2.1-BETA-LVGL - AJOUT : applique l'etalonnage en cours a un point BRUT
// deja lu (sans nouvelle lecture I2C), resultat arrondi et borne a l'ecran.
void ft6336_touch_map_raw(int raw_x, int raw_y, int *x, int *y);

// Lecture ETALONNEE, utilisee par LVGL (my_touchpad_read). Renvoie true si
// un doigt est detecte, avec *x/*y deja convertis en coordonnees ecran via
// la derniere calibration enregistree (ou une correspondance brute par
// defaut, non fiable, si aucun etalonnage n'a encore ete effectue).
bool ft6336_touch_read(int *x, int *y);

// Sauvegarde en memoire flash non volatile (NVS, via la bibliotheque
// Preferences d'Arduino-ESP32) l'etalonnage COURANT, c'est-a-dire celui mis
// en place par le dernier appel a ft6336_touch_set_calibration(). A appeler
// juste apres un etalonnage reussi (demarrage initial ou re-etalonnage a la
// demande) : cet enregistrement survit a un redemarrage, une coupure
// d'alimentation, et meme un reflashage du programme (la NVS est une zone
// separee de la memoire flash, non touchee par un televersement classique).
void ft6336_touch_save_calibration();

// Tente de recharger un etalonnage precedemment enregistre par
// ft6336_touch_save_calibration(). Si un enregistrement valide est trouve,
// l'applique immediatement (equivalent a un appel a
// ft6336_touch_set_calibration() avec les memes valeurs) et renvoie true.
// Renvoie false si aucun etalonnage n'a encore ete enregistre (tout premier
// demarrage, ou apres un effacement de la NVS) - dans ce cas l'appelant doit
// lancer un etalonnage (voir run_touch_calibration() dans le .ino).
bool ft6336_touch_load_calibration();

#endif // FT6336_TOUCH_H
