// tele.h - telecommande Wi-Fi de la tele (Google TV / Android TV : TCL...)
//
// Protocole "Android TV Remote v2", celui des telecommandes Google TV des
// telephones (repris du croquis ESP32S3_TCL_Remote_WebPairing, teste sur la
// TCL : certificats, appairage, touches, ping) :
//   - APPAIRAGE (port 6467, une seule fois) : la carte fabrique une cle RSA
//     2048 bits et un certificat, la tele affiche un code de 6 caracteres
//     (0-9 A-F) qu'on tape sur l'ecran de la carte ou sur le portail. La cle et
//     le certificat sont gardes sur la carte SD (/pub/tv/).
//   - TELECOMMANDE (port 6466, en permanence) : touches Volume + / Volume -,
//     reponse aux "ping" de la tele, et LECTURE DU VOLUME : la tele envoie
//     d'elle-meme son volume (niveau, maximum, muet) a la connexion et a chaque
//     changement.
//
// Pendant une pub, la carte memorise le volume actuel puis descend au NIVEAU
// BAS ; a la fin, elle remonte au NIVEAU HAUT, ou au niveau d'origine. La tele
// n'accepte pas "mets le volume a 12" : la carte envoie le nombre d'appuis
// Volume - / Volume + qu'il faut et verifie le volume renvoye par la tele.
//
// Tout le reseau se passe dans la tache "tele" ; les fonctions ci-dessous ne
// font que deposer une demande (appelables depuis n'importe quelle tache).
#pragma once
#include <stdint.h>

namespace tv {

enum Etat {
    NON_APPAIREE = 0,   // pas de cle sur la carte SD : appairage a faire
    HORS_LIGNE,         // appairee, pas connectee (tele eteinte, IP fausse, Wi-Fi absent)
    CONNEXION,          // connexion en cours
    CONNECTEE,          // prete : touches et volume
    APP_CLE,            // appairage : fabrication de la cle (quelques secondes)
    APP_CONNEXION,      // appairage : connexion a la tele
    APP_CODE,           // appairage : la tele affiche le code, on attend qu'il soit tape
    APP_VALIDATION,     // appairage : verification du code
};

enum Action { RIEN = 0, BAISSER, REMONTER, DIMINUER, AUGMENTER, MILIEU };   // v0.9.64 : MILIEU = palier entre bas et volume d'avant (v0.9.65 : 10, 30 ou 65 %)

struct Infos {
    int etat;
    char ip[16];
    bool appairee;           // cle + certificat presents sur la carte SD
    bool volume_connu;       // la tele a donne son volume
    int volume, volume_max;
    bool muet;
    bool baisse;             // la carte a baisse le son (elle le remontera)
    int origine;             // volume avant la baisse (-1 inconnu)
    int action;              // Action en cours
    int depart, cible;       // pendant l'action : volume de depart, volume vise
    uint32_t action_t0;      // debut de l'action (ms)
    char msg[72];            // dernier resultat : "Volume 18 → 3 ✓", "Télé injoignable"...
    uint32_t msg_n;          // change a chaque message
    int msg_type;            // 0 info, 1 reussi, 2 erreur
    int app_reste_s;         // APP_CODE : secondes restantes pour taper le code
    uint32_t ping_age_s;     // secondes depuis le dernier message de la tele (connectee)
    uint32_t connexions;     // connexions reussies depuis le demarrage
};

// reglages (ecrits par config.cpp)
extern volatile int niveau_bas;      // volume pendant la pub (0..100)
extern volatile int niveau_haut;     // volume apres la pub (0..100)
extern volatile bool origine;        // apres la pub : revenir au volume d'avant (sinon niveau haut)

void init();                         // relit la cle et le certificat (carte SD montee)
void demarrer();                     // cree la tache "tele"

// appele par la tache moteur a chaque passage : baisse / remonte sur les changements
void suivre(bool baisser);
void suivre_niveau(int n);           // 0 son normal (volume d'avant), 1 son bas, 2..4 paliers de remontee (v0.9.65)
bool en_cours();                     // la carte a baisse le son et ne l'a pas encore remonte

// ---- demandes (ecran, portail) ----
void appairer();                     // nouvelle cle, puis la tele affiche un code (IP : cfg::R.tv_ip)
void code(const char *c);            // le code affiche par la tele (6 caracteres 0-9 A-F)
void annuler_appairage();
void oublier();                      // efface la cle, le certificat ; il faudra appairer a nouveau
void reconnecter();                  // l'IP a change : nouvelle connexion
// essais (menu Tele) : memes actions que pendant une pub
void couper();                       // descend au niveau bas (memorise le volume actuel)
void retablir();                     // remonte au niveau haut (ou au volume d'origine)
void diminuer();                     // un cran de moins
void augmenter();                    // un cran de plus (v0.9.17)
void lire();                         // affiche le volume actuel (reconnecte si inconnu)
void infos(Infos &i);
const char *nom_etat(int etat);

}  // namespace tv
