// journal.h - console serie gardee en memoire (carte seulement)
//
// Tout ce que la carte ecrit sur le port serie (Console.printf / println) ET les
// messages du systeme (ESP-IDF, Wi-Fi : esp_log) sont gardes
// dans un anneau de lignes en PSRAM. L'ecran (Journal) et le portail
// (/api/journal) les relisent : la console serie de l'IDE Arduino, sans cable.
#pragma once
#include <Arduino.h>
#include "plateforme.h"

class ConsoleSerie : public Print {
public:
    void begin(unsigned long vitesse);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *b, size_t n) override;
    using Print::write;
};
extern ConsoleSerie Console;

void journal_init();                 // a appeler tout au debut de setup()
void journal_rapport_demarrage();    // ecrit la cause du demarrage (et le journal d'avant un plantage)
bool journal_anormal();              // ce demarrage suit un plantage, un chien de garde ou une chute de tension
const char *journal_cause();
void journal_plantages_effacer();          // v0.9.24 : efface le message de redemarrage anormal
uint32_t journal_plantages();        // redemarrages anormaux depuis la mise sous tension
// JOURNAL COUPE (reglage) : plus rien n'est garde en memoire pour l'ecran et le
// portail, sauf les erreurs (ECHEC, ERREUR, E (...), PLANTAGE) ; le port USB
// recoit toujours tout, et les 32 dernieres lignes gardees pour un plantage aussi.
void journal_activer(bool oui);
bool journal_actif();
