// empreinte.cpp - spectrogramme, constellation de pics, codes
#include "empreinte.h"
#include "fft.h"
#include "plateforme.h"
#include <math.h>
#include <string.h>

namespace emp {

uint8_t (*spectro)[NB] = nullptr;
volatile uint32_t nb_colonnes = 0;
Pic *pics = nullptr;
volatile uint32_t nb_pics = 0;
Lien liens[NB_LIENS_AFF];
volatile uint32_t nb_liens = 0;
volatile int densite = 15;
#ifndef DENSITE_FAIBLE_DEF
#define DENSITE_FAIBLE_DEF 40
#endif
volatile int densite_faible = DENSITE_FAIBLE_DEF;

// ---- decimation 16 -> 8 kHz : filtre passe-bas de 31 coefficients ----
static const int NF = 31;
static float coef[NF];
static float hist16[NF];
static int pos16 = 0, phase16 = 0;

// ---- trames d'analyse ----
// tableaux de travail en PSRAM (alloues dans init) : la memoire interne est
// reservee au Wi-Fi, au Bluetooth et a Matter
static float *trame = nullptr;       // derniers N echantillons a 8 kHz (anneau)
static int pos8 = 0;                 // prochaine case ecrite = plus ancien echantillon
static int remplis = 0;              // echantillons arrives depuis la derniere colonne
static float *fenetre = nullptr;
static float *re = nullptr, *im = nullptr;

// ---- blanchiment et detection ----
static float moy[NB];                // niveau moyen recent de chaque bin (dB)
static bool moy_init = false;
static const int NK = 2 * VOISIN_T + 1;
static float (*blanc)[NB] = nullptr;   // spectres blanchis, anneau de 7 colonnes
static float (*brut)[NB] = nullptr;
static float seuil = 8.0f;           // dB au-dessus du niveau moyen (pics forts)
static float seuil_f = 5.0f;         // idem pour les pics faibles (plus bas)
static const float PLANCHER = -38.0f;  // dB absolus : en dessous, c'est du silence
static uint32_t col = 0;
static const uint32_t CHAUFFE = (uint32_t)(2 * IPS);   // colonnes de mise en route sans code
static uint32_t col_depart = 0;                          // colonne du dernier (re)demarrage de l'analyse

// ---- pics en attente d'appariement ----
static const int NP = 1024;
static Pic *rp = nullptr;            // NP pics (PSRAM)
static uint8_t rp_pris[NP];          // cibles FORTES deja associees a chaque pic fort (ancrage)
static uint8_t rp_tous[NP];          // cibles de toute sorte deja associees (ancrage)
static uint32_t rp_n = 0;            // pics ranges
static uint32_t rp_min = 0;          // premier pic utilisable comme ancrage

// ---- statistiques ----
static int cpt_pics = 0, cpt_faibles = 0, cpt_codes = 0, cpt_cols = 0;
static float pps = 0, cps = 0;
static bool hors_ligne = false;       // v0.9.58 : analyse d'un fichier : rien pour l'ecran

static void filtre_init()
{
    // sinc fenetre (Hamming), coupure 3,6 kHz a 16 kHz
    const double fc = 3600.0 / 16000.0;
    double s = 0;
    for (int i = 0; i < NF; i++) {
        double m = i - (NF - 1) / 2.0;
        double h = m == 0 ? 2 * fc : sin(2 * 3.14159265358979 * fc * m) / (3.14159265358979 * m);
        h *= 0.54 - 0.46 * cos(2 * 3.14159265358979 * i / (NF - 1));
        coef[i] = (float)h;
        s += h;
    }
    for (int i = 0; i < NF; i++) coef[i] = (float)(coef[i] / s);
}

void reinit()
{
    memset(hist16, 0, sizeof hist16);
    pos16 = phase16 = 0;
    if (trame) memset(trame, 0, N * sizeof(float));
    pos8 = 0;
    remplis = 0;
    moy_init = false;
    if (blanc) memset(blanc, 0, NK * NB * sizeof(float));
    if (brut) memset(brut, 0, NK * NB * sizeof(float));
    rp_min = rp_n;
    col_depart = col;
    seuil = 8.0f;
    seuil_f = 5.0f;
}

static float *zero_grand(size_t n)
{
    float *p = (float *)plat_malloc_grand(n * sizeof(float));
    if (p) memset(p, 0, n * sizeof(float));
    return p;
}

void init()
{
    if (!trame) {
        trame = zero_grand(N);
        fenetre = zero_grand(N);
        re = zero_grand(N);
        im = zero_grand(N);
        blanc = (float(*)[NB])zero_grand(NK * NB);
        brut = (float(*)[NB])zero_grand(NK * NB);
    }
    fft::init();
    filtre_init();
    for (int i = 0; i < N; i++) fenetre[i] = 0.5f - 0.5f * cosf(2 * 3.14159265f * i / N);
    if (!rp) rp = (Pic *)plat_malloc_grand(NP * sizeof(Pic));
    if (!pics) pics = (Pic *)plat_malloc_grand(NB_PICS_AFF * sizeof(Pic));
    if (!spectro) spectro = (uint8_t(*)[NB])plat_malloc_grand(HIST * NB);
    if (!spectro) spectro = (uint8_t(*)[NB])new uint8_t[HIST * NB];
    memset(spectro, 0, HIST * NB);
    reinit();
}

uint32_t colonne() { return col; }
float pics_par_s() { return pps; }
float codes_par_s() { return cps; }
float seuil_db() { return seuil; }

// Un nouveau pic b devient la cible des ancrages des 1,28 s precedentes qui
// n'ont pas encore toutes leurs cibles. Le code part donc des que sa cible est
// la. Entre pics FORTS : les 4 premieres cibles (dans l'ordre d'arrivee) font
// les codes appris, les 4 suivantes des codes de recherche. Des qu'un pic
// faible est en jeu : code de recherche FAIBLE, 12 cibles au plus par ancrage.
static int cibler(const Pic &b, Code *out, int max)
{
    int n = 0;
    for (uint32_t j = rp_n; j-- > rp_min;) {
        const Pic &a = rp[j % NP];
        if (a.t + DT_MAX < b.t) break;             // trop ancien : les suivants aussi
        if (a.t == b.t) continue;                  // meme colonne
        int df = (int)b.f - (int)a.f;
        if (df > DF_MAX || df < -DF_MAX) continue;
        uint8_t &pf = rp_pris[j % NP], &pt = rp_tous[j % NP];
        bool ff = a.fort && b.fort;
        bool code_fort = ff && pf < EVENTAIL_REQ;
        bool code_faible = !code_fort && pt < EVENTAIL_FAIBLE && MAX_TOUS_COL > 0;
        if (code_fort || code_faible) {
            // les 2 premieres secondes apres le demarrage, le blanchiment n'est pas encore
            // stable : il donne les memes pics quel que soit le son. Pas de code.
            if (n < max && a.t >= col_depart + CHAUFFE) {
                out[n].h = ((uint32_t)a.f << 14) | ((uint32_t)b.f << 6) | ((b.t - a.t) & 63);
                if (code_faible) out[n].h |= REQUETE | FAIBLE;
                else if (pf >= EVENTAIL) out[n].h |= REQUETE;
                out[n].t = a.t;
                n++;
            }
            if (code_fort && !hors_ligne) {
                if (pf == 0) {                     // un lien par ancrage pour le dessin
                    Lien &l = liens[nb_liens % NB_LIENS_AFF];
                    l.t0 = a.t; l.t1 = b.t; l.f0 = a.f; l.f1 = b.f;
                    nb_liens = nb_liens + 1;
                }
                if (pf < EVENTAIL) cpt_codes++;    // compte les codes gardes
            }
        }
        if (ff && pf < 255) pf++;
        if (pt < 255) pt++;
    }
    return n;
}

// nouvelle colonne de spectre : blanchiment, affichage, pics de la colonne centrale
static int nouvelle_colonne(Code *out, int max)
{
    for (int i = 0; i < N; i++) {
        re[i] = trame[(pos8 + i) & (N - 1)] * fenetre[i];
        im[i] = 0;
    }
    fft::calcul(re, im);
    int k = col % (2 * VOISIN_T + 1);
    static uint8_t aff_vide[NB];
    uint8_t *aff = hors_ligne ? aff_vide : spectro[col % HIST];
    for (int b = 0; b < NB; b++) {
        float r = re[B0 + b], i = im[B0 + b];
        float db = 10.0f * log10f(r * r + i * i + 1e-9f);
        if (!moy_init) moy[b] = db;
#ifndef BLANC_K
#define BLANC_K 0.02f
#endif
        moy[b] += (db - moy[b]) * BLANC_K;
        brut[k][b] = db;
        blanc[k][b] = db - moy[b];
        float v = (db + 40.0f) * (255.0f / 58.0f);
        aff[b] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    moy_init = true;
    if (!hors_ligne) nb_colonnes = col + 1;

    // la colonne centrale du voisinage (3 colonnes en retard) est complete
    int n = 0;
    if (col >= (uint32_t)(2 * VOISIN_T)) {
        uint32_t tc = col - VOISIN_T;
        int kc = tc % (2 * VOISIN_T + 1);
        // candidats : maxima locaux au-dessus du seuil faible (le seuil fort sans pics faibles)
        const int MAXC = MAX_TOUS_COL > MAX_PAR_COL ? MAX_TOUS_COL : MAX_PAR_COL;
        struct { uint8_t f; float v; } cand[MAXC > 0 ? MAXC : 1];
        int nc = 0;
        float s_min = MAX_TOUS_COL > 0 && densite_faible > 0 ? (seuil_f < seuil ? seuil_f : seuil) : seuil;
        for (int b = VOISIN_F; b < NB - VOISIN_F; b++) {
            float v = blanc[kc][b];
            if (v < s_min || brut[kc][b] < PLANCHER) continue;
            bool max_local = true;
            for (int dk = -VOISIN_T; dk <= VOISIN_T && max_local; dk++) {
                int kk = (int)((tc + 2 * VOISIN_T + 1 + dk) % (2 * VOISIN_T + 1));
                for (int db = -VOISIN_F; db <= VOISIN_F; db++) {
                    if (dk == 0 && db == 0) continue;
                    float w = blanc[kk][b + db];
                    // egalite : seul le premier (en temps puis en frequence) gagne
                    if (w > v || (w == v && (dk < 0 || (dk == 0 && db < 0)))) { max_local = false; break; }
                }
            }
            if (!max_local) continue;
            if (nc < MAXC) {
                cand[nc].f = (uint8_t)b; cand[nc].v = v; nc++;
            } else {
                int pire = 0;
                for (int q = 1; q < MAXC; q++) if (cand[q].v < cand[pire].v) pire = q;
                if (v > cand[pire].v) { cand[pire].f = (uint8_t)b; cand[pire].v = v; }
            }
        }
        // forts : au-dessus du seuil fort et parmi les 4 plus marques de la colonne
        uint8_t fort[MAXC > 0 ? MAXC : 1];
        int nf = 0;
        for (int q = 0; q < nc; q++) {
            int rang = 0;
            for (int r = 0; r < nc; r++)
                if (cand[r].v > cand[q].v || (cand[r].v == cand[q].v && r < q)) rang++;
            fort[q] = cand[q].v >= seuil && rang < MAX_PAR_COL;
            nf += fort[q];
        }
        // ranges par frequence croissante (ordre stable pour l'appariement)
        for (int a = 1; a < nc; a++)
            for (int q = a; q > 0 && cand[q].f < cand[q - 1].f; q--) {
                auto x = cand[q]; cand[q] = cand[q - 1]; cand[q - 1] = x;
                uint8_t y = fort[q]; fort[q] = fort[q - 1]; fort[q - 1] = y;
            }
        for (int q = 0; q < nc; q++) {
            Pic p = { tc, cand[q].f, (uint8_t)(cand[q].v > 40 ? 255 : (cand[q].v < 0 ? 0 : cand[q].v * 6.3f)), fort[q] };
            n += cibler(p, out + n, max - n);
            rp[rp_n % NP] = p;
            rp_pris[rp_n % NP] = 0;
            rp_tous[rp_n % NP] = 0;
            rp_n++;
            if (p.fort && !hors_ligne) {               // l'ecran ne montre que les pics forts
                pics[nb_pics % NB_PICS_AFF] = p;
                nb_pics = nb_pics + 1;
            }
        }
        cpt_pics += nf;
        cpt_faibles += nc;
    }

    // seuil adaptatif : vise "densite" pics par seconde, mesure chaque seconde
    if (++cpt_cols >= 31) {
        pps = pps * 0.5f + cpt_pics * (IPS / cpt_cols) * 0.5f;
        cps = cps * 0.5f + cpt_codes * (IPS / cpt_cols) * 0.5f;
        float cible = (float)(densite > 1 ? densite : 1);
        float ecart = (cpt_pics * IPS / cpt_cols - cible) / cible;
        if (ecart > 1) ecart = 1;
        seuil += 1.2f * ecart;
        if (seuil < 4) seuil = 4;
        if (seuil > 40) seuil = 40;
        // pics faibles : meme regle, densite plus grande, jamais au-dessus du seuil fort
        if (densite_faible > 0) {
            float cf = (float)densite_faible;
            float ef = (cpt_faibles * IPS / cpt_cols - cf) / cf;
            if (ef > 1) ef = 1;
            seuil_f += 1.2f * ef;
            if (seuil_f < 2) seuil_f = 2;
            if (seuil_f > seuil) seuil_f = seuil;
        }
        cpt_cols = cpt_pics = cpt_faibles = cpt_codes = 0;
    }
    col++;
    return n;
}

// ---- v0.9.58 : analyse d'un fichier sans toucher a l'etat du direct ----
struct Etat {
    float hist16[NF]; int pos16, phase16;
    float trame[N]; int pos8, remplis;
    float moy[NB]; bool moy_init;
    float blanc[NK][NB], brut[NK][NB];
    float seuil, seuil_f;
    uint32_t col, col_depart;
    Pic rp[NP]; uint8_t rp_pris[NP], rp_tous[NP]; uint32_t rp_n, rp_min;
    int cpt_pics, cpt_faibles, cpt_codes, cpt_cols; float pps, cps;
};
static Etat *sauve = nullptr;

bool hors_ligne_debut()
{
    if (!sauve) sauve = (Etat *)plat_malloc_grand(sizeof(Etat));
    if (!sauve || !trame || !rp || hors_ligne) return false;
    Etat &e = *sauve;
    memcpy(e.hist16, hist16, sizeof hist16); e.pos16 = pos16; e.phase16 = phase16;
    memcpy(e.trame, trame, N * sizeof(float)); e.pos8 = pos8; e.remplis = remplis;
    memcpy(e.moy, moy, sizeof moy); e.moy_init = moy_init;
    memcpy(e.blanc, blanc, sizeof e.blanc); memcpy(e.brut, brut, sizeof e.brut);
    e.seuil = seuil; e.seuil_f = seuil_f; e.col = col; e.col_depart = col_depart;
    memcpy(e.rp, rp, sizeof e.rp); memcpy(e.rp_pris, rp_pris, sizeof rp_pris); memcpy(e.rp_tous, rp_tous, sizeof rp_tous);
    e.rp_n = rp_n; e.rp_min = rp_min;
    e.cpt_pics = cpt_pics; e.cpt_faibles = cpt_faibles; e.cpt_codes = cpt_codes; e.cpt_cols = cpt_cols; e.pps = pps; e.cps = cps;
    hors_ligne = true;
    reinit();                                         // l'analyse du fichier part de zero
    cpt_pics = cpt_faibles = cpt_codes = cpt_cols = 0;
    return true;
}

void hors_ligne_repartir()
{
    // le blanchiment (moy) et les seuils sont gardes ; trames, pics et chauffe repartent
    memset(hist16, 0, sizeof hist16);
    pos16 = phase16 = 0;
    memset(trame, 0, N * sizeof(float));
    pos8 = 0;
    remplis = 0;
    memset(blanc, 0, NK * NB * sizeof(float));
    memset(brut, 0, NK * NB * sizeof(float));
    col += 2 * DT_MAX;                                // pas de paire avec les pics de la mise en route
    rp_min = rp_n;
    col_depart = col >= CHAUFFE ? col - CHAUFFE : 0;  // plus de chauffe
}

void hors_ligne_fin()
{
    if (!hors_ligne || !sauve) return;
    Etat &e = *sauve;
    memcpy(hist16, e.hist16, sizeof hist16); pos16 = e.pos16; phase16 = e.phase16;
    memcpy(trame, e.trame, N * sizeof(float)); pos8 = e.pos8; remplis = e.remplis;
    memcpy(moy, e.moy, sizeof moy); moy_init = e.moy_init;
    memcpy(blanc, e.blanc, sizeof e.blanc); memcpy(brut, e.brut, sizeof e.brut);
    seuil = e.seuil; seuil_f = e.seuil_f; col = e.col; col_depart = e.col_depart;
    memcpy(rp, e.rp, sizeof e.rp); memcpy(rp_pris, e.rp_pris, sizeof rp_pris); memcpy(rp_tous, e.rp_tous, sizeof rp_tous);
    rp_n = e.rp_n; rp_min = e.rp_min;
    cpt_pics = e.cpt_pics; cpt_faibles = e.cpt_faibles; cpt_codes = e.cpt_codes; cpt_cols = e.cpt_cols; pps = e.pps; cps = e.cps;
    hors_ligne = false;
}

int ajouter(const int16_t *x16, int n, Code *out, int max)
{
    int produits = 0;
    for (int i = 0; i < n; i++) {
        hist16[pos16] = x16[i] * (1.0f / 32768.0f);
        pos16 = (pos16 + 1) % NF;
        if (++phase16 < 2) continue;
        phase16 = 0;
        float y = 0;
        int p = pos16;                               // plus ancien echantillon
        for (int k = 0; k < NF; k++) {
            y += coef[k] * hist16[p];
            if (++p == NF) p = 0;
        }
        trame[pos8] = y;
        pos8 = (pos8 + 1) & (N - 1);
        if (++remplis >= SAUT) {
            remplis = 0;
            produits += nouvelle_colonne(out + produits, max - produits);
        }
    }
    return produits;
}

}  // namespace emp
