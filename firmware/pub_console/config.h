// config.h - les reglages, partages par l'ecran tactile et la page web
// (identique sur la carte et le PC). Ranges en NVS a chaque modification.
//
// v0.6.0 : nouvelle serie de reglages (les anciens, Alexa / infrarouge /
// jingles / voix, sont oublies au premier demarrage ; le Wi-Fi de la box et
// l'etalonnage de l'ecran sont gardes a part et ne sont pas touches).
#pragma once
#include <stdint.h>

// v0.9.53 : base de pubs distante (decentralisee : serveur PC, NAS, 2e ESP32).
// 0 = desactivee : tout son code est exclu du programme, la carte travaille
// seulement avec sa bibliotheque locale et le portail web n'affiche plus l'option.
// 1 = comme avant (reglage « Base de pubs (reseau) » sur le portail).
#ifndef BASE_DISTANTE
#define BASE_DISTANTE 0
#endif

namespace cfg {

struct Reglages {
    uint32_t magie;
    // ---- detection ----
    int32_t seuil;      // 0 souple, 1 normal, 2 strict
    int32_t gain;       // 0 automatique, 1..6 = +12 .. +48 dB
    int32_t source;     // 0 micro, 1 entree optique (TOSLINKBee)
    int32_t actif;      // detecteur en marche
    int32_t chaine;     // chaine regardee : 0 = devinee automatiquement, 1.. = base::CHAINES
    int32_t base;       // 0 locale, 1 decentralisee (la carte n'est qu'un cache), 2 les deux
    // ---- coupure du son ----
    int32_t coupure;    // 0 suivre la detection, 1 couper par fenetres (le son revient pour reecouter)
    int32_t coupure_s;  // duree d'une coupure : 10 .. 240 s
    int32_t reecoute;   // reecoute apres le retour du son : 0 3 s, 1 5 s, 2 8 s, 3 12 s
    // ---- tele (telecommande Wi-Fi) ----
    int32_t tv_bas;     // volume pendant la pub (0 = silence)
    int32_t tv_haut;    // volume apres la pub
    int32_t tv_origine; // 1 : apres la pub, revenir au volume d'avant (sinon tv_haut)
    // ---- carte ----
    int32_t bip;        // haut-parleur : 0 non, 1 debut de pub, 2 debut et fin
    int32_t lum;        // retroeclairage 5..100 %
    int32_t ecran;      // 0 toujours allume, 1 discret (s'eteint apres 1 min, LED temoin)
    int32_t led;        // LED RGB de la carte : 0 eteinte, 1 temoin d'etat
    int32_t journal;    // 1 : journal garde en memoire (ecran, portail) ; 0 : coupe (port USB seulement)
    char tv_ip[16];     // adresse IP de la tele ("" = a regler)
    char serveur[48];   // base decentralisee : "" = trouvee toute seule, sinon "192.168.1.20" ou "nom.local"
};

extern Reglages R;
extern volatile uint32_t version;      // change a chaque modification (l'ecran suit)
extern const int SEUILS[3];
extern const int REECOUTES[4];

// description de chaque reglage numerique (validation, page web, JSON)
struct Champ { const char *cle; int32_t Reglages::*champ; int min, max; };
int nb_champs();
const Champ &champ(int i);

void charger();                        // NVS puis appliquer
void appliquer();                      // pousse les reglages vers le moteur, la tele, le journal
void sauver();
// modification par nom ("seuil", "tv_bas"...) ; false si inconnu ou hors
// limites. Applique et sauve.
bool regler(const char *cle, int valeur);
bool regler_texte(const char *cle, const char *valeur);   // "serveur", "tv_ip"
bool ip_valide(const char *s);         // a.b.c.d
void ip_tele(char *out, int n);        // copie de R.tv_ip (protegee)
// tous les reglages en JSON ({"seuil":1,...,"tv_ip":"...","serveur":"..."})
int en_json(char *out, int max);
// applique un JSON complet ou partiel (cles inconnues ignorees) ; renvoie le
// nombre de reglages changes, -1 si une valeur est hors limites (rien n'est change)
int depuis_json(const char *json);

// ---- v0.9.66 : 4 PREREGLAGES (detection et coupure du son ; ni tele, ni Wi-Fi, ni ecran) ----
// Gardes a part dans la NVS ("prereg1") : les reglages courants ne sont jamais remis a zero.
const int NB_PREREG = 4;
struct Prereglage {
    char nom[20];
    int32_t source, coupure, tv_bas, coupure_s, tv_origine, seuil, gain, reecoute;
};
void prereg_init();                    // lit la NVS (ou cree les 4 de depart ; le 4e = copie des reglages actuels)
bool prereg(int i, Prereglage &out);
bool prereg_appliquer(int i);          // copie dans les reglages, applique, sauve
bool prereg_enregistrer(int i, const char *nom);   // reglages actuels -> case i (nom vide : garde l'ancien)
void prereg_origine();                 // les 4 de depart (le 4e = reglages actuels)
int prereg_actif();                    // case identique aux reglages actuels, -1 sinon

}  // namespace cfg
