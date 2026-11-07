// tele.cpp - telecommande Wi-Fi de la tele (voir tele.h) - carte seulement
//
// Le chiffrement (cle RSA, certificat X.509, TLS), l'appairage "Polo" et les
// messages de la telecommande sont ceux du croquis ESP32S3_TCL_Remote_WebPairing
// (v2.1, teste sur la TCL). Ajouts : lecture du volume envoye par la tele,
// messages recus en morceaux recolles, connexion avec delai maximal (tele
// eteinte), cle et certificat sur la carte SD, tout dans une seule tache.
#include <Arduino.h>
#include <WiFi.h>
#include "journal.h"
#include "tele.h"
#include "config.h"
#include "plateforme.h"
#include "coupure.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/error.h>

namespace tv {

volatile int niveau_bas = 3;
volatile int niveau_haut = 20;
volatile bool origine = true;

static const uint16_t PORT_APPAIRAGE = 6467;
static const uint16_t PORT_TELECOMMANDE = 6466;
static const char *DOSSIER = "/pub/tv";
static const char *CHEMIN_CERT = "/pub/tv/cert.pem";
static const char *CHEMIN_CLE = "/pub/tv/key.pem";
// touches Android (KEYCODE_...)
static const int TOUCHE_VOL_PLUS = 24, TOUCHE_VOL_MOINS = 25;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Infos s_i = {};                       // ce que voient l'ecran et le portail

// ---- demandes (deposees par les autres taches) ----
static volatile bool d_appairer = false, d_annuler = false, d_oublier = false, d_reconnecter = false;
static volatile bool d_code = false;
static char d_code_txt[8] = "";
static volatile int d_action = RIEN;         // BAISSER, REMONTER, DIMINUER, AUGMENTER
static volatile bool d_lire = false;
static volatile bool s_baisse = false;       // la carte a baisse le son
static volatile bool s_remonte = false;      // v0.9.5 : une remontee est en cours ou vient de finir (le volume lu n'est pas encore l'origine)
static uint32_t t_remonte = 0;
static volatile bool s_main = false;         // tu as change le volume toi-meme pendant la baisse
static volatile bool s_milieu = false;       // v0.9.64 : son remonte a mi-chemin (la carte l'a encore en main)
static int a_milieu = 0;                     // volume vise par le palier en cours
static volatile int d_pct = 50;              // v0.9.65 : palier demande, en % de l'ecart bas -> volume d'avant

// ---- identite (PEM) : celle sur la carte SD, et celle en cours d'appairage ----
static char *pem_cert = nullptr, *pem_cle = nullptr;           // actives (carte SD)
static char *gen_cert = nullptr, *gen_cle = nullptr;           // fabriquees pour l'appairage
static const size_t PEM_MAX = 4096;

// ---- contextes TLS : telecommande (port 6466) ----
static mbedtls_entropy_context r_entropie;
static mbedtls_ctr_drbg_context r_drbg;
static mbedtls_pk_context r_cle;
static mbedtls_x509_crt r_cert;
static mbedtls_ssl_context r_ssl;
static mbedtls_ssl_config r_conf;
static mbedtls_net_context r_fd;
static bool r_init = false;
// ---- contextes TLS : appairage (port 6467) ----
static mbedtls_entropy_context p_entropie;
static mbedtls_ctr_drbg_context p_drbg;
static mbedtls_pk_context p_cle;
static mbedtls_x509_crt p_cert;
static mbedtls_ssl_context p_ssl;
static mbedtls_ssl_config p_conf;
static mbedtls_net_context p_fd;
static bool p_init = false;

// ---- etat interne (tache tele seulement) ----
static int t_etat = NON_APPAIREE;
static bool t_appairee = false;
static char t_ip[16] = "";
static uint32_t t_essai = 0, t_delai = 2000;  // prochaine connexion : t_essai + t_delai
static uint32_t t_dernier_msg = 0;
static uint32_t t_app0 = 0;                   // debut de l'attente du code
static const uint32_t APP_CODE_MAX_MS = 180000;
// v0.9.71 : tampons de reception en PSRAM (4 Ko de memoire interne liberes)
static const size_t RX_N = 2048;
static uint8_t *rx = nullptr;                 // messages recus (recolles)
static size_t rx_n = 0;
// volume
static bool v_connu = false, v_muet = false;
static int v_niveau = 0, v_max = 100;
static uint32_t v_t = 0;                      // dernier volume recu (ms)
static int v_origine = -1;
// action en cours
static int a_action = RIEN, a_cible = 0, a_depart = 0, a_tours = 0, a_appuis = 0, a_dir = 0;
static uint32_t a_t0 = 0, a_t_appui = 0, a_attente = 0;
static uint32_t t_notre_appui = 0;            // dernier appui envoye par la carte (ms)

static void message(int type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void message(int type, const char *fmt, ...)
{
    char b[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_i.msg, b, sizeof s_i.msg);
    s_i.msg_type = type;
    s_i.msg_n = s_i.msg_n + 1;
    portEXIT_CRITICAL(&s_mux);
    Console.printf("[TV] %s\n", b);
}

const char *nom_etat(int e)
{
    switch (e) {
    case NON_APPAIREE: return "non appairée";
    case HORS_LIGNE: return "hors ligne";
    case CONNEXION: return "connexion…";
    case CONNECTEE: return "connectée";
    case APP_CLE: return "fabrication de la clé…";
    case APP_CONNEXION: return "connexion à la télé…";
    case APP_CODE: return "code affiché sur la télé";
    case APP_VALIDATION: return "vérification du code…";
    default: return "?";
    }
}

static void publier()
{
    Infos i;
    portENTER_CRITICAL(&s_mux);
    i = s_i;
    portEXIT_CRITICAL(&s_mux);
    i.etat = t_etat;
    memcpy(i.ip, t_ip, sizeof i.ip);
    i.appairee = t_appairee;
    i.volume_connu = v_connu;
    i.volume = v_niveau;
    i.volume_max = v_max;
    i.muet = v_muet;
    i.baisse = s_baisse;
    i.origine = v_origine;
    i.action = a_action;
    i.depart = a_depart;
    i.cible = a_cible;
    i.action_t0 = a_t0;
    i.app_reste_s = t_etat == APP_CODE ? (int)((APP_CODE_MAX_MS - (millis() - t_app0)) / 1000) : 0;
    if (i.app_reste_s < 0) i.app_reste_s = 0;
    i.ping_age_s = t_etat == CONNECTEE && t_dernier_msg ? (millis() - t_dernier_msg) / 1000 : 0;
    portENTER_CRITICAL(&s_mux);
    // le message a pu changer entre-temps (autre tache) : on ne touche qu'aux champs d'etat
    char msg[72];
    memcpy(msg, s_i.msg, sizeof msg);
    int mt = s_i.msg_type;
    uint32_t mn = s_i.msg_n;
    uint32_t nc = s_i.connexions;
    s_i = i;
    memcpy(s_i.msg, msg, sizeof msg);
    s_i.msg_type = mt;
    s_i.msg_n = mn;
    s_i.connexions = nc;
    portEXIT_CRITICAL(&s_mux);
}

void infos(Infos &i)
{
    portENTER_CRITICAL(&s_mux);
    i = s_i;
    portEXIT_CRITICAL(&s_mux);
}

// =========================================================================
// Protobuf minimal (repris du croquis)
// =========================================================================
static String pbVarint(uint32_t value)
{
    String out;
    while (true) {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) out += (char)(byte | 0x80);
        else { out += (char)byte; break; }
    }
    return out;
}
static String pbTag(uint32_t field, uint32_t wireType) { return pbVarint((field << 3) | wireType); }
static String pbString(uint32_t field, const String &value) { return pbTag(field, 2) + pbVarint(value.length()) + value; }
static String pbVarintField(uint32_t field, uint32_t value) { return pbTag(field, 0) + pbVarint(value); }

static uint32_t lire_varint(const uint8_t *data, size_t length, size_t &index)
{
    uint32_t result = 0, shift = 0;
    while (index < length) {
        uint8_t byte = data[index++];
        if (shift < 32) result |= (uint32_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return result;
}

// ---- Polo (appairage) ----
struct PoloMessage {
    uint32_t status = 0;
    bool hasPairingRequestAck = false;
    bool hasOptions = false;
    bool hasConfigurationAck = false;
    bool hasSecretAck = false;
};
static String poloBase() { return pbVarintField(1, 2) + pbVarintField(2, 200); }
static String poloPairingRequest() { return poloBase() + pbString(10, pbString(1, "atvremote") + pbString(2, "Detecteur de pub")); }
static String poloOptions() { return poloBase() + pbString(20, pbString(1, pbVarintField(1, 3) + pbVarintField(2, 6)) + pbVarintField(3, 1)); }
static String poloConfiguration() { return poloBase() + pbString(30, pbString(1, pbVarintField(1, 3) + pbVarintField(2, 6)) + pbVarintField(2, 1)); }
static String poloSecret(const uint8_t secret[32]) { return poloBase() + pbString(40, pbString(1, String((const char *)secret, 32))); }

static bool parsePolo(const uint8_t *data, size_t length, PoloMessage &out)
{
    size_t i = 0;
    while (i < length) {
        uint32_t tag = lire_varint(data, length, i);
        uint32_t field = tag >> 3, type = tag & 7;
        if (type == 0) {
            uint32_t value = lire_varint(data, length, i);
            if (field == 2) out.status = value;
        } else if (type == 2) {
            uint32_t len = lire_varint(data, length, i);
            if (i + len > length) return false;
            if (field == 11) out.hasPairingRequestAck = true;
            if (field == 20) out.hasOptions = true;
            if (field == 31) out.hasConfigurationAck = true;
            if (field == 41) out.hasSecretAck = true;
            i += len;
        } else return false;
    }
    return true;
}

// ---- RemoteMessage (telecommande) ----
static String encodeRemoteConfigure()
{
    String deviceInfo;
    deviceInfo += pbString(1, "onn/Android TV Remote");
    deviceInfo += pbString(2, "onn");
    deviceInfo += pbString(3, "1.0");
    deviceInfo += pbString(4, "atvremote");
    deviceInfo += pbString(5, "atvremote");
    String configure;
    configure += pbVarintField(1, 622);
    configure += pbString(2, deviceInfo);
    return pbTag(1, 2) + pbVarint(configure.length()) + configure;
}
static String encodeRemoteSetActive()
{
    String active = pbVarintField(1, 622);
    return pbTag(2, 2) + pbVarint(active.length()) + active;
}
static String encodeRemoteKeyInject(int keyCode)
{
    String inner = pbVarintField(1, keyCode) + pbVarintField(2, 3);   // direction SHORT
    return pbTag(10, 2) + pbVarint(inner.length()) + inner;
}
static String encodePingResponse(uint32_t value)
{
    String inner = pbVarintField(1, value);
    return pbTag(9, 2) + pbVarint(inner.length()) + inner;
}

// =========================================================================
// Identite : cle RSA 2048 + certificat X.509 auto-signe (repris du croquis)
// =========================================================================
static void liberer_appairage()
{
    if (!p_init) return;
    mbedtls_ssl_free(&p_ssl);
    mbedtls_ssl_config_free(&p_conf);
    mbedtls_net_free(&p_fd);
    mbedtls_x509_crt_free(&p_cert);
    mbedtls_pk_free(&p_cle);
    mbedtls_ctr_drbg_free(&p_drbg);
    mbedtls_entropy_free(&p_entropie);
    p_init = false;
}

static bool fabriquer_identite()
{
    liberer_appairage();
    mbedtls_entropy_init(&p_entropie);
    mbedtls_ctr_drbg_init(&p_drbg);
    mbedtls_pk_init(&p_cle);
    mbedtls_ssl_init(&p_ssl);
    mbedtls_ssl_config_init(&p_conf);
    mbedtls_net_init(&p_fd);
    mbedtls_x509_crt_init(&p_cert);
    p_init = true;
    const char *pers = "esp32-tcl-pair";
    int ret = mbedtls_ctr_drbg_seed(&p_drbg, mbedtls_entropy_func, &p_entropie, (const unsigned char *)pers, strlen(pers));
    if (ret != 0) { message(2, "Erreur : générateur aléatoire"); return false; }
    ret = mbedtls_pk_setup(&p_cle, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) { message(2, "Erreur : clé RSA"); return false; }
    uint32_t t0 = millis();
    // plusieurs secondes de calcul : priorite 0 (comme la tache "idle" du coeur 0),
    // sinon le chien de garde des taches (5 s) redemarre la carte
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 0);
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(p_cle), mbedtls_ctr_drbg_random, &p_drbg, 2048, 65537);
    vTaskPrioritySet(NULL, prio);
    if (ret != 0) { message(2, "Erreur : fabrication de la clé RSA"); return false; }
    Console.printf("[TV] Cle RSA 2048 bits fabriquee en %.1f s\n", (millis() - t0) / 1000.0f);
    if (!gen_cle) gen_cle = (char *)malloc(PEM_MAX);
    if (!gen_cert) gen_cert = (char *)malloc(PEM_MAX);
    if (!gen_cle || !gen_cert) { message(2, "Erreur : mémoire"); return false; }
    ret = mbedtls_pk_write_key_pem(&p_cle, (unsigned char *)gen_cle, PEM_MAX);
    if (ret != 0) { message(2, "Erreur : export de la clé"); return false; }

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    unsigned char serial[16];
    mbedtls_ctr_drbg_random(&p_drbg, serial, sizeof serial);
    serial[0] &= 0x7F;
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    const char *dn = "CN=atvremote, C=US, ST=California, L=Mountain View, O=Google Inc., OU=Android, emailAddress=example@google.com";
    if (mbedtls_x509write_crt_set_subject_name(&crt, dn) != 0 || mbedtls_x509write_crt_set_issuer_name(&crt, dn) != 0) {
        mbedtls_x509write_crt_free(&crt);
        message(2, "Erreur : nom du certificat");
        return false;
    }
    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20400101000000");
    mbedtls_x509write_crt_set_subject_key(&crt, &p_cle);
    mbedtls_x509write_crt_set_issuer_key(&crt, &p_cle);
    unsigned char ext_bc[] = { 0x30, 0x03, 0x01, 0x01, 0xFF };
    const unsigned char oid_bc[] = { 0x55, 0x1D, 0x13 };
    mbedtls_x509write_crt_set_extension(&crt, (const char *)oid_bc, sizeof oid_bc, 1, ext_bc, sizeof ext_bc);
    unsigned char ext_ku[] = { 0x03, 0x02, 0x01, 0xB6 };
    const unsigned char oid_ku[] = { 0x55, 0x1D, 0x0F };
    mbedtls_x509write_crt_set_extension(&crt, (const char *)oid_ku, sizeof oid_ku, 1, ext_ku, sizeof ext_ku);
    unsigned char ext_san[] = { 0x30, 0x0B, 0x82, 0x09, 0x61, 0x74, 0x76, 0x72, 0x65, 0x6D, 0x6F, 0x74, 0x65 };
    const unsigned char oid_san[] = { 0x55, 0x1D, 0x11 };
    mbedtls_x509write_crt_set_extension(&crt, (const char *)oid_san, sizeof oid_san, 0, ext_san, sizeof ext_san);
    unsigned char ext_eku[] = { 0x30, 0x14, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
    const unsigned char oid_eku[] = { 0x55, 0x1D, 0x25 };
    mbedtls_x509write_crt_set_extension(&crt, (const char *)oid_eku, sizeof oid_eku, 0, ext_eku, sizeof ext_eku);
    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)gen_cert, PEM_MAX, mbedtls_ctr_drbg_random, &p_drbg);
    mbedtls_x509write_crt_free(&crt);
    if (ret != 0) { message(2, "Erreur : export du certificat"); return false; }
    return true;
}

// connexion TCP avec delai maximal (tele eteinte : pas d'attente de 20 s)
static bool connecter_tcp(mbedtls_net_context &ctx, const char *ip, uint16_t port, uint32_t delai_ms)
{
    struct sockaddr_in adr = {};
    adr.sin_family = AF_INET;
    adr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &adr.sin_addr) != 1) return false;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&adr, sizeof adr);
    if (r < 0 && errno != EINPROGRESS) { close(s); return false; }
    if (r < 0) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = { (time_t)(delai_ms / 1000), (suseconds_t)((delai_ms % 1000) * 1000) };
        r = select(s + 1, nullptr, &w, nullptr, &tv);
        int err = 0;
        socklen_t lg = sizeof err;
        if (r <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &lg) < 0 || err != 0) { close(s); return false; }
    }
    fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    int un = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &un, sizeof un);
    ctx.fd = s;
    return true;
}

static bool poloEnvoyer(const String &m)
{
    String frame = pbVarint(m.length()) + m;
    size_t fait = 0;
    while (fait < frame.length()) {
        int ret = mbedtls_ssl_write(&p_ssl, (const unsigned char *)frame.c_str() + fait, frame.length() - fait);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) { delay(10); continue; }
        if (ret <= 0) return false;
        fait += ret;
    }
    return true;
}

static bool poloRecevoir(PoloMessage &out)
{
    static uint8_t *buffer = (uint8_t *)plat_malloc_grand(RX_N);
    if (!buffer) return false;
    uint32_t lg = 0, shift = 0;
    while (true) {
        uint8_t byte = 0;
        int ret = mbedtls_ssl_read(&p_ssl, &byte, 1);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) { delay(10); continue; }
        if (ret <= 0) return false;
        lg |= (uint32_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift > 21) return false;
    }
    if (lg == 0 || lg > RX_N) return false;
    size_t recu = 0;
    while (recu < lg) {
        int ret = mbedtls_ssl_read(&p_ssl, buffer + recu, lg - recu);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) { delay(10); continue; }
        if (ret <= 0) return false;
        recu += ret;
    }
    return parsePolo(buffer, lg, out);
}

// appairage, 1re partie : connexion au port 6467, puis la tele affiche le code
static bool appairage_debut()
{
    int ret = mbedtls_x509_crt_parse(&p_cert, (const unsigned char *)gen_cert, strlen(gen_cert) + 1);
    if (ret != 0) { message(2, "Erreur : lecture du certificat"); return false; }
    ret = mbedtls_ssl_config_defaults(&p_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { message(2, "Erreur : configuration TLS"); return false; }
    mbedtls_ssl_conf_authmode(&p_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&p_conf, mbedtls_ctr_drbg_random, &p_drbg);
    mbedtls_ssl_conf_read_timeout(&p_conf, 10000);
    ret = mbedtls_ssl_conf_own_cert(&p_conf, &p_cert, &p_cle);
    if (ret != 0) { message(2, "Erreur : certificat client"); return false; }
    ret = mbedtls_ssl_setup(&p_ssl, &p_conf);
    if (ret != 0) { message(2, "Erreur : préparation TLS"); return false; }
    mbedtls_ssl_set_hostname(&p_ssl, t_ip);
    if (!connecter_tcp(p_fd, t_ip, PORT_APPAIRAGE, 4000)) { message(2, "Télé injoignable (%s) : allumée ? bonne IP ?", t_ip); return false; }
    mbedtls_ssl_set_bio(&p_ssl, &p_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    while ((ret = mbedtls_ssl_handshake(&p_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            message(2, "Échec TLS avec la télé (-0x%04X)", (unsigned)-ret);
            return false;
        }
    }
    PoloMessage m;
    if (!poloEnvoyer(poloPairingRequest()) || !poloRecevoir(m) || m.status != 200 || !m.hasPairingRequestAck) { message(2, "La télé refuse l'appairage (demande)"); return false; }
    m = PoloMessage();
    if (!poloEnvoyer(poloOptions()) || !poloRecevoir(m) || m.status != 200 || !m.hasOptions) { message(2, "La télé refuse l'appairage (options)"); return false; }
    m = PoloMessage();
    if (!poloEnvoyer(poloConfiguration()) || !poloRecevoir(m) || m.status != 200 || !m.hasConfigurationAck) { message(2, "La télé refuse l'appairage (configuration)"); return false; }
    return true;
}

static void mpi_octets(mbedtls_mpi *mpi, uint8_t *out, size_t max, size_t &lg)
{
    lg = mbedtls_mpi_size(mpi);
    if (lg > max) lg = max;
    mbedtls_mpi_write_binary(mpi, out, lg);
}
static void exposant_polo(mbedtls_mpi *e, uint8_t *out, size_t max, size_t &lg)
{
    uint8_t raw[8];
    size_t n = 0;
    mpi_octets(e, raw, sizeof raw, n);
    (void)max;
    if (n % 2 == 0) { out[0] = 0x00; memcpy(out + 1, raw, n); lg = n + 1; }
    else { memcpy(out, raw, n); lg = n; }
}

// secret de l'appairage : SHA-256 des deux cles publiques et du code (croquis)
static int calculer_secret(const char *code, uint8_t secret[32])
{
    if (strlen(code) != 6) return 1;
    for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)code[i])) return 1;
    mbedtls_mpi cn, ce, sn, se;
    mbedtls_mpi_init(&cn); mbedtls_mpi_init(&ce); mbedtls_mpi_init(&sn); mbedtls_mpi_init(&se);
    mbedtls_rsa_export(mbedtls_pk_rsa(p_cle), &cn, NULL, NULL, NULL, &ce);
    const mbedtls_x509_crt *pair = mbedtls_ssl_get_peer_cert(&p_ssl);
    if (!pair) { mbedtls_mpi_free(&cn); mbedtls_mpi_free(&ce); mbedtls_mpi_free(&sn); mbedtls_mpi_free(&se); return 2; }
    mbedtls_rsa_export(mbedtls_pk_rsa(pair->pk), &sn, NULL, NULL, NULL, &se);
    uint8_t cnB[256], ceB[8], snB[256], seB[8];
    size_t cnL, ceL, snL, seL;
    mpi_octets(&cn, cnB, sizeof cnB, cnL); exposant_polo(&ce, ceB, sizeof ceB, ceL);
    mpi_octets(&sn, snB, sizeof snB, snL); exposant_polo(&se, seB, sizeof seB, seL);
    char h[3] = { 0, 0, 0 };
    uint8_t code_oct[2];
    h[0] = code[2]; h[1] = code[3]; code_oct[0] = (uint8_t)strtoul(h, NULL, 16);
    h[0] = code[4]; h[1] = code[5]; code_oct[1] = (uint8_t)strtoul(h, NULL, 16);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, cnB, cnL); mbedtls_sha256_update(&sha, ceB, ceL);
    mbedtls_sha256_update(&sha, snB, snL); mbedtls_sha256_update(&sha, seB, seL);
    mbedtls_sha256_update(&sha, code_oct, 2);
    mbedtls_sha256_finish(&sha, secret); mbedtls_sha256_free(&sha);
    mbedtls_mpi_free(&cn); mbedtls_mpi_free(&ce); mbedtls_mpi_free(&sn); mbedtls_mpi_free(&se);
    h[0] = code[0]; h[1] = code[1];
    uint8_t attendu = (uint8_t)strtoul(h, NULL, 16);
    return secret[0] == attendu ? 0 : 3;
}

static bool enregistrer_identite()
{
    plat_dossier_creer("/pub");
    plat_dossier_creer(DOSSIER);
    bool ok = plat_fichier_ecrire(CHEMIN_CERT, gen_cert, strlen(gen_cert)) && plat_fichier_ecrire(CHEMIN_CLE, gen_cle, strlen(gen_cle));
    if (!ok) return false;
    if (!pem_cert) pem_cert = (char *)malloc(PEM_MAX);
    if (!pem_cle) pem_cle = (char *)malloc(PEM_MAX);
    if (!pem_cert || !pem_cle) return false;
    strncpy(pem_cert, gen_cert, PEM_MAX - 1); pem_cert[PEM_MAX - 1] = 0;
    strncpy(pem_cle, gen_cle, PEM_MAX - 1); pem_cle[PEM_MAX - 1] = 0;
    return true;
}

static bool lire_identite()
{
    if (!pem_cert) pem_cert = (char *)malloc(PEM_MAX);
    if (!pem_cle) pem_cle = (char *)malloc(PEM_MAX);
    if (!pem_cert || !pem_cle) return false;
    long a = plat_fichier_lire(CHEMIN_CERT, pem_cert, PEM_MAX - 1);
    long b = plat_fichier_lire(CHEMIN_CLE, pem_cle, PEM_MAX - 1);
    if (a <= 0 || b <= 0) return false;
    pem_cert[a] = 0;
    pem_cle[b] = 0;
    return strstr(pem_cert, "BEGIN CERTIFICATE") && strstr(pem_cle, "PRIVATE KEY");
}

// =========================================================================
// Telecommande (port 6466)
// =========================================================================
static void liberer_telecommande()
{
    if (!r_init) return;
    mbedtls_ssl_free(&r_ssl);
    mbedtls_ssl_config_free(&r_conf);
    mbedtls_net_free(&r_fd);
    mbedtls_x509_crt_free(&r_cert);
    mbedtls_pk_free(&r_cle);
    mbedtls_ctr_drbg_free(&r_drbg);
    mbedtls_entropy_free(&r_entropie);
    r_init = false;
}

static void deconnecter(const char *pourquoi)
{
    if (t_etat == CONNECTEE) {
        mbedtls_ssl_close_notify(&r_ssl);
        if (pourquoi) message(2, "Télé déconnectée (%s)", pourquoi);
    }
    liberer_telecommande();
    rx_n = 0;
    v_connu = false;
    if (t_etat == CONNECTEE || t_etat == CONNEXION) t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
}

static bool envoyer(const String &m)
{
    if (t_etat != CONNECTEE) return false;
    String frame = pbVarint(m.length()) + m;
    size_t fait = 0;
    uint32_t t0 = millis();
    while (fait < frame.length()) {
        int ret = mbedtls_ssl_write(&r_ssl, (const unsigned char *)frame.c_str() + fait, frame.length() - fait);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (millis() - t0 > 2000) return false;
            delay(5);
            continue;
        }
        if (ret <= 0) { deconnecter("écriture impossible"); return false; }
        fait += ret;
    }
    return true;
}

static bool connecter()
{
    if (!t_appairee || !t_ip[0]) return false;
    liberer_telecommande();
    mbedtls_entropy_init(&r_entropie);
    mbedtls_ctr_drbg_init(&r_drbg);
    mbedtls_pk_init(&r_cle);
    mbedtls_x509_crt_init(&r_cert);
    mbedtls_ssl_init(&r_ssl);
    mbedtls_ssl_config_init(&r_conf);
    mbedtls_net_init(&r_fd);
    r_init = true;
    t_etat = CONNEXION;
    publier();
    int ret = mbedtls_x509_crt_parse(&r_cert, (const unsigned char *)pem_cert, strlen(pem_cert) + 1);
    if (ret == 0) ret = mbedtls_ctr_drbg_seed(&r_drbg, mbedtls_entropy_func, &r_entropie, NULL, 0);
    if (ret == 0) ret = mbedtls_pk_parse_key(&r_cle, (const unsigned char *)pem_cle, strlen(pem_cle) + 1, NULL, 0, mbedtls_ctr_drbg_random, &r_drbg);
    if (ret == 0) ret = mbedtls_ssl_config_defaults(&r_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { message(2, "Clé ou certificat illisible : appaire à nouveau"); deconnecter(nullptr); return false; }
    mbedtls_ssl_conf_authmode(&r_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&r_conf, mbedtls_ctr_drbg_random, &r_drbg);
    mbedtls_ssl_conf_read_timeout(&r_conf, 5000);
    if (mbedtls_ssl_conf_own_cert(&r_conf, &r_cert, &r_cle) != 0 || mbedtls_ssl_setup(&r_ssl, &r_conf) != 0) {
        message(2, "Erreur : préparation TLS");
        deconnecter(nullptr);
        return false;
    }
    if (!connecter_tcp(r_fd, t_ip, PORT_TELECOMMANDE, 3000)) {
        deconnecter(nullptr);
        return false;
    }
    mbedtls_ssl_set_bio(&r_ssl, &r_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    uint32_t t0 = millis();
    while ((ret = mbedtls_ssl_handshake(&r_ssl)) != 0) {
        if ((ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) || millis() - t0 > 10000) {
            // certificat refuse : la tele a oublie l'appairage (ou autre tele a cette IP)
            message(2, "Télé : connexion refusée (-0x%04X), appairage à refaire ?", (unsigned)-ret);
            deconnecter(nullptr);
            return false;
        }
    }
    mbedtls_ssl_conf_read_timeout(&r_conf, 30);   // lectures courtes ensuite (la tache fait autre chose)
    t_etat = CONNECTEE;
    rx_n = 0;
    if (!envoyer(encodeRemoteConfigure())) return false;
    delay(20);
    if (!envoyer(encodeRemoteSetActive())) return false;
    t_dernier_msg = millis();
    portENTER_CRITICAL(&s_mux);
    s_i.connexions = s_i.connexions + 1;
    portEXIT_CRITICAL(&s_mux);
    Console.printf("[TV] Telecommande connectee a %s\n", t_ip);
    return true;
}

static bool appui(int touche)
{
    if (!envoyer(encodeRemoteKeyInject(touche))) return false;
    t_notre_appui = millis();
    return true;
}

// un message de la tele (RemoteMessage)
static void traiter_message(const uint8_t *m, size_t n)
{
    size_t p = 0;
    while (p < n) {
        uint32_t tag = lire_varint(m, n, p);
        uint32_t champ = tag >> 3, type = tag & 7;
        if (type == 0) { lire_varint(m, n, p); continue; }
        if (type != 2) return;
        uint32_t lg = lire_varint(m, n, p);
        if (p + lg > n) return;
        const uint8_t *s = m + p;
        if (champ == 8) {                             // ping de la tele : on repond
            size_t q = 0;
            uint32_t val = 0;
            while (q < lg) {
                uint32_t t2 = lire_varint(s, lg, q);
                if ((t2 & 7) == 0) { uint32_t v = lire_varint(s, lg, q); if ((t2 >> 3) == 1) val = v; }
                else if ((t2 & 7) == 2) { uint32_t k = lire_varint(s, lg, q); q += k; }
                else break;
            }
            envoyer(encodePingResponse(val));
        } else if (champ == 50) {                     // RemoteSetVolumeLevel : max = 6, niveau = 7, muet = 8
            size_t q = 0;
            int niv = -1, mx = -1, muet = -1;
            while (q < lg) {
                uint32_t t2 = lire_varint(s, lg, q);
                uint32_t c2 = t2 >> 3;
                if ((t2 & 7) == 0) {
                    uint32_t v = lire_varint(s, lg, q);
                    if (c2 == 6) mx = (int)v;
                    else if (c2 == 7) niv = (int)v;
                    else if (c2 == 8) muet = (int)v;
                } else if ((t2 & 7) == 2) { uint32_t k = lire_varint(s, lg, q); q += k; }
                else break;
            }
            if (niv >= 0) {
                bool premier = !v_connu;
                v_niveau = niv;
                if (mx > 0) v_max = mx;
                v_muet = muet == 1;
                v_connu = true;
                v_t = millis();
                if (premier) Console.printf("[TV] Volume de la tele : %d / %d%s\n", v_niveau, v_max, v_muet ? " (muet)" : "");
            }
        } else if (champ == 3) {
            Console.println("[TV] La tele signale une erreur (message refuse)");
        } else if (champ == 40) {
            Console.println("[TV] La tele signale un changement marche / veille");
        }
        p += lg;
    }
}

static void lire_messages()
{
    if (t_etat != CONNECTEE) return;
    if (!rx) { rx = (uint8_t *)plat_malloc_grand(RX_N); if (!rx) return; rx_n = 0; }
    if (rx_n >= RX_N) rx_n = 0;                       // ne doit pas arriver : message enorme
    int ret = mbedtls_ssl_read(&r_ssl, rx + rx_n, RX_N - rx_n);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_TIMEOUT) return;
    if (ret <= 0) { deconnecter(ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? "fermée par la télé" : "liaison perdue"); return; }
    rx_n += ret;
    t_dernier_msg = millis();
    // messages complets : longueur (varint) + message ; le reste attend la suite
    size_t p = 0;
    while (p < rx_n) {
        size_t q = p;
        uint32_t lg = 0, shift = 0;
        bool complet = false;
        while (q < rx_n) {
            uint8_t b = rx[q++];
            lg |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) { complet = true; break; }
            shift += 7;
            if (shift > 21) { rx_n = 0; return; }
        }
        if (!complet || q + lg > rx_n) break;
        traiter_message(rx + q, lg);
        if (t_etat != CONNECTEE) return;
        p = q + lg;
    }
    if (p) { memmove(rx, rx + p, rx_n - p); rx_n -= p; }
}

// =========================================================================
// Volume : plan d'action (baisser, remonter, un cran de moins)
// =========================================================================
static int borne(int v) { int m = v_max > 0 ? v_max : 100; return v < 0 ? 0 : (v > m ? m : v); }

static void lancer_action(int action)
{
    if (action == MILIEU && (!v_connu || !s_baisse)) return;   // v0.9.64 : sans volume connu, on reste au niveau bas
    a_action = action;
    a_t0 = millis();
    a_tours = 0;
    a_appuis = 0;
    a_attente = 0;
    a_depart = v_connu ? v_niveau : -1;
    if (action == BAISSER) {
        if (v_connu && !s_baisse && !s_remonte) v_origine = v_niveau > niveau_bas ? v_niveau : (v_origine >= 0 ? v_origine : niveau_haut);
        a_cible = borne(niveau_bas);
        s_baisse = true;
        s_main = false;
        s_milieu = false;
    } else if (action == MILIEU) {
        // v0.9.65 : palier entre le niveau bas et le volume d'avant la pub (la carte garde la main)
        int haut = origine && v_origine >= 0 ? v_origine : niveau_haut;
        int pct = d_pct;
        a_cible = borne(haut > niveau_bas ? niveau_bas + ((haut - niveau_bas) * pct + 50) / 100 : niveau_bas);
        if (a_cible >= haut && haut > niveau_bas) a_cible = haut - 1;      // le dernier pas reste le retour au volume d'avant
        a_milieu = a_cible;
        s_milieu = true;
    } else if (action == REMONTER) {
        a_cible = borne(origine && v_origine >= 0 ? v_origine : niveau_haut);
        s_milieu = false;
        s_baisse = false;
        s_remonte = true;
        t_remonte = millis();
    } else if (action == AUGMENTER) {
        a_cible = v_connu ? borne(v_niveau + 1) : 0;
    } else {
        a_cible = v_connu ? borne(v_niveau - 1) : 0;
    }
    if (!v_connu) {
        // volume inconnu : nombre d'appuis fixe (ecart entre les deux niveaux)
        int n = (action == DIMINUER || action == AUGMENTER) ? 1 : abs(niveau_haut - niveau_bas);
        a_appuis = n;
        a_dir = (action == REMONTER || action == AUGMENTER) ? 1 : -1;
    }
}

static void action_finie(bool ok, const char *quoi)
{
    if (ok) {
        if (a_depart >= 0) message(1, "Volume %d → %d ✓", a_depart, v_connu ? v_niveau : a_cible);
        else message(1, "%s", quoi);
    } else message(2, "%s", quoi);
    a_action = RIEN;
    a_appuis = 0;
}

static void avancer_action()
{
    if (s_remonte && a_action != REMONTER && millis() - t_remonte > 4000) s_remonte = false;
    if (a_action == RIEN) return;
    uint32_t ms = millis();
    if (t_etat != CONNECTEE) {
        // pas connectee : on attend un peu la connexion (demandee tout de suite)
        if (ms - a_t0 > 12000) action_finie(false, "Télé injoignable : volume pas changé");
        return;
    }
    // appuis en cours d'envoi (un toutes les 70 ms)
    if (a_appuis > 0) {
        if (ms - a_t_appui >= 70) {
            if (!appui(a_dir > 0 ? TOUCHE_VOL_PLUS : TOUCHE_VOL_MOINS)) { action_finie(false, "Télé : touche refusée"); return; }
            a_t_appui = ms;
            a_appuis--;
            if (!a_appuis) a_attente = ms + 700;          // la tele renvoie son volume juste apres
        }
        return;
    }
    if (a_attente && (int32_t)(ms - a_attente) < 0) return;
    a_attente = 0;
    if (!v_connu) {
        if (a_tours == 0 && a_depart < 0) { action_finie(true, "Volume inconnu : appuis envoyés"); return; }
        action_finie(true, "Appuis envoyés");
        return;
    }
    int ecart = a_cible - v_niveau;
    if (ecart == 0) { action_finie(true, ""); return; }
    if (a_tours >= 4) {
        char b[72];
        snprintf(b, sizeof b, "La télé ne suit pas : volume %d au lieu de %d", v_niveau, a_cible);
        action_finie(false, b);
        return;
    }
    a_tours++;
    a_dir = ecart > 0 ? 1 : -1;
    a_appuis = abs(ecart);
    if (a_appuis > 100) a_appuis = 100;
    a_t_appui = 0;
}

// =========================================================================
// Tache
// =========================================================================
static void lire_ip()
{
    cfg::ip_tele(t_ip, sizeof t_ip);
}

static void tache(void *)
{
    lire_ip();
    for (;;) {
        uint32_t ms = millis();
        // ---- demandes ----
        if (d_oublier) {
            d_oublier = false;
            deconnecter(nullptr);
            liberer_appairage();
            plat_fichier_supprimer(CHEMIN_CERT);
            plat_fichier_supprimer(CHEMIN_CLE);
            t_appairee = false;
            t_etat = NON_APPAIREE;
            s_baisse = false;
            v_origine = -1;
            a_action = RIEN;
            message(0, "Télé oubliée : appairage à refaire");
        }
        if (d_reconnecter) {
            d_reconnecter = false;
            lire_ip();
            if (t_etat == CONNECTEE || t_etat == CONNEXION) deconnecter(nullptr);
            t_essai = 0;
            t_delai = 0;
        }
        if (d_annuler) {
            d_annuler = false;
            if (t_etat >= APP_CLE) {
                liberer_appairage();
                t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
                message(0, "Appairage annulé");
            }
        }
        if (d_appairer) {
            d_appairer = false;
            lire_ip();
            if (!t_ip[0]) message(2, "Entre d'abord l'adresse IP de la télé");
            else if (WiFi.status() != WL_CONNECTED) message(2, "Wi-Fi de la box pas connecté");
            else {
                if (t_etat == CONNECTEE) deconnecter(nullptr);
                t_etat = APP_CLE;
                publier();
                message(0, "Fabrication de la clé (quelques secondes)…");
                bool ok = fabriquer_identite();
                if (ok) {
                    t_etat = APP_CONNEXION;
                    publier();
                    ok = appairage_debut();
                }
                if (ok) {
                    t_etat = APP_CODE;
                    t_app0 = millis();
                    d_code = false;
                    message(0, "Tape le code affiché sur la télé");
                } else {
                    liberer_appairage();
                    t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
                }
            }
        }
        if (t_etat == APP_CODE && d_code) {
            d_code = false;
            char code[8];
            portENTER_CRITICAL(&s_mux);
            memcpy(code, d_code_txt, sizeof code);
            portEXIT_CRITICAL(&s_mux);
            t_etat = APP_VALIDATION;
            publier();
            uint8_t secret[32];
            int r = calculer_secret(code, secret);
            if (r == 1 || r == 3) {
                message(2, r == 1 ? "Code : 6 caractères 0-9 A-F" : "Code incorrect : regarde bien la télé");
                t_etat = APP_CODE;                        // la tele attend toujours : on peut retaper
            } else if (r == 2) {
                message(2, "Certificat de la télé absent : recommence");
                liberer_appairage();
                t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
            } else {
                PoloMessage m;
                if (!poloEnvoyer(poloSecret(secret)) || !poloRecevoir(m) || m.status != 200 || !m.hasSecretAck) {
                    message(2, "La télé refuse le code : recommence l'appairage");
                    liberer_appairage();
                    t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
                } else if (!enregistrer_identite()) {
                    message(2, "Appairée, mais clé non écrite sur la carte SD");
                    liberer_appairage();
                    t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
                } else {
                    liberer_appairage();
                    t_appairee = true;
                    t_etat = HORS_LIGNE;
                    t_essai = 0;
                    t_delai = 0;
                    message(1, "Appairage réussi ✓");
                }
            }
        }
        if (t_etat == APP_CODE && millis() - t_app0 > APP_CODE_MAX_MS) {
            liberer_appairage();
            t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
            message(2, "Pas de code en 3 min : appairage abandonné");
        }
        // ---- actions de volume ----
        int act = d_action;
        if (act != RIEN) {
            d_action = RIEN;
            if (!t_appairee) message(2, "Télé pas appairée (menu Télé)");
            else {
                lancer_action(act);
                if (t_etat != CONNECTEE) { t_essai = 0; t_delai = 0; }   // on se connecte tout de suite
            }
        }
        if (d_lire) {
            d_lire = false;
            if (!t_appairee) message(2, "Télé pas appairée (menu Télé)");
            else if (t_etat == CONNECTEE && v_connu) message(1, "Volume actuel : %d / %d%s", v_niveau, v_max, v_muet ? " (muet)" : "");
            else if (t_etat == CONNECTEE) { message(0, "Volume pas encore donné : reconnexion…"); deconnecter(nullptr); t_essai = 0; t_delai = 0; }
            else { message(0, "Connexion à la télé…"); t_essai = 0; t_delai = 0; }
        }
        // ---- connexion ----
        if ((t_etat == HORS_LIGNE) && t_appairee && t_ip[0] && WiFi.status() == WL_CONNECTED && ms - t_essai >= t_delai) {
            t_essai = millis();
            if (connecter()) {
                t_delai = 2000;
            } else {
                // tele eteinte, IP fausse : on reessaie de plus en plus lentement (30 s au plus)
                t_delai = t_delai < 2000 ? 2000 : (t_delai * 2 > 30000 ? 30000 : t_delai * 2);
                if (a_action != RIEN) message(2, "Télé injoignable (%s)", t_ip);
            }
        }
        if (t_etat == CONNECTEE) {
            lire_messages();
            // plus rien de la tele depuis 90 s (elle envoie un ping toutes les ~5 s) : liaison morte
            if (t_etat == CONNECTEE && millis() - t_dernier_msg > 90000) deconnecter("plus de nouvelles");
            // tu as monte le son toi-meme pendant la baisse : la carte n'y touche plus
            if (t_etat == CONNECTEE && s_baisse && a_action == RIEN && v_connu && v_niveau > (s_milieu ? a_milieu : niveau_bas) + 1 &&
                millis() - t_notre_appui > 2500 && millis() - v_t < 1000) {
                s_baisse = false;
                s_main = true;
                cpr::liberer();
                message(0, "Tu as remis le son : la carte n'y touche plus pour cette pub");
            }
        }
        avancer_action();
        publier();
        vTaskDelay(pdMS_TO_TICKS(t_etat == CONNECTEE ? 2 : 20));
    }
}

// =========================================================================
// Fonctions publiques
// =========================================================================
void init()
{
    t_appairee = lire_identite();
    t_etat = t_appairee ? HORS_LIGNE : NON_APPAIREE;
    lire_ip();
    Console.printf("[TV] Telecommande Wi-Fi : %s, IP de la tele %s\n", t_appairee ? "cle trouvee sur la carte SD" : "pas encore appairee",
                   t_ip[0] ? t_ip : "(a regler)");
    publier();
}

void demarrer()
{
    // pile interne genereuse : la fabrication de la cle RSA et le TLS en ont besoin
    xTaskCreatePinnedToCore(tache, "tele", 16384, NULL, 2, NULL, 0);
}

static int s_suivi = 0;
// v0.9.64 : trois niveaux (0 normal, 1 bas, 2 a mi-chemin). Mi-chemin seulement si le volume est connu
// (sinon le nombre d'appuis pour revenir serait faux) et si la carte a bien baisse le son elle-meme.
void suivre_niveau(int n)
{
    static const int PCT[3] = { 10, 30, 65 };         // v0.9.65 : paliers lents au debut, de plus en plus forts
    if (n >= 2 && !v_connu) n = 1;
    if (n > 4) n = 4;
    if (n == s_suivi) return;
    s_suivi = n;
    if (n == 1) d_action = BAISSER;
    else if (n >= 2) { d_pct = PCT[n - 2]; if (s_baisse && !s_main) d_action = MILIEU; }
    else {
        if (d_action == BAISSER || d_action == MILIEU) d_action = RIEN;   // v0.9.5 : baisse jamais partie (tele occupee) : on l'annule, sinon le son resterait bas
        if (s_baisse && !s_main) d_action = REMONTER;
    }    // remonte seulement ce que la carte a baisse
}
void suivre(bool baisser) { suivre_niveau(baisser ? 1 : 0); }
bool en_cours() { return s_baisse; }

void appairer() { d_appairer = true; }
void code(const char *c)
{
    char b[8] = "";
    int k = 0;
    for (const char *p = c; *p && k < 6; p++) {
        char x = *p;
        if (x >= 'a' && x <= 'f') x = (char)(x - 32);
        if ((x >= '0' && x <= '9') || (x >= 'A' && x <= 'F')) b[k++] = x;
    }
    b[k] = 0;
    portENTER_CRITICAL(&s_mux);
    memcpy(d_code_txt, b, sizeof d_code_txt);
    portEXIT_CRITICAL(&s_mux);
    d_code = true;
}
void annuler_appairage() { d_annuler = true; }
void oublier() { d_oublier = true; }
void reconnecter() { d_reconnecter = true; }
void couper() { d_action = BAISSER; }
void retablir() { d_action = REMONTER; }
void diminuer() { d_action = DIMINUER; }
void augmenter() { d_action = AUGMENTER; }
void lire() { d_lire = true; }

}  // namespace tv
