// coupure.h - quand baisser le son de la tele (identique carte / PC)
//
// La carte baisse le volume de la tele par la telecommande Wi-Fi (tele.h) au
// niveau BAS reglable, puis le remonte au niveau HAUT (ou au niveau d'origine).
//
// Mode "fenetres" (par defaut) : la carte entend mal pendant la coupure (son
// bas ou nul), elle ne peut donc pas entendre la fin de la pub. Elle coupe par
// fenetres :
//   pub confirmee 1,5 s  -> COUPE pendant duree_s (reglable 10 s .. 4 min)
//   fin de la coupure    -> le son revient, REECOUTE pendant reecoute_s
//        - nouvelle preuve de pub (spot reconnu APRES le retour du son) -> nouvelle coupure
//        - rien de publicitaire -> le son reste (REPOS)
//   "PAS PUB" ou detecteur arrete -> le son revient tout de suite
//
// Mode "suivre" : son baisse tant que la detection dit PUB (tampon de 1,5 s
// avant de baisser et de 2 s avant de remonter). A utiliser avec un niveau bas
// assez fort pour que la carte entende encore la tele.
#pragma once
#include <stdint.h>
#include "detecteur.h"

namespace cpr {

enum Etat { REPOS = 0, CONFIRMATION, COUPE, REECOUTE };
enum Mode { SUIVRE = 0, FENETRES = 1 };

extern volatile int mode;            // SUIVRE ou FENETRES (cfg::R.coupure)
extern volatile int duree_s;         // duree d'une coupure (cfg::R.coupure_s, 10..240)
extern volatile int reecoute_s;      // fenetre de reecoute apres le retour du son
extern volatile bool fin_spot;       // rouvrir l'oreille a la fin de la pub reconnue si elle finit avant la duree
extern volatile bool suspendue;       // captation en cours (v0.6.8) : le son de la tele n'est jamais baisse
// v0.9.63 : ENTREE OPTIQUE. La carte entend la tele sur la fibre meme son baisse (sortie optique
// a niveau fixe) : en mode fenetres, le son n'est PAS remonte pour reecouter. Il reste baisse tant que
// des pubs sont reconnues, et remonte apres duree_s sans pub reconnue (12 min au plus). Si la fibre
// ne donne plus de son pendant 5 s, retour au fonctionnement normal (fenetres + reecoute).
extern volatile bool ecoute_fibre;   // le moteur : source optique ET son recu en ce moment
bool coupe_fibre();                  // la coupure en cours suit la pub sur la fibre (pas de reecoute)
// v0.9.65 : fin d'ecran probable : le son remonte en PALIERS (lent au debut, de plus en plus vite).
// Palier 1 a DEMI_MS apres la fin du dernier spot reconnu, paliers 2 et 3 a 30 % et 62 % du reste de
// l'attente, puis le volume d'avant la pub a la fin de l'attente (duree_s sans pub reconnue).
// Une pub reconnue fait redescendre le son tout bas.
int niveau();                        // 0 son normal, 1 son bas, 2..4 paliers 1..3 de la remontee
int palier();                        // 0 aucun, 1..3
const int DEMI_MS = 6000;
const int NB_PALIERS = 3;            // + le retour au volume d'avant = 4 changements de volume
const int FIBRE_PERDUE_MS = 5000;
const uint32_t FIBRE_MAX_MS = 720000u;

const int CONFIRMATION_MS = 1500;    // pub vue au moins 1,5 s avant de couper
const int RETOUR_MS = 2000;          // mode suivre : 2 s de programme avant de remettre le son
const int ATTENTE_MS = 1500;         // apres le retour du son : temps pour que la tele remonte le volume
const int MIN_ENTRE_ACTIONS_MS = 4000;   // mode suivre : pas d'allers-retours

void etape(uint32_t ms, const det::Vue &v);   // appele par le moteur a chaque passage
bool active();                       // son a baisser maintenant
// « REMETTRE LE SON » : le son revient tout de suite et n'est plus baisse jusqu'a
// la fin de cet ecran pub (8 s sans pub, ou 12 min). La bibliotheque n'est pas
// touchee. Renvoie false si le son n'etait pas baisse.
bool liberer();
// v0.9.43 : les dernieres coupures (la pub qui a declenche) : pour la supprimer si c'est une fausse alerte
const int NB_HIST = 5;
struct Coupure { uint32_t ms; int numero, score, chaine; };   // v0.9.71 : plus de mot-cle vocal
int historique(Coupure *out, int max);      // la plus recente en premier ; renvoie le nombre
bool libere();                       // son rendu a la main, en attendant la fin de l'ecran
int etat();
int reste_s();                       // COUPE : secondes avant le retour du son ; REECOUTE : secondes d'ecoute restantes ; CONFIRMATION : ms restantes / 1000
float fraction_restante();           // compte a rebours (coupure, reecoute) : 1 -> 0
int total_s();                       // duree totale de la coupure / de la reecoute en cours
int ecoule_s();                      // secondes deja passees dans l'etat en cours
uint32_t nb_coupures();              // depuis le demarrage

}  // namespace cpr
