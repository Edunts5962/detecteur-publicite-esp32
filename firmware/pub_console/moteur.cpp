// moteur.cpp - source sonore -> empreintes, detecteur, coupure du son, tele
// v0.6.2 : spectre pour l'ecran d'accueil (24 filtres de Goertzel sur les 512
// derniers echantillons, ~15 fois/s, seulement quand l'ecran le demande)
#include "moteur.h"
#include "coupure.h"
#include "empreinte.h"
#include "base.h"
#include "detecteur.h"
#include "plateforme.h"

// v0.9.58 : verrou du moteur d'empreintes (la carte le definit ; rien a faire ailleurs)
__attribute__((weak)) void plat_emp_prendre() {}
__attribute__((weak)) void plat_emp_rendre() {}
#include "tele.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

namespace mot {

volatile int source = SRC_MICRO;

static int src_prec = -1;
static uint32_t ms_prec = 0;
static float s_niveau = -90, s_crete = 0;
static bool s_sature = false;
static uint32_t sature_ms = 0;
static bool s_memoire = false;
static double acc = 0;
static int acc_n = 0;
static float pic = 0;

// ---- spectre pour l'ecran ----
static const int SP_N = 512;                 // 32 ms a 16 kHz
// v0.9.71 : tableaux du spectre en PSRAM (4 Ko de memoire interne liberes) ; nullptr = spectre absent
static int16_t *sp_buf = nullptr;             // les derniers echantillons (anneau)
static int sp_pos = 0;
static float *sp_fen = nullptr;               // fenetre de Hann
static float *sp_x = nullptr;                 // morceau fenetre (calcul)
static float sp_coef[NB_BANDES];             // 2 cos(2 pi f / fs)
static bool sp_pret = false;
static uint8_t sp_val[NB_BANDES];
static volatile uint32_t sp_lu_ms = 0;       // derniere lecture par l'ecran
static uint32_t sp_calc_ms = 0;

static void spectre_init()
{
    if (!sp_fen) sp_fen = (float *)plat_malloc_grand(SP_N * sizeof(float));
    if (!sp_x) sp_x = (float *)plat_malloc_grand(SP_N * sizeof(float));
    if (!sp_fen || !sp_x) return;                // pas de PSRAM : pas de spectre (le reste marche)
    for (int i = 0; i < SP_N; i++) sp_fen[i] = 0.5f - 0.5f * cosf(2 * 3.14159265f * i / (SP_N - 1));
    const float f0 = 120, f1 = 6500;
    for (int b = 0; b < NB_BANDES; b++) {
        float f = f0 * powf(f1 / f0, (float)b / (NB_BANDES - 1));   // bandes logarithmiques
        sp_coef[b] = 2 * cosf(2 * 3.14159265f * f / 16000.0f);
    }
    sp_pret = true;
}

static void spectre_calculer()
{
    if (!sp_pret) spectre_init();
    if (!sp_pret || !sp_buf) return;
    float *x = sp_x;
    for (int i = 0; i < SP_N; i++) x[i] = sp_buf[(sp_pos + i) & (SP_N - 1)] * (1.0f / 32768.0f) * sp_fen[i];
    uint8_t v[NB_BANDES];
    for (int b = 0; b < NB_BANDES; b++) {
        float c = sp_coef[b], s1 = 0, s2 = 0;
        for (int i = 0; i < SP_N; i++) { float s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        float p = s1 * s1 + s2 * s2 - c * s1 * s2;
        float db = 10.0f * log10f(p + 1e-12f) - 42.1f;          // sinus plein echelle ~ 0 dB
        float k = (db + 80.0f) / 70.0f;                         // -80 .. -10 dB -> 0 .. 1
        v[b] = (uint8_t)(k < 0 ? 0 : (k > 1 ? 255 : k * 255));
    }
    plat_verrou();
    memcpy(sp_val, v, sizeof v);
    plat_deverrou();
}

void spectre(uint8_t *out)
{
    sp_lu_ms = plat_ms();
    plat_verrou();
    memcpy(out, sp_val, sizeof sp_val);
    plat_deverrou();
}

// ---- sensibilite du micro ----
const int GAIN_DB[NB_GAINS] = { 12, 21, 30, 36, 42, 48 };
static volatile int g_choix = -1;            // -1 automatique
static volatile int g_niveau = 3;            // +36 dB au depart en automatique
static int g_applique = -1;
static uint32_t g_dernier = 0;               // dernier changement automatique (ms)
static float g_somme = 0, g_crete = 0;       // sur la fenetre d'observation
static int g_n = 0;

void regler_gain(int choix)
{
    if (choix >= NB_GAINS) choix = NB_GAINS - 1;
    g_choix = choix;
    if (choix >= 0) g_niveau = choix;
}
int gain_niveau() { return g_niveau; }
bool gain_auto() { return g_choix < 0; }

// automatique : vise un son de tele entre -40 et -20 dB plein echelle,
// redescend tout de suite si ca sature, change au plus une fois par 3 s.
// Pendant que la carte baisse elle-meme le son de la tele, le gain ne monte pas
// (sinon il serait au maximum au retour du son).
static void gain_automatique(uint32_t ms)
{
    if (g_choix >= 0 || source != SRC_MICRO) { g_n = 0; g_somme = 0; g_crete = 0; return; }
    if (s_sature && g_niveau > 0 && ms - g_dernier > 600) {
        g_niveau = g_niveau - 1;
        g_dernier = ms;
        g_n = 0; g_somme = 0; g_crete = 0;
        return;
    }
    if (ms - g_dernier < 3000 || g_n < 25) return;
    float moy = g_somme / g_n;
    int avant = g_niveau;
    bool son_baisse = cpr::active() || tv::en_cours();
    if ((moy > -20 || g_crete > 0.75f) && g_niveau > 0) g_niveau = g_niveau - 1;
    else if (!son_baisse && moy < -40 && moy > -85 && g_crete < 0.35f && g_niveau < NB_GAINS - 1) g_niveau = g_niveau + 1;
    if (g_niveau != avant) g_dernier = ms;
    g_n = 0; g_somme = 0; g_crete = 0;
}

void init()
{
    emp::init();
    s_memoire = base::init();
    if (s_memoire) base::charger();
    det::init();
}

float niveau_db() { return s_niveau; }
float crete() { return s_crete; }
bool sature() { return s_sature; }
bool memoire_ok() { return s_memoire; }

static void traiter(const int16_t *x, int n, uint32_t ms)
{
    for (int i = 0; i < n; i++) {
        float v = x[i] * (1.0f / 32768.0f);
        acc += v * v;
        float a = fabsf(v);
        if (a > pic) pic = a;
        if (a > 0.97f) sature_ms = ms;
        if (++acc_n >= 1600) {                       // toutes les 100 ms
            float db = 10.0f * log10f((float)(acc / acc_n) + 1e-10f);
            s_niveau = db;
            s_crete = pic;
            g_somme += db;
            if (pic > g_crete) g_crete = pic;
            g_n++;
            pic = 0;
            acc = 0;
            acc_n = 0;
        }
    }
    s_sature = sature_ms && ms - sature_ms < 1500;
    // derniers echantillons pour le spectre de l'ecran
    {
        if (!sp_buf) { sp_buf = (int16_t *)plat_malloc_grand(SP_N * sizeof(int16_t)); if (sp_buf) memset(sp_buf, 0, SP_N * sizeof(int16_t)); }
        int d = n > SP_N ? n - SP_N : 0;
        if (sp_buf) for (int i = d; i < n; i++) { sp_buf[sp_pos] = x[i]; sp_pos = (sp_pos + 1) & (SP_N - 1); }
    }
    // pendant le bip du haut-parleur, le micro s'entend lui-meme : ces codes sont jetes
    plat_archive(x, n);
    bool hp = plat_son_en_cours();
    static emp::Code *codes = (emp::Code *)plat_malloc_grand(1024 * sizeof(emp::Code));
    plat_emp_prendre();                             // v0.9.58 : un fichier peut etre en cours d'analyse (1 a 2 s)
    int k = codes ? emp::ajouter(x, n, codes, 1024) : 0;
    plat_emp_rendre();
    if (k > 0 && !hp) {
        base::journal_ajouter(codes, k);
        det::codes(codes, k);
    }
}

// ---- v0.9.64 : pub inconnue « prise en sandwich » entre deux pubs reconnues ----
// Entree optique seulement, pendant une coupure du son : si un passage de 5 a 60 s separe la fin
// d'une pub reconnue du debut de la suivante, c'est forcement une pub. Elle est apprise (origine
// « auto ») et marquee « a verifier » (raison : apprise pendant une coupure) : validation sur le portail.
static int sw_num = 0;                       // derniere pub reconnue (numero)
static int32_t sw_s = 0, sw_e = 0;           // ses colonnes de debut et de fin
static uint32_t sw_coupure = 0;              // coupure pendant laquelle elle a ete reconnue
static void sandwich(const det::Vue &v)
{
    if (source != SRC_FO || !det::actif || cpr::suspendue || cpr::etat() != cpr::COUPE) { sw_num = 0; return; }
    if (v.etat != det::PUB || v.numero <= 0 || v.score < det::seuil_votes || v.duree_s < 5) return;
    const float ips = emp::IPS;
    int32_t s = (int32_t)v.col - (int32_t)(v.pos_s * ips);
    int32_t e = s + (int32_t)(v.duree_s * ips);
    if (sw_num == v.numero && sw_coupure == cpr::nb_coupures() && abs(s - sw_s) < (int32_t)(3 * ips)) {
        if (e > sw_e) sw_e = e;                  // meme passage de la meme pub
        return;
    }
    if (sw_num && sw_coupure == cpr::nb_coupures() && plat_fo_flux_ok()) {
        // v0.9.67 : 1 s de marge de chaque cote (au lieu de 0,3 s) : on ne prend pas la fin ni le debut
        // des pubs voisines ; le resultat reel (appris, deja connue, trop court...) est ecrit par le detecteur
        const int32_t marge = (int32_t)(1.0f * ips);
        int32_t ma = sw_e + marge, mb = s - marge;
        float d = (mb - ma) / ips;
        if (d >= 5.0f && d <= 60.0f) {
            plat_log("[PUB] Coupure : %.0f s inconnues entre la pub n°%d et la pub n°%d : essai d'apprentissage\n", d, sw_num, v.numero);
            det::commande(det::CMD_APPRENDRE_AUTO, (int)ma, (int)mb, (v.chaine & 255) | 256 | 512);
        }
    }
    sw_num = v.numero; sw_s = s; sw_e = e; sw_coupure = cpr::nb_coupures();
}

uint32_t etape(uint32_t ms)
{
    static int16_t *buf = (int16_t *)plat_malloc_grand(1024 * sizeof(int16_t));   // PSRAM
    int src = source;
    if (src != src_prec) {
        src_prec = src;
        plat_emp_prendre();
        emp::reinit();
        plat_emp_rendre();
        plat_archive_reset();
        ms_prec = ms;
        while (plat_audio_lire(buf, 1024) > 0) {}   // on jette l'ancien son
    }
    int total = 0;
    {
        for (int boucle = 0; boucle < 8; boucle++) {
            int k = plat_audio_lire(buf, 1024);
            if (k <= 0) break;
            traiter(buf, k, ms);
            total += k;
        }
    }
    gain_automatique(ms);
    if (g_niveau != g_applique) {
        g_applique = g_niveau;
        plat_audio_gain(g_niveau);
    }
    // spectre : seulement si l'ecran d'accueil l'a lu depuis moins d'une demi-seconde
    if (ms - sp_lu_ms < 500 && ms - sp_calc_ms >= 66) {
        sp_calc_ms = ms;
        spectre_calculer();
    }
    det::periodique(s_niveau, s_sature);
    {
        det::Vue v;
        det::vue(v);
        cpr::ecoute_fibre = source == SRC_FO && plat_fo_flux_ok();   // v0.9.63 : la carte entend la tele meme son baisse
        cpr::etape(ms, v);                         // quand baisser le son (fenetres, reecoute)
        sandwich(v);                               // v0.9.64 : pubs inconnues entre deux pubs reconnues (fibre)
        tv::suivre_niveau(cpr::niveau());          // telecommande Wi-Fi : bas / mi-chemin (v0.9.64) / normal
    }
    return total >= 1024 * 8 ? 1 : 20;
}

}  // namespace mot
