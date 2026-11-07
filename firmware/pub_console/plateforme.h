// plateforme.h - ce que le coeur du programme demande au materiel.
// Implemente par pub_console.ino (carte ES3C35P ; version d'origine : ES3C28P). Le reste du code (empreintes,
// bibliotheque, detecteur, ecrans, dessin) ne depend pas d'Arduino : il tourne
// aussi sur le PC (bancs d'essai).
#pragma once
#include <stdint.h>
#include <stddef.h>

uint32_t plat_ms();                       // millisecondes depuis le demarrage
uint32_t plat_heure();                    // secondes depuis 1970 (heure Internet), 0 si inconnue
void plat_verrou();                       // section critique courte (moteur <-> interface)
void plat_deverrou();
void plat_luminosite(int pct);            // retroeclairage 5..100 %
uint32_t plat_memoire_libre();            // octets de memoire interne libres
uint32_t plat_psram_libre();              // octets de PSRAM libres

// JOURNAL : la console serie gardee en memoire (ecran Journal, portail)
const int JOURNAL_NB = 200;               // lignes gardees
const int JOURNAL_LARGEUR = 104;          // caracteres par ligne (au-dela : coupee)
struct LigneJournal { uint32_t seq; uint32_t ms; char txt[JOURNAL_LARGEUR]; };
uint32_t plat_journal_dernier();          // numero de la derniere ligne (0 = aucune)
// copie au plus max lignes, les plus recentes jusqu'a la ligne "jusqua" incluse
// (0 = jusqu'a la derniere), de la plus ancienne a la plus recente ; rend le nombre copie
int plat_journal(LigneJournal *out, int max, uint32_t jusqua);
void *plat_malloc_grand(size_t n);        // gros tableaux (PSRAM sur la carte), nullptr si echec

// ---- audio : micro du codec ES8311, ramene a 16 kHz mono ----
bool plat_audio_ok();                     // codec trouve, micro qui repond
const char *plat_audio_etat();            // texte court pour l'ecran
void plat_audio_gain(int niveau);         // 0..5 : +12 / +21 / +30 / +36 / +42 / +48 dB (gain du micro)
// copie les echantillons arrives depuis le dernier appel (au plus nmax), renvoie leur nombre
int plat_audio_lire(int16_t *dst, int nmax);
void plat_source(int src);                // 0 micro, 1 entree optique (le flux optique remplace le micro dans l'anneau)
bool plat_fo_flux_ok();                   // l'optique envoie du son (signal vu depuis moins de 2 s)
int plat_fo_etat();                      // v0.9.60 : -1 micro choisi, 0 optique arretee (micro actif), 1 silence, 2 son recu, 3 test

// ---- haut-parleur de la carte : petit bip (non bloquant) ----
enum { SON_PUB_DEBUT = 1, SON_PUB_FIN = 2, SON_TEST = 3 };
void plat_son(int quoi);
bool plat_son_en_cours();                 // vrai pendant le bip et 0,3 s apres (le micro l'entend)

// ---- audio des pubs (carte SD) : les 12 dernieres minutes tournent dans un
// fichier ; une pub apprise en garde un WAV pour l'ecouter et la decouper ----
void plat_archive_reset();                     // le compteur repart (changement de source)
void plat_archive(const int16_t *x16, int n);  // echantillons 16 kHz analyses, dans l'ordre
// copie les colonnes [c0, c1] (emp::colonne) dans /pub/audio/<numero>.wav
bool plat_audio_extraire(uint32_t c0, uint32_t c1, uint32_t numero);
void plat_audio_supprimer(uint32_t numero);   // AUDIO_TOUS : tous les sons des pubs (/pub/audio)
// une ligne dans le journal de la carte (console serie). Facultatif : sans
// definition (bancs d'essai), une version vide est utilisee.
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// versions du paquet Espressif : "3.3.10" (Arduino ESP32) et "v5.5.4" (ESP-IDF)
const char *plat_version_arduino();
const char *plat_version_idf();
const uint32_t AUDIO_TOUS = 0xFFFFFFFFu;

// ---- captation (v0.6.3) : le son BRUT du micro (16 kHz mono, sans empreinte)
// dans /pub/captations/cap_NNN.wav (+ index.csv), pour le decouper sur le PC.
// La detection continue pendant la captation. ----
const int CAPTATION_MAX_S = 15 * 60;      // arret automatique
const int AUTO_NB_EV = 8;                 // v0.9.41 : derniers verdicts de la captation AUTO (v0.9.57 : 8)
const int AUTO_NB_CONNUES = 16;           // v0.9.57 : numeros des pubs connues entendues pendant la captation
struct EvAuto {
    uint8_t type;            // 1 pub enregistree (APPRENDRE : apprise), 2 trop courte (ignoree), 3 connue (ignoree), 4 jetee
    char txt[44];            // « Pub enregistrée · 15,3 s »
    char nom[48];            // nom du fichier (type 1)
};
struct Captation {
    int etat;                // 0 arretee, 1 demarrage, 2 en cours, 3 arret demande (en cours)
    uint32_t numero;         // captation en cours (ou la derniere)
    float duree_s;           // duree enregistree
    uint32_t fins;           // +1 a chaque fin (l'ecran affiche le resultat)
    bool fin_ok;             // la derniere s'est bien terminee
    float fin_duree_s;
    char fin_txt[48];        // raison de l'arret (ou de l'echec)
    bool automatique;        // v0.9.39 : captation AUTOMATIQUE (decoupe des pubs inconnues)
    bool apprendre;          // v0.9.53 : captation APPRENDRE (en cours, ou celle du bilan affiche) : auto_gardes = pubs apprises
    int auto_gardes;         // pubs inconnues enregistrees
    int auto_connues;        // pubs deja connues, ignorees
    float auto_seg_s;        // duree du morceau en cours
    bool auto_bilan;         // v0.9.41 : la derniere captation etait AUTO : les statistiques restent affichees
    int auto_courtes;        // pubs trop courtes (moins de 5 s), ignorees
    int auto_jetes;          // morceaux jetes (plus de 90 s, incomplets, erreur)
    float auto_total_s;      // secondes de pubs enregistrees
    int auto_nev;            // verdicts disponibles (0 a AUTO_NB_EV)
    EvAuto auto_ev[AUTO_NB_EV];   // le plus recent en premier
    // v0.9.57 : bilan sur TOUTE la duree de la captation
    float auto_connu_s;      // secondes de pubs connues entendues
    float auto_non_s;        // secondes non traitees : pas de silence pour couper, morceaux jetes ou non appris
    int auto_secours;        // morceaux coupes sans vrai silence (AUTO)
    int auto_k_n;            // nombre de numeros dans auto_k
    int auto_k[AUTO_NB_CONNUES];  // numeros des pubs connues entendues (sans doublon)
    bool auto_garde;         // APPRENDRE : la captation entiere est gardee (filet), son numero = numero
    int auto_sures;          // v0.9.58 : APPRENDRE : pubs sures (OK) ou validees en repassant
};
const char *plat_captation_demarrer();    // nullptr : lancee ; sinon la raison du refus
const char *plat_captation_demarrer_auto();   // v0.9.39 : idem, avec decoupe automatique des pubs
const char *plat_captation_demarrer_apprendre();   // v0.9.53 : AUTO + pubs inconnues apprises dans la bibliotheque
void plat_captation_arreter();
int plat_sd_tampon_actif();                         // v0.9.16 : tampon d'ecriture SD en Ko (0 = pas encore alloue)
int plat_sd_tampon_choisi();                        // valeur gardee en NVS (appliquee au prochain demarrage)
bool plat_sd_tampon_regler(int ko);                 // 4, 8 ou 16
int plat_captation_chaine();                        // v0.9.13 : chaine choisie pour la captation (0 = auto)
void plat_captation_chaine_regler(int c);
void plat_captation_annuler();                      // v0.9.10 : arrete et SUPPRIME le fichier de la captation en cours
void plat_captation(Captation &c);
// v0.9.58 : verrou du moteur d'empreintes (analyse d'un fichier pendant que le direct attend)
void plat_emp_prendre();
void plat_emp_rendre();
// v0.9.58 : fichiers AUTO traites depuis le portail (nom = nom du fichier dans /pub/captations/auto)
bool plat_fichiers_demander(const char *nom, bool ajouter);   // compare (et ajoute s'il est nouveau)
int plat_fichiers_tous(bool nouvelles);       // false : compare tous les fichiers ; true : ajoute tous les « nouvelle »
int plat_fichiers_suppr_connus();             // efface les fichiers deja dans la base (sur)
void plat_fichiers_oublier(const char *nom);  // le fichier a ete supprime
// (plat_fichiers_json() : declaree dans reseau.cpp, elle rend une String Arduino)

// journal des coupures (v0.7.0) : /pub/coupures.csv, ecrit par la tache archive
// evenement : 0 = le son est baisse, 1 = le son revient (duree_s = duree reelle de la coupure)
void plat_coupure_log(int evenement, int numero, int score, int chaine, int duree_s);

// ---- Test de l'entree optique (module TOSLINKBee, I2S sur GPIO45/46/44) : v0.6.4 ----
// Enregistre FO_TEST_S secondes et ecrit sur la SD un WAV a ecouter (v0.9.59) :
//   /pub/captations/fo_test_16k.wav (ce que le moteur entendrait)
const int FO_TEST_S = 10;
struct TestOptique {
    int etat;                // 0 repos, 1 enregistre, 2 ecrit sur la SD
    float duree_s;           // deja enregistre
    float niveau;            // crete du dernier instant, 0..1
    uint32_t fins;           // +1 a chaque fin (l'ecran affiche le resultat)
    bool fin_ok;
    float fin_niveau;        // crete sur tout l'essai, 0..1
    char fin_txt[64];        // resume ou raison de l'echec
};
const char *plat_fo_test_demarrer();      // nullptr : lance ; sinon la raison du refus
void plat_fo_test(TestOptique &t);

// ---- reseau : Wi-Fi, page web du telephone, base de pubs ----
struct Reseau {
    int mode;                // 0 eteint, 1 point d'acces seul, 2 connecte a la box, 3 connexion en cours
    char ssid[33];           // reseau de la box, ou nom du point d'acces
    char ip[16];
    char url[48];            // adresse a ouvrir sur le telephone
    char ap_mdp[16];         // mot de passe du point d'acces
    int clients;             // telephones connectes au point d'acces
    // base decentralisee
    int base_etat;           // 0 locale (pas de reseau), 1 recherche, 2 connectee, 3 injoignable
    char base_adr[40];       // adresse de la base (ip:port)
    char base_nom[32];       // son nom (PC, NAS, ESP base...)
    uint32_t base_pubs;      // pubs dans la base distante
    int base_ms;             // temps de reponse (ms)
    uint32_t base_envoyees;  // pubs envoyees a la base depuis le demarrage
    uint32_t base_copiees;   // pubs de la base copiees sur la carte
};
// ecriture NVS differee (tache a pile interne) : les taches a pile en PSRAM ne
// doivent jamais ecrire en flash elles-memes
void plat_nvs_vider();               // attend que tout soit ecrit (avant un redemarrage)
void plat_reseau(Reseau &r);

// ---- fichiers (carte microSD) ----
bool plat_sd();
bool plat_fichier_ecrire(const char *chemin, const void *buf, size_t n);   // cree ou remplace
bool plat_fichier_ajouter(const char *chemin, const void *buf, size_t n);  // ajoute a la fin
long plat_fichier_lire(const char *chemin, void *buf, size_t nmax);       // -1 si absent
long plat_fichier_taille(const char *chemin);                             // -1 si absent
bool plat_fichier_renommer(const char *de, const char *vers);            // remplace vers s'il existe
bool plat_fichier_supprimer(const char *chemin);
bool plat_dossier_creer(const char *chemin);

// ---- LED RGB de la carte (temoin en mode ecran discret) ----
void plat_led(uint8_t r, uint8_t g, uint8_t b);
void plat_appui_zone(int x, int y, int l, int h);   // v0.9.6 : retour immediat au toucher (flash LED + cadre sur l'ecran)

// ---- petits reglages (NVS) ----
bool plat_nvs_lire(const char *cle, void *buf, size_t n);
void plat_nvs_ecrire(const char *cle, const void *buf, size_t n);
