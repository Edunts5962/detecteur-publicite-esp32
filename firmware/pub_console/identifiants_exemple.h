// identifiants_exemple.h - MODELE. Copier ce fichier en « identifiants.h » (meme dossier)
// et y mettre le nom et le mot de passe du Wi-Fi de votre box.
//
// La carte se connecte directement a ce reseau au demarrage. Le point d'acces
// "Detecteur-Pub-XXXX" ne s'allume qu'en SECOURS, si la box reste injoignable
// 2 minutes (pour pouvoir quand meme ouvrir le portail : http://192.168.4.1).
// Un autre reseau donne depuis le portail (Reglages -> Wi-Fi de la box) passe
// avant celui-ci (il est garde dans la memoire de la carte).
//
// ATTENTION : identifiants.h contient votre mot de passe. Il est ignore par git
// (.gitignore) : ne le publiez pas.
#pragma once

#define WIFI_BOX_SSID   "nom_de_votre_box"
#define WIFI_BOX_MDP    "mot_de_passe_wifi"
#define WIFI_SECOURS_MS 120000UL     // box injoignable depuis 2 min : point d'acces de secours
