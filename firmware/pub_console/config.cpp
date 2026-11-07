// config.cpp - reglages partages
#include "config.h"
#include "coupure.h"
#include "detecteur.h"
#include "moteur.h"
#include "tele.h"
#include "plateforme.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// journal en memoire (journal.cpp sur la carte ; rien sur le PC)
__attribute__((weak)) void journal_activer(bool) {}

// v0.9.61 : definition par defaut HORS du namespace cfg. Placee dans cfg, elle creait une
// fonction cfg::plat_source() vide, appelee a la place de la vraie (pub_console.ino) : le choix
// << entree optique >> n'a jamais demarre l'optique (depuis la v0.8.0).
__attribute__((weak)) void plat_source(int) {}

namespace cfg {

static const uint32_t MAGIE = 0x50554260;     // v0.6.0 (telecommande Wi-Fi)
Reglages R = { MAGIE,
               1, 0, mot::SRC_MICRO, 1, 0, 2,          // detection, base : les deux
               1, 30, 2,                               // coupure par fenetres de 30 s, reecoute 8 s
               3, 20, 1,                               // tele : bas 3, haut 20, retour au volume d'origine
               1, 80, 0, 1, 1,                         // carte : bip au debut, lum 80 %, ecran allume, LED, journal
               "", "" };
volatile uint32_t version = 1;
const int SEUILS[3] = { 8, 10, 14 };
const int REECOUTES[4] = { 3, 5, 8, 12 };

static const Champ CHAMPS[] = {
    { "seuil", &Reglages::seuil, 0, 2 },     { "gain", &Reglages::gain, 0, mot::NB_GAINS },
    { "source", &Reglages::source, 0, 1 },   { "actif", &Reglages::actif, 0, 1 },
    { "chaine", &Reglages::chaine, 0, 47 },  { "base", &Reglages::base, 0, 2 },
    { "coupure", &Reglages::coupure, 0, 1 }, { "coupure_s", &Reglages::coupure_s, 10, 240 },
    { "reecoute", &Reglages::reecoute, 0, 3 },
    { "tv_bas", &Reglages::tv_bas, 0, 100 }, { "tv_haut", &Reglages::tv_haut, 0, 100 },
    { "tv_origine", &Reglages::tv_origine, 0, 1 },
    { "bip", &Reglages::bip, 0, 2 },         { "lum", &Reglages::lum, 5, 100 },
    { "ecran", &Reglages::ecran, 0, 1 },     { "led", &Reglages::led, 0, 1 },
    { "journal", &Reglages::journal, 0, 1 },
};
int nb_champs() { return (int)(sizeof CHAMPS / sizeof CHAMPS[0]); }
const Champ &champ(int i) { return CHAMPS[i]; }

void appliquer()
{
    det::seuil_votes = SEUILS[R.seuil];
    mot::regler_gain(R.gain == 0 ? -1 : R.gain - 1);
    ::plat_source(R.source);                    // micro / optique (v0.8.0 ; v0.9.61 : la vraie fonction)
    mot::source = R.source;
    det::jingle_hp = R.bip;
    det::actif = R.actif != 0;
    det::chaine_reglee = R.chaine;
    det::mode_base = BASE_DISTANTE ? R.base : 0;   // v0.9.53 : base distante desactivee -> carte seule
    cpr::mode = R.coupure;
    cpr::duree_s = R.coupure_s;
    cpr::reecoute_s = REECOUTES[R.reecoute];
    { uint8_t fs = 1; if (!plat_nvs_lire("finspot", &fs, sizeof fs)) fs = 1; cpr::fin_spot = fs != 0; }   // v0.9.19 : 1 = automatique (fin du spot), 0 = duree imposee
    tv::niveau_bas = R.tv_bas;
    tv::niveau_haut = R.tv_haut;
    tv::origine = R.tv_origine != 0;
    journal_activer(R.journal != 0);
    plat_luminosite(R.lum);
    version = version + 1;
}

void sauver() { plat_nvs_ecrire("reglages6", &R, sizeof R); }

bool ip_valide(const char *s)
{
    if (!s || !*s) return true;                        // vide = a regler
    int n[4], k = 0;
    char fin = 0;
    k = sscanf(s, "%d.%d.%d.%d%c", &n[0], &n[1], &n[2], &n[3], &fin);
    if (k != 4) return false;
    for (int i = 0; i < 4; i++) if (n[i] < 0 || n[i] > 255) return false;
    return strlen(s) < sizeof R.tv_ip;
}

void ip_tele(char *out, int n)
{
    plat_verrou();
    strncpy(out, R.tv_ip, n - 1);
    out[n - 1] = 0;
    plat_deverrou();
}

static bool valide(const Reglages &r)
{
    for (int i = 0; i < nb_champs(); i++) {
        int v = r.*(CHAMPS[i].champ);
        if (v < CHAMPS[i].min || v > CHAMPS[i].max) return false;
    }
    return memchr(r.serveur, 0, sizeof r.serveur) != nullptr && memchr(r.tv_ip, 0, sizeof r.tv_ip) != nullptr && ip_valide(r.tv_ip);
}

void charger()
{
    Reglages lu;
    if (plat_nvs_lire("reglages6", &lu, sizeof lu) && lu.magie == MAGIE && valide(lu)) R = lu;
    appliquer();
    prereg_init();
}

// ---- v0.9.66 : preregages ----
static const uint32_t MAGIE_PR = 0x50524731;  // "PRG1"
struct PrBloc { uint32_t magie; Prereglage p[NB_PREREG]; };
static PrBloc s_pr;
static bool s_pr_ok = false;

static void pr_depuis_reglages(Prereglage &p, const char *nom)
{
    memset(&p, 0, sizeof p);
    strncpy(p.nom, nom, sizeof p.nom - 1);
    p.source = R.source; p.coupure = R.coupure; p.tv_bas = R.tv_bas; p.coupure_s = R.coupure_s;
    p.tv_origine = R.tv_origine; p.seuil = R.seuil; p.gain = R.gain; p.reecoute = R.reecoute;
}
static void pr_defaut(Prereglage &p, const char *nom, int src, int cs, int bas)
{
    memset(&p, 0, sizeof p);
    strncpy(p.nom, nom, sizeof p.nom - 1);
    p.source = src; p.coupure = 1; p.tv_bas = bas; p.coupure_s = cs;
    p.tv_origine = 1; p.seuil = 1; p.gain = 0; p.reecoute = 2;
}
static bool pr_valide(const Prereglage &p)
{
    return memchr(p.nom, 0, sizeof p.nom) && p.source >= 0 && p.source <= 1 && p.coupure >= 0 && p.coupure <= 1 &&
           p.tv_bas >= 0 && p.tv_bas <= 100 && p.coupure_s >= 10 && p.coupure_s <= 240 && p.tv_origine >= 0 && p.tv_origine <= 1 &&
           p.seuil >= 0 && p.seuil <= 2 && p.gain >= 0 && p.gain <= mot::NB_GAINS && p.reecoute >= 0 && p.reecoute <= 3;
}
static void pr_sauver() { s_pr.magie = MAGIE_PR; plat_nvs_ecrire("prereg1", &s_pr, sizeof s_pr); }

void prereg_origine()
{
    pr_defaut(s_pr.p[0], "Optique confort", 1, 45, 1);
    pr_defaut(s_pr.p[1], "Optique rapide", 1, 20, 1);
    pr_defaut(s_pr.p[2], "Micro", 0, 30, 3);
    pr_depuis_reglages(s_pr.p[3], "Mon réglage");
    s_pr_ok = true;
    pr_sauver();
}

void prereg_init()
{
    PrBloc lu;
    bool ok = plat_nvs_lire("prereg1", &lu, sizeof lu) && lu.magie == MAGIE_PR;
    for (int i = 0; ok && i < NB_PREREG; i++) ok = pr_valide(lu.p[i]);
    if (ok) { s_pr = lu; s_pr_ok = true; }
    else prereg_origine();
}

bool prereg(int i, Prereglage &out)
{
    if (!s_pr_ok || i < 0 || i >= NB_PREREG) return false;
    out = s_pr.p[i];
    return true;
}

bool prereg_appliquer(int i)
{
    if (!s_pr_ok || i < 0 || i >= NB_PREREG) return false;
    const Prereglage &p = s_pr.p[i];
    R.source = p.source; R.coupure = p.coupure; R.tv_bas = p.tv_bas; R.coupure_s = p.coupure_s;
    R.tv_origine = p.tv_origine; R.seuil = p.seuil; R.gain = p.gain; R.reecoute = p.reecoute;
    appliquer();
    sauver();
    version = version + 1;
    return true;
}

bool prereg_enregistrer(int i, const char *nom)
{
    if (!s_pr_ok || i < 0 || i >= NB_PREREG) return false;
    char n[sizeof s_pr.p[0].nom];
    int k = 0;
    if (nom) for (const char *c = nom; *c && k < (int)sizeof n - 1; c++) {
        unsigned char u = (unsigned char)*c;
        if (u < 32 || u == '"' || u == '\\' || u == '<' || u == '>') continue;   // rien qui casse le JSON ou la page
        n[k++] = (char)u;
    }
    while (k > 0 && ((unsigned char)n[k - 1] & 0xC0) == 0x80) k--;   // pas de caractere UTF-8 coupe en deux
    if (k > 0 && ((unsigned char)n[k - 1] & 0xC0) == 0xC0) k--;
    n[k] = 0;
    pr_depuis_reglages(s_pr.p[i], k ? n : s_pr.p[i].nom);
    pr_sauver();
    return true;
}

int prereg_actif()
{
    if (!s_pr_ok) return -1;
    for (int i = 0; i < NB_PREREG; i++) {
        const Prereglage &p = s_pr.p[i];
        if (R.source == p.source && R.coupure == p.coupure && R.tv_bas == p.tv_bas && R.coupure_s == p.coupure_s &&
            R.tv_origine == p.tv_origine && R.seuil == p.seuil && R.gain == p.gain && R.reecoute == p.reecoute) return i;
    }
    return -1;
}

bool regler(const char *cle, int v)
{
    for (int i = 0; i < nb_champs(); i++) {
        if (strcmp(cle, CHAMPS[i].cle) != 0) continue;
        if (v < CHAMPS[i].min || v > CHAMPS[i].max) return false;
        R.*(CHAMPS[i].champ) = v;
        appliquer();
        sauver();
        return true;
    }
    return false;
}

static bool serveur_valide(const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof R.serveur) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '_')) return false;
    }
    return true;
}

bool regler_texte(const char *cle, const char *valeur)
{
    if (strcmp(cle, "serveur") == 0) {
        if (!serveur_valide(valeur)) return false;
        strncpy(R.serveur, valeur, sizeof R.serveur - 1);
        R.serveur[sizeof R.serveur - 1] = 0;
    } else if (strcmp(cle, "tv_ip") == 0) {
        if (!ip_valide(valeur)) return false;
        plat_verrou();
        strncpy(R.tv_ip, valeur, sizeof R.tv_ip - 1);
        R.tv_ip[sizeof R.tv_ip - 1] = 0;
        plat_deverrou();
        tv::reconnecter();
    } else return false;
    appliquer();
    sauver();
    return true;
}

int en_json(char *out, int max)
{
    int k = snprintf(out, max, "{");
    for (int i = 0; i < nb_champs() && k < max; i++)
        k += snprintf(out + k, max - k, "\"%s\":%ld,", CHAMPS[i].cle, (long)(R.*(CHAMPS[i].champ)));
    if (k < max) k += snprintf(out + k, max - k, "\"tv_ip\":\"%s\",\"serveur\":\"%s\"}", R.tv_ip, R.serveur);
    return k;
}

// lecteur JSON minimal : {"cle":nombre, "serveur":"texte"} (ce que la page envoie)
static const char *valeur_de(const char *json, const char *cle)
{
    char motif[24];
    snprintf(motif, sizeof motif, "\"%s\"", cle);
    const char *p = strstr(json, motif);
    if (!p) return nullptr;
    p += strlen(motif);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return nullptr;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int texte_de(const char *json, const char *cle, char *s, int n)
{
    const char *p = valeur_de(json, cle);
    if (!p || *p != '"') return 0;
    int k = 0;
    for (p++; *p && *p != '"'; p++) { if (k >= n - 1) return -1; s[k++] = *p; }
    s[k] = 0;
    return 1;
}

int depuis_json(const char *json)
{
    Reglages n = R;
    int changes = 0;
    for (int i = 0; i < nb_champs(); i++) {
        const char *p = valeur_de(json, CHAMPS[i].cle);
        if (!p) continue;
        char *fin;
        long v = strtol(p, &fin, 10);
        if (fin == p) return -1;
        if (v < CHAMPS[i].min || v > CHAMPS[i].max) return -1;
        if (n.*(CHAMPS[i].champ) != v) { n.*(CHAMPS[i].champ) = (int32_t)v; changes++; }
    }
    char s[sizeof R.serveur];
    int r = texte_de(json, "serveur", s, sizeof s);
    if (r < 0 || (r && !serveur_valide(s))) return -1;
    if (r && strcmp(s, n.serveur) != 0) { strcpy(n.serveur, s); changes++; }
    bool ip_change = false;
    r = texte_de(json, "tv_ip", s, sizeof n.tv_ip);
    if (r < 0 || (r && !ip_valide(s))) return -1;
    if (r && strcmp(s, n.tv_ip) != 0) { strcpy(n.tv_ip, s); changes++; ip_change = true; }
    if (changes) {
        plat_verrou();
        R = n;
        plat_deverrou();
        appliquer();
        sauver();
        if (ip_change) tv::reconnecter();
    }
    return changes;
}

}  // namespace cfg
