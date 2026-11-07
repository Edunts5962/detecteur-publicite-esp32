// distant.h - client de la base de pubs decentralisee (carte seulement)
//
// La base (serveur PC "serveur_base.py", NAS, Raspberry Pi, ou un deuxieme
// ESP32 "pub_base") garde une bibliotheque sans limite pratique. La carte lui
// envoie ses codes (UDP, toutes les 100 ms, ~3 Ko/s, jamais de son) ; la base
// repond avec la pub reconnue. Une pub reconnue par la base est copiee sur la
// carte (HTTP) : la suite et les passages suivants sont reconnus sans reseau.
// Les pubs apprises sur la carte partent vers la base.
//
// Protocole : outils/serveur_base/PROTOCOLE.md
#pragma once
#include "plateforme.h"

namespace distant {

const uint16_t PORT_UDP = 4510;      // requetes et decouverte
const uint16_t PORT_HTTP = 4511;     // paquets (telecharger / envoyer)

void boucle(bool sta);               // appele par la tache reseau (non bloquant, sauf transferts)
void infos(Reseau &r);               // champs base_* de l'etat reseau
void oublier();                      // l'adresse a change : on cherche a nouveau

}  // namespace distant
