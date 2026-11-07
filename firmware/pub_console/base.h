// base.h - la bibliotheque des pubs apprises, le journal des 10 dernieres
// minutes et la recherche des repetitions (identique sur la carte et le PC).
//
// BIBLIOTHEQUE : des "blocs de pub" (tout ce qui a ete entendu entre
//   « c'est une pub » et « c'est pas une pub »). Chaque code d'un bloc
//   devient une entree (code, bloc, instant dans le bloc), et toutes les
//   entrees sont triees par code : une recherche = une dichotomie.
// RECONNAISSANCE : chaque code entendu qui existe dans la bibliotheque vote
//   pour (bloc, decalage = instant entendu - instant dans le bloc). Si la
//   meme pub repasse, des dizaines de votes tombent sur le meme decalage.
// JOURNAL : les codes des 10 dernieres minutes (apprentissage retroactif).
// REPETITION : le passage en cours a-t-il deja ete entendu entre 1 et
//   10 minutes plus tot ? (indice de pub, sans rien avoir appris)
#pragma once
#include <stdint.h>
#include "empreinte.h"

namespace base {

const int CAP = 400000;              // entrees (8 octets) : ~3 h 30 de pubs (v0.5 : moitie moins d'entrees par pub)
const int MAX_BLOCS = 512;
const int JOURNAL = 1 << 17;         // codes gardes : 18 min a 120 codes/s (un ecran pub de 12 min tient en entier)

struct Entree { uint32_t h; uint16_t bloc; uint16_t t; };

// JINGLE_DEBUT_FIN : certaines chaines passent le MEME jingle au debut et a la
// fin de l'ecran pub : premier passage = debut, passage suivant = fin
enum { PUB = 0, JINGLE_DEBUT = 1, JINGLE_FIN = 2, JINGLE_DEBUT_FIN = 3, NB_TYPES };
inline bool est_jingle(int type) { return type != PUB; }
inline bool jingle_ouvre(int type) { return type == JINGLE_DEBUT || type == JINGLE_DEBUT_FIN; }
inline bool jingle_ferme(int type) { return type == JINGLE_FIN || type == JINGLE_DEBUT_FIN; }

struct Bloc {
    uint8_t actif;
    uint8_t type;                    // PUB, JINGLE_DEBUT ou JINGLE_FIN
    uint16_t duree_s;                // duree du bloc
    uint32_t nb;                     // entrees dans la bibliotheque
    uint32_t vus;                    // reconnaissances
    uint32_t numero;                 // numero affiche (1, 2, 3...)
};

bool init();                         // memoire (PSRAM) ; false si echec
bool prete();
int nb_entrees();
int nb_blocs();                      // blocs actifs (pubs et jingles)
int nb_pubs();                       // blocs de pub seulement
int max_numero();
const Bloc &bloc(int id);
float remplissage();                 // 0..1

// ajoute un bloc (codes dates en colonnes absolues, t0 = debut du bloc)
// renvoie son identifiant, -1 si plein
int ajouter_bloc(const emp::Code *c, int n, uint32_t t0, int duree_s, int type = PUB);
// supprime une pub (ou un jingle) ET son son sur la carte SD, et l'ecrit dans le
// journal avec la raison (avant : le son restait, « son orphelin »)
bool supprimer_bloc(int id, const char *raison = "supprimée");
int supprimer_plage(int id, int t0, int t1);   // entrees d'un bloc entre deux instants (colonnes)
void compter_vu(int id);
void effacer();
bool sauver();
bool charger();
int pubs_du_fichier(const char *chemin);   // v0.9.25 : nombre de pubs d'un base.bin (-1 : fichier invalide)

// ---- fiche de chaque bloc : nom, chaine, origine (fichier /pub/infos.bin) ----
// chaines : TNT nationale (1..26, numerotation 2025), puis les chaines du cable,
// du satellite et de Molotov qui diffusent de la pub. 0 = inconnue.
const int NB_CHAINES = 48;
extern const char *const CHAINES[NB_CHAINES];
extern const char *const CHAINES_DOSSIER[NB_CHAINES];   // nom de dossier sur la carte SD (ASCII)
extern const uint8_t CHAINES_TNT[NB_CHAINES];            // numero TNT (0 = hors TNT)
enum { ORIG_BOUTON = 0, ORIG_IMPORT = 1, ORIG_AUTO = 2, ORIG_CACHE = 3 };   // appris au bouton / a la voix, paquet, appris tout seul, copie de la base distante
struct Info {
    char nom[28];
    uint8_t chaine;
    uint8_t audio;                   // WAV disponible sur la carte SD
    uint8_t origine;                 // ORIG_...
    uint8_t drapeaux;                // bit 0 : a verifier (appris recemment, pas encore ecoute) ; 0 dans les anciennes fiches
    uint32_t distant;                // identifiant dans la base distante (0 = aucun)
    uint32_t appris_le;              // date (secondes depuis 1970, 0 = inconnue)
    uint32_t vu_le;                  // derniere reconnaissance (idem)
};
const Info &info(int id);
void renommer(int id, const char *nom, int chaine);
void changer_type(int id, int type);   // jingle de debut ou de fin -> le meme jingle (debut et fin)   // nom nullptr = inchange, chaine < 0 = inchangee
void marquer_audio(int id, bool present);
void marquer_a_verifier(int id, bool oui);   // id < 0 : toutes les pubs (« non » efface aussi la raison du doute)
// v0.9.58 : raison du doute d'une pub apprise toute seule (bits 1..3 de drapeaux, gardes par gestion_pubs.py)
enum { DOUTE_AUCUN = 0, DOUTE_SILENCE = 1, DOUTE_DUREE = 2, DOUTE_RESSEMBLE = 3, DOUTE_COURT = 4, DOUTE_COUPURE = 5 };   // 5 : v0.9.64, apprise pendant une coupure (entre deux pubs)
void marquer_doute(int id, int raison);
int doute(int id);
bool a_verifier(int id);
int nb_a_verifier();
uint32_t nb_detections();             // v0.9.56 : total des passages reconnus (somme des « vus » des pubs)
void marquer_origine(int id, int origine, uint32_t distant);
int trouver_distant(uint32_t distant);                // bloc local copie de ce bloc distant, -1 sinon
// fait de la place pour n entrees et un bloc en oubliant les copies de la base
// distante les moins recemment vues ; false si impossible
bool liberer_place(int n);
// ne garde que les codes entre deux instants du bloc (decoupage), en colonnes
int garder_plage(int id, int t0, int t1);

// ---- paquets de pubs (.pub) : partage, sauvegarde, banque par chaine ----
// exporte les blocs (tous, ou ceux d'une chaine si chaine >= 0) vers un fichier ;
// max > 0 : seulement les max blocs les plus souvent reconnus (les plus recurrents)
int exporter(const char *chemin, int chaine, int max = 0, bool sans_cache = false);
// ajoute les blocs d'un paquet ; renvoie le nombre de blocs ajoutes (-1 fichier invalide)
int importer(const char *chemin);
// paquet d'UN bloc en memoire (envoi a la base distante) ; renvoie sa taille, 0 si trop petit
int exporter_bloc(int id, uint8_t *buf, int max);
// importe un paquet en memoire ; premier = identifiant local du premier bloc ajoute
int importer_tampon(const uint8_t *buf, int taille, int origine, uint32_t distant, int *premier = nullptr);
// taille d'un paquet d'un bloc (octets)
int taille_paquet(int nb_entrees);

// ---- reconnaissance ----
struct Resultat {
    int bloc;                        // -1 : rien
    int score;                       // votes coherents du meilleur bloc
    int score_fort;                  // dont votes de codes forts
    int second;                      // votes du meilleur autre bloc
    int32_t pos;                     // instant atteint dans le bloc (colonnes)
    uint32_t t_dernier;              // colonne du dernier code entendu qui a vote pour ce bloc
    int32_t decalage;                // colonne entendue - colonne dans le bloc
    int32_t etendue;                 // colonnes entre le premier et le dernier code qui ont vote pour ce bloc
    // meilleur jingle (debut ou fin d'ecran pub), cherche a part des pubs
    int jingle;                      // -1 : aucun
    int jingle_score;
    uint32_t jingle_t;               // colonne du dernier code qui a vote pour lui
    int32_t jingle_pos;              // instant atteint dans le jingle (colonnes)
};
void reconnaitre(const emp::Code *c, int n, Resultat &r);
// Recherche INCREMENTALE : chaque code entendu n'est cherche qu'une fois, a son
// arrivee ; ses "touches" (bloc, instant dans le bloc) sont gardees, et les
// votes des 4 dernieres secondes se refont a partir d'elles (15 fois par seconde)
// sans plus toucher a la bibliotheque. 30 fois moins de recherches.
struct Touche { uint32_t tq; uint32_t idx; uint16_t bloc; uint16_t tb; uint8_t fort; uint8_t r[3]; };
int chercher(const emp::Code &c, Touche *out, int max);      // touches d'un code (24 au plus)
void reconnaitre_touches(const Touche *t, int n, Resultat &r);
int accords_touches(const Touche *t, int n, int bloc, int32_t decalage, uint32_t &dernier, bool forts_seuls);
uint32_t version();                  // change a chaque modification de la bibliotheque

// ---- consolidation : la bibliotheque se nettoie toute seule ----
// Chaque entree compte combien de fois elle a ete "reentendue" pendant une
// diffusion reconnue de son bloc. Une fois qu'un passage du bloc a ete reconnu
// au moins CONSOLIDE fois, ses entrees jamais reentendues (conversation dans la
// piece, bruit pendant l'apprentissage) sont retirees : moins de fausses
// reconnaissances, plus de place.
#ifndef CONSOLIDE_DEF
#define CONSOLIDE_DEF 4
#endif
const int CONSOLIDE = CONSOLIDE_DEF;
#ifndef CONSOLIDE_PART
#define CONSOLIDE_PART 0.5f      // part des entrees d'une tranche deja reentendues pour oser nettoyer
#endif
void confirmer(uint32_t idx, int bloc);          // l'entree idx (touche) vient d'etre reentendue
void couvrir(int bloc, int32_t t0, int32_t t1);   // ce passage du bloc (colonnes) a ete reconnu une fois de plus
int consolider(int bloc);                        // retire les entrees jamais reentendues des passages bien couverts
// pour une pub deja reconnue (bloc + decalage) : combien de codes de c
// tombent dessus (a 2 colonnes pres), et la colonne du plus recent
int accords(const emp::Code *c, int n, int bloc, int32_t decalage, uint32_t &dernier);
// ce code existe-t-il deja dans une pub apprise ? (pour apprendre un jingle sans le spot d'a cote)
bool dans_une_pub(uint32_t h);

// ---- journal ----
void journal_ajouter(const emp::Code *c, int n);
int journal_extraire(uint32_t t0, uint32_t t1, emp::Code *out, int max);
uint32_t journal_plus_ancien();      // colonne du plus vieux code garde
// place restante avant que le code de la colonne t0 soit ecrase : codes plus
// anciens que t0 encore gardes + cases libres (surveillance d'un enregistrement)
uint32_t journal_marge(uint32_t t0);



}  // namespace base
