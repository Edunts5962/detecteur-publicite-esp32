// zipflux.h - archive ZIP "stockee" (sans compression) ecrite et lue au fil de l'eau (v0.9.25).
// Aucune dependance Arduino : testable sur PC. Ecriture : tailles connues d'avance (donc la
// longueur totale est connue avant d'envoyer le premier octet : barre de progression exacte),
// CRC32 calcule pendant l'envoi et ecrit dans le descripteur de donnees.
// Lecture : archives "stockees" (celles de ce programme et de gestion_pubs.py).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace zipflux {

static inline uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t T[256];
    static bool pret = false;
    if (!pret) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
        pret = true;
    }
    crc = ~crc;
    while (n--) crc = T[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct Fichier { const char *nom; uint32_t taille; uint32_t crc; uint32_t offset; };

static inline uint32_t taille_totale(const Fichier *f, int n)
{
    uint32_t t = 22;
    for (int i = 0; i < n; i++) { uint32_t l = (uint32_t)strlen(f[i].nom); t += 30 + l + f[i].taille + 16 + 46 + l; }
    return t;
}

typedef bool (*Ecrire)(void *ctx, const uint8_t *p, size_t n);                     // false : arret
typedef size_t (*Lire)(void *ctx, int i, uint32_t pos, uint8_t *buf, size_t n);    // octets lus (0 : erreur)

static inline void p16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void p32(uint8_t *b, uint32_t v) { p16(b, (uint16_t)v); p16(b + 2, (uint16_t)(v >> 16)); }

// envoie l'archive entiere ; octets_envoyes (facultatif) est mis a jour au fil de l'eau
static inline bool envoyer(Fichier *f, int n, Ecrire ec, Lire li, void *ctx, uint8_t *buf, size_t bufn, volatile uint32_t *envoyes = nullptr)
{
    uint32_t pos = 0;
    uint8_t h[64];
    auto sortir = [&](const uint8_t *p, size_t k) { if (!ec(ctx, p, k)) return false; pos += (uint32_t)k; if (envoyes) *envoyes = pos; return true; };
    for (int i = 0; i < n; i++) {
        uint32_t l = (uint32_t)strlen(f[i].nom);
        f[i].offset = pos;
        p32(h, 0x04034b50); p16(h + 4, 20); p16(h + 6, 8); p16(h + 8, 0); p16(h + 10, 0); p16(h + 12, 0x0021);
        p32(h + 14, 0); p32(h + 18, f[i].taille); p32(h + 22, f[i].taille); p16(h + 26, (uint16_t)l); p16(h + 28, 0);
        if (!sortir(h, 30) || !sortir((const uint8_t *)f[i].nom, l)) return false;
        uint32_t crc = 0, fait = 0;
        while (fait < f[i].taille) {
            size_t k = f[i].taille - fait > bufn ? bufn : f[i].taille - fait;
            size_t lu = li(ctx, i, fait, buf, k);
            if (lu != k) return false;
            crc = crc32(crc, buf, lu);
            if (!sortir(buf, lu)) return false;
            fait += (uint32_t)lu;
        }
        f[i].crc = crc;
        p32(h, 0x08074b50); p32(h + 4, crc); p32(h + 8, f[i].taille); p32(h + 12, f[i].taille);
        if (!sortir(h, 16)) return false;
    }
    uint32_t debut_c = pos;
    for (int i = 0; i < n; i++) {
        uint32_t l = (uint32_t)strlen(f[i].nom);
        p32(h, 0x02014b50); p16(h + 4, 20); p16(h + 6, 20); p16(h + 8, 8); p16(h + 10, 0); p16(h + 12, 0); p16(h + 14, 0x0021);
        p32(h + 16, f[i].crc); p32(h + 20, f[i].taille); p32(h + 24, f[i].taille); p16(h + 28, (uint16_t)l);
        p16(h + 30, 0); p16(h + 32, 0); p16(h + 34, 0); p16(h + 36, 0); p32(h + 38, 0); p32(h + 42, f[i].offset);
        if (!sortir(h, 46) || !sortir((const uint8_t *)f[i].nom, l)) return false;
    }
    uint32_t taille_c = pos - debut_c;
    p32(h, 0x06054b50); p16(h + 4, 0); p16(h + 6, 0); p16(h + 8, (uint16_t)n); p16(h + 10, (uint16_t)n);
    p32(h + 12, taille_c); p32(h + 16, debut_c); p16(h + 20, 0);
    return sortir(h, 22);
}

// ---------------------------------------------------------------- lecture
class Lecteur {
public:
    // appeles au fil de l'eau ; donnees() renvoie false pour interrompre
    void *ctx = nullptr;
    bool (*debut)(void *ctx, const char *nom, uint32_t taille) = nullptr;
    bool (*donnees)(void *ctx, const uint8_t *p, size_t n) = nullptr;
    bool (*fin)(void *ctx) = nullptr;
    enum Etat { ENTETE, NOM, EXTRA, DONNEES, DESC, FINI, ERREUR };
    Etat etat = ENTETE;
    const char *erreur = "";
    uint32_t nb_fichiers = 0;

    bool nourrir(const uint8_t *p, size_t n)
    {
        while (n && etat != FINI && etat != ERREUR) {
            if (etat == DONNEES) {
                size_t k = n > reste ? reste : n;
                if (donnees && !donnees(ctx, p, k)) return echec("écriture impossible");
                reste -= (uint32_t)k; p += k; n -= k;
                if (!reste) {
                    if (fin && !fin(ctx)) return echec("écriture impossible");
                    if (flags & 8) { etat = DESC; nt = 0; } else { etat = ENTETE; nt = 0; }
                }
                continue;
            }
            if (etat == EXTRA) {
                size_t k = n > reste ? reste : n;
                reste -= (uint32_t)k; p += k; n -= k;
                if (!reste) commencer_fichier_apres_extra();
                continue;
            }
            size_t besoin = etat == ENTETE ? 30 : (etat == NOM ? nlen : 16);
            if (etat == DESC) {
                // descripteur : 16 octets avec signature, 12 sans
                if (nt < 4) { size_t k = n > 4 - nt ? 4 - nt : n; memcpy(t + nt, p, k); nt += k; p += k; n -= k; if (nt < 4) continue; }
                besoin = (rd32(t) == 0x08074b50) ? 16 : 12;
                if (nt < besoin) { size_t k = n > besoin - nt ? besoin - nt : n; memcpy(t + nt, p, k); nt += k; p += k; n -= k; }
                if (nt >= besoin) { etat = ENTETE; nt = 0; }
                continue;
            }
            size_t k = n > besoin - nt ? besoin - nt : n;
            memcpy(t + nt, p, k); nt += k; p += k; n -= k;
            if (nt < besoin) continue;
            if (etat == ENTETE) {
                uint32_t sig = rd32(t);
                if (sig == 0x02014b50 || sig == 0x06054b50) { etat = FINI; break; }
                if (sig != 0x04034b50) return echec("archive illisible");
                flags = rd16(t + 6);
                if (rd16(t + 8) != 0) return echec("archive compressée : utilise « stocker » (sans compression)");
                taille = rd32(t + 22);
                if ((flags & 8) && taille == 0 && rd32(t + 18) == 0) { /* fichier vide ou taille inconnue */ }
                nlen = rd16(t + 26); elen = rd16(t + 28);
                if (nlen == 0 || nlen > 200) return echec("nom de fichier trop long");
                etat = NOM; nt = 0;
            } else {   // NOM
                memcpy(nom, t, nlen); nom[nlen] = 0; nt = 0;
                if (elen) { etat = EXTRA; reste = elen; } else commencer_fichier_apres_extra();
            }
        }
        return etat != ERREUR;
    }
    bool fini() const { return etat == FINI; }

private:
    uint8_t t[256];
    char nom[208];
    size_t nt = 0, nlen = 0;
    uint32_t reste = 0, taille = 0, elen = 0;
    uint16_t flags = 0;
    static uint32_t rd16(const uint8_t *b) { return b[0] | (b[1] << 8); }
    static uint32_t rd32(const uint8_t *b) { return rd16(b) | (rd16(b + 2) << 16); }
    bool echec(const char *m) { etat = ERREUR; erreur = m; return false; }
    void commencer_fichier_apres_extra()
    {
        nb_fichiers++;
        if (debut && !debut(ctx, nom, taille)) { echec("écriture impossible"); return; }
        if (taille) { etat = DONNEES; reste = taille; }
        else {
            if (fin && !fin(ctx)) { echec("écriture impossible"); return; }
            if (flags & 8) { etat = DESC; nt = 0; } else { etat = ENTETE; nt = 0; }
        }
    }
};

}  // namespace zipflux
