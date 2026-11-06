// High-resolution renderer.
//
// The game keeps drawing into its emulated 320x200 planar VRAM, which stays the source of truth
// for anything it reads back. Alongside it we keep a scaled shadow of all 64K of VRAM. Polygons,
// lines, circles and text are intercepted at the game's driver entry points and drawn natively;
// every other VRAM write is mirrored as a scaled block, and latched VRAM-to-VRAM copies copy the
// shadow, so page flips, window copies and cursor save/restore keep full detail.
//
// Shadow pixels are a colour pair (a<<4 | b): the two entries of a dither pattern. They are
// blended when presented, which turns the game's dithered colours into solid shades.
//
// Two more planes drive the modern look. mats holds each pixel's material; shades holds its
// texture brightness (world, UI) or its distance from the horizon (sky, sea).
#include "machine.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Offsets in the game's image (relative to the load segment).
#define SEG_DRIVER 0x0FA8
#define SEG_DATA 0x19A0
#define SEG_FONT 0x462D
#define OFF_POLYGON 0x4A3D
#define OFF_CIRCLE 0x506A
#define OFF_LINE 0x1C24
#define OFF_CHAR8 0x29C1
#define OFF_CHAR5 0x2A43
#define OFF_CURSOR 0x25F0
#define OFF_HORIZON 0x2264
#define OFF_MESH_FACE 0x4939 // ES:SI -> face record: vertex count, vertex indices, colour
#define OFF_MESH_FACE_CLIPPED 0x1862 // in the main code segment: the same record, for faces that will be clipped
#define OFF_MESH_FACE_NEAR 0x162B // likewise, for objects close enough to need near-plane clipping
// Where each 3D model's faces sit in the model data segment.
#define SEG_MESHES 0x2BC1
#define MESH_CARRIER_NEAR 0x0200
#define MESH_CARRIER_NEAR_END 0x0560
#define MESH_CARRIER_FAR 0x2800
#define MESH_CARRIER_FAR_END 0x28D0
#define MESH_MANTA 0x1300
#define MESH_MANTA_END 0x1500
#define DS_POLY_COUNT 0x821E
#define DS_POLY_VERTS 0x7B94
#define DS_COLOUR 0x0DDF
#define DS_DITHER_TABLE 0x0DBA
#define DS_BACK_SEG 0x0DD6
#define DS_CLIP_PTR 0x0ED4
#define DS_CHAR_FG 0x1076
#define DS_CHAR_BG 0x1078
#define DS_SMALL_COLOUR 0x107A

int hires_scale = 6;
int hires_keep_dither = 0;
int hires_modern = 1;
enum { MAT_UI, MAT_SKY, MAT_SEA, MAT_WORLD, MAT_RAMP };
// Materials from MAT_RAMP on replace the game's colour with a ramp indexed by texture brightness.
enum { MAT_DECK = MAT_RAMP, MAT_DECK_STEEL, MAT_HULL, MAT_TERRAIN, MAT_ROCK, MAT_SAND, MAT_CONCRETE, MAT_END };
enum { COLOUR_SKY = 2, COLOUR_SEA = 10, COLOUR_BRONZE = 3, COLOUR_STEEL = 7 };
#define SHADE_NEUTRAL 128
#define HORIZON_UNITS_PER_PIXEL 2.2f // sky/sea distance stored per low-res pixel from the horizon
static int S, W, H;
static uint8_t *pages[8], *mats[8], *shades[8];
static int suppress;
static uint16_t cur_face_es, cur_face_si; // face record of the polygon being drawn
static uint32_t data_lin, font_lin;
static uint32_t lin_polygon, lin_circle, lin_line, lin_char8, lin_char5, lin_cursor, lin_horizon, lin_mesh, lin_mesh_clipped, lin_mesh_near;

// Per-pixel bitmaps over each 8K page. lowsrc: the shadow holds a scaled block taken from the
// planar image, rather than native drawing. dirty: that block should be re-smoothed.
static uint8_t lowsrc[8][0x2000], dirty[8][0x2000];

// Tilted horizon, drawn when the game's routine returns so its two colours can be sampled.
static struct { int active, flat; float ax, ay, bx, by; } horizon;

// Up to two pending "run until this return" watches: [0] primitive, [1] cursor.
static struct { uint32_t lin; uint16_t sp; uint8_t saved_hook; int saved_suppress; } watch[2];

static inline uint8_t ds8(uint32_t off) { return mem[data_lin + off]; }
static inline uint16_t ds16(uint32_t off) { return (uint16_t)(mem[data_lin + off] | mem[data_lin + off + 1] << 8); }
static inline int16_t ds16s(uint32_t off) { return (int16_t)ds16(off); }

static void arm_return(int slot, int far_call) {
    uint32_t sp = (uint32_t)(cpu.seg[SS]) << 4;
    uint16_t top = cpu.r.w[SP];
    uint16_t ip = (uint16_t)(mem[(sp + top) & 0xFFFFF] | mem[(sp + (uint16_t)(top + 1)) & 0xFFFFF] << 8);
    uint16_t cs = cpu.seg[CS];
    if (far_call) cs = (uint16_t)(mem[(sp + (uint16_t)(top + 2)) & 0xFFFFF] | mem[(sp + (uint16_t)(top + 3)) & 0xFFFFF] << 8);
    uint32_t lin = (((uint32_t)cs << 4) + ip) & 0xFFFFF;
    watch[slot].lin = lin;
    watch[slot].sp = (uint16_t)(top + (far_call ? 4 : 2));
    watch[slot].saved_hook = hookmap[lin];
    hookmap[lin] = 2;
}

static void add_hook(uint32_t *out, uint32_t seg_off) {
    *out = ((uint32_t)(machine_load_seg + SEG_DRIVER) << 4) + seg_off;
    hookmap[*out] = 2;
}

void hires_init(int scale) {
    S = hires_scale = scale;
    W = 320 * S;
    H = 205 * S; // 8K page = 204.8 lines of 40 bytes
    for (int i = 0; i < 8; i++) {
        pages[i] = (uint8_t *)calloc((size_t)W * H, 1);
        mats[i] = (uint8_t *)calloc((size_t)W * H, 1);
        shades[i] = (uint8_t *)malloc((size_t)W * H);
        memset(shades[i], SHADE_NEUTRAL, (size_t)W * H);
    }
    tex_init();
}

void hires_install_hooks(void) {
    data_lin = (uint32_t)(machine_load_seg + SEG_DATA) << 4;
    font_lin = (uint32_t)(machine_load_seg + SEG_FONT) << 4;
    add_hook(&lin_polygon, OFF_POLYGON);
    add_hook(&lin_circle, OFF_CIRCLE);
    add_hook(&lin_line, OFF_LINE);
    add_hook(&lin_char8, OFF_CHAR8);
    add_hook(&lin_char5, OFF_CHAR5);
    add_hook(&lin_cursor, OFF_CURSOR);
    add_hook(&lin_horizon, OFF_HORIZON);
    add_hook(&lin_mesh, OFF_MESH_FACE);
    lin_mesh_clipped = ((uint32_t)machine_load_seg << 4) + OFF_MESH_FACE_CLIPPED;
    hookmap[lin_mesh_clipped] = 2;
    lin_mesh_near = ((uint32_t)machine_load_seg << 4) + OFF_MESH_FACE_NEAR;
    hookmap[lin_mesh_near] = 2;
}

void hires_mode_set(void) {
    if (!pages[0]) return;
    for (int i = 0; i < 8; i++) {
        memset(pages[i], 0, (size_t)W * H);
        memset(mats[i], MAT_UI, (size_t)W * H);
        memset(shades[i], SHADE_NEUTRAL, (size_t)W * H);
    }
    memset(lowsrc, 0, sizeof lowsrc);
    memset(dirty, 0, sizeof dirty);
}

static inline int back_page(void) {
    uint16_t seg = ds16(DS_BACK_SEG);
    return (int)((((uint32_t)(seg - 0xA000) << 4) >> 13) & 7);
}

// ---- Mirroring of ordinary VRAM traffic ---------------------------------------
static inline void mark_dirty(int pg, int pos, uint8_t bits) {
    if (pos >= 0 && pos < 0x2000) dirty[pg][pos] |= bits;
}

void hires_note_cpu_write(uint32_t off, uint8_t mask) {
    int pn = (int)(off >> 13);
    uint32_t pos = off & 0x1FFF;
    // The smoothing and emboss of a pixel depend on its four neighbours, whoever draws them.
    mark_dirty(pn, (int)pos, (uint8_t)(mask | mask << 1 | mask >> 1));
    mark_dirty(pn, (int)pos - 40, mask);
    mark_dirty(pn, (int)pos + 40, mask);
    if (mask & 0x80) mark_dirty(pn, (int)pos - 1, 0x01);
    if (mask & 0x01) mark_dirty(pn, (int)pos + 1, 0x80);
    if (suppress) {
        lowsrc[pn][pos] &= (uint8_t)~mask; // a native primitive owns these pixels
        return;
    }
    lowsrc[pn][pos] |= mask;
    int line = (int)(pos / 40), xb = (int)(pos % 40);
    uint8_t p0 = vga_planes[0][off], p1 = vga_planes[1][off], p2 = vga_planes[2][off], p3 = vga_planes[3][off];
    for (int bit = 0; bit < 8; bit++) {
        if (!(mask & (0x80 >> bit))) continue;
        int b = 7 - bit;
        int c = ((p0 >> b) & 1) | ((p1 >> b) & 1) << 1 | ((p2 >> b) & 1) << 2 | ((p3 >> b) & 1) << 3;
        uint8_t v = (uint8_t)(c << 4 | c);
        size_t at = (size_t)line * S * W + (size_t)(xb * 8 + bit) * S;
        for (int y = 0; y < S; y++, at += W) {
            memset(pages[pn] + at, v, (size_t)S);
            memset(mats[pn] + at, MAT_UI, (size_t)S);
            memset(shades[pn] + at, SHADE_NEUTRAL, (size_t)S);
        }
    }
}

void hires_note_latch_copy(uint32_t dst, uint32_t src, uint8_t mask) {
    (void)mask;
    uint32_t dpos = dst & 0x1FFF, spos = src & 0x1FFF;
    int dpage = (int)(dst >> 13);
    mark_dirty(dpage, (int)dpos, 0xFF);
    mark_dirty(dpage, (int)dpos - 40, 0xFF);
    mark_dirty(dpage, (int)dpos + 40, 0xFF);
    mark_dirty(dpage, (int)dpos - 1, 0x01);
    mark_dirty(dpage, (int)dpos + 1, 0x80);
    if (suppress) {
        lowsrc[dst >> 13][dpos] = 0;
        return;
    }
    if (dst == src) return;
    lowsrc[dst >> 13][dpos] = lowsrc[src >> 13][spos];
    size_t d = (size_t)(dpos / 40) * S * W + (size_t)(dpos % 40) * 8 * S;
    size_t s = (size_t)(spos / 40) * S * W + (size_t)(spos % 40) * 8 * S;
    int dn = (int)(dst >> 13), sn = (int)(src >> 13);
    for (int y = 0; y < S; y++, d += W, s += W) {
        memcpy(pages[dn] + d, pages[sn] + s, (size_t)8 * S);
        memcpy(mats[dn] + d, mats[sn] + s, (size_t)8 * S);
        memcpy(shades[dn] + d, shades[sn] + s, (size_t)8 * S);
    }
}

// ---- Native primitives ---------------------------------------------------------
static inline uint8_t dither_colour(int colour) {
    uint32_t table = ds16(DS_DITHER_TABLE);
    uint8_t a = ds8(table + (uint32_t)colour * 2), b = ds8(table + (uint32_t)colour * 2 + 1);
    return (uint8_t)((a & 15) << 4 | (b & 15));
}

static void clip_rect(int *x0, int *y0, int *x1, int *y1) {
    // The window record is addressed through BP, so it lives in the stack segment.
    uint32_t ss = (uint32_t)cpu.seg[SS] << 4, p = ds16(DS_CLIP_PTR);
#define SS16(o) ((int16_t)(mem[(ss + (uint16_t)(p + (o))) & 0xFFFFF] | mem[(ss + (uint16_t)(p + (o) + 1)) & 0xFFFFF] << 8))
    *x0 = SS16(0) * S;
    *y0 = SS16(2) * S;
    *x1 = (SS16(4) + 1) * S - 1;
    *y1 = (SS16(6) + 1) * S - 1;
#undef SS16
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= W) *x1 = W - 1;
    if (*y1 >= 200 * S) *y1 = 200 * S - 1;
}

static void span(int pn, int y, int xa, int xb, uint8_t v) {
    if (xb < xa) return;
    size_t at = (size_t)y * W + xa, n = (size_t)(xb - xa + 1);
    memset(pages[pn] + at, v, n);
    memset(mats[pn] + at, MAT_UI, n);
    memset(shades[pn] + at, SHADE_NEUTRAL, n);
}

// What a face is made of: the shading material, its texture, an optional fine detail texture
// layered on top, how many times the texture repeats across the face, and how many times more the
// detail repeats. Faces are identified by the 3D object they belong to, then by game colour.
typedef struct { int mat, tile, detail; float tiles, detail_mul; } Surface;

static inline int face_in(uint16_t lo, uint16_t hi) {
    return cur_face_es == (uint16_t)(machine_load_seg + SEG_MESHES) && cur_face_si >= lo && cur_face_si < hi;
}

// Picks a surface texture from what a colour pair actually looks like, so it follows the palette.
static int tile_for_pair(uint8_t v) {
    const uint32_t *pal = vga_palette_argb();
    uint32_t ca = pal[v >> 4], cb = pal[v & 15];
    int r = (int)(((ca >> 16) & 255) + ((cb >> 16) & 255)) / 2;
    int g = (int)(((ca >> 8) & 255) + ((cb >> 8) & 255)) / 2;
    int b = (int)((ca & 255) + (cb & 255)) / 2;
    if (r < 24 && g < 24 && b < 24) return -1;
    if (abs(r - g) < 20 && abs(g - b) < 20) return TEX_METAL;
    return TEX_PAINT;
}

static Surface surface_for(int colour, uint8_t pair) {
    Surface s = {MAT_WORLD, tile_for_pair(pair), -1, 3.0f, 1.0f};
    if (s.tile < 0) return s;
    if (face_in(MESH_CARRIER_NEAR, MESH_CARRIER_NEAR_END) || face_in(MESH_CARRIER_FAR, MESH_CARRIER_FAR_END)) {
        switch (colour) {
        case 9: return (Surface){MAT_DECK, TEX_DECK, TEX_NONSKID, 1.0f, 10.0f};
        case 3: case 42: return (Surface){MAT_DECK_STEEL, TEX_METAL, TEX_NONSKID, 3.0f, 5.0f};
        case 6: case 7: case 12: case 24: case 36: case 38: case 60: case 61:
            return (Surface){MAT_HULL, TEX_HULL, -1, 3.0f, 1.0f};
        }
        return s;
    }
    if (face_in(MESH_MANTA, MESH_MANTA_END)) return s;
    switch (colour) {
    case 4: case 20: case 54: case 56: case 61:
        return (Surface){MAT_TERRAIN, TEX_TERRAIN, TEX_TERRAIN_FINE, 3.0f, 12.0f};
    case 3: case 19:
        return (Surface){MAT_ROCK, TEX_ROCK, -1, 1.5f, 1.0f};
    case 5:
        return (Surface){MAT_SAND, TEX_SAND, -1, 4.0f, 1.0f};
    case 6: case 12:
        return (Surface){MAT_CONCRETE, TEX_CONCRETE, TEX_TERRAIN_FINE, 2.0f, 6.0f};
    }
    return s;
}

// Texture coordinates come from the polygon's own vertices, so the pattern is carried along with
// the face as it moves. Four-sided faces get a true perspective mapping from their corners; others
// use the first and last edges. Tiny or degenerate faces simply stay flat.
#define TILE_TEXELS 256.0f
typedef struct {
    int tile, level, detail, detail_level, mat, projective;
    float detail_mul;
    int32_t u, v, du_dx, dv_dx, du_dy, dv_dy;
    double pu[3], pv[3], pw[3], tiles; // u, v and w as linear functions of shadow x, y
} FaceMap;

// Builds the mapping from shadow pixels to the unit square for a convex quad.
static int quad_map(const float *vx, const float *vy, FaceMap *m) {
    double x[4], y[4];
    for (int i = 0; i < 4; i++) {
        x[i] = ((double)vx[i] + 0.5) * S;
        y[i] = ((double)vy[i] + 0.5) * S;
    }
    double sx = x[0] - x[1] + x[2] - x[3], sy = y[0] - y[1] + y[2] - y[3];
    double dx1 = x[1] - x[2], dx2 = x[3] - x[2], dy1 = y[1] - y[2], dy2 = y[3] - y[2];
    double den = dx1 * dy2 - dx2 * dy1;
    if (fabs(den) < 1.0) return 0;
    double g = (sx * dy2 - dx2 * sy) / den, h = (dx1 * sy - sx * dy1) / den;
    double a = x[1] - x[0] + g * x[1], b = x[3] - x[0] + h * x[3], c = x[0];
    double d = y[1] - y[0] + g * y[1], e = y[3] - y[0] + h * y[3], f = y[0];
    m->pu[0] = e - f * h; m->pu[1] = c * h - b; m->pu[2] = b * f - c * e;
    m->pv[0] = f * g - d; m->pv[1] = a - c * g; m->pv[2] = c * d - a * f;
    m->pw[0] = d * h - e * g; m->pw[1] = b * g - a * h; m->pw[2] = a * e - b * d;
    // Every corner must lie on the same side of the projective horizon.
    int positive = 0;
    for (int i = 0; i < 4; i++) {
        double w = m->pw[0] * x[i] + m->pw[1] * y[i] + m->pw[2];
        if (fabs(w) < 1e-9) return 0;
        if (w > 0) positive++;
    }
    return positive == 0 || positive == 4;
}

static int face_map(const float *vx, const float *vy, int n, int colour, uint8_t pair, FaceMap *m) {
    Surface s = surface_for(colour, pair);
    m->tile = s.tile;
    m->mat = s.mat;
    m->detail = s.detail;
    m->detail_mul = s.detail_mul;
    m->tiles = s.tiles;
    if (m->tile < 0 || n < 3) return 0;
    float e1x = vx[1] - vx[0], e1y = vy[1] - vy[0];
    float e2x = vx[n - 1] - vx[0], e2y = vy[n - 1] - vy[0];
    float det = e1x * e2y - e1y * e2x;
    if (fabsf(det) < 12.0f) return 0;
    // Only for surfaces that really are rectangles: rock faces taper towards the peak.
    m->projective = n == 4 && s.mat >= MAT_RAMP && s.mat != MAT_ROCK && quad_map(vx, vy, m);
    float k = s.tiles * TILE_TEXELS / det / (float)S;
    float du_dx = e2y * k, du_dy = -e2x * k, dv_dx = -e1y * k, dv_dy = e1x * k;
    float size = fabsf(du_dx);
    if (fabsf(du_dy) > size) size = fabsf(du_dy);
    if (fabsf(dv_dx) > size) size = fabsf(dv_dx);
    if (fabsf(dv_dy) > size) size = fabsf(dv_dy);
    m->level = tex_level_for(size);
    m->detail_level = tex_level_for(size * s.detail_mul);
    m->du_dx = (int32_t)(du_dx * 65536.0f);
    m->du_dy = (int32_t)(du_dy * 65536.0f);
    m->dv_dx = (int32_t)(dv_dx * 65536.0f);
    m->dv_dy = (int32_t)(dv_dy * 65536.0f);
    // Value at shadow pixel (0,0): vertex 0 sits at the centre of its low-res pixel.
    float ox = (vx[0] + 0.5f) * S, oy = (vy[0] + 0.5f) * S;
    m->u = (int32_t)fmodf(-(ox * du_dx + oy * du_dy) * 65536.0f, 16777216.0f * 256.0f);
    m->v = (int32_t)fmodf(-(ox * dv_dx + oy * dv_dy) * 65536.0f, 16777216.0f * 256.0f);
    return 1;
}

static void world_span(int pn, int y, int xa, int xb, uint8_t v, const FaceMap *m) {
    if (xb < xa) return;
    size_t at = (size_t)y * W + xa, n = (size_t)(xb - xa + 1);
    memset(pages[pn] + at, v, n);
    memset(mats[pn] + at, m->mat, n);
    uint8_t *sh = shades[pn] + at;
    if (m->projective) {
        double scale = m->tiles * TILE_TEXELS;
        double fx = (double)xa + 0.5, fy = (double)y + 0.5;
        double nu = m->pu[0] * fx + m->pu[1] * fy + m->pu[2];
        double nv = m->pv[0] * fx + m->pv[1] * fy + m->pv[2];
        double w = m->pw[0] * fx + m->pw[1] * fy + m->pw[2];
        // Filter for the footprint at the middle of the span, across and down the screen.
        double mx = fx + (double)(n / 2);
        double w0 = m->pw[0] * mx + m->pw[1] * fy + m->pw[2];
        double u0 = (m->pu[0] * mx + m->pu[1] * fy + m->pu[2]) / w0, v0 = (m->pv[0] * mx + m->pv[1] * fy + m->pv[2]) / w0;
        double wx = w0 + m->pw[0], wy = w0 + m->pw[1];
        double ux = (m->pu[0] * (mx + 1) + m->pu[1] * fy + m->pu[2]) / wx - u0;
        double vx = (m->pv[0] * (mx + 1) + m->pv[1] * fy + m->pv[2]) / wx - v0;
        double uy = (m->pu[0] * mx + m->pu[1] * (fy + 1) + m->pu[2]) / wy - u0;
        double vy = (m->pv[0] * mx + m->pv[1] * (fy + 1) + m->pv[2]) / wy - v0;
        // The geometric mean of the two directions: filtering for the longer one would smear
        // surfaces seen at a shallow angle sideways.
        double size = sqrt(sqrt(ux * ux + vx * vx) * sqrt(uy * uy + vy * vy)) * scale;
        int level = tex_level_for((float)size), dlevel = tex_level_for((float)(size * m->detail_mul));
        for (size_t i = 0; i < n; i++) {
            double uf = nu / w, vf = nv / w;
            // The fill runs half a pixel past the corners; hold the coordinates at the face's edge.
            uf = uf < 0 ? 0 : uf > 1 ? 1 : uf;
            vf = vf < 0 ? 0 : vf > 1 ? 1 : vf;
            int32_t tu = (int32_t)(int64_t)(uf * scale * 65536.0), tv = (int32_t)(int64_t)(vf * scale * 65536.0);
            int s = tex_sample(m->tile, level, tu, tv);
            if (m->detail >= 0) {
                int32_t du = (int32_t)(int64_t)(uf * scale * m->detail_mul * 65536.0);
                int32_t dv = (int32_t)(int64_t)(vf * scale * m->detail_mul * 65536.0);
                s = s * tex_sample(m->detail, dlevel, du, dv) / SHADE_NEUTRAL;
                if (s > 255) s = 255;
            }
            sh[i] = (uint8_t)s;
            nu += m->pu[0];
            nv += m->pv[0];
            w += m->pw[0];
        }
        return;
    }
    uint32_t u = (uint32_t)m->u + (uint32_t)xa * (uint32_t)m->du_dx + (uint32_t)y * (uint32_t)m->du_dy;
    uint32_t tv = (uint32_t)m->v + (uint32_t)xa * (uint32_t)m->dv_dx + (uint32_t)y * (uint32_t)m->dv_dy;
    int mul = (int)m->detail_mul;
    for (size_t i = 0; i < n; i++) {
        int s = tex_sample(m->tile, m->level, (int32_t)u, (int32_t)tv);
        if (m->detail >= 0) {
            s = s * tex_sample(m->detail, m->detail_level, (int32_t)(u * (uint32_t)mul), (int32_t)(tv * (uint32_t)mul)) / SHADE_NEUTRAL;
            if (s > 255) s = 255;
        }
        sh[i] = (uint8_t)s;
        u += (uint32_t)m->du_dx;
        tv += (uint32_t)m->dv_dx;
    }
}

static void draw_polygon(void) {
    int n = ds16(DS_POLY_COUNT);
    if (n < 2 || n > 64) return;
    float vx[64], vy[64];
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++) {
        vx[i] = (float)ds16s(DS_POLY_VERTS + (uint32_t)i * 4);
        vy[i] = (float)ds16s(DS_POLY_VERTS + (uint32_t)i * 4 + 2);
        if (vy[i] < ymin) ymin = vy[i];
        if (vy[i] > ymax) ymax = vy[i];
    }
    int cx0, cy0, cx1, cy1;
    clip_rect(&cx0, &cy0, &cx1, &cy1);
    int pg = back_page();
    uint8_t v = dither_colour(ds8(DS_COLOUR));
    FaceMap map;
    int textured = face_map(vx, vy, n, ds8(DS_COLOUR), v, &map);
    cur_face_es = 0;
    int ya = (int)floorf(ymin * S), yb = (int)ceilf((ymax + 1) * S) - 1;
    if (ya < cy0) ya = cy0;
    if (yb > cy1) yb = cy1;
    for (int y = ya; y <= yb; y++) {
        // Sample in low-res coordinates. Low-res spans include both end pixels, so the fill
        // extends half a pixel past the vertex centres; this also closes gaps between faces.
        float ly = ((float)y + 0.5f) / (float)S - 0.5f;
        if (ly < ymin) ly = ymin;
        if (ly > ymax) ly = ymax;
        float xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < n; i++) {
            int j = i + 1 == n ? 0 : i + 1;
            float y0 = vy[i], y1 = vy[j];
            if ((ly < y0 && ly < y1) || (ly > y0 && ly > y1)) continue;
            if (y0 == y1) {
                if (vx[i] < xl) xl = vx[i];
                if (vx[i] > xr) xr = vx[i];
                if (vx[j] < xl) xl = vx[j];
                if (vx[j] > xr) xr = vx[j];
            } else {
                float x = vx[i] + (vx[j] - vx[i]) * (ly - y0) / (y1 - y0);
                if (x < xl) xl = x;
                if (x > xr) xr = x;
            }
        }
        if (xl > xr) continue;
        int xa = (int)floorf(xl * S), xb = (int)ceilf((xr + 1) * S) - 1;
        if (xa < cx0) xa = cx0;
        if (xb > cx1) xb = cx1;
        if (textured) world_span(pg, y, xa, xb, v, &map);
        else span(pg, y, xa, xb, v);
    }
}

static void draw_circle(void) {
    int cx = (int16_t)cpu.r.w[AX], cy = (int16_t)cpu.r.w[BX], r = (int16_t)cpu.r.w[CX];
    int x0, y0, x1, y1;
    clip_rect(&x0, &y0, &x1, &y1);
    int pg = back_page();
    uint8_t v = dither_colour(ds8(DS_COLOUR));
    float fcx = ((float)cx + 0.5f) * S, fcy = ((float)cy + 0.5f) * S, fr = ((float)r + 0.5f) * S;
    int ya = (int)floorf(fcy - fr), yb = (int)ceilf(fcy + fr);
    if (ya < y0) ya = y0;
    if (yb > y1) yb = y1;
    for (int y = ya; y <= yb; y++) {
        float dy = (float)y + 0.5f - fcy;
        float d = fr * fr - dy * dy;
        if (d < 0) continue;
        float hw = sqrtf(d);
        int xa = (int)floorf(fcx - hw), xb = (int)ceilf(fcx + hw) - 1;
        if (xa < x0) xa = x0;
        if (xb > x1) xb = x1;
        span(pg, y, xa, xb, v);
    }
}

static void draw_line(void) {
    float ax = (float)(int16_t)cpu.r.w[AX], ay = (float)(int16_t)cpu.r.w[BX];
    float bx = (float)(int16_t)cpu.r.w[CX], by = (float)(int16_t)cpu.r.w[DX];
    int c = ds8(DS_COLOUR) & 15;
    uint8_t v = (uint8_t)(c << 4 | c);
    int pg = back_page();
    int t = S * 2 / 3;
    if (t < 1) t = 1;
    float px = (ax + 0.5f) * S, py = (ay + 0.5f) * S, qx = (bx + 0.5f) * S, qy = (by + 0.5f) * S;
    float dx = qx - px, dy = qy - py;
    int steps = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy));
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int x = (int)(px + dx * (float)i / (float)steps) - t / 2;
        int y = (int)(py + dy * (float)i / (float)steps) - t / 2;
        int xe = x + t - 1, ye = y + t - 1;
        if (x < 0) x = 0;
        if (xe >= W) xe = W - 1;
        if (y < 0) y = 0;
        if (ye >= 200 * S) ye = 200 * S - 1;
        for (int yy = y; yy <= ye; yy++) span(pg, yy, x, xe, v);
    }
}

// Scale2x (EPX) of a 1-bit glyph, applied three times, then sampled to the shadow scale.
#define GLYPH_MAX (8 * 8)
static void glyph_upscale(const uint8_t *src, int w, int h, uint8_t *dst, int *ow, int *oh) {
    static uint8_t a[GLYPH_MAX * GLYPH_MAX], b[GLYPH_MAX * GLYPH_MAX];
    memcpy(a, src, (size_t)w * h);
    for (int pass = 0; pass < 3; pass++) {
        int nw = w * 2, nh = h * 2;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t p = a[y * w + x];
                uint8_t up = y ? a[(y - 1) * w + x] : p, dn = y + 1 < h ? a[(y + 1) * w + x] : p;
                uint8_t lf = x ? a[y * w + x - 1] : p, rt = x + 1 < w ? a[y * w + x + 1] : p;
                uint8_t e0 = p, e1 = p, e2 = p, e3 = p;
                if (up != dn && lf != rt) {
                    if (lf == up) e0 = lf;
                    if (up == rt) e1 = rt;
                    if (lf == dn) e2 = lf;
                    if (dn == rt) e3 = rt;
                }
                b[(y * 2) * nw + x * 2] = e0;
                b[(y * 2) * nw + x * 2 + 1] = e1;
                b[(y * 2 + 1) * nw + x * 2] = e2;
                b[(y * 2 + 1) * nw + x * 2 + 1] = e3;
            }
        }
        w = nw;
        h = nh;
        memcpy(a, b, (size_t)w * h);
    }
    memcpy(dst, a, (size_t)w * h);
    *ow = w;
    *oh = h;
}

static void draw_glyph(const uint8_t *rows, int gw, int gh, int x, int y, uint8_t fg, int has_bg, uint8_t bg) {
    static uint8_t bits[8 * 8], big[GLYPH_MAX * GLYPH_MAX];
    for (int r = 0; r < gh; r++)
        for (int c = 0; c < gw; c++) bits[r * gw + c] = (rows[r] >> (7 - c)) & 1;
    int bw, bh;
    glyph_upscale(bits, gw, gh, big, &bw, &bh);
    int pn = back_page();
    int px = x * S, py = y * S;
    for (int yy = 0; yy < gh * S; yy++) {
        if (py + yy < 0 || py + yy >= H) continue;
        size_t base = (size_t)(py + yy) * W;
        const uint8_t *srow = big + (size_t)(yy * bh / (gh * S)) * bw;
        for (int xx = 0; xx < gw * S; xx++) {
            int tx = px + xx;
            if (tx < 0 || tx >= W) continue;
            int on = srow[xx * bw / (gw * S)];
            if (!on && !has_bg) continue;
            pages[pn][base + tx] = on ? fg : bg;
            mats[pn][base + tx] = MAT_UI;
            shades[pn][base + tx] = SHADE_NEUTRAL;
        }
    }
}

static void draw_char8(void) {
    int ch = cpu.r.b[0];
    int y = (int16_t)cpu.r.w[BX], col = (int16_t)cpu.r.w[CX];
    if (ch < 0x20) return;
    const uint8_t *g = &mem[font_lin + (uint32_t)(ch - 0x20) * 8];
    uint8_t fg = ds8(DS_CHAR_FG) & 15, bg = ds8(DS_CHAR_BG) & 15;
    draw_glyph(g, 8, 8, col * 8, y, (uint8_t)(fg << 4 | fg), 1, (uint8_t)(bg << 4 | bg));
}

static void draw_char5(void) {
    int ch = cpu.r.b[0];
    int x = (int16_t)cpu.r.w[BX], y = (int16_t)cpu.r.w[CX];
    if (ch <= 0x20) return;
    const uint8_t *g = &mem[font_lin + 0x300 + (uint32_t)(ch - 0x21) * 5];
    uint8_t c = ds8(DS_SMALL_COLOUR) & 15;
    draw_glyph(g, 8, 5, x, y, (uint8_t)(c << 4 | c), 0, 0);
}

static inline int low_colour(int pn, int x, int y) {
    if (x < 0) x = 0;
    if (x > 319) x = 319;
    if (y < 0) y = 0;
    if (y > 199) y = 199;
    uint32_t off = ((uint32_t)pn << 13) + (uint32_t)y * 40 + (uint32_t)(x >> 3);
    int b = 7 - (x & 7);
    return ((vga_planes[0][off] >> b) & 1) | ((vga_planes[1][off] >> b) & 1) << 1 |
           ((vga_planes[2][off] >> b) & 1) << 2 | ((vga_planes[3][off] >> b) & 1) << 3;
}

static inline int material_of(int colour) {
    return colour == COLOUR_SKY ? MAT_SKY : colour == COLOUR_SEA ? MAT_SEA : MAT_UI;
}

// Fills part of a row, storing each pixel's distance from the horizon for the sky and sea.
// dist and step are in stored distance units, at xa and per shadow pixel.
static void horizon_span(int pn, int y, int xa, int xb, int colour, float dist, float step) {
    if (xb < xa) return;
    size_t at = (size_t)y * W + xa, n = (size_t)(xb - xa + 1);
    memset(pages[pn] + at, colour << 4 | colour, n);
    int mat = material_of(colour);
    memset(mats[pn] + at, mat, n);
    if (mat == MAT_UI) {
        memset(shades[pn] + at, SHADE_NEUTRAL, n);
        return;
    }
    uint8_t *sh = shades[pn] + at;
    for (size_t i = 0; i < n; i++, dist += step) {
        float d = fabsf(dist);
        sh[i] = (uint8_t)(d > 255.0f ? 255.0f : d);
    }
}

static void draw_horizon(void) {
    int x0, y0, x1, y1;
    clip_rect(&x0, &y0, &x1, &y1);
    int pn = back_page();
    if (horizon.flat) {
        // The whole window is one side of the horizon, which is out of view.
        int colour = low_colour(pn, x0 / S + 1, y0 / S + 1);
        for (int y = y0; y <= y1; y++) horizon_span(pn, y, x0, x1, colour, 255.0f, 0.0f);
        return;
    }
    float ax = (horizon.ax + 0.5f) * S, ay = (horizon.ay + 0.5f) * S;
    float bx = (horizon.bx + 0.5f) * S, by = (horizon.by + 0.5f) * S;
    float dx = bx - ax, dy = by - ay;
    if (dx == 0 && dy == 0) return;
    // Colour on each side of the line: the planar image at the window corner furthest from it.
    int colour[2] = {-1, -1};
    float best[2] = {0, 0};
    const int cx[4] = {x0, x1, x0, x1}, cy[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; i++) {
        float cross = dx * ((float)cy[i] - ay) - dy * ((float)cx[i] - ax);
        int side = cross >= 0;
        if (fabsf(cross) > best[side]) {
            best[side] = fabsf(cross);
            int lx = (cx[i] == x0 ? cx[i] + S : cx[i] - S) / S, ly = (cy[i] == y0 ? cy[i] + S : cy[i] - S) / S;
            colour[side] = low_colour(pn, lx, ly);
        }
    }
    if (colour[0] < 0) colour[0] = colour[1];
    if (colour[1] < 0) colour[1] = colour[0];
    if (colour[0] < 0) return;
    // Dividing the cross product by this gives stored distance units from the line.
    float unit = sqrtf(dx * dx + dy * dy) * (float)S / HORIZON_UNITS_PER_PIXEL;
    float step = -dy / unit;
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y + 0.5f - ay;
        // Sign of the cross product at the row's two ends decides how the row splits.
        float cl = dx * fy - dy * ((float)x0 - ax), cr = dx * fy - dy * ((float)x1 - ax);
        int sl = cl >= 0 ? colour[1] : colour[0], sr = cr >= 0 ? colour[1] : colour[0];
        if (sl == sr) {
            horizon_span(pn, y, x0, x1, sl, cl / unit, step);
        } else {
            int split = (int)(ax + dx * fy / dy);
            if (split < x0) split = x0;
            if (split > x1) split = x1;
            horizon_span(pn, y, x0, split, sl, cl / unit, step);
            horizon_span(pn, y, split + 1, x1, sr, (cl + (float)(split + 1 - x0) * -dy) / unit, step);
        }
    }
}

int hires_hook(uint32_t lin) {
    // Returning from a watched call. Checked first: a return address may also be a hook entry.
    for (int i = 0; i < 2; i++) {
        if (watch[i].lin == lin && watch[i].sp == cpu.r.w[SP]) {
            hookmap[lin] = watch[i].saved_hook;
            watch[i].lin = 0;
            if (i == 0) {
                suppress = 0;
                if (horizon.active) {
                    horizon.active = 0;
                    draw_horizon();
                }
            } else {
                suppress = watch[i].saved_suppress;
            }
        }
    }
    if (lin == lin_cursor) {
        // The cursor is drawn from the timer interrupt or a primitive's tail; it must always
        // reach the shadow, even while the interrupted primitive has mirroring off.
        if (!watch[1].lin) {
            watch[1].saved_suppress = suppress;
            suppress = 0;
            arm_return(1, 1);
        }
        return 1;
    }
    if (suppress) return 1;
    if (lin == lin_mesh || lin == lin_mesh_clipped || lin == lin_mesh_near) { cur_face_es = cpu.seg[ES]; cur_face_si = cpu.r.w[SI]; return 1; }
    if (lin == lin_polygon) { draw_polygon(); suppress = 1; arm_return(0, 0); }
    else if (lin == lin_circle) { draw_circle(); suppress = 1; arm_return(0, 1); }
    else if (lin == lin_line) { draw_line(); suppress = 1; arm_return(0, 1); }
    else if (lin == lin_char8) { draw_char8(); suppress = 1; arm_return(0, 1); }
    else if (lin == lin_char5) { draw_char5(); suppress = 1; arm_return(0, 1); }
    else if (lin == lin_horizon && cpu.r.b[2] <= 2) {
        // AL,AH = line x ends and BL,BH = y ends, relative to the window. CL = 0 selects a tilted
        // fill; 1 and 2 fill the whole window with sky or sea.
        horizon.flat = cpu.r.b[2] != 0;
        uint32_t ss = (uint32_t)cpu.seg[SS] << 4, p = ds16(DS_CLIP_PTR);
        int wx = (int16_t)(mem[(ss + p) & 0xFFFFF] | mem[(ss + p + 1) & 0xFFFFF] << 8);
        int wy = (int16_t)(mem[(ss + p + 2) & 0xFFFFF] | mem[(ss + p + 3) & 0xFFFFF] << 8);
        horizon.ax = (float)(wx + cpu.r.b[0]);
        horizon.bx = (float)(wx + cpu.r.b[1]);
        horizon.ay = (float)(wy + cpu.r.b[6]);
        horizon.by = (float)(wy + cpu.r.b[7]);
        horizon.active = 1;
        suppress = 1;
        arm_return(0, 1);
    }
    return 1;
}

// ---- Bitmap smoothing ----------------------------------------------------------
// Icons, panels and the cursor reach the shadow as square blocks. Two rounds of Scale2x, taken
// over the planar image, round their diagonals off.
static inline void epx(int p, int up, int rt, int lf, int dn, int *e) {
    e[0] = e[1] = e[2] = e[3] = p;
    if (up != dn && lf != rt) {
        if (lf == up) e[0] = lf;
        if (up == rt) e[1] = rt;
        if (lf == dn) e[2] = lf;
        if (dn == rt) e[3] = rt;
    }
}

static void smooth_pixel(int pn, int x, int y) {
    int c[5][5];
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) c[j][i] = low_colour(pn, x + i - 2, y + j - 2);
    int p = c[2][2];
    uint8_t *pg = pages[pn];
    size_t at = (size_t)y * S * W + (size_t)x * S;
    uint8_t *row = pg + at;
    // Emboss: a lit top-left edge and a shaded bottom-right edge make icons and panels read as
    // raised shapes. Black backgrounds and one-pixel strokes are left alone.
    int lit = p && (c[1][2] != p || c[2][1] != p), dark = p && (c[3][2] != p || c[2][3] != p);
    uint8_t emboss = lit == dark ? SHADE_NEUTRAL : lit ? 168 : 88;
    uint8_t *sh = shades[pn] + at;
    for (int j = 0; j < S; j++, sh += W) memset(sh, emboss, (size_t)S);
    if (c[1][2] == p && c[3][2] == p && c[2][1] == p && c[2][3] == p) {
        uint8_t v = (uint8_t)(p << 4 | p);
        for (int j = 0; j < S; j++, row += W) memset(row, v, (size_t)S);
        return;
    }
    // First round: the centre 3x3 becomes 6x6.
    int e6[6][6], e[4];
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            int yy = j + 1, xx = i + 1;
            epx(c[yy][xx], c[yy - 1][xx], c[yy][xx + 1], c[yy][xx - 1], c[yy + 1][xx], e);
            e6[j * 2][i * 2] = e[0];
            e6[j * 2][i * 2 + 1] = e[1];
            e6[j * 2 + 1][i * 2] = e[2];
            e6[j * 2 + 1][i * 2 + 1] = e[3];
        }
    }
    // Second round: the centre 2x2 of that becomes the pixel's final 4x4.
    int f[4][4];
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int yy = j + 2, xx = i + 2;
            epx(e6[yy][xx], e6[yy - 1][xx], e6[yy][xx + 1], e6[yy][xx - 1], e6[yy + 1][xx], e);
            f[j * 2][i * 2] = e[0];
            f[j * 2][i * 2 + 1] = e[1];
            f[j * 2 + 1][i * 2] = e[2];
            f[j * 2 + 1][i * 2 + 1] = e[3];
        }
    }
    for (int j = 0; j < S; j++, row += W) {
        const int *fr = f[j * 4 / S];
        for (int i = 0; i < S; i++) {
            int v = fr[i * 4 / S];
            row[i] = (uint8_t)(v << 4 | v);
        }
    }
}

static void smooth_page(int pn) {
    for (int pos = 0; pos < 8000; pos++) {
        uint8_t d = dirty[pn][pos];
        if (!d) continue;
        dirty[pn][pos] = 0;
        d &= lowsrc[pn][pos];
        if (!d) continue;
        int y = pos / 40, xb = pos % 40;
        for (int bit = 0; bit < 8; bit++)
            if (d & (0x80 >> bit)) smooth_pixel(pn, xb * 8 + bit, y);
    }
}

// ---- Presentation --------------------------------------------------------------
static uint8_t mul_table[256][256]; // channel * shade / 128, clamped

static inline uint32_t shaded(uint32_t c, int s) {
    return 0xFF000000u | (uint32_t)mul_table[(c >> 16) & 255][s] << 16 | (uint32_t)mul_table[(c >> 8) & 255][s] << 8 |
           mul_table[c & 255][s];
}

static inline uint32_t mix(uint32_t a, uint32_t b, int t) { // t 0..255 moves a towards b
    int r = (int)((a >> 16) & 255), g = (int)((a >> 8) & 255), bl = (int)(a & 255);
    r += (((int)((b >> 16) & 255) - r) * t) >> 8;
    g += (((int)((b >> 8) & 255) - g) * t) >> 8;
    bl += (((int)(b & 255) - bl) * t) >> 8;
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

// Colour ramps for the ramp materials: {shade, r, g, b} knots. ref is the game's own colour for the
// material, used so anything that dims or lifts the game's palette still shows through.
typedef struct { int shade, r, g, b; } RampKnot;
static const struct { uint32_t ref; RampKnot knots[6]; } ramp_defs[MAT_END - MAT_RAMP] = {
    {0x00BE00, {{0, 16, 18, 21}, {112, 54, 58, 62}, {180, 84, 88, 90}, {205, 110, 108, 88}, {225, 226, 186, 54}, {255, 238, 236, 224}}}, // deck
    {0x7D7D7D, {{0, 24, 25, 26}, {128, 92, 92, 88}, {200, 134, 134, 126}, {255, 170, 170, 162}}}, // deck steel
    {0x7D7D7D, {{0, 24, 30, 36}, {128, 88, 98, 108}, {200, 124, 134, 142}, {255, 158, 168, 176}}}, // hull
    {0x3C9D7D, {{0, 18, 44, 28}, {84, 34, 80, 44}, {128, 66, 112, 56}, {168, 112, 134, 70}, {208, 162, 148, 94}, {255, 198, 180, 132}}}, // terrain
    {0x9D7D5D, {{0, 34, 30, 28}, {88, 84, 72, 62}, {128, 122, 106, 90}, {190, 160, 146, 128}, {255, 208, 198, 184}}}, // rock
    {0xDEBE00, {{0, 120, 100, 64}, {128, 214, 190, 134}, {255, 248, 234, 194}}}, // sand
    {0x7D7D7D, {{0, 44, 46, 48}, {128, 126, 128, 126}, {255, 200, 202, 198}}}, // concrete
};
static uint32_t ramps[MAT_END - MAT_RAMP][256];
static uint8_t frame_ramp_scale[MAT_END - MAT_RAMP][256];

static inline int luma(uint32_t c) { return (int)((((c >> 16) & 255) * 2 + ((c >> 8) & 255) * 5 + (c & 255)) / 8); }

static void build_ramps(void) {
    for (int m = 0; m < MAT_END - MAT_RAMP; m++) {
        const RampKnot *k = ramp_defs[m].knots;
        int seg = 0;
        for (int s = 0; s < 256; s++) {
            while (k[seg + 1].shade && k[seg + 1].shade <= s && k[seg + 1].shade < 255) seg++;
            const RampKnot *a = &k[seg], *b = k[seg + 1].shade ? &k[seg + 1] : a;
            int span_len = b->shade - a->shade;
            int t = span_len > 0 ? (s - a->shade) * 256 / span_len : 0;
            if (t > 256) t = 256;
            ramps[m][s] = 0xFF000000u | (uint32_t)(a->r + ((b->r - a->r) * t >> 8)) << 16 |
                          (uint32_t)(a->g + ((b->g - a->g) * t >> 8)) << 8 | (uint32_t)(a->b + ((b->b - a->b) * t >> 8));
        }
    }
}

#define RENDER_THREADS 4
typedef struct { int y0, y1; } RenderBand;
static uint32_t frame_sky[256], frame_sea[256];
static int32_t frame_sea_scale[256], frame_sea_v[256];
static uint8_t frame_sea_level[256];
static uint32_t *frame_out;
static const uint32_t *frame_lut;
static int frame_pn, frame_drift;

static int render_band(void *arg) {
    const RenderBand *band = (const RenderBand *)arg;
    const uint8_t *pg = pages[frame_pn], *mt = mats[frame_pn], *sh = shades[frame_pn];
    const uint32_t *lut = frame_lut;
    int cx = W / 2;
    for (int y = band->y0; y < band->y1; y++) {
        size_t base = (size_t)y * W;
        uint32_t *o = frame_out + base;
        int py = (y * 256 / (S * 2)) << 16; // texture scale independent of the shadow scale
        for (int x = 0; x < W; x++) {
            size_t i = base + x;
            uint8_t pair = pg[i];
            if (mt[i] >= MAT_RAMP) {
                int k = mt[i] - MAT_RAMP;
                o[x] = shaded(ramps[k][sh[i]], frame_ramp_scale[k][pair]);
                continue;
            }
            switch (mt[i]) {
            case MAT_SKY: {
                // Noise on the lookup index hides the gradient's banding.
                uint32_t h = ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 2246822519u);
                int d = sh[i] + (int)((h >> 28) & 15) - 7;
                o[x] = frame_sky[d < 0 ? 0 : d > 255 ? 255 : d];
                break;
            }
            case MAT_SEA: {
                int d = sh[i];
                int32_t u = (x - cx) * frame_sea_scale[d] + frame_drift;
                int w = tex_sample(TEX_WAVES, frame_sea_level[d], u, frame_sea_v[d]);
                uint32_t c = shaded(frame_sea[d], 104 + (w * 48) / 255);
                // Sparkle on wave crests, fading out in the distance.
                if (w > 226 && d > 16) c = mix(c, 0xFFFFFFFFu, (w - 226) * 3);
                o[x] = c;
                break;
            }
            case MAT_WORLD:
                o[x] = shaded(lut[pair], sh[i]);
                break;
            default:
                if (pair == (COLOUR_STEEL << 4 | COLOUR_STEEL) || pair == (COLOUR_BRONZE << 4 | COLOUR_BRONZE)) {
                    int u = (x * 256 / (S * 2)) << 16;
                    int t = tex_sample(TEX_PANEL, 0, u, py);
                    o[x] = shaded(shaded(lut[pair], t), sh[i]);
                } else {
                    o[x] = shaded(lut[pair], sh[i]);
                }
                break;
            }
        }
    }
    return 0;
}

static void render_modern(uint32_t *out, int pn, const uint32_t *lut) {
    static int tables_ready;
    if (!tables_ready) {
        for (int c = 0; c < 256; c++) {
            for (int s = 0; s < 256; s++) {
                int v = c * s / SHADE_NEUTRAL;
                mul_table[c][s] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
        build_ramps();
        tables_ready = 1;
    }
    for (int m = 0; m < MAT_END - MAT_RAMP; m++) {
        int ref = luma(ramp_defs[m].ref);
        for (int p = 0; p < 256; p++) {
            int s = luma(lut[p]) * SHADE_NEUTRAL / ref;
            frame_ramp_scale[m][p] = (uint8_t)(s < 40 ? 40 : s > 190 ? 190 : s);
        }
    }
    // Sky and sea colours by distance from the horizon, rebuilt so they follow the palette.
    const uint32_t haze = 0xFFDCE8F0u;
    uint32_t sky_base = lut[COLOUR_SKY << 4 | COLOUR_SKY], sea_base = lut[COLOUR_SEA << 4 | COLOUR_SEA];
    uint32_t sky_top = shaded(sky_base, 92);
    float seconds = (float)vga_frame_tick / 44100.0f;
    for (int d = 0; d < 256; d++) {
        float t = (float)d / 255.0f;
        frame_sky[d] = mix(mix(haze, sky_base, (int)(255.0f * sqrtf(t * 3.0f > 1.0f ? 1.0f : t * 3.0f))), sky_top, (int)(255.0f * t * t));
        float h = 1.0f - t * 5.0f;
        frame_sea[d] = mix(shaded(sea_base, 112), haze, (int)(190.0f * (h > 0 ? h * h : 0)));
        // Waves recede towards the horizon: one texel covers more of the sea the smaller d is.
        float perspective = 3600.0f / ((float)d + 14.0f);
        float next = 3600.0f / ((float)d + 15.0f);
        frame_sea_v[d] = (int32_t)((perspective + seconds * 9.0f) * 65536.0f);
        float per_pixel = perspective / (float)(W / 2) * 2.2f;
        frame_sea_scale[d] = (int32_t)(per_pixel * 65536.0f);
        // Filter for the vertical step too; d only changes every couple of shadow rows.
        float per_row = (perspective - next) * HORIZON_UNITS_PER_PIXEL / (float)S;
        frame_sea_level[d] = (uint8_t)tex_level_for(per_row > per_pixel ? per_row : per_pixel);
    }
    frame_out = out;
    frame_pn = pn;
    frame_lut = lut;
    frame_drift = (int)(seconds * 3.0f * 65536.0f);
    // The sea and panel textures cost real time at this resolution, so share the rows out.
    int cpus = SDL_GetCPUCount();
    int threads = cpus > RENDER_THREADS ? RENDER_THREADS : cpus < 1 ? 1 : cpus;
    SDL_Thread *worker[RENDER_THREADS];
    int rows = 200 * S;
    static RenderBand bands[RENDER_THREADS];
    for (int i = 0; i < threads; i++) {
        bands[i].y0 = rows * i / threads;
        bands[i].y1 = rows * (i + 1) / threads;
        worker[i] = i ? SDL_CreateThread(render_band, "render", &bands[i]) : NULL;
        if (i && !worker[i]) render_band(&bands[i]);
    }
    render_band(&bands[0]);
    for (int i = 1; i < threads; i++)
        if (worker[i]) SDL_WaitThread(worker[i], NULL);
}

void hires_render(uint32_t *out) {
    if (!vga_planar_active()) {
        memset(out, 0, (size_t)W * 200 * S * 4);
        return;
    }
    const uint32_t *pal = vga_palette_argb();
    static uint32_t lut[256];
    for (int a = 0; a < 16; a++) {
        for (int b = 0; b < 16; b++) {
            uint32_t ca = pal[a], cb = pal[b];
            uint32_t r = (((ca >> 16) & 255) + ((cb >> 16) & 255)) / 2;
            uint32_t g = (((ca >> 8) & 255) + ((cb >> 8) & 255)) / 2;
            uint32_t bl = ((ca & 255) + (cb & 255)) / 2;
            lut[a << 4 | b] = 0xFF000000u | r << 16 | g << 8 | bl;
        }
    }
    uint32_t start = vga_start_address();
    if (S >= 2) smooth_page((start >> 13) & 7);
    const uint8_t *pg = pages[(start >> 13) & 7];
    size_t n = (size_t)W * 200 * S;
    if (hires_modern && !hires_keep_dither) {
        render_modern(out, (start >> 13) & 7, lut);
    } else if (hires_keep_dither) {
        for (size_t i = 0; i < n; i++) {
            uint8_t v = pg[i];
            size_t y = i / W, x = i % W;
            int odd = (int)((x / S + y / S) & 1);
            int c = odd ? (v & 15) : (v >> 4);
            out[i] = pal[c];
        }
    } else {
        for (size_t i = 0; i < n; i++) out[i] = lut[pg[i]];
    }
}
