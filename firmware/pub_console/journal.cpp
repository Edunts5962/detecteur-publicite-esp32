// journal.cpp - console serie gardee en memoire (voir journal.h)
#include "journal.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <stdarg.h>

// les 32 dernieres lignes sont aussi gardees en memoire RTC : elle survit a un
// plantage, a un chien de garde ou a esp_restart (pas a une coupure de courant).
// Au demarrage suivant, on sait ce que faisait la carte juste avant.
static const int RTC_NB = 32, RTC_L = 96;
static const uint32_t RTC_MAGIE = 0x4A524E31;   // "JRN1"
RTC_NOINIT_ATTR static uint32_t rtc_magie;
RTC_NOINIT_ATTR static uint32_t rtc_n;
RTC_NOINIT_ATTR static uint32_t rtc_plantages;  // redemarrages anormaux depuis la mise sous tension
RTC_NOINIT_ATTR static char rtc_lignes[RTC_NB][RTC_L];
static char (*s_avant)[RTC_L] = nullptr;          // copie des lignes d'avant le redemarrage
static int s_avant_n = 0;
static esp_reset_reason_t s_cause = ESP_RST_UNKNOWN;

ConsoleSerie Console;

static LigneJournal *s_lignes = nullptr;   // anneau en PSRAM
static uint32_t s_seq = 0;                 // numero de la derniere ligne ecrite (0 = aucune)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// deux lignes en cours de construction : notre console, et les messages du systeme
// (ecrits par d'autres taches, on ne les melange pas)
struct EnCours { char b[JOURNAL_LARGEUR]; int n; };
static EnCours s_console = {}, s_systeme = {};
static vprintf_like_t s_vprintf_avant = nullptr;
static volatile bool s_actif = true;

void journal_activer(bool oui) { s_actif = oui; }
bool journal_actif() { return s_actif; }

// journal coupe : seules les erreurs sont gardees pour l'ecran et le portail
static bool erreur(const char *s)
{
    return (s[0] == 'E' && s[1] == ' ' && s[2] == '(') || strstr(s, "ECHEC") || strstr(s, "ERREUR") || strstr(s, "PLANTAGE") ||
           strstr(s, "Erreur") || strstr(s, "chec");
}

static void pousser_ligne_verrouille(EnCours &e)
{
    // enlever les fins de ligne et les codes couleur ANSI (ESC [ ... m) des logs ESP-IDF
    char propre[JOURNAL_LARGEUR];
    int k = 0;
    for (int i = 0; i < e.n && k < JOURNAL_LARGEUR - 1; i++) {
        char c = e.b[i];
        if (c == 0x1B) {                          // ESC [ ... lettre
            while (i < e.n && !((e.b[i] >= 'A' && e.b[i] <= 'Z') || (e.b[i] >= 'a' && e.b[i] <= 'z'))) i++;
            continue;
        }
        if (c == '\r' || c == '\n') continue;
        propre[k++] = c;
    }
    propre[k] = 0;
    e.n = 0;
    if (!k || !s_lignes) return;
    if (rtc_magie == RTC_MAGIE) {
        int r = (int)(rtc_n % RTC_NB);
        memcpy(rtc_lignes[r], propre, k < RTC_L - 1 ? k + 1 : RTC_L);
        rtc_lignes[r][RTC_L - 1] = 0;
        rtc_n++;
    }
    if (!s_actif && !erreur(propre)) return;
    s_seq++;
    LigneJournal &l = s_lignes[s_seq % JOURNAL_NB];
    l.seq = s_seq;
    l.ms = millis();
    memcpy(l.txt, propre, k + 1);
}

static void ajouter(EnCours &e, const uint8_t *b, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        char c = (char)b[i];
        if (c == '\n') { pousser_ligne_verrouille(e); continue; }
        if (e.n < JOURNAL_LARGEUR - 1) e.b[e.n++] = c;
        else { pousser_ligne_verrouille(e); e.b[e.n++] = c; }   // ligne tres longue : coupee
    }
    portEXIT_CRITICAL(&s_mux);
}

void ConsoleSerie::begin(unsigned long vitesse) { Serial.begin(vitesse); }
size_t ConsoleSerie::write(uint8_t c) { return write(&c, 1); }
size_t ConsoleSerie::write(const uint8_t *b, size_t n)
{
    ajouter(s_console, b, n);
    return Serial.write(b, n);
}

// messages du systeme (ESP_LOGx, Wi-Fi) : copies dans le journal, puis
// envoyes comme avant sur le port serie
static int capter_log(const char *fmt, va_list args)
{
    // les messages du systeme viennent de taches parfois a petite pile (Wi-Fi,
    // minuteries) : si la pile de la tache est deja presque pleine, on ne les
    // copie pas (le port serie les recoit quand meme)
    if (uxTaskGetStackHighWaterMark(NULL) < 1536) return s_vprintf_avant ? s_vprintf_avant(fmt, args) : vprintf(fmt, args);
    char b[160];
    va_list copie;
    va_copy(copie, args);
    int n = vsnprintf(b, sizeof b, fmt, copie);
    va_end(copie);
    if (n > 0) ajouter(s_systeme, (const uint8_t *)b, n < (int)sizeof b ? n : (int)sizeof b - 1);
    return s_vprintf_avant ? s_vprintf_avant(fmt, args) : vprintf(fmt, args);
}

static const char *texte_cause(esp_reset_reason_t c)
{
    switch (c) {
    case ESP_RST_POWERON: return "mise sous tension";
    case ESP_RST_EXT: return "bouton reset";
    case ESP_RST_SW: return "redemarrage voulu (logiciel)";
    case ESP_RST_PANIC: return "PLANTAGE (erreur du programme)";
    case ESP_RST_INT_WDT: return "PLANTAGE (chien de garde des interruptions)";
    case ESP_RST_TASK_WDT: return "PLANTAGE (chien de garde des taches)";
    case ESP_RST_WDT: return "PLANTAGE (chien de garde)";
    case ESP_RST_DEEPSLEEP: return "reveil";
    case ESP_RST_BROWNOUT: return "ALIMENTATION trop faible (chute de tension)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "inconnue";
    }
}

bool journal_anormal()
{
    return s_cause == ESP_RST_PANIC || s_cause == ESP_RST_INT_WDT || s_cause == ESP_RST_TASK_WDT || s_cause == ESP_RST_WDT ||
           s_cause == ESP_RST_BROWNOUT;
}
const char *journal_cause() { return texte_cause(s_cause); }
void journal_plantages_effacer() { rtc_plantages = 0; }
uint32_t journal_plantages() { return rtc_magie == RTC_MAGIE ? rtc_plantages : 0; }

void journal_init()
{
    if (s_lignes) return;
    s_lignes = (LigneJournal *)heap_caps_calloc(JOURNAL_NB, sizeof(LigneJournal), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cause = esp_reset_reason();
    // memoire RTC : valable seulement si elle a survecu (pas apres une mise sous tension)
    if (s_cause == ESP_RST_POWERON || rtc_magie != RTC_MAGIE || rtc_n > 100000000u) {
        rtc_magie = RTC_MAGIE;
        rtc_n = 0;
        rtc_plantages = 0;
    } else if (rtc_n) {
        int n = rtc_n < (uint32_t)RTC_NB ? (int)rtc_n : RTC_NB;
        s_avant = (char(*)[RTC_L])heap_caps_malloc((size_t)n * RTC_L, MALLOC_CAP_SPIRAM);
        if (s_avant) {
            for (int i = 0; i < n; i++) {
                memcpy(s_avant[i], rtc_lignes[(rtc_n - n + i) % RTC_NB], RTC_L);
                s_avant[i][RTC_L - 1] = 0;
            }
            s_avant_n = n;
        }
        rtc_n = 0;
    }
    if (journal_anormal()) rtc_plantages++;
    s_vprintf_avant = esp_log_set_vprintf(capter_log);
}

void journal_rapport_demarrage()
{
    Console.printf("[PUB] Demarrage : cause = %s", texte_cause(s_cause));
    if (journal_plantages()) Console.printf(" (%lu redemarrage(s) anormal(aux) depuis la mise sous tension)", (unsigned long)journal_plantages());
    Console.println();
    if (s_avant_n && journal_anormal()) {
        Console.println("[PUB] ---- dernieres lignes AVANT le redemarrage ----");
        for (int i = 0; i < s_avant_n; i++) Console.printf("avant | %s\n", s_avant[i]);
        Console.println("[PUB] ---- fin des lignes d'avant ----");
    }
    if (s_avant) { heap_caps_free(s_avant); s_avant = nullptr; s_avant_n = 0; }
}

uint32_t plat_journal_dernier()
{
    portENTER_CRITICAL(&s_mux);
    uint32_t s = s_seq;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

int plat_journal(LigneJournal *out, int max, uint32_t jusqua)
{
    if (!s_lignes || max <= 0) return 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t fin = (jusqua == 0 || jusqua > s_seq) ? s_seq : jusqua;
    uint32_t plus_vieux = s_seq > (uint32_t)JOURNAL_NB ? s_seq - JOURNAL_NB + 1 : 1;
    uint32_t debut = fin + 1 > (uint32_t)max ? fin + 1 - max : 1;
    if (debut < plus_vieux) debut = plus_vieux;
    int n = 0;
    for (uint32_t q = debut; q <= fin && fin > 0; q++) out[n++] = s_lignes[q % JOURNAL_NB];
    portEXIT_CRITICAL(&s_mux);
    return n;
}
