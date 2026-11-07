// moteur.h - la boucle d'analyse.
//
// Sur la carte : appele en boucle par la tache moteur (coeur 0).
// Sur le PC : appele par les bancs d'essai.
//   micro -> niveau -> empreintes -> journal + detecteur
//   -> coupure du son -> telecommande Wi-Fi de la tele
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mot {

enum { SRC_MICRO = 0, SRC_FO = 1 };
extern volatile int source;

void init();                         // empreintes, bibliotheque (relue sur la SD), simulation

// traite ce qui est arrive ; renvoie le temps (ms) avant le prochain appel utile
uint32_t etape(uint32_t ms);

// sensibilite du micro : choix -1 = automatique, 0..NB_GAINS-1 = niveau fixe
const int NB_GAINS = 6;              // +12 / +21 / +30 / +36 / +42 / +48 dB
extern const int GAIN_DB[NB_GAINS];
void regler_gain(int choix);
int gain_niveau();                   // niveau applique en ce moment (aussi en automatique)
bool gain_auto();

// zones du niveau sonore (vumetre) : en dessous, trop faible ; au-dessus, trop fort
const float NIVEAU_FAIBLE = -48.0f;  // dB plein echelle
const float NIVEAU_FORT = -15.0f;

float niveau_db();                   // niveau du son (dB plein echelle)
float crete();                       // 0..1
bool sature();
bool memoire_ok();

// spectre du son pour l'ecran d'accueil (v0.6.2) : 24 bandes de 120 Hz a 6,5 kHz,
// calcule ~15 fois par seconde SEULEMENT si l'ecran le lit (sinon : rien a payer).
// Independant des empreintes (qui ne changent pas).
const int NB_BANDES = 24;
void spectre(uint8_t *out);          // copie NB_BANDES valeurs 0..255

}  // namespace mot
