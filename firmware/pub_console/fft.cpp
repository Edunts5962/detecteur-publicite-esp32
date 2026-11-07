// fft.cpp - FFT radix 2 de 512 points
#include "fft.h"
#include <math.h>
#include <stdint.h>

namespace fft {

static float cos_t[N / 2], sin_t[N / 2];
static uint16_t inv[N];
static bool pret = false;

void init()
{
    if (pret) return;
    int log2n = 0;
    while ((1 << log2n) < N) log2n++;
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < log2n; b++) if (i & (1 << b)) r |= 1 << (log2n - 1 - b);
        inv[i] = (uint16_t)r;
    }
    for (int i = 0; i < N / 2; i++) {
        double a = -2.0 * 3.14159265358979323846 * i / N;
        cos_t[i] = (float)cos(a);
        sin_t[i] = (float)sin(a);
    }
    pret = true;
}

void calcul(float *re, float *im)
{
    for (int i = 0; i < N; i++) {
        int j = inv[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int taille = 2; taille <= N; taille <<= 1) {
        int moitie = taille >> 1, pas = N / taille;
        for (int d = 0; d < N; d += taille) {
            for (int k = 0; k < moitie; k++) {
                float wr = cos_t[k * pas], wi = sin_t[k * pas];
                int a = d + k, b = a + moitie;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

}  // namespace fft
