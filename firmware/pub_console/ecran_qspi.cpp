// ecran_qspi.cpp - ecran ST77922 (QSPI, 320x480 natif portrait) de la carte ES3C35P.
//
// v2 (01/11/2026) : l'essai sur la carte a montre que ce panneau IGNORE le
// passage en paysage par MADCTL (bit MV) : image coupee en deux colonnes et
// retournee. L'ecran reste donc dans son sens natif (portrait 320x480) et la
// rotation en paysage est faite ici, pendant l'agrandissement.
//
// Sources de la sequence d'initialisation : micrologiciel d'usine de la carte,
// publie par github.com/jlmeredith/ES3C35P et github.com/andrewscherpbier/esp32-s3-display.
//   - le RESET de l'ecran est relie au EN de l'ESP32 : reset logiciel (01h) ;
//     apres un televersement, debrancher / rebrancher si l'ecran reste noir ;
//   - fenetres d'ecriture alignees sur 4 pixels ; 40 MHz.
// Protocole QSPI : commande = 32 bits sur 1 fil (02h, 00h, commande, 00h) puis
// parametres ; pixels avec le code 32h, sur 4 fils.
#include "ecran_qspi.h"
#include <Arduino.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#define LCD_CS      10
#define LCD_CLK     12
#define LCD_D0      11
#define LCD_D1      13
#define LCD_D2      14
#define LCD_D3      9
#define LCD_HOTE    SPI2_HOST
#define LCD_HZ      (40 * 1000 * 1000)

// panneau (sens natif) : 320 colonnes x 480 lignes
static const int PW = 320, PH = 480;
// image vue en paysage : 480 x 320 ; interface : 320 x 240
static const int EL = 480, EH = 320;
static const int SL = 320, SH = 240;
static const int CW = 427;                    // largeur de l'interface agrandie x4/3
static const int OX = 26;                     // bande de gauche (celle de droite : 27)
static const int BANDE = 16;                  // lignes du panneau par envoi (multiple de 4) - v0.9.48 : 8 -> 16

static esp_lcd_panel_io_handle_t s_io = nullptr;
static uint16_t *s_buf[2] = { nullptr, nullptr };   // DMA, memoire interne
static uint16_t *s_plein = nullptr;                 // v0.9.47 : image entiere du panneau (PSRAM), calculee avant l'envoi
static uint32_t s_nb_images = 0;
// v0.9.52 : mise a jour partielle. s_prec = derniere image de l'interface affichee ;
// seule la zone qui a change est recalculee et envoyee.
static uint16_t *s_prec = nullptr;
static bool s_prec_ok = false;                      // faux : la prochaine image est envoyee en entier
static int f_x0 = SL, f_y0 = SH, f_x1 = -1, f_y1 = -1;   // zone a refaire de force (cadre de toucher)
static uint16_t s_x0[CW];  static uint8_t s_fx[CW]; // colonne source (et poids en quarts)
static uint8_t s_y0[EH];   static uint8_t s_fy[EH]; // ligne source (et poids en quarts)

static inline int cmd_param(uint8_t c) { return (0x02 << 24) | (c << 8); }
static inline int cmd_pixels() { return (0x32 << 24) | (0x2C << 8); }

static void commande(uint8_t c, const uint8_t *p, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, cmd_param(c), p, n);
}

struct Init { uint8_t cmd; uint8_t n; uint8_t d[16]; uint16_t attente; };

static const Init SEQ[] = {
    {0xF1, 1, {0x00}, 0},
    {0x60, 3, {0x00, 0x00, 0x00}, 0},
    {0x65, 1, {0x80}, 0},
    {0x79, 1, {0x06}, 0},
    {0x7B, 3, {0x00, 0x08, 0x08}, 0},
    {0x80, 11, {0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}, 0},
    {0x81, 4, {0x26, 0x52, 0x72, 0x27}, 0},
    {0x84, 2, {0x92, 0x25}, 0},
    {0x87, 6, {0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}, 0},
    {0x88, 15, {0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06}, 0},
    {0x89, 3, {0x00, 0x00, 0x00}, 0},
    {0x8A, 11, {0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}, 0},
    {0x8B, 9, {0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}, 0},
    {0x8C, 13, {0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09}, 0},
    {0x8D, 1, {0x0C}, 0},
    {0x8E, 6, {0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}, 0},
    {0xB3, 2, {0x00, 0x30}, 0},
    {0xF1, 1, {0x00}, 0},
    {0x71, 1, {0xD0}, 0},
    {0x66, 2, {0x02, 0x3F}, 0},
    {0xBE, 3, {0x26, 0x00, 0x9D}, 0},
    {0x70, 12, {0x01, 0xA6, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x60, 0x11, 0x00, 0x00, 0x1A}, 0},
    {0x90, 9, {0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x2D, 0x2D}, 0},
    {0x91, 9, {0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x2D, 0x2D}, 0},
    {0x92, 10, {0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x2D}, 0},
    {0x93, 10, {0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x2D}, 0},
    {0x94, 6, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0},
    {0x95, 5, {0x96, 0x16, 0x00, 0x00, 0xFF}, 0},
    {0x96, 12, {0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x9A, 0x2D, 0x00, 0x44}, 0},
    {0x97, 12, {0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x9A, 0x2D, 0x00, 0x44}, 0},
    {0xBA, 5, {0x55, 0x9A, 0x2D, 0x9A, 0x2D}, 0},
    {0x9A, 7, {0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 0},
    {0x9B, 7, {0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 0},
    {0x9C, 13, {0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}, 0},
    {0x9D, 8, {0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 0},
    {0x9E, 7, {0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 0},
    {0xB4, 12, {0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}, 0},
    {0xB5, 12, {0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}, 0},
    {0xB6, 7, {0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}, 0},
    {0x86, 14, {0xC6, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5}, 0},
    {0xB7, 16, {0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D}, 0},
    {0xB8, 16, {0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D}, 0},
    {0xB9, 2, {0x23, 0x23}, 0},
    {0xBF, 6, {0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}, 0},
    {0xF2, 1, {0x00}, 0},
    {0x73, 5, {0x04, 0xDA, 0x12, 0x54, 0x47}, 0},
    {0x77, 5, {0x6B, 0x5B, 0xFD, 0xC3, 0xC5}, 0},
    {0x7A, 2, {0x15, 0x27}, 0},
    {0x7B, 2, {0x04, 0x57}, 0},
    {0x7E, 2, {0x01, 0x0E}, 0},
    {0xBF, 1, {0x36}, 0},
    {0xE3, 2, {0x40, 0x40}, 0},
    {0xF0, 1, {0x00}, 0},
    {0xD0, 1, {0x00}, 0},
    {0x21, 0, {0}, 0},                // couleurs inversees : necessaire sur ce panneau
    {0x11, 0, {0}, 120},              // sortie de veille
    {0x3A, 1, {0x01}, 0},             // RGB565 (valeur du fabricant)
    {0x36, 1, {0x00}, 0},             // portrait natif : la rotation est faite par logiciel
    {0x35, 1, {0x01}, 20},            // signal TE (inutilise ici)
};

static void fenetre(int x0, int y0, int x1, int y1)   // coordonnees du panneau, x1/y1 inclus
{
    uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    commande(0x2A, ca, 4);
    commande(0x2B, ra, 4);
}

bool ecran_init()
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num = LCD_CLK;
    bus.data0_io_num = LCD_D0;
    bus.data1_io_num = LCD_D1;
    bus.data2_io_num = LCD_D2;
    bus.data3_io_num = LCD_D3;
    bus.max_transfer_sz = PW * BANDE * 2;
    if (spi_bus_initialize(LCD_HOTE, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = LCD_CS;
    io.dc_gpio_num = -1;
    io.spi_mode = 0;
    io.pclk_hz = LCD_HZ;
    io.trans_queue_depth = 4;
    io.lcd_cmd_bits = 32;
    io.lcd_param_bits = 8;
    io.flags.quad_mode = 1;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOTE, &io, &s_io) != ESP_OK) return false;

    for (int i = 0; i < 2; i++) {
        s_buf[i] = (uint16_t *)heap_caps_malloc(PW * BANDE * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) return false;
    }
    // image entiere calculee avant l'envoi : plus d'effet de "rideau" (la nouvelle
    // image se peignait colonne par colonne pendant le calcul)
    s_plein = (uint16_t *)heap_caps_malloc(PW * PH * 2, MALLOC_CAP_SPIRAM);
    s_prec = (uint16_t *)heap_caps_malloc(SL * SH * 2, MALLOC_CAP_SPIRAM);
    for (int c = 0; c < CW; c++) { int q = c * 3; s_x0[c] = (uint16_t)(q >> 2); s_fx[c] = (uint8_t)(q & 3); }
    for (int r = 0; r < EH; r++) { int q = r * 3; s_y0[r] = (uint8_t)(q >> 2); s_fy[r] = (uint8_t)(q & 3); }

    commande(0x01, nullptr, 0);                         // reset logiciel (pas de broche RESET)
    delay(120);
    for (const Init &s : SEQ) {
        commande(s.cmd, s.n ? s.d : nullptr, s.n);
        if (s.attente) delay(s.attente);
    }
    ecran_remplir(0x0000);
    commande(0x29, nullptr, 0);                         // affichage en marche (apres l'ecran noir)
    return true;
}

static inline uint16_t be(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
static inline uint16_t lit(const uint16_t *img, int x, int y, bool gb)
{
    uint16_t v = img[y * SL + x];
    return gb ? be(v) : v;
}

// melange bilineaire de 4 pixels RGB565, poids en seiziemes (somme 16).
// v0.9.50 : les 3 couleurs sont ecartees dans un mot de 32 bits (vert en haut,
// rouge et bleu en bas, avec des bits vides entre elles) : une seule
// multiplication par pixel et par poids au lieu de trois. Resultat identique.
static inline uint32_t ecarte(uint16_t c) { return ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu; }
static inline uint16_t mix4(uint16_t a, uint16_t b, uint16_t c, uint16_t d, int wa, int wb, int wc, int wd)
{
    uint32_t v = ecarte(a) * wa + ecarte(b) * wb + ecarte(c) * wc + ecarte(d) * wd + 0x01004008u;   // + 8 par couleur (arrondi)
    v = (v >> 4) & 0x07E0F81Fu;
    return (uint16_t)(v | (v >> 16));
}

// Orientation relevee sur les photos des essais (v0.9.45 : image en miroir) :
// paysage (X 0..479, Y 0..319), micro a DROITE : py = 479 - X, px = Y.
// ECRAN_RETOURNE (micro a gauche) : py = X, px = 319 - Y.
// ECRAN_MIROIR : inverse le haut et le bas (secours, normalement inutile).
// Une ligne du panneau = une colonne du paysage. Ces relations sont leur propre inverse.
static inline int Y_de_px(int px)
{
    int Y = px;
#if ECRAN_RETOURNE
    Y = EH - 1 - Y;
#endif
#if ECRAN_MIROIR
    Y = EH - 1 - Y;
#endif
    return Y;
}
static inline int X_de_py(int py)
{
#if ECRAN_RETOURNE
    return py;
#else
    return EL - 1 - py;
#endif
}

// v0.9.48 : calcul par blocs de BLOC lignes du panneau (= BLOC colonnes voisines
// de l'interface). Pour chaque px, on lit 2 lignes de l'image source sur quelques
// pixels voisins : les acces a la PSRAM restent dans le cache. La version ligne par
// ligne lisait l'image en colonne (un defaut de cache par pixel) : 130 a 185 ms.
static const int BLOC = 32;                   // v0.9.49 : 8 -> 32 (4 fois moins de lectures hors cache)

static void bloc_panneau(const uint16_t *img, int py0, uint16_t *out, bool gb, int n = BLOC, bool lisse = ECRAN_LISSAGE,
                         int px_a = 0, int px_b = PW - 1)   // out = ligne py0 du panneau ; colonnes px_a..px_b
{
    int x0[BLOC], x1[BLOC], fx[BLOC];
    for (int i = 0; i < n; i++) {
        int cc = X_de_py(py0 + i) - OX;                 // bandes : on prolonge le bord de l'image
        if (cc < 0) cc = 0;
        if (cc > CW - 1) cc = CW - 1;
        x0[i] = s_x0[cc];
        fx[i] = s_fx[cc];
        x1[i] = x0[i] + 1 < SL ? x0[i] + 1 : SL - 1;
    }
    for (int px = px_a; px <= px_b; px++) {
        int Y = Y_de_px(px);
        int y0 = s_y0[Y], fy = s_fy[Y];
        int y1 = y0 + 1 < SH ? y0 + 1 : SH - 1;
        const uint16_t *l0 = img + y0 * SL, *l1 = img + y1 * SL;
        uint16_t *o = out + px;
        for (int i = 0; i < n; i++, o += PW) {
            uint16_t a = l0[x0[i]], p;
            if (gb) a = be(a);
            if (!lisse || (!fx[i] && !fy)) p = a;
            else {
                uint16_t b = l0[x1[i]], c = l1[x0[i]], d = l1[x1[i]];
                if (gb) { b = be(b); c = be(c); d = be(d); }
                int f = fx[i];
                p = mix4(a, b, c, d, (4 - f) * (4 - fy), f * (4 - fy), (4 - f) * fy, f * fy);
            }
            *o = be(p);
        }
    }
}

// Zone de l'interface qui a change depuis la derniere image (bornes incluses).
// Les lignes identiques du haut et du bas sont ecartees par memcmp, puis les
// colonnes, par mots de 32 bits. false : rien n'a change.
static bool zone_changee(const uint16_t *img, int &x0, int &y0, int &x1, int &y1)
{
    y0 = 0;
    while (y0 < SH && memcmp(img + y0 * SL, s_prec + y0 * SL, SL * 2) == 0) y0++;
    if (y0 == SH) return false;
    y1 = SH - 1;
    while (y1 > y0 && memcmp(img + y1 * SL, s_prec + y1 * SL, SL * 2) == 0) y1--;
    int g = SL / 2, d = -1;                             // en mots de 2 pixels
    for (int y = y0; y <= y1; y++) {
        const uint32_t *a = (const uint32_t *)(img + y * SL), *b = (const uint32_t *)(s_prec + y * SL);
        int w = 0;
        while (w < g && a[w] == b[w]) w++;
        if (w < g) g = w;
        w = SL / 2 - 1;
        while (w > d && a[w] == b[w]) w--;
        if (w > d) d = w;
    }
    if (d < g) { g = 0; d = SL / 2 - 1; }               // lignes differentes mais mots egaux : impossible, prudence
    x0 = g * 2;
    x1 = d * 2 + 1;
    return true;
}

static void envoyer(const uint16_t *img, bool gb)
{
    if (!s_io) return;
    if (s_plein) {
        // priorite haute le temps du calcul (sinon la boucle de l'interface,
        // meme coeur et meme priorite, se partage le processeur avec lui)
        UBaseType_t prio = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, 5);
        uint32_t t0 = micros();

        // 1) zone de l'interface qui a change (+ zone forcee par un cadre de toucher)
        int x0 = 0, y0 = 0, x1 = SL - 1, y1 = SH - 1;
        bool tout = !s_prec || !s_prec_ok;
        if (!tout) {
            bool ch = zone_changee(img, x0, y0, x1, y1);
            if (f_x1 >= 0) {
                if (!ch) { x0 = SL; y0 = SH; x1 = -1; y1 = -1; }
                if (f_x0 < x0) x0 = f_x0;
                if (f_y0 < y0) y0 = f_y0;
                if (f_x1 > x1) x1 = f_x1;
                if (f_y1 > y1) y1 = f_y1;
                ch = true;
            }
            if (!ch) { vTaskPrioritySet(NULL, prio); return; }   // rien a faire
        }
        f_x0 = SL; f_y0 = SH; f_x1 = -1; f_y1 = -1;
        if (s_prec) {
            for (int y = y0; y <= y1; y++) memcpy(s_prec + y * SL + x0, img + y * SL + x0, (x1 - x0 + 1) * 2);
            s_prec_ok = true;
        }

        // 2) zone correspondante de l'ecran : un pixel source de plus de chaque cote
        //    (lissage), agrandissement x4/3, bandes laterales si on touche un bord
        int cg = (4 * (x0 - 1)) / 3 - 1, cd = (4 * (x1 + 1)) / 3 + 2;
        int rh = (4 * (y0 - 1)) / 3 - 1, rb = (4 * (y1 + 1)) / 3 + 2;
        if (cg < 0) cg = 0;
        if (rh < 0) rh = 0;
        if (cd > CW - 1) cd = CW - 1;
        if (rb > EH - 1) rb = EH - 1;
        int Xg = cg == 0 ? 0 : OX + cg, Xd = cd == CW - 1 ? EL - 1 : OX + cd;
        int a = X_de_py(Xg), b = X_de_py(Xd);           // X <-> py et Y <-> px : relations involutives
        int py0 = a < b ? a : b, py1 = a < b ? b : a;
        a = Y_de_px(rh); b = Y_de_px(rb);
        int px0 = a < b ? a : b, px1 = a < b ? b : a;
        py0 &= ~3; px0 &= ~3; py1 |= 3; px1 |= 3;       // fenetres alignees sur 4 pixels
        if (py1 > PH - 1) py1 = PH - 1;
        if (px1 > PW - 1) px1 = PW - 1;

        // 3) calcul de cette zone seulement
        for (int py = py0; py <= py1; py += BLOC) {
            int n = py1 + 1 - py < BLOC ? py1 + 1 - py : BLOC;
            bloc_panneau(img, py, s_plein + py * PW, gb, n, ECRAN_LISSAGE, px0, px1);
        }
        uint32_t t1 = micros();
        vTaskPrioritySet(NULL, prio);

        // 4) envoi de la zone, d'un trait
        int w = px1 - px0 + 1;
        int hs = ((PW * BANDE) / w) & ~3;               // lignes par envoi (le tampon DMA fait PW*BANDE pixels)
        if (hs < 4) hs = 4;
        int k = 0;
        for (int y = py0; y <= py1; y += hs, k ^= 1) {
            int h = py1 + 1 - y < hs ? py1 + 1 - y : hs;
            for (int r = 0; r < h; r++) memcpy(s_buf[k] + r * w, s_plein + (y + r) * PW + px0, w * 2);
            fenetre(px0, y, px1, y + h - 1);
            esp_lcd_panel_io_tx_color(s_io, cmd_pixels(), s_buf[k], w * h * 2);
        }
        uint32_t t2 = micros();
        s_nb_images++;
        if (s_nb_images == 3 || s_nb_images == 100 || s_nb_images % 1000 == 0)
            Serial.printf("[ECRAN] image %lu : zone %dx%d, calcul %lu ms, envoi %lu ms\n", (unsigned long)s_nb_images,
                          w, py1 - py0 + 1, (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
        return;
    }
    int k = 0;                                          // secours sans PSRAM : calcul et envoi par bandes
    for (int y = 0; y < PH; y += BANDE, k ^= 1) {
        uint16_t *b = s_buf[k];
        bloc_panneau(img, y, b, gb, BANDE);
        // tx_param attend la fin de l'envoi precedent : l'autre tampon se remplit pendant ce temps
        fenetre(0, y, PW - 1, y + BANDE - 1);
        esp_lcd_panel_io_tx_color(s_io, cmd_pixels(), b, PW * BANDE * 2);
    }
}

void ecran_envoyer(const uint16_t *img) { envoyer(img, false); }

// v0.9.51 : banc d'essai au demarrage (rien d'autre ne tourne) : chaque etape mesuree a part
void ecran_banc(const uint16_t *img)
{
    if (!s_io || !s_plein) { Serial.println("[ECRAN] banc : pas de PSRAM pour l'image entiere"); return; }
    uint32_t t0 = micros();
    memcpy(s_plein, img, SL * SH * 2);                  // 150 Ko lus + 150 Ko ecrits en PSRAM
    memcpy(s_plein + SL * SH, img, SL * SH * 2);
    uint32_t t1 = micros();
    for (int py = 0; py < PH; py += BLOC) bloc_panneau(img, py, s_plein + py * PW, false, BLOC, false);
    uint32_t t2 = micros();
    for (int py = 0; py < PH; py += BLOC) bloc_panneau(img, py, s_plein + py * PW, false, BLOC, true);
    uint32_t t3 = micros();
    int k = 0;
    for (int y = 0; y < PH; y += BANDE, k ^= 1) {
        memcpy(s_buf[k], s_plein + y * PW, PW * BANDE * 2);
        fenetre(0, y, PW - 1, y + BANDE - 1);
        esp_lcd_panel_io_tx_color(s_io, cmd_pixels(), s_buf[k], PW * BANDE * 2);
    }
    uint32_t t4 = micros();
    volatile uint32_t somme = 0;                        // calcul pur en memoire interne (sans PSRAM)
    for (int i = 0; i < PW * BANDE; i++) somme += mix4(s_buf[0][i], s_buf[1][i], (uint16_t)i, (uint16_t)(i * 7), 6, 2, 6, 2);
    uint32_t t5 = micros();
    Serial.printf("[ECRAN] banc : copie PSRAM 300 Ko %lu ms | sans lissage %lu ms | lisse %lu ms | envoi %lu ms | %d melanges en RAM interne %lu us\n",
                  (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000), (unsigned long)((t3 - t2) / 1000),
                  (unsigned long)((t4 - t3) / 1000), PW * BANDE, (unsigned long)(t5 - t4));
    s_prec_ok = false;
    Serial.printf("[ECRAN] CPU %lu MHz, PSRAM %lu Ko\n", (unsigned long)getCpuFrequencyMhz(), (unsigned long)(ESP.getPsramSize() / 1024));
}
void ecran_envoyer_gb(const uint16_t *img) { envoyer(img, true); }

// rectangle plein en coordonnees du panneau, elargi aux multiples de 4 pixels
static void plein(int x0, int y0, int x1, int y1, uint16_t c)
{
    x0 &= ~3; y0 &= ~3; x1 |= 3; y1 |= 3;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PW - 1) x1 = PW - 1;
    if (y1 > PH - 1) y1 = PH - 1;
    if (x0 > x1 || y0 > y1) return;
    int n = (x1 - x0 + 1) * (y1 - y0 + 1);
    if (n > PW * BANDE) return;
    uint16_t v = be(c);
    for (int i = 0; i < n; i++) s_buf[0][i] = v;        // meme couleur partout : sans risque pendant un envoi
    fenetre(x0, y0, x1, y1);
    esp_lcd_panel_io_tx_color(s_io, cmd_pixels(), s_buf[0], n * 2);
}

void ecran_cadre(int x, int y, int l, int h, uint16_t c)
{
    if (!s_io || l <= 0 || h <= 0) return;
    // le cadre est dessine directement sur l'ecran : la prochaine image devra repasser dessus
    int ax = x < 0 ? 0 : x, ay = y < 0 ? 0 : y;
    int bx = x + l - 1 > SL - 1 ? SL - 1 : x + l - 1, by = y + h - 1 > SH - 1 ? SH - 1 : y + h - 1;
    if (ax < f_x0) f_x0 = ax;
    if (ay < f_y0) f_y0 = ay;
    if (bx > f_x1) f_x1 = bx;
    if (by > f_y1) f_y1 = by;
    // interface -> paysage
    int X0 = OX + x * 4 / 3, X1 = OX + (x + l) * 4 / 3 - 1;
    int Y0 = y * 4 / 3, Y1 = (y + h) * 4 / 3 - 1;
    // paysage -> panneau (memes relations que pour l'image)
    int a = Y_de_px(Y0), b = Y_de_px(Y1), e = X_de_py(X0), f = X_de_py(X1);
    int px0 = a < b ? a : b, px1 = a < b ? b : a;
    int py0 = e < f ? e : f, py1 = e < f ? f : e;
    for (int yy = py0; yy <= py1; yy += 64) {           // deux cotes verticaux du panneau, par morceaux
        int ye = yy + 63 < py1 ? yy + 63 : py1;
        plein(px0, yy, px0, ye, c);
        plein(px1, yy, px1, ye, c);
    }
    plein(px0, py0, px1, py0, c);                       // et les deux autres
    plein(px0, py1, px1, py1, c);
}

void ecran_remplir(uint16_t c)
{
    if (!s_io) return;
    s_prec_ok = false;                                  // la prochaine image repart en entier
    uint16_t v = be(c);
    for (int i = 0; i < PW * BANDE; i++) { s_buf[0][i] = v; s_buf[1][i] = v; }
    for (int y = 0; y < PH; y += BANDE) {
        fenetre(0, y, PW - 1, y + BANDE - 1);
        esp_lcd_panel_io_tx_color(s_io, cmd_pixels(), s_buf[(y / BANDE) & 1], PW * BANDE * 2);
    }
}
