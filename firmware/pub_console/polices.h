// polices.h - GENERE par outils/generer_polices.py, ne pas modifier
// Police Inter (c) Rasmus Andersson - SIL Open Font License 1.1
#pragma once
#include <stdint.h>

struct GlypheAA { uint16_t code; uint8_t l, h; int8_t dx, dy; uint8_t avance; uint32_t pos; };
struct PoliceAA { const uint8_t *bits; const GlypheAA *glyphes; uint16_t nb;
                  int8_t ascendante; int8_t descendante; uint8_t interligne; };

extern const PoliceAA POLICE_11;   // Inter-Medium.otf 11 px
extern const PoliceAA POLICE_13;   // Inter-SemiBold.otf 13 px
extern const PoliceAA POLICE_16;   // Inter-Bold.otf 16 px
extern const PoliceAA POLICE_22;   // InterDisplay-Bold.otf 22 px
extern const PoliceAA POLICE_30;   // InterDisplay-Bold.otf 30 px
extern const PoliceAA POLICE_40;   // InterDisplay-Bold.otf 40 px
