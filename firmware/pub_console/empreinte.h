// empreinte.h - empreintes audio "facon Shazam" (identique sur la carte et le PC).
//
//   16 kHz --decimation x2--> 8 kHz --FFT 512 / saut 256--> spectrogramme
//   (31,25 colonnes/s, 250 Hz .. 3,7 kHz) --blanchiment--> pics locaux
//   (constellation, ~15/s) --paires de pics--> codes de 22 bits
//
// Un code = (frequence du pic d'ancrage, frequence d'un pic qui le suit,
// ecart de temps) : il ne depend ni du volume ni du bruit de la piece,
// seulement de la position des pics. On le date par sa colonne d'ancrage.
#pragma once
#include <stdint.h>

namespace emp {

const int FS = 8000;                 // frequence d'analyse
const int N = 512;                   // FFT (64 ms)
const int SAUT = 256;                // nouvelle colonne toutes les 32 ms
const float IPS = (float)FS / SAUT;  // 31,25 colonnes par seconde
const int B0 = 16;                   // premier bin garde (250 Hz)
const int NB = 224;                  // bins gardes (jusqu'a 3,73 kHz)
const int DT_MAX = 40;               // ecart max ancrage -> cible (1,28 s)
const int DF_MAX = 48;               // ecart de frequence max (750 Hz)
const int EVENTAIL = 4;              // cibles par ancrage gardees dans la bibliotheque
#ifndef EVENTAIL_REQ_DEF
#define EVENTAIL_REQ_DEF 8
#endif
const int EVENTAIL_REQ = EVENTAIL_REQ_DEF;  // cibles par ancrage pour la recherche
// Un code de rang >= EVENTAIL ne sert qu'a la RECHERCHE (bit 31 = 1) : il
// n'entre jamais dans la bibliotheque. Si un pic parasite s'intercale a
// l'ecoute, les 4 paires apprises se retrouvent quand meme parmi les 8.
const uint32_t REQUETE = 0x80000000u;
// Pics FORTS (les ~15/s les plus marques, 4 par colonne au plus) : eux seuls
// forment les codes appris. Pics FAIBLES (jusqu'a ~40/s, 8 par colonne) :
// a l'ecoute seulement. Dans un salon bruyant, la voix des gens donne des pics
// plus forts que ceux de la tele : les pics appris passent "faibles", mais
// leurs paires sont quand meme formees, avec le meme code. Ces codes portent
// FAIBLE (bit 30) et REQUETE : jamais appris, jamais comptes dans les
// statistiques de fin de pub (calibrees sur les codes forts).
const uint32_t FAIBLE = 0x40000000u;
const uint32_t MASQUE = 0x003FFFFFu;
#ifndef EVENTAIL_FAIBLE_DEF
#define EVENTAIL_FAIBLE_DEF 12
#endif
const int EVENTAIL_FAIBLE = EVENTAIL_FAIBLE_DEF;   // cibles par ancrage, tous pics confondus
const int VOISIN_F = 3, VOISIN_T = 3;  // voisinage d'un pic (7 x 7)
const int MAX_PAR_COL = 4;           // pics FORTS gardes par colonne
#ifndef MAX_FAIBLES_DEF
#define MAX_FAIBLES_DEF 8
#endif
const int MAX_TOUS_COL = MAX_FAIBLES_DEF;   // pics gardes par colonne, faibles compris (0..8)
const int HIST = 320;                // colonnes gardees pour l'affichage (10 s)

struct Code { uint32_t h; uint32_t t; };       // code 22 bits (+ bit REQUETE), colonne d'ancrage
struct Pic { uint32_t t; uint8_t f; uint8_t force; uint8_t fort; };
struct Lien { uint32_t t0, t1; uint8_t f0, f1; };

void init();
void reinit();                       // oublie tout (changement de source)

// ajoute n echantillons a 16 kHz ; copie au plus max codes produits dans out
int ajouter(const int16_t *x16, int n, Code *out, int max);

uint32_t colonne();                  // colonnes calculees depuis le debut (horloge des codes)
// v0.9.58 : empreintes d'un FICHIER (portail), sans rien changer a l'analyse en direct :
// debut : l'etat du direct est sauve, l'analyse repart a zero (rien n'est affiche) ;
// repartir : apres ~2 s de mise en route sur le debut du fichier, on reprend au debut
// du fichier SANS la chauffe (le blanchiment est deja stable : codes des la 1re seconde) ;
// fin : l'etat du direct est remis exactement (horloge des colonnes comprise).
// A faire sous plat_emp_prendre() (le moteur en direct attend).
bool hors_ligne_debut();
void hors_ligne_repartir();
void hors_ligne_fin();

// ---- pour l'affichage (ecrit par la tache moteur, lu par l'interface) ----
extern uint8_t (*spectro)[NB];       // [HIST][NB] en PSRAM : intensite 0..255, colonne t -> spectro[t % HIST]
extern volatile uint32_t nb_colonnes;
const int NB_PICS_AFF = 512;
extern Pic *pics;                    // [NB_PICS_AFF] : pic k -> pics[k % NB_PICS_AFF]
extern volatile uint32_t nb_pics;
const int NB_LIENS_AFF = 128;
extern Lien liens[NB_LIENS_AFF];
extern volatile uint32_t nb_liens;

extern volatile int densite;         // pics FORTS par seconde vises (seuil adaptatif)
extern volatile int densite_faible;  // pics par seconde vises, faibles compris (0 = pas de pics faibles)
float pics_par_s();
float codes_par_s();
float seuil_db();

}  // namespace emp
