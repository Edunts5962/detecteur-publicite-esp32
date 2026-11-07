// carte.h - services de pub_console.ino utilises par reseau.cpp (carte seulement)
#pragma once
#include <stdint.h>

void sd_prendre();                    // acces exclusif a la carte SD (mutex recursif)
void sd_rendre();
bool sd_ok();
// decoupe le WAV d'un bloc : garde [debut, fin] (dixiemes de seconde), en tache de fond
void audio_couper(uint32_t numero, int debut_ds, int fin_ds);

