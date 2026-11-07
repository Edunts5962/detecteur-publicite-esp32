// VERSION DE CE FICHIER : v1.3 (2026-11-01)
// Dernier changement : carte ES3C35P - detection automatique du controleur
// tactile (FT6336 a 0x38 sur l'ES3C28P, ST7123 a 0x55 sur l'ES3C35P).
// Version precedente : v1.2 (2026-09-23)
// Dernier changement (feuille de route "fiab-3", uniformiser le journal) :
// tous les Console.println()/printf() sont remplaces par ESP_LOGI()/
// ESP_LOGE()/ESP_LOGW() + TAG, exactement le meme mecanisme que
// mp3_player.ino (esp_log.h) - jusqu'ici ce fichier (comme playlist.cpp)
// utilisait encore Serial.* directement, incoherent avec le reste du
// projet. Aucun changement de comportement ou de logique, uniquement la
// facon dont les messages sont journalises.
// Version precedente (v1.1) : ajout de ft6336_touch_save_calibration() et
// ft6336_touch_load_calibration() (stockage des 6 coefficients d'etalonnage
// en NVS via la bibliotheque Preferences), pour ne plus avoir a refaire
// l'etalonnage a chaque demarrage.
#include "ft6336_touch.h"
#include "journal.h"
#include <math.h>
#include <Preferences.h>
#include "esp_log.h"

static const char *TAG = "ft6336_touch";

// Registres FT6336 (famille FT6206/FT6336, documentation constructeur) :
//   0x02 = TD_STATUS  -> nombre de points touches (bits 3:0)
//   0x03 = TOUCH1_XH  -> bits 3:0 = 4 bits hauts de X
//   0x04 = TOUCH1_XL  -> 8 bits bas de X
//   0x05 = TOUCH1_YH  -> bits 3:0 = 4 bits hauts de Y
//   0x06 = TOUCH1_YL  -> 8 bits bas de Y
// Les 5 registres sont lus en UNE seule transaction I2C (et non deux comme
// dans une version precedente), pour eviter tout risque de lire un statut
// et des coordonnees qui ne correspondraient pas exactement au meme instant.
#define FT6336_REG_STATUS 0x02

// v1.3 (carte ES3C35P) : le tactile integre a l'ecran 3,5" n'est PAS un
// FT6336 (contrairement a la notice) mais un controleur Sitronix a l'adresse
// 0x55, avec la table de registres du ST7123 : adresses de registre sur
// 16 bits (poids fort d'abord), 0x0001 = etat (0x1 = encore en
// initialisation), 0x0010 = debut du compte rendu, points a partir de 0x0014,
// 7 octets par point (XH, XL, YH, YL, surface, intensite, reserve), bit 7 de
// XH = point valide, 6 bits utiles dans XH/YH. Coordonnees brutes dans le
// repere natif du panneau (320x480 portrait) : l'etalonnage a 3 points
// ci-dessous les ramene a l'ecran, quelle que soit l'orientation.
// On essaie le FT6336 (0x38), puis le ST7123 (0x55) : le meme fichier sert
// aux deux cartes.
#define ST7123_I2C_ADDR 0x55
#define ST7123_REG_STATUS 0x0001
#define ST7123_REG_INFO 0x0010
#define ST7123_NB_POINTS 5            // valeur lue sur ce controleur (registre 0x0009)

static i2c_master_dev_handle_t s_touch_dev = NULL;
static bool s_st7123 = false;

static bool st7123_lire(uint16_t reg, uint8_t *buf, size_t len)
{
    if (s_touch_dev == NULL) return false;
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_touch_dev, a, 2, buf, len, pdMS_TO_TICKS(100)) == ESP_OK;
}
static uint16_t s_screen_w = 0, s_screen_h = 0;

// Coefficients de la transformation affine ecran = f(brut), etablie par
// ft6336_touch_set_calibration(). Par defaut (avant tout etalonnage), on
// part d'une correspondance brute 1:1 - fausse dans la quasi-totalite des
// cas, mais qui evite un comportement indefini si jamais ft6336_touch_read()
// etait appelee avant calibration.
static float s_a = 1.0f, s_b = 0.0f, s_c = 0.0f;
static float s_d = 0.0f, s_e = 1.0f, s_f = 0.0f;

static bool ft6336_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_touch_dev == NULL)
    {
        return false;
    }
    esp_err_t err = i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
    return err == ESP_OK;
}

bool ft6336_touch_init(i2c_master_bus_handle_t bus, gpio_num_t rst_pin,
                        uint16_t screen_w, uint16_t screen_h)
{
    s_screen_w = screen_w;
    s_screen_h = screen_h;

    // Reset materiel (actif a l'etat bas) : valable pour les deux controleurs.
    pinMode((int)rst_pin, OUTPUT);
    digitalWrite((int)rst_pin, LOW);
    delay(10);
    digitalWrite((int)rst_pin, HIGH);
    delay(150);                         // ST7123 : ~100 ms avant de repondre (50 ms ne suffisaient pas)

    // Le tactile est un peripherique de plus sur le bus I2C du codec ES8311
    // (un seul maitre I2C, plusieurs adresses). Plusieurs essais pendant 1 s.
    uint16_t adr = 0;
    for (int essai = 0; essai < 20 && !adr; essai++)
    {
        if (i2c_master_probe(bus, FT6336_I2C_ADDR, 50) == ESP_OK) adr = FT6336_I2C_ADDR;
        else if (i2c_master_probe(bus, ST7123_I2C_ADDR, 50) == ESP_OK) adr = ST7123_I2C_ADDR;
        else delay(50);
    }
    if (!adr)
    {
        ESP_LOGE(TAG, "Aucun controleur tactile (ni 0x38, ni 0x55)");
        Serial.print("[PUB] Tactile : rien a 0x38 ni 0x55 ; bus I2C :");
        for (uint8_t a = 1; a < 127; a++)
            if (i2c_master_probe(bus, a, 20) == ESP_OK) Serial.printf(" 0x%02X", a);
        Serial.println();
        return false;
    }
    s_st7123 = adr == ST7123_I2C_ADDR;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = adr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_touch_dev);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Ajout sur le bus I2C echoue (%s)", esp_err_to_name(err));
        s_touch_dev = NULL;
        return false;
    }
    if (s_st7123)
    {
        // le controleur demande jusqu'a ~20 ms apres le reset : on attend
        // qu'il ne soit plus "en initialisation"
        uint8_t st = 1;
        for (int i = 0; i < 30; i++)
        {
            if (st7123_lire(ST7123_REG_STATUS, &st, 1) && (st & 0x0F) != 0x1) break;
            delay(10);
        }
        Serial.printf("[PUB] Tactile ST7123 (0x55), etat %02X\n", st);
    }
    else
    {
        ESP_LOGI(TAG, "Tactile FT6336 (0x38)");
    }
    return true;
}

static bool st7123_get_raw(int *raw_x, int *raw_y)
{
    uint8_t buf[4 + ST7123_NB_POINTS * 7];
    if (!st7123_lire(ST7123_REG_INFO, buf, sizeof buf)) return false;
    for (int i = 0; i < ST7123_NB_POINTS; i++)
    {
        const uint8_t *p = buf + 4 + i * 7;
        if (!(p[0] & 0x80)) continue;            // pas de doigt sur ce point
        *raw_x = ((p[0] & 0x3F) << 8) | p[1];
        *raw_y = ((p[2] & 0x3F) << 8) | p[3];
        return true;
    }
    return false;
}

bool ft6336_touch_get_raw(int *raw_x, int *raw_y)
{
    if (s_st7123) return st7123_get_raw(raw_x, raw_y);
    uint8_t buf[5]; // TD_STATUS, XH, XL, YH, YL
    if (!ft6336_read_regs(FT6336_REG_STATUS, buf, 5))
    {
        return false;
    }
    if ((buf[0] & 0x0F) == 0)
    {
        return false; // aucun doigt detecte
    }
    *raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    *raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    return true;
}

bool ft6336_touch_set_calibration(int raw_x0, int raw_y0, int screen_x0, int screen_y0,
                                   int raw_x1, int raw_y1, int screen_x1, int screen_y1,
                                   int raw_x2, int raw_y2, int screen_x2, int screen_y2)
{
    // Etalonnage affine a 3 points (methode standard de calibration d'ecran
    // tactile) : on resout le systeme lineaire
    //   screen_x = A*raw_x + B*raw_y + C
    //   screen_y = D*raw_x + E*raw_y + F
    // a partir de 3 couples (point ecran connu, point brut mesure) non
    // alignes. Contrairement a une simple mise a l'echelle par axe, cette
    // methode gere automatiquement une eventuelle permutation X/Y et/ou
    // inversion d'axe due au montage physique du capteur tactile, sans
    // avoir besoin de la deviner a l'avance - formules verifiees par calcul
    // direct (regle de Cramer sur le systeme 3x3 correspondant).
    float xm0 = raw_x0, ym0 = raw_y0, xm1 = raw_x1, ym1 = raw_y1, xm2 = raw_x2, ym2 = raw_y2;
    float xr0 = screen_x0, yr0 = screen_y0, xr1 = screen_x1, yr1 = screen_y1, xr2 = screen_x2, yr2 = screen_y2;

    float delta = (xm0 - xm2) * (ym1 - ym2) - (xm1 - xm2) * (ym0 - ym2);
    if (fabsf(delta) < 1e-6f)
    {
        ESP_LOGE(TAG, "Points d'etalonnage alignes ou identiques, calibration ignoree");
        return false;
    }

    s_a = ((xr0 - xr2) * (ym1 - ym2) - (xr1 - xr2) * (ym0 - ym2)) / delta;
    s_b = ((xr1 - xr2) * (xm0 - xm2) - (xr0 - xr2) * (xm1 - xm2)) / delta;
    s_c = xr2 - s_a * xm2 - s_b * ym2;

    s_d = ((yr0 - yr2) * (ym1 - ym2) - (yr1 - yr2) * (ym0 - ym2)) / delta;
    s_e = ((yr1 - yr2) * (xm0 - xm2) - (yr0 - yr2) * (xm1 - xm2)) / delta;
    s_f = yr2 - s_d * xm2 - s_e * ym2;

    ESP_LOGI(TAG, "Etalonnage tactile enregistre");
    return true;
}

void ft6336_touch_get_coeffs(float out[6])
{
    out[0] = s_a; out[1] = s_b; out[2] = s_c;
    out[3] = s_d; out[4] = s_e; out[5] = s_f;
}

void ft6336_touch_set_coeffs(const float in[6])
{
    s_a = in[0]; s_b = in[1]; s_c = in[2];
    s_d = in[3]; s_e = in[4]; s_f = in[5];
}

void ft6336_touch_map_raw(int raw_x, int raw_y, int *x, int *y)
{
    // v2.1-BETA-LVGL : arrondi au pixel le plus proche (lroundf) au lieu
    // de la troncature (int) de la v1.39, qui decalait systematiquement
    // les points d'un demi-pixel vers le haut et la gauche en moyenne.
    int sx = (int)lroundf(s_a * raw_x + s_b * raw_y + s_c);
    int sy = (int)lroundf(s_d * raw_x + s_e * raw_y + s_f);

    if (sx < 0) sx = 0;
    if (s_screen_w > 0 && sx > (int)s_screen_w - 1) sx = s_screen_w - 1;
    if (sy < 0) sy = 0;
    if (s_screen_h > 0 && sy > (int)s_screen_h - 1) sy = s_screen_h - 1;

    *x = sx;
    *y = sy;
}

// Espace de noms NVS dedie a cet etalonnage (isole des autres eventuelles
// preferences du projet). Les 6 coefficients de la transformation affine
// (s_a..s_f) sont stockes un par un sous forme de "float", plus un
// indicateur booleen "valid" qui ne passe a true qu'une fois les 6 valeurs
// effectivement ecrites - evite de considerer une NVS partiellement remplie
// (ex. coupure de courant en plein milieu de l'ecriture) comme un
// etalonnage valide.
static const char *TOUCH_CAL_NVS_NS = "touchcal35";   // v0.9.46 : nouvel espace -> nouvel etalonnage (l'ancien a ete fait sur l'image en miroir)

void ft6336_touch_save_calibration()
{
    Preferences prefs;
    if (!prefs.begin(TOUCH_CAL_NVS_NS, false /* lecture/ecriture */))
    {
        ESP_LOGW(TAG, "Ouverture NVS (ecriture) echouee, etalonnage non sauvegarde");
        return;
    }
    // "valid" est efface AVANT d'ecrire les nouvelles valeurs, puis remis a
    // true seulement une fois les 6 ecrites : une coupure de courant pendant
    // l'operation laisse alors un enregistrement clairement invalide plutot
    // qu'un melange incoherent d'anciennes et de nouvelles valeurs.
    prefs.putBool("valid", false);
    prefs.putFloat("a", s_a);
    prefs.putFloat("b", s_b);
    prefs.putFloat("c", s_c);
    prefs.putFloat("d", s_d);
    prefs.putFloat("e", s_e);
    prefs.putFloat("f", s_f);
    prefs.putBool("valid", true);
    prefs.end();
    ESP_LOGI(TAG, "Etalonnage tactile sauvegarde en memoire flash (NVS)");
}

bool ft6336_touch_load_calibration()
{
    Preferences prefs;
    // true = ouverture en lecture seule (ne cree pas l'espace de noms s'il
    // n'existe pas encore, ce qui est exactement le cas au tout premier
    // demarrage - evite de "polluer" la NVS pour rien dans ce cas).
    if (!prefs.begin(TOUCH_CAL_NVS_NS, true))
    {
        return false;
    }
    bool valid = prefs.getBool("valid", false);
    if (!valid)
    {
        prefs.end();
        return false;
    }
    s_a = prefs.getFloat("a", 1.0f);
    s_b = prefs.getFloat("b", 0.0f);
    s_c = prefs.getFloat("c", 0.0f);
    s_d = prefs.getFloat("d", 0.0f);
    s_e = prefs.getFloat("e", 1.0f);
    s_f = prefs.getFloat("f", 0.0f);
    prefs.end();
    ESP_LOGI(TAG, "Etalonnage tactile charge depuis la memoire flash (NVS)");
    return true;
}

bool ft6336_touch_read(int *x, int *y)
{
    int rx, ry;
    if (!ft6336_touch_get_raw(&rx, &ry))
    {
        return false;
    }
    ft6336_touch_map_raw(rx, ry, x, y);
    return true;
}
