// fft.h - FFT complexe de 512 points (radix 2, en place), partagee par
// les empreintes (8 kHz) et la voix (16 kHz). Appelee seulement depuis la
// tache moteur : pas besoin de verrou.
#pragma once

namespace fft {

const int N = 512;

void init();                          // tables de rotation et d'inversion des bits
void calcul(float *re, float *im);    // N points, en place, sens direct

}  // namespace fft
