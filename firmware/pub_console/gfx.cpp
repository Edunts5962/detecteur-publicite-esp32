// gfx.cpp - voir gfx.h
#include "gfx.h"
#include <math.h>
#include <string.h>

namespace gfx {

uint16_t *fb = nullptr;
static int cx0 = 0, cy0 = 0, cx1 = L, cy1 = H;

void init(uint16_t *tampon) { fb = tampon; sans_decoupe(); }

void decoupe(int x0, int y0, int x1, int y1)
{
    cx0 = x0 < 0 ? 0 : x0;  cy0 = y0 < 0 ? 0 : y0;
    cx1 = x1 > L ? L : x1;  cy1 = y1 > H ? H : y1;
}
void sans_decoupe() { cx0 = 0; cy0 = 0; cx1 = L; cy1 = H; }

static inline float sat(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Melange rapide sur 5 bits d'opacite (astuce des champs separes du RGB565)
static inline uint16_t mel(uint16_t fond, uint16_t c, uint8_t a)
{
    uint32_t al = (a + 4) >> 3;
    if (al == 0) return fond;
    if (al >= 32) return c;
    uint32_t b = (fond | ((uint32_t)fond << 16)) & 0x07E0F81F;
    uint32_t f = (c | ((uint32_t)c << 16)) & 0x07E0F81F;
    uint32_t r = ((((f - b) * al) >> 5) + b) & 0x07E0F81F;
    return (uint16_t)((r >> 16) | r);
}

uint16_t melange(uint16_t c0, uint16_t c1, uint8_t t)
{
    int r0 = c0 >> 11, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    int r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    int r = r0 + ((r1 - r0) * t + 127) / 255;
    int g = g0 + ((g1 - g0) * t + 127) / 255;
    int b = b0 + ((b1 - b0) * t + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void put(int x, int y, uint16_t c, uint8_t a)
{
    uint16_t *p = &fb[y * L + x];
    *p = (a >= 250) ? c : mel(*p, c, a);
}

void pixel(int x, int y, uint16_t c, uint8_t a)
{
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || a == 0) return;
    put(x, y, c, a);
}

static inline void ajout(int x, int y, uint16_t c, uint32_t a)
{
    uint16_t *p = &fb[y * L + x];
    uint16_t f = *p;
    int r = (f >> 11) + (((c >> 11) * a) >> 8);
    int g = ((f >> 5) & 63) + ((((c >> 5) & 63) * a) >> 8);
    int b = (f & 31) + (((c & 31) * a) >> 8);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    *p = (uint16_t)((r << 11) | (g << 5) | b);
}

void pixel_ajout(int x, int y, uint16_t c, uint8_t a)
{
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || a == 0) return;
    ajout(x, y, c, a);
}

void rect(int x, int y, int l, int h, uint16_t c, uint8_t a)
{
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + l > cx1 ? cx1 : x + l, y1 = y + h > cy1 ? cy1 : y + h;
    if (x0 >= x1 || y0 >= y1 || a == 0) return;
    for (int j = y0; j < y1; j++) {
        uint16_t *p = &fb[j * L + x0];
        if (a >= 250) for (int i = x0; i < x1; i++) *p++ = c;
        else          for (int i = x0; i < x1; i++, p++) *p = mel(*p, c, a);
    }
}

void degrade_v(int x, int y, int l, int h, uint16_t c0, uint16_t c1, uint8_t a)
{
    for (int j = 0; j < h; j++)
        rect(x, y + j, l, 1, melange(c0, c1, h > 1 ? (uint8_t)(255 * j / (h - 1)) : 0), a);
}

void degrade_h(int x, int y, int l, int h, uint16_t c0, uint16_t c1, uint8_t a)
{
    for (int i = 0; i < l; i++)
        rect(x + i, y, 1, h, melange(c0, c1, l > 1 ? (uint8_t)(255 * i / (l - 1)) : 0), a);
}

// Distance signee au bord d'un rectangle arrondi (negatif = dedans)
static inline float sdf_rect(float px, float py, float mx, float my, float hx, float hy, float r)
{
    float qx = fabsf(px - mx) - (hx - r);
    float qy = fabsf(py - my) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float dedans = qx > qy ? qx : qy;
    if (dedans > 0) dedans = 0;
    return sqrtf(ox * ox + oy * oy) + dedans - r;
}

static void rect_arrondi_gen(int x, int y, int l, int h, int r, uint16_t c0, uint16_t c1,
                             uint8_t a, float ep)
{
    if (l <= 0 || h <= 0) return;
    if (r * 2 > l) r = l / 2;
    if (r * 2 > h) r = h / 2;
    float mx = x + l * 0.5f, my = y + h * 0.5f, hx = l * 0.5f, hy = h * 0.5f;
    int y0 = y < cy0 ? cy0 : y, y1 = y + h > cy1 ? cy1 : y + h;
    int x0 = x < cx0 ? cx0 : x, x1 = x + l > cx1 ? cx1 : x + l;
    for (int j = y0; j < y1; j++) {
        uint16_t c = (c0 == c1) ? c0 : melange(c0, c1, (uint8_t)(255 * (j - y) / (h > 1 ? h - 1 : 1)));
        bool coin_v = (j < y + r) || (j >= y + h - r);
        for (int i = x0; i < x1; i++) {
            bool coin = coin_v && ((i < x + r) || (i >= x + l - r));
            float cov;
            if (!coin && ep <= 0) {
                cov = 1;
            } else {
                float d = sdf_rect(i + 0.5f, j + 0.5f, mx, my, hx, hy, (float)r);
                cov = sat(0.5f - d);
                if (ep > 0) {
                    float in = sat(d + ep + 0.5f);
                    if (in < cov) cov = in;
                }
            }
            if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
        }
    }
}

void rect_arrondi(int x, int y, int l, int h, int r, uint16_t c, uint8_t a)
{
    rect_arrondi_gen(x, y, l, h, r, c, c, a, 0);
}
void rect_arrondi_degrade(int x, int y, int l, int h, int r, uint16_t c0, uint16_t c1, uint8_t a)
{
    rect_arrondi_gen(x, y, l, h, r, c0, c1, a, 0);
}
void contour_arrondi(int x, int y, int l, int h, int r, float ep, uint16_t c, uint8_t a)
{
    rect_arrondi_gen(x, y, l, h, r, c, c, a, ep);
}

void disque(float cx, float cy, float r, uint16_t c, uint8_t a)
{
    if (r <= 0) return;
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    float rin = r - 0.7f, rout = r + 0.7f;
    float rin2 = rin > 0 ? rin * rin : 0, rout2 = rout * rout;
    for (int j = y0; j < y1; j++) {
        float dy = j + 0.5f - cy;
        for (int i = x0; i < x1; i++) {
            float dx = i + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= rout2) continue;
            if (d2 <= rin2) { put(i, j, c, a); continue; }
            float cov = sat(r + 0.5f - sqrtf(d2));
            put(i, j, c, (uint8_t)(a * cov));
        }
    }
}

void anneau(float cx, float cy, float r, float ep, uint16_t c, uint8_t a)
{
    float ro = r + ep * 0.5f + 1;
    int x0 = (int)floorf(cx - ro), x1 = (int)ceilf(cx + ro);
    int y0 = (int)floorf(cy - ro), y1 = (int)ceilf(cy + ro);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    float ri = r - ep * 0.5f - 1;
    float ri2 = ri > 0 ? ri * ri : 0, ro2 = ro * ro;
    for (int j = y0; j < y1; j++) {
        float dy = j + 0.5f - cy;
        for (int i = x0; i < x1; i++) {
            float dx = i + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= ro2 || d2 <= ri2) continue;
            float cov = sat(ep * 0.5f + 0.5f - fabsf(sqrtf(d2) - r));
            if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
        }
    }
}

void arc(float cx, float cy, float r, float ep, float ang0, float ang1, uint16_t c, uint8_t a)
{
    const float DPI = 6.2831853f;
    float span = ang1 - ang0;
    if (span <= 0) return;
    if (span >= DPI) { anneau(cx, cy, r, ep, c, a); return; }
    float ro = r + ep * 0.5f + 1;
    float ri = r - ep * 0.5f - 1;
    if (ri < 0) ri = 0;
    // boite englobante de l'arc seul (extremites + angles droits traverses)
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    auto inclure = [&](float an) {
        float c = cosf(an), s = sinf(an);
        const float rayons[2] = { ri, ro };
        for (float rr : rayons) {
            float x = cx + c * rr, y = cy + s * rr;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    };
    float ext0 = ang0 - 0.1f, ext1 = ang1 + 0.1f;       // marge pour le lissage
    inclure(ext0);
    inclure(ext1);
    for (int q = (int)ceilf(ext0 / (DPI / 4)); q * (DPI / 4) < ext1; q++) inclure(q * (DPI / 4));
    int x0 = (int)floorf(bx0) - 1, x1 = (int)ceilf(bx1) + 1;
    int y0 = (int)floorf(by0) - 1, y1 = (int)ceilf(by1) + 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    float ri2 = ri * ri, ro2 = ro * ro;
    for (int j = y0; j < y1; j++) {
        float dy = j + 0.5f - cy;
        for (int i = x0; i < x1; i++) {
            float dx = i + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= ro2 || d2 <= ri2) continue;
            float d = sqrtf(d2);
            float cov = sat(ep * 0.5f + 0.5f - fabsf(d - r));
            if (cov <= 0) continue;
            float t = atan2f(dy, dx) - ang0;
            t = fmodf(t + 4 * DPI, DPI);
            float m;
            if (t <= span) m = fminf(t, span - t);
            else           m = -fminf(t - span, DPI - t);
            float ca = sat(0.5f + m * d);
            cov *= ca;
            if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
        }
    }
}

// Distance d'un point au segment [A,B]
static inline float dist_seg(float px, float py, float ax, float ay, float bx, float by,
                             float vx, float vy, float inv_l2)
{
    float t = ((px - ax) * vx + (py - ay) * vy) * inv_l2;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float qx = ax + vx * t - px, qy = ay + vy * t - py;
    return sqrtf(qx * qx + qy * qy);
}

void trait(float x0, float y0, float x1, float y1, float ep, uint16_t c, uint8_t a)
{
    float vx = x1 - x0, vy = y1 - y0;
    float l2 = vx * vx + vy * vy;
    float hw = ep * 0.5f;
    if (l2 < 1e-6f) { disque(x0, y0, hw, c, a); return; }
    float inv = 1.0f / l2;
    int marge = (int)ceilf(hw + 1.5f);
    if (fabsf(vx) >= fabsf(vy)) {
        int xa = (int)floorf(fminf(x0, x1)) - marge, xb = (int)ceilf(fmaxf(x0, x1)) + marge;
        if (xa < cx0) xa = cx0;
        if (xb > cx1) xb = cx1;
        float pente = vy / vx;
        for (int i = xa; i < xb; i++) {
            float px = i + 0.5f;
            float tx = px < fminf(x0, x1) ? fminf(x0, x1) : (px > fmaxf(x0, x1) ? fmaxf(x0, x1) : px);
            float yc = y0 + (tx - x0) * pente;
            int ya = (int)floorf(yc) - marge, yb = (int)ceilf(yc) + marge + 1;
            if (ya < cy0) ya = cy0;
            if (yb > cy1) yb = cy1;
            for (int j = ya; j < yb; j++) {
                float cov = sat(hw + 0.5f - dist_seg(px, j + 0.5f, x0, y0, x1, y1, vx, vy, inv));
                if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
            }
        }
    } else {
        int ya = (int)floorf(fminf(y0, y1)) - marge, yb = (int)ceilf(fmaxf(y0, y1)) + marge;
        if (ya < cy0) ya = cy0;
        if (yb > cy1) yb = cy1;
        float pente = vx / vy;
        for (int j = ya; j < yb; j++) {
            float py = j + 0.5f;
            float ty = py < fminf(y0, y1) ? fminf(y0, y1) : (py > fmaxf(y0, y1) ? fmaxf(y0, y1) : py);
            float xc = x0 + (ty - y0) * pente;
            int xa = (int)floorf(xc) - marge, xb = (int)ceilf(xc) + marge + 1;
            if (xa < cx0) xa = cx0;
            if (xb > cx1) xb = cx1;
            for (int i = xa; i < xb; i++) {
                float cov = sat(hw + 0.5f - dist_seg(i + 0.5f, py, x0, y0, x1, y1, vx, vy, inv));
                if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
            }
        }
    }
}

void trait_pointille(float x0, float y0, float x1, float y1, float ep, float tiret, float trou,
                     uint16_t c, uint8_t a)
{
    float dx = x1 - x0, dy = y1 - y0;
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 0.5f) return;
    dx /= l; dy /= l;
    for (float s = 0; s < l; s += tiret + trou) {
        float e = fminf(s + tiret, l);
        trait(x0 + dx * s, y0 + dy * s, x0 + dx * e, y0 + dy * e, ep, c, a);
    }
}

void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c, uint8_t a)
{
    // orientation positive
    float aire = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (fabsf(aire) < 1e-3f) return;
    if (aire < 0) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    float ex[3] = { x0, x1, x2 }, ey[3] = { y0, y1, y2 };
    float nx[3], ny[3], nc[3];
    for (int k = 0; k < 3; k++) {
        int m = (k + 1) % 3;
        float vx = ex[m] - ex[k], vy = ey[m] - ey[k];
        float l = sqrtf(vx * vx + vy * vy);
        nx[k] = -vy / l; ny[k] = vx / l;               // normale vers l'interieur
        nc[k] = -(nx[k] * ex[k] + ny[k] * ey[k]);
    }
    int xa = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, xb = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int ya = (int)floorf(fminf(y0, fminf(y1, y2))) - 1, yb = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    if (xa < cx0) xa = cx0;
    if (ya < cy0) ya = cy0;
    if (xb > cx1) xb = cx1;
    if (yb > cy1) yb = cy1;
    for (int j = ya; j < yb; j++)
        for (int i = xa; i < xb; i++) {
            float px = i + 0.5f, py = j + 0.5f;
            float m = 1e9f;
            for (int k = 0; k < 3; k++) {
                float d = nx[k] * px + ny[k] * py + nc[k];
                if (d < m) m = d;
            }
            float cov = sat(m + 0.5f);
            if (cov > 0) put(i, j, c, (uint8_t)(a * cov));
        }
}

void halo(float cx, float cy, float r, uint16_t c, uint8_t intensite)
{
    if (r < 1) return;
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    float inv = 1.0f / (r * r);
    for (int j = y0; j < y1; j++) {
        float dy = j + 0.5f - cy;
        for (int i = x0; i < x1; i++) {
            float dx = i + 0.5f - cx;
            float f = 1.0f - (dx * dx + dy * dy) * inv;
            if (f <= 0) continue;
            uint32_t al = (uint32_t)(intensite * f * f);
            if (al) ajout(i, j, c, al);
        }
    }
}

// ----------------------------- Texte ---------------------------------------
static const GlypheAA *trouver(const PoliceAA &p, uint16_t code)
{
    int a = 0, b = p.nb - 1;
    while (a <= b) {
        int m = (a + b) >> 1;
        uint16_t c = p.glyphes[m].code;
        if (c == code) return &p.glyphes[m];
        if (c < code) a = m + 1; else b = m - 1;
    }
    return nullptr;
}

static uint16_t utf8(const char *&s)
{
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *s) return (uint16_t)(((c & 0x1F) << 6) | ((uint8_t)*s++ & 0x3F));
    if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
        uint16_t v = (uint16_t)(((c & 0x0F) << 12) | (((uint8_t)s[0] & 0x3F) << 6) | ((uint8_t)s[1] & 0x3F));
        s += 2;
        return v;
    }
    return '?';
}

int largeur_texte(const char *s, const PoliceAA &p)
{
    int l = 0;
    while (*s) {
        const GlypheAA *g = trouver(p, utf8(s));
        if (!g) g = trouver(p, '?');
        if (g) l += g->avance;
    }
    return l;
}

int hauteur(const PoliceAA &p) { return p.interligne; }

int texte(int x, int y, const char *s, const PoliceAA &p, uint16_t c, uint8_t a, int align)
{
    int l = largeur_texte(s, p);
    if (align == CENTRE) x -= l / 2;
    else if (align == DROITE) x -= l;
    int base = y + p.ascendante;
    while (*s) {
        const GlypheAA *g = trouver(p, utf8(s));
        if (!g) g = trouver(p, '?');
        if (!g) continue;
        int gx = x + g->dx, gy = base + g->dy;
        const uint8_t *bits = p.bits + g->pos;
        for (int j = 0; j < g->h; j++) {
            int yy = gy + j;
            if (yy < cy0 || yy >= cy1) continue;
            for (int i = 0; i < g->l; i++) {
                int xx = gx + i;
                if (xx < cx0 || xx >= cx1) continue;
                int k = j * g->l + i;
                uint8_t v = bits[k >> 1];
                v = (k & 1) ? (v & 0x0F) : (v >> 4);
                if (v) put(xx, yy, c, (uint8_t)((v * 17 * a) / 255));
            }
        }
        x += g->avance;
    }
    return l;
}

void copier(const uint16_t *source) { memcpy(fb, source, L * H * sizeof(uint16_t)); }

}  // namespace gfx
