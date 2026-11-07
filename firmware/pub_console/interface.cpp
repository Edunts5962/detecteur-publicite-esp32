// interface.cpp - ecrans du detecteur de pub (v0.6.3)
//
//   ACCUEIL  : l'etat en grand (PUB / PROGRAMME), un seul gros bouton TESTER
//              (facon Shazam), la coupure du son expliquee (temps, volume).
//              TESTER = debut de la pub ; pub inconnue : le gros bouton devient
//              ENREGISTRER (fin de la pub), ANNULER a cote
//   TELE     : telecommande Wi-Fi : IP, appairage (clavier), essais, oubli
//   PUBS     : la bibliotheque (nombre, dernieres pubs, tout effacer),
//              CAPTER / ARRETER : son brut du micro sur la SD (v0.6.3)
//   JOURNAL  : la console serie, interrupteur marche / arret
//   REGLAGES : son de la tele, detection, carte, Wi-Fi
//
// Reactivite : chaque bouton est enregistre a l'endroit ou il est dessine ; il
// reagit DES QUE LE DOIGT TOUCHE (couleur + action), une image n'est dessinee
// que si quelque chose change. Un seul message a la fois, toujours au meme
// endroit (bande au-dessus des onglets).
#include <time.h>
#include "interface.h"
#include "gfx.h"
#include "moteur.h"
#include "detecteur.h"
#include "empreinte.h"
#include "base.h"
#include "plateforme.h"
#include "config.h"
#include "qrcodegen.h"
#include "coupure.h"
#include "tele.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

using namespace gfx;

// versions du paquet Espressif : fournies par la carte (bancs : version vide)
__attribute__((weak)) const char *plat_version_arduino() { return "PC"; }
__attribute__((weak)) const char *plat_version_idf() { return ""; }
// captation : fournie par la carte (bancs : indisponible)
__attribute__((weak)) const char *plat_captation_demarrer() { return "pas de micro (PC)"; }
__attribute__((weak)) const char *plat_captation_demarrer_auto() { return "pas de micro (PC)"; }
__attribute__((weak)) const char *plat_captation_demarrer_apprendre() { return "pas de micro (PC)"; }
__attribute__((weak)) void plat_captation_arreter() {}
__attribute__((weak)) void plat_captation_annuler() {}
__attribute__((weak)) int plat_captation_chaine() { return 0; }
__attribute__((weak)) void plat_captation_chaine_regler(int) {}
__attribute__((weak)) void plat_captation(Captation &c) { memset(&c, 0, sizeof c); }
__attribute__((weak)) bool plat_fo_flux_ok() { return false; }
__attribute__((weak)) int plat_fo_etat() { return -1; }
__attribute__((weak)) const char *plat_fo_test_demarrer() { return "pas d'entrée optique (PC)"; }
__attribute__((weak)) void plat_fo_test(TestOptique &t) { memset(&t, 0, sizeof t); }

namespace ui {

// ============================ Palette ======================================
static const uint16_t FOND_HAUT = rgb(4, 6, 18);
static const uint16_t FOND_BAS  = rgb(16, 12, 42);
static const uint16_t PANNEAU   = rgb(16, 22, 52);
static const uint16_t PANNEAU2  = rgb(28, 36, 80);
static const uint16_t BORD      = rgb(52, 66, 130);
static const uint16_t TEXTE     = rgb(236, 242, 255);
static const uint16_t TEXTE2    = rgb(156, 168, 210);
static const uint16_t TEXTE3    = rgb(96, 108, 156);
static const uint16_t CYAN      = rgb(40, 228, 255);
static const uint16_t BLEU      = rgb(70, 120, 255);
static const uint16_t VIOLET    = rgb(140, 92, 255);
static const uint16_t MAGENTA   = rgb(255, 64, 200);
static const uint16_t JAUNE     = rgb(255, 212, 64);
static const uint16_t ORANGE    = rgb(255, 140, 60);
static const uint16_t ROUGE     = rgb(255, 78, 106);
static const uint16_t VERT      = rgb(60, 255, 156);
static const uint16_t GRIS      = rgb(120, 130, 170);
static const uint16_t NOIR_BLEU = rgb(6, 10, 30);

static const float PI_F = 3.14159265f;

// couleurs et mots de chaque etat (ordre de det::Etat)
static const uint16_t C_ETAT[det::NB_ETATS]  = { GRIS, VIOLET, VERT, ORANGE, MAGENTA, JAUNE, GRIS };
static const uint16_t C_ETAT2[det::NB_ETATS] = { rgb(80, 90, 130), BLEU, CYAN, rgb(255, 90, 60), ROUGE, ORANGE, rgb(70, 76, 110) };
static const char *MOT_ETAT[det::NB_ETATS] = { "SILENCE", "ÉCOUTE", "PROGRAMME", "PUB ?", "PUB", "J'ENREGISTRE", "EN PAUSE" };

// ============================ Etat ==========================================
enum { O_ACCUEIL, O_TELE, O_CAPTER, O_JOURNAL, O_REGLAGES, NB_ONGLETS };    // v0.6.7 : plus d'onglet Pubs
static const char *NOMS_ONGLETS[NB_ONGLETS] = { "Accueil", "Télé", "Capter", "Journal", "Réglages" };
static const float LARG_ONGLET = 320.0f / (float)(int)NB_ONGLETS;

static uint16_t *s_fond = nullptr;
static float t = 0, dt = 0.033f;
static uint32_t ms_prec = 0;
static bool splash = true;
static float splash_t0 = 0;
static int onglet = O_ACCUEIL;
static det::Vue V;                    // copie de l'etat du detecteur, a chaque image
static tv::Infos T;                   // copie de l'etat de la tele
static float vu_aff = 0;
static float faible_t = 0;            // depuis quand le son est trop faible
static bool son_faible = false;

// reponse immediate (v0.6.2) : la commande est partie, le moteur ne l'a pas encore
// traitee ; le bouton change tout de suite et ne reagit plus a un 2e appui
static int s_attente = 0;             // 0 rien, 1 TESTER, 2 ENREGISTRER, 3 ANNULER
static int s_attente_etat = -1;       // etat du test au moment de l'appui
static uint32_t s_attente_ms = 0;
static void attendre(int quoi) { s_attente = quoi; s_attente_etat = V.test_etat; s_attente_ms = plat_ms(); }
static void fin_attente()
{
    if (s_attente && (V.test_etat != s_attente_etat || plat_ms() - s_attente_ms > 3000)) s_attente = 0;
}
// spectre (Accueil)

using cfg::R;

// ---- dessin a la demande ----
static volatile bool s_sale = true;   // une image est demandee
static uint32_t s_der_image = 0;      // derniere image dessinee (ms)
static bool s_anime = false;          // l'image en cours demande la suivante vite (animation)
void redessiner() { s_sale = true; }

// ---- le message (une seule bande, toujours au meme endroit) ----
static char msg_txt[80] = "";
static char msg_det[200] = "";
static uint16_t msg_c = CYAN;
static float msg_t0 = -100;
static volatile uint32_t msg_n = 0;
static const float MSG_DUREE = 9.0f;
static void message(const char *s, uint16_t c, const char *detail = nullptr)
{
    strncpy(msg_txt, s, sizeof msg_txt - 1);
    msg_txt[sizeof msg_txt - 1] = 0;
    strncpy(msg_det, detail ? detail : "", sizeof msg_det - 1);
    msg_det[sizeof msg_det - 1] = 0;
    msg_c = c;
    msg_t0 = t;
    msg_n = msg_n + 1;
    s_sale = true;
}

// ---- boutons : zones enregistrees a chaque image ----
enum Id {
    B_AUCUN = 0, B_ONGLET0, B_ACTIF = B_ONGLET0 + NB_ONGLETS,
    B_TESTER, B_ANNULER, B_ENREGISTRER, B_SON, B_PAS_PUB,
    B_TV_IP, B_TV_APPAIRER, B_TV_TEST, B_TV_OUBLIER, B_TV_RETOUR, B_TV_COUPER, B_TV_DIMINUER, B_TV_AUGMENTER, B_TV_RETABLIR, B_TV_LIRE,
    B_TV_OUBLIER_OUI, B_TV_ANNULER_APP,
    B_CLAVIER0,                      // 18 touches : 0-9, A-F, '.', effacer
    B_CL_OK = B_CLAVIER0 + 18, B_CL_ANNULER,
    B_PUBS_EFFACER, B_PUBS_EFFACER_OUI, B_PUBS_NON, B_CAPTER, B_CAP_AUTO, B_CAP_APPR, B_TEST_FO, B_CAP_ANNULER, B_CAP_OUI, B_CAP_NON, B_CAP_CH_MOINS, B_CAP_CH_PLUS,
    B_JR_ONOFF, B_JR_HAUT, B_JR_BAS,
    B_PAGE0,                         // pages des reglages
    B_RG_MOINS0 = B_PAGE0 + 5,       // 8 steppers / choix : moins, plus, choix   (v0.9.66 : 5 pages)
    B_RG_PLUS0 = B_RG_MOINS0 + 8,
    B_RG_CHOIX0 = B_RG_PLUS0 + 8,    // 8 lignes x 4 choix
    B_PREREG0 = B_RG_CHOIX0 + 32,    // v0.9.66 : 4 preregages
    B_FIN = B_PREREG0 + 4
};
struct Zone { int16_t x, y, l, h; uint16_t id; bool repete; };
static Zone zones[64];
static int nzones = 0;
// v0.6.4 : zone tactile plus large que le dessin pour une rangee collee a la barre d'onglets
// (boutons de 22 px de haut, collee a la barre d'onglets : on appuyait trop souvent a cote).
// Elle monte de zone_haut px et s'elargit de 2 px de chaque cote ; elle ne descend jamais
// sous y = 203 (la barre d'onglets commence a 204).
static int zone_haut = 0;
static void zone(int id, int x, int y, int l, int h, bool repete = false)
{
    if (zone_haut > 0) {
        int y2 = y + h > 204 ? 204 : y + h;
        x -= 2; l += 4;
        y -= zone_haut; h = y2 - y;
    }
    if (nzones < 64) zones[nzones++] = Zone{ (int16_t)x, (int16_t)y, (int16_t)l, (int16_t)h, (uint16_t)id, repete };
}
// toucher
static bool tc_appui = false;
static int tc_id = B_AUCUN;           // bouton sous le doigt (couleur "appuye")
static int tc_n = 0;                  // lectures consecutives du doigt
static uint32_t tc_t0 = 0, tc_rep = 0;
static bool tc_fait = false;          // action deja faite pour cet appui
static bool tc_repete = false;

// ============================ Outils ========================================
static inline float sat01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float approche(float v, float c, float k) { return v + (c - v) * (1.0f - expf(-k * dt)); }
static inline float sortie(float x) { x = sat01(x); return 1 - (1 - x) * (1 - x) * (1 - x); }
static inline float pulse(float f, float ph = 0) { return 0.5f + 0.5f * sinf(t * f * 2 * PI_F + ph); }

static void fmt_duree(char *b, int n, float s)
{
    if (s < 0) s = 0;
    int k = (int)(s + 0.5f);
    if (k >= 3600) snprintf(b, n, "%d h %02d", k / 3600, (k / 60) % 60);
    else snprintf(b, n, "%d:%02d", k / 60, k % 60);
}

static bool dans(int x, int y, int rx, int ry, int rl, int rh)
{
    return x >= rx && x < rx + rl && y >= ry && y < ry + rh;
}

static void appliquer_sauver() { cfg::appliquer(); cfg::sauver(); }

// ============================ Fond ==========================================
static void dessiner_fond_complet()
{
    degrade_v(0, 0, L, H, FOND_HAUT, FOND_BAS);
    halo(255, 50, 150, VIOLET, 30);
    halo(40, 210, 140, BLEU, 24);
    halo(170, 250, 110, MAGENTA, 14);
}

static void fond()
{
    if (s_fond) copier(s_fond);
    else degrade_v(0, 0, L, H, FOND_HAUT, FOND_BAS);
}

// ============================ Elements communs ==============================
static void panneau(int x, int y, int l, int h, uint8_t a = 150)
{
    rect_arrondi(x, y, l, h, 12, PANNEAU, a);
    contour_arrondi(x, y, l, h, 12, 1, BORD, 110);
}

// petite tele (logo) ; s = echelle
static void tele(float cx, float cy, float s, uint16_t c)
{
    contour_arrondi((int)(cx - 9 * s), (int)(cy - 6 * s), (int)(18 * s), (int)(12 * s), (int)(3 * s), 1.4f * s, c);
    rect((int)(cx - 6 * s), (int)(cy - 3 * s), (int)(12 * s), (int)(6 * s), c, 120);
    trait(cx - 4 * s, cy + 7.5f * s, cx + 4 * s, cy + 7.5f * s, 1.4f * s, c);
    trait(cx - 3 * s, cy - 9 * s, cx, cy - 6 * s, 1.1f * s, c);
    trait(cx + 3 * s, cy - 9 * s, cx, cy - 6 * s, 1.1f * s, c);
}

// bouton : fond en degrade (ca, cb), titre et sous-titre facultatif.
// appuye = le doigt est dessus (plus clair, enfonce de 1 px)
static void bouton(int id, int x, int y, int l, int h, const char *titre, const char *sous, uint16_t ca, uint16_t cb,
                   bool actif = true, bool repete = false)
{
    bool appuye = tc_appui && tc_id == id;
    if (actif) zone(id, x, y, l, h, repete);
    int dy = appuye ? 1 : 0;
    if (!actif) {
        rect_arrondi(x, y, l, h, 10, PANNEAU2, 200);
        contour_arrondi(x, y, l, h, 10, 1, BORD, 120);
    } else {
        rect_arrondi_degrade(x, y + dy, l, h, 10, appuye ? melange(ca, TEXTE, 90) : ca, appuye ? melange(cb, TEXTE, 90) : cb);
        if (appuye) contour_arrondi(x - 1, y + dy - 1, l + 2, h + 2, 11, 2, TEXTE, 230);
    }
    uint16_t ct = actif ? NOIR_BLEU : TEXTE3;
    const PoliceAA &p = largeur_texte(titre, POLICE_13) <= l - 8 ? POLICE_13 : POLICE_11;
    if (sous) {
        texte(x + l / 2, y + dy + h / 2 - 14, titre, p, ct, 255, CENTRE);
        texte(x + l / 2, y + dy + h / 2 + 1, sous, POLICE_11, ct, 210, CENTRE);
    } else texte(x + l / 2, y + dy + (h - hauteur(p)) / 2, titre, p, ct, 255, CENTRE);
}

// bouton sombre (secondaire)
static void bouton2(int id, int x, int y, int l, int h, const char *titre, uint16_t c, bool actif = true, bool repete = false)
{
    bool appuye = tc_appui && tc_id == id;
    if (actif) zone(id, x, y, l, h, repete);
    rect_arrondi(x, y + (appuye ? 1 : 0), l, h, 10, appuye ? melange(PANNEAU2, c, 110) : PANNEAU2, 235);
    contour_arrondi(x, y + (appuye ? 1 : 0), l, h, 10, appuye ? 2 : 1, actif ? c : BORD, actif ? 220 : 110);
    const PoliceAA &p = largeur_texte(titre, POLICE_13) <= l - 8 ? POLICE_13 : POLICE_11;
    texte(x + l / 2, y + (appuye ? 1 : 0) + (h - hauteur(p)) / 2, titre, p, actif ? TEXTE : TEXTE3, 255, CENTRE);
}

// choix parmi n (segments) ; id0 + i
static void segments(int id0, int x, int y, int l, int h, const char *const *lab, int n, int sel)
{
    rect_arrondi(x, y, l, h, h / 2, PANNEAU2, 230);
    float w = (float)l / n;
    for (int i = 0; i < n; i++) {
        int sx = (int)(x + w * i);
        bool appuye = tc_appui && tc_id == id0 + i;
        zone(id0 + i, sx, y - 3, (int)w, h + 6);
        if (i == sel) rect_arrondi_degrade(sx + 2, y + 2, (int)w - 4, h - 4, (h - 4) / 2, rgb(60, 150, 255), rgb(120, 70, 240));
        else if (appuye) rect_arrondi(sx + 2, y + 2, (int)w - 4, h - 4, (h - 4) / 2, BORD, 230);
        texte((int)(sx + w / 2), y + (h - 13) / 2, lab[i], POLICE_11, i == sel ? TEXTE : TEXTE2, 255, CENTRE);
    }
}

// valeur reglable : [ − ]  valeur  [ + ]  (maintenir pour aller vite)
static void stepper(int k, int x, int y, const char *val, bool actif = true)
{
    bouton2(B_RG_MOINS0 + k, x, y, 34, 26, "−", CYAN, actif, true);
    texte(x + 34 + 37, y + 6, val, POLICE_13, actif ? TEXTE : TEXTE3, 255, CENTRE);
    bouton2(B_RG_PLUS0 + k, x + 108, y, 34, 26, "+", CYAN, actif, true);
}

// ============================ Barres ========================================
static void barre_haut()
{
    uint16_t c = C_ETAT[V.etat], c2 = C_ETAT2[V.etat];
    rect(0, 0, L, 24, PANNEAU, 200);
    degrade_h(0, 24, L, 1, c, c2, 150);
    tele(12, 12, 0.85f, rgb(255, 236, 200));
    texte(26, 4, "PUB", POLICE_13, TEXTE);
    // telecommande : pastille
    uint16_t ct = T.etat == tv::CONNECTEE ? VERT : (T.etat == tv::NON_APPAIREE ? TEXTE3 : ORANGE);
    disque(62, 12, 3.5f, ct);
    char tvb[24];                                   // v0.9.17 : le volume de la tele, visible sur tous les onglets
    if (T.etat == tv::CONNECTEE && T.volume_connu) {
        if (T.muet) snprintf(tvb, sizeof tvb, "télé muet");
        else if (T.action != tv::RIEN && T.depart >= 0) snprintf(tvb, sizeof tvb, "télé %d>%d", T.volume, T.cible);
        else snprintf(tvb, sizeof tvb, "télé %d", T.volume);
    } else snprintf(tvb, sizeof tvb, "%s", T.etat == tv::CONNECTEE ? "télé" : (T.etat == tv::NON_APPAIREE ? "télé ?" : "télé…"));
    texte(69, 5, tvb, POLICE_11, ct);
    // vumetre (14 barres, -62 .. -10 dB) : rouge trop faible, vert bon, jaune trop fort
    float niv = mot::niveau_db();
    float v = sat01((niv + 62) / 52);
    vu_aff = approche(vu_aff, v, v > vu_aff ? 30 : 6);
    const float F_FAIBLE = (mot::NIVEAU_FAIBLE + 62) / 52, F_FORT = (mot::NIVEAU_FORT + 62) / 52;
    for (int i = 0; i < 14; i++) {
        float f = (i + 0.5f) / 14;
        uint16_t cv = f < F_FAIBLE ? ROUGE : (f < F_FORT ? VERT : JAUNE);
        bool on = f < vu_aff;
        rect_arrondi(128 + i * 6, 7, 4, 10, 2, on ? cv : melange(PANNEAU2, cv, 70), on ? 255 : 220);
    }
    // etiquette marche / arret (toucher = basculer)
    char buf[32];
    uint16_t ce;
    int capt_etat = 0;
    { Captation cpb; plat_captation(cpb); capt_etat = cpb.etat; }
    if (!det::actif) { snprintf(buf, sizeof buf, "ARRÊTÉ"); ce = GRIS; }
    else if (mot::source == mot::SRC_FO) {
        int fe = plat_fo_etat();                    // v0.9.60 : l'etat reel de l'entree optique
        if (fe == 2) { snprintf(buf, sizeof buf, "OPTIQUE"); ce = CYAN; }
        else if (fe == 1) { snprintf(buf, sizeof buf, "Optique: silence"); ce = ORANGE; }
        else if (fe == 3) { snprintf(buf, sizeof buf, "Optique: test"); ce = ORANGE; }
        else { snprintf(buf, sizeof buf, "Optique: démarrage"); ce = ROUGE; }
    }
    else if (capt_etat) { snprintf(buf, sizeof buf, capt_etat == 2 ? "● CAPTATION" : "CAPTATION…"); ce = ROUGE; }
    else if (!plat_audio_ok()) { snprintf(buf, sizeof buf, "Audio absent"); ce = ORANGE; }
    else if (mot::sature()) { snprintf(buf, sizeof buf, "Saturé"); ce = ROUGE; }
    else if (son_faible) { snprintf(buf, sizeof buf, "Son faible"); ce = ROUGE; }
    else { snprintf(buf, sizeof buf, "En marche"); ce = VERT; }
    int l = largeur_texte(buf, POLICE_11) + 26;
    int x = L - 6 - l;
    bool appuye = tc_appui && tc_id == B_ACTIF;
    zone(B_ACTIF, x - 10, 0, l + 16, 26);
    rect_arrondi(x, 4, l, 16, 8, appuye ? BORD : PANNEAU2, 230);
    contour_arrondi(x, 4, l, 16, 8, 1, ce, 140);
    disque(x + 10, 12, 3, ce);
    texte(x + 18, 6, buf, POLICE_11, TEXTE);
}

static void icone(int i, float cx, float cy, uint16_t c)
{
    switch (i) {
    case O_ACCUEIL:                                   // cercle "Shazam"
        anneau(cx, cy, 6.5f, 1.6f, c);
        disque(cx, cy, 2.6f, c);
        break;
    case O_TELE:
        tele(cx, cy + 1, 0.75f, c);
        break;
    case O_CAPTER:                                    // pastille d'enregistrement
        anneau(cx, cy, 7.0f, 1.6f, c);
        disque(cx, cy, 3.6f, c);
        break;
    case O_JOURNAL:
        for (int k = 0; k < 4; k++) rect((int)(cx - 7), (int)(cy - 6 + k * 4), k == 3 ? 7 : (k == 1 ? 10 : 13), 2, c);
        break;
    case O_REGLAGES:
        for (int k = 0; k < 8; k++) {
            float an = k * PI_F / 4;
            trait(cx + cosf(an) * 5, cy + sinf(an) * 5, cx + cosf(an) * 7.5f, cy + sinf(an) * 7.5f, 2.2f, c);
        }
        anneau(cx, cy, 4.5f, 2.4f, c);
        break;
    }
}

static void barre_onglets()
{
    uint16_t c = C_ETAT[V.etat];
    rect(0, 204, L, 36, PANNEAU, 220);
    degrade_h(0, 204, L, 1, C_ETAT2[V.etat], c, 120);
    for (int i = 0; i < NB_ONGLETS; i++) {
        bool actif = i == onglet;
        bool appuye = tc_appui && tc_id == B_ONGLET0 + i;
        int hx = (int)(3 + i * LARG_ONGLET), hl = (int)LARG_ONGLET - 6;
        zone(B_ONGLET0 + i, (int)(i * LARG_ONGLET), 204, (int)LARG_ONGLET, 36);
        if (actif) {
            rect_arrondi_degrade(hx, 207, hl, 31, 10, rgb(40, 60, 150), rgb(80, 40, 140), 210);
            contour_arrondi(hx, 207, hl, 31, 10, 1, c, 150);
        } else if (appuye) rect_arrondi(hx, 207, hl, 31, 10, PANNEAU2, 230);
        float cx = i * LARG_ONGLET + LARG_ONGLET / 2;
        icone(i, cx, 216, actif ? c : TEXTE3);
        const char *nom = NOMS_ONGLETS[i];
        if (largeur_texte(nom, POLICE_11) > (int)LARG_ONGLET - 2) nom = i == O_REGLAGES ? "Régl." : nom;   // 6 onglets : « Réglages » peut ne pas tenir
        texte((int)cx, 224, nom, POLICE_11, actif ? TEXTE : TEXTE3, 255, CENTRE);
    }
}

// la bande de message (y 180..202) : le dernier message, ou une aide
static const int MY = 180, MH = 22;
static void bande_message(const char *aide)
{
    float age = t - msg_t0;
    bool m = msg_txt[0] && age >= 0 && age < MSG_DUREE;
    if (!m && !aide) return;
    uint16_t c = m ? msg_c : TEXTE3;
    rect_arrondi(6, MY, 308, MH, 11, PANNEAU2, 225);
    contour_arrondi(6, MY, 308, MH, 11, 1, c, m ? 200 : 90);
    disque(18, MY + MH / 2.0f, 3.5f, c);
    const char *s = m ? msg_txt : aide;
    const PoliceAA &p = largeur_texte(s, POLICE_13) <= 280 ? POLICE_13 : POLICE_11;
    texte(28, MY + (MH - hauteur(p)) / 2, s, p, m ? TEXTE : TEXTE2);
    if (m) {
        // la duree restante du message : un fin trait qui se vide
        float f = 1 - age / MSG_DUREE;
        rect(20, MY + MH - 3, (int)(280 * f), 1, c, 150);
    }
}

// ============================ ACCUEIL =======================================
static const int BX = 8, BY = 30, BL = 304, BH = 52;      // bandeau d'etat

static const char *nom_pub(int numero)
{
    for (int i = 0; i < base::MAX_BLOCS; i++)
        if (base::bloc(i).actif && (int)base::bloc(i).numero == numero) return base::info(i).nom;
    return "Pub";
}

static void bandeau()
{
    int e = V.etat;
    uint16_t c = C_ETAT[e], c2 = C_ETAT2[e];
    bool fort = e == det::PUB || e == det::ENREGISTREMENT;
    rect_arrondi(BX, BY, BL, BH, 14, PANNEAU2, 225);
    rect_arrondi_degrade(BX, BY, BL, BH, 14, c, c2, fort ? 90 : 40);
    contour_arrondi(BX, BY, BL, BH, 14, fort ? 2.0f : 1.2f, c, fort ? 230 : 140);
    rect_arrondi(BX + 6, BY + 9, 5, BH - 18, 2, c, 230);
    const char *mot = MOT_ETAT[e];
    if (V.test_etat == 1) mot = "J'ÉCOUTE";
    const PoliceAA *p = largeur_texte(mot, POLICE_30) <= 200 ? &POLICE_30 : &POLICE_22;
    texte(BX + 20, BY + 4, mot, *p, TEXTE);
    char l1[80], a[16], b[16];
    switch (e) {
    case det::PAUSE:
        snprintf(l1, sizeof l1, V.test_etat == 1 ? "Détection arrêtée pendant le test (3 s)" : "Touche « ARRÊTÉ » en haut pour reprendre");
        break;
    case det::PUB:
        if (V.distant) { snprintf(l1, sizeof l1, "Reconnue par la base de pubs"); break; }
        fmt_duree(a, sizeof a, V.pos_s);
        fmt_duree(b, sizeof b, V.duree_s);
        { const char *nm = nom_pub(V.numero); if (strncmp(nm, "Pub", 3) == 0) snprintf(l1, sizeof l1, "Pub n°%d · %s / %s", V.numero, a, b); else snprintf(l1, sizeof l1, "%s · n°%d · %s / %s", nm, V.numero, a, b); }
        break;
    case det::PROGRAMME: snprintf(l1, sizeof l1, "Rien de reconnu · %d pub%s apprise%s", base::nb_pubs(), base::nb_pubs() > 1 ? "s" : "", base::nb_pubs() > 1 ? "s" : ""); break;
    case det::ENREGISTREMENT: fmt_duree(a, sizeof a, V.enreg_s); snprintf(l1, sizeof l1, "Pub inconnue · %s · bouton rond à la fin", a); break;
    case det::APPRENTISSAGE: snprintf(l1, sizeof l1, "Aucune pub apprise : TESTER au début d'une pub"); break;
    default: snprintf(l1, sizeof l1, "%s", R.source ? "Pas de son sur l'entrée optique" : "Télé coupée ou trop loin du micro"); break;
    }
    decoupe(BX + 14, BY, BX + BL - 52, BY + BH);
    texte(BX + 20, BY + BH - 17, l1, POLICE_11, TEXTE2);
    sans_decoupe();
    // nombre de pubs, a droite
    char n[12];
    snprintf(n, sizeof n, "%d", base::nb_pubs());
    texte(BX + BL - 14, BY + 6, n, POLICE_22, TEXTE, 255, DROITE);
    texte(BX + BL - 14, BY + 30, base::nb_pubs() > 1 ? "pubs" : "pub", POLICE_11, TEXTE3, 255, DROITE);
}

// texte coupe aux espaces sur au plus n lignes de largeur l (POLICE_11)
static void texte_lignes(int x, int y, const char *s, int l, int n, uint16_t c)
{
    char ligne[96];
    while (*s && n-- > 0) {
        int k = 0, coupe = 0, len = (int)strlen(s);
        for (int i = 0; i <= len && i < (int)sizeof ligne - 1; i++) {
            if (s[i] != ' ' && s[i] != 0) continue;
            memcpy(ligne, s, i);
            ligne[i] = 0;
            if (largeur_texte(ligne, POLICE_11) > l) break;
            coupe = i;
        }
        if (!coupe) coupe = len < (int)sizeof ligne - 1 ? len : (int)sizeof ligne - 1;
        memcpy(ligne, s, coupe);
        ligne[coupe] = 0;
        texte(x, y, ligne, POLICE_11, c);
        y += 14;
        s += coupe;
        while (*s == ' ') s++;
        (void)k;
    }
}

// timelapse : les 30 dernieres minutes (une case toutes les 10 s), sous le bandeau
static const int RX = 4, RY = 84, RL = 312, RH = 8;
static void ruban()
{
    rect_arrondi(RX, RY, RL, RH, 3, PANNEAU2, 220);
    contour_arrondi(RX, RY, RL, RH, 3, 1, BORD, 120);
    uint32_t nc = det::nb_cases;
    for (int i = 0; i < det::NB_CASES; i++) {
        int32_t k = (int32_t)nc - det::NB_CASES + i;
        if (k < 0) continue;
        int e = det::historique[k % det::NB_CASES];
        if (e >= det::NB_ETATS) continue;
        int x0 = RX + i * RL / det::NB_CASES, x1 = RX + (i + 1) * RL / det::NB_CASES;
        bool fort = e == det::PUB || e == det::ENREGISTREMENT;
        rect(x0, RY, x1 - x0, RH, C_ETAT[e], fort ? 255 : 110);
    }
}

// spectrogramme en couleurs qui defile (v0.7.1) : l'historique du moteur d'empreintes
// (emp::spectro : 224 frequences de 250 Hz a 3,7 kHz, une colonne toutes les 32 ms), 1 pixel par
// colonne, les plus recentes a droite. Chaque frequence est comparee a sa moyenne sur la fenetre
// visible : les sons qui bougent ressortent, quel que soit le volume.
static uint16_t sp_pal[256];
static bool sp_pal_ok = false;
static void spectrogramme(int x, int y, int l, int h)
{
    if (!emp::spectro) return;
    if (!sp_pal_ok) {
        static const struct { int i, r, g, b; } P[] = { { 0, 6, 10, 30 }, { 50, 24, 36, 120 }, { 110, 130, 40, 190 },
                                                       { 170, 255, 90, 70 }, { 225, 255, 212, 64 }, { 255, 255, 255, 225 } };
        for (int k = 0; k < 5; k++)
            for (int i = P[k].i; i <= P[k + 1].i; i++) {
                int d = P[k + 1].i - P[k].i;
                int r = P[k].r + (P[k + 1].r - P[k].r) * (i - P[k].i) / d;
                int g = P[k].g + (P[k + 1].g - P[k].g) * (i - P[k].i) / d;
                int b = P[k].b + (P[k + 1].b - P[k].b) * (i - P[k].i) / d;
                sp_pal[i] = rgb(r, g, b);
            }
        sp_pal_ok = true;
    }
    const int NBN = emp::NB;
    if (l > emp::HIST - 4) l = emp::HIST - 4;
    if (h > 64) h = 64;
    rect_arrondi(x - 2, y - 2, l + 4, h + 4, 5, NOIR_BLEU, 235);
    int32_t n = (int32_t)emp::nb_colonnes;
    static float moy[emp::NB];
    int c0 = n - l < 0 ? 0 : n - l;                      // premiere colonne visible
    int nc = n - c0;
    if (nc < 2) return;
    for (int b = 0; b < NBN; b++) moy[b] = 0;
    for (int c = c0; c < n; c++) {
        const uint8_t *col = emp::spectro[c % emp::HIST];
        for (int b = 0; b < NBN; b++) moy[b] += col[b];
    }
    for (int b = 0; b < NBN; b++) moy[b] /= nc;
    for (int px = 0; px < nc; px++) {
        const uint8_t *col = emp::spectro[(c0 + px) % emp::HIST];
        uint16_t *dst = fb + y * L + x + (l - nc) + px;
        for (int py = 0; py < h; py++) {
            int b0 = (h - 1 - py) * NBN / h, b1 = (h - py) * NBN / h;
            if (b1 <= b0) b1 = b0 + 1;
            float m = -255;
            for (int b = b0; b < b1; b++) { float d = col[b] - moy[b]; if (d > m) m = d; }
            int v = (int)(45 + m * 2.2f);
            dst[py * L] = sp_pal[v < 0 ? 0 : (v > 255 ? 255 : v)];
        }
    }
    // constellation par-dessus : les pics forts (points) et le lien d'un pic vers sa cible (trait),
    // comme l'ecoute la carte pour fabriquer ses codes
    const int xs = x + (l - nc);
    auto px_de = [&](uint32_t t) { return xs + (int)((int32_t)t - c0); };
    auto py_de = [&](int f) { return y + h - 1 - f * h / NBN; };
    int32_t nl = (int32_t)emp::nb_liens;
    for (int32_t k = nl - 1; k >= 0 && k > nl - emp::NB_LIENS_AFF; k--) {
        const emp::Lien &q = emp::liens[k % emp::NB_LIENS_AFF];
        if ((int32_t)q.t0 < c0) break;
        trait(px_de(q.t0), py_de(q.f0), px_de(q.t1), py_de(q.f1), 1.0f, rgb(210, 240, 255), 150);
    }
    int32_t np = emp::pics ? (int32_t)emp::nb_pics : 0;
    for (int32_t k = np - 1; k >= 0 && k > np - emp::NB_PICS_AFF; k--) {
        const emp::Pic &q = emp::pics[k % emp::NB_PICS_AFF];
        if ((int32_t)q.t < c0) break;
        disque(px_de(q.t), py_de(q.f), 1.4f, rgb(255, 255, 255), 230);
    }
    s_anime = true;
}

// le gros bouton TESTER (cercle a gauche) ; pendant l'enregistrement : ENREGISTRER
static const float TCX = 62, TCY = 130, TR = 36;
static void gros_tester()
{
    bool appuye = tc_appui && tc_id == B_TESTER;
    bool ecoute = V.test_etat == 1, enreg = V.test_etat == 4;
    bool occupe = ecoute || enreg || s_attente;
    if (!s_attente) zone(B_TESTER, (int)(TCX - TR - 6), (int)(TCY - TR - 6), (int)(2 * TR + 12), (int)(2 * TR + 12));
    uint16_t c = s_attente ? GRIS : (enreg ? JAUNE : (V.test_etat == 2 ? VERT : CYAN));
    if (occupe) {
        // ondes qui partent du bouton
        for (int k = 0; k < 3; k++) {
            float ph = fmodf(t * 0.8f + k / 3.0f, 1.0f);
            anneau(TCX, TCY, TR + 2 + 24 * ph, 2, c, (uint8_t)(200 * (1 - ph)));
        }
        s_anime = true;
    } else if (V.test_etat == 2 && V.test_age_s < 1.2f) {
        float ph = V.test_age_s / 1.2f;
        anneau(TCX, TCY, TR + 26 * ph, 3, VERT, (uint8_t)(255 * (1 - ph)));
        s_anime = true;
    }
    float r = appuye ? TR - 2 : TR;
    disque(TCX, TCY, r + 3, NOIR_BLEU, 200);
    disque(TCX, TCY, r, appuye ? melange(c, TEXTE, 100) : c);
    disque(TCX, TCY, r - 6, melange(c, rgb(30, 120, 220), 120), 120);
    uint16_t cs = NOIR_BLEU;
    if (enreg) {
        // symbole : carre "stop" (fin de la pub)
        rect_arrondi((int)(TCX - 11), (int)(TCY - 11), 22, 22, 4, cs);
    } else {
        // symbole : trois arcs (ecoute) dans le cercle
        disque(TCX, TCY, 5, cs);
        for (int k = 1; k <= 2; k++) {
            float rr = 6 + 7 * k;
            float a = occupe ? 120 + 135 * pulse(1.2f, k * 0.8f) : 255;
            arc(TCX, TCY, rr, 2.6f, -0.7f, 0.7f, cs, (uint8_t)a);
            arc(TCX, TCY, rr, 2.6f, PI_F - 0.7f, PI_F + 0.7f, cs, (uint8_t)a);
        }
    }
    texte((int)TCX, (int)(TCY + TR + 2), s_attente ? "un instant…" : (ecoute ? "j'écoute…" : (enreg ? "ENREGISTRER" : "TESTER")), POLICE_11, occupe ? c : TEXTE, 255, CENTRE);
}

// colonne de droite : ce qui se passe (test, coupure du son, spectre au repos)
static const int CX = 124, CY = 94, CL = 190, CH = 80;
static void panneau_coupure()
{
    int ec = cpr::etat();
    bool test_tv = T.baisse && ec != cpr::COUPE;      // son baisse depuis le menu Tele
    uint16_t c = ec == cpr::REECOUTE ? JAUNE : (ec == cpr::CONFIRMATION ? ORANGE : MAGENTA);
    panneau(CX, CY, CL, CH, 190);
    contour_arrondi(CX, CY, CL, CH, 12, 1.5f, c, 200);
    char l1[48], l2[48];
    int reste = cpr::reste_s();
    float f = cpr::fraction_restante();
    if (ec == cpr::CONFIRMATION) { snprintf(l1, sizeof l1, "PUB CONFIRMÉE…"); snprintf(l2, sizeof l2, "le son baisse dans 1 s"); }
    else if (ec == cpr::COUPE) {
        snprintf(l1, sizeof l1, cpr::palier() ? "LE SON REMONTE" : "SON BAISSÉ");   // v0.9.65 : paliers de fin d'ecran
        if (cpr::palier()) snprintf(l2, sizeof l2, "palier %d/4 · %d:%02d", cpr::palier(), reste / 60, reste % 60);
        else if (cpr::coupe_fibre()) snprintf(l2, sizeof l2, "suit la pub · %d:%02d", reste / 60, reste % 60);   // v0.9.63 : fibre, sans reecoute
        else snprintf(l2, sizeof l2, "remonte dans %d:%02d", reste / 60, reste % 60);
    } else if (ec == cpr::REECOUTE) { snprintf(l1, sizeof l1, "SON REMONTÉ"); snprintf(l2, sizeof l2, "j'écoute encore %d s", reste); }
    else { snprintf(l1, sizeof l1, "SON BAISSÉ (essai)"); snprintf(l2, sizeof l2, "menu Télé → Rétablir"); f = 1; }
    texte(CX + 10, CY + 6, l1, POLICE_16, c);
    texte(CX + 10, CY + 26, l2, POLICE_13, TEXTE);
    // barre du temps
    int bx = CX + 10, bl = CL - 20, by = CY + 46;
    rect_arrondi(bx, by, bl, 6, 3, NOIR_BLEU, 230);
    if (f > 0.005f) rect_arrondi(bx, by, (int)(bl * f), 6, 3, c);
    // volume
    char v[48];
    if (T.action != tv::RIEN && T.depart >= 0) snprintf(v, sizeof v, "Volume %d → %d…", T.volume, T.cible);
    else if (T.volume_connu) snprintf(v, sizeof v, "Volume télé : %d", T.volume);
    else if (T.etat != tv::CONNECTEE) snprintf(v, sizeof v, "Télé : %s", tv::nom_etat(T.etat));
    else snprintf(v, sizeof v, "Volume inconnu");
    texte(CX + 10, CY + 58, v, POLICE_11, TEXTE2);
    if (!test_tv) bouton(B_SON, CX + CL - 84, CY + 56, 76, 24, "Remettre", nullptr, VERT, CYAN);
    else bouton(B_SON, CX + CL - 84, CY + 56, 76, 24, "Rétablir", nullptr, VERT, CYAN);
    s_anime = true;
}

static void ecran_accueil()
{
    bandeau();
    ruban();
    gros_tester();
    int ec = cpr::etat();
    bool coupure = ec != cpr::REPOS || T.baisse;
    const char *aide = nullptr;
    if (coupure && V.test_etat != 4 && !s_attente) panneau_coupure();
    else {
        char l1[48], l2[64];
        uint16_t c = TEXTE;
        bool annuler = false, spec = false;
        if (s_attente) {
            snprintf(l1, sizeof l1, s_attente == 2 ? "J'enregistre la pub…" : (s_attente == 3 ? "J'annule…" : "Je commence…"));
            snprintf(l2, sizeof l2, "un instant");
            c = s_attente == 2 ? VERT : (s_attente == 3 ? JAUNE : CYAN);
            s_anime = true;
        } else if (V.test_etat == 1) { snprintf(l1, sizeof l1, "J'écoute 3 s…"); snprintf(l2, sizeof l2, "Je cherche cette pub"); c = CYAN; annuler = true; }
        else if (V.test_etat == 4) {
            // enregistrement : seulement le temps (barre jusqu'a 90 s) et ANNULER ;
            // l'etat est dans le bandeau, la fin se fait avec le bouton rond
            char d[12], m[12];
            fmt_duree(d, sizeof d, V.enreg_s);
            fmt_duree(m, sizeof m, (float)det::ENREG_MAX_S);
            snprintf(l1, sizeof l1, "%s / %s", d, m);
            l2[0] = 0;
            c = JAUNE;
            annuler = true;
            float f = sat01(V.enreg_s / det::ENREG_MAX_S);
            rect_arrondi(CX, CY + 30, CL, 8, 4, NOIR_BLEU, 230);
            if (f > 0.005f) rect_arrondi(CX, CY + 30, (int)(CL * f), 8, 4, f > 0.85f ? ORANGE : JAUNE);
            s_anime = true;
        } else if (V.test_etat == 2 && V.test_appris) {
            snprintf(l1, sizeof l1, "Pub n°%d apprise ✓", V.test_num);
            int r = det::ANNULER_S - (int)V.test_age_s;
            snprintf(l2, sizeof l2, "Annuler possible encore %d s", r > 0 ? r : 0); c = VERT; annuler = true;
            s_anime = true;
        } else if (V.test_etat == 2) { snprintf(l1, sizeof l1, "Pub reconnue ✓"); snprintf(l2, sizeof l2, "%s · n°%d", nom_pub(V.test_num), V.test_num); c = VERT; spec = true; }
        else if (V.etat == det::PUB) { snprintf(l1, sizeof l1, "Pub reconnue"); snprintf(l2, sizeof l2, "Erreur ? « Pas une pub » corrige"); c = MAGENTA; }
        else { snprintf(l1, sizeof l1, "Une pub commence ?"); snprintf(l2, sizeof l2, "TESTER au début de la pub"); c = TEXTE; spec = true; }
        texte(CX, CY + 6, l1, POLICE_16, c);
        if (l2[0]) texte_lignes(CX, CY + 26, l2, CL, spec ? 1 : 2, TEXTE2);
        if (spec) spectrogramme(CX, CY + 44, CL, 36);
        if (V.test_etat == 4 && !s_attente) bouton(B_ANNULER, CX, CY + 50, CL, 30, "ANNULER", nullptr, JAUNE, ORANGE);
        else if (annuler) bouton(B_ANNULER, CX, CY + 54, 110, 26, "ANNULER", nullptr, JAUNE, ORANGE);
        else if (V.etat == det::PUB && !V.distant) bouton2(B_PAS_PUB, CX, CY + 54, 110, 26, "Pas une pub", ROUGE);
        if (!det::actif) aide = "Détecteur arrêté : touche l'étiquette en haut";
        else if (T.etat == tv::NON_APPAIREE) aide = "Télé pas encore appairée : onglet Télé";
        else if (T.etat != tv::CONNECTEE) aide = "Télé hors ligne : allumée ? (onglet Télé)";
    }
    // statistiques de la bibliotheque (v0.6.7). v0.9.56 : toujours visibles, en alternance toutes
    // les 4 s avec le message du moment : chiffres (pubs · a verifier · detections), puis date et version
    static char stats[96];
    uint32_t tour = (plat_ms() / 4000) % (aide ? 3 : 2);
    if (aide && tour > 0) { aide = nullptr; tour--; }
    if (!aide) {
        char vu[16];
        fmt_duree(vu, sizeof vu, V.pub_jour_s);
        bool sd_o = false, sd_e = false;
        uint32_t sd_a = 0;
        det::sauvegarde(sd_o, sd_a, sd_e);
        char dh[16] = "";
        if (plat_heure()) { time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt); strftime(dh, sizeof dh, "%d/%m %H:%M · ", &lt); }   // v0.9.22 : date de la carte
        if (sd_e) snprintf(stats, sizeof stats, "%s%s · ÉCHEC sauvegarde SD · %d pubs", dh, PUB_VERSION, base::nb_pubs());
        else if (tour == 0) snprintf(stats, sizeof stats, "%d pubs · %d à vérifier · %lu détections",
                                     base::nb_pubs(), base::nb_a_verifier(), (unsigned long)base::nb_detections());
        else snprintf(stats, sizeof stats, "%s%s · vue %s · mém. %.0f %%", dh, PUB_VERSION, vu, base::remplissage() * 100);
        aide = stats;
    }
    bande_message(aide);
}

// « Remettre le son » : le son revient sans rien changer a la bibliotheque
void remettre_son()
{
    bool coupe = cpr::liberer();
    if (T.baisse && !coupe) tv::retablir();           // baisse a l'essai (menu Tele)
    if (coupe) message("Son remis jusqu'à la fin de cet écran pub", VERT, "La bibliothèque n'est pas changée.");
    else if (T.baisse) message("Volume rétabli", VERT);
    else message("Le son n'était pas baissé", VERT);
}

// ============================ TELE ==========================================
enum { TV_ACCUEIL, TV_CLAVIER_IP, TV_CLAVIER_CODE, TV_TEST, TV_OUBLIER };
static int tv_page = TV_ACCUEIL;
static char saisie[20] = "";
static const char TOUCHES[18] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'A', 'B', 'C', 'D', 'E', 'F', '.', '<' };

static void ligne_info(int y, const char *nom, const char *val, uint16_t c)
{
    texte(18, y, nom, POLICE_11, TEXTE3);
    texte(104, y, val, POLICE_11, c);
}

static void ecran_tele_accueil()
{
    panneau(8, 30, 304, 84, 170);
    uint16_t ce = T.etat == tv::CONNECTEE ? VERT : (T.etat == tv::NON_APPAIREE ? TEXTE3 : (T.etat >= tv::APP_CLE ? CYAN : ORANGE));
    disque(22, 44, 5, ce);
    texte(34, 36, "Télécommande Wi-Fi", POLICE_16, TEXTE);
    char b[64];
    snprintf(b, sizeof b, "%s", tv::nom_etat(T.etat));
    texte(306, 38, b, POLICE_11, ce, 255, DROITE);
    ligne_info(58, "Télé (IP)", T.ip[0] ? T.ip : "à régler", T.ip[0] ? TEXTE : ORANGE);
    ligne_info(73, "Appairage", T.appairee ? "fait (clé sur la carte SD)" : "à faire", T.appairee ? TEXTE : ORANGE);
    if (T.volume_connu) snprintf(b, sizeof b, "%d / %d%s", T.volume, T.volume_max, T.muet ? " · muet" : "");
    else snprintf(b, sizeof b, "inconnu");
    ligne_info(88, "Volume", b, T.volume_connu ? TEXTE : TEXTE3);
    snprintf(b, sizeof b, "bas %ld · haut %ld%s", (long)R.tv_bas, (long)R.tv_haut, R.tv_origine ? " (ou celui d'avant)" : "");
    ligne_info(101, "Pendant pub", b, TEXTE2);
    bool app = T.etat >= tv::APP_CLE && T.etat != tv::APP_CODE;
    bouton(B_TV_IP, 8, 122, 72, 46, "IP", "de la télé", rgb(30, 170, 230), CYAN);
    bouton(B_TV_APPAIRER, 86, 122, 72, 46, T.appairee ? "RÉAPPAIRER" : "APPAIRER", "code télé", VIOLET, MAGENTA, T.ip[0] && !app);
    bouton(B_TV_TEST, 164, 122, 72, 46, "ESSAYER", "le volume", VERT, CYAN, T.appairee);
    bouton(B_TV_OUBLIER, 242, 122, 70, 46, "OUBLIER", "la télé", ROUGE, MAGENTA, T.appairee && !app);
    const char *aide = nullptr;
    if (T.etat == tv::APP_CLE || T.etat == tv::APP_CONNEXION || T.etat == tv::APP_VALIDATION) { aide = tv::nom_etat(T.etat); s_anime = true; }
    else if (!T.ip[0]) aide = "1) adresse IP de la télé  2) appairer  3) essayer";
    else if (!T.appairee) aide = "Allume la télé puis touche APPAIRER";
    bande_message(aide);
}

static void clavier(bool ip)
{
    // champ de saisie
    panneau(8, 28, 304, 29, 200);
    const char *titre = ip ? "IP de la télé" : "Code de la télé";
    texte(16, 35, titre, POLICE_11, TEXTE3);
    char aff[24];
    snprintf(aff, sizeof aff, "%s%s", saisie, fmodf(t, 1.0f) < 0.5f ? "|" : " ");
    texte(112, 33, aff, POLICE_16, TEXTE);
    if (!ip && T.etat == tv::APP_CODE) {
        char r[24];
        snprintf(r, sizeof r, "%d s", T.app_reste_s);
        texte(304, 35, r, POLICE_11, T.app_reste_s < 30 ? ORANGE : TEXTE3, 255, DROITE);
    } else if (!ip) texte(304, 35, tv::nom_etat(T.etat), POLICE_11, CYAN, 255, DROITE);
    s_anime = true;                                    // curseur qui clignote
    // 18 touches (6 x 3)
    const int KX = 8, KY = 60, KL = 49, KH = 28, G = 2;
    for (int i = 0; i < 18; i++) {
        int cx = KX + (i % 6) * (KL + 3), cy = KY + (i / 6) * (KH + G);
        char k = TOUCHES[i];
        bool actif = ip ? ((k >= '0' && k <= '9') || k == '.' || k == '<') : (k != '.');
        char lab[4] = { k, 0, 0, 0 };
        if (k == '<') strcpy(lab, "←");
        bouton2(B_CLAVIER0 + i, cx, cy, KL, KH, lab, k == '<' ? ORANGE : CYAN, actif);
    }
    int y = KY + 3 * (KH + G) + 2;
    bouton2(B_CL_ANNULER, 8, y, 120, 26, "Annuler", TEXTE3);
    bool ok = ip ? cfg::ip_valide(saisie) && saisie[0] : strlen(saisie) == 6;
    bouton(B_CL_OK, 192, y, 120, 26, ip ? "VALIDER" : "ENVOYER", nullptr, VERT, CYAN, ok);
    bande_message(ip ? "Chiffres et points, ex. 192.168.1.55 (box → appareils)" : "6 caractères 0-9 A-F, comme sur la télé");
}

static void ecran_tele_test()
{
    panneau(8, 30, 304, 52, 170);
    char b[64];
    if (T.volume_connu) snprintf(b, sizeof b, "Volume de la télé : %d / %d%s", T.volume, T.volume_max, T.muet ? " (muet)" : "");
    else snprintf(b, sizeof b, "Volume de la télé : inconnu");
    texte(18, 36, b, POLICE_13, TEXTE);
    if (T.action != tv::RIEN) { snprintf(b, sizeof b, "En cours : %d → %d", T.volume, T.cible); s_anime = true; }
    else snprintf(b, sizeof b, "Télé %s · niveau bas %ld · haut %ld", tv::nom_etat(T.etat), (long)R.tv_bas, (long)R.tv_haut);
    texte(18, 58, b, POLICE_11, T.action != tv::RIEN ? JAUNE : TEXTE2);
    bool ok = T.appairee;
    bouton(B_TV_COUPER, 8, 88, 98, 40, "COUPER", "→ niveau bas", MAGENTA, ROUGE, ok);
    bouton(B_TV_DIMINUER, 111, 88, 98, 40, "DIMINUER", "un cran", ORANGE, JAUNE, ok);
    bouton(B_TV_AUGMENTER, 214, 88, 98, 40, "AUGMENTER", "un cran", VERT, CYAN, ok);
    bouton(B_TV_RETABLIR, 8, 134, 148, 40, "RÉTABLIR", R.tv_origine ? "volume d'avant" : "→ niveau haut", VERT, CYAN, ok);
    bouton(B_TV_LIRE, 164, 134, 148, 40, "LIRE", "le volume", rgb(30, 170, 230), CYAN, ok);
    bouton2(B_TV_RETOUR, 252, 34, 54, 20, "Retour", TEXTE3);
    bande_message("Essaie les boutons : le volume de la télé change");
}

static void ecran_tele_oublier()
{
    panneau(8, 34, 304, 90, 200);
    contour_arrondi(8, 34, 304, 90, 12, 1.5f, ROUGE, 200);
    texte(160, 44, "Oublier la télé ?", POLICE_16, ROUGE, 255, CENTRE);
    texte(160, 68, "La clé et le certificat sont effacés de la carte SD.", POLICE_11, TEXTE2, 255, CENTRE);
    texte(160, 84, "Sur la télé, retire aussi « Detecteur de pub »", POLICE_11, TEXTE2, 255, CENTRE);
    texte(160, 98, "(Paramètres → télécommandes et accessoires).", POLICE_11, TEXTE2, 255, CENTRE);
    bouton2(B_TV_RETOUR, 8, 134, 148, 40, "Non, garder", TEXTE3);
    bouton(B_TV_OUBLIER_OUI, 164, 134, 148, 40, "OUBLIER", "il faudra réappairer", ROUGE, MAGENTA);
}

static void ecran_tele()
{
    // l'appairage demande le code : le clavier s'ouvre tout seul
    static int etat_vu = -1;
    if (T.etat != etat_vu) {
        if (T.etat == tv::APP_CODE && tv_page != TV_CLAVIER_CODE) { tv_page = TV_CLAVIER_CODE; saisie[0] = 0; }
        if (etat_vu == tv::APP_CODE && T.etat != tv::APP_CODE && T.etat != tv::APP_VALIDATION && tv_page == TV_CLAVIER_CODE) tv_page = TV_ACCUEIL;
        etat_vu = T.etat;
    }
    switch (tv_page) {
    case TV_CLAVIER_IP: clavier(true); break;
    case TV_CLAVIER_CODE: clavier(false); break;
    case TV_TEST: ecran_tele_test(); break;
    case TV_OUBLIER: ecran_tele_oublier(); break;
    default:
        ecran_tele_accueil();
        if (T.etat == tv::APP_CLE || T.etat == tv::APP_CONNEXION) bouton2(B_TV_ANNULER_APP, 226, 52, 80, 22, "Annuler", TEXTE3);
        break;
    }
}

// ============================ PUBS ==========================================
static bool conf_effacer = false;
static bool conf_cap = false;                    // v0.9.10 : « supprimer cette captation ? »

// ============================ CAPTER (v0.6.6) ===============================
// grands boutons CAPTER (son brut du micro, 15 min au plus) et TEST FO (10 s de l'entree
// optique), spectre et niveau du micro comme a l'accueil.
static void gros_bouton(int id, int x, int y, int l, int h, const char *titre, const char *sous, uint16_t ca, uint16_t cb, bool actif)
{
    bouton(id, x, y, l, h, "", nullptr, ca, cb, actif);
    bool appuye = tc_appui && tc_id == id;
    int dy = appuye ? 1 : 0;
    uint16_t ct = actif ? NOIR_BLEU : TEXTE3;
    const PoliceAA &p = largeur_texte(titre, POLICE_16) <= l - 10 ? POLICE_16 : POLICE_13;
    if (sous) {
        texte(x + l / 2, y + dy + h / 2 - 20, titre, p, ct, 255, CENTRE);
        texte(x + l / 2, y + dy + h / 2 + 4, sous, POLICE_13, ct, 220, CENTRE);
    } else texte(x + l / 2, y + dy + (h - hauteur(p)) / 2, titre, p, ct, 255, CENTRE);
}

static void ecran_capter()
{
    Captation cp;
    plat_captation(cp);
    TestOptique fo;
    plat_fo_test(fo);
    char b[72], d[12], m[12];               // v0.9.57 : 72 (« · N jetées » etait coupe)
    fmt_duree(m, sizeof m, (float)CAPTATION_MAX_S);
    // ---- CAPTER / ARRETER ----
    if (cp.etat == 2) {
        fmt_duree(d, sizeof d, cp.duree_s);
        if (cp.automatique) snprintf(b, sizeof b, "%s · %d %s · %d connues", d, cp.auto_gardes, cp.apprendre ? "appr." : "nouv.", cp.auto_connues);
        else snprintf(b, sizeof b, "%s / %s", d, m);
        gros_bouton(B_CAPTER, 8, 32, 196, 70, "ARRÊTER", b, ORANGE, ROUGE, true);
        s_anime = true;
    } else if (cp.etat == 1) gros_bouton(B_CAPTER, 8, 32, 196, 70, "un instant…", "je prépare le fichier", ORANGE, ROUGE, false);
    else if (cp.etat == 3) gros_bouton(B_CAPTER, 8, 32, 196, 70, "j'arrête…", "je ferme le fichier", ORANGE, ROUGE, false);
    else {
        // v0.9.39 : CAPTER (fichier entier) et AUTO (entier + decoupe des pubs inconnues)
        // v0.9.53 : + APPRENDRE (les pubs inconnues vont directement dans la bibliotheque)
        bool libre = plat_sd() && fo.etat == 0;
        gros_bouton(B_CAPTER, 8, 32, 60, 70, "CAPTER", "entier", rgb(30, 170, 230), CYAN, libre);
        gros_bouton(B_CAP_AUTO, 72, 32, 62, 70, "AUTO", "découpe", rgb(120, 90, 230), VIOLET, libre);
        gros_bouton(B_CAP_APPR, 138, 32, 86, 70, "APPRENDRE", "bibliothèque", JAUNE, ORANGE, libre);
    }
    float f = cp.etat >= 2 ? sat01(cp.duree_s / CAPTATION_MAX_S) : 0;
    rect_arrondi(8, 106, 196, 8, 4, NOIR_BLEU, 230);
    if (f > 0.003f) rect_arrondi(8, 106, (int)(196 * f) < 8 ? 8 : (int)(196 * f), 8, 4, f > 0.9f ? ORANGE : CYAN);
    // ---- ANNULER la captation (v0.9.10) : a la place de TEST FO pendant la captation ----
    bool capt_en_cours = cp.etat == 1 || cp.etat == 2;
    if (!capt_en_cours) conf_cap = false;
    if (capt_en_cours) {
        if (!conf_cap) gros_bouton(B_CAP_ANNULER, 212, 32, 100, 70, "ANNULER", cp.apprendre ? "pubs gardées" : "jeter le fichier", JAUNE, ORANGE, true);
        else {
            bouton(B_CAP_OUI, 212, 32, 100, 34, "SUPPRIMER", nullptr, ROUGE, ORANGE, true);
            bouton2(B_CAP_NON, 212, 68, 100, 34, "NON", TEXTE, true);
        }
    } else
    // ---- TEST FO ----
    if (fo.etat == 1) {
        snprintf(b, sizeof b, "%.0f / %d s", fo.duree_s, FO_TEST_S);
        gros_bouton(B_TEST_FO, 228, 32, 84, 70, "FO…", b, ORANGE, ROUGE, false);
        s_anime = true;
    } else if (fo.etat == 2) gros_bouton(B_TEST_FO, 228, 32, 84, 70, "écrit…", "sur la SD", ORANGE, ROUGE, false);
    else gros_bouton(B_TEST_FO, 228, 32, 84, 70, "TEST FO", "optique", rgb(30, 170, 230), CYAN, plat_sd() && cp.etat == 0);
    if (!capt_en_cours) {
        rect_arrondi(228, 106, 84, 8, 4, NOIR_BLEU, 230);
        if (fo.etat == 1) {
            float h = sat01(fo.duree_s / FO_TEST_S);
            if (h > 0.003f) rect_arrondi(228, 106, (int)(84 * h) < 8 ? 8 : (int)(84 * h), 8, 4, CYAN);
        }
    }
    // ---- ce que le micro entend (comme l'accueil) ----
    panneau(8, 120, 304, 58, 160);
    spectrogramme(16, 123, 288, 20);
    float niv = mot::niveau_db();
    const char *qual;
    uint16_t cq;
    if (!plat_audio_ok()) { qual = "micro absent"; cq = ORANGE; }
    else if (mot::sature()) { qual = "saturé : baisse le son de la télé"; cq = ROUGE; }
    else if (niv < mot::NIVEAU_FAIBLE) { qual = "trop faible : monte le son"; cq = ROUGE; }
    else if (niv > mot::NIVEAU_FORT) { qual = "un peu fort"; cq = JAUNE; }
    else { qual = "niveau bon"; cq = VERT; }
    snprintf(b, sizeof b, "%.0f dB · %s", niv, qual);
    texte(16, 146, b, POLICE_11, cq);
    bool en_auto = cp.automatique && (cp.etat == 1 || cp.etat == 2);
    if (en_auto) {
        char t1[12], t2[12];
        fmt_duree(t1, sizeof t1, cp.auto_total_s);
        fmt_duree(t2, sizeof t2, cp.auto_seg_s);
        char t3[12];
        fmt_duree(t3, sizeof t3, cp.auto_non_s);
        if (cp.auto_non_s >= 1) snprintf(b, sizeof b, "Gardé %s · non traité %s", t1, t3);   // v0.9.57
        else snprintf(b, sizeof b, "Gardé %s · en cours %s", t1, t2);
    } else snprintf(b, sizeof b, "%.0f pics/s", emp::pics_par_s());
    texte(304, 146, b, POLICE_11, TEXTE2, 255, DROITE);
    // ---- chaine de la captation (v0.9.13) : ecrite dans index.csv, reprise par gestion_pubs.py ----
    if (en_auto) {
        // v0.9.41 : statistiques en direct a la place de la chaine
        snprintf(b, sizeof b, "%d %s · %d connues · %d courtes · %d jetées", cp.auto_gardes, cp.apprendre ? "appr." : "enreg.", cp.auto_connues, cp.auto_courtes, cp.auto_jetes);
        texte(160, 160, b, POLICE_11, TEXTE, 255, CENTRE);
    } else {
        int ch = plat_captation_chaine();
        bool libre = cp.etat == 0;
        char nom[56];
        if (ch > 0 && ch < base::NB_CHAINES) snprintf(nom, sizeof nom, "Chaîne : %s", base::CHAINES[ch]);
        else snprintf(nom, sizeof nom, "Chaîne : auto (%s)", V.chaine > 0 && V.chaine < base::NB_CHAINES ? base::CHAINES[V.chaine] : "?");
        bouton2(B_CAP_CH_MOINS, 14, 158, 34, 18, "<", CYAN, libre, true);
        bouton2(B_CAP_CH_PLUS, 272, 158, 34, 18, ">", CYAN, libre, true);
        texte(160, 160, nom, POLICE_13, libre ? TEXTE : TEXTE3, 255, CENTRE);
    }
    const char *aide = nullptr;
    if (!plat_sd()) aide = "Pas de carte SD : captation impossible";
    else if (en_auto && cp.auto_nev > 0) aide = cp.auto_ev[0].txt;
    else if (cp.etat == 2) aide = "Son de la télé jamais baissé pendant la captation";
    else if (cp.etat == 0 && cp.auto_bilan) {
        static char bilan[96];
        char t1[12];
        fmt_duree(t1, sizeof t1, cp.auto_total_s);
        char t2[12];
        fmt_duree(t2, sizeof t2, cp.auto_non_s);
        // v0.9.57 : le son non traite est montre ; captation gardee (filet) signalee
        if (cp.apprendre && cp.auto_garde)
            snprintf(bilan, sizeof bilan, "%d appr. · %d conn. · cap. n°%lu gardée", cp.auto_gardes, cp.auto_connues, (unsigned long)cp.numero);
        else if (cp.apprendre) snprintf(bilan, sizeof bilan, "APPRENDRE : %d appr. · %d conn. · non traité %s", cp.auto_gardes, cp.auto_connues, t2);
        else snprintf(bilan, sizeof bilan, "AUTO : %d enreg. · %d conn. · non déc. %s", cp.auto_gardes, cp.auto_connues, t2);
        aide = bilan;
    }
    else if (fo.etat == 0 && cp.etat == 0) aide = "Fichiers dans /pub/captations/ de la carte SD";
    bande_message(aide);
}


// ============================ JOURNAL =======================================
static uint32_t jr_fige = 0;                 // 0 = en direct ; sinon derniere ligne affichee
static const int JR_Y0 = 56, JR_Y1 = 198, JR_LH = 13;

static uint16_t couleur_ligne(const char *s)
{
    if (s[0] == 'E' && s[1] == ' ' && s[2] == '(') return ROUGE;
    if (s[0] == 'W' && s[1] == ' ' && s[2] == '(') return ORANGE;
    if (strstr(s, "ECHEC") || strstr(s, "chec") || strstr(s, "impossible") || strstr(s, "Erreur")) return ROUGE;
    if (strncmp(s, "[TV]", 4) == 0) return CYAN;
    if (strncmp(s, "[PUB]", 5) == 0) return TEXTE;
    return TEXTE3;
}

// coupe une ligne en morceaux qui tiennent dans la largeur ; rend le nombre de morceaux
static int couper(const char *s, int largeur, int *debuts, int max)
{
    int n = 0, i = 0, len = (int)strlen(s);
    while (i < len && n < max) {
        debuts[n++] = i;
        char tmp[JOURNAL_LARGEUR];
        int j = i, dernier_espace = -1;
        while (j < len) {
            int k = j + 1;
            while (k < len && ((unsigned char)s[k] & 0xC0) == 0x80) k++;
            memcpy(tmp, s + i, k - i);
            tmp[k - i] = 0;
            if (largeur_texte(tmp, POLICE_11) > largeur) break;
            if (s[j] == ' ') dernier_espace = j;
            j = k;
        }
        if (j >= len) break;
        i = (dernier_espace > i) ? dernier_espace + 1 : (j > i ? j : i + 1);
    }
    return n;
}

static void ecran_journal()
{
    bool on = R.journal != 0;
    // interrupteur
    bool appuye = tc_appui && tc_id == B_JR_ONOFF;
    zone(B_JR_ONOFF, 214, 26, 106, 28);
    rect_arrondi(262, 31, 46, 20, 10, on ? melange(PANNEAU2, VERT, 110) : PANNEAU2, 240);
    contour_arrondi(262, 31, 46, 20, 10, appuye ? 2 : 1, on ? VERT : BORD, 220);
    disque(on ? 298 : 272, 41, 7, on ? VERT : TEXTE2);
    texte(254, 34, on ? "Journal" : "Coupé", POLICE_11, on ? TEXTE : TEXTE3, 255, DROITE);
    uint32_t der = plat_journal_dernier();
    if (jr_fige && jr_fige >= der) jr_fige = 0;
    char h[64];
    if (!on) snprintf(h, sizeof h, "Coupé : seules les erreurs sont gardées");
    else if (jr_fige) snprintf(h, sizeof h, "Ligne %lu / %lu · pause", (unsigned long)jr_fige, (unsigned long)der);
    else snprintf(h, sizeof h, "Direct · %lu lignes", (unsigned long)der);
    texte(10, 34, h, POLICE_11, jr_fige ? JAUNE : TEXTE2);
    panneau(4, JR_Y0 - 3, 312, JR_Y1 - JR_Y0 + 6, 170);
    zone(B_JR_HAUT, 4, JR_Y0, 312, (JR_Y1 - JR_Y0) / 2);
    zone(B_JR_BAS, 4, (JR_Y0 + JR_Y1) / 2, 312, (JR_Y1 - JR_Y0) / 2);
    static LigneJournal *lignes = (LigneJournal *)plat_malloc_grand(24 * sizeof(LigneJournal));
    if (!lignes) return;
    int n = plat_journal(lignes, 24, jr_fige);
    int y = JR_Y1 - JR_LH;
    decoupe(6, JR_Y0, 314, JR_Y1);
    for (int i = n - 1; i >= 0 && y >= JR_Y0 - JR_LH; i--) {
        const char *s = lignes[i].txt;
        int debuts[6];
        int nm = couper(s, 300, debuts, 6);
        uint16_t c = couleur_ligne(s);
        for (int m = nm - 1; m >= 0 && y >= JR_Y0 - JR_LH; m--) {
            char morceau[JOURNAL_LARGEUR];
            int fin = m + 1 < nm ? debuts[m + 1] : (int)strlen(s);
            int lg = fin - debuts[m];
            memcpy(morceau, s + debuts[m], lg);
            morceau[lg] = 0;
            texte(m ? 16 : 8, y, morceau, POLICE_11, c);
            y -= JR_LH;
        }
    }
    sans_decoupe();
    if (!n) texte(160, (JR_Y0 + JR_Y1) / 2 - 6, on ? "Pas encore de message" : "Journal coupé (le port USB reçoit tout)", POLICE_11, TEXTE3, 255, CENTRE);
}

// ============================ REGLAGES ======================================
// v0.9.66 : page 4 = preregages, montree en premier dans la barre (les pages 0..3 gardent leurs numeros)
static int page_rg = 4;
static const char *PAGES_RG[5] = { "Profils", "Son télé", "Détection", "Carte", "Wi-Fi" };
static const int RY0 = 60, RDY = 28;            // premiere ligne, ecart

static void ligne_rg(int i, const char *nom, const char *detail = nullptr)
{
    int y = RY0 + i * RDY;
    texte(12, y + 5, nom, POLICE_13, TEXTE);
    (void)detail;                                   // (pas de 2e ligne : l'ecran reste lisible)
}

static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
static char qr_texte[64] = "";
static bool qr_ok = false;
static void dessiner_qr(const char *txt, int x, int y, int taille)
{
    if (strcmp(txt, qr_texte) != 0) {
        static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
        strncpy(qr_texte, txt, sizeof qr_texte - 1);
        qr_texte[sizeof qr_texte - 1] = 0;
        qr_ok = txt[0] && qrcodegen_encodeText(txt, tmp, qr, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO, true);
    }
    rect_arrondi(x, y, taille, taille, 8, rgb(255, 255, 255));
    if (!qr_ok) return;
    int n = qrcodegen_getSize(qr), m = (taille - 10) / n, o = (taille - m * n) / 2;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            if (qrcodegen_getModule(qr, i, j)) rect(x + o + i * m, y + o + j * m, m, m, NOIR_BLEU);
}

static void ecran_reglages()
{
    segments(B_PAGE0, 8, 30, 304, 22, PAGES_RG, 5, page_rg == 4 ? 0 : page_rg + 1);
    char v[24];
    const int SX = 168;                         // colonne des reglages
    if (page_rg == 4) {
        // v0.9.66 : 4 preregages (on les enregistre depuis le portail, onglet Reglages)
        int actif = cfg::prereg_actif();
        for (int i = 0; i < cfg::NB_PREREG; i++) {
            cfg::Prereglage p;
            if (!cfg::prereg(i, p)) continue;
            int x = 8 + (i % 2) * 156, y = 60 + (i / 2) * 66, l = 148, h = 60;
            char r[40];
            snprintf(r, sizeof r, "%s · son %ld · %ld s", p.source ? "optique" : "micro", (long)p.tv_bas, (long)p.coupure_s);
            if (i == actif) bouton(B_PREREG0 + i, x, y, l, h, p.nom, "actif", VERT, CYAN);
            else {
                bouton2(B_PREREG0 + i, x, y, l, h, "", CYAN);
                texte(x + l / 2, y + 14, p.nom, POLICE_13, TEXTE, 255, CENTRE);
                texte(x + l / 2, y + 36, r, POLICE_11, TEXTE2, 255, CENTRE);
            }
        }
    } else if (page_rg == 0) {
        ligne_rg(0, "Volume pendant la pub", "0 = silence");
        snprintf(v, sizeof v, "%ld", (long)R.tv_bas); stepper(0, SX, RY0, v);
        ligne_rg(1, "Volume après la pub");
        snprintf(v, sizeof v, "%ld", (long)R.tv_haut); stepper(1, SX, RY0 + RDY, v, !R.tv_origine);
        ligne_rg(2, "Après la pub");
        static const char *const O[2] = { "Niveau haut", "Celui d'avant" };
        segments(B_RG_CHOIX0 + 2 * 4, SX - 10, RY0 + 2 * RDY + 2, 152, 22, O, 2, R.tv_origine ? 1 : 0);
        ligne_rg(3, "Coupure", R.coupure ? "le son remonte pour réécouter" : "tant que la pub est reconnue");
        static const char *const C[2] = { "Suivre", "Fenêtres" };
        segments(B_RG_CHOIX0 + 3 * 4, SX - 10, RY0 + 3 * RDY + 2, 152, 22, C, 2, R.coupure);
        ligne_rg(4, "Durée d'une coupure");
        snprintf(v, sizeof v, "%ld s", (long)R.coupure_s); stepper(4, SX, RY0 + 4 * RDY, v, R.coupure == 1);
    } else if (page_rg == 1) {
        ligne_rg(0, "Sensibilité", "votes pour dire PUB");
        static const char *const S[3] = { "Souple", "Normal", "Strict" };
        segments(B_RG_CHOIX0 + 0 * 4, SX - 30, RY0 + 2, 172, 22, S, 3, R.seuil);
        ligne_rg(1, "Gain du micro");
        if (R.gain == 0) snprintf(v, sizeof v, "auto %+d", mot::GAIN_DB[mot::gain_niveau()]);
        else snprintf(v, sizeof v, "+%d dB", mot::GAIN_DB[R.gain - 1]);
        stepper(1, SX, RY0 + RDY, v);
        ligne_rg(2, "Bip de la carte");
        static const char *const B[3] = { "Non", "Début", "Déb.+fin" };
        segments(B_RG_CHOIX0 + 2 * 4, SX - 30, RY0 + 2 * RDY + 2, 172, 22, B, 3, R.bip);
        ligne_rg(3, "Réécoute", "après le retour du son");
        snprintf(v, sizeof v, "%d s", cfg::REECOUTES[R.reecoute]); stepper(3, SX, RY0 + 3 * RDY, v, R.coupure == 1);
        { int fe = plat_fo_etat();
          ligne_rg(4, "Source du son", !R.source ? "le micro de la carte" : fe == 2 ? "optique : son reçu" : fe == 1 ? "optique : silence"
                                   : fe == 3 ? "optique : test en cours" : "optique : démarrage…"); }
        static const char *const M[2] = { "Micro", "Optique" };
        segments(B_RG_CHOIX0 + 4 * 4, SX - 10, RY0 + 4 * RDY + 2, 152, 22, M, 2, R.source);
    } else if (page_rg == 2) {
        ligne_rg(0, "Luminosité");
        snprintf(v, sizeof v, "%ld %%", (long)R.lum); stepper(0, SX, RY0, v);
        ligne_rg(1, "Écran", R.ecran ? "s'éteint après 1 min" : "toujours allumé");
        static const char *const E[2] = { "Allumé", "Discret" };
        segments(B_RG_CHOIX0 + 1 * 4, SX - 10, RY0 + RDY + 2, 152, 22, E, 2, R.ecran);
        ligne_rg(2, "LED témoin");
        static const char *const D[2] = { "Éteinte", "État" };
        segments(B_RG_CHOIX0 + 2 * 4, SX - 10, RY0 + 2 * RDY + 2, 152, 22, D, 2, R.led);
        ligne_rg(3, "Journal en mémoire", "coupé : moins de travail");
        static const char *const J[2] = { "Coupé", "Gardé" };
        segments(B_RG_CHOIX0 + 3 * 4, SX - 10, RY0 + 3 * RDY + 2, 152, 22, J, 2, R.journal);
        // effacer toute la bibliotheque (v0.6.7 : avant, dans l'onglet Pubs)
        zone_haut = 10;
        if (conf_effacer) {
            bouton2(B_PUBS_NON, 8, RY0 + 4 * RDY, 120, 22, "Non, garder", TEXTE3);
            bouton(B_PUBS_EFFACER_OUI, 134, RY0 + 4 * RDY, 178, 22, "OUI, TOUT EFFACER", nullptr, ROUGE, MAGENTA);
        } else {
            ligne_rg(4, "Pubs apprises");
            char b[24];
            snprintf(b, sizeof b, "%d", base::nb_pubs());
            texte(SX - 14, RY0 + 4 * RDY + 5, b, POLICE_13, TEXTE2, 255, DROITE);
            bouton2(B_PUBS_EFFACER, SX - 10, RY0 + 4 * RDY, 152, 22, "Tout effacer", ROUGE, base::nb_blocs() > 0);
        }
        zone_haut = 0;
    } else {
        Reseau w;
        plat_reseau(w);
        panneau(8, 58, 304, 140, 170);
        const char *etat = w.mode == 2 ? "connectée à la box" : (w.mode == 1 ? "point d'accès de la carte" : (w.mode == 3 ? "connexion à la box…" : "Wi-Fi éteint"));
        texte(18, 64, etat, POLICE_13, w.mode == 2 ? VERT : JAUNE);
        char b[64];
        snprintf(b, sizeof b, "Réseau : %s", w.ssid);
        texte(18, 84, b, POLICE_11, TEXTE2);
        if (w.mode == 1 || w.mode == 3) { snprintf(b, sizeof b, "Mot de passe : %s", w.ap_mdp); texte(18, 100, b, POLICE_11, TEXTE2); }
        texte(18, 120, "Portail (téléphone, PC) :", POLICE_11, TEXTE3);
        texte(18, 136, w.url, POLICE_13, CYAN);
        if (w.mode == 2) texte(18, 156, "ou http://detecteur-pub.local", POLICE_11, TEXTE2);
        else texte(18, 156, "Box : portail → Wi-Fi", POLICE_11, TEXTE2);
        dessiner_qr(w.url, 212, 66, 92);
        snprintf(b, sizeof b, "%s · ESP32 %s · %s", PUB_VERSION, plat_version_arduino(), plat_version_idf());
        texte(18, 176, b, POLICE_11, TEXTE3);
    }
}

// ============================ Ecran de demarrage ============================
static void dessiner_splash()
{
    float age = t - splash_t0;
    fond();
    for (int k = 0; k < 3; k++) {
        float a = age - k * 0.25f;
        if (a < 0) continue;
        float r = 14 + 130 * sortie(a / 1.4f);
        anneau(160, 88, r, 2.2f, k % 2 ? MAGENTA : CYAN, (uint8_t)(170 * (1 - sat01(a / 1.4f))));
    }
    float a = sat01((age - 0.2f) / 0.5f);
    tele(160, 82, 2.6f, rgb(255, 236, 210));
    texte(160, 120, "DÉTECTEUR DE PUB", POLICE_30, TEXTE, (uint8_t)(255 * a), CENTRE);
    texte(160, 158, "Il reconnaît les pubs et baisse le son de la télé", POLICE_11, TEXTE2, (uint8_t)(255 * a), CENTRE);
    char buf[72];
    snprintf(buf, sizeof buf, "%s · ESP32 %s · audio : %s", PUB_VERSION, plat_version_arduino(), plat_audio_etat());
    texte(160, 218, buf, POLICE_11, plat_audio_ok() ? TEXTE3 : ORANGE, (uint8_t)(200 * a), CENTRE);
    s_anime = true;
    if (age > 2.0f) { splash = false; s_sale = true; }
}

// ============================ Evenements ====================================
static void lire_evenements()
{
    det::Evenement e;
    char b[80];
    while (det::evenement(e)) {
        s_sale = true;
        switch (e.type) {
        case det::EV_TEST:
            if (e.b == 0) { snprintf(b, sizeof b, "Pub connue : n°%d (%s)", e.a, nom_pub(e.a)); message(b, VERT); }
            else if (e.b == 3) message("Rien entendu : la télé est-elle assez forte ?", ORANGE);
            else if (e.b == 5) { }                       // enregistrement lance : deja affiche (bandeau, bouton rond)
            else if (e.b == 6) { snprintf(b, sizeof b, "Pub n°%d apprise (%.0f s) ✓", e.a, e.x); message(b, VERT, "Elle sera reconnue et le son baissé au prochain passage."); }
            else if (e.b == 7) { if (e.a) snprintf(b, sizeof b, "Déjà connue : pub n°%d", e.a); else snprintf(b, sizeof b, "Rien appris : trop court ou silencieux"); message(b, e.a ? VERT : ORANGE); }
            else if (e.b == 8) { snprintf(b, sizeof b, "Plus de %.0f s sans ENREGISTRER : rien appris", e.x); message(b, ORANGE); }
            break;
        case det::EV_ANNULE:
            if (e.a == 1) message("Annulé : rien n'est appris", JAUNE);
            else { snprintf(b, sizeof b, "Pub n°%d oubliée", e.b); message(b, JAUNE); }
            break;
        case det::EV_RIEN_A_ANNULER: message("Rien à annuler", TEXTE2); break;
        case det::EV_DEJA_OUVERT: message("Un test est déjà en cours", TEXTE2); break;
        case det::EV_CORRECTION: snprintf(b, sizeof b, "Corrigé : pub n°%d oubliée à cet endroit", e.a); message(b, VERT); break;
        case det::EV_RIEN_A_CORRIGER: message("Aucune pub reconnue à corriger", TEXTE2); break;
        case det::EV_BLOC_PLEIN: message("Mémoire des pubs pleine", ROUGE); break;
        case det::EV_EFFACE: message("Bibliothèque effacée", JAUNE); break;
        case det::EV_SUPPRIME: snprintf(b, sizeof b, "Pub n°%d supprimée", e.a); message(b, JAUNE); break;
        case det::EV_IMPORTE: snprintf(b, sizeof b, "%d pubs importées", e.a); message(b, VERT); break;
        case det::EV_PURGE: snprintf(b, sizeof b, "%d pubs jamais revues supprimées", e.a); message(b, JAUNE); break;
        case det::EV_SON_FAIBLE: message("Son faible : la carte entend mal la télé", ORANGE, "Rapproche la carte des haut-parleurs."); break;
        case det::EV_SAUVE_ECHEC: message("ÉCHEC : bibliothèque non écrite sur la SD", ROUGE); break;
        case det::EV_CACHE: snprintf(b, sizeof b, "Pub n°%d copiée de la base", e.a); message(b, CYAN); break;
        default: break;
        }
    }
    // fin d'une captation (v0.6.3)
    static uint32_t cap_vu = 0;
    Captation cp;
    plat_captation(cp);
    if (cp.fins != cap_vu) {
        cap_vu = cp.fins;
        char d[12];
        fmt_duree(d, sizeof d, cp.fin_duree_s);
        if (cp.fin_ok) { snprintf(b, sizeof b, "Captation n°%lu enregistrée (%s)", (unsigned long)cp.numero, d); message(b, VERT, cp.fin_txt); }
        else { snprintf(b, sizeof b, "Captation n°%lu : %s", (unsigned long)cp.numero, cp.fin_txt); message(b, ORANGE); }
    }
    // fin du test optique (v0.6.4)
    static uint32_t fo_vu = 0;
    TestOptique fo;
    plat_fo_test(fo);
    if (fo.fins != fo_vu) {
        fo_vu = fo.fins;
        if (fo.fin_ok && fo.fin_niveau > 0.001f) message("Test optique terminé", VERT, fo.fin_txt);
        else if (fo.fin_ok) message("Test optique : silence", ORANGE, fo.fin_txt);
        else { snprintf(b, sizeof b, "Test optique : %s", fo.fin_txt); message(b, ORANGE); }
    }
    // resultats de la telecommande (volume, appairage)
    static uint32_t tv_vu = 0;
    if (T.msg_n != tv_vu) {
        tv_vu = T.msg_n;
        if (T.msg[0]) message(T.msg, T.msg_type == 1 ? VERT : (T.msg_type == 2 ? ORANGE : CYAN));
    }
}

uint32_t dernier_message(char *txt, int n, uint16_t *couleur, char *detail, int nd)
{
    if (txt && n > 0) { strncpy(txt, msg_txt, n - 1); txt[n - 1] = 0; }
    if (detail && nd > 0) { strncpy(detail, msg_det, nd - 1); detail[nd - 1] = 0; }
    if (couleur) *couleur = msg_c;
    return msg_n;
}

// ============================ Actions =======================================
static void basculer_actif()
{
    R.actif = !R.actif;
    appliquer_sauver();
    message(R.actif ? "Détecteur en marche" : "Détecteur arrêté : le son n'est plus baissé", R.actif ? VERT : GRIS);
}

static void reglage_pas(int k, int sens)
{
    int32_t *c = nullptr;
    int mn = 0, mx = 0, pas = 1;
    if (page_rg == 0) {
        if (k == 0) { c = &R.tv_bas; mx = 100; }
        else if (k == 1) { c = &R.tv_haut; mx = 100; }
        else if (k == 4) { c = &R.coupure_s; mn = 10; mx = 240; pas = 10; }
    } else if (page_rg == 1) {
        if (k == 1) { c = &R.gain; mx = mot::NB_GAINS; }
        else if (k == 3) { c = &R.reecoute; mx = 3; }
    } else if (page_rg == 2) {
        if (k == 0) { c = &R.lum; mn = 5; mx = 100; pas = 5; }
    }
    if (!c) return;
    int v = *c + sens * pas;
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    if (v == *c) return;
    *c = v;
    cfg::appliquer();                           // tout de suite ; sauve au relachement
}

static void reglage_choix(int ligne, int i)
{
    int32_t *c = nullptr;
    if (page_rg == 0) { if (ligne == 2) c = &R.tv_origine; else if (ligne == 3) c = &R.coupure; }
    else if (page_rg == 1) { if (ligne == 0) c = &R.seuil; else if (ligne == 2) c = &R.bip; else if (ligne == 4) c = &R.source; }
    else if (page_rg == 2) { if (ligne == 1) c = &R.ecran; else if (ligne == 2) c = &R.led; else if (ligne == 3) c = &R.journal; }
    if (!c || *c == i) return;
    *c = i;
    appliquer_sauver();
}

static void action(int id)
{
    if (id >= B_ONGLET0 && id < B_ONGLET0 + NB_ONGLETS) {
        int o = id - B_ONGLET0;
        if (o == onglet && o == O_TELE) tv_page = TV_ACCUEIL;   // retoucher l'onglet : retour a son accueil
        onglet = o;
        conf_effacer = false;
        return;
    }
    if (id >= B_CLAVIER0 && id < B_CLAVIER0 + 18) {
        char k = TOUCHES[id - B_CLAVIER0];
        int n = (int)strlen(saisie);
        if (k == '<') { if (n) saisie[n - 1] = 0; }
        else if (n < (tv_page == TV_CLAVIER_IP ? 15 : 6)) { saisie[n] = k; saisie[n + 1] = 0; }
        return;
    }
    if (id >= B_PAGE0 && id < B_PAGE0 + 5) { int k = id - B_PAGE0; page_rg = k == 0 ? 4 : k - 1; conf_effacer = false; return; }
    if (id >= B_PREREG0 && id < B_PREREG0 + cfg::NB_PREREG) {          // v0.9.66 : appliquer un prereglage
        int i = id - B_PREREG0;
        cfg::Prereglage p;
        if (cfg::prereg_appliquer(i) && cfg::prereg(i, p)) { char b[48]; snprintf(b, sizeof b, "Préréglage « %s » appliqué", p.nom); message(b, VERT); }
        return;
    }
    if (id >= B_RG_MOINS0 && id < B_RG_MOINS0 + 8) { reglage_pas(id - B_RG_MOINS0, -1); return; }
    if (id >= B_RG_PLUS0 && id < B_RG_PLUS0 + 8) { reglage_pas(id - B_RG_PLUS0, +1); return; }
    if (id >= B_RG_CHOIX0 && id < B_RG_CHOIX0 + 32) { int k = id - B_RG_CHOIX0; reglage_choix(k / 4, k % 4); return; }
    switch (id) {
    case B_ACTIF: basculer_actif(); break;
    case B_TESTER:
        if (V.test_etat == 4) { det::commande(det::CMD_ENREGISTRER); attendre(2); }   // enregistrement : fin de la pub
        else if (V.test_etat == 1) { det::commande(det::CMD_ANNULER); attendre(3); }  // ecoute : retoucher = arreter
        else { det::commande(det::CMD_TESTER); attendre(1); }                         // debut de la pub
        break;
    case B_ENREGISTRER: det::commande(det::CMD_ENREGISTRER); attendre(2); break;
    case B_ANNULER: det::commande(det::CMD_ANNULER); attendre(3); break;
    case B_PAS_PUB: det::commande(det::CMD_PAS_PUB); break;
    case B_SON: remettre_son(); break;
    case B_TV_IP: tv_page = TV_CLAVIER_IP; strncpy(saisie, T.ip, sizeof saisie - 1); saisie[sizeof saisie - 1] = 0; break;
    case B_TV_APPAIRER: tv::appairer(); message("Appairage : un code va s'afficher sur la télé", CYAN); break;
    case B_TV_ANNULER_APP: tv::annuler_appairage(); break;
    case B_TV_TEST: tv_page = TV_TEST; break;
    case B_TV_OUBLIER: tv_page = TV_OUBLIER; break;
    case B_TV_OUBLIER_OUI: tv::oublier(); tv_page = TV_ACCUEIL; break;
    case B_TV_RETOUR: tv_page = TV_ACCUEIL; break;
    case B_TV_COUPER: tv::couper(); break;
    case B_TV_DIMINUER: tv::diminuer(); break;
    case B_TV_AUGMENTER: tv::augmenter(); break;
    case B_TV_RETABLIR: tv::retablir(); break;
    case B_TV_LIRE: tv::lire(); break;
    case B_CL_ANNULER:
        if (tv_page == TV_CLAVIER_CODE) tv::annuler_appairage();
        tv_page = TV_ACCUEIL;
        break;
    case B_CL_OK:
        if (tv_page == TV_CLAVIER_IP) {
            if (cfg::regler_texte("tv_ip", saisie)) { char b[48]; snprintf(b, sizeof b, "IP de la télé : %s", saisie); message(b, VERT); tv_page = TV_ACCUEIL; }
            else message("Adresse IP invalide (ex. 192.168.1.55)", ORANGE);
        } else {
            tv::code(saisie);
            message("Code envoyé : vérification…", CYAN);
        }
        break;
    case B_PUBS_EFFACER: conf_effacer = true; message("Efface toutes les pubs et leurs sons ?", ROUGE); break;
    case B_PUBS_NON: conf_effacer = false; break;
    case B_CAPTER: {
        Captation cp;
        plat_captation(cp);
        if (cp.etat == 2) { plat_captation_arreter(); message("Arrêt de la captation…", JAUNE); }
        else if (cp.etat) { }                                // demarrage ou arret deja en cours
        else {
            const char *err = plat_captation_demarrer();
            char b[80];
            if (err) { snprintf(b, sizeof b, "Captation impossible : %s", err); message(b, ORANGE); }
            else message("Captation lancée (15 min max)", CYAN, "Le son brut du micro va sur la SD ; la détection continue.");
        }
        break;
    }
    case B_CAP_AUTO: {
        Captation cp;
        plat_captation(cp);
        if (!cp.etat) {
            const char *err = plat_captation_demarrer_auto();
            char b[80];
            if (err) { snprintf(b, sizeof b, "Captation impossible : %s", err); message(b, ORANGE); }
            else message("Captation AUTO lancée (15 min max)", CYAN, "Le bandeau entier est gardé ; les pubs inconnues sont découpées.");
        }
        break;
    }
    case B_CAP_APPR: {
        Captation cp;
        plat_captation(cp);
        if (!cp.etat) {
            const char *err = plat_captation_demarrer_apprendre();
            char b[80];
            if (err) { snprintf(b, sizeof b, "Captation impossible : %s", err); message(b, ORANGE); }
            else message("APPRENDRE lancé (15 min max)", JAUNE, "Les pubs inconnues vont dans la bibliothèque, à vérifier.");
        }
        break;
    }
    case B_CAP_ANNULER: {
        Captation cp;
        plat_captation(cp);
        conf_cap = true;
        message(cp.apprendre ? "Annuler ? Les pubs apprises restent" : "Supprimer cette captation ?", ROUGE);
        break;
    }
    case B_CAP_NON: conf_cap = false; break;
    case B_CAP_CH_MOINS: { int c = plat_captation_chaine(); plat_captation_chaine_regler((c + base::NB_CHAINES - 1) % base::NB_CHAINES); break; }
    case B_CAP_CH_PLUS: { int c = plat_captation_chaine(); plat_captation_chaine_regler((c + 1) % base::NB_CHAINES); break; }
    case B_CAP_OUI: conf_cap = false; plat_captation_annuler(); message("Captation annulée…", JAUNE); break;
    case B_TEST_FO: {
        TestOptique fo;
        plat_fo_test(fo);
        if (fo.etat) break;
        const char *err = plat_fo_test_demarrer();
        char b[80];
        if (err) { snprintf(b, sizeof b, "Test optique impossible : %s", err); message(b, ORANGE); }
        else message("Test optique : 10 s à l'écoute", CYAN, "Mets un son sur la télé. Un WAV ira sur la SD.");
        break;
    }
    case B_PUBS_EFFACER_OUI: conf_effacer = false; det::commande(det::CMD_EFFACER); break;
    case B_JR_ONOFF: R.journal = !R.journal; appliquer_sauver(); jr_fige = 0; break;
    case B_JR_HAUT: {
        uint32_t der = plat_journal_dernier();
        uint32_t base_l = jr_fige ? jr_fige : der;
        uint32_t plus_vieux = der > (uint32_t)JOURNAL_NB ? der - JOURNAL_NB + 1 : 1;
        jr_fige = base_l > plus_vieux + 5 ? base_l - 5 : plus_vieux;
        break;
    }
    case B_JR_BAS: {
        uint32_t der = plat_journal_dernier();
        if (jr_fige) jr_fige = jr_fige + 5 >= der ? 0 : jr_fige + 5;
        break;
    }
    default: break;
    }
}

// ============================ Toucher =======================================
static Zone s_zone_touchee = {0, 0, 0, 0, 0, false};
__attribute__((weak)) void plat_appui_zone(int, int, int, int) {}   // NB : dans ui -> la version de la carte n'est pas appelee (retour au toucher inactif, laisse tel quel)
static int chercher_zone(int x, int y, bool *repete)
{
    // la derniere zone dessinee est au-dessus : on cherche a l'envers
    for (int i = nzones - 1; i >= 0; i--) {
        const Zone &z = zones[i];
        if (z.l > 0 && dans(x, y, z.x, z.y, z.l, z.h)) { if (repete) *repete = z.repete; s_zone_touchee = z; return z.id; }
    }
    return B_AUCUN;
}

void toucher(bool appui, int x, int y)
{
    uint32_t ms = plat_ms();
    if (splash) {
        if (appui && t - splash_t0 > 0.3f) { splash = false; s_sale = true; }
        tc_appui = false;
        return;
    }
    if (appui) {
        tc_n++;
        if (tc_n == 1 && !tc_fait) {
            // des la premiere lecture du doigt (v0.6.2 : avant, il en fallait deux, soit
            // une image entiere de retard pendant les animations). On agit TOUT DE
            // SUITE, la couleur change dans la meme image.
            tc_id = chercher_zone(x, y, &tc_repete);
            if (tc_id != B_AUCUN) plat_appui_zone(s_zone_touchee.x, s_zone_touchee.y, s_zone_touchee.l, s_zone_touchee.h);   // v0.9.6 : avant l'image complete
            tc_appui = true;
            tc_t0 = ms;
            tc_rep = ms;
            tc_fait = true;
            if (tc_id != B_AUCUN) action(tc_id);
            s_sale = true;
        } else if (tc_fait && tc_repete && tc_id != B_AUCUN && ms - tc_t0 > 450 && ms - tc_rep > 110) {
            // maintenir + ou - : la valeur defile
            tc_rep = ms;
            if (chercher_zone(x, y, nullptr) == tc_id) { action(tc_id); s_sale = true; }
        }
    } else {
        if (tc_appui) {
            if (tc_repete) cfg::sauver();              // reglage change en maintenant : sauve une fois
            s_sale = true;
        }
        tc_n = 0;
        tc_appui = false;
        tc_fait = false;
        tc_id = B_AUCUN;
        tc_repete = false;
    }
}

// ============================ Boucle ========================================
void init(uint16_t *fond_buf)
{
    s_fond = fond_buf;
    if (s_fond) {
        uint16_t *cur = gfx::fb;
        gfx::fb = s_fond;
        dessiner_fond_complet();
        gfx::fb = cur;
    }
    cfg::charger();
}

static uint32_t s_det_vu = 0, s_cpr_vu = 0;
static int s_etat_vu = -1, s_test_vu = -1, s_tv_vu = -1, s_tv_act_vu = -1, s_vol_vu = -1;

bool a_dessiner(uint32_t ms)
{
    // ce qui change l'affichage : etat du detecteur, du test, de la coupure, de la tele
    det::Vue v;
    det::vue(v);
    tv::Infos ti;
    tv::infos(ti);
    if (v.etat != s_etat_vu || v.test_etat != s_test_vu || ti.etat != s_tv_vu || ti.action != s_tv_act_vu || ti.volume != s_vol_vu ||
        ti.msg_n != s_det_vu || (uint32_t)cpr::etat() != s_cpr_vu) {
        s_etat_vu = v.etat; s_test_vu = v.test_etat; s_tv_vu = ti.etat; s_tv_act_vu = ti.action; s_vol_vu = ti.volume;
        s_det_vu = ti.msg_n; s_cpr_vu = (uint32_t)cpr::etat();
        s_sale = true;
    }
    uint32_t age = ms - s_der_image;
    if (s_sale) return true;
    if (s_anime && age >= 40) return true;            // animation : 25 images/s
    return age >= 125;                                // sinon 8 images/s (vumetre, compte a rebours)
}

// captation lancee depuis le portail / le smartphone : l'ecran bascule sur l'onglet Capter.
// (v0.8.5 : appelee par erreur seulement quand l'ecran dort ; v0.8.7 : aussi a chaque image)
static volatile int s_vers_onglet = -1;
void aller_onglet(const char *nom)
{
    static const char *const N[] = { "direct", "tele", "capter", "journal", "reglages" };   // ordre de l'enum des onglets
    for (int i = 0; i < NB_ONGLETS; i++) if (!strcmp(nom, N[i])) s_vers_onglet = i;
}

static void bascule_capter()
{
    if (s_vers_onglet >= 0) {
        int o = s_vers_onglet; s_vers_onglet = -1;
        if (onglet != o) { onglet = o; conf_effacer = false; s_sale = true; }
    }
    static int capt_prec = 0;
    Captation cp;
    plat_captation(cp);
    if (cp.etat && !capt_prec && onglet != O_CAPTER) { onglet = O_CAPTER; conf_effacer = false; s_sale = true; }
    capt_prec = cp.etat;
}

void suivre(uint32_t ms)
{
    (void)ms;
    det::vue(V);
    tv::infos(T);
    lire_evenements();
    fin_attente();
    bascule_capter();
}

void image(uint32_t ms)
{
    if (ms_prec == 0) ms_prec = ms;
    dt = (ms - ms_prec) / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    if (dt < 0.001f) dt = 0.001f;
    ms_prec = ms;
    t += dt;
    s_der_image = ms;
    s_sale = false;
    s_anime = false;
    nzones = 0;
    if (mot::source == mot::SRC_MICRO && plat_audio_ok() && mot::niveau_db() < mot::NIVEAU_FAIBLE) faible_t += dt;
    else faible_t = 0;
    son_faible = faible_t > 2.0f;
    if (splash_t0 == 0 && splash) splash_t0 = t;

    det::vue(V);
    tv::infos(T);
    lire_evenements();
    fin_attente();
    bascule_capter();
    if (splash) { dessiner_splash(); return; }

    fond();
    switch (onglet) {
    case O_ACCUEIL: ecran_accueil(); break;
    case O_TELE: ecran_tele(); break;
    case O_CAPTER: ecran_capter(); break;
    case O_JOURNAL: ecran_journal(); break;
    case O_REGLAGES: ecran_reglages(); break;
    }
    barre_haut();
    barre_onglets();
}

}  // namespace ui
