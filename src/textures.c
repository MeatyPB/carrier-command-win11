// Procedurally generated surface textures. All tiles wrap seamlessly and hold a brightness value
// where 128 leaves the underlying game colour unchanged.
#include "machine.h"
#include <math.h>
#include <stdlib.h>

#define TILE 256
#define PI 3.14159265f

static uint8_t mips[TEX_COUNT][TEX_LEVELS][TILE * TILE];
static float lattice[256][256];

// Value noise on a lattice of `period` cells, wrapping at the tile edge.
static float noise(float x, float y, int period) {
    float fx = x * (float)period / TILE, fy = y * (float)period / TILE;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - (float)ix, ty = fy - (float)iy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    int x0 = ((ix % period) + period) % period, x1 = (x0 + 1) % period;
    int y0 = ((iy % period) + period) % period, y1 = (y0 + 1) % period;
    float a = lattice[y0][x0], b = lattice[y0][x1], c = lattice[y1][x0], d = lattice[y1][x1];
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// Fractal sum normalised to 0..1, with separate lattice periods on each axis.
static float fbm(float x, float y, int px, int py, int octaves) {
    float sum = 0, amp = 1, total = 0;
    for (int o = 0; o < octaves; o++) {
        // Anisotropic noise: stretch one axis by sampling it at a scaled position.
        int p = px > py ? px : py;
        float sx = x * (float)px / (float)p, sy = y * (float)py / (float)p;
        sum += noise(sx, sy, p) * amp;
        total += amp;
        amp *= 0.5f;
        px *= 2;
        py *= 2;
        if (px > 256 || py > 256) break;
    }
    return sum / total;
}

static inline float rnd(void) { return (float)rand() / (float)RAND_MAX; }

static float stretch(float v, float lo, float hi) {
    v = (v - lo) / (hi - lo);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float tile_value(int t, int x, int y) {
    float fx = (float)x, fy = (float)y;
    switch (t) {
    case TEX_TERRAIN: {
        // Broad patches: the colour ramp turns low values into forest and high ones into dry ground.
        float patches = fbm(fx, fy, 3, 3, 6), hills = fbm(fx + 91, fy + 37, 6, 6, 4);
        return 128 + (patches - 0.5f) * 210 + (hills - 0.5f) * 70;
    }
    case TEX_TERRAIN_FINE: {
        float clumps = fbm(fx, fy, 24, 24, 4);
        return 128 + (clumps - 0.5f) * 90 + (rnd() - 0.5f) * 18;
    }
    case TEX_ROCK: {
        float warp = fbm(fx, fy, 4, 4, 4);
        float strata = sinf((fy / TILE * 10 + warp * 4) * 2 * PI);
        float crack = 1 - fabsf(fbm(fx, fy, 16, 16, 4) * 2 - 1);
        float grain = fbm(fx, fy, 16, 32, 4);
        return 128 + strata * 14 + (grain - 0.5f) * 60 - crack * crack * crack * 26 + (rnd() - 0.5f) * 6;
    }
    case TEX_SAND: {
        float warp = fbm(fx, fy, 4, 4, 3);
        float ripple = sinf((fy / TILE * 24 + warp * 6) * 2 * PI);
        return 128 + ripple * 14 + (fbm(fx, fy, 8, 8, 4) - 0.5f) * 70 + (rnd() - 0.5f) * 16;
    }
    case TEX_CONCRETE: {
        float v = 128 + (fbm(fx, fy, 6, 6, 5) - 0.5f) * 56 + (rnd() - 0.5f) * 10;
        // Poured slabs with dark expansion joints.
        if (x % 64 < 2 || y % 64 < 2) v -= 46;
        return v;
    }
    case TEX_DECK: {
        // One tile spans the whole flight deck, so these are its actual markings. The colour ramp
        // reads values from about 205 upwards as paint: yellow, then white.
        float v = 112 + (fbm(fx, fy, 4, 4, 5) - 0.5f) * 44;
        if (x % 32 < 1 || y % 32 < 1) v -= 16; // plate seams
        int edge = x < 256 - x ? x : 256 - x;
        if (y < edge) edge = y;
        if (256 - y < edge) edge = 256 - y;
        if (edge >= 7 && edge < 11) v = 225; // safety border
        float dx = fx - 128, dy = fy - 128, r = sqrtf(dx * dx + dy * dy);
        if (r > 50 && r < 55) v = 255; // landing ring
        if (fabsf(dy) < 2.5f && (x < 62 || x > 194) && edge >= 14 && ((x / 12) & 1)) v = 255; // centreline
        return v;
    }
    case TEX_NONSKID: {
        float v = 128 + (rnd() - 0.5f) * 44 + (fbm(fx, fy, 16, 16, 3) - 0.5f) * 30;
        // Tie-down points.
        int dx = (x % 64) - 32, dy = (y % 64) - 32;
        if (dx * dx + dy * dy < 16) v -= 42;
        return v;
    }
    case TEX_HULL: {
        // Weld lines between hull plates, with rust and grime streaking down from them.
        float v = 128 + (fbm(fx, fy, 16, 2, 3) - 0.5f) * 46 + (rnd() - 0.5f) * 6;
        if (y % 64 < 2) v -= 40;
        else if (y % 64 < 4) v += 16;
        if (x % 85 < 2) v -= 18;
        return v;
    }
    case TEX_METAL: {
        float v = 128 + (fbm(fx, fy, 8, 8, 4) - 0.5f) * 34 + (rnd() - 0.5f) * 5;
        int gx = x % 128, gy = y % 128;
        // Plate seams: a dark groove with a lit lip.
        if (gx < 2 || gy < 2) v -= 62;
        else if (gx < 4 || gy < 4) v += 26;
        // Rivets inset from each plate corner.
        int rx = gx < 64 ? gx - 11 : gx - 117, ry = gy < 64 ? gy - 11 : gy - 117;
        if (rx * rx + ry * ry < 7) v += 34;
        return v;
    }
    case TEX_PAINT:
        return 128 + (fbm(fx, fy, 8, 8, 4) - 0.5f) * 40;
    case TEX_PANEL: {
        // Brushed metal: grain runs along the x axis.
        float brush = fbm(fx, fy, 2, 64, 3), fine = fbm(fx, fy, 8, 128, 2);
        return 128 + (brush - 0.5f) * 40 + (fine - 0.5f) * 26;
    }
    case TEX_WAVES: {
        float swell = fbm(fx, fy, 8, 8, 5);
        float ridge = 1 - fabsf(fbm(fx, fy, 16, 16, 3) * 2 - 1);
        return 255 * stretch(swell * 0.55f + ridge * ridge * 0.45f, 0.28f, 0.72f);
    }
    case TEX_CLOUDS:
        return 255 * stretch(fbm(fx, fy, 4, 8, 6), 0.46f, 0.70f);
    }
    return 128;
}

void tex_init(void) {
    srand(1988);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) lattice[y][x] = rnd();
    for (int t = 0; t < TEX_COUNT; t++) {
        uint8_t *m = mips[t][0];
        for (int y = 0; y < TILE; y++) {
            for (int x = 0; x < TILE; x++) {
                float v = tile_value(t, x, y);
                m[y * TILE + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        for (int l = 1; l < TEX_LEVELS; l++) {
            int sz = TILE >> l;
            const uint8_t *src = mips[t][l - 1];
            uint8_t *dst = mips[t][l];
            for (int y = 0; y < sz; y++) {
                for (int x = 0; x < sz; x++) {
                    int xa = x * 2, xb = (x * 2 + 1) % (sz * 2), ya = y * 2, yb = (y * 2 + 1) % (sz * 2);
                    int w = sz * 2;
                    dst[y * sz + x] = (uint8_t)((src[ya * w + xa] + src[ya * w + xb] + src[yb * w + xa] + src[yb * w + xb]) / 4);
                }
            }
        }
    }
}

// Bilinear sample; u and v are in level-0 texels, 16.16 fixed point.
uint8_t tex_sample(int tile, int level, int32_t u, int32_t v) {
    int sz = TILE >> level, mask = sz - 1;
    const uint8_t *m = mips[tile][level];
    u >>= level;
    v >>= level;
    int x0 = (u >> 16) & mask, y0 = (v >> 16) & mask;
    int x1 = (x0 + 1) & mask, y1 = (y0 + 1) & mask;
    int fx = (u >> 8) & 255, fy = (v >> 8) & 255;
    int a = m[y0 * sz + x0], b = m[y0 * sz + x1], c = m[y1 * sz + x0], d = m[y1 * sz + x1];
    int top = a + (((b - a) * fx) >> 8), bot = c + (((d - c) * fx) >> 8);
    return (uint8_t)(top + (((bot - top) * fy) >> 8));
}

int tex_level_for(float texels_per_pixel) {
    int l = 0;
    while (texels_per_pixel > 1.5f && l < TEX_LEVELS - 1) {
        texels_per_pixel *= 0.5f;
        l++;
    }
    return l;
}
