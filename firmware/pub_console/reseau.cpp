// reseau.cpp - Wi-Fi, page web du telephone, base de pubs - carte seulement
// v0.6.0 : plus d'Alexa (Matter, Bluetooth) ni d'infrarouge ; routes de la
// telecommande Wi-Fi de la tele (/api/tv).
#include <Arduino.h>
#include "journal.h"
#include <vector>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <SD_MMC.h>
#include "esp_mac.h"
#include <esp_system.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include "reseau.h"
#include "carte.h"
#include "config.h"
#include "detecteur.h"
#include "empreinte.h"
#include "moteur.h"
#include "base.h"
#include "interface.h"
#include "portail.h"
#if BASE_DISTANTE
#include "distant.h"
#endif
#include "coupure.h"
#include "zipflux.h"
#include "plateforme.h"
#include "tele.h"
#include "identifiants.h"
#include <time.h>

String plat_fichiers_json();          // v0.9.58 : pub_console.ino (fichiers AUTO traites sur la carte)

namespace reseau {

// Les navigateurs (surtout sur smartphone) ouvrent des connexions "de reserve" qui n'envoient rien.
// Le WebServer d'origine attend jusqu'a 5 s sur chacune avant de traiter la suivante : les ordres
// (CAPTER...) arrivaient avec ~10 s de retard. Ici on abandonne une connexion muette apres 0,6 s.
class MonServeur : public WebServer {
public:
    using WebServer::WebServer;
    uint32_t requetes = 0;                        // requetes servies (diagnostic / auto-reparation)
    int etat_interne() const { return (int)_currentStatus; }
    void purger_muettes() {
        if (_currentStatus == HC_WAIT_READ && !_currentClient.available() && millis() - _statusChange > 600) {
            _currentClient.stop();
            _currentStatus = HC_NONE;
        }
    }
};
static MonServeur serveur(80);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Reseau s_info = {};
static char s_ap_nom[33] = "Detecteur-Pub";
static const char *AP_MDP = "pub12345";
struct Identifiants { char ssid[33]; char mdp[65]; };
static Identifiants s_wifi = {};
static volatile bool s_reconnecter = false;
static volatile uint32_t s_redemarrer = 0;             // v0.9.18 : redemarrage demande depuis le portail (date de la demande)

// mDNS : http://detecteur-pub.local
static bool s_mdns = false;
static void annoncer_mdns()
{
    if (s_mdns || !WiFi.localIP()) return;
    if (!MDNS.begin("detecteur-pub")) return;
    MDNS.addService("http", "tcp", 80);
    s_mdns = true;
}

// ------------------------------------------------------------------ outils
static String json_txt(const char *s)
{
    String o = "\"";
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// v0.9.71 : JSON fabrique dans un tampon en PSRAM, sans les dizaines de petits morceaux de texte
// (String) qui se dispersaient dans la memoire interne a chaque requete (le « plus grand bloc » baissait).
struct TamponJ {
    char *p; size_t cap, n;
    void raz() { n = 0; if (p && cap) p[0] = 0; }
    void ajout(const char *s) { while (p && *s && n + 1 < cap) p[n++] = *s++; if (p && cap) p[n] = 0; }
    void ajoutf(const char *f, ...) {
        if (!p || n + 1 >= cap) return;
        va_list a; va_start(a, f);
        int k = vsnprintf(p + n, cap - n, f, a);
        va_end(a);
        if (k > 0) n = (size_t)k < cap - n ? n + k : cap - 1;
    }
    void texte(const char *s) {                    // texte JSON entre guillemets, comme json_txt()
        ajout("\"");
        for (; s && *s && n + 8 < cap; s++) {
            uint8_t c = (uint8_t)*s;
            if (c == '"' || c == '\\') { p[n++] = '\\'; p[n++] = (char)c; }
            else if (c < 0x20) n += snprintf(p + n, cap - n, "\\u%04x", c);
            else p[n++] = (char)c;
        }
        if (p && cap) p[n] = 0;
        ajout("\"");
    }
};
static TamponJ tampon_psram(size_t cap)
{
    TamponJ t = { (char *)plat_malloc_grand(cap), cap, 0 };
    if (!t.p) t.cap = 0; else t.p[0] = 0;
    return t;
}

static void repondre(const String &txt, int code = 200) { serveur.requetes++; serveur.send(code, "text/plain; charset=utf-8", txt); }
static void repondre_json(const String &j) { serveur.requetes++; serveur.sendHeader("Cache-Control", "no-store"); serveur.send(200, "application/json", j); }

static bool chemin_valide(const String &c)
{
    return c.startsWith("/pub/") && c.indexOf("..") < 0 && c.length() < 64;
}

// envoie un fichier de la carte SD par morceaux (le mutex n'est tenu que le temps
// de lire 4 Ko), avec les requetes partielles "Range" (lecteur audio de Safari)
static void envoyer_fichier(const char *chemin, const char *type, bool telecharger)
{
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    sd_prendre();
    fs::File f = SD_MMC.open(chemin, FILE_READ);
    size_t taille = f ? f.size() : 0;
    sd_rendre();
    if (!f || !taille) { repondre("Fichier introuvable", 404); return; }
    // jamais de copie gardee par le navigateur : apres un decoupage ou une
    // suppression, on entend / telecharge toujours le fichier actuel
    serveur.sendHeader("Cache-Control", "no-store");
    size_t debut = 0, fin = taille - 1;
    bool partiel = false;
    if (serveur.hasHeader("Range")) {
        String r = serveur.header("Range");
        int tiret = r.indexOf('-');
        if (r.startsWith("bytes=") && tiret > 6) {
            debut = (size_t)r.substring(6, tiret).toInt();
            String b = r.substring(tiret + 1);
            if (b.length()) fin = (size_t)b.toInt();
            if (fin >= taille) fin = taille - 1;
            partiel = debut <= fin;
            if (!partiel) { debut = 0; fin = taille - 1; }
        }
    }
    size_t n = fin - debut + 1;
    serveur.setContentLength(n);
    serveur.sendHeader("Accept-Ranges", "bytes");
    if (partiel) {
        char cr[64];
        snprintf(cr, sizeof cr, "bytes %u-%u/%u", (unsigned)debut, (unsigned)fin, (unsigned)taille);
        serveur.sendHeader("Content-Range", cr);
    }
    if (telecharger) {
        const char *nom = strrchr(chemin, '/');
        String d = String("attachment; filename=\"") + (nom ? nom + 1 : chemin) + "\"";
        serveur.sendHeader("Content-Disposition", d);
    }
    serveur.send(partiel ? 206 : 200, type, "");
    serveur.client().setNoDelay(true);
    static uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;
    size_t pos = debut;
    while (n) {
        size_t k = n > 4096 ? 4096 : n;
        sd_prendre();
        f.seek(pos);
        size_t lu = f.read(buf, k);
        sd_rendre();
        if (!lu || serveur.client().write(buf, lu) != lu) break;
        pos += lu;
        n -= lu;
    }
    sd_prendre(); f.close(); sd_rendre();
}

static int attendre_resultat(uint32_t avant, uint32_t ms)
{
    uint32_t t0 = millis();
    while (det::nb_resultats == avant && millis() - t0 < ms) delay(50);
    return det::nb_resultats == avant ? -100 : det::resultat;
}
static void api_regler()
{
    String k = serveur.arg("cle");
    int v = serveur.arg("v").toInt();
    if (cfg::regler(k.c_str(), v)) repondre("OK");
    else repondre("Réglage refusé", 400);
}

// tous les reglages d'un coup (la page web les envoie ensemble : "Envoyer a la carte")
static void api_config_lire()
{
    char j[600];
    cfg::en_json(j, sizeof j);
    repondre_json(j);
}

static void api_config_ecrire()
{
    String corps = serveur.arg("plain");
    if (corps.length() > 1024) { repondre("Trop long", 413); return; }
    String serveur_avant = cfg::R.serveur;
    int n = cfg::depuis_json(corps.c_str());
    if (n < 0) { repondre("Une valeur est hors limites : rien n'a été changé", 400); return; }
#if BASE_DISTANTE
    if (serveur_avant != cfg::R.serveur) distant::oublier();
#endif
    char j[620];
    int k = snprintf(j, sizeof j, "{\"changes\":%d,\"config\":", n);
    cfg::en_json(j + k, sizeof j - k - 2);
    strcat(j, "}");
    repondre_json(j);
}
static void api_base()
{
    Reseau r;
    infos(r);
    char j[300];
    snprintf(j, sizeof j, "{\"mode\":%ld,\"etat\":%d,\"adr\":%s,\"nom\":%s,\"pubs\":%lu,\"ms\":%d,\"envoyees\":%lu,\"copiees\":%lu,\"reglee\":%s}",
             (long)cfg::R.base, r.base_etat, json_txt(r.base_adr).c_str(), json_txt(r.base_nom).c_str(), (unsigned long)r.base_pubs,
             r.base_ms, (unsigned long)r.base_envoyees, (unsigned long)r.base_copiees, json_txt(cfg::R.serveur).c_str());
    repondre_json(j);
}


// ------------------------------------------------------------------ v0.9.25 : base complete (ZIP)
// Rapatrier : base.bin + infos.bin + sons (/pub/audio/*.wav) en une archive ZIP sans compression,
// de longueur connue d'avance (barre de progression exacte dans le portail).
// Remplacer : le ZIP corrige est recu dans /pub/_nouvelle/, verifie, puis, apres confirmation,
// l'ancienne base est rangee dans /pub/ancienne_base/ et la nouvelle prend sa place (redemarrage).
struct FichBase { String chemin; String nom; uint32_t taille; };
static std::vector<FichBase> s_fb;
static volatile uint32_t s_zip_total = 0, s_zip_envoye = 0;
static uint32_t s_ms_sd = 0, s_ms_res = 0, s_ms_tot = 0;   // v0.9.31 : temps passe a lire la SD / a envoyer sur le reseau (dernier telechargement)
static bool s_det_avant = false;                 // detection en marche avant le transfert (v0.9.25 : elle est arretee pendant)
static bool s_det_arretee = false;
// v0.9.34 : l'economie d'energie du Wi-Fi (modem endormi entre les balises de la box) ajoute 100 a 300 ms de latence
// a chaque echange : c'est ce qui plafonne un transfert a ~100 Ko/s. Forcee a "aucune" au debut de chaque transfert.
static int wifi_sans_veille()
{
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_ps_type_t t = WIFI_PS_MIN_MODEM;
    esp_wifi_get_ps(&t);
    return (int)t;                                   // 0 = aucune economie (voulu), 1 = moderee, 2 = forte
}
static bool captation_en_cours() { Captation c; plat_captation(c); return c.etat != 0; }
static void detection_arreter() { if (!s_det_arretee) { s_det_avant = det::actif; det::actif = false; s_det_arretee = true; } }
static void detection_reprendre() { if (s_det_arretee) { det::actif = s_det_avant; s_det_arretee = false; } }

static void envoyer_zip(const char *nom_zip);
static void supprimer_dossier(const String &d);

// v0.9.54 : date de la derniere base recue du PC (portail : Bibliotheque -> Base complete)
#define SYNCHRO_FICHIER "/pub/synchro.txt"
#define CAPT_DOSSIER "/pub/captations"
static uint32_t derniere_synchro()
{
    uint32_t t = 0;
    sd_prendre();
    fs::File f = SD_MMC.open(SYNCHRO_FICHIER, FILE_READ);
    if (f) { t = (uint32_t)f.readStringUntil('\n').toInt(); f.close(); }
    sd_rendre();
    return t;
}

// v0.9.54 : les fichiers de captation sont ranges en groupes : "cap" (bandeaux entiers
// cap_NNN.wav + index.csv), "auto" (dossier auto/ : morceaux decoupes + index.csv),
// "fo" (test de l'entree optique fo_test_*.wav). Liste d'un groupe dans s_fb (pour le ZIP).
static bool groupe_valide(const String &g) { return g == "cap" || g == "auto" || g == "fo"; }
static void lister_groupe(const String &g)
{
    s_fb.clear();
    String dos = g == "auto" ? String(CAPT_DOSSIER "/auto") : String(CAPT_DOSSIER);
    sd_prendre();
    fs::File d = SD_MMC.open(dos);
    if (d) {
        for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
            if (f.isDirectory()) continue;
            String n = f.name();
            int k = n.lastIndexOf('/');
            if (k >= 0) n = n.substring(k + 1);
            bool garde;
            if (g == "auto") garde = n.endsWith(".wav") || n == "index.csv";
            else if (g == "cap") garde = (n.startsWith("cap_") && n.endsWith(".wav")) || n == "index.csv";
            else garde = n.startsWith("fo_test") && n.endsWith(".wav");
            if (garde && f.size() > 0) s_fb.push_back({ dos + "/" + n, n, (uint32_t)f.size() });
        }
        d.close();
    }
    sd_rendre();
}

static void api_cap_zip()
{
    String g = serveur.arg("g");
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    if (!groupe_valide(g)) { repondre("Groupe inconnu", 400); return; }
    if (captation_en_cours()) { repondre("Captation en cours : arrête-la d'abord", 409); return; }
    lister_groupe(g);
    if (s_fb.empty()) { repondre("Aucun fichier dans ce groupe", 404); return; }
    detection_arreter();
    wifi_sans_veille();
    String nom = String("captations_") + g + ".zip";
    envoyer_zip(nom.c_str());
    detection_reprendre();
}

// Vider un groupe : tous ses fichiers sont supprimes (auto : tout le dossier auto/)
static void api_cap_vider()
{
    String g = serveur.arg("g");
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    if (!groupe_valide(g)) { repondre("Groupe inconnu", 400); return; }
    if (captation_en_cours()) { repondre("Captation en cours : arrête-la d'abord", 409); return; }
    int n = 0;
    if (g == "auto") {
        lister_groupe(g);
        n = (int)s_fb.size();
        supprimer_dossier(CAPT_DOSSIER "/auto");    // la carte le recree a la prochaine captation AUTO
    } else {
        lister_groupe(g);
        sd_prendre();
        for (auto &f : s_fb) if (SD_MMC.remove(f.chemin)) n++;
        sd_rendre();
    }
    s_fb.clear();
    Console.printf("[PUB] Captations : groupe %s vidé (%d fichiers)\n", g.c_str(), n);
    repondre(String(n) + " fichier(s) supprimé(s)");
}

static void lister_base()
{
    s_fb.clear();
    sd_prendre();
    const char *fixes[2] = { "base.bin", "infos.bin" };
    for (int i = 0; i < 2; i++) {
        String c = String("/pub/") + fixes[i];
        fs::File f = SD_MMC.open(c, FILE_READ);
        if (f) { s_fb.push_back({ c, fixes[i], (uint32_t)f.size() }); f.close(); }
    }
    fs::File d = SD_MMC.open("/pub/audio");
    if (d) {
        for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
            String n = f.name();
            if (!f.isDirectory() && n.endsWith(".wav") && f.size() > 0) s_fb.push_back({ String("/pub/audio/") + n, String("audio/") + n, (uint32_t)f.size() });
        }
        d.close();
    }
    sd_rendre();
}

static void api_base_info()
{
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    lister_base();
    bool a_jour = false, ok = false, echec = false;
    uint32_t age = 0;
    det::sauvegarde(a_jour, age, echec);
    (void)ok;
    uint32_t tot = 0;
    zipflux::Fichier *f = (zipflux::Fichier *)malloc(sizeof(zipflux::Fichier) * (s_fb.size() + 1));
    if (f) {
        for (size_t i = 0; i < s_fb.size(); i++) f[i] = { s_fb[i].nom.c_str(), s_fb[i].taille, 0, 0 };
        tot = zipflux::taille_totale(f, (int)s_fb.size());
        free(f);
    }
    char j[320];
    snprintf(j, sizeof j, "{\"octets\":%lu,\"fichiers\":%u,\"a_jour\":%d,\"pubs\":%d,\"ms_sd\":%lu,\"ms_res\":%lu,\"ms_tot\":%lu,\"nv\":%d,\"det\":%lu,\"synchro\":%lu}", (unsigned long)tot, (unsigned)s_fb.size(), a_jour ? 1 : 0, base::nb_pubs(),
             (unsigned long)s_ms_sd, (unsigned long)s_ms_res, (unsigned long)s_ms_tot, base::nb_a_verifier(), (unsigned long)base::nb_detections(), (unsigned long)derniere_synchro());
    repondre_json(j);
}

static bool zip_ecrire(void *, const uint8_t *p, size_t n)
{
    uint32_t tdeb = millis();
    struct Chrono { uint32_t t; ~Chrono() { s_ms_res += millis() - t; } } chrono{ tdeb };
    NetworkClient &c = serveur.client();
    size_t fait = 0;
    uint32_t t0 = millis();
    while (fait < n) {
        if (!c.connected()) return false;
        size_t k = c.write(p + fait, n - fait);
        if (k) { fait += k; t0 = millis(); }
        else { if (millis() - t0 > 15000) return false; delay(2); }
    }
    return true;
}
// v0.9.54 : fichier ouvert remis a zero au debut de chaque transfert (avant : apres un
// transfert interrompu, le transfert suivant pouvait relire le fichier de l'ancien)
static fs::File s_zl_f;
static int s_zl_courant = -1;
static void zip_lire_reinit()
{
    sd_prendre();
    if (s_zl_f) s_zl_f.close();
    s_zl_courant = -1;
    sd_rendre();
}
static size_t zip_lire(void *, int i, uint32_t pos, uint8_t *buf, size_t n)
{
    fs::File &f = s_zl_f;
    int &courant = s_zl_courant;
    uint32_t tdeb = millis();
    struct Chrono { uint32_t t; ~Chrono() { s_ms_sd += millis() - t; } } chrono{ tdeb };
    sd_prendre();
    if (courant != i || !f) {
        if (f) f.close();
        f = SD_MMC.open(s_fb[i].chemin, FILE_READ);
        courant = f ? i : -1;
    }
    size_t lu = 0;
    if (f) { f.seek(pos); lu = f.read(buf, n); }
    if (pos + lu >= s_fb[i].taille && f) { f.close(); courant = -1; }
    sd_rendre();
    return lu;
}


// v0.9.31 : test de debit. m=reseau : 4 Mo depuis la memoire (sans SD) ; m=sd : 4 Mo lus sur la SD (sans reseau)
static void api_debit()
{
    String m = serveur.arg("m");
    static size_t bufn2 = 16384;
    static uint8_t *b2 = (uint8_t *)heap_caps_malloc(bufn2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b2) { repondre("Mémoire insuffisante", 500); return; }
    if (captation_en_cours()) { repondre("Captation en cours", 409); return; }
    detection_arreter();
    const uint32_t TOTAL = 2u * 1024 * 1024;
    static uint32_t var_ms[5];
    if (m == "reseau") {
        // 4 manieres d'envoyer 512 Ko chacune ; les durees (mesurees par la carte) sont lues ensuite avec m=variantes
        memset(b2, 0x55, bufn2);
        const uint32_t PAR = 524288;                                 // 512 Ko par variante (5 variantes)
        serveur.sendHeader("Cache-Control", "no-store");
        serveur.sendHeader("X-RSSI", String(WiFi.RSSI()));          // force du signal Wi-Fi de la carte (dBm)
        serveur.sendHeader("X-PS", String(wifi_sans_veille()));
        {   // v0.9.36 : memoire interne (le Wi-Fi s'etrangle si elle manque) et liaison radio
            char mem[64];
            snprintf(mem, sizeof mem, "%u/%u/%u", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            serveur.sendHeader("X-MEM", mem);
            wifi_ap_record_t ap;
            char phy[64] = "?";
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                snprintf(phy, sizeof phy, "canal %d, box %s%s%s", (int)ap.primary, ap.phy_11b ? "b" : "", ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "");
            serveur.sendHeader("X-PHY", phy);
        }
        serveur.setContentLength(PAR * 5);
        serveur.send(200, "application/octet-stream", "");
        NetworkClient &c = serveur.client();
        static const char *calmes[] = { "moteur", "ecran", "archive", "audio", "tele", "loopTask" };
        TaskHandle_t susp[6] = {};
        for (int v = 0; v < 5; v++) {
            if (v == 4) {                                            // E : tout le reste de la carte est endormi pendant la mesure
                for (int i = 0; i < 6; i++) { susp[i] = xTaskGetHandle(calmes[i]); if (susp[i]) vTaskSuspend(susp[i]); }
                delay(50);
            }
            c.setNoDelay(v != 1);                                    // B : algorithme de Nagle actif
            uint32_t t0 = millis(), fait = 0;
            while (fait < PAR) {
                if (v == 3) {                                        // D : send() direct, sans select
                    ssize_t k = ::send(c.fd(), b2, bufn2, MSG_DONTWAIT);
                    if (k > 0) fait += (uint32_t)k;
                    else if (errno == EAGAIN || errno == ENOMEM) vTaskDelay(1);
                    else break;
                } else {
                    size_t n = v == 2 ? 1440 : bufn2;                // C : blocs d'un segment
                    if (!zip_ecrire(nullptr, b2, n)) break;
                    fait += (uint32_t)n;
                }
            }
            var_ms[v] = millis() - t0;
            if (v == 4) for (int i = 0; i < 6; i++) if (susp[i]) vTaskResume(susp[i]);
        }
    } else if (m == "variantes") {
        char j[128];
        snprintf(j, sizeof j, "[%lu,%lu,%lu,%lu,%lu]", (unsigned long)var_ms[0], (unsigned long)var_ms[1], (unsigned long)var_ms[2], (unsigned long)var_ms[3], (unsigned long)var_ms[4]);
        repondre_json(j);
    } else {
        lister_base();
        uint32_t lus = 0, t0 = millis();
        for (size_t i = 0; i < s_fb.size() && lus < TOTAL; i++) {
            uint32_t pos = 0;
            while (pos < s_fb[i].taille && lus < TOTAL) {
                size_t k = s_fb[i].taille - pos > bufn2 ? bufn2 : s_fb[i].taille - pos;
                size_t l = zip_lire(nullptr, (int)i, pos, b2, k);
                if (l != k) break;
                pos += (uint32_t)l; lus += (uint32_t)l;
            }
        }
        char j[96];
        snprintf(j, sizeof j, "{\"octets\":%lu,\"ms\":%lu}", (unsigned long)lus, (unsigned long)(millis() - t0));
        repondre_json(j);
    }
    detection_reprendre();
}

// v0.9.38 : debit PC -> carte (reception) : la carte compte les octets recus et jette les donnees
static uint32_t s_up_octets = 0, s_up_t0 = 0, s_up_ms = 0;
static void api_debit_up_morceaux()
{
    HTTPUpload &u = serveur.upload();
    if (u.status == UPLOAD_FILE_START) { s_up_octets = 0; s_up_t0 = millis(); detection_arreter(); }
    else if (u.status == UPLOAD_FILE_WRITE) s_up_octets += u.currentSize;
    else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) { s_up_ms = millis() - s_up_t0; detection_reprendre(); }
}
static void api_debit_up()
{
    char j[80];
    snprintf(j, sizeof j, "{\"octets\":%lu,\"ms\":%lu}", (unsigned long)s_up_octets, (unsigned long)s_up_ms);
    repondre_json(j);
}

static void api_base_zip()
{
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    bool a_jour = false, echec = false;
    uint32_t age = 0;
    det::sauvegarde(a_jour, age, echec);
    for (int i = 0; i < 60 && !a_jour; i++) { delay(100); det::sauvegarde(a_jour, age, echec); }   // sauvegarde en cours : on attend (6 s au plus)
    if (!a_jour) { repondre("La base n'est pas encore sauvegardée sur la carte : réessaie dans quelques secondes", 409); return; }
    if (captation_en_cours()) { repondre("Captation en cours : arrête-la d'abord", 409); return; }
    detection_arreter();                         // pas de lecture / ecriture parasite pendant le transfert
    wifi_sans_veille();
    lister_base();
    if (s_fb.empty()) { detection_reprendre(); repondre("Aucune base sur la carte", 404); return; }
    envoyer_zip("base_pub.zip");
    detection_reprendre();
}

// v0.9.54 : envoie en ZIP les fichiers de s_fb (base complete, ou un dossier de captations)
static void envoyer_zip(const char *nom_zip)
{
    zip_lire_reinit();
    int n = (int)s_fb.size();
    zipflux::Fichier *f = (zipflux::Fichier *)malloc(sizeof(zipflux::Fichier) * n);
    static size_t bufn = 16384;
    static uint8_t *buf = (uint8_t *)heap_caps_malloc(bufn, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { bufn = 8192; buf = (uint8_t *)malloc(bufn); }
    if (!f || !buf) { free(f); repondre("Mémoire insuffisante", 500); return; }
    for (int i = 0; i < n; i++) f[i] = { s_fb[i].nom.c_str(), s_fb[i].taille, 0, 0 };
    uint32_t total = zipflux::taille_totale(f, n);
    s_zip_total = total; s_zip_envoye = 0;
    serveur.sendHeader("Cache-Control", "no-store");
    serveur.sendHeader("Content-Disposition", String("attachment; filename=\"") + nom_zip + "\"");
    serveur.setContentLength(total);
    serveur.send(200, "application/zip", "");
    serveur.client().setNoDelay(true);           // v0.9.28 : sans cela, chaque fin de bloc attend l'accuse du PC (~0,1 Mo/s)
    s_ms_sd = s_ms_res = 0;
    uint32_t t_zip0 = millis();
    bool ok = zipflux::envoyer(f, n, zip_ecrire, zip_lire, nullptr, buf, bufn, &s_zip_envoye);
    s_ms_tot = millis() - t_zip0;
    free(f);
    Console.printf("[PUB] %s rapatrie : %s (%lu / %lu octets) en %lu ms (SD %lu ms, reseau %lu ms)\n", nom_zip, ok ? "OK" : "interrompu", (unsigned long)s_zip_envoye, (unsigned long)total,
                   (unsigned long)s_ms_tot, (unsigned long)s_ms_sd, (unsigned long)s_ms_res);
    zip_lire_reinit();
    if (!ok) serveur.client().stop();
}

// --- reception du ZIP corrige
static zipflux::Lecteur s_zl;
static fs::File s_zf;
static bool s_zf_ignore = false;
static String s_z_err;
static uint32_t s_z_octets = 0;
static volatile uint32_t s_remplacer = 0;       // demande de remplacement (date), traite dans la boucle reseau
static int s_z_pubs = -1;

static bool zl_debut(void *, const char *nom, uint32_t)
{
    String n = nom;
    bool bon = n == "base.bin" || n == "infos.bin";
    if (n.startsWith("audio/") && n.endsWith(".wav") && n.indexOf("..") < 0 && n.length() < 24) {
        bon = true;
        for (unsigned i = 6; i < n.length() - 4; i++) if (n[i] < '0' || n[i] > '9') bon = false;
    }
    s_zf_ignore = !bon;
    if (bon) { sd_prendre(); s_zf = SD_MMC.open(String("/pub/_nouvelle/") + n, FILE_WRITE); sd_rendre(); if (!s_zf) return false; }
    return true;
}
static bool zl_donnees(void *, const uint8_t *p, size_t n)
{
    if (s_zf_ignore) return true;
    sd_prendre(); size_t k = s_zf.write(p, n); sd_rendre();
    return k == n;
}
static bool zl_fin(void *)
{
    if (!s_zf_ignore && s_zf) { sd_prendre(); s_zf.close(); sd_rendre(); }
    return true;
}
static void supprimer_dossier(const String &d)
{
    sd_prendre();
    if (SD_MMC.exists(d)) {
        fs::File r = SD_MMC.open(d);
        if (r) {
            std::vector<String> noms;
            for (fs::File f = r.openNextFile(); f; f = r.openNextFile()) noms.push_back(f.path());
            r.close();
            for (auto &n : noms) {
                fs::File t = SD_MMC.open(n);
                bool dossier = t && t.isDirectory();
                if (t) t.close();
                if (dossier) { sd_rendre(); supprimer_dossier(n); sd_prendre(); }
                else SD_MMC.remove(n);
            }
        }
        SD_MMC.rmdir(d);
    }
    sd_rendre();
}
static void api_base_envoi_morceaux()
{
    HTTPUpload &u = serveur.upload();
    if (u.status == UPLOAD_FILE_START) {
        s_zl = zipflux::Lecteur();
        s_zl.debut = zl_debut; s_zl.donnees = zl_donnees; s_zl.fin = zl_fin;
        s_z_err = ""; s_z_octets = 0; s_z_pubs = -1; s_zf_ignore = false;
        if (s_remplacer) { s_z_err = "Remplacement déjà en cours"; return; }
        if (captation_en_cours()) { s_z_err = "Captation en cours : arrête-la d'abord"; return; }
        detection_arreter();
        supprimer_dossier("/pub/_nouvelle");
        sd_prendre(); SD_MMC.mkdir("/pub/_nouvelle"); SD_MMC.mkdir("/pub/_nouvelle/audio"); sd_rendre();
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (s_z_err.length() || s_zl.etat == zipflux::Lecteur::ERREUR) return;
        s_z_octets += u.currentSize;
        s_zl.nourrir(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
        if (s_zf) { sd_prendre(); s_zf.close(); sd_rendre(); }
        detection_reprendre();
    }
}
static void api_base_envoi()
{
    if (s_z_err.length()) { repondre(s_z_err, 409); return; }
    if (s_zl.etat == zipflux::Lecteur::ERREUR || !s_zl.fini()) {
        String m = s_zl.etat == zipflux::Lecteur::ERREUR ? String("Archive refusée : ") + s_zl.erreur : String("Archive incomplète (envoi interrompu ?)");
        supprimer_dossier("/pub/_nouvelle");
        repondre(m, 400);
        return;
    }
    int n = base::pubs_du_fichier("/pub/_nouvelle/base.bin");
    if (n < 0) { supprimer_dossier("/pub/_nouvelle"); repondre("Archive refusée : base.bin absent ou invalide", 400); return; }
    s_z_pubs = n;
    char j[160];
    snprintf(j, sizeof j, "{\"pubs_nouvelle\":%d,\"pubs_actuelle\":%d,\"fichiers\":%lu,\"octets\":%lu}", n, base::nb_pubs(), (unsigned long)s_zl.nb_fichiers, (unsigned long)s_z_octets);
    repondre_json(j);
}
static void api_base_appliquer()
{
    if (serveur.arg("a") == "annuler") { supprimer_dossier("/pub/_nouvelle"); s_z_pubs = -1; repondre("Remplacement annulé"); return; }
    if (s_z_pubs < 0 || !sd_ok()) { repondre("Aucune base reçue à appliquer", 409); return; }
    detection_arreter();                         // arretee jusqu'au redemarrage : rien ne reecrit l'ancienne base
    cpr::suspendue = true;
    uint32_t t = millis();
    s_remplacer = t ? t : 1;
    repondre("Remplacement en cours : la carte redémarre dans quelques secondes");
}
// appele depuis la boucle reseau (reponse envoyee) : range l'ancienne base, installe la nouvelle, redemarre
static void remplacer_base()
{
    Console.println("[PUB] Remplacement de la base");
    supprimer_dossier("/pub/ancienne_base");
    sd_prendre();
    SD_MMC.mkdir("/pub/ancienne_base");
    const char *fx[4] = { "base.bin", "base.new", "infos.bin", "infos.new" };
    for (int i = 0; i < 4; i++) {
        String a = String("/pub/") + fx[i];
        if (SD_MMC.exists(a)) SD_MMC.rename(a, String("/pub/ancienne_base/") + fx[i]);
    }
    if (SD_MMC.exists("/pub/audio")) SD_MMC.rename("/pub/audio", "/pub/ancienne_base/audio");
    SD_MMC.rename("/pub/_nouvelle/audio", "/pub/audio");
    if (!SD_MMC.exists("/pub/audio")) SD_MMC.mkdir("/pub/audio");
    SD_MMC.rename("/pub/_nouvelle/base.bin", "/pub/base.bin");
    if (SD_MMC.exists("/pub/_nouvelle/infos.bin")) SD_MMC.rename("/pub/_nouvelle/infos.bin", "/pub/infos.bin");
    SD_MMC.rmdir("/pub/_nouvelle");
    {   // v0.9.54 : date de la derniere base recue du PC (affichee sur le portail)
        time_t t = time(nullptr);
        fs::File f = SD_MMC.open(SYNCHRO_FICHIER, FILE_WRITE);
        if (f) { f.printf("%lu\n", (unsigned long)(t > 1700000000 ? t : 0)); f.close(); }
    }
    sd_rendre();
    plat_nvs_vider();
    esp_restart();
}

static void api_pub_couper()
{
    int id = serveur.arg("id").toInt(), d = serveur.arg("debut").toInt(), f = serveur.arg("fin").toInt();
    if (id < 0 || id >= base::MAX_BLOCS || !base::bloc(id).actif || f <= d) { repondre("Découpage refusé", 400); return; }
    audio_couper(base::bloc(id).numero, d, f);
    det::commande(det::CMD_COUPER, id, d, f);
    repondre("Découpée : seul le passage choisi sera reconnu");
}

#define SAUV_DOSSIER "/pub/sauvegardes"
#define SAUV_GARDEES 5                               // v0.9.54 : sauvegardes .pub gardees (les plus recentes)
static void lister(fs::File dir, String &j, bool &premier)
{
    for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String nom = f.path();
        if (f.isDirectory()) { lister(f, j, premier); continue; }
        if (!premier) j += ",";
        j += "{\"nom\":" + json_txt(nom.c_str()) + ",\"taille\":" + String((unsigned long)f.size());
        if (nom.startsWith(SAUV_DOSSIER "/") && nom.endsWith(".pub")) {      // v0.9.21 : nombre de pubs (entete du paquet) et date d'ecriture
            uint32_t e[2] = { 0, 0 };
            if (f.size() >= 8 && f.read((uint8_t *)e, 8) == 8 && e[0] == 0x314B5050) j += ",\"nb\":" + String((unsigned long)e[1]);
            time_t t = f.getLastWrite();
            if (t > 1700000000) j += ",\"t\":" + String((unsigned long)t);
        }
        j += "}";
        premier = false;
    }
}

static void api_sd()
{
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    // ?d=/pub/captations : liste d'UN dossier, sans calcul d'espace libre (v0.8.6).
    // Avant, chaque appel comptait tout l'espace utilise de la carte (plusieurs secondes sur une
    // grosse carte) en bloquant le serveur : l'ordre CAPTER restait en attente derriere.
    String d = serveur.arg("d");
    bool dossier = d.length() > 0;
    if (dossier && (!d.startsWith("/pub/") || d.indexOf("..") >= 0)) { repondre("Chemin refusé", 400); return; }
    static double c_total = 0, c_libre = 0;
    static uint32_t c_ms = 0;
    String j = "{\"total\":";
    sd_prendre();
    if (!dossier && (c_ms == 0 || millis() - c_ms > 300000)) {          // espace : recalcule toutes les 5 min au plus
        c_total = (double)SD_MMC.totalBytes();
        c_libre = c_total - (double)SD_MMC.usedBytes();
        c_ms = millis();
    }
    j += String(c_total, 0) + ",\"libre\":" + String(c_libre, 0) + ",\"fichiers\":[";
    bool premier = true;
    fs::File racine = SD_MMC.open(dossier ? d.c_str() : "/pub");
    if (racine) lister(racine, j, premier);
    sd_rendre();
    repondre_json(j + "]}");
}

static void api_sd_fichier()
{
    String c = serveur.arg("f");
    if (!chemin_valide(c) || c.startsWith("/pub/tv/")) { repondre("Chemin refusé", 400); return; }
    const char *type = c.endsWith(".wav") ? "audio/wav" : (c.endsWith(".csv") ? "text/csv" : "application/octet-stream");
    envoyer_fichier(c.c_str(), type, true);
}

static void api_sd_suppr()
{
    String c = serveur.arg("f");
    if (!chemin_valide(c) || c == "/pub/archive.ulaw" || c == "/pub/base.bin" || c == "/pub/infos.bin") { repondre("Fichier protégé", 400); return; }
    // la cle de la telecommande Wi-Fi : par le bouton « Oublier la télé »
    if (c.startsWith("/pub/tv/")) { repondre("Utilise « Oublier la télé » (onglet Télé)", 400); return; }
    sd_prendre();
    bool ok = SD_MMC.remove(c);
    sd_rendre();
    if (ok && c.startsWith("/pub/captations/auto/")) plat_fichiers_oublier(c.substring(21).c_str());   // v0.9.58
    repondre(ok ? "Supprimé" : "Échec : fichier introuvable ou en cours d'utilisation");
}

// ---- v0.9.58 : fichiers AUTO traites sur la carte (comparer, ajouter, effacer les deja connus) ----
static void api_fi()
{
    if (serveur.method() == HTTP_POST) {
        String op = serveur.arg("op"), f = serveur.arg("f");
        int n = -1;
        if (op == "comparer") n = plat_fichiers_tous(false);
        else if (op == "nouvelles") n = plat_fichiers_tous(true);
        else if (op == "valider") n = plat_fichiers_demander(f.c_str(), true) ? 1 : 0;
        else if (op == "un") n = plat_fichiers_demander(f.c_str(), false) ? 1 : 0;
        else if (op == "suppr_connus") n = plat_fichiers_suppr_connus();
        if (n < 0) { repondre("Opération inconnue", 400); return; }
        repondre(String(n));
        return;
    }
    repondre_json(::plat_fichiers_json());
}

// ---- v0.9.58 : pubs apprises « a verifier » : liste, valider, supprimer ----
// v0.9.66 : preregages (GET : liste + actif ; POST op = appliquer | enregistrer | origine ; i ; nom)
static void api_prereg()
{
    if (serveur.method() == HTTP_POST) {
        String op = serveur.arg("op");
        int i = serveur.hasArg("i") ? serveur.arg("i").toInt() : -1;
        bool ok = false;
        if (op == "appliquer") ok = cfg::prereg_appliquer(i);
        else if (op == "enregistrer") ok = cfg::prereg_enregistrer(i, serveur.arg("nom").c_str());
        else if (op == "origine") { cfg::prereg_origine(); ok = true; }
        if (ok) repondre("OK");
        else repondre("Refusé", 400);
        return;
    }
    String r = "{\"a\":" + String(cfg::prereg_actif()) + ",\"p\":[";
    for (int i = 0; i < cfg::NB_PREREG; i++) {
        cfg::Prereglage p;
        if (!cfg::prereg(i, p)) continue;
        char l[160];
        snprintf(l, sizeof l, ",\"src\":%d,\"cm\":%d,\"bas\":%d,\"cs\":%d,\"or\":%d,\"se\":%d,\"g\":%d,\"re\":%d}",
                 (int)p.source, (int)p.coupure, (int)p.tv_bas, (int)p.coupure_s, (int)p.tv_origine, (int)p.seuil, (int)p.gain, (int)p.reecoute);
        if (i) r += ',';
        r += "{\"n\":" + json_txt(p.nom) + l;
    }
    r += "]}";
    repondre_json(r);
}

static void api_averifier()
{
    if (serveur.method() == HTTP_POST) {
        String op = serveur.arg("op");
        int id = serveur.hasArg("id") ? serveur.arg("id").toInt() : -1;
        bool une = id >= 0 && id < base::MAX_BLOCS && base::bloc(id).actif;
        bool ok = false;
        if (op == "ok" && une) ok = det::commande(det::CMD_GARDER, id);
        else if (op == "suppr" && une) ok = det::commande(det::CMD_SUPPRIMER, id);
        else if (op == "ok_tous") ok = det::commande(det::CMD_GARDER, -1);
        else if (op == "suppr_tous") ok = det::commande(det::CMD_SUPPRIMER_A_VERIFIER);
        if (ok) repondre("OK");
        else repondre("Refusé : pub introuvable ou détecteur occupé, réessaie", 409);
        return;
    }
    String r = "{\"l\":[";
    bool premier = true;
    for (int i = 0; i < base::MAX_BLOCS; i++) {
        const base::Bloc &b = base::bloc(i);
        if (!b.actif || b.type != base::PUB || !base::a_verifier(i)) continue;
        const base::Info &in = base::info(i);
        char nom[sizeof in.nom + 1];
        memcpy(nom, in.nom, sizeof in.nom); nom[sizeof in.nom] = 0;
        char l[96];
        snprintf(l, sizeof l, "\"id\":%d,\"n\":%lu,\"d\":%d,\"o\":%d,\"t\":%lu,\"r\":%d,\"a\":%d,", i, (unsigned long)b.numero, (int)b.duree_s,
                 (int)in.origine, (unsigned long)in.appris_le, base::doute(i), in.audio ? 1 : 0);
        if (!premier) r += ',';
        premier = false;
        r += "{"; r += l;
        r += "\"nom\":"; r += json_txt(nom);
        r += ",\"ch\":"; r += json_txt(in.chaine < base::NB_CHAINES && in.chaine ? base::CHAINES[in.chaine] : "");
        r += "}";
    }
    r += "]}";
    repondre_json(r);
}

// NETTOYER : supprime les sons qui ne correspondent plus a aucune pub
// (pubs supprimees, bibliotheque effacee avec une ancienne version...)
static void api_sd_nettoyer()
{
    if (!sd_ok()) { repondre("Pas de carte SD", 404); return; }
    int n = 0;
    uint32_t octets = 0;
    std::vector<String> noms;
    sd_prendre();
    fs::File d = SD_MMC.open("/pub/audio");
    if (d) {
        for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
            if (f.isDirectory()) continue;
            String nom = f.name();                    // "12.wav"
            long num = nom.toInt();
            bool garder = false;
            if (num > 0 && nom.endsWith(".wav"))
                for (int i = 0; i < base::MAX_BLOCS && !garder; i++)
                    garder = base::bloc(i).actif && (long)base::bloc(i).numero == num;
            if (!garder) { noms.push_back(String(f.path())); octets += f.size(); }
        }
    }
    for (auto &p : noms) if (SD_MMC.remove(p)) n++;
    sd_rendre();
    Console.printf("[PUB] Nettoyage SD : %d sons orphelins supprimes (%lu Ko)\n", n, (unsigned long)(octets / 1024));
    char r[96];
    if (n) snprintf(r, sizeof r, "%d son%s sans pub supprimé%s · %lu Mo libérés", n, n > 1 ? "s" : "", n > 1 ? "s" : "",
                    (unsigned long)((octets + 524288) / 1048576));
    else snprintf(r, sizeof r, "Rien à nettoyer : chaque son correspond à une pub");
    repondre(r);
}

// JOURNAL : les lignes de la console serie apres "depuis" (portail, Config)
static void api_journal()
{
    uint32_t depuis = (uint32_t)serveur.arg("depuis").toInt();
    uint32_t der = plat_journal_dernier();
    if (depuis > der) depuis = 0;                            // la carte a redemarre
    int voulu = (int)(der - depuis);
    if (voulu > JOURNAL_NB) voulu = JOURNAL_NB;
    LigneJournal *l = (LigneJournal *)heap_caps_malloc(sizeof(LigneJournal) * JOURNAL_NB, MALLOC_CAP_SPIRAM);
    if (!l) { repondre("Mémoire", 500); return; }
    int n = voulu > 0 ? plat_journal(l, voulu, der) : 0;
    String j;
    j.reserve(256 + n * 128);
    j = "{\"der\":" + String((unsigned long)der) + ",\"lignes\":[";
    for (int i = 0; i < n; i++) {
        if (i) j += ",";
        j += "{\"s\":" + String((unsigned long)l[i].seq) + ",\"ms\":" + String((unsigned long)l[i].ms) + ",\"t\":" + json_txt(l[i].txt) + "}";
    }
    heap_caps_free(l);
    repondre_json(j + "]}");
}

static void importer_et_repondre(const char *chemin)
{
    uint32_t avant = det::nb_resultats;
    det::commande(det::CMD_IMPORTER, 0, 0, 0, chemin);
    int n = attendre_resultat(avant, 20000);
    repondre(n < 0 ? String("Paquet invalide") : String(n) + " pubs ajoutées");
}

// ---------------------------------------------------------------------------
// v0.6.9 : sauvegardes (paquets .pub : empreintes seulement, sans les sons), effacement,
// captation et test optique pour le portail
// ---------------------------------------------------------------------------

static bool nom_pub_ok(const String &n)
{
    if (n.length() < 5 || n.length() > 40 || !n.endsWith(".pub")) return false;
    for (size_t i = 0; i < n.length(); i++) {
        char c = n[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return n.indexOf("..") < 0;
}

// exporte toute la bibliotheque dans SAUV_DOSSIER/<nom> ; renvoie le nombre de pubs ou -1
static int sauvegarder_vers(const String &nom)
{
    if (!sd_ok()) return -1;
    sd_prendre();
    if (!SD_MMC.exists(SAUV_DOSSIER)) SD_MMC.mkdir(SAUV_DOSSIER);
    sd_rendre();
    String chemin = String(SAUV_DOSSIER "/") + nom;
    uint32_t avant = det::nb_resultats;
    if (!det::commande(det::CMD_EXPORTER, -1, 0, 0, chemin.c_str())) return -4;
    return attendre_resultat(avant, 60000);          // v0.9.70 : 60 s (l'export est bien plus rapide, marge)
}

// v0.9.70 : raison claire d'un echec de sauvegarde
static String sauv_echec(int n)
{
    switch (n) {
    case -1: return "Sauvegarde ratée : écriture impossible sur la carte SD";
    case -2: return "Sauvegarde ratée : pas assez de mémoire (PSRAM) pour le tampon";
    case -3: return "Sauvegarde ratée : fichier incomplet sur la carte SD (carte pleine ?)";
    case -4: return "Sauvegarde ratée : détecteur occupé, réessaie";
    case -100: return "Sauvegarde ratée : pas de réponse du détecteur";
    default: return "Sauvegarde ratée (code " + String(n) + ")";
    }
}

// v0.9.70 : sauvegarde SANS bloquer le serveur web. Avant, la page attendait la fin (15 s au plus) :
// le serveur ne repondait plus, les requetes du navigateur s'empilaient en memoire interne (rouge),
// et au-dela de 15 s la page affichait « Sauvegarde impossible » alors que l'ecriture continuait.
// POST /api/sauvegarder : lance -> « EN_COURS » ; POST op=etat : « EN_COURS » ou le resultat final.
static bool sv_cours = false;
static uint32_t sv_avant = 0, sv_t0 = 0, sv_mx = 0;
static char sv_nom[24] = "";
static String sv_fin = "";
static String sauv_terminer(int n)
{
    sv_cours = false;
    if (n >= 0) {                                    // v0.9.54 : seules les 5 dernieres sauvegardes sont gardees
        sd_prendre();
        for (uint32_t k = 1; k + SAUV_GARDEES <= sv_mx + 1; k++) {
            char vieux[48];
            snprintf(vieux, sizeof vieux, SAUV_DOSSIER "/sauv_%03lu.pub", (unsigned long)k);
            if (SD_MMC.exists(vieux)) SD_MMC.remove(vieux);
        }
        sd_rendre();
        sv_fin = String(n) + " pubs sauvegardées dans " SAUV_DOSSIER "/" + sv_nom + " (" + String((unsigned long)((millis() - sv_t0 + 500) / 1000)) + " s)";
    } else sv_fin = sauv_echec(n);
    Console.printf("[PUB] %s\n", sv_fin.c_str());
    return sv_fin;
}
static void api_sauvegarder()
{
    if (serveur.arg("op") == "etat") {
        if (!sv_cours) { repondre(sv_fin.length() ? sv_fin : String("Aucune sauvegarde en cours")); return; }
        if (det::nb_resultats != sv_avant) { int n = det::resultat; String m = sauv_terminer(n); repondre(m, n >= 0 ? 200 : 500); return; }
        if (millis() - sv_t0 > 120000) { String m = sauv_terminer(-100); repondre(m, 500); return; }
        repondre("EN_COURS", 202);
        return;
    }
    if (sv_cours) { repondre("EN_COURS", 202); return; }
    if (!sd_ok()) { repondre("Pas de carte SD", 400); return; }
    uint32_t mx = 0;
    sd_prendre();
    if (!SD_MMC.exists(SAUV_DOSSIER)) SD_MMC.mkdir(SAUV_DOSSIER);
    fs::File d = SD_MMC.open(SAUV_DOSSIER);
    if (d) {
        for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
            String n = f.name();
            if (n.startsWith("sauv_")) { uint32_t k = (uint32_t)n.substring(5).toInt(); if (k > mx) mx = k; }
        }
        d.close();
    }
    sd_rendre();
    sv_mx = mx;
    snprintf(sv_nom, sizeof sv_nom, "sauv_%03lu.pub", (unsigned long)(mx + 1));
    String chemin = String(SAUV_DOSSIER "/") + sv_nom;
    sv_avant = det::nb_resultats;
    sv_t0 = millis();
    if (!det::commande(det::CMD_EXPORTER, -1, 0, 0, chemin.c_str())) { repondre(sauv_echec(-4), 503); return; }
    sv_cours = true;
    sv_fin = "";
    repondre("EN_COURS", 202);
}

static void api_restaurer()
{
    String nom = serveur.arg("nom");
    if (!nom_pub_ok(nom)) { repondre("Nom refusé", 400); return; }
    importer_et_repondre((String(SAUV_DOSSIER "/") + nom).c_str());
}

static void api_effacer()
{
    if (serveur.arg("confirme") != "EFFACER") { repondre("Confirmation manquante", 400); return; }
    // filet de securite : une sauvegarde est faite juste avant (fichier avant_effacement.pub)
    int n = base::nb_pubs() > 0 ? sauvegarder_vers("avant_effacement.pub") : 0;
    if (n < 0) { repondre("Sauvegarde de sécurité impossible : rien n'est effacé", 500); return; }
    det::commande(det::CMD_EFFACER);
    repondre("Bibliothèque effacée (copie : " SAUV_DOSSIER "/avant_effacement.pub)");
}

static int bloc_par_numero(int numero)
{
    if (numero <= 0) return -1;
    for (int i = 0; i < base::MAX_BLOCS; i++)
        if (base::bloc(i).actif && (int)base::bloc(i).numero == numero) return i;
    return -1;
}

static void api_capter_etat()
{
    Captation c;
    plat_captation(c);
    TestOptique t;
    plat_fo_test(t);
    tv::Infos tvi;
    tv::infos(tvi);
    char j[760];
    snprintf(j, sizeof j, "{\"vol\":%d,\"volmax\":%d,\"muet\":%d,\"volcible\":%d,\"chsel\":%d,\"cap\":%d,\"num\":%lu,\"s\":%.1f,\"max\":%d,\"capfins\":%lu,\"capok\":%d,\"captxt\":%s,"
             "\"auto\":%d,\"ap\":%d,\"ab\":%d,\"ag\":%d,\"ac\":%d,\"ak\":%d,\"aj\":%d,\"as\":%.1f,\"at\":%.1f,"
             "\"fo\":%d,\"fos\":%.1f,\"fon\":%.2f,\"fofins\":%lu,\"fook\":%d,\"fotxt\":%s}",
             tvi.volume_connu ? tvi.volume : -1, tvi.volume_max, tvi.muet ? 1 : 0, tvi.action != tv::RIEN ? tvi.cible : -1, plat_captation_chaine(), c.etat, (unsigned long)c.numero, c.duree_s, CAPTATION_MAX_S, (unsigned long)c.fins, c.fin_ok ? 1 : 0, json_txt(c.fin_txt).c_str(),
             c.automatique ? 1 : 0, c.apprendre ? 1 : 0, c.auto_bilan ? 1 : 0, c.auto_gardes, c.auto_connues, c.auto_courtes, c.auto_jetes, c.auto_seg_s, c.auto_total_s,
             t.etat, t.duree_s, t.niveau, (unsigned long)t.fins, t.fin_ok ? 1 : 0, json_txt(t.fin_txt).c_str());
    // v0.9.71 : la suite dans un tampon en PSRAM (au lieu de nombreux petits String en memoire interne)
    static TamponJ r = tampon_psram(4096);
    if (!r.p) { repondre("mémoire", 500); return; }
    r.raz();
    r.ajout(j);
    if (r.n) r.p[--r.n] = 0;                        // enleve le '}' final : on ajoute les derniers verdicts
    r.ajout(",\"ev\":[");
    for (int i = 0; i < c.auto_nev && i < AUTO_NB_EV; i++) {
        if (i) r.ajout(",");
        r.ajoutf("{\"t\":%d,\"x\":", (int)c.auto_ev[i].type);
        r.texte(c.auto_ev[i].txt);
        r.ajout(",\"n\":");
        r.texte(c.auto_ev[i].nom);
        r.ajout("}");
    }
    r.ajout("]");
    // v0.9.57 : bilan sur toute la duree
    r.ajoutf(",\"cs\":%.1f,\"ns\":%.1f,\"sc\":%d,\"ga\":%d,\"su\":%d,\"kl\":[", c.auto_connu_s, c.auto_non_s, c.auto_secours, c.auto_garde ? 1 : 0, c.auto_sures);
    for (int i = 0; i < c.auto_k_n && i < AUTO_NB_CONNUES; i++) r.ajoutf(i ? ",%d" : "%d", (int)c.auto_k[i]);
    r.ajout("]}");
    serveur.requetes++;
    serveur.sendHeader("Cache-Control", "no-store");
    serveur.send(200, "application/json", (const char *)r.p);
}

static void api_capter()
{
    String a = serveur.arg("a");
    if (a == "stop") { plat_captation_arreter(); repondre("Arrêt de la captation…"); return; }
    if (a == "chaine") { plat_captation_chaine_regler(serveur.arg("v").toInt()); repondre("ok"); return; }
    if (a == "annuler") { plat_captation_annuler(); repondre("Captation annulée : fichier supprimé"); return; }
    if (a == "auto") {
        const char *err = plat_captation_demarrer_auto();
        repondre(err ? String("Captation impossible : ") + err : String("Captation AUTO lancée (15 min au plus)"), err ? 409 : 200);
        return;
    }
    if (a == "apprendre") {                         // v0.9.53
        const char *err = plat_captation_demarrer_apprendre();
        repondre(err ? String("Captation impossible : ") + err : String("APPRENDRE lancé : les pubs inconnues vont dans la bibliothèque"), err ? 409 : 200);
        return;
    }
    if (a != "start") { repondre("Action inconnue", 400); return; }
    const char *err = plat_captation_demarrer();
    repondre(err ? String("Captation impossible : ") + err : String("Captation lancée (15 min au plus)"), err ? 409 : 200);
}

static void api_fo_test()
{
    const char *err = plat_fo_test_demarrer();
    repondre(err ? String("Test optique impossible : ") + err : String("Test optique lancé (10 s)"), err ? 409 : 200);
}

static fs::File s_import;
static void api_import_morceaux()
{
    HTTPUpload &u = serveur.upload();
    if (u.status == UPLOAD_FILE_START) { sd_prendre(); s_import = SD_MMC.open("/pub/import.pub", FILE_WRITE); sd_rendre(); }
    else if (u.status == UPLOAD_FILE_WRITE && s_import) { sd_prendre(); s_import.write(u.buf, u.currentSize); sd_rendre(); }
    else if (u.status == UPLOAD_FILE_END && s_import) { sd_prendre(); s_import.close(); sd_rendre(); }
}
static void api_import() { importer_et_repondre("/pub/import.pub"); }

// banque : la carte telecharge elle-meme le paquet (https) puis l'importe
static void api_banque()
{
    String url = serveur.arg("url");
    if (!url.startsWith("https://") || WiFi.status() != WL_CONNECTED) { repondre("Il faut une adresse https et la box", 400); return; }
    NetworkClientSecure cli;
    cli.setInsecure();                         // paquets d'empreintes publics : pas de donnees sensibles
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(cli, url)) { repondre("Adresse invalide", 400); return; }
    int code = http.GET();
    if (code != 200) { http.end(); repondre("Téléchargement refusé (" + String(code) + ")", 502); return; }
    int total = http.getSize();
    if (total > 8 * 1024 * 1024) { http.end(); repondre("Paquet trop gros", 413); return; }
    NetworkClient *flux = http.getStreamPtr();
    sd_prendre(); fs::File f = SD_MMC.open("/pub/banque.pub", FILE_WRITE); sd_rendre();
    static uint8_t *buf = (uint8_t *)malloc(4096);
    int lu = 0;
    uint32_t t0 = millis();
    while (f && buf && http.connected() && (total < 0 || lu < total) && millis() - t0 < 60000) {
        size_t dispo = flux->available();
        if (!dispo) { delay(5); continue; }
        int k = flux->readBytes(buf, dispo > 4096 ? 4096 : dispo);
        sd_prendre(); f.write(buf, k); sd_rendre();
        lu += k;
    }
    sd_prendre(); if (f) f.close(); sd_rendre();
    http.end();
    importer_et_repondre("/pub/banque.pub");
}

static void api_wifi()
{
    Identifiants id = {};
    strncpy(id.ssid, serveur.arg("ssid").c_str(), sizeof id.ssid - 1);
    strncpy(id.mdp, serveur.arg("mdp").c_str(), sizeof id.mdp - 1);
    if (!id.ssid[0]) { repondre("Nom du réseau manquant", 400); return; }
    plat_nvs_ecrire("wifi", &id, sizeof id);
    s_wifi = id;
    repondre(String("Connexion à ") + id.ssid + "… la carte affiche sa nouvelle adresse (onglet Réglages → Wi-Fi)");
    s_reconnecter = true;
}

// ------------------------------------------------------------------ etat
static const char *MOTS[det::NB_ETATS] = { "SILENCE", "ÉCOUTE", "PROGRAMME", "PUB ?", "PUB", "J'ENREGISTRE", "EN PAUSE" };

static void api_etat()
{
    det::Vue v;
    det::vue(v);
    char sous[96];
    const char *mot = MOTS[v.etat];
    switch (v.etat) {
    case det::PUB:
        snprintf(sous, sizeof sous, "Pub reconnue · vue %d fois", v.vus);
        for (int i = 0; i < base::MAX_BLOCS; i++)
            if (base::bloc(i).actif && (int)base::bloc(i).numero == v.numero) {
                snprintf(sous, sizeof sous, "%s · vue %d fois", base::info(i).nom, v.vus);
                break;
            }
        if (v.distant) snprintf(sous, sizeof sous, "Reconnue par la base de pubs");
        break;
    case det::PROGRAMME: snprintf(sous, sizeof sous, "Rien de reconnu · %d pub%s apprise%s", base::nb_pubs(), base::nb_pubs() > 1 ? "s" : "", base::nb_pubs() > 1 ? "s" : ""); break;
    case det::APPRENTISSAGE: snprintf(sous, sizeof sous, "Aucune pub apprise : TESTER au début d'une pub"); break;
    case det::ENREGISTREMENT: snprintf(sous, sizeof sous, "Pub inconnue : je l'enregistre. Touche ENREGISTRER à la fin de la pub."); break;
    case det::PAUSE: snprintf(sous, sizeof sous, v.test_etat ? "Détection arrêtée pendant le test (3 s)" : "Détecteur arrêté"); break;
    default: snprintf(sous, sizeof sous, "%s", cfg::R.source ? "Pas de son sur l'entrée optique" : "Télé coupée ou trop loin du micro"); break;
    }
    if (v.test_etat == 1) mot = "J'ÉCOUTE";
    char msg[80], msgd[200];
    uint16_t mc = 0;
    uint32_t mn = ui::dernier_message(msg, sizeof msg, &mc, msgd, sizeof msgd);
    tv::Infos t;
    tv::infos(t);
    bool s_ok = false, s_echec = false;
    uint32_t s_age = 0;
    det::sauvegarde(s_ok, s_age, s_echec);
    int nv = base::nb_a_verifier();
    // le serveur web traite une requete a la fois : tampons statiques en PSRAM
    static TamponJ cps = tampon_psram(1024), tx = tampon_psram(2048);
    if (!cps.p || !tx.p) { repondre("mémoire", 500); return; }
    cps.raz(); tx.raz();
    cps.ajout("[");
    {
        cpr::Coupure h[cpr::NB_HIST];
        int nh = cpr::historique(h, cpr::NB_HIST);
        uint32_t now = plat_ms();
        for (int i = 0; i < nh; i++) {
            const char *ch = h[i].chaine > 0 && h[i].chaine < base::NB_CHAINES ? base::CHAINES[h[i].chaine] : "";
            if (i) cps.ajout(",");
            cps.ajoutf("{\"n\":%d,\"s\":%d,\"c\":", h[i].numero, h[i].score);
            cps.texte(ch);
            cps.ajoutf(",\"a\":%lu,\"ok\":%d}", (unsigned long)((now - h[i].ms) / 1000u), bloc_par_numero(h[i].numero) >= 0 ? 1 : 0);
        }
        cps.ajout("]");
    }
    char heure[24] = "heure inconnue";
    if (plat_heure()) { time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt); strftime(heure, sizeof heure, "%d/%m/%Y %H:%M:%S", &lt); }
    // v0.9.63 : encadre « En ce moment » du portail (reglages en service et captation)
    char ec[240];
    {
        Captation cp;
        plat_captation(cp);
        int ng = mot::gain_niveau();
        snprintf(ec, sizeof ec,
                 "{\"src\":%d,\"g\":%d,\"gdb\":%d,\"se\":%d,\"cm\":%d,\"cs\":%d,\"bas\":%d,\"re\":%d,\"fs\":%d,"
                 "\"ce\":%d,\"ca\":%d,\"cd\":%.0f,\"cg\":%d,\"cf\":%d,\"pr\":%d,\"ml\":%u,\"mb\":%u,\"mm\":%u}",
                 (int)cfg::R.source, (int)cfg::R.gain, (ng >= 0 && ng < mot::NB_GAINS) ? mot::GAIN_DB[ng] : 0, (int)cfg::R.seuil,
                 (int)cfg::R.coupure, (int)cfg::R.coupure_s, (int)cfg::R.tv_bas, cfg::REECOUTES[cfg::R.reecoute & 3], cpr::fin_spot ? 1 : 0,
                 cp.etat, cp.apprendre ? 2 : (cp.automatique ? 1 : 0), cp.duree_s, cp.auto_gardes, cpr::coupe_fibre() ? 1 + cpr::palier() : 0, cfg::prereg_actif(),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),            // v0.9.69 : memoire interne (Ko)
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
    // textes JSON (entre guillemets, echappes) ranges a la suite dans tx
    const char *T[9];
    {
        const char *src[9] = { sous, msg, msgd, journal_cause(), tv::nom_etat(t.etat), t.ip, t.msg, heure, "" };
        for (int i = 0; i < 9; i++) { T[i] = tx.p + tx.n; tx.texte(src[i]); if (tx.n + 1 < tx.cap) tx.p[tx.n++] = 0; else T[i] = "\"\""; }
    }
    // v0.9.64 : tampon en PSRAM (la RAM interne est juste) ; le serveur web traite une requete a la fois
    const size_t JN = 3000;
    static char *j = (char *)plat_malloc_grand(JN);
    if (!j) { repondre("mémoire", 500); return; }
    snprintf(j, JN,
             "{\"etat\":%d,\"mot\":\"%s\",\"sous\":%s,\"score\":%d,\"numero\":%d,\"actif\":%d,\"enreg\":%d,\"enreg_s\":%.1f,"
             "\"test\":%d,\"test_num\":%d,\"test_appris\":%d,\"test_age\":%.1f,\"niveau\":%.1f,\"sature\":%d,\"nb_pubs\":%d,\"a_verifier\":%d,\"det\":%lu,\"fo\":%d,"
             "\"pub_jour\":%.0f,\"ecrans\":%d,\"version\":\"%s\",\"arduino\":\"%s\",\"idf\":\"%s\","
             "\"coupe\":%d,\"coupe_etat\":%d,\"coupe_reste\":%d,\"coupe_total\":%d,\"coupe_ecoule\":%d,\"libere\":%d,\"coupures\":%lu,"
             "\"msg_n\":%lu,\"msg\":%s,\"msg_d\":%s,\"msg_c\":\"#%02x%02x%02x\",\"sauve_echec\":%d,\"journal\":%d,\"demarrage\":%s,\"plantages\":%lu,"
             "\"tv\":{\"etat\":%d,\"nom\":%s,\"ip\":%s,\"appairee\":%d,\"connu\":%d,\"volume\":%d,\"max\":%d,\"muet\":%d,\"baisse\":%d,"
             "\"origine\":%d,\"action\":%d,\"depart\":%d,\"cible\":%d,\"msg\":%s,\"msg_n\":%lu,\"msg_type\":%d,\"reste\":%d},\"heure\":%s,\"cps\":%s,\"ec\":%s}",
             v.etat, mot, T[0], v.score, v.numero, det::actif ? 1 : 0, v.enreg ? 1 : 0, v.enreg_s,
             v.test_etat, v.test_num, v.test_appris ? 1 : 0, v.test_age_s, mot::niveau_db(), mot::sature() ? 1 : 0, base::nb_pubs(), nv, (unsigned long)base::nb_detections(), plat_fo_etat(),
             v.pub_jour_s, v.ecrans, PUB_VERSION, plat_version_arduino(), plat_version_idf(),
             cpr::active() ? 1 : 0, cpr::etat(), cpr::reste_s(), cpr::total_s(), cpr::ecoule_s(), cpr::libere() ? 1 : 0, (unsigned long)cpr::nb_coupures(),
             (unsigned long)mn, T[1], T[2], ((mc >> 11) & 31) * 255 / 31, ((mc >> 5) & 63) * 255 / 63, (mc & 31) * 255 / 31,
             s_echec ? 1 : 0, (int)cfg::R.journal, T[3], (unsigned long)journal_plantages(),
             t.etat, T[4], T[5], t.appairee ? 1 : 0, t.volume_connu ? 1 : 0, t.volume, t.volume_max,
             t.muet ? 1 : 0, t.baisse ? 1 : 0, t.origine, t.action, t.depart, t.cible, T[6], (unsigned long)t.msg_n, t.msg_type, t.app_reste_s, T[7], cps.p, ec);
    repondre_json(j);
}

// v0.9.64 : spectre du son (24 bandes, 0..255) pour le graphique du portail. Le calcul est celui de
// l'ecran de la carte (tres leger) ; il ne tourne que si quelqu'un le lit.
static void api_spectre()
{
    uint8_t b[mot::NB_BANDES];
    mot::spectre(b);
    char j[mot::NB_BANDES * 4 + 24];
    int n = snprintf(j, sizeof j, "{\"b\":[");
    for (int i = 0; i < mot::NB_BANDES && n < (int)sizeof j - 6; i++) n += snprintf(j + n, sizeof j - n, i ? ",%u" : "%u", (unsigned)b[i]);
    snprintf(j + n, sizeof j - n, "]}");
    repondre_json(j);
}

static void api_cmd()
{
    String c = serveur.arg("c");
    // le RESULTAT (pub connue, apprise, rien a annuler...) arrive une fraction
    // de seconde plus tard : le portail l'affiche depuis /api/etat (msg)
    if (c == "tester") { det::commande(det::CMD_TESTER); repondre("…"); }
    else if (c == "enregistrer") { det::commande(det::CMD_ENREGISTRER); repondre("…"); }   // fin de la pub : sauve l'empreinte (comme sur la carte)
    else if (c == "annuler") { det::commande(det::CMD_ANNULER); repondre("…"); }
    else if (c == "paspub") { det::commande(det::CMD_PAS_PUB); repondre("…"); }
    else if (c == "supprimer") {                       // v0.9.43 : fausse alerte : supprime la pub qui a coupe le son
        int b = bloc_par_numero(serveur.arg("n").toInt());
        if (b < 0) { repondre("Cette pub n'existe plus", 404); return; }
        det::commande(det::CMD_SUPPRIMER, b);
        repondre("Pub n°" + serveur.arg("n") + " supprimée");
    }
    else if (c == "son") { ui::remettre_son(); repondre("…"); }   // meme effet et meme message que le bouton de la carte
    else if (c == "synchroniser") { det::commande(det::CMD_SYNCHRONISER); repondre("Envoi des pubs à la base…"); }
    else if (c == "bip") { plat_son(SON_PUB_DEBUT); repondre("Bip !"); }
    else repondre("Commande inconnue", 400);
}

// telecommande Wi-Fi de la tele : a = ip (v), appairer, code (v), annuler,
// oublier, couper, retablir, diminuer, lire
static void api_tv()
{
    String a = serveur.arg("a");
    if (a == "ip") {
        String ip = serveur.arg("v");
        ip.trim();
        if (!cfg::regler_texte("tv_ip", ip.c_str())) { repondre("Adresse IP invalide (ex. 192.168.1.55)", 400); return; }
        repondre("IP de la télé : " + ip);
    }
    else if (a == "appairer") { tv::appairer(); repondre("Appairage : regarde la télé, un code va s'afficher"); }
    else if (a == "code") { tv::code(serveur.arg("v").c_str()); repondre("Code envoyé"); }
    else if (a == "annuler") { tv::annuler_appairage(); repondre("Appairage annulé"); }
    else if (a == "oublier") {
        if (serveur.arg("confirme") != "1") { repondre("Confirmation manquante", 400); return; }
        tv::oublier();
        repondre("Télé oubliée. Retire aussi « Detecteur de pub » des appareils de la télé (Paramètres → télécommandes)");
    }
    else if (a == "couper") { tv::couper(); repondre("…"); }
    else if (a == "retablir") { tv::retablir(); repondre("…"); }
    else if (a == "diminuer") { tv::diminuer(); repondre("…"); }
    else if (a == "augmenter") { tv::augmenter(); repondre("…"); }
    else if (a == "lire") { tv::lire(); repondre("…"); }
    else repondre("Action inconnue", 400);
}

static void api_reseau()
{
    Reseau r;
    infos(r);
    char j[400];
    snprintf(j, sizeof j, "{\"mode\":%d,\"ssid\":%s,\"ip\":\"%s\",\"url\":\"%s\",\"ap_mdp\":\"%s\",\"clients\":%d,\"wifi_regle\":%s}",
             r.mode, json_txt(r.ssid).c_str(), r.ip, r.url, r.ap_mdp, r.clients, json_txt(s_wifi.ssid).c_str());
    repondre_json(j);
}

static void routes()
{
    static const char *entetes[] = { "Range" };
    serveur.collectHeaders(entetes, 1);
    serveur.on("/", HTTP_GET, [] {
        // jamais de copie gardee par le navigateur : apres un nouveau flash, la page
        // du portail est toujours celle de la carte
        serveur.sendHeader("Cache-Control", "no-store");
        serveur.send_P(200, "text/html; charset=utf-8", PAGE_PORTAIL);
    });
    serveur.on("/api/etat", HTTP_GET, api_etat);
    serveur.on("/api/spectre", HTTP_GET, api_spectre);
    serveur.on("/api/cmd", HTTP_POST, api_cmd);
    serveur.on("/api/tv", HTTP_POST, api_tv);
    serveur.on("/api/regler", HTTP_POST, api_regler);
    serveur.on("/api/config", HTTP_GET, api_config_lire);
    serveur.on("/api/config", HTTP_POST, api_config_ecrire);
    serveur.on("/api/base", HTTP_GET, api_base);
    serveur.on("/api/pub/couper", HTTP_POST, api_pub_couper);
    serveur.on("/api/sd", HTTP_GET, api_sd);
    serveur.on("/api/sd/fichier", HTTP_GET, api_sd_fichier);
    serveur.on("/api/sd/suppr", HTTP_POST, api_sd_suppr);
    serveur.on("/api/fi", HTTP_GET, api_fi);                  // v0.9.58
    serveur.on("/api/fi", HTTP_POST, api_fi);
    serveur.on("/api/averifier", HTTP_GET, api_averifier);
    serveur.on("/api/prereg", HTTP_GET, api_prereg);
    serveur.on("/api/prereg", HTTP_POST, api_prereg);
    serveur.on("/api/averifier", HTTP_POST, api_averifier);
    serveur.on("/api/sd/nettoyer", HTTP_POST, api_sd_nettoyer);
    serveur.on("/api/cap/zip", HTTP_GET, api_cap_zip);          // v0.9.54
    serveur.on("/api/cap/vider", HTTP_POST, api_cap_vider);
    serveur.on("/api/journal", HTTP_GET, api_journal);
    serveur.on("/api/banque", HTTP_POST, api_banque);
    serveur.on("/api/sauvegarder", HTTP_POST, api_sauvegarder);
    serveur.on("/api/restaurer", HTTP_POST, api_restaurer);
    serveur.on("/api/effacer", HTTP_POST, api_effacer);
    serveur.on("/api/redemarrer", HTTP_POST, []() {
        Captation c;
        plat_captation(c);
        if (c.etat) { repondre("Captation en cours : arrête-la ou annule-la d'abord", 409); return; }
        repondre("Redémarrage de la carte…");
        uint32_t t = millis();
        s_redemarrer = t ? t : 1;
    });
    serveur.on("/api/import", HTTP_POST, api_import, api_import_morceaux);
    serveur.on("/api/base_info", HTTP_GET, api_base_info);
    serveur.on("/api/base_zip", HTTP_GET, api_base_zip);
    serveur.on("/api/debit", HTTP_GET, api_debit);
    serveur.on("/api/debit_up", HTTP_POST, api_debit_up, api_debit_up_morceaux);
    serveur.on("/api/base_envoi", HTTP_POST, api_base_envoi, api_base_envoi_morceaux);
    serveur.on("/api/base_appliquer", HTTP_POST, api_base_appliquer);
    serveur.on("/api/plantage_effacer", HTTP_POST, []() { journal_plantages_effacer(); repondre("Message effacé"); });
    serveur.on("/api/finspot", HTTP_GET, []() { repondre_json(cpr::fin_spot ? "{\"auto\":1}" : "{\"auto\":0}"); });
    serveur.on("/api/finspot", HTTP_POST, []() {
        uint8_t v = serveur.arg("v").toInt() ? 1 : 0;
        cpr::fin_spot = v != 0;
        plat_nvs_ecrire("finspot", &v, sizeof v);
        repondre(v ? "Coupure automatique : le son revient à la fin du spot reconnu" : "Coupure imposée : durée réglée respectée");
    });
    serveur.on("/api/sdbuf", HTTP_GET, []() {
        char j[64];
        snprintf(j, sizeof j, "{\"actif\":%d,\"choisi\":%d}", plat_sd_tampon_actif(), plat_sd_tampon_choisi());
        repondre_json(j);
    });
    serveur.on("/api/sdbuf", HTTP_POST, []() {
        if (!plat_sd_tampon_regler(serveur.arg("v").toInt())) { repondre("Valeur refusée (4, 8 ou 16 Ko)", 400); return; }
        repondre("Tampon d'écriture SD : " + serveur.arg("v") + " Ko, appliqué au prochain démarrage de la carte");
    });
    serveur.on("/api/chaines", HTTP_GET, []() {
        String j = "[";
        for (int i = 0; i < base::NB_CHAINES; i++) { if (i) j += ","; j += json_txt(i ? base::CHAINES[i] : "Auto (chaîne regardée)"); }
        repondre_json(j + "]");
    });
    serveur.on("/api/capter", HTTP_GET, api_capter_etat);
    serveur.on("/api/capter", HTTP_POST, api_capter);
    serveur.on("/api/fo", HTTP_POST, api_fo_test);
    serveur.on("/api/ecran", HTTP_POST, []() { ui::aller_onglet(serveur.arg("o").c_str()); repondre("ok"); });
    serveur.on("/api/wifi", HTTP_POST, api_wifi);
    serveur.on("/api/reseau", HTTP_GET, api_reseau);
    serveur.onNotFound([] { serveur.sendHeader("Location", "/"); serveur.send(302, "text/plain", ""); });   // portail captif
}

// ------------------------------------------------------------------ Wi-Fi
static void maj_info()
{
    Reseau r = {};
    wl_status_t st = WiFi.status();
    bool sta = st == WL_CONNECTED;
    bool ap = (WiFi.getMode() & WIFI_AP) != 0;
    if (sta) {
        r.mode = 2;
        strncpy(r.ssid, WiFi.SSID().c_str(), sizeof r.ssid - 1);
        strncpy(r.ip, WiFi.localIP().toString().c_str(), sizeof r.ip - 1);
    } else if (ap) {
        r.mode = s_wifi.ssid[0] ? 3 : 1;
        strncpy(r.ssid, s_ap_nom, sizeof r.ssid - 1);
        strncpy(r.ip, WiFi.softAPIP().toString().c_str(), sizeof r.ip - 1);
        r.clients = WiFi.softAPgetStationNum();
    } else {
        r.mode = s_wifi.ssid[0] ? 3 : 0;
    }
    snprintf(r.url, sizeof r.url, "http://%s", r.ip);
    strncpy(r.ap_mdp, AP_MDP, sizeof r.ap_mdp - 1);
#if BASE_DISTANTE
    distant::infos(r);
#endif
    portENTER_CRITICAL(&s_mux);
    s_info = r;
    portEXIT_CRITICAL(&s_mux);
}

void infos(Reseau &r)
{
    portENTER_CRITICAL(&s_mux);
    r = s_info;
    portEXIT_CRITICAL(&s_mux);
}

static void lancer_ap()
{
    WiFi.mode(WIFI_AP_STA);
    bool ok = WiFi.softAP(s_ap_nom, AP_MDP);
    Console.printf("[PUB] point d'acces %s : %s, portail http://%s\n", s_ap_nom, ok ? "OK" : "ECHEC",
                  WiFi.softAPIP().toString().c_str());
}


// v0.9.69 : memoire interne : vert (plus grand bloc >= 16 Ko : la tele peut se reconnecter),
// orange (8 a 16 Ko), rouge (moins de 8 Ko ou moins de 15 Ko libres). Une ligne dans la console a chaque
// changement : tout de suite quand ca empire, apres 10 s stables quand ca s'ameliore (pas de rafale).
static int niveau_memoire(unsigned libre_ko, unsigned bloc_ko)
{
    if (bloc_ko < 8 || libre_ko < 15) return 2;
    if (bloc_ko < 16) return 1;
    return 0;
}
static void surveiller_memoire()
{
    static int niv = 0, cand = 0;
    static uint32_t t_cand = 0;
    unsigned l = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    unsigned b = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    int n = niveau_memoire(l, b);
    if (n == niv) { cand = niv; return; }
    if (n != cand) { cand = n; t_cand = millis(); }
    if (n > niv || millis() - t_cand >= 10000) {
        static const char *NOM[] = { "VERT (OK)", "ORANGE (chargée)", "ROUGE (surcharge)" };
        niv = n;
        Console.printf("[PUB] Mémoire interne : %s · %u Ko libres, plus grand bloc %u Ko\n", NOM[n], l, b);
    }
}

static void tache_reseau(void *arg)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);             // valable avant le demarrage du Wi-Fi
    snprintf(s_ap_nom, sizeof s_ap_nom, "Detecteur-Pub-%02X%02X", mac[4], mac[5]);
    if (s_wifi.ssid[0]) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(s_wifi.ssid, s_wifi.mdp);
        Console.printf("[PUB] Wi-Fi : connexion a la box %s\n", s_wifi.ssid);
    } else lancer_ap();
    uint32_t t_sans_box = millis();                  // box injoignable depuis...
    routes();
    serveur.begin();
    bool heure = false;
    uint32_t t_essai = millis(), t_connecte = 0;
    for (;;) {
        serveur.purger_muettes();
        serveur.handleClient();
        if (s_remplacer && millis() - s_remplacer > 1500) remplacer_base();
        if (s_redemarrer && millis() - s_redemarrer > 800) {  // la reponse est partie : on range et on redemarre
            Console.println("[PUB] Redemarrage demande depuis le portail");
            delay(100);
            plat_nvs_vider();
            esp_restart();
        }
        bool sta = WiFi.status() == WL_CONNECTED;
        bool ap = (WiFi.getMode() & WIFI_AP) != 0;
        if (s_reconnecter) {                                  // nouveaux identifiants
            s_reconnecter = false;
            delay(300);                                       // laisse partir la reponse
            WiFi.disconnect();
            WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
            WiFi.begin(s_wifi.ssid, s_wifi.mdp);
            t_essai = millis();
        }
        if (!sta) {
            // point d'acces seulement en SECOURS : box injoignable depuis 2 min
            if (!ap && (!s_wifi.ssid[0] || millis() - t_sans_box > WIFI_SECOURS_MS)) {
                if (s_wifi.ssid[0]) Console.printf("[PUB] Wi-Fi : box %s injoignable depuis 2 min, point d'acces de secours\n", s_wifi.ssid);
                lancer_ap();
            }
            // nouvel essai de la box toutes les 60 s, mais PAS si quelqu'un est sur
            // le point d'acces : la recherche de la box change de canal et le coupe
            if (s_wifi.ssid[0] && millis() - t_essai > 60000 && (!ap || WiFi.softAPgetStationNum() == 0)) {
                WiFi.begin(s_wifi.ssid, s_wifi.mdp);
                t_essai = millis();
            }
            t_connecte = 0;
        }
        if (sta) {
            t_sans_box = millis();
            if (!t_connecte) {
                t_connecte = millis();
                Console.printf("[PUB] Wi-Fi connecte : %s (%s)\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
                // veille Wi-Fi coupee : la telecommande de la tele repond tout de suite
                WiFi.setSleep(false);
            }
            // connecte a la box : le point d'acces de secours s'arrete (des que personne n'est dessus)
            if (ap && millis() - t_connecte > 10000 && WiFi.softAPgetStationNum() == 0) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); WiFi.setSleep(false); }   // le changement de mode peut remettre la veille Wi-Fi
            if (!heure) { configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com"); heure = true; }
            annoncer_mdns();
        }
#if BASE_DISTANTE
        distant::boucle(sta);
#endif
        static uint32_t t_info = 0;
        if (millis() - t_info > 1000) { t_info = millis(); maj_info(); surveiller_memoire(); }
        static uint32_t t_mem = 0;
        if (millis() - t_mem > 300000) {                      // diagnostic : memoire interne toutes les 5 min
            t_mem = millis();
            Console.printf("[PUB] memoire interne libre %u Ko, plus grand bloc %u Ko, mini %u Ko · web : %lu requetes, etat %d\n",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned long)serveur.requetes, serveur.etat_interne());
            // v0.9.71 : pile jamais utilisee de chaque tache (en octets) : pour decider plus tard quelles piles
            // peuvent etre reduites. Interne = pile en memoire interne ; PSRAM = deja en PSRAM.
            static const char *NOMS[] = { "audio", "fo_flux", "fo_surv", "tele", "reseau", "nvs", "loopTask", "moteur", "archive", "ecran", "fichiers" };
            char l[300]; int k = snprintf(l, sizeof l, "[PUB] piles jamais utilisées :");
            for (const char *nm : NOMS) {
                TaskHandle_t h = xTaskGetHandle(nm);
                if (h && k < (int)sizeof l - 24) k += snprintf(l + k, sizeof l - k, " %s %u", nm, (unsigned)uxTaskGetStackHighWaterMark(h));
            }
            Console.printf("%s octets (audio, fo_flux, fo_surv, tele, reseau, nvs, loopTask en mémoire interne)\n", l);
        }
        // auto-reparation (v0.9.4) : le portail devenait injoignable alors que le ping passait.
        // Toutes les 2 min sans requete servie, la carte se connecte a son propre serveur ;
        // si ca echoue, le serveur web est relance (les autres taches ne bougent pas).
        // v0.9.57 : ce test echouait TOUJOURS sur cette carte (connexion a soi-meme refusee), meme serveur
        // en bonne sante : relance inutile toutes les 2 min. Le test est verifie une premiere fois ; s'il
        // echoue des le debut, il est desactive (un seul message) au lieu de relancer le serveur pour rien.
        static uint32_t t_auto = 0, req_vues = 0;
        static int8_t auto_test = -1;                         // -1 pas encore essaye, 1 utilisable, 0 impossible ici
        if (sta && auto_test != 0 && millis() - t_auto > 120000) {
            t_auto = millis();
            if (serveur.requetes == req_vues) {
                bool ok = false;
                for (int k = 0; k < 2 && !ok; k++) {
                    WiFiClient essai;
                    ok = essai.connect(k == 0 ? WiFi.localIP() : IPAddress(127, 0, 0, 1), 80, 1500);
                    essai.stop();
                }
                if (ok) auto_test = 1;
                else if (auto_test < 0) {
                    auto_test = 0;
                    Console.println("[PUB] serveur web : auto-test impossible sur cette carte, surveillance désactivée (sans conséquence)");
                } else {
                    Console.println("[PUB] serveur web muet : relance du serveur");
                    serveur.stop();
                    serveur.begin();
                }
            }
            req_vues = serveur.requetes;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

void preparer()
{
    // reseau donne depuis le portail (memoire de la carte), sinon celui de identifiants.h
    if (!plat_nvs_lire("wifi", &s_wifi, sizeof s_wifi) || !s_wifi.ssid[0]) {
        memset(&s_wifi, 0, sizeof s_wifi);
        strncpy(s_wifi.ssid, WIFI_BOX_SSID, sizeof s_wifi.ssid - 1);
        strncpy(s_wifi.mdp, WIFI_BOX_MDP, sizeof s_wifi.mdp - 1);
    }
    WiFi.persistent(false);
    WiFi.setHostname("detecteur-pub");
    WiFi.mode(WIFI_STA);
}

void demarrer()
{
    // coeur 1, priorite 2 : le portail repond tout de suite aux boutons
    xTaskCreatePinnedToCore(tache_reseau, "reseau", 12288, NULL, 2, NULL, 1);
}

}  // namespace reseau
