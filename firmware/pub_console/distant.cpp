// distant.cpp - client de la base de pubs decentralisee
#include "config.h"
#if BASE_DISTANTE   // v0.9.53 : tout le fichier est exclu si la base distante est desactivee
#include <Arduino.h>
#include "journal.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include "esp_mac.h"
#include "distant.h"
#include "config.h"
#include "detecteur.h"
#include "empreinte.h"

namespace distant {

static WiFiUDP udp;
static bool udp_ok = false;
static IPAddress ip_base;
static bool trouvee = false;
static uint16_t port_http = PORT_HTTP;
static char nom_base[32] = "";
static uint32_t pubs_base = 0;
static uint32_t client_id = 0, seq = 0;
static uint32_t t_envoi = 0, t_reponse = 0, t_cherche = 0, t_envoi_q = 0;
static int latence = 0;
static bool joignable = false;
static uint32_t envoyees = 0, copiees = 0;
static uint32_t dl_id = 0, dl_t = 0;          // dernier telechargement tente
static char adr_prec[48] = "\x01";

static const uint32_t PD = 0x31304450;        // "PD01" : qui est la base ?
static const uint32_t PE = 0x31304550;        // "PE01" : c'est moi
static const uint32_t PQ = 0x31305150;        // "PQ01" : codes entendus
static const uint32_t PR = 0x31305250;        // "PR01" : pub reconnue

#pragma pack(push, 1)
struct EnteteQ { uint32_t magie, client, seq; uint16_t n, drapeaux; };
struct CodeQ { uint32_t h, t; };
struct Reponse {
    uint32_t magie, seq, id;
    uint16_t score, second;
    int32_t pos;
    uint32_t t_dernier;
    uint8_t type, chaine;
    uint16_t duree_s, score_fort, reserve;
    char nom[28];
};
#pragma pack(pop)

void oublier() { trouvee = false; joignable = false; adr_prec[0] = 1; }

static void chercher()
{
    // adresse donnee dans les reglages ? sinon on demande a tout le reseau
    const char *adr = cfg::R.serveur;
    if (strcmp(adr, adr_prec) != 0) { strncpy(adr_prec, adr, sizeof adr_prec - 1); trouvee = false; }
    if (trouvee) return;
    if (millis() - t_cherche < 4000) return;
    t_cherche = millis();
    if (adr[0]) {
        char hote[48];
        strncpy(hote, adr, sizeof hote - 1);
        hote[sizeof hote - 1] = 0;
        char *deuxp = strchr(hote, ':');
        if (deuxp) *deuxp = 0;
        IPAddress ip;
        bool ok = ip.fromString(hote);
        if (!ok) {
            size_t n = strlen(hote);
            if (n > 6 && strcmp(hote + n - 6, ".local") == 0) {
                hote[n - 6] = 0;
                ip = MDNS.queryHost(hote, 1500);
                ok = ip != IPAddress(0, 0, 0, 0);
            } else ok = WiFi.hostByName(hote, ip) == 1;
        }
        if (!ok) return;
        ip_base = ip;                                 // on lui demande son nom (et on verifie qu'elle repond)
        uint32_t m = PD;
        udp.beginPacket(ip_base, PORT_UDP);
        udp.write((const uint8_t *)&m, 4);
        udp.endPacket();
    } else {
        uint32_t m = PD;                              // diffusion sur le reseau de la maison
        udp.beginPacket(IPAddress(255, 255, 255, 255), PORT_UDP);
        udp.write((const uint8_t *)&m, 4);
        udp.endPacket();
    }
}

static void recevoir()
{
    static uint8_t buf[256];
    for (int boucle = 0; boucle < 8; boucle++) {
        int n = udp.parsePacket();
        if (n <= 0) return;
        int k = udp.read(buf, sizeof buf);
        if (k < 4) continue;
        uint32_t m;
        memcpy(&m, buf, 4);
        if (m == PE && k >= 10) {                     // annonce d'une base
            if (!trouvee) {
                ip_base = udp.remoteIP();
                memcpy(&port_http, buf + 4, 2);
                memcpy(&pubs_base, buf + 6, 4);
                int l = k - 10 < (int)sizeof nom_base - 1 ? k - 10 : (int)sizeof nom_base - 1;
                memcpy(nom_base, buf + 10, l);
                nom_base[l] = 0;
                trouvee = true;
                t_reponse = millis();
                Console.printf("[PUB] Base trouvee : %s (%s), %lu pubs\n", nom_base, ip_base.toString().c_str(), (unsigned long)pubs_base);
            } else if (udp.remoteIP() == ip_base) memcpy(&pubs_base, buf + 6, 4);
        } else if (m == PR && k >= (int)sizeof(Reponse) && udp.remoteIP() == ip_base) {
            Reponse r;
            memcpy(&r, buf, sizeof r);
            uint32_t maintenant = millis();
            if (!joignable) det::commande(det::CMD_SYNCHRONISER);   // (re)trouvee : on lui envoie ce qui lui manque
            joignable = true;
            latence = (int)(maintenant - t_envoi_q);
            t_reponse = maintenant;
            if (r.id && r.score_fort >= 3) {
                det::ResDistant d = {};
                d.id = r.id; d.score = r.score; d.t_dernier = r.t_dernier; d.pos = r.pos;
                d.type = r.type; d.chaine = r.chaine; d.duree_s = r.duree_s;
                memcpy(d.nom, r.nom, sizeof d.nom);
                d.nom[sizeof d.nom - 1] = 0;
                det::distant_resultat(d);
            }
        }
    }
}

static void envoyer_codes()
{
    if (millis() - t_envoi < 100) return;
    t_envoi = millis();
    static emp::Code *codes = (emp::Code *)malloc(512 * sizeof(emp::Code));   // 4 Ko : PSRAM (malloc > 4 Ko)
    if (!codes) return;
    int n = det::distant_codes(codes, 512);
    // meme sans code (silence), on donne signe de vie toutes les 2 s
    if (n == 0 && millis() - t_envoi_q < 2000) return;
    static uint8_t *paquet = (uint8_t *)plat_malloc_grand(sizeof(EnteteQ) + 170 * sizeof(CodeQ));   // PSRAM
    if (!paquet) return;
    int i = 0;
    do {
        int k = n - i > 170 ? 170 : n - i;
        EnteteQ e = { PQ, client_id, ++seq, (uint16_t)k, 0 };
        memcpy(paquet, &e, sizeof e);
        for (int j = 0; j < k; j++) {
            CodeQ c = { codes[i + j].h, codes[i + j].t };
            memcpy(paquet + sizeof e + j * sizeof(CodeQ), &c, sizeof c);
        }
        udp.beginPacket(ip_base, PORT_UDP);
        udp.write(paquet, sizeof e + k * sizeof(CodeQ));
        udp.endPacket();
        i += k;
    } while (i < n);
    t_envoi_q = millis();
}

static String url(const char *chemin)
{
    return String("http://") + ip_base.toString() + ":" + String(port_http) + chemin;
}

// une pub reconnue par la base, inconnue ici : on la copie
static void telecharger()
{
    uint32_t id = det::distant_a_telecharger();
    if (!id || (id == dl_id && millis() - dl_t < 30000)) return;
    dl_id = id;
    dl_t = millis();
    HTTPClient http;
    http.setTimeout(4000);
    char ch[32];
    snprintf(ch, sizeof ch, "/pack?id=%lu", (unsigned long)id);
    if (!http.begin(url(ch))) return;
    int code = http.GET();
    int taille = http.getSize();
    if (code == 200 && taille > 0 && taille <= 200 * 1024) {
        static uint8_t *buf = (uint8_t *)heap_caps_malloc(200 * 1024, MALLOC_CAP_SPIRAM);
        NetworkClient *flux = http.getStreamPtr();
        int lu = 0;
        uint32_t t0 = millis();
        while (buf && lu < taille && http.connected() && millis() - t0 < 5000) {
            int d = flux->available();
            if (!d) { delay(2); continue; }
            lu += flux->readBytes(buf + lu, d > taille - lu ? taille - lu : d);
        }
        if (buf && lu == taille && det::distant_paquet(buf, taille, id)) copiees++;
    }
    http.end();
}

// une pub apprise ici : on l'envoie a la base
static void envoyer_pub()
{
    const uint8_t *buf;
    int n, bloc;
    if (!det::sortie_prendre(buf, n, bloc)) return;
    HTTPClient http;
    http.setTimeout(5000);
    uint32_t id = 0;
    if (http.begin(url("/pack"))) {
        http.addHeader("Content-Type", "application/octet-stream");
        int code = http.POST((uint8_t *)buf, n);
        if (code == 200) {
            String r = http.getString();             // "id=123"
            int k = r.indexOf("id=");
            if (k >= 0) id = (uint32_t)r.substring(k + 3).toInt();
        }
        http.end();
    }
    if (id) envoyees++;
    det::sortie_faite(bloc, id);
}

void boucle(bool sta)
{
    if (!sta || cfg::R.base == 0) { joignable = false; return; }
    if (!udp_ok) {
        udp_ok = udp.begin(4512) == 1;
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        client_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
        if (!udp_ok) return;
    }
    chercher();
    recevoir();
    if (!trouvee) return;
    envoyer_codes();
    if (joignable && millis() - t_reponse > 6000) {
        joignable = false;                            // plus de reponse : on la cherchera de nouveau
        trouvee = false;
    }
    if (joignable) {
        telecharger();
        envoyer_pub();
    }
}

void infos(Reseau &r)
{
    r.base_etat = cfg::R.base == 0 ? 0 : (joignable ? 2 : (trouvee ? 3 : 1));
    if (trouvee) snprintf(r.base_adr, sizeof r.base_adr, "%s:%u", ip_base.toString().c_str(), port_http);
    else r.base_adr[0] = 0;
    strncpy(r.base_nom, nom_base, sizeof r.base_nom - 1);
    r.base_pubs = pubs_base;
    r.base_ms = latence;
    r.base_envoyees = envoyees;
    r.base_copiees = copiees;
}

}  // namespace distant

#endif  // BASE_DISTANTE
