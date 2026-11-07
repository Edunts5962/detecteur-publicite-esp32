// =====================================================================
// pub_console.ino - Detecteur de pub v0.6.3
// Sur la carte ES3C35P (ESP32-S3 N16R8, ecran 3,5" IPS ST77922 en QSPI,
// tactile Sitronix ST7123, SD, codec audio ES8311 avec micro). Rien a cabler.
//
// PORTAGE ES3C35P (v0.9.44, novembre 2026) depuis la carte ES3C28P :
//   - meme codec ES8311 (0x18), meme LED WS2812, meme SD 4 bits, meme ampli
//     a niveau d'activation detecte au premier demarrage : seules les broches
//     changent ;
//   - ecran : pilote ecran_qspi.cpp (ST77922, 40 MHz QSPI) a la place de
//     TFT_eSPI ; l'interface reste dessinee en 320x240 et est agrandie x4/3
//     (427x320 au centre de l'ecran 480x320) ;
//   - tactile : controleur ST7123 a 0x55 (ft6336_touch.cpp le detecte seul) ;
//     un etalonnage a 3 croix est demande au premier demarrage ;
//   - entree optique (TOSLINKBee) : deplacee sur GPIO45 / GPIO46 (connecteur
//     d'extension) et GPIO44 (connecteur serie), voir FO_BCLK plus bas.
//
// La carte, posee pres de la tele, ecoute le son et reconnait les pubs par
// leurs empreintes audio (principe de Shazam). Pendant une pub, elle BAISSE
// LE SON de la tele par sa telecommande Wi-Fi (Google TV / Android TV, TCL),
// puis le remet a la fin. On lui apprend une pub avec le gros bouton TESTER.
//
// v0.6.0 (repart de la v0.5.31) : plus d'Alexa / Matter / Bluetooth, plus
// d'infrarouge, plus de jingles, plus de commande vocale, plus de spectre a
// l'ecran ; telecommande Wi-Fi de la tele ; ecran simplifie et plus reactif.
// v0.6.3 : CAPTATION (onglet Pubs) : le son brut du micro, 16 kHz mono, est
// copie dans /pub/captations/cap_NNN.wav (+ index.csv), 15 min au plus, pour
// decouper les pubs sur le PC. La detection continue pendant la captation.
//
// Architecture :
//   - coeur 0 : tache "audio" (I2S 48 kHz -> 16 kHz), tache "moteur"
//               (empreintes, bibliotheque, detecteur, coupure), tache "ecran"
//               (envoi des images), tache "tele" (telecommande Wi-Fi),
//               tache "archive" (son des pubs, captation) ;
//   - coeur 1 : loop() lit le tactile et dessine (seulement si quelque chose
//               change), tache "reseau" (Wi-Fi, portail web).
//
// Fichiers du croquis :
//   interface.cpp/.h   les ecrans et le tactile
//   tele.cpp/.h        telecommande Wi-Fi de la tele (appairage, volume)
//   moteur.cpp/.h      boucle d'analyse (micro ou tele simulee)
//   detecteur.cpp/.h   verdict PUB / PROGRAMME, TESTER, commandes
//   coupure.cpp/.h     quand baisser et remettre le son
//   empreinte.cpp/.h   spectrogramme, pics, codes de 22 bits
//   base.cpp/.h        bibliotheque triee, votes, journal des codes
//   config.cpp/.h      reglages (NVS)
//   reseau.cpp/.h      Wi-Fi, portail web (portail.h), base de pubs (distant.*)
//   journal.cpp/.h     console serie gardee en memoire
//   fft.cpp/.h         FFT de 512 points
//   gfx.cpp/.h         moteur de dessin lisse (identique aux autres projets)
//   polices.cpp/.h     polices Inter lissees avec accents (generees)
//   ft6336_touch.*     pilote tactile (inchange)
//   qrcodegen.*        QR code de l'adresse du portail
//   plateforme.h       ce que le programme demande au materiel (ici)
//
// Reglages Arduino IDE (identiques aux autres projets) :
//   paquet esp32 3.3.10, carte "ESP32S3 Dev Module", PSRAM "OPI PSRAM",
//   Flash 16 Mo, partition "16M Flash (3MB APP/9.9MB FATFS)".
//   USB CDC On Boot "Enabled" (la console passe par l'USB : l'UART0 et ses
//   broches 43/44 restent libres). TFT_eSPI n'est plus necessaire.
// =====================================================================
#include <Arduino.h>
#include "journal.h"
#include <vector>
#include "ecran_qspi.h"
#include <SD_MMC.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "ft6336_touch.h"
#include "gfx.h"
#include "interface.h"
#include "moteur.h"
#include "plateforme.h"
#include "carte.h"
#include "reseau.h"
#include "config.h"
#include "detecteur.h"
#include "base.h"
#include "tele.h"
#include <stdarg.h>
#include <esp_arduino_version.h>
#include <esp_idf_version.h>
#include "empreinte.h"
#include "coupure.h"
#include "driver/gpio.h"
#include <time.h>
#include <WiFi.h>
#include <WiFiUdp.h>
// ---------------- Broches ES3C35P ----------------------------------------
#define LCD_BL_PIN     41              // retroeclairage (niveau haut = allume, transistor BSS138)
#define I2C_SDA        38              // bus partage : tactile ST7123 + codec ES8311 + connecteur I2C
#define I2C_SCL        39
#define TOUCH_RST      48
#define SD_SCK         5
#define SD_CMD         4
#define SD_D0          6
#define SD_D1          7
#define SD_D2          2
#define SD_D3          3
#define I2S_MCLK       17
#define I2S_BCLK       18
#define I2S_WS         21
#define I2S_DOUT       15              // ESP32 -> codec (haut-parleur : bip)
#define I2S_DIN        16              // codec -> ESP32 (micro)
#define PA_PIN         1               // ampli SC8002B (actif a l'etat BAS sur cette carte ; detecte au 1er demarrage)
#define ES8311_ADR     0x18
#define RGB_LED_PIN    40              // LED RGB WS2812 de la carte
#define LCD_BL_HZ      25000           // PWM du retroeclairage hors de l'audible (le micro est a 2 cm)

#define FS_I2S         48000
#define CH             240             // trames I2S par passage de la tache audio (5 ms)
#define ANNEAU_N       (1 << 18)       // 16,4 s de son a 16 kHz (PSRAM) - v0.9.11 : etait 8,2 s


// =========================================================================
// Plateforme (voir plateforme.h)
// =========================================================================
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sd_ok = false;
static Preferences s_prefs;
static volatile bool s_audio_ok = false;
static char s_audio_etat[40] = "démarrage…";
// anneau du micro a 16 kHz (rempli par la tache audio ; lu par le moteur et
// par la captation, chacun avec son propre index)
static int16_t *s_anneau = nullptr;
static volatile uint32_t r_ecrit = 0;



uint32_t plat_ms() { return millis(); }
uint32_t plat_heure()
{
    time_t t = time(nullptr);
    return t > 1700000000 ? (uint32_t)t : 0;      // reglee par Internet (SNTP) une fois connecte a la box
}
void plat_verrou() { portENTER_CRITICAL(&s_mux); }
void plat_deverrou() { portEXIT_CRITICAL(&s_mux); }
void plat_luminosite(int pct) { ledcWrite(LCD_BL_PIN, (uint32_t)(pct * 255 / 100)); }
uint32_t plat_memoire_libre() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t plat_psram_libre() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
void *plat_malloc_grand(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
bool plat_audio_ok() { return s_audio_ok; }
const char *plat_audio_etat() { return s_audio_etat; }
bool plat_sd() { return s_sd_ok; }
void plat_reseau(Reseau &r) { reseau::infos(r); }

// La carte SD est partagee par la tache moteur (bibliotheque), la tache
// d'archive (audio), la page web et l'ecran : un mutex recursif la protege.
static SemaphoreHandle_t s_sd_mtx = NULL;
void sd_prendre() { if (s_sd_mtx) xSemaphoreTakeRecursive(s_sd_mtx, portMAX_DELAY); }
void sd_rendre() { if (s_sd_mtx) xSemaphoreGiveRecursive(s_sd_mtx); }
bool sd_ok() { return s_sd_ok; }

// Ecriture d'un fichier sur la carte SD, par morceaux de 4 Ko recopies dans un
// tampon de memoire INTERNE (le pilote SD ecrit par DMA ; les grandes zones en
// PSRAM comme la bibliotheque passent ainsi par un tampon sur).
// (Avant v0.5.31 : le resultat etait teste APRES f.close(), qui rend le fichier
// "faux" : chaque sauvegarde etait declaree ratee et base.new n'etait jamais
// renomme en base.bin. Au redemarrage, la carte relisait une vieille bibliotheque.)
// A appeler SD prise (sd_prendre) : le tampon est partage.
static uint8_t *s_sd_dma = nullptr;            // tampon d'ecriture en memoire interne (DMA)
static size_t s_sd_tampon = 0;                 // v0.9.16 : sa taille (4, 8 ou 16 Ko), choisie dans le portail (cle NVS "sdbuf"), lue au premier usage
static size_t ecrire_morceaux(fs::File &f, const uint8_t *p, size_t n)
{
    if (!s_sd_dma) {
        int ko = 16;
        if (plat_nvs_lire("sdbuf", &ko, sizeof ko) && (ko == 4 || ko == 8 || ko == 16)) { } else ko = 16;
        s_sd_tampon = (size_t)ko * 1024;
        s_sd_dma = (uint8_t *)heap_caps_malloc(s_sd_tampon, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_sd_dma) s_sd_tampon = 0;
    }
    if (!s_sd_dma) return f.write(p, n);
    size_t fait = 0;
    while (fait < n) {
        size_t k = n - fait > s_sd_tampon ? s_sd_tampon : n - fait;
        memcpy(s_sd_dma, p + fait, k);
        size_t w = f.write(s_sd_dma, k);
        fait += w;
        if (w != k) break;
    }
    return fait;
}

bool plat_fichier_ecrire(const char *chemin, const void *buf, size_t n)
{
    if (!s_sd_ok) return false;
    sd_prendre();
    fs::File f = SD_MMC.open(chemin, FILE_WRITE);
    bool ok = false;
    if (f) {
        size_t e = ecrire_morceaux(f, (const uint8_t *)buf, n);
        f.flush();
        ok = e == n;                                  // AVANT close()
        f.close();
    }
    sd_rendre();
    if (!ok) Console.printf("[PUB] Ecriture de %s : ECHEC (%u octets)\n", chemin, (unsigned)n);
    return ok;
}

bool plat_fichier_ajouter(const char *chemin, const void *buf, size_t n)
{
    if (!s_sd_ok) return false;
    sd_prendre();
    fs::File f = SD_MMC.open(chemin, FILE_APPEND);
    bool ok = false;
    if (f) {
        size_t e = ecrire_morceaux(f, (const uint8_t *)buf, n);
        f.flush();
        ok = e == n;                                  // AVANT close()
        f.close();
    }
    sd_rendre();
    if (!ok) Console.printf("[PUB] Ajout a %s : ECHEC (%u octets)\n", chemin, (unsigned)n);
    return ok;
}

long plat_fichier_lire(const char *chemin, void *buf, size_t nmax)
{
    if (!s_sd_ok) return -1;
    sd_prendre();
    long lu = -1;
    if (SD_MMC.exists(chemin)) {
        fs::File f = SD_MMC.open(chemin, FILE_READ);
        if (f) {
            size_t n = f.size();
            if (n > nmax) n = nmax;
            lu = (long)f.read((uint8_t *)buf, n);
            f.close();
        }
    }
    sd_rendre();
    return lu;
}

bool plat_fichier_renommer(const char *de, const char *vers)
{
    if (!s_sd_ok) return false;
    sd_prendre();
    bool ok = SD_MMC.exists(de);
    if (ok) {
        if (SD_MMC.exists(vers)) SD_MMC.remove(vers);   // FAT : le renommage ne remplace pas
        ok = SD_MMC.rename(de, vers);
    }
    sd_rendre();
    return ok;
}

bool plat_fichier_supprimer(const char *chemin)
{
    if (!s_sd_ok) return false;
    sd_prendre();
    bool ok = SD_MMC.exists(chemin) && SD_MMC.remove(chemin);
    sd_rendre();
    return ok;
}

bool plat_dossier_creer(const char *chemin)
{
    if (!s_sd_ok) return false;
    sd_prendre();
    bool ok = SD_MMC.exists(chemin) || SD_MMC.mkdir(chemin);
    sd_rendre();
    return ok;
}

// =========================================================================
void plat_led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_LED_PIN, r, g, b); }
static volatile uint32_t s_flash_fin = 0;     // v0.9.6 : flash blanc au toucher (jusqu'a cet instant)

long plat_fichier_taille(const char *chemin)
{
    if (!s_sd_ok) return -1;
    sd_prendre();
    long n = -1;
    if (SD_MMC.exists(chemin)) {
        fs::File f = SD_MMC.open(chemin, FILE_READ);
        if (f) { n = (long)f.size(); f.close(); }
    }
    sd_rendre();
    return n;
}

// =========================================================================
// Archive audio : les 7 dernieres minutes, 8 kHz en loi mu (8 Ko/s), tournent
// dans /pub/archive.ulaw. Quand un bloc est appris, son passage est copie en
// WAV (/pub/audio/<n>.wav) pour l'ecouter et le decouper depuis le telephone.
// =========================================================================
#define ARCH_FICHIER   "/pub/archive.ulaw"
#define ARCH_TAILLE    (8000UL * 720)          // 720 s : un ecran pub de 12 min garde son son
#define ARCH_TAMPON    (1 << 17)               // 16 s d'avance en PSRAM
static uint8_t *a_tampon = nullptr;
static volatile uint32_t a_ecrit = 0;          // octets produits depuis le demarrage
static volatile uint32_t a_vide = 0;           // octets deja ecrits sur la carte
static volatile uint32_t a_col0 = 0;           // octet de la colonne 0 des empreintes
static int16_t a_moitie = 0;
static bool a_pair = false;
struct ReqAudio { int type; uint32_t a, b, c; };   // 1 extraire (c0, c1, n), 2 couper (n, d, f), 3 supprimer (n), 4 nettoyer les sons sans pub
static QueueHandle_t a_file = NULL;
static volatile bool a_pret = false;            // la tache archive tourne et ecrit l'archive

static uint8_t ulaw(int16_t x)
{
    const int BIAIS = 0x84, CLIP = 32635;
    int signe = (x >> 8) & 0x80;
    int v = signe ? -(int)x : x;
    if (v > CLIP) v = CLIP;
    v += BIAIS;
    int exp = 7;
    for (int m = 0x4000; (v & m) == 0 && exp > 0; m >>= 1) exp--;
    int mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(signe | (exp << 4) | mant);
}

static int16_t ulaw_inv(uint8_t u)
{
    u = ~u;
    int signe = u & 0x80, exp = (u >> 4) & 7, mant = u & 0x0F;
    int v = ((mant << 3) + 0x84) << exp;
    v -= 0x84;
    return (int16_t)(signe ? -v : v);
}

void plat_archive_reset()
{
    // les colonnes des empreintes ne repartent pas de 0 (emp::reinit garde le
    // compteur) : l'octet de la colonne 0 est donc "maintenant - colonne x 256".
    // Avant, a_col0 = a_ecrit : apres un changement de source, chaque son
    // de pub etait cherche dans le futur et jamais ecrit.
    a_col0 = a_ecrit - emp::colonne() * (uint32_t)emp::SAUT;
    a_pair = false;
}

void plat_archive(const int16_t *x, int n)
{
    if (!a_tampon) return;
    uint32_t e = a_ecrit;
    for (int i = 0; i < n; i++) {
        if (!a_pair) { a_moitie = x[i]; a_pair = true; continue; }
        a_pair = false;
        a_tampon[e & (ARCH_TAMPON - 1)] = ulaw((int16_t)(((int)a_moitie + x[i]) / 2));
        e++;
    }
    a_ecrit = e;
}

bool plat_audio_extraire(uint32_t c0, uint32_t c1, uint32_t numero)
{
    if (!s_sd_ok || !a_file || c1 <= c0) {
        Console.printf("[PUB] Son de la pub n°%lu : pas enregistré (%s)\n", (unsigned long)numero,
                       !s_sd_ok ? "pas de carte SD" : !a_file ? "archive audio absente" : "passage vide");
        return false;
    }
    if (!a_pret) {
        Console.printf("[PUB] Son de la pub n°%lu : pas enregistré (archive audio pas encore prête)\n", (unsigned long)numero);
        return false;
    }
    uint32_t b0 = a_col0 + c0 * 256;
    if (a_ecrit > ARCH_TAILLE && b0 < a_ecrit - ARCH_TAILLE) {
        Console.printf("[PUB] Son de la pub n°%lu : pas enregistré (début plus vieux que les 12 min gardées)\n", (unsigned long)numero);
        return false;
    }
    ReqAudio q = { 1, c0, c1, numero };
    if (xQueueSend(a_file, &q, 0) == pdTRUE) return true;
    Console.printf("[PUB] Son de la pub n°%lu : pas enregistré (file d'attente pleine)\n", (unsigned long)numero);
    return false;
}

const char *plat_version_arduino()
{
    static char b[16];
    if (!b[0]) snprintf(b, sizeof b, "%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
    return b;
}
const char *plat_version_idf()
{
    static char b[20];
    if (!b[0]) {
        snprintf(b, sizeof b, "%s", esp_get_idf_version());
        char *t = strchr(b, '-');                     // "v5.5.1-718-g..." -> "v5.5.1"
        if (t) *t = 0;
    }
    return b;
}

// une ligne dans le journal (detecteur, bibliotheque : sans dependre de la carte)
void plat_log(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Console.print(b);
}


void plat_audio_supprimer(uint32_t numero)
{
    ReqAudio q = { 3, numero, 0, 0 };
    if (a_file) xQueueSend(a_file, &q, 0);
}

void audio_couper(uint32_t numero, int debut_ds, int fin_ds)
{
    ReqAudio q = { 2, numero, (uint32_t)debut_ds, (uint32_t)fin_ds };
    if (a_file) xQueueSend(a_file, &q, 0);
}

// entete WAV 16 bits mono (8 kHz : sons des pubs ; 16 kHz : captations)
static void entete_wav(uint8_t *h, uint32_t octets, uint32_t freq = 8000)
{
    auto u32 = [](uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; };
    auto u16 = [](uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; };
    memcpy(h, "RIFF", 4); u32(h + 4, 36 + octets); memcpy(h + 8, "WAVEfmt ", 8);
    u32(h + 16, 16); u16(h + 20, 1); u16(h + 22, 1); u32(h + 24, freq); u32(h + 28, freq * 2);
    u16(h + 32, 2); u16(h + 34, 16); memcpy(h + 36, "data", 4); u32(h + 40, octets);
}

// ecrit sur la carte ce qui attend dans le tampon
static void vider(fs::File &arch)
{
    uint32_t e = a_ecrit;
    if (e - a_vide > ARCH_TAMPON - 4096) a_vide = e - (ARCH_TAMPON - 4096);   // retard : on perd un bout
    while (a_vide < e) {
        uint32_t n = e - a_vide, pos = a_vide % ARCH_TAILLE, t = a_vide & (ARCH_TAMPON - 1);
        if (n > 4096) n = 4096;
        if (pos + n > ARCH_TAILLE) n = ARCH_TAILLE - pos;
        if (t + n > ARCH_TAMPON) n = ARCH_TAMPON - t;
        sd_prendre();
        arch.seek(pos);
        arch.write(a_tampon + t, n);
        sd_rendre();
        a_vide += n;
    }
}

static void extraire_wav(fs::File &arch, uint32_t c0, uint32_t c1, uint32_t numero)
{
    uint32_t b0 = a_col0 + c0 * 256, b1 = a_col0 + c1 * 256;
    uint32_t t0 = millis();
    while (a_vide < b1 && millis() - t0 < 3000) { vider(arch); vTaskDelay(pdMS_TO_TICKS(20)); }
    sd_prendre(); arch.flush(); sd_rendre();
    if (a_vide < b1) b1 = a_vide;
    if (a_vide > ARCH_TAILLE && b0 < a_vide - ARCH_TAILLE) b0 = a_vide - ARCH_TAILLE;
    if (b1 <= b0 || (int32_t)(b1 - b0) < 0) {
        Console.printf("[PUB] Son de la pub n°%lu : ECHEC, passage absent de l'archive (demandé %lu..%lu, archive jusqu'à %lu)\n",
                       (unsigned long)numero, (unsigned long)(a_col0 + c0 * 256), (unsigned long)(a_col0 + c1 * 256), (unsigned long)a_vide);
        return;
    }
    char chemin[40];
    snprintf(chemin, sizeof chemin, "/pub/audio/%lu.wav", (unsigned long)numero);
    static uint8_t *lu = (uint8_t *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    static int16_t *pcm = (int16_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!lu || !pcm) { Console.printf("[PUB] Son de la pub n°%lu : ECHEC, mémoire PSRAM\n", (unsigned long)numero); return; }
    uint8_t h[44];
    entete_wav(h, (b1 - b0) * 2);
    sd_prendre();
    fs::File w = SD_MMC.open(chemin, FILE_WRITE);
    sd_rendre();
    if (!w) { Console.printf("[PUB] Son de la pub n°%lu : ECHEC, impossible de créer %s\n", (unsigned long)numero, chemin); return; }
    size_t ecrits = 0;
    sd_prendre(); w.write(h, 44); sd_rendre();
    for (uint32_t b = b0; b < b1;) {
        uint32_t pos = b % ARCH_TAILLE, n = b1 - b;
        if (n > 2048) n = 2048;
        if (pos + n > ARCH_TAILLE) n = ARCH_TAILLE - pos;
        sd_prendre();
        arch.seek(pos);
        arch.read(lu, n);
        sd_rendre();
        for (uint32_t i = 0; i < n; i++) pcm[i] = ulaw_inv(lu[i]);
        sd_prendre(); ecrits += w.write((uint8_t *)pcm, n * 2); sd_rendre();
        b += n;
    }
    sd_prendre(); w.close(); sd_rendre();
    if (ecrits != (size_t)(b1 - b0) * 2)
        Console.printf("[PUB] Son de la pub n°%lu : ECHEC d'écriture sur la carte SD (%u / %lu octets)\n", (unsigned long)numero,
                       (unsigned)ecrits, (unsigned long)(b1 - b0) * 2);
    else
        Console.printf("[PUB] Son de la pub n°%lu enregistré : %s, %.0f s\n", (unsigned long)numero, chemin, (b1 - b0) / 8000.0f);
}

static void couper_wav(uint32_t numero, int d_ds, int f_ds)
{
    char chemin[40], tmp[40];
    snprintf(chemin, sizeof chemin, "/pub/audio/%lu.wav", (unsigned long)numero);
    snprintf(tmp, sizeof tmp, "/pub/audio/%lu.tmp", (unsigned long)numero);
    sd_prendre();
    fs::File r = SD_MMC.open(chemin, FILE_READ);
    fs::File w = r ? SD_MMC.open(tmp, FILE_WRITE) : fs::File();
    sd_rendre();
    if (!r || !w) {
        Console.printf("[PUB] Découpage du son n°%lu : ECHEC, %s\n", (unsigned long)numero, !r ? "pas de son sur la carte" : "fichier temporaire impossible");
        sd_prendre(); if (r) r.close(); sd_rendre();
        return;
    }
    uint32_t total = (uint32_t)((r.size() - 44) / 2);
    uint32_t s0 = (uint32_t)d_ds * 800, s1 = (uint32_t)f_ds * 800;
    if (s1 > total) s1 = total;
    if (s0 >= s1) { sd_prendre(); r.close(); w.close(); SD_MMC.remove(tmp); sd_rendre(); return; }
    uint8_t h[44];
    entete_wav(h, (s1 - s0) * 2);
    static uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) { sd_prendre(); r.close(); w.close(); sd_rendre(); return; }
    sd_prendre(); w.write(h, 44); r.seek(44 + s0 * 2); sd_rendre();
    for (uint32_t k = (s1 - s0) * 2; k > 0;) {
        uint32_t n = k > 4096 ? 4096 : k;
        sd_prendre(); r.read(buf, n); w.write(buf, n); sd_rendre();
        k -= n;
    }
    sd_prendre(); r.close(); w.close(); bool ok = SD_MMC.remove(chemin) && SD_MMC.rename(tmp, chemin); sd_rendre();
    if (ok) Console.printf("[PUB] Son de la pub n°%lu découpé : gardé de %.1f à %.1f s (%.0f s -> %.0f s)\n", (unsigned long)numero,
                           s0 / 8000.0f, s1 / 8000.0f, total / 8000.0f, (s1 - s0) / 8000.0f);
    else Console.printf("[PUB] Découpage du son n°%lu : ECHEC au remplacement du fichier\n", (unsigned long)numero);
}

// =========================================================================
// Captation (v0.6.3) : le son BRUT du micro (anneau 16 kHz, avant analyse),
// copie tel quel dans /pub/captations/cap_NNN.wav, 15 min au plus. Lue avec
// son propre index : la detection n'est pas touchee. Ecrite par la tache
// archive ; l'entete WAV est remis a jour toutes les 30 s (fichier lisible
// meme si la carte est coupee pendant la captation).
// =========================================================================
#define CAP_DOSSIER    "/pub/captations"
#define CAP_INDEX      CAP_DOSSIER "/index.csv"
#define CAP_FS         16000
static const uint32_t CAP_MAX_ECH = (uint32_t)CAPTATION_MAX_S * CAP_FS;
static volatile int c_etat = 0;                // 0 arretee, 1 demarrage, 2 en cours
static volatile bool c_demande = false, c_stop = false, c_annule = false;   // c_annule (v0.9.10) : la fin supprime le fichier
static volatile uint32_t c_lu_dem = 0;          // position du son au moment du toucher (v0.7.4)
static uint32_t c_num_max = 0; static bool c_num_ok = false;   // dernier numero cap_NNN : liste la SD une seule fois
static volatile uint32_t c_num = 0, c_ech = 0, c_fins = 0;
static volatile bool c_fin_ok = false;
static volatile float c_fin_duree = 0;
static char c_fin_txt[48] = "";
static uint32_t c_lu = 0, c_ech_entete = 0, c_heure = 0, c_perdus = 0;
static int c_chaine = 0;
static volatile int c_chaine_sel = 0;           // v0.9.13 : chaine choisie pour la captation (0 = celle que la carte regarde)
static bool c_chaine_lue = false;
static int16_t *c_buf = nullptr;               // 2048 echantillons (PSRAM)
static fs::File c_f;

// =========================================================================
// Captation AUTOMATIQUE (v0.9.39, verdicts et statistiques en direct v0.9.41). En plus du fichier
// entier cap_NNN.wav, la carte decoupe le bandeau aux silences entre les pubs. Chaque morceau :
//   - moins de 5 s -> "trop courte", ignoree ; plus de 90 s -> jete (programme) ;
//   - deja connu de la bibliotheque (reconnu pendant le morceau) -> "connue", ignoree ;
//   - inconnu -> "enregistree" : /pub/captations/auto/pub_AAAAMMJJ_HHMMSS_CHAINE_NNN.wav
// La liste (index.csv du dossier auto) est ecrite a la fin ; ANNULER supprime tout.
// Un "creux" = niveau < 15 % du niveau moyen pendant 360 ms.
// v0.9.57 : (1) les pubs RECONNUES servent de reperes : coupe juste avant et juste apres, meme sans
// silence ; le morceau entre deux limites franches est rattrape. (2) AUTO seulement : apres 40 s
// sans vrai silence, coupe au premier creux doux (160 ms < 35 %), fichier « _sans_silence ».
// (3) APPRENDRE n'apprend que des morceaux bornes proprement de 5 a 60 s ; si du son n'a pas pu
// etre traite (20 s ou plus), la captation entiere est GARDEE (filet). (4) bilan sur toute la duree.
// =========================================================================
#define AUTO_DOSSIER   CAP_DOSSIER "/auto"
#define AUTO_TMP       AUTO_DOSSIER "/_en_cours.wav"
#define AUTO_INDEX     AUTO_DOSSIER "/index.csv"
static const uint32_t AU_TRAME   = 320;                       // 20 ms
static const uint32_t AU_MIN     = 5UL * CAP_FS;              // pub : 5 s au moins
static const uint32_t AU_MAX     = 90UL * CAP_FS;             // pub : 90 s au plus (au-dela : programme, jete)
static const int      AU_CREUX_N = 18;                        // 18 trames = 360 ms de creux pour couper
static const uint32_t AU_MARGE   = CAP_FS * 15 / 100;         // 150 ms de silence gardes de chaque cote
static const uint32_t AU_RETENUE = CAP_FS * 6 / 10;           // l'ecriture a 0,6 s de retard sur l'analyse
static const float    AU_REL     = 0.15f;                     // creux : niveau < 15 % du niveau moyen
static const float    AU_PLANCHER = 200.0f;                   // niveau moyen minimal pris comme reference
static const uint32_t AU_BRUIT   = CAP_FS;                    // v0.9.57 : moins de 1 s entre deux creux : ignore sans le compter
static const uint32_t AU_APPR_MAX = 60UL * CAP_FS;            // v0.9.57 : APPRENDRE : 60 s au plus entre deux vrais silences...
static const uint32_t AU_APPR_MAX1 = 45UL * CAP_FS;           // ... 45 s si une seule limite est un vrai silence (l'autre : pub reconnue)
static const uint32_t AU_SECOURS = 40UL * CAP_FS;             // v0.9.57 : AUTO : apres 40 s sans vrai silence...
static const float    AU_DOUX    = 0.35f;                     // ... un creux doux (< 35 % du niveau moyen)...
static const int      AU_DOUX_N  = 8;                         // ... de 160 ms suffit pour couper
static const float    AU_FILET_S = 20.0f;                     // v0.9.57 : APPRENDRE : 20 s non traitees ou plus = captation gardee
struct AuFichier { char nom[64]; float duree; uint32_t heure; };
static volatile bool c_auto = false;                          // la captation en cours est automatique
// v0.9.53 : captation APPRENDRE = AUTO + chaque pub inconnue apprise directement dans la
// bibliotheque (origine « auto », « a verifier »). Aucun fichier n'est garde : le son
// de chaque pub apprise va dans /pub/audio/ (comme ENREGISTRER), le bandeau entier
// cap_NNN.wav est supprime a la fin. ANNULER garde les pubs deja apprises.
static volatile bool c_apprendre = false;
static volatile bool au_appr_bilan = false;                   // le bilan affiche est celui d'une captation APPRENDRE
static uint32_t au_res_lu = 0;                                // resultats lus (det::auto_resultat)
static int au_attente = 0;                                    // apprentissages demandes, resultat pas encore revenu
static bool au_plein = false;                                 // bibliotheque pleine : on n'apprend plus
static uint32_t colonne_de(uint32_t ech);                     // echantillon de l'anneau -> colonne des empreintes
static volatile bool au_bilan = false;                        // la derniere captation etait automatique : statistiques a afficher
static volatile int au_gardes = 0, au_connues = 0, au_courtes = 0, au_jetes = 0;
static volatile float au_seg_s = 0, au_total_s = 0;
static bool au_seg = false, au_creux = false, au_connue = false, au_perdu = false;
static uint32_t au_ana = 0, au_s0 = 0, au_ecr = 0, au_creux_deb = 0, au_fin_prec = 0;
static int au_creux_n = 0, au_n = 0, au_connue_num = 0;
static float au_moy = 0;
static fs::File au_f;
static int16_t *au_buf = nullptr;
static std::vector<AuFichier> au_liste;                       // morceaux gardes pendant cette captation
static EvAuto au_ev[AUTO_NB_EV];                              // derniers verdicts (le plus recent en 0)
static int au_nev = 0;
// v0.9.57 : reperes des pubs reconnues et bilan sur toute la duree
static bool au_k_actif = false;                               // une pub connue est en train de passer
static uint32_t au_k_deb = 0, au_k_fin = 0;                   // ses limites (echantillons de l'anneau)
static int au_k_num = 0;
static volatile int au_k_n = 0;
static int au_k_liste[AUTO_NB_CONNUES];                       // numeros entendus, sans doublon
static volatile float au_connu_s = 0, au_non_s = 0;           // secondes : pubs connues / son non traite
static volatile int au_secours = 0;
static volatile int au_sures = 0;                             // v0.9.58 : APPRENDRE : pubs sures (OK tout de suite) ou validees (vues 2 fois)
static bool au_fin_propre = false;                            // au_fin_prec est une vraie limite (creux, pub connue)
static bool au_fin_net = false;                               // ... et c'est un vrai silence (pas la fin d'une pub connue)
static bool au_s0_net = false;                                // le morceau en cours commence a un vrai silence
static int au_dn = 0;                                         // creux doux en cours (trames)
static uint32_t au_ddeb = 0;
static volatile bool au_garde = false;                        // APPRENDRE : captation gardee (filet)
static uint32_t ech_de_col(uint32_t col);                     // colonne des empreintes -> echantillon de l'anneau

static void au_ev_ajouter(int type, const char *txt, const char *nom)
{
    plat_verrou();
    for (int i = AUTO_NB_EV - 1; i > 0; i--) au_ev[i] = au_ev[i - 1];
    au_ev[0].type = (uint8_t)type;
    strncpy(au_ev[0].txt, txt, sizeof au_ev[0].txt - 1); au_ev[0].txt[sizeof au_ev[0].txt - 1] = 0;
    strncpy(au_ev[0].nom, nom, sizeof au_ev[0].nom - 1); au_ev[0].nom[sizeof au_ev[0].nom - 1] = 0;
    if (au_nev < AUTO_NB_EV) au_nev++;
    plat_deverrou();
}

// "15.3" -> "15,3" (affichage francais)
static void au_virgule(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

static void au_debut()
{
    au_gardes = 0; au_connues = 0; au_courtes = 0; au_jetes = 0; au_seg_s = 0; au_total_s = 0; au_n = 0;
    au_nev = 0;
    memset(au_ev, 0, sizeof au_ev);
    au_bilan = true;
    au_seg = false; au_creux = false; au_connue = false; au_perdu = false;
    au_ana = c_lu_dem; au_fin_prec = c_lu_dem; au_creux_n = 0; au_moy = 0;
    au_liste.clear();
    au_appr_bilan = c_apprendre;
    au_attente = 0;
    au_plein = false;
    au_k_actif = false; au_k_num = 0; au_k_n = 0; au_connu_s = 0; au_non_s = 0; au_secours = 0; au_sures = 0;
    au_fin_propre = false; au_fin_net = false; au_s0_net = false; au_dn = 0; au_garde = false;
    { det::ResAuto r; while (det::auto_resultat(au_res_lu, r)) {} }   // resultats d'avant : oublies
    if (!au_buf) au_buf = (int16_t *)heap_caps_malloc(2048 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (c_apprendre) {
        Console.println("[PUB] Apprendre : les pubs inconnues (5 à 90 s) vont directement dans la bibliothèque (à vérifier)");
        return;
    }
    sd_prendre();
    if (!SD_MMC.exists(AUTO_DOSSIER)) SD_MMC.mkdir(AUTO_DOSSIER);
    if (SD_MMC.exists(AUTO_TMP)) SD_MMC.remove(AUTO_TMP);
    sd_rendre();
    Console.printf("[PUB] Auto : découpe des pubs (5 à 90 s, pubs connues ignorées), dossier %s\n", AUTO_DOSSIER);
}

// ecrit [au_ecr, fin) du morceau en cours ; false si l'ecriture a echoue
static bool au_ecrire_jusqua(uint32_t fin)
{
    if (c_apprendre) { au_ecr = fin; return true; }          // APPRENDRE : pas de fichier (le son vient de l'archive)
    uint32_t e = r_ecrit;
    if (e - au_ecr > ANNEAU_N - 4096) { au_perdu = true; au_ecr = e - (ANNEAU_N - 4096); }
    while ((int32_t)(fin - au_ecr) > 0) {
        uint32_t n = fin - au_ecr;
        if (n > 2048) n = 2048;
        for (uint32_t i = 0; i < n; i++) au_buf[i] = s_anneau[(au_ecr + i) & (ANNEAU_N - 1)];
        sd_prendre();
        size_t w = ecrire_morceaux(au_f, (const uint8_t *)au_buf, n * 2);
        sd_rendre();
        au_ecr += n;
        if (w != n * 2) return false;
    }
    return true;
}

static void au_ouvrir(uint32_t debut, bool net = true)
{
    if (!au_buf) return;
    au_s0_net = net;
    if (c_apprendre) {                                       // APPRENDRE : pas de fichier temporaire
        au_seg = true; au_s0 = au_ecr = debut; au_connue = false; au_perdu = false; au_connue_num = 0;
        return;
    }
    sd_prendre();
    au_f = SD_MMC.open(AUTO_TMP, FILE_WRITE);
    if (au_f) { uint8_t h[44]; entete_wav(h, 0, CAP_FS); au_f.write(h, 44); }
    sd_rendre();
    if (!au_f) { Console.println("[PUB] Auto : fichier temporaire impossible à créer"); return; }
    au_seg = true; au_s0 = au_ecr = debut; au_connue = false; au_perdu = false; au_connue_num = 0;
}

// ferme et supprime le fichier temporaire du morceau en cours
static void au_effacer_tmp()
{
    if (au_f) { sd_prendre(); au_f.close(); SD_MMC.remove(AUTO_TMP); sd_rendre(); }
    au_seg = false;
}

// morceau jete (trop long, incomplet, carte SD lente, erreur d'ecriture)
static void au_jeter(const char *raison)
{
    if (au_seg) au_non_s = au_non_s + (float)(au_ana - au_s0) / CAP_FS;
    au_effacer_tmp();
    au_jetes = au_jetes + 1;
    char t[44];
    snprintf(t, sizeof t, "Morceau jeté : %s", raison);
    au_ev_ajouter(4, t, "");
    Console.printf("[PUB] Auto : %s\n", t);
}

// morceau de moins de 5 s : ignore (la pub suivante commencera apres le creux)
static void au_courte(uint32_t fin)
{
    // v0.9.69 : duree avec son signe. Le silence peut commencer juste AVANT le debut du morceau (morceau
    // ouvert a la fin d'une pub connue, marge comprise) : fin - debut < 0. Avant, ce calcul sans signe
    // donnait 268435 s et comptait une fausse « pub trop courte ».
    int32_t n_ech = (int32_t)(fin - au_s0);
    float d = (float)n_ech / CAP_FS;
    bool bruit = n_ech < (int32_t)AU_BRUIT;                  // moins de 1 s, ou negatif : rien a compter
    au_effacer_tmp();
    au_fin_prec = fin;
    au_fin_propre = true;
    au_fin_net = true;
    if (bruit) return;                                       // v0.9.57 : un claquement entre deux creux, pas une pub
    au_courtes = au_courtes + 1;
    char t[44], nb[12];
    snprintf(nb, sizeof nb, "%.1f", d); au_virgule(nb);
    snprintf(t, sizeof t, "Pub trop courte · %s s : ignorée", nb);
    au_ev_ajouter(2, t, "");
    Console.printf("[PUB] Auto : %s\n", t);
}

// fin = index apres le dernier echantillon du morceau ; secours : coupe sans vrai silence (AUTO)
static void au_fermer(uint32_t fin, bool secours = false, bool fin_net = true)
{
    float d = (float)(fin - au_s0) / CAP_FS;
    bool ok = au_ecrire_jusqua(fin);
    uint32_t debut = au_s0;
    au_fin_prec = fin;
    au_fin_propre = true;
    au_fin_net = fin_net && !secours;
    int nets = (au_s0_net ? 1 : 0) + (au_fin_net ? 1 : 0);   // limites qui sont de vrais silences
    if (!secours && nets == 0) secours = true;               // entre deux pubs reconnues sans silence : peut-etre plusieurs pubs
    if (c_apprendre && !au_connue && (secours || fin - debut > (nets == 2 ? AU_APPR_MAX : AU_APPR_MAX1))) {
        // v0.9.57 : pas sur que ce soit UNE seule pub : pas appris, la captation sera gardee
        au_seg = false;
        au_non_s = au_non_s + d;
        char t[44], nb[12];
        snprintf(nb, sizeof nb, "%.0f", d);
        snprintf(t, sizeof t, secours ? "Non appris : %s s (sans silence)" : "Non appris : %s s (trop long)", nb);
        au_ev_ajouter(4, t, "");
        Console.printf("[PUB] Apprendre : %s\n", t);
        return;
    }
    if (!ok) { au_jeter("écriture SD impossible"); return; }
    if (au_perdu) { au_jeter("carte SD trop lente"); return; }
    if (au_connue) {
        au_effacer_tmp();
        au_connues = au_connues + 1;
        char t[44];
        snprintf(t, sizeof t, "Pub connue n°%d · %.0f s : ignorée", au_connue_num, d);
        au_ev_ajouter(3, t, "");
        Console.printf("[PUB] Auto : %s\n", t);
        return;
    }
    if (c_apprendre) {                                       // APPRENDRE : le detecteur l'apprend (resultat dans au_resultats)
        au_seg = false;
        if (au_plein) { au_jeter("bibliothèque pleine"); return; }
        uint32_t c0 = colonne_de(debut), c1 = colonne_de(fin);
        if (!det::commande(det::CMD_APPRENDRE_AUTO, (int)c0, (int)c1, c_chaine | (nets == 2 ? 256 : 0))) { au_jeter("détecteur occupé"); return; }
        au_attente++;
        return;
    }
    // morceau inconnu : on le garde, sous un nom parlant
    uint32_t h = plat_heure();
    uint32_t t = h ? h - (uint32_t)((r_ecrit - debut) / CAP_FS) : 0;
    char ch[16] = "ND";
    if (c_chaine > 0 && c_chaine < base::NB_CHAINES) {
        int k = 0;
        for (const char *p = base::CHAINES_DOSSIER[c_chaine]; *p && k < 12; p++) ch[k++] = (isalnum((unsigned char)*p) || *p == '-') ? *p : '_';
        ch[k] = 0;
    }
    au_n++;
    char nom[64];
    if (t) {
        time_t tt = (time_t)t;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(nom, sizeof nom, "pub_%04d%02d%02d_%02d%02d%02d_%s_%03d%s.wav", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ch, au_n, secours ? "_sans_silence" : "");
    } else snprintf(nom, sizeof nom, "pub_cap%03lu_%s_%03d%s.wav", (unsigned long)c_num, ch, au_n, secours ? "_sans_silence" : "");
    char chemin[96];
    snprintf(chemin, sizeof chemin, AUTO_DOSSIER "/%s", nom);
    uint8_t hd[44];
    entete_wav(hd, (fin - debut) * 2, CAP_FS);
    sd_prendre();
    au_f.seek(0); au_f.write(hd, 44); au_f.flush(); au_f.close();
    if (SD_MMC.exists(chemin)) SD_MMC.remove(chemin);
    bool renomme = SD_MMC.rename(AUTO_TMP, chemin);
    sd_rendre();
    au_seg = false;
    if (!renomme) { au_jeter("renommage impossible"); return; }
    AuFichier f;
    strncpy(f.nom, nom, sizeof f.nom - 1); f.nom[sizeof f.nom - 1] = 0;
    f.duree = d; f.heure = t;
    au_liste.push_back(f);
    au_gardes = au_gardes + 1;
    au_total_s = au_total_s + d;
    if (secours) au_secours = au_secours + 1;
    char tx[44], nb[12];
    snprintf(nb, sizeof nb, "%.1f", d); au_virgule(nb);
    snprintf(tx, sizeof tx, secours ? "Coupée sans silence · %s s" : "Pub enregistrée · %s s", nb);
    au_ev_ajouter(1, tx, nom);
    Console.printf("[PUB] Auto : %s : %s\n", tx, nom);
}

// v0.9.53 : resultats des apprentissages demandes par la captation APPRENDRE
static void au_resultats()
{
    det::ResAuto r;
    while (det::auto_resultat(au_res_lu, r)) {
        if (au_attente > 0) au_attente--;
        char t[44], nb[12];
        snprintf(nb, sizeof nb, "%.1f", r.duree); au_virgule(nb);
        switch (r.statut) {
        case det::AA_APPRISE:
            au_gardes = au_gardes + 1;
            au_total_s = au_total_s + r.duree;
            if (r.doute == 0) au_sures = au_sures + 1;       // v0.9.58 : sure -> OK tout de suite
            snprintf(t, sizeof t, r.doute == 0 ? "Apprise n°%d · %s s · sûre" : "Apprise n°%d · %s s · à vérifier", r.numero, nb);
            au_ev_ajouter(1, t, "");
            break;
        case det::AA_CONNUE:
            au_connues = au_connues + 1;
            if (r.doute == 1) au_sures = au_sures + 1;       // apprise « a verifier », entendue une 2e fois : validee
            snprintf(t, sizeof t, r.doute == 1 ? "Pub n°%d revue : validée" : "Pub connue n°%d · %s s : ignorée", r.numero, nb);
            au_ev_ajouter(3, t, "");
            break;
        case det::AA_PLEINE:
            au_plein = true;
            au_jetes = au_jetes + 1;
            snprintf(t, sizeof t, "Bibliothèque pleine : rien appris");
            au_ev_ajouter(4, t, "");
            break;
        default:
            au_jetes = au_jetes + 1;
            snprintf(t, sizeof t, "Morceau jeté : trop silencieux");
            au_ev_ajouter(4, t, "");
            break;
        }
        Console.printf("[PUB] Apprendre : %s\n", t);
    }
}

// v0.9.57 : fin d'une pub connue : comptee, et le morceau suivant commence juste apres elle
static void au_k_finir(uint32_t fin, bool suite = true)
{
    float d = (float)(int32_t)(fin - au_k_deb) / CAP_FS;
    if (d < 0) d = 0;
    au_k_actif = false;
    au_connues = au_connues + 1;
    au_connu_s = au_connu_s + d;
    char t[44];
    snprintf(t, sizeof t, "Pub connue n°%d · %.0f s : ignorée", au_k_num, d);
    au_ev_ajouter(3, t, "");
    Console.printf("[PUB] Auto : %s\n", t);
    au_fin_prec = fin;
    au_fin_propre = true;
    au_fin_net = false;
    if (suite && !au_seg) { au_ouvrir(fin, false); au_creux = false; }
}

// v0.9.57 : une pub connue vient d'etre reconnue, de kd a kf : on coupe autour d'elle
static void au_k_nouvelle(int num, uint32_t kd, uint32_t kf)
{
    au_k_actif = true; au_k_num = num; au_k_deb = kd; au_k_fin = kf;
    bool deja = false;
    for (int i = 0; i < au_k_n; i++) if (au_k_liste[i] == num) deja = true;
    if (!deja && au_k_n < AUTO_NB_CONNUES) { au_k_liste[au_k_n] = num; au_k_n = au_k_n + 1; }
    if (au_seg) {
        if ((int32_t)(kd - au_s0) >= (int32_t)AU_MIN) au_fermer(kd, false, false);   // la pub inconnue juste avant
        else { au_effacer_tmp(); au_connue = false; }                         // le morceau, c'est la pub connue
    } else if (au_fin_propre && (int32_t)(kd - au_fin_prec) >= (int32_t)AU_MIN && (int32_t)(kd - au_fin_prec) <= (int32_t)AU_MAX
               && (c_apprendre || (uint32_t)(r_ecrit - au_fin_prec) < ANNEAU_N - 8192)) {
        // morceau entre deux limites franches (pas de silence pour l'ouvrir) : rattrape
        float d = (float)(kd - au_fin_prec) / CAP_FS;
        au_non_s = au_non_s > d ? au_non_s - d : 0;
        au_ouvrir(au_fin_prec, au_fin_net);
        if (au_seg) au_fermer(kd, false, false);
    }
}

// appele a chaque tour de la tache archive pendant une captation automatique
static void au_pas()
{
    if (!au_buf) return;
    if (c_apprendre) au_resultats();
    uint32_t e = r_ecrit;
    if (e - au_ana > ANNEAU_N - 4096) {                      // l'analyse n'a pas suivi : on saute et on jette le morceau
        au_ana = e - (ANNEAU_N - 4096);
        if (au_seg) au_perdu = true;
    }
    // v0.9.57 : une pub reconnue = repere : on sait ou elle commence et finit (position et duree)
    {
        det::Vue v;
        det::vue(v);
        if (v.numero > 0 && v.age_match_s < 2.0f && v.duree_s >= 3 && v.pos_s > -1 && v.pos_s < v.duree_s + 3) {
            uint32_t m = ech_de_col(v.col);
            uint32_t kd = m - (uint32_t)(int32_t)(v.pos_s * CAP_FS) - AU_MARGE;
            uint32_t kf = kd + (uint32_t)(v.duree_s * CAP_FS) + 2 * AU_MARGE;
            if ((int32_t)(kd - c_lu_dem) < 0) kd = c_lu_dem;
            if (au_k_actif && v.numero == au_k_num && (int32_t)(kd - au_k_fin) < 0) {
                if ((int32_t)(kf - au_k_fin) > 0) au_k_fin = kf;          // meme passage : la fin peut s'affiner
            } else if ((int32_t)(kf - au_ana) > 0) {                       // nouveau passage (pas deja fini)
                if (au_k_actif) au_k_finir((int32_t)(kd - au_k_fin) < 0 ? kd : au_k_fin);
                au_k_nouvelle(v.numero, kd, kf);
            }
        } else if (au_seg && !au_connue && v.numero > 0 && v.age_match_s < 20.0f) {
            // reconnaissance sans position fiable : comme avant, le morceau est « connu »
            uint32_t recul = (uint32_t)((v.age_match_s + 0.3f) * CAP_FS);
            if ((int32_t)((e - recul) - (au_s0 + CAP_FS / 4)) >= 0) { au_connue = true; au_connue_num = v.numero; }
        }
    }
    while ((int32_t)(e - au_ana) >= (int32_t)AU_TRAME) {
        uint64_t s2 = 0;
        for (uint32_t i = 0; i < AU_TRAME; i++) { int32_t x = s_anneau[(au_ana + i) & (ANNEAU_N - 1)]; s2 += (uint64_t)(x * x); }
        float r = sqrtf((float)s2 / (float)AU_TRAME);
        uint32_t deb = au_ana;
        au_ana += AU_TRAME;
        if (au_moy < 1.0f) au_moy = r; else au_moy += (r - au_moy) * 0.0067f;     // moyenne sur ~3 s
        float ref = au_moy < AU_PLANCHER ? AU_PLANCHER : au_moy;
        if (au_k_actif && (int32_t)(au_ana - au_k_fin) >= 0) au_k_finir(au_k_fin);  // la pub connue est finie
        bool dans_k = au_k_actif;
        if (!au_seg && !dans_k && r >= AU_REL * ref) au_non_s = au_non_s + (float)AU_TRAME / CAP_FS;   // son hors morceau
        // AUTO : apres 40 s sans vrai silence, un creux doux suffit (fichier « _sans_silence »)
        if (r < AU_DOUX * ref) {
            if (au_dn == 0) au_ddeb = deb;
            if (++au_dn == AU_DOUX_N && !c_apprendre && au_seg && (int32_t)(au_ddeb - au_s0) >= (int32_t)AU_SECOURS) {
                uint32_t coupe = au_ddeb + AU_DOUX_N * AU_TRAME / 2;
                au_fermer(coupe, true);
                if (!au_seg && !au_k_actif) au_ouvrir(coupe, false);
            }
        } else au_dn = 0;
        if (r < AU_REL * ref) {
            if (au_creux_n == 0) au_creux_deb = deb;
            if (++au_creux_n == AU_CREUX_N) {
                au_creux = true;
                if (au_seg) {
                    if ((int32_t)(au_creux_deb - au_s0) >= (int32_t)AU_MIN) au_fermer(au_creux_deb + AU_MARGE);
                    else au_courte(au_creux_deb + AU_MARGE);
                }
            }
        } else {
            au_creux_n = 0;
            if (au_creux) {                                   // le son revient apres un vrai creux
                au_creux = false;
                if (!au_seg && !dans_k) {
                    uint32_t d0 = deb - AU_MARGE;
                    if ((int32_t)(d0 - au_fin_prec) < 0) d0 = au_fin_prec;
                    au_ouvrir(d0);
                }
            }
        }
        if (au_seg && au_ana - au_s0 > AU_MAX) au_jeter("plus de 90 s (programme ?)");
    }
    if (au_seg) {
        uint32_t limite = e - AU_RETENUE;
        if ((int32_t)(limite - au_ecr) > 0 && !au_ecrire_jusqua(limite)) au_jeter("écriture SD impossible");
    }
    au_seg_s = au_seg ? (float)(e - au_s0) / CAP_FS : 0;
}

// fin de la captation : annule (tout supprime) ou liste ecrite
static void au_terminer(bool annule)
{
    if (au_k_actif) au_k_finir((int32_t)(au_ana - au_k_fin) < 0 ? au_ana : au_k_fin, false);
    if (au_seg) au_jeter("dernier morceau incomplet");
    // v0.9.57 : filet APPRENDRE : du son n'a pas pu etre traite -> la captation entiere est gardee
    au_garde = c_apprendre && !annule && au_non_s >= AU_FILET_S;
    if (c_apprendre) {
        // les derniers apprentissages demandes : on attend leur resultat (1 s au plus)
        for (int i = 0; i < 50 && au_attente > 0; i++) { delay(20); au_resultats(); }
        au_resultats();
        if (annule) Console.printf("[PUB] Apprendre : captation annulée, les %d pub(s) déjà apprise(s) restent dans la bibliothèque\n", (int)au_gardes);
        Console.printf("[PUB] Apprendre : bilan : %d apprises, %d connues (%.0f s), %d trop courtes, %d jetées, %.0f s apprises, %.0f s non traitées%s\n",
                       (int)au_gardes, (int)au_connues, (float)au_connu_s, (int)au_courtes, (int)au_jetes, (float)au_total_s, (float)au_non_s,
                       au_garde ? " (captation gardée)" : "");
        au_bilan = true;                                     // le bilan reste affiche, meme apres ANNULER
        au_liste.clear();
        return;
    }
    if (annule) {
        sd_prendre();
        for (auto &f : au_liste) { char c[96]; snprintf(c, sizeof c, AUTO_DOSSIER "/%s", f.nom); SD_MMC.remove(c); }
        sd_rendre();
        au_gardes = 0; au_total_s = 0;
        au_bilan = false;
    } else if (au_liste.size()) {
        if (plat_fichier_taille(AUTO_INDEX) < 0) {
            const char *e = "fichier,date,duree_s,chaine\n";
            plat_fichier_ecrire(AUTO_INDEX, e, strlen(e));
        }
        for (auto &f : au_liste) {
            char date[24] = "inconnue";
            if (f.heure) {
                time_t tt = (time_t)f.heure;
                struct tm tm;
                localtime_r(&tt, &tm);
                strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
            }
            char l[140];
            int n = snprintf(l, sizeof l, "%s,%s,%.1f,%d\n", f.nom, date, f.duree, c_chaine);
            plat_fichier_ajouter(AUTO_INDEX, l, n);
        }
    }
    if (!annule)
        Console.printf("[PUB] Auto : bilan : %d enregistrées (%d sans silence), %d connues (%.0f s), %d trop courtes, %d jetées, %.0f s gardées, %.0f s non découpées\n",
                       (int)au_gardes, (int)au_secours, (int)au_connues, (float)au_connu_s, (int)au_courtes, (int)au_jetes, (float)au_total_s, (float)au_non_s);
    au_liste.clear();
}

const char *plat_captation_demarrer()
{
    if (!s_sd_ok) return "pas de carte SD";
    if (!s_anneau || !s_audio_ok) return "micro absent";
    if (!a_pret) return "carte SD pas encore prête";
    if (c_etat) return nullptr;
    c_stop = false; c_annule = false;
    if (!c_auto) au_bilan = false;
    {   // v0.9.5 : prelecture de 5 s (l'anneau garde 8 s) : le debut de la pub, avant le temps de reaction
        uint32_t e = r_ecrit, pre = (uint32_t)(5 * CAP_FS);
        c_lu_dem = e - (e < pre ? e : pre);
    }
    c_etat = 1;
    cpr::suspendue = true;
    c_demande = true;
    return nullptr;
}

const char *plat_captation_demarrer_auto()
{
    if (c_etat) return "une captation est déjà en cours";
    c_auto = true;
    c_apprendre = false;
    const char *e = plat_captation_demarrer();
    if (e) c_auto = false;
    return e;
}

const char *plat_captation_demarrer_apprendre()
{
    if (c_etat) return "une captation est déjà en cours";
    c_auto = true;
    c_apprendre = true;
    const char *e = plat_captation_demarrer();
    if (e) { c_auto = false; c_apprendre = false; }
    return e;
}

void plat_captation_arreter() { if (c_etat) c_stop = true; }
void plat_captation_annuler() { if (c_etat) { c_annule = true; c_stop = true; } }

void plat_captation(Captation &c)
{
    plat_verrou();
    c.etat = (c_etat == 2 && c_stop) ? 3 : c_etat;     // 3 : ARRETER touche, fin en cours
    c.numero = c_num;
    c.duree_s = (float)c_ech / CAP_FS;
    if (c_etat == 1) {   // fichier en preparation : le son est deja garde depuis le toucher, on l'affiche comme en cours
        c.etat = c_stop ? 3 : 2;
        c.duree_s = (float)(r_ecrit - c_lu_dem) / CAP_FS;
    }
    c.automatique = c_auto;
    c.apprendre = c_auto ? c_apprendre : au_appr_bilan;
    c.auto_bilan = au_bilan;
    c.auto_gardes = au_gardes;
    c.auto_connues = au_connues;
    c.auto_courtes = au_courtes;
    c.auto_jetes = au_jetes;
    c.auto_seg_s = au_seg_s;
    c.auto_total_s = au_total_s;
    c.auto_nev = au_nev;
    memcpy(c.auto_ev, au_ev, sizeof c.auto_ev);
    c.auto_connu_s = au_connu_s;
    c.auto_non_s = au_non_s;
    c.auto_secours = au_secours;
    c.auto_k_n = au_k_n;
    memcpy(c.auto_k, au_k_liste, sizeof c.auto_k);
    c.auto_garde = au_garde;
    c.auto_sures = au_sures;
    c.fins = c_fins;
    c.fin_ok = c_fin_ok;
    c.fin_duree_s = c_fin_duree;
    memcpy(c.fin_txt, c_fin_txt, sizeof c.fin_txt);
    plat_deverrou();
}

static void cap_fin(bool ok, const char *pourquoi)
{
    bool fichier = (bool)c_f;
    bool annule = c_annule && ok;                  // v0.9.10 : captation annulee : le fichier est supprime, rien n'est indexe
    char txt_auto[48];
    bool apprendre = c_auto && c_apprendre;
    if (c_auto) {                                  // v0.9.39 : captation automatique : morceaux gardes (ou supprimes si annulee)
        au_terminer(annule);
        if (apprendre && ok) {
            snprintf(txt_auto, sizeof txt_auto, "%s%d apprise(s), %d connue(s)%s", annule ? "annulée · " : "", (int)au_gardes, (int)au_connues,
                     au_garde ? " · gardée" : "");
            pourquoi = txt_auto;
        } else if (!annule && ok) {
            snprintf(txt_auto, sizeof txt_auto, "%s · %d enreg., %d connues", pourquoi, (int)au_gardes, (int)au_connues);
            pourquoi = txt_auto;
        }
        c_auto = false;
        c_apprendre = false;
    }
    if (apprendre && fichier && au_garde) {        // v0.9.57 : filet : du son n'a pas pu etre traite, on le garde
        annule = false;
        char t[44];
        snprintf(t, sizeof t, "Captation gardée : %.0f s non traitées", (float)au_non_s);
        au_ev_ajouter(4, t, "");
        Console.printf("[PUB] Apprendre : %s -> cap_%03lu.wav (Captations entières)\n", t, (unsigned long)c_num);
    } else if (apprendre && fichier) {             // v0.9.53 : APPRENDRE : le bandeau entier n'est pas garde
        char chemin[48];
        snprintf(chemin, sizeof chemin, CAP_DOSSIER "/cap_%03lu.wav", (unsigned long)c_num);
        sd_prendre(); c_f.close(); SD_MMC.remove(chemin); sd_rendre();
        if (c_num_max == c_num) c_num_max = c_num - 1;       // le numero est rendu
        fichier = false;
        annule = false;                            // les pubs apprises restent : ce n'est pas un echec
    }
    c_annule = false;
    if (fichier && annule) {
        char chemin[48];
        snprintf(chemin, sizeof chemin, CAP_DOSSIER "/cap_%03lu.wav", (unsigned long)c_num);
        sd_prendre(); c_f.close(); SD_MMC.remove(chemin); sd_rendre();
        if (c_num_max == c_num) c_num_max = c_num - 1;       // le numero est rendu
        fichier = false;
        ok = false;
        pourquoi = "annulée : fichier supprimé";
    } else if (fichier) {
        uint8_t h[44];
        entete_wav(h, c_ech * 2, CAP_FS);
        sd_prendre(); c_f.seek(0); c_f.write(h, 44); c_f.flush(); c_f.close(); sd_rendre();
    } else if (annule) {
        ok = false;
        pourquoi = "annulée";
    }
    float d = (float)c_ech / CAP_FS;
    // fin signalee tout de suite (l'ecran se libere) ; index.csv est ecrit apres
    plat_verrou();
    c_fin_ok = ok;
    c_fin_duree = d;
    strncpy(c_fin_txt, pourquoi, sizeof c_fin_txt - 1);
    c_fin_txt[sizeof c_fin_txt - 1] = 0;
    c_fins = c_fins + 1;
    c_etat = 0;
    cpr::suspendue = false;
    c_stop = false;
    plat_deverrou();
    if (fichier) {
        if (plat_fichier_taille(CAP_INDEX) < 0) {
            const char *e = "numero,date,duree_s,chaine\n";
            plat_fichier_ecrire(CAP_INDEX, e, strlen(e));
        }
        char date[24] = "inconnue";
        if (c_heure) {
            time_t tt = (time_t)c_heure;
            struct tm tm;
            localtime_r(&tt, &tm);
            strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
        }
        char l[80];
        int n = snprintf(l, sizeof l, "%lu,%s,%.1f,%d\n", (unsigned long)c_num, date, d, c_chaine);
        plat_fichier_ajouter(CAP_INDEX, l, n);
    }
    Console.printf("[PUB] Captation n°%lu : %s, %.0f s (%s)%s\n", (unsigned long)c_num, ok ? "terminée" : (annule ? "annulée" : "ECHEC"), d, pourquoi,
                   (c_perdus && !annule) ? " - son perdu par endroits (carte SD trop lente)" : "");
}

static void cap_debut()
{
    uint32_t t_prep = millis();
    c_demande = false;
    if (!c_buf) c_buf = (int16_t *)heap_caps_malloc(2048 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    // numero suivant : le plus grand cap_NNN deja sur la carte, + 1
    uint32_t mx = c_num_max;
    sd_prendre();
    uint32_t t_a = millis();                     // v0.9.11 : mesures de la preparation (attente SD / dossier / ouverture)
    if (!SD_MMC.exists(CAP_DOSSIER)) SD_MMC.mkdir(CAP_DOSSIER);
    fs::File d = c_num_ok ? fs::File() : SD_MMC.open(CAP_DOSSIER);
    if (d) {
        for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
            String n = f.name();
            if (!f.isDirectory() && n.startsWith("cap_")) {
                uint32_t k = (uint32_t)n.substring(4).toInt();
                if (k > mx) mx = k;
            }
        }
        d.close();
    }
    uint32_t t_b = millis();
    c_num = mx + 1;
    c_num_max = c_num; c_num_ok = true;
    c_ech = 0;
    char chemin[48];
    snprintf(chemin, sizeof chemin, CAP_DOSSIER "/cap_%03lu.wav", (unsigned long)c_num);
    c_f = c_buf ? SD_MMC.open(chemin, FILE_WRITE) : fs::File();
    if (c_f) {
        uint8_t h[44];
        entete_wav(h, 0, CAP_FS);
        c_f.write(h, 44);
    }
    sd_rendre();
    uint32_t t_c = millis();
    if (!c_f) { cap_fin(false, c_buf ? "fichier impossible à créer" : "mémoire PSRAM"); return; }
    det::Vue v;
    det::vue(v);
    c_chaine = (c_chaine_sel > 0 && c_chaine_sel < base::NB_CHAINES) ? c_chaine_sel : v.chaine;
    c_heure = plat_heure();
    c_ech_entete = 0;
    c_perdus = 0;
    c_lu = c_lu_dem;                                  // depuis le toucher : rien n'est perdu pendant la preparation
    c_etat = 2;
    cpr::suspendue = true;
    if (c_auto) au_debut();
    Console.printf("[PUB] Captation : fichier prêt en %lu ms (le son est gardé depuis le toucher)\n", (unsigned long)(millis() - t_prep));
    Console.printf("[PUB] Captation : préparation = attente SD %lu ms + dossier %lu ms + ouverture %lu ms\n", (unsigned long)(t_a - t_prep), (unsigned long)(t_b - t_a), (unsigned long)(t_c - t_b));
    Console.printf("[PUB] Captation n°%lu : %s (16 kHz mono, %d min au plus)\n", (unsigned long)c_num, chemin, CAPTATION_MAX_S / 60);
}

// copie dans le fichier ce que le micro a donne depuis le dernier passage
static void cap_ecrire()
{
    uint32_t e = r_ecrit;
    if (e - c_lu > ANNEAU_N - 4096) { c_perdus += e - c_lu - (ANNEAU_N - 4096); c_lu = e - (ANNEAU_N - 4096); }
    while (c_lu != e) {
        uint32_t n = e - c_lu;
        if (n > 2048) n = 2048;
        uint32_t reste = CAP_MAX_ECH - c_ech;
        if (n > reste) n = reste;
        if (!n) break;
        for (uint32_t i = 0; i < n; i++) c_buf[i] = s_anneau[(c_lu + i) & (ANNEAU_N - 1)];
        sd_prendre();
        size_t w = ecrire_morceaux(c_f, (const uint8_t *)c_buf, n * 2);
        sd_rendre();
        c_lu += n;
        c_ech = c_ech + (uint32_t)(w / 2);
        if (w != n * 2) { cap_fin(false, "écriture SD impossible (carte pleine ?)"); return; }
    }
    if (c_ech - c_ech_entete >= (uint32_t)CAP_FS * 30) {
        uint8_t h[44];
        entete_wav(h, c_ech * 2, CAP_FS);
        sd_prendre(); c_f.seek(0); c_f.write(h, 44); c_f.seek(44 + c_ech * 2); c_f.flush(); sd_rendre();
        c_ech_entete = c_ech;
    }
    if (c_ech >= CAP_MAX_ECH) cap_fin(true, "15 min atteintes");
}

// ---- journal des coupures (v0.7.0) : le moteur depose, la tache archive ecrit sur la SD ----
#define COUP_JOURNAL "/pub/coupures.csv"
struct LigneCoupure { uint32_t heure; int ev, numero, score, chaine, duree; };
static LigneCoupure cj_anneau[16];
static volatile uint8_t cj_tete = 0, cj_queue = 0;

void plat_coupure_log(int evenement, int numero, int score, int chaine, int duree_s)
{
    uint8_t t = cj_tete, s = (t + 1) & 15;
    if (s == cj_queue) return;                         // plein (SD tres lente) : on perd la ligne
    cj_anneau[t] = LigneCoupure{ plat_heure(), evenement, numero, score, chaine, duree_s };
    cj_tete = s;
}

static void cj_ecrire()
{
    while (cj_queue != cj_tete) {
        const LigneCoupure l = cj_anneau[cj_queue];
        cj_queue = (cj_queue + 1) & 15;
        if (plat_fichier_taille(COUP_JOURNAL) < 0) {
            const char *e = "date,heure,evenement,numero,score,chaine,duree_s\n";
            plat_fichier_ecrire(COUP_JOURNAL, e, strlen(e));
        } else if (plat_fichier_taille(COUP_JOURNAL) > 300 * 1024) {      // 300 Ko : on garde l'ancien a cote
            sd_prendre(); SD_MMC.remove("/pub/coupures_ancien.csv"); SD_MMC.rename(COUP_JOURNAL, "/pub/coupures_ancien.csv"); sd_rendre();
            const char *e = "date,heure,evenement,numero,score,chaine,duree_s\n";
            plat_fichier_ecrire(COUP_JOURNAL, e, strlen(e));
        }
        char d[24] = "", h[16] = "";
        if (l.heure) {
            time_t tt = (time_t)l.heure;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            strftime(d, sizeof d, "%Y-%m-%d", &tmv);
            strftime(h, sizeof h, "%H:%M:%S", &tmv);
        }
        char ligne[96];
        int n = snprintf(ligne, sizeof ligne, "%s,%s,%s,%d,%d,%d,%d\n", d, h, l.ev ? "RETOUR" : "COUPE", l.numero, l.score, l.chaine, l.duree);
        plat_fichier_ajouter(COUP_JOURNAL, ligne, n);
    }
}

// =========================================================================
// v0.9.58 : fichiers AUTO traites depuis le portail (▶ / ✓ / ✗ et « Dans la base ? »)
// La tache des fichiers lit le WAV sur la carte SD, calcule ses empreintes avec le moteur
// (le direct attend 1 a 2 s sous plat_emp_prendre, son etat est remis exactement : rien
// n'est perdu, la detection reprend avec 1 a 2 s de retard), puis le detecteur compare et,
// si demande, ajoute la pub (validee). Le son de la pub (8 kHz) est ecrit ensuite et le
// fichier AUTO efface. Jamais pendant une captation.
// =========================================================================
static SemaphoreHandle_t s_emp_mx = nullptr;
void plat_emp_prendre() { if (s_emp_mx) xSemaphoreTake(s_emp_mx, portMAX_DELAY); }
void plat_emp_rendre() { if (s_emp_mx) xSemaphoreGive(s_emp_mx); }

enum { FE_RIEN = 0, FE_FILE, FE_COURS, FE_NOUVELLE, FE_RESSEMBLE, FE_CONNUE, FE_AJOUTEE, FE_ERREUR, FE_PLEINE, FE_VIDE };
struct FiEntree { char nom[64]; int8_t etat; uint8_t ajouter; int16_t numero; int8_t pct; };
static const int FI_MAX = 200;
static FiEntree *fi_tab = nullptr;                            // PSRAM
static volatile int fi_nb = 0;
static emp::Code *fi_codes = nullptr;                         // PSRAM
static const int FI_CODES = 32768;
static const float FI_MAX_S = 95.0f;                          // fichiers AUTO : 90 s au plus (+ marges)

static int fi_trouver(const char *nom)
{
    for (int i = 0; i < fi_nb; i++) if (!strcmp(fi_tab[i].nom, nom)) return i;
    return -1;
}

// met un fichier en file (ajouter : l'apprendre s'il n'est pas connu) ; false si refuse
bool plat_fichiers_demander(const char *nom, bool ajouter)
{
    if (!fi_tab || !nom || !*nom || strchr(nom, '/') || strlen(nom) >= sizeof fi_tab[0].nom) return false;
    plat_verrou();
    int i = fi_trouver(nom);
    if (i < 0 && fi_nb < FI_MAX) { i = fi_nb; memset(&fi_tab[i], 0, sizeof fi_tab[i]); strcpy(fi_tab[i].nom, nom); fi_nb = fi_nb + 1; }
    bool ok = i >= 0 && fi_tab[i].etat != FE_COURS && fi_tab[i].etat != FE_AJOUTEE;
    if (ok) { fi_tab[i].etat = FE_FILE; fi_tab[i].ajouter = ajouter ? 1 : 0; }
    plat_deverrou();
    return ok;
}

// tous les fichiers AUTO pas encore compares ; nouvelles = ajoute ceux trouves « nouvelle »
int plat_fichiers_tous(bool nouvelles)
{
    if (!fi_tab) return 0;
    int n = 0;
    if (nouvelles) {
        for (int i = 0; i < fi_nb; i++) if (fi_tab[i].etat == FE_NOUVELLE && plat_fichiers_demander(fi_tab[i].nom, true)) n++;
        return n;
    }
    std::vector<String> noms;
    sd_prendre();
    fs::File d = SD_MMC.open(AUTO_DOSSIER);
    if (d) for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
        String nm = String(f.name());
        int k = nm.lastIndexOf('/');
        if (k >= 0) nm = nm.substring(k + 1);
        if (!f.isDirectory() && nm.endsWith(".wav") && !nm.startsWith("_")) noms.push_back(nm);
    }
    sd_rendre();
    for (auto &nm : noms) {
        int i = fi_trouver(nm.c_str());
        if (i < 0 || fi_tab[i].etat == FE_RIEN || fi_tab[i].etat == FE_ERREUR)
            if (plat_fichiers_demander(nm.c_str(), false)) n++;
    }
    return n;
}

// fichiers « deja dans la base » (sur) : effaces de la carte
int plat_fichiers_suppr_connus()
{
    if (!fi_tab) return 0;
    int n = 0;
    for (int i = 0; i < fi_nb; i++) {
        if (fi_tab[i].etat != FE_CONNUE) continue;
        char c[96];
        snprintf(c, sizeof c, AUTO_DOSSIER "/%s", fi_tab[i].nom);
        sd_prendre(); bool ok = SD_MMC.remove(c); sd_rendre();
        if (ok) { plat_verrou(); fi_tab[i].etat = FE_RIEN; fi_tab[i].nom[0] = 0; plat_deverrou(); n++; }
    }
    return n;
}

void plat_fichiers_oublier(const char *nom)
{
    if (!fi_tab) return;
    plat_verrou();
    int i = fi_trouver(nom);
    if (i >= 0 && fi_tab[i].etat != FE_COURS) { fi_tab[i].etat = FE_RIEN; fi_tab[i].nom[0] = 0; }
    plat_deverrou();
}

// etat de chaque fichier connu, pour le portail : {"cours":nom,"file":n,"l":[{"n":..,"e":..,"u":..,"p":..}]}
String plat_fichiers_json()
{
    // copie sous verrou (section critique : aucune allocation dedans), texte construit apres
    static FiEntree *copie = (FiEntree *)heap_caps_malloc(FI_MAX * sizeof(FiEntree), MALLOC_CAP_SPIRAM);
    int nb = 0;
    if (fi_tab && copie) {
        plat_verrou();
        nb = fi_nb;
        memcpy(copie, fi_tab, nb * sizeof(FiEntree));
        plat_deverrou();
    }
    String r = "{\"l\":[";
    int file = 0;
    bool premier = true;
    for (int i = 0; i < nb; i++) {
        const FiEntree &e = copie[i];
        if (!e.nom[0] || e.etat == FE_RIEN) continue;
        if (e.etat == FE_FILE || e.etat == FE_COURS) file++;
        if (!premier) r += ',';
        premier = false;
        r += "{\"n\":\""; r += e.nom; r += "\",\"e\":"; r += String((int)e.etat);
        r += ",\"u\":"; r += String((int)e.numero); r += ",\"p\":"; r += String((int)e.pct); r += '}';
    }
    r += "],\"file\":"; r += String(file); r += ",\"cap\":"; r += String(c_etat ? 1 : 0); r += '}';
    return r;
}

// chaine notee par la captation pour ce fichier (index.csv du dossier auto), 0 si inconnue
static int fi_chaine(const char *nom)
{
    static char *buf = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    if (!buf) return 0;
    long n = plat_fichier_lire(AUTO_INDEX, buf, 32767);
    if (n <= 0) return 0;
    buf[n] = 0;
    size_t lg = strlen(nom);
    for (char *l = buf; l && *l;) {
        char *fin = strchr(l, '\n');
        if (!strncmp(l, nom, lg) && l[lg] == ',') {
            // fichier,date,duree_s,chaine
            char *v = l;
            for (int k = 0; k < 3 && v; k++) { v = strchr(v, ','); if (v) v++; }
            return v ? atoi(v) : 0;
        }
        l = fin ? fin + 1 : nullptr;
    }
    return 0;
}

// lit l'entete WAV (PCM 16 bits mono) : position et taille des donnees ; false si autre format
static bool fi_entete(fs::File &f, uint32_t &deb, uint32_t &taille, uint32_t &freq)
{
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    uint16_t canaux = 0, bits = 0;
    freq = 0;
    while (f.available() >= 8) {
        uint8_t c[8];
        f.read(c, 8);
        uint32_t lg = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (lg < 16 || f.read(fm, 16) != 16) return false;
            canaux = fm[2] | (fm[3] << 8);
            freq = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
            bits = fm[14] | (fm[15] << 8);
            if (lg > 16) f.seek(f.position() + lg - 16);
        } else if (!memcmp(c, "data", 4)) {
            deb = f.position();
            taille = lg;
            if (deb + taille > f.size()) taille = f.size() - deb;
            return canaux == 1 && bits == 16 && freq == 16000;
        } else f.seek(f.position() + lg + (lg & 1));
    }
    return false;
}

// lit et donne au moteur les echantillons [deb, deb+octets) ; codes ajoutes a fi_codes[*n]
static void fi_alimenter(fs::File &f, uint32_t deb, uint32_t octets, int *n)
{
    static int16_t *x = (int16_t *)heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    static emp::Code *tmp = (emp::Code *)heap_caps_malloc(1024 * sizeof(emp::Code), MALLOC_CAP_SPIRAM);
    if (!x || !tmp) return;
    sd_prendre(); f.seek(deb); sd_rendre();
    for (uint32_t lu = 0; lu < octets;) {
        uint32_t k = octets - lu;
        if (k > 8192) k = 8192;
        sd_prendre(); int r = f.read((uint8_t *)x, k); sd_rendre();
        if (r <= 1) break;
        lu += r;
        int m = emp::ajouter(x, r / 2, tmp, 1024);
        for (int i = 0; i < m && n && *n < FI_CODES; i++) fi_codes[(*n)++] = tmp[i];
    }
}

// son de la pub (8 kHz, comme ceux appris en direct) depuis le fichier 16 kHz
static bool fi_ecrire_son(fs::File &f, uint32_t deb, uint32_t octets, int numero)
{
    static int16_t *x = (int16_t *)heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    if (!x) return false;
    char chemin[40];
    snprintf(chemin, sizeof chemin, "/pub/audio/%d.wav", numero);
    uint32_t ech8 = octets / 4;
    uint8_t h[44];
    entete_wav(h, ech8 * 2);
    sd_prendre();
    if (!SD_MMC.exists("/pub/audio")) SD_MMC.mkdir("/pub/audio");
    fs::File w = SD_MMC.open(chemin, FILE_WRITE);
    bool ok = w && w.write(h, 44) == 44;
    f.seek(deb);
    sd_rendre();
    for (uint32_t lu = 0; ok && lu + 4 <= octets;) {
        uint32_t k = octets - lu;
        if (k > 8192) k = 8192;
        k &= ~3u;
        sd_prendre(); int r = f.read((uint8_t *)x, k); sd_rendre();
        if (r < 4) break;
        lu += r;
        int m = r / 4;
        for (int i = 0; i < m; i++) x[i] = (int16_t)(((int32_t)x[2 * i] + x[2 * i + 1]) / 2);   // 16 -> 8 kHz
        sd_prendre(); ok = w.write((uint8_t *)x, m * 2) == (size_t)(m * 2); sd_rendre();
    }
    sd_prendre(); if (w) w.close(); sd_rendre();
    return ok;
}

static void fi_traiter(int i)
{
    char nom[64], chemin[96];
    strcpy(nom, fi_tab[i].nom);
    bool ajouter = fi_tab[i].ajouter;
    snprintf(chemin, sizeof chemin, AUTO_DOSSIER "/%s", nom);
    auto fin = [&](int etat, int numero, int pct) {
        plat_verrou();
        int j = fi_trouver(nom);
        if (j >= 0) { fi_tab[j].etat = (int8_t)etat; fi_tab[j].numero = (int16_t)numero; fi_tab[j].pct = (int8_t)pct; }
        plat_deverrou();
    };
    sd_prendre(); fs::File f = SD_MMC.open(chemin, FILE_READ); sd_rendre();
    uint32_t deb = 0, taille = 0, freq = 0;
    sd_prendre(); bool ok = f && fi_entete(f, deb, taille, freq); sd_rendre();
    float duree = taille / 2.0f / 16000.0f;
    if (!ok || duree < 3 || duree > FI_MAX_S) {
        Console.printf("[PUB] Portail : %s : %s\n", nom, !f ? "introuvable" : (!ok ? "format non lu (WAV 16 kHz mono attendu)" : "durée hors limites"));
        sd_prendre(); if (f) f.close(); sd_rendre();
        fin(FE_ERREUR, 0, 0);
        return;
    }
    // empreintes : le direct attend, son etat est remis exactement ensuite
    int n = 0;
    uint32_t t0 = 0;
    uint32_t ms = millis();
    plat_emp_prendre();
    if (emp::hors_ligne_debut()) {
        uint32_t chauffe = taille < 2u * 2 * 16000 ? taille : 2u * 2 * 16000;   // 2 s de mise en route
        fi_alimenter(f, deb, chauffe, nullptr);
        emp::hors_ligne_repartir();
        t0 = emp::colonne();
        fi_alimenter(f, deb, taille, &n);
        emp::hors_ligne_fin();
    }
    plat_emp_rendre();
    uint32_t ms_emp = millis() - ms;
    // le detecteur compare (et ajoute si demande)
    det::ResFichier r = { 0, det::FI_VIDE, 0, 0 };
    int chaine = ajouter ? fi_chaine(nom) : 0;
    det::fichier_poser(fi_codes, n, t0, duree);
    uint32_t seq = det::fichier_seq();
    bool rep = false;
    if (det::commande(det::CMD_FICHIER, ajouter ? 1 : 0, chaine))
        for (int k = 0; k < 250 && !(rep = det::fichier_resultat(seq, r)); k++) vTaskDelay(pdMS_TO_TICKS(20));
    det::fichier_poser(nullptr, 0, 0, 0);
    if (!rep) { sd_prendre(); f.close(); sd_rendre(); fin(FE_ERREUR, 0, 0); return; }
    int etat = r.statut == det::FI_NOUVELLE ? FE_NOUVELLE : r.statut == det::FI_RESSEMBLE ? FE_RESSEMBLE
             : r.statut == det::FI_CONNUE ? FE_CONNUE : r.statut == det::FI_AJOUTEE ? FE_AJOUTEE
             : r.statut == det::FI_PLEINE ? FE_PLEINE : FE_VIDE;
    if (etat == FE_AJOUTEE) {
        bool son = fi_ecrire_son(f, deb, taille, r.numero);
        sd_prendre(); f.close(); SD_MMC.remove(chemin); sd_rendre();   // la pub est dans la bibliotheque : le fichier ne sert plus
        Console.printf("[PUB] Portail : %s -> pub n°%d (%.0f s, %d codes, empreintes en %lu ms)%s\n", nom, r.numero, duree, n,
                       (unsigned long)ms_emp, son ? "" : " - son NON écrit");
    } else {
        sd_prendre(); f.close(); sd_rendre();
        Console.printf("[PUB] Portail : %s : %s%s (%d codes, %lu ms)\n", nom,
                       etat == FE_CONNUE ? "déjà dans la base, pub n°" : etat == FE_RESSEMBLE ? "ressemble à la pub n°" : etat == FE_NOUVELLE ? "nouvelle" : "rien à comparer",
                       (etat == FE_CONNUE || etat == FE_RESSEMBLE) ? String(r.numero).c_str() : "", n, (unsigned long)ms_emp);
    }
    fin(etat, r.numero, r.pct);
}

static void tache_fichiers(void *)
{
    for (;;) {
        int i = -1;
        if (!c_etat && fi_tab) {                              // jamais pendant une captation
            plat_verrou();
            for (int k = 0; k < fi_nb && i < 0; k++) if (fi_tab[k].nom[0] && fi_tab[k].etat == FE_FILE) i = k;
            if (i >= 0) fi_tab[i].etat = FE_COURS;
            plat_deverrou();
        }
        if (i >= 0) { fi_traiter(i); vTaskDelay(pdMS_TO_TICKS(300)); }   // le direct rattrape entre deux fichiers
        else vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void tache_archive(void *arg)
{
    // fichier tournant cree une fois (3,4 Mo), puis garde ouvert en lecture-ecriture
    sd_prendre();
    bool pret = SD_MMC.exists(ARCH_FICHIER);
    if (pret) { fs::File f = SD_MMC.open(ARCH_FICHIER, FILE_READ); pret = f && f.size() >= ARCH_TAILLE && f.size() < ARCH_TAILLE + 8192; if (f) f.close(); }
    sd_rendre();
    if (!pret) {
        Console.println("[PUB] Archive audio : création de /pub/archive.ulaw (5,8 Mo, une seule fois, ~1 min)");
        uint8_t *zero = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (zero) memset(zero, 0xFF, 4096);            // 0xFF = silence en loi mu
        sd_prendre(); fs::File f = SD_MMC.open(ARCH_FICHIER, FILE_WRITE); sd_rendre();
        // exactement ARCH_TAILLE octets (avant : 821 x 4096, un peu plus, et le
        // fichier etait juge "pas pret" et recree a chaque demarrage)
        for (uint32_t k = 0; f && zero && k < ARCH_TAILLE; k += 4096) {
            uint32_t n = ARCH_TAILLE - k < 4096 ? ARCH_TAILLE - k : 4096;
            sd_prendre(); f.write(zero, n); sd_rendre();
            vTaskDelay(1);
        }
        sd_prendre(); if (f) f.close(); sd_rendre();
        if (zero) heap_caps_free(zero);
    }
    sd_prendre(); fs::File arch = SD_MMC.open(ARCH_FICHIER, "r+"); sd_rendre();
    if (!arch) { Console.println("[PUB] Archive audio impossible : les pubs apprises n'auront pas de son"); vTaskDelete(NULL); }
    a_vide = a_ecrit;                                  // on part de maintenant
    a_pret = true;
    Console.println("[PUB] Archive audio : prête (les 12 dernières minutes, pour le son des pubs)");
    for (;;) {
        // captation d'abord : ARRETER est traite avant l'archive et les sons des pubs
        cj_ecrire();
        if (c_demande) cap_debut();
        if (c_etat == 2) {
            cap_ecrire();
            if (c_etat == 2 && c_auto) au_pas();
            if (c_etat == 2 && c_stop) cap_fin(true, "arrêtée");
        }
        vider(arch);
        ReqAudio q;
        while (xQueueReceive(a_file, &q, 0) == pdTRUE) {
            if (q.type == 1) extraire_wav(arch, q.a, q.b, q.c);
            else if (q.type == 2) couper_wav(q.a, (int)q.b, (int)q.c);
            else if (q.type == 3 && q.a == AUDIO_TOUS) {
                // "Tout effacer" : plus aucune pub, donc plus aucun son a garder
                std::vector<String> noms;
                sd_prendre();
                fs::File d = SD_MMC.open("/pub/audio");
                if (d) for (fs::File f = d.openNextFile(); f; f = d.openNextFile())
                    if (!f.isDirectory()) noms.push_back(String(f.path()));
                for (auto &n : noms) SD_MMC.remove(n);
                sd_rendre();
                Console.printf("[PUB] Tout effacer : %u sons de pubs supprimes\n", (unsigned)noms.size());
            } else if (q.type == 4) {
                // sons qui ne correspondent a aucune pub de la bibliotheque
                std::vector<String> noms;
                uint32_t octets = 0;
                sd_prendre();
                fs::File d = SD_MMC.open("/pub/audio");
                if (d) for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
                    if (f.isDirectory()) continue;
                    String nom = f.name();
                    long num = nom.toInt();
                    bool garder = false;
                    if (num > 0 && nom.endsWith(".wav"))
                        for (int i = 0; i < base::MAX_BLOCS && !garder; i++) garder = base::bloc(i).actif && (long)base::bloc(i).numero == num;
                    if (!garder) { noms.push_back(String(f.path())); octets += f.size(); }
                }
                for (auto &n : noms) SD_MMC.remove(n);
                sd_rendre();
                if (noms.size()) Console.printf("[PUB] Nettoyage au demarrage : %u son(s) sans pub supprime(s) (%lu Ko)\n", (unsigned)noms.size(), (unsigned long)(octets / 1024));
            } else if (q.type == 3) {
                char c[40];
                snprintf(c, sizeof c, "/pub/audio/%lu.wav", (unsigned long)q.a);
                sd_prendre(); if (SD_MMC.exists(c)) SD_MMC.remove(c); sd_rendre();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(c_etat || c_demande ? 50 : 250));   // captation : reagit en 50 ms
    }
}

bool plat_nvs_lire(const char *cle, void *buf, size_t n)
{
    if (s_prefs.getBytesLength(cle) != n) return false;
    return s_prefs.getBytes(cle, buf, n) == n;
}

// Ecritures NVS : faites par une petite tache a pile INTERNE. Une ecriture en
// flash coupe le cache ; une tache dont la pile est en PSRAM (moteur) ne doit
// donc jamais ecrire elle-meme. Les autres taches deposent une copie ici.
struct EcritureNvs { char cle[16]; void *data; size_t n; };
static QueueHandle_t s_nvs_file = NULL;
static volatile int s_nvs_en_cours = 0;
static void tache_nvs(void *)
{
    EcritureNvs e;
    for (;;) {
        if (xQueueReceive(s_nvs_file, &e, portMAX_DELAY) != pdTRUE) continue;
        s_prefs.putBytes(e.cle, e.data, e.n);
        free(e.data);
        s_nvs_en_cours = s_nvs_en_cours - 1;
    }
}
void plat_nvs_ecrire(const char *cle, const void *buf, size_t n)
{
    if (!s_nvs_file) { s_prefs.putBytes(cle, buf, n); return; }   // au demarrage (setup, pile interne)
    EcritureNvs e = {};
    strncpy(e.cle, cle, sizeof e.cle - 1);
    e.data = malloc(n);
    if (!e.data) return;
    memcpy(e.data, buf, n);
    e.n = n;
    s_nvs_en_cours = s_nvs_en_cours + 1;
    if (xQueueSend(s_nvs_file, &e, pdMS_TO_TICKS(500)) != pdTRUE) { free(e.data); s_nvs_en_cours = s_nvs_en_cours - 1; }
}
int plat_sd_tampon_actif() { return (int)(s_sd_tampon / 1024); }      // 0 : pas encore alloue (aucune ecriture depuis le demarrage)
int plat_sd_tampon_choisi()
{
    int ko = 16;
    if (!plat_nvs_lire("sdbuf", &ko, sizeof ko) || !(ko == 4 || ko == 8 || ko == 16)) ko = 16;
    return ko;
}
bool plat_sd_tampon_regler(int ko)
{
    if (!(ko == 4 || ko == 8 || ko == 16)) return false;
    plat_nvs_ecrire("sdbuf", &ko, sizeof ko);
    return true;
}
int plat_captation_chaine()
{
    if (!c_chaine_lue) {
        c_chaine_lue = true;
        int v = 0;
        if (plat_nvs_lire("capch", &v, sizeof v) && v >= 0 && v < base::NB_CHAINES) c_chaine_sel = v;
    }
    return c_chaine_sel;
}
void plat_captation_chaine_regler(int c)
{
    if (c < 0 || c >= base::NB_CHAINES) c = 0;
    c_chaine_lue = true;
    c_chaine_sel = c;
    plat_nvs_ecrire("capch", &c, sizeof c);
}
void plat_nvs_vider()
{
    for (int i = 0; i < 100 && s_nvs_en_cours > 0; i++) delay(20);
}

// =========================================================================
// Codec ES8311 (I2C) : esclave, MCLK = 256 x 48 kHz, I2S 16 bits
// (meme sequence que la console "vitesse du son", ici seul le micro sert)
// =========================================================================
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_codec = NULL;
static int s_gain = 3;

static bool es_ecrire(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_codec, b, 2, 50) == ESP_OK;
}

static int es_lire(uint8_t reg)
{
    uint8_t v = 0;
    if (i2c_master_transmit_receive(s_codec, &reg, 1, &v, 1, 50) != ESP_OK) return -1;
    return v;
}

// gain total = gain analogique du micro (PGA, 3 dB par pas, +30 dB au plus)
//            + volume numerique de l'ADC (0,5 dB par pas : 0xBF = 0 dB, +12 = +6 dB)
void plat_audio_gain(int niveau)
{
    static const uint8_t PGA[6] = { 0x14, 0x17, 0x1A, 0x1A, 0x1A, 0x1A };   // +12 / +21 / +30 dB analogiques
    static const uint8_t ADC[6] = { 0xBF, 0xBF, 0xBF, 0xCB, 0xD7, 0xE3 };   // +0 / +6 / +12 / +18 dB numeriques
    s_gain = niveau < 0 ? 0 : (niveau > 5 ? 5 : niveau);
    if (!s_codec) return;
    es_ecrire(0x14, PGA[s_gain]);
    es_ecrire(0x17, ADC[s_gain]);
}

static bool init_codec()
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = ES8311_ADR;
    cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(s_bus, &cfg, &s_codec) != ESP_OK) return false;
    int id1 = es_lire(0xFD), id2 = es_lire(0xFE);
    Console.printf("[PUB] ES8311 identifiant %02X %02X\n", id1, id2);
    if (id1 < 0) return false;

    es_ecrire(0x00, 0x1F); delay(20); es_ecrire(0x00, 0x00);
    es_ecrire(0x45, 0x00);
    es_ecrire(0x01, 0x30); es_ecrire(0x02, 0x00); es_ecrire(0x03, 0x10);
    es_ecrire(0x16, 0x24); es_ecrire(0x04, 0x10); es_ecrire(0x05, 0x00);
    es_ecrire(0x0B, 0x00); es_ecrire(0x0C, 0x00); es_ecrire(0x10, 0x1F); es_ecrire(0x11, 0x7F);
    es_ecrire(0x00, 0x80);                          // esclave, mise en route
    es_ecrire(0x01, 0x3F);                          // MCLK depuis la broche, toutes horloges actives
    es_ecrire(0x02, 0x00); es_ecrire(0x05, 0x00);
    es_ecrire(0x03, 0x10); es_ecrire(0x04, 0x10);
    es_ecrire(0x07, 0x00); es_ecrire(0x08, 0xFF); es_ecrire(0x06, 0x03);
    es_ecrire(0x09, 0x0C); es_ecrire(0x0A, 0x0C);   // I2S 16 bits
    es_ecrire(0x13, 0x10);
    es_ecrire(0x1B, 0x0A); es_ecrire(0x1C, 0x6A);  // filtre passe-haut de l'ADC
    es_ecrire(0x17, 0xBF);                          // volume numerique ADC 0 dB
    es_ecrire(0x0E, 0x02);
    es_ecrire(0x12, 0x00);
    es_ecrire(0x0D, 0x01);
    es_ecrire(0x15, 0x40);
    es_ecrire(0x37, 0x08);
    es_ecrire(0x45, 0x00);
    es_ecrire(0x32, 0xB6);                          // DAC -4,5 dB : petit jingle sur le haut-parleur
    plat_audio_gain(s_gain);
    return true;
}

// =========================================================================
// I2S en continu (coeur 0, haute priorite) : lit le micro a 48 kHz, filtre
// et garde un echantillon sur trois -> 16 kHz dans un anneau en PSRAM
// (s_anneau, r_ecrit : declares en haut, la captation les lit aussi).
// =========================================================================
static i2s_chan_handle_t s_tx = NULL, s_rx = NULL;
static uint32_t r_lu = 0;

// v0.9.53 : echantillon de l'anneau (16 kHz) -> colonne des empreintes (31,25/s, une
// colonne = 512 echantillons de l'anneau). Le moteur lit l'anneau en r_lu et sa
// colonne courante correspond a ce point (a ~2 colonnes pres : 64 ms, sous les
// 150 ms de marge gardees de chaque cote d'un morceau).
static uint32_t colonne_de(uint32_t ech)
{
    uint32_t lu = *(volatile uint32_t *)&r_lu;
    int32_t col = (int32_t)emp::colonne();
    int32_t c = col - (int32_t)(lu - ech) / 512;
    return c < 0 ? 0 : (uint32_t)c;
}
// v0.9.57 : inverse de colonne_de (repere d'une pub reconnue -> position dans l'anneau)
static uint32_t ech_de_col(uint32_t col)
{
    uint32_t lu = *(volatile uint32_t *)&r_lu;
    int32_t maintenant = (int32_t)emp::colonne();
    return lu - (uint32_t)((maintenant - (int32_t)col) * 512);
}
static volatile float s_rms_test = 0;

static const int NFD = 33;                      // passe-bas 7 kHz pour la decimation par 3
static float s_coef[NFD];

static void init_filtre()
{
    const double fc = 7000.0 / FS_I2S;
    double s = 0;
    for (int i = 0; i < NFD; i++) {
        double m = i - (NFD - 1) / 2.0;
        double h = m == 0 ? 2 * fc : sin(2 * PI * fc * m) / (PI * m);
        h *= 0.54 - 0.46 * cos(2 * PI * i / (NFD - 1));
        s_coef[i] = (float)h;
        s += h;
    }
    for (int i = 0; i < NFD; i++) s_coef[i] = (float)(s_coef[i] / s);
}

static bool init_i2s()
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = CH;
    cc.auto_clear = true;                           // silence en sortie
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) return false;
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FS_I2S);    // MCLK = 256 x 48 kHz
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
    sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
    sc.gpio_cfg.ws = (gpio_num_t)I2S_WS;
    sc.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
    sc.gpio_cfg.din = (gpio_num_t)I2S_DIN;
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) return false;
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    return true;
}

// =========================================================================
// TEST DE L'ENTREE OPTIQUE (v0.6.4) : module TOSLINKBee sur le connecteur P3.
// L'ESP32 est MAITRE I2S sur le port 1 (le module est esclave, sans MCLK) :
// il fournit BCK et LRCK, et lit les donnees. Aucun lien avec le micro (port 0).
// 10 s sont gardees en PSRAM (pas d'ecriture SD pendant la lecture : une
// attente de la carte ferait des trous), puis ecrites dans un WAV 16 kHz a ecouter (v0.9.59).
// =========================================================================
// ES3C35P : les GPIO 21/14/2 de l'ES3C28P sont pris (I2S du codec, ecran, SD).
// Seules broches libres : GPIO45 et GPIO46 (connecteur d'extension 4P) et les
// broches de l'UART0 (connecteur serie 4P), libres puisque la console passe par
// l'USB. GPIO45/46 sont des broches de configuration au demarrage : ici ce sont
// des SORTIES de l'ESP32 vers le module (aucun risque tant que le module ne les
// force pas au demarrage).
// ATTENTION : la notice LCDwiki inverse RX et TX ; dans la puce, GPIO44 = RX0.
#define FO_BCLK        45              // extension : GPIO45 -> BCK  (J1 broche 8 du module)
#define FO_WS          46              // extension : GPIO46 -> LRCK (J1 broche 10)
#define FO_DIN         44              // serie : GPIO44 (RX0) <- SD (J1 broche 9)
#define FO_FS          48000
#define FO_SAUTE       (FO_FS / 2)     // 0,5 s ignorees (le convertisseur du module se cale)
static volatile int fo_etat = 0;
static volatile uint32_t fo_ech = 0;
static volatile float fo_niveau = 0;
static volatile uint32_t fo_fins = 0;
static volatile bool fo_fin_ok = false;
static volatile float fo_fin_niveau = 0;
static char fo_fin_txt[64] = "";

static void fo_tache(void *);

// ---- ENTREE OPTIQUE EN CONTINU (v0.8.0) : remplace le micro dans l'anneau ----
// Meme filtre / meme decimation que le micro (48 kHz -> 16 kHz) : le moteur ne voit
// aucune difference. Le micro est mis en sourdine (il ne remplit plus l'anneau).
// v0.9.60 : un SURVEILLANT (tache fo_surv) fait tourner l'optique tant que la source
// voulue est « optique » : demarrage, relance apres un TEST FO, nouvel essai toutes les
// 5 s en cas d'echec. Le micro n'est coupe que pendant que l'optique ecrit vraiment
// dans l'anneau (avant : un choix pendant le test etait perdu et l'ecran affichait
// « Optique: rien » avec le micro toujours actif).
static volatile bool s_mic_muet = false;      // le micro n'ecrit plus dans l'anneau
static volatile bool s_fo_veut = false;       // la tache optique doit continuer
static volatile bool s_fo_vit = false;        // la tache optique ecrit dans l'anneau
static volatile bool s_fo_vive = false;       // la tache optique existe (de sa creation a sa fin)
static volatile int s_src_veut = 0;           // source choisie : 0 micro, 1 optique
static volatile uint32_t s_fo_signal_ms = 0;  // dernier son non nul
static void fo_flux_tache(void *);

bool plat_fo_flux_ok() { return s_fo_vit && (uint32_t)(millis() - s_fo_signal_ms) < 2000; }
// -1 micro choisi ; 0 optique choisie mais arretee (le micro ecoute) ; 1 en service, silence ;
// 2 son recu ; 3 test optique en cours
int plat_fo_etat()
{
    if (!s_src_veut) return -1;
    if (fo_etat) return 3;
    if (!s_fo_vit) return 0;
    return plat_fo_flux_ok() ? 2 : 1;
}

// arrete la tache optique et attend qu'elle ait rendu l'anneau au micro et le port I2S
// (1,5 s au plus : un message sur le port serie peut bloquer un instant si le PC ne lit pas)
static bool fo_flux_arreter()
{
    s_fo_veut = false;
    for (int k = 0; k < 150 && s_fo_vive; k++) delay(10);
    return !s_fo_vive;
}

static void fo_surv_tache(void *)
{
    uint32_t prochain = 0;
    for (;;) {
        uint32_t ms = millis();
        if (s_src_veut == 1) {
            if (!s_fo_vive && fo_etat == 0 && (int32_t)(ms - prochain) >= 0) {
                prochain = ms + 5000;                      // s'il s'arrete tout seul : pas avant 5 s
                s_fo_signal_ms = 0;
                s_fo_veut = true;
                s_fo_vive = true;
                if (xTaskCreatePinnedToCore(fo_flux_tache, "fo_flux", 5120, NULL, 5, NULL, 1) != pdPASS) {
                    s_fo_vive = false;
                    s_fo_veut = false;
                    Console.printf("[FO] tâche optique impossible (RAM interne %u Ko) : le micro reste actif, nouvel essai dans 5 s\n",
                                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
                }
            }
        } else {
            prochain = 0;
            if (s_fo_vive) s_fo_veut = false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void plat_source(int src)
{
    static bool surv = false;
    int avant = s_src_veut;
    s_src_veut = src == 1 ? 1 : 0;
    if (!surv) {
        surv = xTaskCreatePinnedToCore(fo_surv_tache, "fo_surv", 4096, NULL, 2, NULL, 1) == pdPASS;
        if (!surv) Console.println("[FO] surveillant de l'entrée optique impossible à lancer");
    }
    if (s_src_veut != avant) Console.printf("[FO] source du son : %s\n", s_src_veut ? "entrée optique" : "micro");
    if (!s_src_veut && s_fo_vive) fo_flux_arreter();   // retour au micro tout de suite
}

const char *plat_fo_test_demarrer()
{
    if (!s_sd_ok) return "pas de carte SD";
    if (fo_etat) return nullptr;
    fo_etat = 1;                                           // le surveillant ne relance plus l'optique
    s_fo_veut = false;                                     // v0.9.62 : l'optique s'arrete ; la tache de test l'attend
    fo_ech = 0;
    fo_niveau = 0;
    if (xTaskCreatePinnedToCore(fo_tache, "fo_test", 5120, NULL, 3, NULL, 1) != pdPASS) {
        fo_etat = 0;
        return "tâche impossible";
    }
    return nullptr;
}

void plat_fo_test(TestOptique &t)
{
    plat_verrou();
    t.etat = fo_etat;
    t.duree_s = (float)fo_ech / FO_FS;
    t.niveau = fo_niveau;
    t.fins = fo_fins;
    t.fin_ok = fo_fin_ok;
    t.fin_niveau = fo_fin_niveau;
    memcpy(t.fin_txt, fo_fin_txt, sizeof t.fin_txt);
    plat_deverrou();
}

static void fo_fin(bool ok, float pic, const char *txt)
{
    char tmp[sizeof fo_fin_txt];
    snprintf(tmp, sizeof tmp, "%s", txt);
    plat_verrou();
    fo_fin_ok = ok;
    fo_fin_niveau = pic;
    memcpy(fo_fin_txt, tmp, sizeof tmp);
    fo_fins = fo_fins + 1;
    fo_etat = 0;
    plat_deverrou();
    Console.printf("[FO] Test optique : %s\n", txt);
}

static bool fo_ecrire_wav(const char *chemin, const int16_t *x, uint32_t n, uint32_t freq)
{
    uint8_t h[44];
    entete_wav(h, n * 2, freq);
    sd_prendre();
    SD_MMC.remove(chemin);
    fs::File f = SD_MMC.open(chemin, FILE_WRITE);
    sd_rendre();
    if (!f) return false;
    sd_prendre(); size_t w = f.write(h, 44); sd_rendre();
    bool ok = w == 44;
    for (uint32_t k = 0; ok && k < n; k += 2048) {
        uint32_t m = n - k < 2048 ? n - k : 2048;
        sd_prendre();
        size_t e = ecrire_morceaux(f, (const uint8_t *)(x + k), m * 2);
        sd_rendre();
        ok = e == m * 2;
        vTaskDelay(1);
    }
    sd_prendre(); f.close(); sd_rendre();
    return ok;
}

// Reglage I2S commun au test et au flux (v0.9.59) : ESP32 maitre, Philips, 16 bits utiles
// dans des cases de 32 (BCK = 64 x 48 kHz). ws_width = 32 OBLIGATOIRE : la valeur par defaut
// de la bibliotheque suit la taille du mot (16), ce qui donnait un LRCK haut 16 tops sur 64
// au lieu d'un carre 32/32 -> le module ne trouvait pas les voies et l'ESP32 lisait des zeros.
static void fo_regler_std(i2s_std_config_t &sc)
{
    sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FO_FS);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    sc.slot_cfg.ws_width = 32;
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = (gpio_num_t)FO_BCLK;
    sc.gpio_cfg.ws = (gpio_num_t)FO_WS;
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din = (gpio_num_t)FO_DIN;
}

static void fo_tache(void *)
{
    // v0.9.59 : seul le WAV 16 kHz est garde (320 Ko au lieu de 1,28 Mo : la PSRAM libre
    // ne suffisait plus pour les deux). Le 16 kHz est calcule pendant l'ecoute.
    // v0.9.62 : en mode Optique, le flux continu rend d'abord le port I2S (3 s au plus)
    for (int k = 0; k < 300 && s_fo_vive; k++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_fo_vive) { fo_fin(false, 0, "entrée optique occupée, réessaie"); vTaskDelete(NULL); }
    const uint32_t N48 = (uint32_t)FO_TEST_S * FO_FS, N16 = N48 / 3;
    const uint32_t libre = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    Console.printf("[FO] Test optique : PSRAM %u Ko d'un bloc, %u Ko demandés\n",
                   (unsigned)(libre / 1024), (unsigned)(N16 * 2 / 1024));
    int16_t *x16 = (int16_t *)heap_caps_malloc(N16 * 2, MALLOC_CAP_SPIRAM);
    int16_t *blk = (int16_t *)heap_caps_malloc(2 * CH * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    i2s_chan_handle_t rx = NULL;
    const char *echec = nullptr;
    if (!x16 || !blk) echec = "mémoire PSRAM insuffisante";
    if (!echec) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        cc.dma_desc_num = 8;
        cc.dma_frame_num = CH;
        if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) echec = "port I2S 1 indisponible";
    }
    if (!echec) {
        i2s_std_config_t sc;
        fo_regler_std(sc);
        if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) echec = "I2S optique : init refusée";
    }
    uint32_t n48 = 0, n16 = 0, vus = 0, nuls = 0, sature = 0, non_nuls = 0;
    int pic = 0;
    if (!echec) {
        float coef[NFD], hist[NFD] = {};
        {   // meme filtre que le micro (passe-bas 7 kHz), calcule ici pour ne pas dependre du micro
            const double fc = 7000.0 / FO_FS;
            double s = 0;
            for (int i = 0; i < NFD; i++) {
                double m = i - (NFD - 1) / 2.0;
                double h = m == 0 ? 2 * fc : sin(2 * PI * fc * m) / (PI * m);
                h *= 0.54 - 0.46 * cos(2 * PI * i / (NFD - 1));
                coef[i] = (float)h; s += h;
            }
            for (int i = 0; i < NFD; i++) coef[i] = (float)(coef[i] / s);
        }
        int pos = 0, phase = 0;
        uint32_t t0 = millis();
        while (n48 < N48 && millis() - t0 < (FO_TEST_S + 6) * 1000UL) {
            size_t lus = 0;
            if (i2s_channel_read(rx, blk, 2 * CH * 2, &lus, 300) != ESP_OK && !lus) { nuls++; continue; }
            int trames = lus / 4;
            int pic_bloc = 0;
            for (int i = 0; i < trames && n48 < N48; i++) {
                int16_t v = (int16_t)(((int32_t)blk[2 * i] + blk[2 * i + 1]) / 2);   // v0.9.67 : (gauche + droite) / 2
                vus++;
                if (vus <= (uint32_t)FO_SAUTE) continue;
                n48++;
                if (v) non_nuls++;
                int a = v < 0 ? -(int)v : v;
                if (a > pic_bloc) pic_bloc = a;
                if (a >= 32700) sature++;
                hist[pos] = (float)v;
                pos = pos + 1 == NFD ? 0 : pos + 1;
                if (++phase == 3) {
                    phase = 0;
                    float acc = 0;
                    for (int k = 0; k < NFD; k++) { int j = pos - 1 - k; if (j < 0) j += NFD; acc += coef[k] * hist[j]; }
                    if (n16 < N16) x16[n16++] = (int16_t)(acc > 32767.f ? 32767 : (acc < -32768.f ? -32768 : acc));
                }
            }
            if (pic_bloc > pic) pic = pic_bloc;
            fo_niveau = pic_bloc / 32768.0f;
            fo_ech = n48;
        }
    }
    if (rx) { i2s_channel_disable(rx); i2s_del_channel(rx); }
    float pic_f = pic / 32768.0f;
    char t[64];
    if (!echec) Console.printf("[FO] Test optique : %u échantillons, %u non nuls, crête %d, lectures vides %u\n",
                               (unsigned)n48, (unsigned)non_nuls, pic, (unsigned)nuls);
    if (!echec && n48 < N48) echec = "pas de signal I2S (horloge ?)";
    if (echec) {
        fo_fin(false, pic_f, echec);
    } else {
        fo_etat = 2;
        sd_prendre(); if (!SD_MMC.exists(CAP_DOSSIER)) SD_MMC.mkdir(CAP_DOSSIER); SD_MMC.remove(CAP_DOSSIER "/fo_test_48k.wav"); sd_rendre();
        bool ok = fo_ecrire_wav(CAP_DOSSIER "/fo_test_16k.wav", x16, n16, 16000);
        if (!ok) fo_fin(false, pic_f, "écriture SD impossible");
        else if (pic < 30) { snprintf(t, sizeof t, "ZÉROS : aucun son (télé en PCM ? 5 V ?)"); fo_fin(true, pic_f, t); }
        else { snprintf(t, sizeof t, "crête %d %%%s, fo_test_16k.wav", (int)(pic_f * 100 + 0.5f), sature > 50 ? " SATURÉ" : ""); fo_fin(true, pic_f, t); }
    }
    if (x16) heap_caps_free(x16);
    if (blk) heap_caps_free(blk);
    vTaskDelete(NULL);
}

static void fo_flux_tache(void *)
{
    int16_t *blk = (int16_t *)heap_caps_malloc(2 * CH * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *hist = (float *)calloc(NFD, sizeof(float));
    i2s_chan_handle_t rx = NULL;
    const char *err = nullptr;
    if (!blk || !hist) err = "mémoire";
    if (!err) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        cc.dma_desc_num = 8;
        cc.dma_frame_num = CH;
        if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) { rx = NULL; err = "port I2S 1 occupé"; }
    }
    if (!err) {
        i2s_std_config_t sc;
        fo_regler_std(sc);
        if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) err = "réglage I2S refusé";
    }
    bool ok = !err;
    if (!ok) {
        Console.printf("[FO] entrée optique : démarrage impossible (%s), le micro reste actif, nouvel essai dans 5 s\n", err);
    } else {
        s_mic_muet = true;                              // le micro finit son bloc (5 ms) puis n'ecrit plus
        vTaskDelay(pdMS_TO_TICKS(30));
        s_fo_vit = true;
        Console.println("[FO] entrée optique en service (48 kHz -> 16 kHz), micro coupé");
    }
    int pos = 0, phase = 0;
    // diagnostic (v0.9.59) : une ligne toutes les 5 s dans la console
    // (toutes les 5 s pendant les 30 premieres secondes, puis toutes les minutes)
    uint32_t d_t0 = millis(), d_ech = 0, d_non_nuls = 0, d_vides = 0;
    int d_pic = 0, d_lignes = 0;
    while (ok && s_fo_veut) {
        if ((uint32_t)(millis() - d_t0) >= (d_lignes < 6 ? 5000u : 60000u)) {
            uint32_t duree = millis() - d_t0;
            d_lignes++;
            Console.printf("[FO] %u s : %u échantillons (attendu %u), %u non nuls, crête %d (%d %%), lectures vides %u\n",
                           (unsigned)((duree + 500) / 1000), (unsigned)d_ech, (unsigned)((uint64_t)FO_FS * duree / 1000), (unsigned)d_non_nuls, d_pic,
                           (int)(d_pic * 100L / 32768), (unsigned)d_vides);
            d_t0 = millis(); d_ech = d_non_nuls = d_vides = 0; d_pic = 0;
        }
        size_t lus = 0;
        if (i2s_channel_read(rx, blk, 2 * CH * 2, &lus, 200) != ESP_OK && !lus) { d_vides++; continue; }
        int trames = lus / 4;
        uint32_t e = r_ecrit;
        for (int i = 0; i < trames; i++) {
            int16_t v = (int16_t)(((int32_t)blk[2 * i] + blk[2 * i + 1]) / 2);   // v0.9.67 : (gauche + droite) / 2
            d_ech++;
            if (v) d_non_nuls++;
            { int a = v < 0 ? -(int)v : v; if (a > d_pic) d_pic = a; }
            if (v > 30 || v < -30) s_fo_signal_ms = millis();
            hist[pos] = (float)v;
            if (++pos == NFD) pos = 0;
            if (++phase < 3) continue;
            phase = 0;
            float y = 0;
            int p = pos;
            for (int k = 0; k < NFD; k++) { y += s_coef[k] * hist[p]; if (++p == NFD) p = 0; }
            int w = (int)lroundf(y);
            s_anneau[e & (ANNEAU_N - 1)] = (int16_t)(w > 32767 ? 32767 : (w < -32768 ? -32768 : w));
            e++;
        }
        r_ecrit = e;
    }
    s_fo_vit = false;                               // plus rien n'est ecrit dans l'anneau
    s_mic_muet = false;                             // le micro reprend
    if (rx) { if (ok) i2s_channel_disable(rx); i2s_del_channel(rx); }
    if (blk) heap_caps_free(blk);
    if (hist) free(hist);
    s_fo_vive = false;                              // v0.9.62 : port I2S rendu AVANT d'ecrire sur la console
    if (ok) Console.println("[FO] entrée optique arrêtée, retour au micro");
    vTaskDelete(NULL);
}

// ---- bip du haut-parleur (fabrique dans la tache audio, sans bloquer) ----
struct Note { float f; uint16_t ms; };
static const Note M_DEBUT[] = { { 1318.5f, 90 }, { 1046.5f, 90 }, { 784.0f, 200 } };   // ding-ding-dong
static const Note M_FIN[]   = { { 784.0f, 90 }, { 1174.7f, 180 } };                    // retour au programme
static const Note M_TEST[]  = { { 1046.5f, 80 }, { 1318.5f, 80 }, { 1568.0f, 160 } };
static const Note *volatile s_melodie = nullptr;
static volatile int s_nb_notes = 0;
static volatile uint32_t s_son_jusqua = 0;      // ms : fin du jingle + 0,3 s (le micro l'entend)
static volatile int s_pa_actif = HIGH;          // niveau qui allume l'ampli (teste au premier demarrage)

void plat_son(int quoi)
{
    if (!s_tx) return;
    const Note *m = quoi == SON_PUB_FIN ? M_FIN : (quoi == SON_TEST ? M_TEST : M_DEBUT);
    int n = quoi == SON_PUB_FIN ? 2 : 3;
    uint32_t duree = 0;
    for (int i = 0; i < n; i++) duree += m[i].ms;
    s_son_jusqua = millis() + duree + 300;
    s_nb_notes = n;
    s_melodie = m;                                  // la tache audio le prend au prochain passage
}

bool plat_son_en_cours() { return (int32_t)(s_son_jusqua - millis()) > 0; }

static void tache_audio(void *arg)
{
    static int16_t entree[2 * CH], sortie[2 * CH];
    const Note *mel = nullptr;
    int nb = 0, note = 0;
    uint32_t ech = 0, ech_note = 0;
    double ph = 0;
    int pa_coupe_dans = 0;
    static float hist[NFD];
    int pos = 0, phase = 0;
    double acc = 0;
    int acc_n = 0;
    for (;;) {
        // ---- sortie : jingle eventuel (sinus + harmonique, attaque douce, decroissance) ----
        if (s_melodie) {
            mel = s_melodie; nb = s_nb_notes; s_melodie = nullptr;
            note = 0; ech = 0; ph = 0;
            ech_note = (uint32_t)mel[0].ms * FS_I2S / 1000;
            digitalWrite(PA_PIN, s_pa_actif);
            pa_coupe_dans = 0;
        }
        for (int i = 0; i < CH; i++) {
            int16_t v = 0;
            if (mel) {
                float t = (float)ech / FS_I2S;
                float env = (t < 0.004f ? t / 0.004f : 1.0f) * expf(-t * 9.0f);
                ph += 2.0 * PI * mel[note].f / FS_I2S;
                if (ph > 2 * PI) ph -= 2 * PI;
                v = (int16_t)(9000.0f * env * (sinf((float)ph) + 0.25f * sinf(2.0f * (float)ph)));
                if (++ech >= ech_note) {
                    ech = 0;
                    if (++note >= nb) { mel = nullptr; pa_coupe_dans = FS_I2S / 10; }
                    else ech_note = (uint32_t)mel[note].ms * FS_I2S / 1000;
                }
            }
            sortie[2 * i] = sortie[2 * i + 1] = v;
        }
        if (pa_coupe_dans > 0 && (pa_coupe_dans -= CH) <= 0) digitalWrite(PA_PIN, !s_pa_actif);   // ampli coupe : pas de souffle
        size_t n = 0;
        i2s_channel_write(s_tx, sortie, sizeof sortie, &n, portMAX_DELAY);
        i2s_channel_read(s_rx, entree, sizeof entree, &n, portMAX_DELAY);
        int trames = (int)(n / 4);
        if (s_mic_muet) trames = 0;                 // optique en service : le micro n'ecrit plus dans l'anneau
        uint32_t e = r_ecrit;
        for (int i = 0; i < trames; i++) {
            float x = entree[2 * i];                // micro sur la voie gauche
            hist[pos] = x;
            if (++pos == NFD) pos = 0;
            if (++phase < 3) continue;
            phase = 0;
            float y = 0;
            int p = pos;
            for (int k = 0; k < NFD; k++) {
                y += s_coef[k] * hist[p];
                if (++p == NFD) p = 0;
            }
            int v = (int)lroundf(y);
            s_anneau[e & (ANNEAU_N - 1)] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            e++;
            acc += (double)y * y;
            acc_n++;
        }
        if (trames) r_ecrit = e;
        if (acc_n >= 1600) {
            s_rms_test = (float)sqrt(acc / acc_n);
            acc = 0;
            acc_n = 0;
        }
    }
}

int plat_audio_lire(int16_t *dst, int nmax)
{
    uint32_t e = r_ecrit;
    if (e - r_lu > ANNEAU_N - 4096) r_lu = e - (ANNEAU_N - 4096);   // trop de retard : on saute
    int n = 0;
    while (r_lu != e && n < nmax) dst[n++] = s_anneau[(r_lu++) & (ANNEAU_N - 1)];
    return n;
}

static void init_audio()
{
    s_anneau = (int16_t *)heap_caps_malloc(ANNEAU_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    pinMode(PA_PIN, OUTPUT);
    int pa = -1;
    if (plat_nvs_lire("pa", &pa, sizeof pa) && (pa == HIGH || pa == LOW)) s_pa_actif = pa;
    digitalWrite(PA_PIN, !s_pa_actif);
    init_filtre();
    // l'horloge MCLK doit tourner avant de configurer le codec
    if (!s_anneau || !init_i2s()) { snprintf(s_audio_etat, sizeof s_audio_etat, "I2S en échec"); return; }
    xTaskCreatePinnedToCore(tache_audio, "audio", 4096, NULL, 6, NULL, 0);
    delay(30);
    if (!s_bus || !init_codec()) { snprintf(s_audio_etat, sizeof s_audio_etat, "codec ES8311 absent"); return; }
    // le micro doit donner un signal (meme le souffle d'une piece calme)
    delay(400);
    float rms = s_rms_test;
    Console.printf("[PUB] Micro : niveau %.1f (sur 32768)\n", rms);
    if (rms > 0.5f) {
        s_audio_ok = true;
        snprintf(s_audio_etat, sizeof s_audio_etat, "micro OK");
    } else {
        snprintf(s_audio_etat, sizeof s_audio_etat, "micro muet");
    }
    // premier demarrage : quel niveau allume l'ampli du haut-parleur ? (les documents
    // de la carte divergent) On joue le jingle de test avec chacun, le micro juge.
    if (s_audio_ok && !(pa == HIGH || pa == LOW)) {
        float mesure[2];
        for (int k = 0; k < 2; k++) {
            s_pa_actif = k == 0 ? HIGH : LOW;
            plat_son(SON_TEST);
            float mx = 0;
            for (int w = 0; w < 6; w++) { delay(60); if (s_rms_test > mx) mx = s_rms_test; }
            mesure[k] = mx;
            delay(250);
        }
        s_pa_actif = mesure[0] >= mesure[1] ? HIGH : LOW;
        pa = s_pa_actif;
        plat_nvs_ecrire("pa", &pa, sizeof pa);
        Console.printf("[PUB] Ampli : niveau %s (micro %.0f / %.0f)\n", pa == HIGH ? "HAUT" : "BAS", mesure[0], mesure[1]);
    }
}

// =========================================================================
// Tache moteur (coeur 0) : empreintes, bibliotheque, detecteur, coupure du son
// =========================================================================
static void tache_moteur(void *arg)
{
    for (;;) {
        uint32_t attente = mot::etape(millis());
        if (attente > 30) attente = 30;
        vTaskDelay(pdMS_TO_TICKS(attente ? attente : 1));
    }
}

// =========================================================================
// Ecran : deux tampons, envoi sur le coeur 0
// =========================================================================
static uint16_t *s_tampon[2] = { nullptr, nullptr };
static uint16_t *s_fond = nullptr;
static bool s_double = false;
static int s_courant = 0;
static volatile int s_a_envoyer = 0;
static SemaphoreHandle_t s_sem_pret, s_sem_libre;

static void envoyer(const uint16_t *img) { ecran_envoyer(img); }

// v0.9.6 : retour immediat au toucher, avant que l'image complete soit prete.
// 1) la LED flashe en blanc 100 ms ; 2) un cadre blanc est trace tout de suite autour du bouton,
// seulement si l'ecran n'est pas en train de recevoir une image (sinon l'image arrive de toute facon).
void plat_appui_zone(int x, int y, int l, int h)
{
    if (cfg::R.led) { rgbLedWrite(RGB_LED_PIN, 40, 40, 40); s_flash_fin = millis() + 100; if (!s_flash_fin) s_flash_fin = 1; }
    if (l <= 0 || h <= 0) return;
    if (s_double) {
        if (xSemaphoreTake(s_sem_libre, 0) != pdTRUE) return;
        ecran_cadre(x - 1, y - 1, l + 2, h + 2, 0xFFFF);
        xSemaphoreGive(s_sem_libre);
    } else {
        ecran_cadre(x - 1, y - 1, l + 2, h + 2, 0xFFFF);
    }
}

static void tache_ecran(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_sem_pret, portMAX_DELAY);
        envoyer(s_tampon[s_a_envoyer]);
        xSemaphoreGive(s_sem_libre);
        vTaskDelay(1);
    }
}

static bool init_tampons()
{
    const size_t taille = 320 * 240 * sizeof(uint16_t);
    s_tampon[0] = (uint16_t *)heap_caps_malloc(taille, MALLOC_CAP_SPIRAM);
    s_tampon[1] = (uint16_t *)heap_caps_malloc(taille, MALLOC_CAP_SPIRAM);
    s_fond = (uint16_t *)heap_caps_malloc(taille, MALLOC_CAP_SPIRAM);
    if (s_tampon[0] && s_tampon[1]) {
        s_double = true;
    } else {
        Console.println("[PUB] PSRAM absente : un seul tampon, animations ralenties");
        if (s_tampon[0]) heap_caps_free(s_tampon[0]);
        if (s_tampon[1]) heap_caps_free(s_tampon[1]);
        if (s_fond) heap_caps_free(s_fond);
        s_fond = nullptr;
        s_tampon[1] = nullptr;
        s_tampon[0] = (uint16_t *)heap_caps_malloc(taille, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_tampon[0]) return false;
    }
    return true;
}

// =========================================================================
// Tactile (meme etalonnage que la console NES et les autres projets)
// =========================================================================
static bool s_tactile_ok = false;

static bool init_bus()
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)I2C_SDA;
    cfg.scl_io_num = (gpio_num_t)I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&cfg, &s_bus) == ESP_OK;
}

static bool lire_appui_brut(int *rx, int *ry)
{
    static int xs[64], ys[64];
    while (ft6336_touch_get_raw(rx, ry)) delay(10);
    uint32_t t0 = millis();
    while (!ft6336_touch_get_raw(rx, ry)) {
        if (millis() - t0 > 30000) return false;
        delay(10);
    }
    int n = 0;
    while (ft6336_touch_get_raw(&xs[n % 64], &ys[n % 64])) { n++; delay(10); }
    if (n > 64) n = 64;
    if (n < 3) return false;
    qsort(xs, n, sizeof(int), [](const void *a, const void *b) { return *(int *)a - *(int *)b; });
    qsort(ys, n, sizeof(int), [](const void *a, const void *b) { return *(int *)a - *(int *)b; });
    *rx = xs[n / 2];
    *ry = ys[n / 2];
    return true;
}

static void etalonner()
{
    const int px[3] = { 30, 290, 160 }, py[3] = { 30, 120, 210 };
    int rx[3], ry[3];
    for (;;) {
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            gfx::fb = s_tampon[0];
            gfx::degrade_v(0, 0, 320, 240, gfx::rgb(4, 6, 18), gfx::rgb(16, 12, 42));
            gfx::texte(160, 96, "Étalonnage de l'écran", POLICE_16, gfx::rgb(236, 242, 255), 255, gfx::CENTRE);
            gfx::texte(160, 120, "Touche le centre de la croix", POLICE_13, gfx::rgb(156, 168, 210), 255, gfx::CENTRE);
            gfx::halo(px[i], py[i], 18, gfx::rgb(255, 64, 200), 160);
            gfx::trait(px[i] - 12, py[i], px[i] + 12, py[i], 2, gfx::rgb(255, 64, 200));
            gfx::trait(px[i], py[i] - 12, px[i], py[i] + 12, 2, gfx::rgb(255, 64, 200));
            envoyer(s_tampon[0]);
            ok = lire_appui_brut(&rx[i], &ry[i]);
        }
        if (ok && ft6336_touch_set_calibration(rx[0], ry[0], px[0], py[0], rx[1], ry[1], px[1], py[1],
                                               rx[2], ry[2], px[2], py[2])) {
            ft6336_touch_save_calibration();
            return;
        }
    }
}

// =========================================================================
// v0.9.11 : essai d'ecriture / relecture de 256 Ko pour valider le bus rapide (40 MHz).
// Renvoie false au moindre octet different ou a la moindre erreur ; remplit les debits (Ko/s).
static bool sd_test_bus(int &ko_ecr, int &ko_lec)
{
    const size_t N = 256 * 1024, P = 4096;
    uint8_t *b = (uint8_t *)heap_caps_malloc(P, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!b) return true;                               // pas de memoire pour tester : on ne bloque pas
    bool ok = true;
    uint32_t t0 = millis();
    fs::File f = SD_MMC.open("/sdtest.bin", FILE_WRITE);
    if (!f) ok = false;
    for (size_t k = 0; ok && k < N / P; k++) {
        for (size_t i = 0; i < P; i++) b[i] = (uint8_t)(i * 31 + k * 7);
        if (f.write(b, P) != P) ok = false;
    }
    if (f) { f.flush(); f.close(); }
    uint32_t t1 = millis();
    f = SD_MMC.open("/sdtest.bin", "r");
    if (!f) ok = false;
    for (size_t k = 0; ok && k < N / P; k++) {
        if (f.read(b, P) != P) { ok = false; break; }
        for (size_t i = 0; i < P; i++) if (b[i] != (uint8_t)(i * 31 + k * 7)) { ok = false; break; }
    }
    if (f) f.close();
    uint32_t t2 = millis();
    SD_MMC.remove("/sdtest.bin");
    heap_caps_free(b);
    ko_ecr = (int)(256000UL / (t1 - t0 ? t1 - t0 : 1));
    ko_lec = (int)(256000UL / (t2 - t1 ? t2 - t1 : 1));
    return ok;
}

static void init_sd()
{
    if (!SD_MMC.setPins(SD_SCK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) return;
    // v0.9.11 : on essaie d'abord 40 MHz en 4 bits, avec un test d'ecriture / relecture ;
    // au moindre doute on revient a 20 MHz (les erreurs 0x107 vues avant venaient d'un bus
    // trop rapide pour la carte)
    bool rapide = false;
    if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED)) {
        int e = 0, l = 0;
        rapide = sd_test_bus(e, l);
        Console.printf("[PUB] Carte SD a 40 MHz : %s (ecriture %d Ko/s, lecture %d Ko/s)\n", rapide ? "validee" : "ECHEC du test, retour a 20 MHz", e, l);
        if (!rapide) SD_MMC.end();
    } else {
        SD_MMC.end();
        Console.println("[PUB] Carte SD a 40 MHz : refusee, retour a 20 MHz");
    }
    if (!rapide) {
        if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT)) {
            SD_MMC.end();
            if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) return;
        }
        int e = 0, l = 0;
        sd_test_bus(e, l);
        Console.printf("[PUB] Carte SD a 20 MHz : ecriture %d Ko/s, lecture %d Ko/s\n", e, l);
    }
    s_sd_ok = true;
    if (!SD_MMC.exists("/pub")) SD_MMC.mkdir("/pub");
    if (!SD_MMC.exists("/pub/audio")) SD_MMC.mkdir("/pub/audio");
    if (!SD_MMC.exists(CAP_DOSSIER)) SD_MMC.mkdir(CAP_DOSSIER);
    if (!SD_MMC.exists("/pub/journal.csv")) {
        const char *e = "# secondes_depuis_demarrage,evenement,pub_n,valeur\n";
        plat_fichier_ecrire("/pub/journal.csv", e, strlen(e));
    }
}

// Garde la liaison Wi-Fi eveillee (certaines box, dont la Bbox, endorment le
// pont 2,4 GHz) : un petit paquet multicast toutes les 30 s, sans rien ecrire
// dans le journal. (Ajout de la v0.5.31, garde.)
static WiFiUDP s_udp_eveil;
static void garder_reseau_eveille()
{
    static uint32_t t = 0;
    if (millis() - t < 30000) return;
    t = millis();
    if (WiFi.status() != WL_CONNECTED) return;
    s_udp_eveil.beginPacket(IPAddress(224, 0, 0, 251), 5353);
    s_udp_eveil.write((const uint8_t *)"PUB_PING", 8);
    s_udp_eveil.endPacket();
}

static void memoire(const char *quand)
{
    Console.printf("[PUB] Memoire interne %s : %u Ko libres (plus grand bloc %u Ko), PSRAM %u Ko\n", quand,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void setup()
{
    Serial.begin(115200);
    journal_init();                                   // la console serie est aussi gardee en memoire (ecran Journal)
    delay(200);
    Console.printf("\n[PUB] Detecteur de pub %s · Arduino ESP32 %s (Espressif) · ESP-IDF %s\n", PUB_VERSION, plat_version_arduino(), plat_version_idf());
    journal_rapport_demarrage();                      // cause du demarrage (et journal d'avant un plantage)
    // malloc() de 256 octets et plus : en PSRAM (au lieu de 4 Ko). La memoire
    // interne reste au Wi-Fi, au chiffrement (tele) et aux tampons DMA.
    heap_caps_malloc_extmem_enable(256);

    ledcAttachChannel(LCD_BL_PIN, LCD_BL_HZ, 8, 0);
    ledcWrite(LCD_BL_PIN, 0);                          // eteint pendant l'init (pas d'image parasite)
    if (!ecran_init()) Console.println("[PUB] Ecran ST77922 : init en echec");
    ledcWrite(LCD_BL_PIN, 204);

    s_prefs.begin("pub", false);
    if (!init_tampons()) {
        ecran_remplir(0xF800);                        // ecran rouge : memoire insuffisante (PSRAM OPI ?)
        for (;;) delay(1000);
    }
    gfx::init(s_tampon[0]);
    ecran_banc(s_tampon[0]);                          // v0.9.51 : temps de chaque etape de l'ecran (journal)

    // le tactile non reinitialise peut tenir SDA a 0 : on le relache avant de creer le bus
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(40);
    if (init_bus()) {
        s_tactile_ok = ft6336_touch_init(s_bus, (gpio_num_t)TOUCH_RST, 320, 240);
        if (!s_tactile_ok) Console.println("[PUB] Tactile introuvable");
        else if (!ft6336_touch_load_calibration()) etalonner();
    }

    s_nvs_file = xQueueCreate(8, sizeof(EcritureNvs));
    xTaskCreatePinnedToCore(tache_nvs, "nvs", 3072, NULL, 1, NULL, 1);

    reseau::preparer();                               // Wi-Fi : ta box (identifiants.h, ou reseau donne par le portail)
    s_sd_mtx = xSemaphoreCreateRecursiveMutex();
    s_emp_mx = xSemaphoreCreateMutex();               // v0.9.58 : moteur d'empreintes (direct / fichier du portail)
    init_sd();
    a_tampon = (uint8_t *)heap_caps_malloc(ARCH_TAMPON, MALLOC_CAP_SPIRAM);
    a_file = xQueueCreate(32, sizeof(ReqAudio));
    // pile en PSRAM : cette tache n'ecrit que sur la carte SD (jamais en flash)
    if (s_sd_ok && a_tampon) {
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(tache_archive, "archive", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM);
        if (ok != pdPASS) ok = xTaskCreatePinnedToCore(tache_archive, "archive", 6144, NULL, 2, NULL, 0);
        if (ok != pdPASS) Console.println("[PUB] Archive audio : tâche impossible à lancer, les pubs apprises n'auront pas de son");
    } else if (s_sd_ok) Console.println("[PUB] Archive audio : pas de mémoire PSRAM, les pubs apprises n'auront pas de son");
    Console.printf("[PUB] Carte SD : %s\n", s_sd_ok ? "OK" : "absente");
    mot::init();                                      // empreintes, bibliotheque (relue sur la SD)
    {
        int nj = base::nb_blocs() - base::nb_pubs();
        Console.printf("[PUB] Bibliotheque relue : %d pub(s), %d empreintes, dernier numero %d, %d a verifier%s\n",
                      base::nb_pubs(), base::nb_entrees(), base::max_numero(), base::nb_a_verifier(),
                      nj ? " (anciens jingles ignores)" : "");
        // sons sans pub (pub supprimee par une ancienne version, carte coupee...) :
        // retires une fois l'archive prete
        ReqAudio q = { 4, 0, 0, 0 };
        if (a_file) xQueueSend(a_file, &q, 0);
        // fichiers des anciennes versions qui ne servent plus (commande vocale)
        if (s_sd_ok) {
            sd_prendre();
            if (SD_MMC.exists("/pub/voix/modeles.bin")) SD_MMC.remove("/pub/voix/modeles.bin");
            if (SD_MMC.exists("/pub/voix")) SD_MMC.rmdir("/pub/voix");
            sd_rendre();
        }
    }
    init_audio();
    Console.printf("[PUB] Audio : %s\n", s_audio_etat);

    ui::init(s_fond);                                 // lit les reglages et applique gain / source / luminosite
    tv::init();                                       // telecommande Wi-Fi : cle et certificat (carte SD)

    // pile en PSRAM : le moteur n'ecrit jamais en flash lui-meme (NVS par tache_nvs)
    xTaskCreatePinnedToCoreWithCaps(tache_moteur, "moteur", 12288, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    // v0.9.58 : fichiers AUTO traites depuis le portail (priorite basse, coeur 1)
    fi_tab = (FiEntree *)heap_caps_calloc(FI_MAX, sizeof(FiEntree), MALLOC_CAP_SPIRAM);
    fi_codes = (emp::Code *)heap_caps_malloc(FI_CODES * sizeof(emp::Code), MALLOC_CAP_SPIRAM);
    if (s_sd_ok && fi_tab && fi_codes &&
        xTaskCreatePinnedToCoreWithCaps(tache_fichiers, "fichiers", 6144, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS)
        Console.println("[PUB] Portail : tâche des fichiers impossible à lancer");
    if (s_double) {
        s_sem_pret = xSemaphoreCreateBinary();
        s_sem_libre = xSemaphoreCreateBinary();
        xSemaphoreGive(s_sem_libre);
        xTaskCreatePinnedToCoreWithCaps(tache_ecran, "ecran", 4096, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM);   // v0.9.50 : coeur 1 (le calcul de l'image n'est plus interrompu par le moteur du coeur 0)   // envoi a l'ecran seulement : pile en PSRAM
    }
    reseau::demarrer();                               // Wi-Fi, page web
    tv::demarrer();                                   // telecommande Wi-Fi de la tele
    memoire("pret");
}

// LED RGB : temoin discret de l'etat (ecran discret derriere la tele)
static void temoin_led()
{
    static int prec = -1;
    if (s_flash_fin) {                                  // v0.9.6 : flash du toucher en cours
        if ((int32_t)(millis() - s_flash_fin) < 0) return;
        s_flash_fin = 0;
        prec = -1;                                      // retour a la couleur d'etat
    }
    det::Vue v;
    det::vue(v);
    int e = cfg::R.led ? v.etat : -2;
    if (e == prec) return;
    prec = e;
    switch (e) {
    case det::PUB: plat_led(18, 0, 14); break;                        // magenta
    case det::PROGRAMME: plat_led(0, 10, 2); break;                   // vert
    case det::APPRENTISSAGE: case det::ENREGISTREMENT: plat_led(0, 4, 16); break;   // bleu
    default: plat_led(0, 0, 0); break;                                // eteinte (pause, silence, LED coupee)
    }
}

// Boucle (coeur 1) : le tactile est lu toutes les 4 ms ; une image n'est
// dessinee que si l'interface le demande (appui, changement d'etat, animation).
// Un appui se voit donc tout de suite, au lieu d'attendre la fin de l'image en cours.
void loop()
{
    garder_reseau_eveille();
    static uint32_t dernier_appui = 0;
    static bool veille = false;
    int x = 0, y = 0;
    bool appui = s_tactile_ok && ft6336_touch_read(&x, &y);
    uint32_t ms = millis();
    if (appui) dernier_appui = ms;
    // ecran discret : il s'eteint apres 1 min sans toucher ; un appui le rallume
    bool doit_dormir = cfg::R.ecran == 1 && ms - dernier_appui > 60000;
    if (doit_dormir && !veille) { veille = true; ledcWrite(LCD_BL_PIN, 0); }
    if (veille && appui) {
        veille = false;
        plat_luminosite(cfg::R.lum);
        while (s_tactile_ok && ft6336_touch_read(&x, &y)) delay(10);   // cet appui ne fait que rallumer
        appui = false;
        ui::redessiner();
    }
    if (veille && cfg::R.ecran == 0) { veille = false; plat_luminosite(cfg::R.lum); ui::redessiner(); }
    temoin_led();
    ui::toucher(appui, x, y);
    if (veille) { ui::suivre(ms); delay(50); return; }   // on suit les evenements, sans afficher
    if (!ui::a_dessiner(ms)) { delay(4); return; }

    gfx::fb = s_tampon[s_courant];
    ui::image(millis());
    if (s_double) {
        xSemaphoreTake(s_sem_libre, portMAX_DELAY);   // l'image precedente est partie
        s_a_envoyer = s_courant;
        xSemaphoreGive(s_sem_pret);
        s_courant ^= 1;
    } else {
        envoyer(s_tampon[0]);
    }
}
