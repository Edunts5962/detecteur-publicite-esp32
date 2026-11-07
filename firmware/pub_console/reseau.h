// reseau.h - Wi-Fi, page web du telephone, base de pubs (carte seulement)
//
//  - Wi-Fi : connexion directe a la box (identifiants.h, ou reseau donne par le
//    portail) ; point d'acces "Detecteur-Pub-XXXX" (mot de passe pub12345)
//    seulement en secours, box injoignable depuis 2 min.
//  - Page web : http://detecteur-pub.local (ou l'adresse IP affichee a l'ecran).
#pragma once
#include "plateforme.h"

namespace reseau {

void preparer();                     // lit les identifiants de la box (debut de setup)
void demarrer();                     // cree la tache reseau (coeur 1)
void infos(Reseau &r);

}  // namespace reseau
