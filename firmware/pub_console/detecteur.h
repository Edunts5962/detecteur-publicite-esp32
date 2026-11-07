// detecteur.h - le verdict PUB / PROGRAMME, le bouton TESTER (facon Shazam),
// les commandes, l'historique (identique sur la carte et le PC).
//
// v0.6.0 : plus de jingles, plus d'apprentissage automatique, plus de commande
// vocale.
// v0.6.2 : c'est l'utilisateur qui marque la pub. TESTER = debut de la pub : la
// carte ecoute 3 s ; pub connue -> PUB tout de suite (le son est baisse) ; pub
// inconnue -> l'enregistrement part de l'appui sur TESTER et dure jusqu'a
// ENREGISTRER (fin de la pub : l'empreinte est sauvee). Plus d'arret au silence,
// plus d'analyse ni de decoupe automatique. ANNULER pour l'oublier ; au-dela de
// ENREG_MAX_S sans ENREGISTRER, rien n'est appris.
//
// Tout ce qui modifie la bibliotheque se passe dans la tache moteur :
// l'interface ne fait que DEMANDER (commande) et LIRE (vue, evenements).
#pragma once
#include <stdint.h>
#include "empreinte.h"

namespace det {

enum Etat { SILENCE, APPRENTISSAGE, PROGRAMME, PUB_PROBABLE, PUB, ENREGISTREMENT, PAUSE, NB_ETATS };
enum Commande {
    CMD_PAS_PUB = 1,    // « ce n'est pas une pub » : la pub reconnue a tort est corrigee a cet endroit
    CMD_ANNULER,        // pendant l'enregistrement : rien n'est appris ; juste apres : la pub apprise est oubliee
    CMD_EFFACER,        // toute la bibliotheque
    CMD_SUPPRIMER,      // a = bloc
    CMD_RENOMMER,       // a = bloc, b = chaine (-1 inchangee), texte = nom (vide = inchange)
    CMD_COUPER,         // a = bloc, b = debut, c = fin (dixiemes de seconde dans le bloc)
    CMD_EXPORTER,       // a = chaine (-1 toutes), texte = chemin du fichier
    CMD_IMPORTER,       // texte = chemin du fichier
    CMD_MARQUER_DISTANT,    // a = bloc, b = identifiant dans la base distante (envoi reussi)
    CMD_SYNCHRONISER,       // la base distante vient d'etre trouvee : lui envoyer les pubs qu'elle n'a pas
    CMD_GARDER,             // a = bloc (-1 : tous) : ecoutee, c'est bon (plus "a verifier")
    CMD_PURGER,             // supprime les pubs jamais revues, apprises il y a plus de a jours (sauf « a verifier »)
    CMD_TESTER,             // debut de la pub : la carte ecoute 3 s ; connue -> PUB ; inconnue -> enregistrement
    CMD_ENREGISTRER,        // fin de la pub : l'enregistrement s'arrete et l'empreinte est sauvee
    CMD_APPRENDRE_AUTO,     // v0.9.53 : captation APPRENDRE : apprendre le passage a..b (colonnes), c = chaine (+ 256 : deux vrais silences)
    CMD_FICHIER,            // v0.9.58 : codes d'un fichier (fichier_poser) : a = 1 ajouter si nouveau, 0 comparer seulement ; b = chaine
    CMD_SUPPRIMER_A_VERIFIER,   // v0.9.58 : supprime toutes les pubs « a verifier »
};

// v0.9.53 : resultat d'un CMD_APPRENDRE_AUTO (lu par la captation APPRENDRE)
enum { AA_APPRISE = 1, AA_CONNUE, AA_VIDE, AA_PLEINE };
struct ResAuto {
    int statut;                      // AA_...
    int numero;                      // pub apprise (ou deja connue)
    float duree;                     // secondes
    int doute;                       // v0.9.58 : AA_APPRISE : 0 sure (OK tout de suite), sinon base::DOUTE_... ; AA_CONNUE : 1 = a verifier revue -> validee
};
bool auto_resultat(uint32_t &lu, ResAuto &r);   // le resultat suivant (lu : compteur de l'appelant)
// v0.9.58 : fichier AUTO traite depuis le portail. La tache des fichiers calcule les codes,
// les pose ici puis envoie CMD_FICHIER ; le detecteur (seul a toucher la bibliotheque)
// compare, ajoute si demande, et rend le resultat.
enum { FI_NOUVELLE = 1, FI_RESSEMBLE, FI_CONNUE, FI_AJOUTEE, FI_PLEINE, FI_VIDE };
struct ResFichier { uint32_t seq; int statut, numero, pct; };
void fichier_poser(const emp::Code *c, int n, uint32_t t0, float duree_s);
uint32_t fichier_seq();                         // avant d'envoyer CMD_FICHIER
bool fichier_resultat(uint32_t seq_avant, ResFichier &r);   // vrai quand le resultat est la
enum Type {
    EV_PUB_DEBUT = 1, EV_PUB_FIN, EV_BLOC_COURT, EV_BLOC_PLEIN, EV_DEJA_OUVERT, EV_CORRECTION,
    EV_RIEN_A_CORRIGER, EV_ANNULE, EV_RIEN_A_ANNULER, EV_EFFACE, EV_SAUVE, EV_SUPPRIME, EV_COUPE,
    EV_EXPORTE, EV_IMPORTE,
    EV_DISTANT,         // pub reconnue par la base distante (a = identifiant distant)
    EV_CACHE,           // pub de la base distante copiee sur la carte (a = numero local)
    EV_SON_FAIBLE,      // la carte entendait mal pendant l'enregistrement (a = niveau dB)
    EV_SAUVE_ECHEC,     // la bibliotheque n'a pas pu etre ecrite sur la carte SD
    EV_PURGE,           // a = pubs jamais revues supprimees
    EV_TEST,            // « TESTER » : b = 0 connue (a = numero, x = passages), 3 rien entendu,
                        // 5 inconnue : enregistrement lance, 6 apprise (a = numero, x = duree),
                        // 7 rien appris (a = numero si deja connue), 8 trop long : rien appris (x = duree max)
};

struct Evenement {
    int type;
    int a, b;                        // selon le type : numero de bloc, nombre...
    float x;                         // duree, score...
};

struct Vue {
    int etat;
    float confiance;                 // 0..1
    int numero;                      // bloc reconnu (numero affiche), 0 = aucun
    int vus;                         // fois ou ce bloc a ete reconnu
    float pos_s, duree_s;            // ou on en est dans le bloc reconnu
    int score, second;               // votes du meilleur bloc et du suivant
    bool enreg;                      // pub en cours d'enregistrement (apres TESTER)
    float enreg_s;                   // sa duree actuelle
    int test_etat;                   // « TESTER » : 0 rien, 1 ecoute, 2 resultat (connue ou apprise), 4 enregistrement
    int test_num;                    // pub reconnue (ou apprise) par le test
    float test_age_s;                // depuis le debut de l'etat de test (animation, delai d'ANNULER)
    bool test_appris;                // etat 2 : la pub vient d'etre apprise (ANNULER possible)
    float pub_jour_s;                // secondes de pub reconnues depuis le demarrage
    int ecrans;                      // ecrans publicitaires reconnus
    float ecoute_s;                  // secondes d'ecoute
    bool distant;                    // PUB reconnue par la base distante (pas encore copiee ici)
    int chaine;                      // chaine regardee (reglee ou devinee), 0 = inconnue
    bool chaine_devinee;
    uint32_t nb_pas_pub;             // "ce n'est pas une pub" (compteur : la coupure du son y voit les nouveautes)
    float age_match_s;               // v0.9.39 : il y a combien de secondes (son) la bibliotheque a reconnu quelque chose (99 = jamais)
    uint32_t col;                    // v0.9.57 : colonne d'empreintes a laquelle pos_s se rapporte (repere de coupure AUTO)
};

// ---- base decentralisee (serveur PC, NAS, ESP "base") ----
// La tache reseau envoie les codes entendus a la base et lui rend ses reponses.
struct ResDistant {
    uint32_t id;                     // identifiant du bloc dans la base distante (0 = rien)
    int score;
    uint32_t t_dernier;              // colonne (horloge de la carte) du dernier code concordant
    int32_t pos;                     // instant atteint dans le bloc (colonnes)
    int type;                        // base::PUB (les anciens jingles sont ignores)
    int chaine;
    int duree_s;
    char nom[28];
};
void distant_resultat(const ResDistant &r);          // tache reseau : reponse de la base
uint32_t distant_a_telecharger();                    // tache reseau : bloc a copier ici (0 = rien)
bool distant_paquet(const uint8_t *buf, int n, uint32_t id);   // tache reseau : paquet recu (copie)
// pubs apprises a envoyer a la base : la tache reseau prend le paquet, l'envoie,
// puis signale l'identifiant que la base lui a donne
bool sortie_prendre(const uint8_t *&buf, int &n, int &bloc);
void sortie_faite(int bloc, uint32_t id_distant);    // id 0 = echec (on reessaiera plus tard)
// codes recents pour la base (tache reseau) : copie au plus max codes arrives
// depuis le dernier appel
int distant_codes(emp::Code *out, int max);

// reglages (ecrits par config.cpp)
extern volatile int seuil_votes;     // votes coherents pour dire PUB
// resultat de la derniere exportation / importation (pour la page web)
extern volatile int resultat;
extern volatile uint32_t nb_resultats;
extern volatile bool actif;          // detecteur en marche (etiquette en haut a droite, page web)
extern volatile int jingle_hp;       // bip du haut-parleur : 0 non, 1 au debut de la pub, 2 debut et fin
extern volatile int chaine_reglee;   // 0 = devinee
extern volatile int mode_base;       // 0 locale, 1 decentralisee, 2 les deux
const int ENREG_MAX_S = 90;          // TESTER : sans ENREGISTRER apres 90 s, rien n'est appris
const int ANNULER_S = 20;            // apres l'apprentissage : ANNULER reste propose 20 s

void init();
bool commande(int cmd, int a = 0, int b = 0, int c = 0, const char *texte = nullptr);   // depuis l'ecran ou la page web ; false : file pleine
bool evenement(Evenement &e);        // depuis l'interface : prochain evenement
void vue(Vue &v);                    // depuis l'interface : copie de l'etat

// historique : une case toutes les 10 s, 30 minutes
const int NB_CASES = 180;
const int CASE_S = 10;
extern uint8_t historique[NB_CASES]; // etat dominant de la case k -> historique[k % NB_CASES]
extern volatile uint32_t nb_cases;   // cases terminees

// sauvegarde de la bibliotheque sur la carte SD : a jour ? depuis quand ? echec ?
void sauvegarde(bool &a_jour, uint32_t &il_y_a_s, bool &echec);

// ---- appele par la tache moteur ----
void codes(const emp::Code *c, int n);
void periodique(float niveau_db, bool sature);

}  // namespace det
