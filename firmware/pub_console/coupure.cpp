// coupure.cpp - coupure du son par fenetres (voir coupure.h)
// v0.6.0 : plus de jingles (le son revient a la fin de la reecoute ou sur
// « PAS PUB ») ; le test des coupures se fait dans le menu Tele (tele.h).
#include "coupure.h"
#include "plateforme.h"

// valeur par defaut (bancs d'essai PC) : la carte la remplace dans pub_console.ino
__attribute__((weak)) void plat_coupure_log(int, int, int, int, int) {}

namespace cpr {

volatile int mode = FENETRES;
volatile int duree_s = 30;
volatile int reecoute_s = 8;
volatile bool fin_spot = true;
volatile bool suspendue = false;
volatile bool ecoute_fibre = false;
static bool s_fibre = false;               // la coupure en cours suit la fibre
static uint32_t t_fibre_vue = 0;           // derniere fois que la fibre donnait du son
static uint32_t t_coupe_debut = 0;         // debut de la coupure (pour le plafond de 12 min)
static int s_palier = 0;                   // v0.9.65 : palier de la remontee (0 = son tout bas)
static uint32_t t_rec = 0;                 // derniere pub reconnue (ms)
static uint32_t t_spot_fin = 0;            // fin prevue du spot reconnu (ms)


static volatile int s_etat = REPOS;
static volatile int s_reste = 0;
static volatile uint32_t s_nb = 0;
static volatile uint32_t t_debut = 0;  // debut de l'etat courant
static volatile uint32_t t_fin = 0;    // COUPE : fin de la coupure
static volatile uint32_t s_total = 1;  // COUPE : duree totale de la coupure en cours (ms), pour le compte a rebours
static uint32_t t_pub = 0;           // CONFIRMATION : pub vue depuis
static uint32_t t_prog = 0;          // mode suivre : programme vu depuis
static uint32_t t_action = 0;        // dernier changement coupe / pas coupe
static uint32_t vu_pas_pub = 0;
static bool init = false;
static volatile uint32_t t_libre = 0;   // « remettre le son » touche a cet instant (0 = non)
static uint32_t t_libre_prog = 0;       // depuis quand plus aucune pub n'est vue

static Coupure s_hist[NB_HIST];
static int s_nh = 0;
static int s_num = 0, s_score = 0, s_chaine = 0;   // derniere pub vue (pour le journal des coupures)

// v0.9.64 : une pub reconnue pendant une coupure fibre : fin repoussee, son de nouveau tout bas
static void fibre_reconnue(uint32_t ms, const det::Vue &v)
{
    t_rec = ms;
    // v0.9.67 : pos_s est deja ramene a "maintenant" par le detecteur (pos_cour + temps depuis la
    // derniere preuve) : on ne retire plus age_match_s (il etait compte deux fois -> fin prevue trop tot)
    float reste = v.duree_s > 0 ? v.duree_s - v.pos_s : 0;
    t_spot_fin = reste > 0 ? ms + (uint32_t)(reste * 1000.0f) : ms;
    s_total = (uint32_t)duree_s * 1000u;
    t_fin = ms + s_total;
    if ((int32_t)(t_spot_fin + 3000u - t_fin) > 0) { t_fin = t_spot_fin + 3000u; s_total = t_fin - ms; }
    s_palier = 0;
}

static void passer(int e, uint32_t ms)
{
    if ((e == COUPE) != (s_etat == COUPE)) t_action = ms;
    if (e == COUPE && s_etat != COUPE) {
        s_nb = s_nb + 1;
        Coupure c = { ms ? ms : 1, s_num, s_score, s_chaine };
        if (s_nh > 0 && s_hist[0].numero == c.numero && ms - s_hist[0].ms < 180000u) s_hist[0].ms = c.ms;   // meme pub, re-coupure
        else {
            for (int i = NB_HIST - 1; i > 0; i--) s_hist[i] = s_hist[i - 1];
            s_hist[0] = c;
            if (s_nh < NB_HIST) s_nh++;
        }
        plat_coupure_log(0, s_num, s_score, s_chaine, 0);
    }
    if (e != COUPE && s_etat == COUPE) plat_coupure_log(1, s_num, s_score, s_chaine, (int)((ms - t_debut) / 1000u));
    if (e != COUPE) { s_fibre = false; s_palier = 0; }   // v0.9.63 / v0.9.65
    s_etat = e;
    t_debut = ms;
}

// duree de la coupure : le reglage, ou moins si la pub reconnue finit avant
// (on rouvre l'oreille au changement de spot, la ou un nouveau spot commence)
static uint32_t duree_coupure(const det::Vue &v)
{
    uint32_t d = (uint32_t)duree_s * 1000u;
    if (fin_spot && v.duree_s > 0) {
        float reste = v.duree_s - v.pos_s;
        if (reste > 3.0f && reste * 1000.0f + 300.0f < (float)d) d = (uint32_t)(reste * 1000.0f + 300.0f);
    }
    return d;
}

void etape(uint32_t ms, const det::Vue &v)
{
    if (!init) { vu_pas_pub = v.nb_pas_pub; init = true; }
    bool pas_pub = v.nb_pas_pub != vu_pas_pub;
    vu_pas_pub = v.nb_pas_pub;
    if (v.numero > 0) { s_num = v.numero; s_score = v.score; }
    s_chaine = v.chaine;

    // captation (v0.6.8) : on veut le bandeau entier, sans que la carte baisse la tele.
    // La detection continue (etat, statistiques), seule la coupure est suspendue.
    // v0.9.5 : pareil pendant TESTER (ecoute 1, enregistrement 4) : le son doit etre normal
    if (suspendue || v.test_etat == 1 || v.test_etat == 4) {
        if (s_etat != REPOS) passer(REPOS, ms);
        s_reste = 0;
        t_pub = 0;
        return;
    }

    // le detecteur arrete ou "PAS PUB" : le son revient tout de suite
    if (!det::actif || (v.etat == det::PAUSE && v.test_etat == 0) || pas_pub) {
        if (s_etat != REPOS) passer(REPOS, ms);
        s_reste = 0;
        t_pub = 0;
        return;
    }
    bool pub = v.etat == det::PUB;

    // son rendu a la main : pas de coupure jusqu'a la fin de cet ecran pub
    if (t_libre) {
        if (pub) t_libre_prog = 0;
        else if (!t_libre_prog) t_libre_prog = ms ? ms : 1;
        uint32_t tl = t_libre;                       // v0.9.5 : lu une fois, difference signee (liberer() peut ecrire apres le ms de cette passe)
        bool fini = (t_libre_prog && (int32_t)(ms - t_libre_prog) > 8000) || (int32_t)(ms - tl) > 720000;
        if (fini) { t_libre = 0; t_libre_prog = 0; t_pub = 0; }
        else {
            if (s_etat != REPOS) passer(REPOS, ms);
            s_reste = 0;
            t_pub = 0;
            return;
        }
    }
    // vraie reconnaissance : assez de votes MAINTENANT (le detecteur garde aussi
    // "PUB" quelques secondes entre deux spots, avec 2 ou 3 votes : ce maintien
    // n'est pas une preuve)
    bool reconnu = pub && v.score >= det::seuil_votes;

    if (mode == SUIVRE) {
        switch (s_etat) {
        case REPOS:
        case CONFIRMATION:
            if (!pub) { t_pub = 0; if (s_etat != REPOS) passer(REPOS, ms); break; }
            if (!t_pub) { t_pub = ms ? ms : 1; passer(CONFIRMATION, ms); }
            if (ms - t_pub >= (uint32_t)CONFIRMATION_MS && ms - t_action >= (uint32_t)MIN_ENTRE_ACTIONS_MS) { s_total = 1; t_fin = ms; passer(COUPE, ms); t_prog = 0; }
            break;
        default:                                          // COUPE
            if (pub) { t_prog = 0; break; }
            if (!t_prog) t_prog = ms ? ms : 1;
            if (ms - t_prog >= (uint32_t)RETOUR_MS && ms - t_action >= (uint32_t)MIN_ENTRE_ACTIONS_MS) {
                passer(REPOS, ms);
                t_pub = 0;
            }
            break;
        }
        s_reste = 0;
        return;
    }

    // ---- mode fenetres ----
    switch (s_etat) {
    case REPOS:
    case CONFIRMATION:
        if (!reconnu) { t_pub = 0; if (s_etat != REPOS) passer(REPOS, ms); break; }
        if (!t_pub) { t_pub = ms ? ms : 1; passer(CONFIRMATION, ms); }
        if (ms - t_pub >= (uint32_t)CONFIRMATION_MS) {
            s_fibre = ecoute_fibre;
            t_fibre_vue = ms;
            t_coupe_debut = ms;
            s_total = duree_coupure(v);
            t_fin = ms + s_total;
            passer(COUPE, ms);
            if (s_fibre) fibre_reconnue(ms, v);
        }
        break;
    case COUPE:
        if (s_fibre) {
            // v0.9.63 : la carte entend la pub sur la fibre : chaque pub reconnue repousse la fin
            if (ecoute_fibre) t_fibre_vue = ms;
            if ((int32_t)(ms - t_fibre_vue) > FIBRE_PERDUE_MS) {
                s_fibre = false;                                          // plus de son sur la fibre : fenetres normales
            } else if (reconnu) {
                fibre_reconnue(ms, v);
            }
            if (s_fibre) {
                // fin d'ecran probable : paliers de remontee, lents au debut puis de plus en plus rapides
                uint32_t ref = (int32_t)(t_spot_fin - t_rec) > 0 ? t_spot_fin : t_rec;
                int32_t e = (int32_t)(ms - ref);
                int32_t w = (int32_t)(t_fin - ref) - DEMI_MS;         // duree des paliers 1 a 3
                int p = 0;
                if (e >= DEMI_MS && w > 0) {
                    float x = (float)(e - DEMI_MS) / (float)w;
                    p = x >= 0.62f ? 3 : (x >= 0.30f ? 2 : 1);
                }
                if (p > s_palier) s_palier = p;
            }
            if (s_fibre && ms - t_coupe_debut >= FIBRE_MAX_MS) { passer(REPOS, ms); t_pub = 0; s_fibre = false; break; }
            if (s_fibre && (int32_t)(ms - t_fin) >= 0) { passer(REPOS, ms); t_pub = 0; s_fibre = false; break; }   // plus de pub : le son revient, sans reecoute
            if (s_fibre) break;
        }
        // pendant la coupure, la carte n'entend (presque) rien : on ne regarde pas
        // la detection, seulement l'heure
        if ((int32_t)(ms - t_fin) >= 0) passer(REECOUTE, ms);
        break;
    case REECOUTE:
        if (ms - t_debut >= (uint32_t)ATTENTE_MS && reconnu) {          // encore de la pub : on recoupe
            s_fibre = ecoute_fibre;
            t_fibre_vue = ms;
            t_coupe_debut = ms;
            s_total = duree_coupure(v);
            t_fin = ms + s_total;
            passer(COUPE, ms);
            if (s_fibre) fibre_reconnue(ms, v);
            break;
        }
        if (ms - t_debut >= (uint32_t)reecoute_s * 1000u) { passer(REPOS, ms); t_pub = 0; }   // plus de pub : le son reste
        break;
    }
    if (s_etat == COUPE) s_reste = (int)((t_fin - ms + 999) / 1000);
    else if (s_etat == REECOUTE) s_reste = (int)(((uint32_t)reecoute_s * 1000u - (ms - t_debut) + 999) / 1000);
    else s_reste = 0;
    if (s_reste < 0) s_reste = 0;
}


int historique(Coupure *out, int max)
{
    int n = s_nh < max ? s_nh : max;
    for (int i = 0; i < n; i++) out[i] = s_hist[i];
    return n;
}

bool liberer()
{
    bool coupe = s_etat == COUPE || s_etat == REECOUTE || s_etat == CONFIRMATION;
    uint32_t ms = plat_ms();
    if (coupe) { t_libre = ms ? ms : 1; }      // v0.9.5 : rien de coupe, rien a bloquer
    return coupe;
}
bool libere() { return t_libre != 0; }

bool active() { return s_etat == COUPE; }
bool coupe_fibre() { return s_etat == COUPE && s_fibre; }
int niveau() { return s_etat != COUPE ? 0 : (s_fibre && s_palier ? 1 + s_palier : 1); }
int palier() { return s_etat == COUPE && s_fibre ? s_palier : 0; }

// compte a rebours affiche : 1 = tout le temps reste, 0 = le son revient
float fraction_restante()
{
    uint32_t now = plat_ms();
    float f = 0;
    if (s_etat == COUPE) f = (float)(int32_t)(t_fin - now) / (float)(s_total ? s_total : 1);
    else if (s_etat == REECOUTE) f = 1.0f - (float)(now - t_debut) / (float)((uint32_t)reecoute_s * 1000u);
    else if (s_etat == CONFIRMATION) f = 1.0f - (float)(now - t_debut) / (float)CONFIRMATION_MS;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}
int total_s()
{
    if (s_etat == COUPE) return (int)((s_total + 500) / 1000);
    if (s_etat == REECOUTE) return reecoute_s;
    return 0;
}
int ecoule_s() { return s_etat == REPOS ? 0 : (int)((plat_ms() - t_debut) / 1000); }
int etat() { return s_etat; }
int reste_s() { return s_reste; }
uint32_t nb_coupures() { return s_nb; }

}  // namespace cpr
