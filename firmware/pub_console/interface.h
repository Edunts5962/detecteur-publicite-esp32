// interface.h - ecrans et tactile du detecteur de pub
#pragma once
#include <stdint.h>

#define PUB_VERSION "v0.9.72-35P"

namespace ui {

// fond : tampon L*H facultatif (PSRAM) pour le fond pre-calcule, ou nullptr
void init(uint16_t *fond);

// a chaque lecture du tactile (toutes les ~4 ms) : etat du doigt (ecran 320x240).
// Le bouton reagit DES L'APPUI (couleur et action), pas au relachement.
void toucher(bool appui, int x, int y);

// faut-il dessiner une image maintenant ? (appui, changement, animation, ou
// rafraichissement lent du vumetre) ; sinon la boucle ne fait que lire le tactile
bool a_dessiner(uint32_t ms);
void redessiner();                   // la prochaine image tout de suite
void aller_onglet(const char *nom);    // le portail change d'onglet : l'ecran de la carte suit (v0.8.9)
void suivre(uint32_t ms);            // ecran eteint : on suit les evenements sans dessiner

// dessine une image complete dans gfx::fb
void image(uint32_t ms);

// « REMETTRE LE SON » (bouton de l'ecran ou du portail) : meme effet, meme message
void remettre_son();
// dernier message affiche sur l'ecran (resultat reel d'une commande : "Pub n°12
// apprise", "Volume 18 → 3 ✓"...) : le portail le reaffiche.
// Renvoie son numero (0 = aucun), copie le texte et la couleur (RGB565).
uint32_t dernier_message(char *txt, int n, uint16_t *couleur, char *detail = nullptr, int nd = 0);

}  // namespace ui
