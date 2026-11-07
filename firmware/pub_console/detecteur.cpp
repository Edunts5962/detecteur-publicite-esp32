// detecteur.cpp - etats, TESTER (facon Shazam), commandes, evenements, historique
//
// v0.6.0 (repart de v0.5.31) :
//  - retires : jingles (annonce, fin d'ecran, apprentissage automatique entre
//    deux jingles, syntheses), commande vocale, « PUB ... FIN DE PUB » ;
//  - corrige : ANNULER pendant l'enregistrement de TESTER laissait la detection
//    en pause pour toujours (test_etat restait a 4).
// v0.6.2 : c'est l'utilisateur qui marque la pub.
//  - TESTER = debut de la pub. Connue -> PUB tout de suite (le son est baisse).
//    Inconnue -> enregistree depuis l'appui sur TESTER ;
//  - ENREGISTRER = fin de la pub : l'empreinte de tout le passage est sauvee ;
//  - plus d'arret au prochain silence, plus de decoupe aux silences, plus de
//    recherche de la pub suivante pendant l'enregistrement ;
//  - ANNULER pendant l'enregistrement ou dans les 20 s qui suivent ;
//    90 s sans ENREGISTRER : rien n'est appris.
// La reconnaissance continue (votes, fin de pub, consolidation) est celle de la
// v0.5.31, inchangee.
#include "detecteur.h"
#include "base.h"
#include "plateforme.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

namespace det {

volatile int seuil_votes = 10;
volatile int jingle_hp = 1;
volatile bool actif = true;
volatile int resultat = 0;
volatile uint32_t nb_resultats = 0;
volatile int chaine_reglee = 0;
volatile int mode_base = 0;      // v0.9.53 : carte seule par defaut (config.cpp applique le reglage si BASE_DISTANTE)
uint8_t historique[NB_CASES];
volatile uint32_t nb_cases = 0;

static const float IPS = emp::IPS;
static const uint32_t FENETRE = (uint32_t)(4 * IPS);       // 4 s de codes pour reconnaitre
// Fin de pub : on compte les codes entendus APRES le dernier son reconnu. Pendant
// la pub, une part p des codes tombe sur le bloc reconnu (mesuree en direct). Si
// n codes de suite ne tombent plus dessus alors que (1 - p)^n < 1/10 000, la pub
// est finie (et au moins 60 codes). Presque plus de codes (silence entre deux
// spots, son faible) : on attend au plus MAINTIEN.
static const uint32_t MAINTIEN = (uint32_t)(3 * IPS);      // PUB tenu au plus 3 s sans preuve
#ifndef FIN_ECART
#define FIN_ECART 0.8f
#endif
#ifndef S2_MIN
#define S2_MIN 4
#endif
#ifndef FIN_MIN
#define FIN_MIN 60
#endif
#ifndef FIN_RISQUE
#define FIN_RISQUE 1e-4f
#endif
static const uint32_t ECART_MIN = (uint32_t)(FIN_ECART * IPS);  // au moins ce temps apres le dernier son reconnu
static const uint32_t BLOC_MAX = (uint32_t)(420 * IPS);    // une pub : 7 min au plus
static const uint32_t BLOC_MIN = (uint32_t)(5 * IPS);      // une pub : 5 s au moins
static const int NF = 4096;          // codes forts ET faibles : ~450/s a l'ecoute

// fenetre glissante des codes recents
static emp::Code *fen = nullptr;      // NF codes (PSRAM)
static int fen_n = 0;
// touches (resultats de recherche) des codes recents, pour voter sans rechercher
static const int NT = 16384;
static base::Touche *tou = nullptr;   // NT touches (PSRAM)
static int tou_n = 0;
static base::Touche *tsel = nullptr;  // selection de travail (PSRAM)
static uint32_t tou_version = 0;
static uint32_t col = 0;             // colonne la plus recente vue
static uint32_t prochain_test = 0, prochaine_case = 0;

// reconnaissance
static uint32_t der_match = 0;       // colonne de la derniere reconnaissance (0 = jamais)
static float taux_pub = 0.3f;        // part des codes qui tombent sur la pub reconnue
// rythme des preuves (touches concordantes par seconde) pendant la pub : dans un
// salon bruyant les preuves arrivent en rafales, il faut attendre plus longtemps
// un "trou" avant de conclure. Attente = ln(1/risque) / rythme, bornee.
static float rythme = 20.0f;
#ifndef MAINTIEN_MAX_S
#define MAINTIEN_MAX_S 4.5f
#endif
static const uint32_t MAINTIEN_MAX = (uint32_t)(MAINTIEN_MAX_S * IPS);
static int32_t dec_cour = 0;         // decalage de la pub reconnue
// Entre deux spots, les chaines laissent un court silence. Si le son a plonge
// juste apres la derniere reconnaissance, c'est sans doute le spot suivant qui
// commence : on lui laisse le temps d'etre reconnu.
static float niv_pub = -30;           // niveau moyen pendant la pub
static uint32_t creux_t = 0;          // dernier creux de son (colonne)
#ifndef CREUX_S
#define CREUX_S 2.5f
#endif
static const uint32_t APRES_CREUX = (uint32_t)(CREUX_S * IPS);
static int bloc_cour = -1;
static int32_t pos_cour = 0;
static int score = 0, second_ = 0;
static uint32_t cpt_pas_pub = 0;     // compteur pour la coupure du son
static int etat = APPRENTISSAGE;
static bool en_pub = false;
// ---- chaine regardee : devinee d'apres la derniere pub reconnue ----
static int chaine_dev = 0;
static int chaine_courante() { return chaine_reglee > 0 ? chaine_reglee : chaine_dev; }
// ---- base decentralisee ----
static uint32_t der_distant = 0;     // derniere reconnaissance par la base distante (colonne)
static volatile uint32_t a_telecharger = 0;
static uint32_t echec_id = 0, echec_t = 0;
static ResDistant res_dist;
static volatile bool res_dist_nouveau = false;
static const int BOITE = 200 * 1024;
static uint8_t *boite = nullptr;     // paquet recu de la base (PSRAM)
static volatile int boite_n = 0;
static volatile uint32_t boite_id = 0;
static uint8_t *sortie = nullptr;    // paquet a envoyer a la base (PSRAM)
static bool base_vue = false;        // une base de pubs a repondu (CMD_SYNCHRONISER)
static volatile int sortie_n = 0, sortie_bloc = -1;
static int16_t a_envoyer[32];
static int nenv = 0;
static const int NDC = 4096;
static emp::Code *dcodes = nullptr;  // codes pour la base (anneau, PSRAM)
static volatile uint32_t dc_e = 0, dc_l = 0;
static void envoyer_plus_tard(int b)
{
    if (b < 0) return;
    for (int i = 0; i < nenv; i++) if (a_envoyer[i] == b) return;
    if (nenv < 32) a_envoyer[nenv++] = (int16_t)b;
}
// une pub n'est comptee "vue" qu'une fois par episode PUB
static uint16_t vu_episode[base::MAX_BLOCS];
static uint16_t episode = 1;
static void voir(int b)
{
    if (b < 0 || b >= base::MAX_BLOCS || vu_episode[b] == episode) return;
    vu_episode[b] = episode;
    base::compter_vu(b);
}
static uint32_t pub_debut = 0, pub_fin = 0;
// consolidation : touches confirmees au fil de l'eau, passages couverts pendant l'episode
static uint32_t conf_jusqua = 0;
struct Couv { int bloc; int32_t pmin, pmax; };
static Couv ep_couv[16];
static int ep_n = 0;
static void noter_couverture(int bloc, int32_t tb)
{
    for (int i = 0; i < ep_n; i++)
        if (ep_couv[i].bloc == bloc) {
            if (tb < ep_couv[i].pmin) ep_couv[i].pmin = tb;
            if (tb > ep_couv[i].pmax) ep_couv[i].pmax = tb;
            return;
        }
    if (ep_n < 16) ep_couv[ep_n++] = Couv{ bloc, tb, tb };
}
static float pub_total = 0;
static int nb_ecrans = 0;
static uint32_t silence_depuis = 0;
static bool silencieux = false;
static uint8_t case_max = 0;

// ---- TESTER et enregistrement ----
static bool enreg = false;           // une pub inconnue est en cours d'enregistrement
static uint32_t enreg_t0 = 0;        // debut de l'enregistrement (appui sur TESTER)
static int test_etat = 0, test_num = 0;   // voir Vue
static bool test_appris = false;
static uint32_t test_t = 0, test_et_t = 0;  // appui sur TESTER, debut de l'etat de test
static int der_bloc = -1;            // derniere pub apprise (ANNULER)
static uint32_t der_t = 0;           // ... a cette colonne
static uint32_t version_sauvee = 0, t_sauve = 0;
static bool sauve_echec = false;
// silences : debut de chaque creux de son (pour que la reconnaissance de TESTER
// ne remonte pas jusqu'a la pub PRECEDENTE)
static uint32_t sil_t[128];
static uint32_t sil_n = 0;
static float niv_ref = -40;          // niveau habituel de la tele (lent)
static bool sil_etat = false;
static emp::Code *tampon = nullptr;  // codes extraits du journal (PSRAM)
static const int TAMPON = 32768;
static bool a_sauver = false;
static uint32_t ignore_jusqua = 0;   // fin de la derniere pub apprise : elle ne doit pas se reconnaitre elle-meme

// niveau du son (pour prevenir quand la carte apprend sans bien entendre)
static float niv_lisse = -90;                 // niveau des ~2 dernieres secondes
static float enreg_niv_somme = 0;
static uint32_t enreg_niv_n = 0;
static const float NIV_FAIBLE = -50;          // en dessous : la carte entend mal la tele

// files
static Evenement evs[16];
static int ev_e = 0, ev_l = 0;
struct Requete { int cmd, a, b, c; char texte[40]; };
static Requete cmds[8];
static int cmd_e = 0, cmd_l = 0;
static Vue v_partagee;

static const uint8_t PRIORITE[NB_ETATS] = { 0, 1, 2, 4, 5, 3, 0 };   // pour l'historique

// le test met la detection en pause pendant l'ecoute (3 s) et l'enregistrement
static bool test_pause() { return test_etat == 1 || test_etat == 4; }

static void emettre(int type, int a = 0, int b = 0, float x = 0)
{
    plat_verrou();
    if (ev_e - ev_l < 16) {
        Evenement &e = evs[ev_e & 15];
        e.type = type; e.a = a; e.b = b; e.x = x;
        ev_e++;
    }
    plat_deverrou();
}

static void journal_csv(const char *quoi, int a, float x)
{
    char l[80];
    int n = snprintf(l, sizeof l, "%lu,%s,%d,%.1f\n", (unsigned long)(plat_ms() / 1000), quoi, a, x);
    plat_fichier_ajouter("/pub/journal.csv", l, n);
}

bool evenement(Evenement &e)
{
    bool ok = false;
    plat_verrou();
    if (ev_l != ev_e) { e = evs[ev_l & 15]; ev_l++; ok = true; }
    plat_deverrou();
    return ok;
}

bool commande(int cmd, int a, int b, int c, const char *texte)
{
    Requete q = { cmd, a, b, c, {} };
    if (texte) { strncpy(q.texte, texte, sizeof q.texte - 1); q.texte[sizeof q.texte - 1] = 0; }
    bool ok = false;
    plat_verrou();
    if (cmd_e - cmd_l < 8) { cmds[(cmd_e++) & 7] = q; ok = true; }
    plat_deverrou();
    return ok;
}

// v0.9.53 : resultats des apprentissages de la captation APPRENDRE (8 derniers)
static ResAuto aa_res[8];
static volatile uint32_t aa_n = 0;

static void aa_publier(int statut, int numero, float duree, int doute = 0)
{
    plat_verrou();
    aa_res[aa_n & 7] = { statut, numero, duree, doute };
    aa_n = aa_n + 1;
    plat_deverrou();
}

bool auto_resultat(uint32_t &lu, ResAuto &r)
{
    bool ok = false;
    plat_verrou();
    if (aa_n - lu > 8) lu = aa_n - 8;
    if (lu != aa_n) { r = aa_res[lu & 7]; lu++; ok = true; }
    plat_deverrou();
    return ok;
}

void vue(Vue &v)
{
    plat_verrou();
    v = v_partagee;
    plat_deverrou();
}

void init()
{
    tampon = (emp::Code *)plat_malloc_grand(TAMPON * sizeof(emp::Code));
    fen = (emp::Code *)plat_malloc_grand(NF * sizeof(emp::Code));
    tou = (base::Touche *)plat_malloc_grand(NT * sizeof(base::Touche));
    tsel = (base::Touche *)plat_malloc_grand(NT * sizeof(base::Touche));
    // boite / sortie (2 x 200 Ko) : alloues seulement quand une base de pubs est
    // trouvee sur le reseau
    dcodes = (emp::Code *)plat_malloc_grand(NDC * sizeof(emp::Code));
    memset(historique, 0, sizeof historique);
    memset(vu_episode, 0, sizeof vu_episode);
    memset(&v_partagee, 0, sizeof v_partagee);
    v_partagee.etat = base::nb_pubs() ? PROGRAMME : APPRENTISSAGE;
}

// les touches plus vieilles que la fenetre (et une marge) sont oubliees
static void tou_compacter(uint32_t lim)
{
    int w = 0;
    for (int i = 0; i < tou_n; i++) if (tou[i].tq + 64 >= lim) tou[w++] = tou[i];
    tou_n = w;
}

static void tou_chercher(const emp::Code &c)
{
    if (!tou) return;
    if (tou_n > NT - 32) {
        uint32_t lim = col > FENETRE ? col - FENETRE : 0;
        tou_compacter(lim);
        if (tou_n > NT - 32) tou_n = 0;              // ne doit pas arriver
    }
    tou_n += base::chercher(c, tou + tou_n, 24);
}

// la bibliotheque a change (pub apprise, supprimee, decoupee, importee) :
// les touches des codes recents sont recalculees
static void tou_refaire()
{
    tou_version = base::version();
    tou_n = 0;
    uint32_t lim = col > FENETRE + 64 ? col - FENETRE - 64 : 0;
    for (int i = 0; i < fen_n; i++) if (fen[i].t >= lim) tou_chercher(fen[i]);
}

void codes(const emp::Code *c, int n)
{
    if (!fen) return;
    if (tou_version != base::version()) tou_refaire();
    for (int i = 0; i < n; i++) {
        if (fen_n == NF) { memmove(fen, fen + NF / 2, (NF / 2) * sizeof(emp::Code)); fen_n = NF / 2; }
        fen[fen_n++] = c[i];
        if (c[i].t > col) col = c[i].t;
        tou_chercher(c[i]);
    }
    // copie pour la base decentralisee (la tache reseau la vide toutes les 100 ms)
    if (dcodes && mode_base != 0 && actif) {
        plat_verrou();
        for (int i = 0; i < n; i++) {
            if (dc_e - dc_l >= (uint32_t)NDC) dc_l = dc_e - NDC + 1;   // personne ne lit : on oublie les plus vieux
            dcodes[dc_e % NDC] = c[i];
            dc_e = dc_e + 1;
        }
        plat_deverrou();
    }
}

int distant_codes(emp::Code *out, int max)
{
    if (!dcodes) return 0;
    int k = 0;
    plat_verrou();
    while (dc_l != dc_e && k < max) { out[k++] = dcodes[dc_l % NDC]; dc_l = dc_l + 1; }
    plat_deverrou();
    return k;
}

void distant_resultat(const ResDistant &r)
{
    plat_verrou();
    res_dist = r;
    res_dist_nouveau = true;
    plat_deverrou();
}

uint32_t distant_a_telecharger() { return a_telecharger; }

bool distant_paquet(const uint8_t *buf, int n, uint32_t id)
{
    if (!boite) boite = (uint8_t *)plat_malloc_grand(BOITE);
    if (!boite || n <= 0 || n > BOITE) return false;
    if (boite_n) return false;                       // le precedent n'est pas encore range
    memcpy(boite, buf, n);
    plat_verrou();
    boite_id = id;
    boite_n = n;
    plat_deverrou();
    return true;
}

bool sortie_prendre(const uint8_t *&buf, int &n, int &bloc)
{
    if (!sortie || sortie_n <= 0) return false;
    buf = sortie; n = sortie_n; bloc = sortie_bloc;
    return true;
}

void sortie_faite(int bloc, uint32_t id_distant)
{
    if (id_distant) commande(CMD_MARQUER_DISTANT, bloc, (int)id_distant);
    plat_verrou();
    sortie_n = 0;
    sortie_bloc = -1;
    plat_deverrou();
}

// garde les codes des 4 dernieres secondes
static int fenetre_recente(const emp::Code *&debut)
{
    if (!fen) { debut = nullptr; return 0; }
    uint32_t lim = col > FENETRE ? col - FENETRE : 0;
    if (lim <= ignore_jusqua) lim = ignore_jusqua + 1;
    int i = 0;
    while (i < fen_n && fen[i].t < lim) i++;
    if (i > 0 && i > fen_n / 2) { memmove(fen, fen + i, (fen_n - i) * sizeof(emp::Code)); fen_n -= i; i = 0; }
    debut = fen + i;
    return fen_n - i;
}

// touches dont le code d'ancrage est apres "apres" (et dans la fenetre)
static int touches_apres(uint32_t apres)
{
    if (!tou || !tsel) return 0;
    if (tou_version != base::version()) tou_refaire();
    uint32_t lim = col > FENETRE ? col - FENETRE : 0;
    if (lim <= ignore_jusqua) lim = ignore_jusqua + 1;
    if (apres + 1 > lim) lim = apres + 1;
    int k = 0;
    for (int i = 0; i < tou_n && k < NT / 2; i++) if (tou[i].tq >= lim) tsel[k++] = tou[i];
    return k;                                        // (la 2e moitie de tsel sert de brouillon)
}

// une pub vient d'etre apprise : sa chaine, son origine, et l'envoi a la base decentralisee
static void apres_ajout(int id)
{
    base::renommer(id, nullptr, chaine_courante());
    base::marquer_origine(id, base::ORIG_BOUTON, 0);
    base::marquer_a_verifier(id, true);              // apprise ici : a ecouter une fois
    if (mode_base != 0) envoyer_plus_tard(id);
}

// v0.9.53 : captation APPRENDRE : le passage [ma, mb[ (colonnes) a ete decoupe aux
// silences par la captation et rien ne l'a reconnu : il est appris comme avec
// ENREGISTRER, avec l'origine « auto » et la marque « a verifier ». Le reste de
// la detection (TESTER, verdict en cours) n'est pas touche.
// v0.9.58 : duree d'une pub classique (10, 15, 20, 25, 30, 40, 45 ou 60 s, a 1 s pres)
static bool duree_classique(float d)
{
    static const float D[] = { 10, 15, 20, 25, 30, 40, 45, 60 };
    for (float x : D) if (fabsf(d - x) <= 1.0f) return true;
    return false;
}

static void apprendre_auto(uint32_t ma, uint32_t mb, int chaine_et_silences)
{
    int chaine = chaine_et_silences & 255;
    bool deux_silences = (chaine_et_silences & 256) != 0;   // v0.9.58 : coupe a un vrai silence des deux cotes
    bool coupure = (chaine_et_silences & 512) != 0;          // v0.9.64 : entre deux pubs reconnues pendant une coupure : toujours « a verifier »
    if (mb > emp::colonne()) mb = emp::colonne();
    if (ma < base::journal_plus_ancien()) ma = base::journal_plus_ancien();
    if (mb - ma > BLOC_MAX) mb = ma + BLOC_MAX;
    float duree = mb > ma ? (mb - ma) / IPS : 0;
    if (mb < ma + BLOC_MIN || !tampon) {
        if (coupure) plat_log("[PUB] Apprendre (coupure) : passage de %.0f s trop court : rien appris\n", duree);   // v0.9.67
        aa_publier(AA_VIDE, 0, duree); return;
    }
    int n = base::journal_extraire(ma, mb, tampon, TAMPON);
    if (n < 40) {                                                     // presque silencieux
        if (coupure) plat_log("[PUB] Apprendre (coupure) : %.0f s presque sans son (%d codes) : rien appris\n", duree, n);
        aa_publier(AA_VIDE, 0, duree); return;
    }
    base::Resultat r;
    base::reconnaitre(tampon, n, r);
    // v0.9.67 : pendant une coupure (sandwich), « deja connue » seulement si la pub reconnue couvre
    // au moins 60 % du passage : un petit bout d'une pub voisine qui deborde ne suffit plus
    float couvert = r.bloc >= 0 && duree > 0 ? (r.etendue / IPS) / duree : 0;
    bool connue = r.bloc >= 0 && r.score >= seuil_votes && base::bloc(r.bloc).type == base::PUB && (!coupure || couvert >= 0.6f);
    if (coupure && r.bloc >= 0 && r.score >= seuil_votes && !connue)
        plat_log("[PUB] Apprendre (coupure) : ressemble à la pub n°%lu (score %d) mais sur %d %% du passage seulement : appris quand même\n",
                 (unsigned long)base::bloc(r.bloc).numero, (int)r.score, (int)(couvert * 100 + 0.5f));
    if (connue) {
        if (coupure)
            plat_log("[PUB] Apprendre (coupure) : %.0f s = pub n°%lu déjà connue (score %d, %d %% du passage) : rien appris\n",
                     duree, (unsigned long)base::bloc(r.bloc).numero, (int)r.score, (int)(couvert * 100 + 0.5f));
        // v0.9.58 : une pub apprise « a verifier » qui repasse : c'est bien une pub, elle est validee
        int revue = 0;
        if (base::a_verifier(r.bloc) && base::info(r.bloc).origine == base::ORIG_AUTO) {
            base::marquer_a_verifier(r.bloc, false);
            a_sauver = true;
            revue = 1;
            plat_log("[PUB] Apprendre (captation) : pub n°%lu entendue une 2e fois : validée\n", (unsigned long)base::bloc(r.bloc).numero);
        }
        aa_publier(AA_CONNUE, (int)base::bloc(r.bloc).numero, duree, revue);   // pas de doublon
        return;
    }
    // v0.9.58 : sure (deux vrais silences, duree de pub classique, ne ressemble a rien de connu) -> OK tout de suite
    int doute = base::DOUTE_AUCUN;
    if (r.bloc >= 0 && base::bloc(r.bloc).type == base::PUB && r.score >= 6) doute = base::DOUTE_RESSEMBLE;
    else if (!deux_silences) doute = base::DOUTE_SILENCE;
    else if (!duree_classique(duree)) doute = base::DOUTE_DUREE;
    else if (coupure) doute = base::DOUTE_COUPURE;
    int id = base::ajouter_bloc(tampon, n, ma, (int)(duree + 0.5f));
    if (id < 0) {
        if (coupure) plat_log("[PUB] Apprendre (coupure) : bibliothèque pleine : rien appris\n");
        aa_publier(AA_PLEINE, 0, duree); return;
    }
    base::renommer(id, nullptr, chaine > 0 ? chaine : chaine_courante());
    base::marquer_origine(id, base::ORIG_AUTO, 0);
    base::marquer_a_verifier(id, doute != base::DOUTE_AUCUN);   // doute : a ecouter (portail ou gestion_pubs.py)
    base::marquer_doute(id, doute);
    if (mode_base != 0) envoyer_plus_tard(id);
    base::marquer_audio(id, plat_audio_extraire(ma, mb, base::bloc(id).numero));
    journal_csv(doute ? "pub_apprise_auto" : "pub_apprise_auto_sure", (int)base::bloc(id).numero, duree);
    a_sauver = true;
    plat_log("[PUB] Apprendre (%s) : pub n°%lu apprise, %.0f s, %s\n", coupure ? "coupure" : "captation", (unsigned long)base::bloc(id).numero, duree,
             doute == base::DOUTE_AUCUN ? "sûre (validée)" : doute == base::DOUTE_RESSEMBLE ? "à vérifier (ressemble à une pub connue)"
             : doute == base::DOUTE_SILENCE ? "à vérifier (pas de silence d'un côté)" : doute == base::DOUTE_COUPURE ? "à vérifier (entre deux pubs, pendant une coupure)"
             : "à vérifier (durée inhabituelle)");
    aa_publier(AA_APPRISE, (int)base::bloc(id).numero, duree, doute);
}

// ---- v0.9.58 : fichier AUTO traite depuis le portail ----
static const emp::Code *fi_codes = nullptr;
static int fi_n = 0;
static uint32_t fi_t0 = 0;
static float fi_duree = 0;
static volatile uint32_t fi_seq = 0;
static ResFichier fi_res = { 0, 0, 0, 0 };

void fichier_poser(const emp::Code *c, int n, uint32_t t0, float duree_s)
{
    fi_codes = c; fi_n = n; fi_t0 = t0; fi_duree = duree_s;
}
uint32_t fichier_seq() { return fi_seq; }
bool fichier_resultat(uint32_t seq_avant, ResFichier &r)
{
    plat_verrou();
    bool ok = fi_seq != seq_avant;
    if (ok) r = fi_res;
    plat_deverrou();
    return ok;
}

static void fi_publier(int statut, int numero, int pct)
{
    plat_verrou();
    fi_res.statut = statut; fi_res.numero = numero; fi_res.pct = pct;
    fi_res.seq = fi_seq + 1;
    fi_seq = fi_seq + 1;
    plat_deverrou();
}

// compare le fichier a la bibliotheque ; ajouter : l'apprend s'il n'y est pas (OK : tu l'as ecoute)
static void traiter_fichier(bool ajouter, int chaine)
{
    if (!fi_codes || fi_n < 40 || fi_duree < 3) { fi_publier(FI_VIDE, 0, 0); return; }
    base::Resultat r;
    base::reconnaitre(fi_codes, fi_n, r);
    int statut = FI_NOUVELLE, numero = 0, pct = 0;
    if (r.bloc >= 0 && base::bloc(r.bloc).type == base::PUB) {
        numero = (int)base::bloc(r.bloc).numero;
        // part du fichier ET de la pub couverte par les codes concordants (comme gestion_pubs.py)
        float couvert = r.etendue / IPS;
        float dp = base::bloc(r.bloc).duree_s > 0 ? (float)base::bloc(r.bloc).duree_s : fi_duree;
        float cf = couvert / fi_duree, cp = couvert / dp;
        float c = cf < cp ? cf : cp;
        pct = (int)(100 * (c > 1 ? 1 : c));
        if (r.score >= seuil_votes && cf >= 0.6f && cp >= 0.6f) statut = FI_CONNUE;
        else if (r.score >= 6) statut = FI_RESSEMBLE;
        else numero = 0;
    }
    if (ajouter && statut != FI_CONNUE) {
        int id = base::ajouter_bloc(fi_codes, fi_n, fi_t0, (int)(fi_duree + 0.5f));
        if (id < 0) { fi_publier(FI_PLEINE, 0, 0); return; }
        base::renommer(id, nullptr, chaine > 0 ? chaine : chaine_courante());
        base::marquer_origine(id, base::ORIG_AUTO, 0);
        base::marquer_a_verifier(id, false);         // validee sur le portail : tu l'as ecoutee
        base::marquer_audio(id, true);               // le son est ecrit juste apres par la tache des fichiers
        if (mode_base != 0) envoyer_plus_tard(id);
        journal_csv("pub_validee_portail", (int)base::bloc(id).numero, fi_duree);
        a_sauver = true;
        plat_log("[PUB] Portail : pub n°%lu ajoutée depuis un fichier AUTO, %.0f s\n", (unsigned long)base::bloc(id).numero, fi_duree);
        fi_publier(FI_AJOUTEE, (int)base::bloc(id).numero, 0);
        return;
    }
    fi_publier(statut, numero, pct);
}

// ENREGISTRER : fin de la pub. Tout le passage, de l'appui sur TESTER a
// maintenant, est appris tel quel (pas de decoupe aux silences).
static void fermer_bloc()
{
    enreg = false;
    uint32_t ma = enreg_t0, mb = emp::colonne();
    if (mb - ma > BLOC_MAX) mb = ma + BLOC_MAX;
    float niv_moy = enreg_niv_n ? enreg_niv_somme / enreg_niv_n : niv_lisse;
    float duree = (mb - ma) / IPS;
    int id = -1, num_connue = 0;
    bool plein = false;
    if (mb >= ma + BLOC_MIN && tampon) {
        int n = base::journal_extraire(ma, mb, tampon, TAMPON);
        if (n >= 40) {                               // sinon : presque silencieux
            base::Resultat r;
            base::reconnaitre(tampon, n, r);
            if (r.bloc >= 0 && r.score >= seuil_votes && base::bloc(r.bloc).type == base::PUB)
                num_connue = (int)base::bloc(r.bloc).numero;   // deja connue : pas de doublon
            else {
                id = base::ajouter_bloc(tampon, n, ma, (int)(duree + 0.5f));
                if (id < 0) plein = true;
            }
        }
    }
    ignore_jusqua = mb;
    test_et_t = emp::colonne();
    if (plein) {
        emettre(EV_BLOC_PLEIN);
        der_bloc = -1;
        test_etat = 0;
        test_appris = false;
        test_num = 0;
        return;
    }
    if (id >= 0) {
        apres_ajout(id);
        base::marquer_audio(id, plat_audio_extraire(ma, mb, base::bloc(id).numero));
        journal_csv("pub_apprise", (int)base::bloc(id).numero, duree);
        a_sauver = true;
        der_bloc = id;
        der_t = emp::colonne();
        test_etat = 2;
        test_appris = true;
        test_num = (int)base::bloc(id).numero;
    } else {
        der_bloc = -1;
        test_etat = 0;
        test_appris = false;
        test_num = num_connue;
    }
    plat_log("[PUB] Enregistrer : %.0f s, %s\n", duree, id >= 0 ? "pub apprise" : (num_connue ? "deja connue" : "rien appris (trop court ou silencieux)"));
    emettre(EV_TEST, test_num, id >= 0 ? 6 : 7, duree);
    if (id >= 0 && niv_moy < NIV_FAIBLE) emettre(EV_SON_FAIBLE, (int)niv_moy);
}

void sauvegarde(bool &a_jour, uint32_t &il_y_a_s, bool &echec)
{
    // v0.9.27 : apres le demarrage, la base lue sur la carte EST la version sauvee (avant : "pas a jour"
    // jusqu'a la premiere modification, ce qui bloquait le telechargement de la base complete)
    if (!t_sauve && version_sauvee == 0 && !a_sauver) version_sauvee = base::version();
    a_jour = version_sauvee == base::version() && !a_sauver;
    il_y_a_s = t_sauve ? (plat_ms() - t_sauve) / 1000 : 0xFFFFFFFF;
    echec = sauve_echec;
}

// TESTER, apres 3 s d'ecoute : la pub qui passe est-elle connue ?
static void conclure_test(uint32_t maintenant)
{
    // fenetre : depuis le silence qui ouvre la pub en cours (9 s au plus avant
    // l'appui), pour ne pas reconnaitre la fin de la pub PRECEDENTE
    uint32_t t0 = test_t > (uint32_t)(9 * IPS) ? test_t - (uint32_t)(9 * IPS) : 0;
    for (uint32_t k = sil_n > 128 ? sil_n - 128 : 0; k < sil_n; k++) {
        uint32_t ts = sil_t[k % 128];
        if (ts > t0 && ts + (uint32_t)IPS < test_t) t0 = ts;
    }
    int nb = tampon ? base::journal_extraire(t0, maintenant, tampon, TAMPON) : 0;
    test_et_t = maintenant;
    if (nb < 40) { test_etat = 0; emettre(EV_TEST, 0, 3); return; }
    base::Resultat r;
    base::reconnaitre(tampon, nb, r);
    // reconnue seulement si elle « joue encore » au moment de l'appui
    bool en_cours = r.t_dernier + (uint32_t)(2 * IPS) >= test_t;
    if (r.bloc >= 0 && r.score >= seuil_votes && en_cours && base::bloc(r.bloc).type == base::PUB) {
        test_etat = 2;
        test_appris = false;
        test_num = (int)base::bloc(r.bloc).numero;
        // PUB tout de suite : la reconnaissance continue reprend a partir d'ici
        // (le son est baisse sans attendre)
        if (actif) {
            voir(r.bloc);
            bloc_cour = r.bloc;
            pos_cour = r.pos;
            dec_cour = r.decalage;
            score = r.score;
            if (r.t_dernier > der_match) der_match = r.t_dernier;
            if (base::info(r.bloc).chaine > 0) chaine_dev = base::info(r.bloc).chaine;
        }
        emettre(EV_TEST, test_num, 0, (float)base::bloc(r.bloc).vus);
        return;
    }
    // inconnue : l'enregistrement part de l'appui sur TESTER (c'est l'utilisateur
    // qui dit ou la pub commence) et dure jusqu'a ENREGISTRER
    // v0.9.5 : le debut de la pub est avant l'appui (temps de reaction) : on recule jusqu'au
    // dernier silence des 5 s precedentes, sinon de 2 s
    uint32_t debut = test_t > (uint32_t)(2 * IPS) ? test_t - (uint32_t)(2 * IPS) : 0;
    {
        uint32_t lim = test_t > (uint32_t)(5 * IPS) ? test_t - (uint32_t)(5 * IPS) : 0, ds = 0;
        for (uint32_t k = sil_n > 128 ? sil_n - 128 : 0; k < sil_n; k++) {
            uint32_t ts = sil_t[k % 128];
            if (ts > lim && ts + (uint32_t)IPS < test_t) ds = ts;
        }
        if (ds) debut = ds;
    }
    if (debut < base::journal_plus_ancien()) debut = base::journal_plus_ancien();
    enreg = true;
    enreg_t0 = debut;
    enreg_niv_somme = 0;
    enreg_niv_n = 0;
    test_etat = 4;
    test_num = 0;
    test_appris = false;
    emettre(EV_TEST, 0, 5);
    if (niv_lisse < NIV_FAIBLE) emettre(EV_SON_FAIBLE, (int)niv_lisse);
}

static void executer(const Requete &q)
{
    int c = q.cmd;
    uint32_t maintenant = emp::colonne();
    switch (c) {
    case CMD_SUPPRIMER:
        if (q.a >= 0 && q.a < base::MAX_BLOCS && base::bloc(q.a).actif) {
            int num = (int)base::bloc(q.a).numero;
            if (bloc_cour == q.a) { bloc_cour = -1; der_match = 0; }
            if (der_bloc == q.a) der_bloc = -1;
            base::supprimer_bloc(q.a, "supprimée (à ta demande)");
            a_sauver = true;
            emettre(EV_SUPPRIME, num);
        }
        break;
    case CMD_RENOMMER:
        base::renommer(q.a, q.texte[0] ? q.texte : nullptr, q.b);
        base::marquer_a_verifier(q.a, false);           // tu l'as regardee
        a_sauver = true;
        break;
    case CMD_TESTER:
        // debut de la pub : la carte ecoute encore 3 s, puis compare
        if (enreg || test_pause()) { emettre(EV_DEJA_OUVERT); break; }
        test_etat = 1;
        test_t = maintenant;
        test_et_t = maintenant;
        test_num = 0;
        test_appris = false;
        break;
    case CMD_ENREGISTRER:
        // fin de la pub : l'empreinte est sauvee
        if (enreg) fermer_bloc();
        else emettre(EV_RIEN_A_ANNULER);
        break;
    case CMD_APPRENDRE_AUTO:
        apprendre_auto((uint32_t)q.a, (uint32_t)q.b, q.c);
        break;
    case CMD_FICHIER:
        traiter_fichier(q.a != 0, q.b);
        break;
    case CMD_SUPPRIMER_A_VERIFIER: {
        int n = 0;
        for (int i = 0; i < base::MAX_BLOCS; i++)
            if (base::bloc(i).actif && base::bloc(i).type == base::PUB && base::a_verifier(i)) {
                if (bloc_cour == i) { bloc_cour = -1; der_match = 0; }
                if (der_bloc == i) der_bloc = -1;
                base::supprimer_bloc(i, "supprimée (à vérifier, depuis le portail)");
                n++;
            }
        if (n) a_sauver = true;
        plat_log("[PUB] Portail : %d pub(s) « à vérifier » supprimée(s)\n", n);
        break;
    }
    case CMD_PURGER: {
        // pubs jamais revues : apprises il y a plus de q.a jours (ou a une date
        // inconnue), jamais reconnues depuis, et deja ecoutees (pas « a verifier »)
        uint32_t maint_h = plat_heure(), age = (uint32_t)(q.a > 0 ? q.a : 3) * 86400u;
        int k = 0;
        for (int i = 0; i < base::MAX_BLOCS; i++) {
            const base::Bloc &bl = base::bloc(i);
            if (!bl.actif || bl.type != base::PUB || bl.vus || base::a_verifier(i)) continue;
            const base::Info &in = base::info(i);
            if (in.origine == base::ORIG_CACHE) continue;
            if (in.appris_le && (!maint_h || maint_h - in.appris_le < age)) continue;
            if (bloc_cour == i) bloc_cour = -1;
            if (der_bloc == i) der_bloc = -1;
            base::supprimer_bloc(i, "supprimée : jamais revue (purge)");
            k++;
        }
        if (k) a_sauver = true;
        emettre(EV_PURGE, k);
        break;
    }
    case CMD_GARDER:
        base::marquer_a_verifier(q.a, false);
        a_sauver = true;
        break;
    case CMD_COUPER: {
        int t0 = (int)(q.b * IPS / 10), t1 = (int)(q.c * IPS / 10);
        int num = (int)base::bloc(q.a).numero;
        int r = base::garder_plage(q.a, t0, t1);
        if (bloc_cour == q.a) der_match = 0;
        a_sauver = true;
        emettre(EV_COUPE, num, r);
        break;
    }
    case CMD_EXPORTER:
        resultat = base::exporter(q.texte, q.a);
        nb_resultats = nb_resultats + 1;
        emettre(EV_EXPORTE, resultat);
        break;
    case CMD_IMPORTER: {
        int n = base::importer(q.texte);
        if (n > 0) a_sauver = true;
        resultat = n;
        nb_resultats = nb_resultats + 1;
        emettre(EV_IMPORTE, n);
        break;
    }
    case CMD_PAS_PUB:
        cpt_pas_pub++;
        if (der_match && (en_pub || maintenant - der_match < MAINTIEN) && bloc_cour >= 0) {
            int num = (int)base::bloc(bloc_cour).numero;
            int r = base::supprimer_plage(bloc_cour, pos_cour - (int)(10 * IPS), pos_cour + (int)(2 * IPS));
            der_match = 0;
            a_sauver = true;
            emettre(EV_CORRECTION, num, r);
            journal_csv("correction", num, (float)r);
        } else {
            emettre(EV_RIEN_A_CORRIGER);
        }
        break;
    case CMD_ANNULER:
        if (enreg || test_etat == 1) {
            // pendant l'ecoute ou l'enregistrement : rien n'est appris, la detection reprend
            enreg = false;
            test_etat = 0;
            test_appris = false;
            test_et_t = maintenant;
            emettre(EV_ANNULE, 1);
        } else if (der_bloc >= 0 && base::bloc(der_bloc).actif && maintenant - der_t < (uint32_t)(ANNULER_S * IPS)) {
            int num = (int)base::bloc(der_bloc).numero;
            if (bloc_cour == der_bloc) bloc_cour = -1;
            base::supprimer_bloc(der_bloc, "oubliée (Annuler)");
            der_bloc = -1;
            a_sauver = true;
            if (test_etat == 2) { test_etat = 0; test_appris = false; test_et_t = maintenant; }
            emettre(EV_ANNULE, 2, num);
        } else emettre(EV_RIEN_A_ANNULER);
        break;
    case CMD_MARQUER_DISTANT:
        if (q.a >= 0 && q.a < base::MAX_BLOCS && base::bloc(q.a).actif) {
            int orig = mode_base == 1 ? base::ORIG_CACHE : base::info(q.a).origine;   // decentralisee : la carte n'est qu'un cache
            base::marquer_origine(q.a, orig, (uint32_t)q.b);
            a_sauver = true;
        }
        break;
    case CMD_SYNCHRONISER:
        base_vue = true;                               // une base de pubs repond : on peut lui envoyer
        if (mode_base != 0)
            for (int i = 0; i < base::MAX_BLOCS; i++)
                if (base::bloc(i).actif && base::bloc(i).type == base::PUB && !base::info(i).distant &&
                    base::info(i).origine != base::ORIG_CACHE) envoyer_plus_tard(i);
        break;
    case CMD_EFFACER:
        base::effacer();
        plat_audio_supprimer(AUDIO_TOUS);              // et les sons des pubs (sinon la pub n°1 suivante aurait l'ancien son)
        enreg = false;
        test_etat = 0;
        der_bloc = -1;
        der_match = 0;
        bloc_cour = -1;
        a_sauver = true;
        emettre(EV_EFFACE);
        break;
    }
}

void periodique(float niveau_db, bool sature)
{
    (void)sature;
    // commandes en attente
    for (;;) {
        Requete q = {};
        plat_verrou();
        if (cmd_l != cmd_e) q = cmds[(cmd_l++) & 7];
        plat_deverrou();
        if (!q.cmd) break;
        executer(q);
    }
    uint32_t maintenant = emp::colonne();

    // ---- silences : le son tombe 18 dB sous son niveau habituel (ou sous -58 dB) ----
    niv_lisse += (niveau_db - niv_lisse) * 0.02f;
    {
        if (niveau_db > -60 && niveau_db > niv_ref - 15) niv_ref += (niveau_db - niv_ref) * 0.05f;
        bool sil = niveau_db < niv_ref - 18 || niveau_db < -58;
        if (sil && !sil_etat) { sil_t[sil_n % 128] = emp::colonne(); sil_n++; }
        sil_etat = sil;
    }

    // ---- TESTER ----
    if (test_etat == 1 && maintenant >= test_t + (uint32_t)(3 * IPS)) conclure_test(maintenant);
    // resultat affiche 8 s (20 s si la pub vient d'etre apprise : ANNULER possible)
    if (test_etat == 2 && maintenant - test_et_t > (uint32_t)((test_appris ? ANNULER_S : 8) * IPS)) {
        test_etat = 0;
        test_appris = false;
        test_et_t = maintenant;
    }
    if (enreg) {
        // l'enregistrement dure jusqu'a ENREGISTRER (pas d'arret au silence) ;
        // oublie au-dela de ENREG_MAX_S : rien n'est appris
        enreg_niv_somme += niveau_db;
        enreg_niv_n++;
        if (maintenant - enreg_t0 > (uint32_t)(ENREG_MAX_S * IPS)) {
            enreg = false;
            test_etat = 0;
            test_appris = false;
            test_num = 0;
            test_et_t = maintenant;
            der_bloc = -1;
            plat_log("[PUB] Tester : %d s sans ENREGISTRER, rien appris\n", ENREG_MAX_S);
            emettre(EV_TEST, 0, 8, (float)ENREG_MAX_S);
        }
    }

    // niveau pendant la pub et creux entre deux spots
    if (en_pub) {
        if (niveau_db > -60 && niveau_db > niv_pub - 15) niv_pub += (niveau_db - niv_pub) * 0.05f;
        if (niveau_db < niv_pub - 18 || niveau_db < -58) creux_t = emp::colonne();
    }
    // ---- base decentralisee : reponse, paquet recu, paquet a envoyer ----
    if (res_dist_nouveau) {
        ResDistant rd;
        plat_verrou();
        rd = res_dist;
        res_dist_nouveau = false;
        plat_deverrou();
        // les jingles de la base sont ignores (v0.6.0)
        if (mode_base != 0 && actif && !test_etat && rd.id && rd.score >= seuil_votes && rd.type == base::PUB &&
            base::trouver_distant(rd.id) < 0 && rd.t_dernier > ignore_jusqua) {
            if (!der_distant || maintenant - der_distant > (uint32_t)(10 * IPS)) {
                emettre(EV_DISTANT, (int)rd.id);
                journal_csv("pub_distante", (int)rd.id, 0);
            }
            if (rd.t_dernier > der_distant) der_distant = rd.t_dernier;
            if (rd.chaine > 0) chaine_dev = rd.chaine;
            // on la copie ici : la suite et les prochains passages seront reconnus sans reseau
            bool echoue_recemment = rd.id == echec_id && plat_ms() - echec_t < 300000;
            if (!a_telecharger && !echoue_recemment) a_telecharger = rd.id;
        }
    }
    if (boite_n > 0) {
        int premier = -1;
        uint32_t id = boite_id;
        int k = base::importer_tampon(boite, boite_n, base::ORIG_CACHE, id, &premier);
        plat_verrou();
        boite_n = 0;
        plat_deverrou();
        if (k > 0 && premier >= 0) {
            a_sauver = true;
            emettre(EV_CACHE, (int)base::bloc(premier).numero);
            journal_csv("copie_distante", (int)base::bloc(premier).numero, 0);
        } else if (k < 0 || base::trouver_distant(id) < 0) { echec_id = id; echec_t = plat_ms(); }
        if (a_telecharger == id) a_telecharger = 0;
    }
    if (mode_base != 0 && base_vue && !sortie) sortie = (uint8_t *)plat_malloc_grand(BOITE);
    if (mode_base != 0 && base_vue && sortie && sortie_n == 0 && nenv > 0) {
        int b = a_envoyer[0];
        for (int i = 1; i < nenv; i++) a_envoyer[i - 1] = a_envoyer[i];
        nenv--;
        if (base::bloc(b).actif && !base::info(b).distant) {
            int n = base::exporter_bloc(b, sortie, BOITE);
            if (n > 0) {
                plat_verrou();
                sortie_bloc = b;
                sortie_n = n;
                plat_deverrou();
            }
        }
    }
    // silence : moins de -58 dB plein echelle pendant 3 s
    if (niveau_db < -58) {
        if (!silence_depuis) silence_depuis = maintenant ? maintenant : 1;
        silencieux = maintenant - silence_depuis > (uint32_t)(3 * IPS);
    } else {
        silence_depuis = 0;
        silencieux = false;
    }

    const emp::Code *f;
    int n = fenetre_recente(f);
    // codes FORTS de la fenetre : les statistiques de fin de pub sont calibrees sur eux
    int n_forts = 0;
    for (int i = 0; i < n; i++) n_forts += !(f[i].h & emp::FAIBLE);
    // pendant l'ecoute et l'enregistrement de TESTER, la detection est arretee
    if (!test_pause() && maintenant >= prochain_test) {               // reconnaissance 15 fois par seconde
        prochain_test = maintenant + 2;
        base::Resultat r;
        int nt = touches_apres(0);
        base::reconnaitre_touches(tsel, nt, r);
        score = r.score;
        second_ = r.second;
        int seuil = seuil_votes;
        if (r.bloc >= 0 && base::bloc(r.bloc).type != base::PUB) r.bloc = -1;   // anciens jingles : ignores
        // une NOUVELLE pub doit aussi avoir des votes de codes forts : les codes
        // faibles seuls trouvent parfois une ressemblance partielle (fond musical)
#ifndef PART_FORTE
#define PART_FORTE 0.3f
#endif
        bool nouvelle = !en_pub || r.bloc != bloc_cour;
        if (r.bloc >= 0 && nouvelle && r.score_fort < (int)(PART_FORTE * seuil + 0.5f)) r.bloc = -1;
        if (r.bloc >= 0 && r.score >= seuil) {
            voir(r.bloc);
            bloc_cour = r.bloc;
            pos_cour = r.pos;
            dec_cour = r.decalage;
            uint32_t td = r.t_dernier ? r.t_dernier : 1;   // date du son, pas du calcul
            if (td > der_match) der_match = td;
            int chp = base::info(r.bloc).chaine;
            if (chp > 0) chaine_dev = chp;
        }
        if (en_pub && der_match) {
            // pendant une pub reconnue : le son qui suit le dernier accord est-il une
            // AUTRE pub connue (spot suivant) ? On la cherche dans les seuls codes
            // recents, avec un seuil reduit : l'enchainement de deux spots appris ne
            // fait pas retomber le bandeau.
            base::Resultat r2;
            int nt2 = touches_apres(der_match);
            base::reconnaitre_touches(tsel, nt2, r2);
            int s2 = seuil / 3 > S2_MIN ? seuil / 3 : S2_MIN;
            if (r2.bloc >= 0 && base::bloc(r2.bloc).type == base::PUB && r2.score >= s2 && r2.t_dernier > der_match) {
                voir(r2.bloc);
                bloc_cour = r2.bloc;
                pos_cour = r2.pos;
                dec_cour = r2.decalage;
                der_match = r2.t_dernier;
            }
        }
    }
    // suivi fin de la pub reconnue : chaque code est compare a (bloc, decalage)
    if (der_match && bloc_cour >= 0 && n > 0 && maintenant < der_match + 2 * MAINTIEN_MAX) {
        int nt = touches_apres(0);
        uint32_t dern = 0, dern_f = 0;
        int k_tous = base::accords_touches(tsel, nt, bloc_cour, dec_cour, dern, false);
        // une touche faible isolee peut tomber la par hasard (salon bruyant) : seules
        // les preuves fortes, ou au moins 3 touches concordantes dans la derniere
        // seconde, prolongent la pub
        uint32_t dern_conf = 0;
        {
            uint32_t lim1 = maintenant > (uint32_t)IPS ? maintenant - (uint32_t)IPS : 0;
            int nt1 = 0;
            for (int i = 0; i < nt; i++) if (tsel[i].tq >= lim1) tsel[NT / 2 + nt1++] = tsel[i];
            uint32_t d1 = 0;
            int k1 = nt1 ? base::accords_touches(tsel + NT / 2, nt1, bloc_cour, dec_cour, d1, false) : 0;
#ifndef K1_MIN
#define K1_MIN 3
#endif
            if (k1 >= K1_MIN) dern_conf = d1;
        }
        if (en_pub && maintenant - der_match < (uint32_t)IPS) {
            float duree = (float)FENETRE / IPS;
            float r_inst = k_tous / duree;
            rythme = rythme * 0.95f + r_inst * 0.05f;
        }
        int k = base::accords_touches(tsel, nt, bloc_cour, dec_cour, dern_f, true);
#ifndef KF_MIN
#define KF_MIN 2
#endif
        // une preuve forte isolee peut etre le hasard : il en faut KF_MIN dans les 1,5 s
        if (dern_f && KF_MIN > 1) {
            uint32_t lim15 = dern_f > (uint32_t)(1.5f * IPS) ? dern_f - (uint32_t)(1.5f * IPS) : 0;
            int nt15 = 0;
            for (int i = 0; i < nt; i++) if (tsel[i].tq >= lim15 && tsel[i].fort) tsel[NT / 2 + nt15++] = tsel[i];
            uint32_t d15 = 0;
            int k15 = nt15 ? base::accords_touches(tsel + NT / 2, nt15, bloc_cour, dec_cour, d15, true) : 0;
            if (k15 < KF_MIN) dern_f = 0;
        }
        uint32_t preuve = dern_f > dern_conf ? dern_f : dern_conf;
        if (preuve > der_match) der_match = preuve;
        if (n_forts > 0) {
            float taux = (float)k / n_forts;
            if (taux > 1) taux = 1;
            taux_pub = en_pub ? taux_pub * 0.9f + taux * 0.1f : taux;
        }
    }
    // confirmations : les touches completes (cible arrivee, 42 colonnes plus tard)
    // qui concordent avec la pub reconnue marquent leur entree "reentendue"
    {
        uint32_t lim = maintenant > 42 ? maintenant - 42 : 0;
        if (lim > conf_jusqua) {
            if (en_pub && bloc_cour >= 0 && der_match && tou) {
                for (int i = 0; i < tou_n; i++) {
                    const base::Touche &t = tou[i];
                    if (t.tq <= conf_jusqua || t.tq > lim || t.bloc != bloc_cour) continue;
                    int32_t d = (int32_t)t.tq - (int32_t)t.tb - dec_cour;
                    if (d < -2 || d > 3) continue;
                    base::confirmer(t.idx, t.bloc);
                    noter_couverture(t.bloc, t.tb);
                }
            }
            conf_jusqua = lim;
        }
    }
    // etat
    bool pub_connue = false;
    if (der_match) {
        int apres = 0;                                // codes forts entendus depuis le dernier son reconnu
        for (int i = 0; i < n; i++) if (f[i].t > der_match && !(f[i].h & emp::FAIBLE)) apres++;
        float p = taux_pub < 0.05f ? 0.05f : (taux_pub > 0.8f ? 0.8f : taux_pub);
        int besoin = (int)ceilf(logf(FIN_RISQUE) / logf(1 - p));
        if (besoin < FIN_MIN) besoin = FIN_MIN;          // au moins ~0,6 s de son : laisse le temps de reconnaitre le spot suivant
        if (besoin > 150) besoin = 150;
        uint32_t ecart = maintenant > der_match ? maintenant - der_match : 0;
        // trou de preuves "anormal" pour le rythme mesure (risque 1/10 000)
        float r = rythme < 2.0f ? 2.0f : rythme;
#ifndef TROU_K
#define TROU_K 9.2f
#endif
        uint32_t trou = (uint32_t)(TROU_K / r * IPS);
        if (trou < ECART_MIN) trou = ECART_MIN;
        if (trou > MAINTIEN_MAX) trou = MAINTIEN_MAX;
        bool preuve_fin = ecart >= trou && apres >= besoin;
        // creux de son apres la derniere reconnaissance : on attend le spot suivant
        if (preuve_fin && creux_t + 8 >= der_match && maintenant - creux_t < APRES_CREUX) preuve_fin = false;
        uint32_t maintien = trou > MAINTIEN ? trou : MAINTIEN;
        pub_connue = !preuve_fin && ecart < maintien;
    }
    // pub reconnue par la base distante, en attendant que sa copie arrive ici
    bool pub_distante = der_distant && maintenant - der_distant < MAINTIEN;
    if (!der_distant || maintenant - der_distant > (uint32_t)(30 * IPS)) der_distant = 0;
    bool par_distant = pub_distante && !pub_connue;
    if (par_distant) pub_connue = true;
    pub_distante = par_distant;
    if (!actif || test_pause()) pub_connue = false;   // detecteur arrete ou test en cours : jamais PUB
    int e;
    if (!actif || test_pause()) e = PAUSE;            // test : le son n'est pas coupe, rien n'est detecte
    else if (pub_connue) e = PUB;
    else if (silencieux) e = SILENCE;
    else if (base::nb_pubs() == 0) e = APPRENTISSAGE;
    else e = PROGRAMME;
    etat = e;

    if (pub_connue && !en_pub) {
        en_pub = true;
        if (!pub_fin || maintenant - pub_fin > (uint32_t)(60 * IPS)) nb_ecrans++;
        pub_debut = maintenant;
        int num = bloc_cour >= 0 ? (int)base::bloc(bloc_cour).numero : 0;
        emettre(EV_PUB_DEBUT, num);
        journal_csv("pub_debut", num, 0);
        if (jingle_hp >= 1) plat_son(SON_PUB_DEBUT);
    } else if (!pub_connue && en_pub) {
        en_pub = false;
        pub_fin = maintenant;
        uint32_t fin_t = der_match > pub_debut ? der_match : maintenant;
        float d = (float)(fin_t - pub_debut) / IPS + 1;
        emettre(EV_PUB_FIN, 0, 0, d);
        journal_csv("pub_fin", 0, d);
        if (jingle_hp >= 2) plat_son(SON_PUB_FIN);
        if (++episode == 0) episode = 1;
        // fin d'episode : passages reconnus -> couverture, puis nettoyage
        int retires = 0;
        for (int i = 0; i < ep_n; i++) {
            if (!base::bloc(ep_couv[i].bloc).actif) continue;
            base::couvrir(ep_couv[i].bloc, ep_couv[i].pmin, ep_couv[i].pmax);
            retires += base::consolider(ep_couv[i].bloc);
        }
        ep_n = 0;
        if (retires > 0) {
            a_sauver = true;
            journal_csv("consolidation", retires, 0);
        }
    }
    static uint32_t prec = 0;
    if (en_pub && prec) pub_total += (maintenant - prec) / IPS;
    prec = maintenant;

    // historique (une case toutes les 10 s)
    int code = enreg ? ENREGISTREMENT : e;
    if (PRIORITE[code] > PRIORITE[case_max] || case_max == 0) case_max = (uint8_t)code;
    if (maintenant >= prochaine_case) {
        if (prochaine_case) {
            historique[nb_cases % NB_CASES] = case_max;
            nb_cases = nb_cases + 1;
        }
        prochaine_case = maintenant + (uint32_t)(CASE_S * IPS);
        case_max = (uint8_t)code;
    }

    // vue partagee
    Vue v;
    v.etat = enreg ? ENREGISTREMENT : e;
    v.numero = 0; v.vus = 0; v.pos_s = 0; v.duree_s = 0;
    v.col = maintenant;
    if (bloc_cour >= 0 && base::bloc(bloc_cour).actif) {
        const base::Bloc &b = base::bloc(bloc_cour);
        v.numero = (int)b.numero;
        v.vus = (int)b.vus;
        v.duree_s = b.duree_s;
        v.pos_s = (pos_cour + (int32_t)(maintenant - der_match)) / IPS;
    }
    float age = der_match ? (maintenant - der_match) / IPS : 99;
    float c = score / (2.0f * (seuil_votes > 0 ? seuil_votes : 1));
    v.confiance = pub_connue ? (c > 1 ? 1 : c) * (1 - age / 10) : (c > 1 ? 1 : c) * 0.5f;
    if (v.confiance < 0) v.confiance = 0;
    v.score = score;
    v.second = second_;
    v.enreg = enreg;
    v.enreg_s = enreg ? (maintenant - enreg_t0) / IPS : 0;
    v.test_etat = test_etat;
    v.test_num = test_num;
    v.test_age_s = (maintenant - test_et_t) / IPS;
    v.test_appris = test_appris;
    v.pub_jour_s = pub_total;
    v.ecrans = nb_ecrans;
    v.ecoute_s = maintenant / IPS;
    v.distant = pub_distante;
    v.chaine = chaine_courante();
    v.chaine_devinee = chaine_reglee <= 0 && chaine_dev > 0;
    v.nb_pas_pub = cpt_pas_pub;
    v.age_match_s = age;
    plat_verrou();
    v_partagee = v;
    plat_deverrou();

    // sauvegarde sur la carte SD APRES la mise a jour de l'ecran (v0.6.2) : elle
    // peut durer plusieurs secondes ; avant, l'ecran gardait l'ancien etat
    // (bouton ENREGISTRER encore affiche) jusqu'a la fin de l'ecriture
    if (a_sauver) {
        a_sauver = false;
        uint32_t v_avant = base::version();
        if (base::sauver()) { version_sauvee = v_avant; t_sauve = plat_ms(); if (!t_sauve) t_sauve = 1; sauve_echec = false; emettre(EV_SAUVE); }
        else if (plat_sd()) {
            sauve_echec = true;
            emettre(EV_SAUVE_ECHEC);
            plat_log("[PUB] ECHEC : la bibliotheque n'a pas pu etre ecrite sur la carte SD (nouvel essai au prochain changement)\n");
        }
    }
}

}  // namespace det
