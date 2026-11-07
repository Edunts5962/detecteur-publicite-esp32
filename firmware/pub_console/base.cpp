// base.cpp - bibliotheque triee, votes par decalage, journal, repetitions
#include "base.h"
#include "plateforme.h"
#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
// liberation d'un bloc obtenu par plat_malloc_grand (heap_caps_malloc ou malloc : free convient aux deux)
static inline void heap_libere(void *p) { free(p); }

// journal : version vide si la plateforme n'en fournit pas (simulateur, bancs)
__attribute__((weak)) void plat_log(const char *, ...) {}

namespace base {

static const char *FICHIER = "/pub/base.bin";
static const char *FICHIER_NEUF = "/pub/base.new";
static const uint32_t MAGIE = 0x31425550;      // "PUB1"

// une seule zone en PSRAM, ecrite et relue d'un bloc sur la carte SD
struct Entete { uint32_t magie, nb_entrees, max_numero, reserve; };
struct Zone { Entete e; Bloc blocs[MAX_BLOCS]; Entree entrees[CAP]; };
static Zone *z = nullptr;
static Info *infos = nullptr;                  // MAX_BLOCS fiches (PSRAM)
static const uint32_t MAGIE_INFOS = 0x32464E49; // "INF2"
static const uint32_t MAGIE_INFOS1 = 0x31464E49; // "INF1" (v0.4 : 32 octets par fiche)
struct Info1 { char nom[28]; uint8_t chaine; uint8_t audio; uint16_t reserve; };

const char *const CHAINES[NB_CHAINES] = {
    "?",
    // TNT nationale gratuite (numerotation de juin 2025)
    "TF1", "France 2", "France 3", "France 4", "France 5", "M6", "Arte", "LCP", "W9", "TMC",
    "TFX", "Gulli", "BFMTV", "CNews", "LCI", "franceinfo", "CStar", "T18", "NOVO19", "TF1 Séries Films",
    "L'Équipe", "6ter", "RMC Story", "RMC Découverte", "RMC Life", "Paris Première",
    // cable, satellite, box, Molotov
    "Canal+", "BFM Business", "France 24", "Tech & Co", "RMC Sport", "Eurosport 1", "beIN Sports 1",
    "Téva", "Série Club", "13ème Rue", "Comédie+", "Canal J", "TiJi", "M6 Music", "RMC Mécanic",
    "Sport en France", "Figaro Live", "Brut", "Molotov", "Chaîne locale", "Autre" };
const char *const CHAINES_DOSSIER[NB_CHAINES] = {
    "inconnue",
    "tf1", "france2", "france3", "france4", "france5", "m6", "arte", "lcp", "w9", "tmc",
    "tfx", "gulli", "bfmtv", "cnews", "lci", "franceinfo", "cstar", "t18", "novo19", "tf1seriesfilms",
    "lequipe", "6ter", "rmcstory", "rmcdecouverte", "rmclife", "parispremiere",
    "canalplus", "bfmbusiness", "france24", "techandco", "rmcsport", "eurosport1", "beinsports1",
    "teva", "serieclub", "13emerue", "comedieplus", "canalj", "tiji", "m6music", "rmcmecanic",
    "sportenfrance", "figarolive", "brut", "molotov", "locale", "autre" };
const uint8_t CHAINES_TNT[NB_CHAINES] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static emp::Code *journal = nullptr;           // anneau
static uint32_t j_n = 0;

// table des votes : (bloc, decalage / 2) -> nombre de votes
struct Vote { uint32_t cle; uint16_t n, nf; uint16_t bloc; uint16_t r; int32_t dec; uint32_t der, pre; };
static const int NV = 2048;
static Vote *votes = nullptr;          // NV cases, en PSRAM (la memoire interne va au Wi-Fi)
static uint16_t *v_utilises = nullptr;
static uint32_t s_version = 1;
static const int NCOUV = 160;                  // tranches de 2 s (blocs de 5 min au plus)
static uint8_t (*couv)[NCOUV] = nullptr;       // [MAX_BLOCS][NCOUV] diffusions reconnues de chaque tranche (PSRAM)
uint32_t version() { return s_version; }
static inline void modifie() { s_version++; }

// les bits 24..27 de h comptent les confirmations (consolidation) : la
// comparaison ne regarde que le code (22 bits)
static const uint32_t CONF_UN = 1u << 24, CONF_MAX = 15u << 24, CONF_BITS = 15u << 24;
static inline uint32_t H(uint32_t h) { return h & emp::MASQUE; }
static bool par_code(const Entree &a, const Entree &b)
{
    if (H(a.h) != H(b.h)) return H(a.h) < H(b.h);
    if (a.bloc != b.bloc) return a.bloc < b.bloc;
    return a.t < b.t;
}

// ajoute des entrees deja rangees en fin de tableau : tri de la fin seule puis
// fusion (O(n)) au lieu de retrier les 400 000 entrees
static void fusionner(uint32_t ancien_n)
{
    Entree *deb = z->entrees, *mil = z->entrees + ancien_n, *fin = z->entrees + z->e.nb_entrees;
    std::sort(mil, fin, par_code);
    if (ancien_n) std::inplace_merge(deb, mil, fin, par_code);
}

bool init()
{
    if (z) return true;
    z = (Zone *)plat_malloc_grand(sizeof(Zone));
    journal = (emp::Code *)plat_malloc_grand(JOURNAL * sizeof(emp::Code));
    infos = (Info *)plat_malloc_grand(MAX_BLOCS * sizeof(Info));
    votes = (Vote *)plat_malloc_grand(NV * sizeof(Vote));
    v_utilises = (uint16_t *)plat_malloc_grand(NV * sizeof(uint16_t));
    couv = (uint8_t(*)[NCOUV])plat_malloc_grand(MAX_BLOCS * NCOUV);
    if (couv) memset(couv, 0, MAX_BLOCS * NCOUV);
    if (votes) memset(votes, 0, NV * sizeof(Vote));
    if (!z || !journal || !infos || !votes || !v_utilises) return false;
    memset(infos, 0, MAX_BLOCS * sizeof(Info));
    memset(&z->e, 0, sizeof z->e);
    memset(z->blocs, 0, sizeof z->blocs);
    z->e.magie = MAGIE;
    return true;
}

bool prete() { return z != nullptr; }
int nb_entrees() { return z ? (int)z->e.nb_entrees : 0; }
int max_numero() { return z ? (int)z->e.max_numero : 0; }
float remplissage() { return z ? (float)z->e.nb_entrees / CAP : 0; }

int nb_blocs()
{
    if (!z) return 0;
    int n = 0;
    for (int i = 0; i < MAX_BLOCS; i++) n += z->blocs[i].actif;
    return n;
}

int nb_pubs()
{
    if (!z) return 0;
    int n = 0;
    for (int i = 0; i < MAX_BLOCS; i++) n += z->blocs[i].actif && z->blocs[i].type == PUB;
    return n;
}

const Bloc &bloc(int id)
{
    static Bloc vide = {};
    if (!z || id < 0 || id >= MAX_BLOCS) return vide;
    return z->blocs[id];
}

int ajouter_bloc(const emp::Code *c, int n, uint32_t t0, int duree_s, int type)
{
    if (!z || n <= 0) return -1;
    if (z->e.nb_entrees + (uint32_t)n > (uint32_t)CAP) return -1;
    int id = -1;
    for (int i = 0; i < MAX_BLOCS; i++) if (!z->blocs[i].actif) { id = i; break; }
    if (id < 0) return -1;
    Entree *e = z->entrees + z->e.nb_entrees;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].h & emp::REQUETE) continue;       // codes de recherche : jamais stockes
        if (c[i].t < t0) continue;
        uint32_t t = c[i].t - t0;
        if (t > 65535) continue;
        e[k].h = H(c[i].h);
        e[k].bloc = (uint16_t)id;
        e[k].t = (uint16_t)t;
        k++;
    }
    if (k == 0) return -1;
    uint32_t ancien = z->e.nb_entrees;
    z->e.nb_entrees += k;
    fusionner(ancien);
    modifie();
    Bloc &b = z->blocs[id];
    if (couv) memset(couv[id], 0, NCOUV);
    b.actif = 1;
    b.type = (uint8_t)type;
    b.duree_s = (uint16_t)duree_s;
    b.nb = k;
    b.vus = 0;
    b.numero = ++z->e.max_numero;
    if (infos) {
        memset(&infos[id], 0, sizeof(Info));
        snprintf(infos[id].nom, sizeof infos[id].nom, type == JINGLE_DEBUT ? "Jingle de début" :
                 (type == JINGLE_FIN ? "Jingle de fin" : (type == JINGLE_DEBUT_FIN ? "Jingle début et fin" : "Pub n°%lu")),
                 (unsigned long)b.numero);
        infos[id].appris_le = plat_heure();
    }
    return id;
}

// retire les entrees qui verifient "rejet" en gardant l'ordre (donc le tri)
template <typename F> static int compacter(F rejet)
{
    uint32_t w = 0, n = z->e.nb_entrees;
    for (uint32_t r = 0; r < n; r++) {
        if (rejet(z->entrees[r])) {
            Bloc &b = z->blocs[z->entrees[r].bloc];
            if (b.nb) b.nb--;
            continue;
        }
        if (w != r) z->entrees[w] = z->entrees[r];
        w++;
    }
    z->e.nb_entrees = w;
    modifie();
    return (int)(n - w);
}

bool supprimer_bloc(int id, const char *raison)
{
    if (!z || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif) return false;
    uint32_t num = z->blocs[id].numero;
    plat_log("[PUB] %s n°%lu %s\n", z->blocs[id].type == PUB ? "Pub" : "Jingle", (unsigned long)num, raison ? raison : "supprimée");
    plat_audio_supprimer(num);                        // son du bloc : jamais orphelin
    compacter([id](const Entree &e) { return e.bloc == id; });
    memset(&z->blocs[id], 0, sizeof(Bloc));
    if (infos) memset(&infos[id], 0, sizeof(Info));
    if (couv) memset(couv[id], 0, NCOUV);
    return true;
}

int supprimer_plage(int id, int t0, int t1)
{
    if (!z || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif) return 0;
    if (t0 < 0) t0 = 0;
    int n = compacter([=](const Entree &e) { return e.bloc == id && e.t >= t0 && e.t <= t1; });
    if (z->blocs[id].nb == 0) supprimer_bloc(id, "supprimée : la correction n'en a rien laissé");
    return n;
}

void compter_vu(int id)
{
    if (z && id >= 0 && id < MAX_BLOCS && z->blocs[id].actif) {
        z->blocs[id].vus++;
        if (infos) infos[id].vu_le = plat_heure();
    }
}

void effacer()
{
    if (!z) return;
    if (infos) memset(infos, 0, MAX_BLOCS * sizeof(Info));
    z->e.nb_entrees = 0;
    z->e.max_numero = 0;
    memset(z->blocs, 0, sizeof z->blocs);
    if (couv) memset(couv, 0, MAX_BLOCS * NCOUV);
    modifie();
}

int pubs_du_fichier(const char *chemin)
{
    long taille = plat_fichier_taille(chemin);
    size_t fixe = sizeof(Entete) + sizeof(Bloc) * MAX_BLOCS;
    if (taille < (long)fixe) return -1;
    uint8_t *tmp = (uint8_t *)malloc(fixe);
    if (!tmp) return -1;
    int n = -1;
    if (plat_fichier_lire(chemin, tmp, fixe) == (long)fixe) {
        Entete e;
        memcpy(&e, tmp, sizeof e);
        if (e.magie == MAGIE && e.nb_entrees <= (uint32_t)CAP && (size_t)taille == fixe + (size_t)e.nb_entrees * sizeof(Entree)) {
            n = 0;
            const Bloc *b = (const Bloc *)(tmp + sizeof(Entete));
            for (int i = 0; i < MAX_BLOCS; i++) n += b[i].actif && b[i].type == PUB;
        }
    }
    free(tmp);
    return n;
}

bool sauver()
{
    if (!z || !plat_sd()) return false;
    size_t n = sizeof(Entete) + sizeof(z->blocs) + (size_t)z->e.nb_entrees * sizeof(Entree);
    // ecriture sure : une coupure de courant pendant l'ecriture (tele eteinte, USB
    // coupe) ne doit pas perdre la bibliotheque. On ecrit a cote, puis on remplace.
    bool ok = plat_fichier_ecrire(FICHIER_NEUF, z, n) && plat_fichier_taille(FICHIER_NEUF) == (long)n &&
              plat_fichier_renommer(FICHIER_NEUF, FICHIER);
    if (infos) {
        bool ok2 = plat_fichier_ecrire("/pub/infos.new", &MAGIE_INFOS, 4) &&
                   plat_fichier_ajouter("/pub/infos.new", infos, MAX_BLOCS * sizeof(Info));
        if (ok2) plat_fichier_renommer("/pub/infos.new", "/pub/infos.bin");
    }
    return ok;
}

bool charger()
{
    if (!z || !plat_sd()) return false;
    size_t fixe = sizeof(Entete) + sizeof(z->blocs);
    auto valide = [&](long n) {
        return n >= (long)fixe && z->e.magie == MAGIE && z->e.nb_entrees <= (uint32_t)CAP &&
               (size_t)n == fixe + (size_t)z->e.nb_entrees * sizeof(Entree);
    };
    // base.new complet = la sauvegarde la plus recente (avant v0.5.31, il n'etait
    // jamais renomme : base.bin restait ancien). On le reprend s'il est entier.
    bool neuf = false;
    long n = -1;
    if (plat_fichier_taille(FICHIER_NEUF) > 0) {
        n = plat_fichier_lire(FICHIER_NEUF, z, sizeof(Zone));
        neuf = valide(n);
        if (neuf) {
            plat_log("[PUB] Bibliotheque : reprise de la derniere sauvegarde (base.new, %lu empreintes)\n", (unsigned long)z->e.nb_entrees);
            plat_fichier_renommer(FICHIER_NEUF, FICHIER);
        } else plat_log("[PUB] Bibliotheque : base.new incomplet, ignore\n");
    }
    if (!neuf) n = plat_fichier_lire(FICHIER, z, sizeof(Zone));
    if (!neuf && !valide(n)) {
        memset(&z->e, 0, sizeof z->e);
        memset(z->blocs, 0, sizeof z->blocs);
        z->e.magie = MAGIE;
        return false;
    }
    modifie();
    if (infos) {
        const size_t tt = 4 + MAX_BLOCS * sizeof(Info);
        uint8_t *tmp = (uint8_t *)plat_malloc_grand(tt);
        if (tmp) {
            // fiches : infos.new s'il est entier (meme raison que base.new), sinon infos.bin
            long k = -1;
            uint32_t m = 0;
            if (plat_fichier_taille("/pub/infos.new") == (long)tt) {
                k = plat_fichier_lire("/pub/infos.new", tmp, tt);
                if (k >= 4) memcpy(&m, tmp, 4);
                if (k == (long)tt && m == MAGIE_INFOS) plat_fichier_renommer("/pub/infos.new", "/pub/infos.bin");
                else k = -1;
            }
            if (k < 0) { k = plat_fichier_lire("/pub/infos.bin", tmp, tt); m = 0; if (k >= 4) memcpy(&m, tmp, 4); }
            if (k == (long)tt && m == MAGIE_INFOS) memcpy(infos, tmp + 4, MAX_BLOCS * sizeof(Info));
            else if (k == (long)(4 + MAX_BLOCS * sizeof(Info1)) && m == MAGIE_INFOS1) {
                // fiches de la v0.4 : meme nom et meme audio ; les chaines ont ete renumerotees
                static const uint8_t ANCIENNES[27] = { 0, 1, 2, 3, 27, 5, 6, 7, 0, 9, 10, 11, 0, 8, 4, 13, 14, 17, 12, 20,
                                                       21, 22, 23, 24, 25, 15, 16 };
                const Info1 *a = (const Info1 *)(tmp + 4);
                for (int i = 0; i < MAX_BLOCS; i++) {
                    memset(&infos[i], 0, sizeof(Info));
                    memcpy(infos[i].nom, a[i].nom, sizeof infos[i].nom);
                    infos[i].chaine = a[i].chaine < 27 ? ANCIENNES[a[i].chaine] : 0;
                    infos[i].audio = a[i].audio;
                }
            }
            heap_libere(tmp);
        }
        for (int i = 0; i < MAX_BLOCS; i++) {
            if (!z->blocs[i].actif) { memset(&infos[i], 0, sizeof(Info)); continue; }
            infos[i].nom[sizeof infos[i].nom - 1] = 0;
            if (!infos[i].nom[0]) snprintf(infos[i].nom, sizeof infos[i].nom, "Pub n°%lu", (unsigned long)z->blocs[i].numero);
            if (infos[i].chaine >= NB_CHAINES) infos[i].chaine = 0;
        }
    }
    return true;
}

const Info &info(int id)
{
    static Info vide = {};
    if (!infos || id < 0 || id >= MAX_BLOCS) return vide;
    return infos[id];
}

void renommer(int id, const char *nom, int chaine)
{
    if (!infos || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif) return;
    if (nom) {
        strncpy(infos[id].nom, nom, sizeof infos[id].nom - 1);
        infos[id].nom[sizeof infos[id].nom - 1] = 0;
        // ne pas couper une lettre accentuee (UTF-8 sur 2 ou 3 octets) en deux
        int k = (int)strlen(infos[id].nom);
        if (k == (int)sizeof infos[id].nom - 1) {
            int d = k;
            while (d > 0 && ((uint8_t)infos[id].nom[d - 1] & 0xC0) == 0x80) d--;   // octets de suite
            if (d > 0 && ((uint8_t)infos[id].nom[d - 1] & 0xC0) == 0xC0) {         // debut d'une lettre
                uint8_t c = (uint8_t)infos[id].nom[d - 1];
                int lg = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                if (k - (d - 1) < lg) infos[id].nom[d - 1] = 0;                    // incomplete : retiree
            }
        }
    }
    if (chaine >= 0 && chaine < NB_CHAINES) infos[id].chaine = (uint8_t)chaine;
}

void changer_type(int id, int type)
{
    if (!z || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif || type < 0 || type >= NB_TYPES) return;
    z->blocs[id].type = (uint8_t)type;
    if (infos && !strncmp(infos[id].nom, "Jingle", 6))
        snprintf(infos[id].nom, sizeof infos[id].nom, "%s", type == JINGLE_DEBUT ? "Jingle de début" :
                 (type == JINGLE_FIN ? "Jingle de fin" : (type == JINGLE_DEBUT_FIN ? "Jingle début et fin" : "Pub")));
    modifie();
}

void marquer_a_verifier(int id, bool oui)
{
    if (!infos) return;
    for (int i = id < 0 ? 0 : id; i < MAX_BLOCS && (id < 0 || i == id); i++) {
        if (oui) infos[i].drapeaux |= 1;
        else infos[i].drapeaux &= (uint8_t)~0x0F;      // v0.9.58 : la raison du doute part avec
    }
}
void marquer_doute(int id, int raison)
{
    if (!infos || id < 0 || id >= MAX_BLOCS) return;
    infos[id].drapeaux = (uint8_t)((infos[id].drapeaux & ~0x0E) | ((raison & 7) << 1));
}
int doute(int id) { return (infos && id >= 0 && id < MAX_BLOCS) ? (infos[id].drapeaux >> 1) & 7 : 0; }
bool a_verifier(int id) { return infos && id >= 0 && id < MAX_BLOCS && z && z->blocs[id].actif && (infos[id].drapeaux & 1); }
int nb_a_verifier()
{
    int n = 0;
    for (int i = 0; i < MAX_BLOCS; i++) n += a_verifier(i);
    return n;
}

uint32_t nb_detections()
{
    if (!z) return 0;
    uint32_t n = 0;
    for (int i = 0; i < MAX_BLOCS; i++)
        if (z->blocs[i].actif && z->blocs[i].type == PUB) n += z->blocs[i].vus;
    return n;
}

void marquer_audio(int id, bool present)
{
    if (infos && id >= 0 && id < MAX_BLOCS) infos[id].audio = present;
}

void marquer_origine(int id, int origine, uint32_t distant)
{
    if (!infos || id < 0 || id >= MAX_BLOCS) return;
    infos[id].origine = (uint8_t)origine;
    infos[id].distant = distant;
}

int trouver_distant(uint32_t distant)
{
    if (!z || !infos || !distant) return -1;
    for (int i = 0; i < MAX_BLOCS; i++) if (z->blocs[i].actif && infos[i].distant == distant) return i;
    return -1;
}

bool liberer_place(int n)
{
    if (!z || !infos) return false;
    for (;;) {
        bool bloc_libre = false;
        for (int i = 0; i < MAX_BLOCS && !bloc_libre; i++) bloc_libre = !z->blocs[i].actif;
        if (bloc_libre && z->e.nb_entrees + (uint32_t)n <= (uint32_t)CAP) return true;
        // la copie de la base distante la moins recemment vue
        int vieux = -1;
        for (int i = 0; i < MAX_BLOCS; i++) {
            if (!z->blocs[i].actif || infos[i].origine != ORIG_CACHE) continue;
            if (vieux < 0 || infos[i].vu_le < infos[vieux].vu_le ||
                (infos[i].vu_le == infos[vieux].vu_le && z->blocs[i].vus < z->blocs[vieux].vus)) vieux = i;
        }
        if (vieux < 0) return false;
        supprimer_bloc(vieux, "retirée : place libérée (copie de la base la moins vue)");
    }
}

int garder_plage(int id, int t0, int t1)
{
    if (!z || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif || t1 <= t0) return 0;
    if (t0 < 0) t0 = 0;
    int n = compacter([=](const Entree &e) { return e.bloc == id && (e.t < t0 || e.t > t1); });
    // le passage garde devient le nouveau debut du bloc (comme le son, recoupe
    // lui aussi a partir de t0) : sinon un 2e decoupage, la position affichee et
    // la fin des jingles seraient decales. Retirer la meme valeur a toutes les
    // entrees du bloc garde l'ordre du tri (code, bloc, t).
    for (uint32_t i = 0; i < z->e.nb_entrees; i++)
        if (z->entrees[i].bloc == id) z->entrees[i].t = (uint16_t)(z->entrees[i].t - t0);
    if (couv) {
        const int TR = (int)(2 * emp::IPS), s = t0 / TR;
        for (int i = 0; i < NCOUV; i++) couv[id][i] = i + s < NCOUV ? couv[id][i + s] : 0;
    }
    z->blocs[id].duree_s = (uint16_t)((t1 - t0) / emp::IPS + 0.5f);
    if (z->blocs[id].nb == 0) supprimer_bloc(id, "supprimée : le découpage n'en a rien laissé");
    return n;
}

// ---- paquets .pub ----
struct EntetePaquet { uint32_t magie; uint32_t nb_blocs; };
struct BlocPaquet { uint8_t type, chaine; uint16_t duree_s; uint32_t nb; char nom[28]; };
struct EntreePaquet { uint32_t h; uint16_t t; uint16_t reserve; };
static const uint32_t MAGIE_PAQUET = 0x314B5050;   // "PPK1"

int taille_paquet(int nb_entrees) { return (int)(sizeof(EntetePaquet) + sizeof(BlocPaquet) + (size_t)nb_entrees * sizeof(EntreePaquet)); }

// v0.9.70 : export rapide. Avant : une lecture complete de la bibliotheque PAR PUB (162 pubs = 162
// lectures de centaines de milliers d'entrees) et ~700 ouvertures du fichier sur la SD -> plus de 15 s,
// le portail abandonnait (« Sauvegarde impossible ») et la detection etait en pause pendant tout ce temps.
// Maintenant : un comptage, puis les pubs par paquets qui tiennent dans un tampon de 256 Ko (PSRAM) :
// une lecture de la bibliotheque et UNE ecriture sur la SD par paquet. Fichier identique octet pour octet
// (meme ordre des pubs, meme ordre des entrees). A la fin, la taille du fichier est verifiee.
// Retour : nombre de pubs ; -1 ecriture SD ratee ; -2 pas de memoire ; -3 fichier incomplet.
int exporter(const char *chemin, int chaine, int max, bool sans_cache)
{
    if (!z || !infos) return -1;
    // v0.9.71 : tableaux de travail en PSRAM, rendus a la fin (5,5 Ko de memoire interne liberes)
    struct Travail { int16_t ordre[MAX_BLOCS]; uint32_t compte[MAX_BLOCS], pos_e[MAX_BLOCS]; uint8_t dans[MAX_BLOCS]; };
    Travail *w = (Travail *)plat_malloc_grand(sizeof(Travail));
    if (!w) return -2;
    int16_t *ordre = w->ordre;
    uint32_t *compte = w->compte, *pos_e = w->pos_e;
    uint8_t *dans = w->dans;
    // blocs retenus, du plus reconnu au moins reconnu
    int nb = 0;
    for (int i = 0; i < MAX_BLOCS; i++)
        if (z->blocs[i].actif && (chaine < 0 || infos[i].chaine == chaine) && !(sans_cache && infos[i].origine == ORIG_CACHE))
            ordre[nb++] = (int16_t)i;
    for (int a = 1; a < nb; a++)
        for (int q = a; q > 0 && z->blocs[ordre[q]].vus > z->blocs[ordre[q - 1]].vus; q--) {
            int16_t x = ordre[q]; ordre[q] = ordre[q - 1]; ordre[q - 1] = x;
        }
    if (max > 0 && nb > max) nb = max;
    // nombre d'entrees de chaque bloc (une seule lecture)
    memset(compte, 0, sizeof w->compte);
    for (uint32_t j = 0; j < z->e.nb_entrees; j++)
        if (z->entrees[j].bloc < MAX_BLOCS) compte[z->entrees[j].bloc]++;
    // tampon de sortie : 256 Ko si possible, sinon plus petit
    size_t cap = 256 * 1024;
    uint8_t *out = nullptr;
    while (cap >= 16 * 1024 && !(out = (uint8_t *)plat_malloc_grand(cap))) cap /= 2;
    if (!out) { free(w); return -2; }
    EntetePaquet e = { MAGIE_PAQUET, (uint32_t)nb };
    if (!plat_fichier_ecrire(chemin, &e, sizeof e)) { free(out); free(w); return -1; }
    size_t attendu = sizeof e;
    memset(dans, 0, sizeof w->dans);
    int res = nb;
    for (int o = 0; o < nb && res >= 0;) {
        // paquet : les blocs ordre[o .. p[ qui tiennent dans le tampon (au moins un)
        size_t taille = 0;
        int p = o;
        while (p < nb) {
            size_t t = sizeof(BlocPaquet) + (size_t)compte[ordre[p]] * sizeof(EntreePaquet);
            if (p > o && taille + t > cap) break;
            taille += t;
            p++;
        }
        if (taille <= cap) {
            size_t pos = 0;
            for (int q = o; q < p; q++) {
                int i = ordre[q];
                BlocPaquet b = { z->blocs[i].type, infos[i].chaine, z->blocs[i].duree_s, compte[i], {} };
                memcpy(b.nom, infos[i].nom, sizeof b.nom);
                memcpy(out + pos, &b, sizeof b);
                pos += sizeof b;
                pos_e[i] = (uint32_t)pos;
                pos += (size_t)compte[i] * sizeof(EntreePaquet);
                dans[i] = 1;
            }
            for (uint32_t j = 0; j < z->e.nb_entrees; j++) {
                const Entree &en = z->entrees[j];
                if (en.bloc >= MAX_BLOCS || !dans[en.bloc]) continue;
                EntreePaquet ep = { H(en.h), en.t, 0 };
                memcpy(out + pos_e[en.bloc], &ep, sizeof ep);
                pos_e[en.bloc] += sizeof ep;
            }
            for (int q = o; q < p; q++) dans[ordre[q]] = 0;
            if (!plat_fichier_ajouter(chemin, out, taille)) res = -1;
        } else {
            // une seule pub plus grosse que le tampon (rare) : par lots, comme avant
            int i = ordre[o];
            BlocPaquet b = { z->blocs[i].type, infos[i].chaine, z->blocs[i].duree_s, compte[i], {} };
            memcpy(b.nom, infos[i].nom, sizeof b.nom);
            if (!plat_fichier_ajouter(chemin, &b, sizeof b)) res = -1;
            EntreePaquet *lot = (EntreePaquet *)out;
            const int NL = (int)(cap / sizeof(EntreePaquet));
            int k = 0;
            for (uint32_t j = 0; j < z->e.nb_entrees && res >= 0; j++) {
                if (z->entrees[j].bloc != i) continue;
                lot[k].h = H(z->entrees[j].h); lot[k].t = z->entrees[j].t; lot[k].reserve = 0;
                if (++k == NL) { if (!plat_fichier_ajouter(chemin, lot, (size_t)k * sizeof(EntreePaquet))) res = -1; k = 0; }
            }
            if (k && res >= 0 && !plat_fichier_ajouter(chemin, lot, (size_t)k * sizeof(EntreePaquet))) res = -1;
        }
        attendu += taille;
        o = p;
    }
    free(out);
    free(w);
    if (res >= 0 && plat_fichier_taille(chemin) != (long)attendu) res = -3;   // fichier incomplet
    return res;
}

int exporter_bloc(int id, uint8_t *buf, int max)
{
    if (!z || !infos || id < 0 || id >= MAX_BLOCS || !z->blocs[id].actif) return 0;
    int t = taille_paquet((int)z->blocs[id].nb);
    if (t > max) return 0;
    EntetePaquet e = { MAGIE_PAQUET, 1 };
    BlocPaquet b = { z->blocs[id].type, infos[id].chaine, z->blocs[id].duree_s, z->blocs[id].nb, {} };
    memcpy(b.nom, infos[id].nom, sizeof b.nom);
    memcpy(buf, &e, sizeof e);
    memcpy(buf + sizeof e, &b, sizeof b);
    EntreePaquet *ep = (EntreePaquet *)(buf + sizeof e + sizeof b);
    uint32_t k = 0;
    for (uint32_t j = 0; j < z->e.nb_entrees && k < b.nb; j++) {
        if (z->entrees[j].bloc != id) continue;
        ep[k].h = H(z->entrees[j].h); ep[k].t = z->entrees[j].t; ep[k].reserve = 0;
        k++;
    }
    return t;
}

int importer_tampon(const uint8_t *buf, int taille, int origine, uint32_t distant, int *premier)
{
    if (premier) *premier = -1;
    if (!z || !infos || taille < (int)sizeof(EntetePaquet)) return -1;
    EntetePaquet e;
    memcpy(&e, buf, sizeof e);
    if (e.magie != MAGIE_PAQUET) return -1;
    int ajoutes = 0;
    uint32_t ancien = z->e.nb_entrees;
    size_t pos = sizeof e;
    for (uint32_t k = 0; k < e.nb_blocs; k++) {
        if (pos + sizeof(BlocPaquet) > (size_t)taille) break;
        BlocPaquet b;
        memcpy(&b, buf + pos, sizeof b);
        pos += sizeof b;
        if (pos + (size_t)b.nb * sizeof(EntreePaquet) > (size_t)taille) break;
        const EntreePaquet *ep = (const EntreePaquet *)(buf + pos);
        pos += (size_t)b.nb * sizeof(EntreePaquet);
        b.nom[sizeof b.nom - 1] = 0;
        // deja present ? (meme nom, meme type, meme nombre de codes, ou meme bloc distant)
        bool deja = distant && trouver_distant(distant) >= 0;
        for (int i = 0; i < MAX_BLOCS && !deja; i++)
            deja = z->blocs[i].actif && z->blocs[i].type == b.type && z->blocs[i].nb == b.nb &&
                   strcmp(infos[i].nom, b.nom) == 0;
        if (deja || b.nb == 0 || b.type >= NB_TYPES) continue;
        if (origine == ORIG_CACHE) {
            // fusionner d'abord ce qui a deja ete ajoute : liberer_place peut compacter
            if (z->e.nb_entrees > ancien) { fusionner(ancien); ancien = z->e.nb_entrees; }
            if (!liberer_place((int)b.nb)) break;
            ancien = z->e.nb_entrees;
        }
        if (z->e.nb_entrees + b.nb > (uint32_t)CAP) break;
        int id = -1;
        for (int i = 0; i < MAX_BLOCS; i++) if (!z->blocs[i].actif) { id = i; break; }
        if (id < 0) break;
        Entree *dst = z->entrees + z->e.nb_entrees;
        for (uint32_t j = 0; j < b.nb; j++) { dst[j].h = ep[j].h & emp::MASQUE; dst[j].bloc = (uint16_t)id; dst[j].t = ep[j].t; }
        z->e.nb_entrees += b.nb;
        Bloc &nb = z->blocs[id];
        if (couv) memset(couv[id], 0, NCOUV);
        nb.actif = 1; nb.type = b.type; nb.duree_s = b.duree_s; nb.nb = b.nb; nb.vus = 0;
        nb.numero = ++z->e.max_numero;
        memset(&infos[id], 0, sizeof(Info));
        memcpy(infos[id].nom, b.nom, sizeof b.nom);
        infos[id].chaine = b.chaine < NB_CHAINES ? b.chaine : 0;
        infos[id].origine = (uint8_t)origine;
        infos[id].distant = distant;
        infos[id].appris_le = plat_heure();
        if (premier && *premier < 0) *premier = id;
        ajoutes++;
    }
    if (z->e.nb_entrees > ancien) fusionner(ancien);
    if (ajoutes > 0) modifie();
    return ajoutes;
}

int importer(const char *chemin)
{
    if (!z || !infos) return -1;
    long taille = plat_fichier_taille(chemin);
    if (taille < (long)sizeof(EntetePaquet) || taille > 8 * 1024 * 1024) return -1;
    uint8_t *buf = (uint8_t *)plat_malloc_grand((size_t)taille);
    if (!buf) return -1;
    int ajoutes = -1;
    if (plat_fichier_lire(chemin, buf, (size_t)taille) == taille) ajoutes = importer_tampon(buf, (int)taille, ORIG_IMPORT, 0);
    heap_libere(buf);
    return ajoutes;
}

// ---------------------------------------------------------------- votes
// table de hachage ouverte : (bloc, decalage / 2) -> nombre de votes

static int nv = 0;

static void votes_raz()
{
    for (int i = 0; i < nv; i++) votes[v_utilises[i]].n = 0;
    nv = 0;
}

static void voter(uint16_t bloc, int32_t dq, uint32_t tq, uint8_t fort = 1)
{
    uint32_t cle = ((uint32_t)bloc << 21) ^ ((uint32_t)dq & 0x1FFFFF);
    uint32_t i = (cle * 2654435761u) >> 21;      // 11 bits -> 0..2047
    for (int essai = 0; essai < 32; essai++, i = (i + 1) & (NV - 1)) {
        Vote &v = votes[i];
        if (v.n == 0) {
            if (nv >= NV - 64) return;              // table pleine : on ignore
            v.cle = cle; v.n = 1; v.nf = fort; v.bloc = bloc; v.dec = dq; v.der = tq; v.pre = tq;
            v_utilises[nv++] = (uint16_t)i;
            return;
        }
        if (v.cle == cle) { if (v.n < 65535) { v.n++; v.nf += fort; } if (tq > v.der) v.der = tq; if (tq < v.pre) v.pre = tq; return; }
    }
}

// vote pour le decalage d, avec une tolerance d'une colonne
static void voter_tol(uint16_t bloc, int32_t d, uint32_t tq, uint8_t fort = 1)
{
    int32_t a = d >> 1, b = (d + 1) >> 1;
    voter(bloc, a, tq, fort);
    if (b != a) voter(bloc, b, tq, fort);
}

void reconnaitre(const emp::Code *c, int n, Resultat &r)
{
    r.bloc = -1; r.score = 0; r.score_fort = 0; r.second = 0; r.pos = 0; r.t_dernier = 0; r.decalage = 0; r.etendue = 0;
    r.jingle = -1; r.jingle_score = 0; r.jingle_t = 0; r.jingle_pos = 0;
    if (!z || !votes || z->e.nb_entrees == 0 || n <= 0) return;
    votes_raz();
    Entree *deb = z->entrees, *fin = z->entrees + z->e.nb_entrees;
    for (int i = 0; i < n; i++) {
        Entree cle = { c[i].h & emp::MASQUE, 0, 0 };
        Entree *p = std::lower_bound(deb, fin, cle, par_code);
        for (int k = 0; p < fin && H(p->h) == (c[i].h & emp::MASQUE) && k < 24; p++, k++)
            voter_tol(p->bloc, (int32_t)c[i].t - (int32_t)p->t, c[i].t);
    }
    int meilleur = -1, mj = -1;
    for (int i = 0; i < nv; i++) {
        const Vote &v = votes[v_utilises[i]];
        if (z->blocs[v.bloc].type != PUB) {
            if (mj < 0 || v.n > votes[mj].n) mj = v_utilises[i];
        } else if (meilleur < 0 || v.n > votes[meilleur].n) meilleur = v_utilises[i];
    }
    if (mj >= 0) {
        r.jingle = votes[mj].bloc;
        r.jingle_score = votes[mj].n;
        r.jingle_t = votes[mj].der;
        r.jingle_pos = (int32_t)votes[mj].der - votes[mj].dec * 2;
    }
    if (meilleur < 0) return;
    const Vote &m = votes[meilleur];
    r.bloc = m.bloc;
    r.score = m.n;
    r.score_fort = m.nf;
    for (int i = 0; i < nv; i++) {
        const Vote &v = votes[v_utilises[i]];
        if (v.bloc != m.bloc && z->blocs[v.bloc].type == PUB && v.n > r.second) r.second = v.n;
    }
    r.t_dernier = m.der;
    r.decalage = m.dec * 2;
    r.pos = (int32_t)m.der - m.dec * 2;
    r.etendue = (int32_t)(m.der - m.pre);
}

int chercher(const emp::Code &c, Touche *out, int max)
{
    if (!z || z->e.nb_entrees == 0) return 0;
    uint32_t h = c.h & emp::MASQUE;
    Entree cle = { h, 0, 0 };
    Entree *fin = z->entrees + z->e.nb_entrees;
    int k = 0;
    uint8_t fort = (c.h & emp::FAIBLE) ? 0 : 1;
    for (Entree *p = std::lower_bound(z->entrees, fin, cle, par_code); p < fin && H(p->h) == h && k < max; p++, k++) {
        out[k].tq = c.t; out[k].idx = (uint32_t)(p - z->entrees); out[k].bloc = p->bloc; out[k].tb = p->t; out[k].fort = fort;
    }
    return k;
}

void reconnaitre_touches(const Touche *t, int n, Resultat &r)
{
    r.bloc = -1; r.score = 0; r.score_fort = 0; r.second = 0; r.pos = 0; r.t_dernier = 0; r.decalage = 0; r.etendue = 0;
    r.jingle = -1; r.jingle_score = 0; r.jingle_t = 0; r.jingle_pos = 0;
    if (!z || !votes || n <= 0) return;
    votes_raz();
    for (int i = 0; i < n; i++)
        if (z->blocs[t[i].bloc].actif) voter_tol(t[i].bloc, (int32_t)t[i].tq - (int32_t)t[i].tb, t[i].tq, t[i].fort);
    int meilleur = -1, mj = -1;
    for (int i = 0; i < nv; i++) {
        const Vote &v = votes[v_utilises[i]];
        if (z->blocs[v.bloc].type != PUB) {
            if (mj < 0 || v.n > votes[mj].n) mj = v_utilises[i];
        } else if (meilleur < 0 || v.n > votes[meilleur].n) meilleur = v_utilises[i];
    }
    if (mj >= 0) {
        r.jingle = votes[mj].bloc;
        r.jingle_score = votes[mj].n;
        r.jingle_t = votes[mj].der;
        r.jingle_pos = (int32_t)votes[mj].der - votes[mj].dec * 2;
    }
    if (meilleur < 0) return;
    const Vote &m = votes[meilleur];
    r.bloc = m.bloc;
    r.score = m.n;
    r.score_fort = m.nf;
    for (int i = 0; i < nv; i++) {
        const Vote &v = votes[v_utilises[i]];
        if (v.bloc != m.bloc && z->blocs[v.bloc].type == PUB && v.n > r.second) r.second = v.n;
    }
    r.t_dernier = m.der;
    r.decalage = m.dec * 2;
    r.pos = (int32_t)m.der - m.dec * 2;
    r.etendue = (int32_t)(m.der - m.pre);
}

int accords_touches(const Touche *t, int n, int bloc, int32_t decalage, uint32_t &dernier, bool forts_seuls)
{
    dernier = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].bloc != bloc || (forts_seuls && !t[i].fort)) continue;
        int32_t d = (int32_t)t[i].tq - (int32_t)t[i].tb - decalage;
        if (d >= -2 && d <= 3) {
            if (t[i].tq > dernier) dernier = t[i].tq;
            k++;
        }
    }
    return k;
}

void confirmer(uint32_t idx, int bloc)
{
    if (!z || idx >= z->e.nb_entrees) return;
    Entree &e = z->entrees[idx];
    if (e.bloc != bloc) return;
    if ((e.h & CONF_BITS) != CONF_MAX) e.h += CONF_UN;
}

void couvrir(int bloc, int32_t t0, int32_t t1)
{
    if (!couv || bloc < 0 || bloc >= MAX_BLOCS || t1 < t0) return;
    const int32_t TR = (int32_t)(2 * emp::IPS);
    int a = t0 < 0 ? 0 : t0 / TR, b = t1 / TR;
    if (b >= NCOUV) b = NCOUV - 1;
    for (int i = a; i <= b; i++) if (couv[bloc][i] < 255) couv[bloc][i]++;
}

int consolider(int bloc)
{
    if (!z || !couv || bloc < 0 || bloc >= MAX_BLOCS || !z->blocs[bloc].actif) return 0;
    const int32_t TR = (int32_t)(2 * emp::IPS);
    // tranches assez reconnues, et ou l'on a vraiment retrouve le bloc (au moins
    // 4 entrees confirmees) : les autres entrees de ces tranches sont du bruit
    // (dans un salon bruyant, peu d'entrees sont reentendues : on ne nettoie pas,
    // de peur de jeter de bons codes masques par le bruit)
    static uint16_t confirmees[NCOUV], totales[NCOUV];
    static uint8_t nettoyer[NCOUV];
    memset(confirmees, 0, sizeof confirmees);
    memset(totales, 0, sizeof totales);
    for (uint32_t i = 0; i < z->e.nb_entrees; i++) {
        const Entree &e = z->entrees[i];
        if (e.bloc != bloc || e.t / TR >= NCOUV) continue;
        totales[e.t / TR]++;
        if (e.h & CONF_BITS) confirmees[e.t / TR]++;
    }
    bool rien = true;
    for (int i = 0; i < NCOUV; i++) {
        nettoyer[i] = couv[bloc][i] >= CONSOLIDE && confirmees[i] >= 4 && confirmees[i] >= CONSOLIDE_PART * totales[i];
        if (nettoyer[i]) rien = false;
    }
    // rognage : un bloc appris au bouton commence souvent par du PROGRAMME (tampon
    // arriere) et finit parfois par du programme (bouton presse en retard). Une
    // vraie pub repasse a l'identique ; si le bloc a deja ete reconnu CONSOLIDE
    // fois et que son debut (ou sa fin) n'a jamais ete reentendu, on le coupe.
    int prem = -1, der = -1;
    for (int i = 0; i < NCOUV; i++) if (couv[bloc][i] >= 2) { if (prem < 0) prem = i; der = i; }
    int rogne_av = -1, rogne_ap = NCOUV;
    if (prem >= 0 && (int)z->blocs[bloc].vus >= CONSOLIDE && z->blocs[bloc].type == PUB) {
        int tranches = (int)(z->blocs[bloc].duree_s / 2) + 1;
        bool vu_avant = false, vu_apres = false;
        for (int i = 0; i < prem; i++) vu_avant |= couv[bloc][i] > 0;
        for (int i = der + 1; i < NCOUV; i++) vu_apres |= couv[bloc][i] > 0;
        // jamais plus que le tampon arriere (30 s) au debut, 10 s a la fin
        if (!vu_avant && prem > 0) rogne_av = (prem - 1 < 14 ? prem - 1 : 14);
        if (!vu_apres && der < tranches - 1) rogne_ap = (der + 1 > tranches - 5 ? der + 1 : tranches - 5);
        if (rogne_av >= 0 || rogne_ap < NCOUV) rien = false;
    }
    if (rien) return 0;
    int n = compacter([=](const Entree &e) {
        if (e.bloc != bloc) return false;
        int tr = e.t / TR;
        if (tr <= rogne_av || tr >= rogne_ap) return true;
        if (e.h & CONF_BITS) return false;
        return tr < NCOUV && nettoyer[tr];
    });
    if (z->blocs[bloc].nb == 0) supprimer_bloc(bloc, "supprimée : jamais réentendue à l'identique (nettoyage automatique)");
    return n;
}

int accords(const emp::Code *c, int n, int bloc, int32_t decalage, uint32_t &dernier)
{
    dernier = 0;
    if (!z || z->e.nb_entrees == 0 || bloc < 0) return 0;
    int k = 0;
    Entree *deb = z->entrees, *fin = z->entrees + z->e.nb_entrees;
    for (int i = 0; i < n; i++) {
        Entree cle = { c[i].h & emp::MASQUE, (uint16_t)bloc, 0 };
        Entree *p = std::lower_bound(deb, fin, cle, par_code);   // trie par code puis bloc
        for (; p < fin && H(p->h) == (c[i].h & emp::MASQUE) && p->bloc == bloc; p++) {
            int32_t d = (int32_t)c[i].t - (int32_t)p->t - decalage;
            if (d >= -2 && d <= 3) {
                k++;
                if (c[i].t > dernier) dernier = c[i].t;
                break;
            }
        }
    }
    return k;
}

bool dans_une_pub(uint32_t h)
{
    if (!z || z->e.nb_entrees == 0) return false;
    Entree cle = { h & emp::MASQUE, 0, 0 };
    Entree *fin = z->entrees + z->e.nb_entrees;
    for (Entree *p = std::lower_bound(z->entrees, fin, cle, par_code); p < fin && H(p->h) == cle.h; p++)
        if (z->blocs[p->bloc].type == PUB) return true;
    return false;
}

// ---------------------------------------------------------------- journal
void journal_ajouter(const emp::Code *c, int n)
{
    if (!journal) return;
    for (int i = 0; i < n; i++) {
        if (c[i].h & emp::REQUETE) continue;
        journal[j_n & (JOURNAL - 1)] = c[i];
        j_n++;
    }
}

uint32_t journal_plus_ancien()
{
    if (!journal || j_n == 0) return 0;
    uint32_t i = j_n > (uint32_t)JOURNAL ? j_n - JOURNAL : 0;
    return journal[i & (JOURNAL - 1)].t;
}

uint32_t journal_marge(uint32_t t0)
{
    if (!journal) return 0;
    uint32_t debut = j_n > (uint32_t)JOURNAL ? j_n - JOURNAL : 0, a = debut, b = j_n;
    while (a < b) {                                   // premier code de colonne >= t0 (colonnes croissantes)
        uint32_t m = a + (b - a) / 2;
        if (journal[m & (JOURNAL - 1)].t < t0) a = m + 1;
        else b = m;
    }
    return (a - debut) + ((uint32_t)JOURNAL - (j_n - debut));
}

int journal_extraire(uint32_t t0, uint32_t t1, emp::Code *out, int max)
{
    if (!journal) return 0;
    int k = 0;
    uint32_t i = j_n > (uint32_t)JOURNAL ? j_n - JOURNAL : 0;
    for (; i < j_n && k < max; i++) {
        const emp::Code &c = journal[i & (JOURNAL - 1)];
        if (c.t >= t0 && c.t <= t1) out[k++] = c;
    }
    return k;
}


}  // namespace base
