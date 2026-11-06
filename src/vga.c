// EGA/VGA planar adapter for mode 0Dh (320x200, 16 colours) and text-mode stubs.
#include "machine.h"
#include <string.h>

uint8_t vga_planes[4][0x10000];
static uint8_t latch[4];
static uint32_t last_read_off;
static int mode = 3;
static uint8_t seq[8], seq_idx;
static uint8_t gc[16], gc_idx;
static uint8_t crtc[32], crtc_idx;
static uint8_t atc[32], atc_idx;
static int atc_flip;
static uint8_t misc_out = 0x63;
static uint8_t dac[256][3];
static int dac_w, dac_r, dac_comp;
static uint32_t pal_argb[16];
static int pal_dirty = 1;
uint32_t vga_frame_tick;
static const uint32_t ticks_per_frame = 630; // 44100 / 70

static const uint8_t default_dac16[16][3] = {
    {0, 0, 0},    {0, 0, 42},   {0, 42, 0},   {0, 42, 42},  {42, 0, 0},   {42, 0, 42},  {42, 21, 0},  {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
};

void vga_set_mode(int m) {
    mode = m & 0x7F;
    memset(seq, 0, sizeof seq);
    memset(gc, 0, sizeof gc);
    seq[2] = 0x0F;
    gc[8] = 0xFF;
    gc[7] = 0x0F;
    crtc[0x0C] = crtc[0x0D] = 0;
    atc_flip = 0;
    atc[0x12] = 0x0F;
    // EGA palette registers for 16 colours: 0-7 map to 0-7, 8-15 to 16-23.
    for (int i = 0; i < 16; i++) atc[i] = (uint8_t)(i < 8 ? i : i + 8);
    atc[6] = 0x14;
    // Default VGA DAC for the 64-colour EGA space.
    for (int i = 0; i < 64; i++) {
        int r = ((i >> 2) & 1) * 2 + ((i >> 5) & 1);
        int g = ((i >> 1) & 1) * 2 + ((i >> 4) & 1);
        int b = (i & 1) * 2 + ((i >> 3) & 1);
        dac[i][0] = (uint8_t)(r * 21);
        dac[i][1] = (uint8_t)(g * 21);
        dac[i][2] = (uint8_t)(b * 21);
    }
    (void)default_dac16;
    if (!(m & 0x80)) memset(vga_planes, 0, sizeof vga_planes);
    pal_dirty = 1;
    hires_mode_set();
}
int vga_get_mode(void) { return mode; }
int vga_planar_active(void) { return mode == 0x0D; }
uint16_t vga_start_address(void) { return (uint16_t)(crtc[0x0C] << 8 | crtc[0x0D]); }

void vga_set_palette_reg(int idx, uint8_t val) {
    if (idx < 32) atc[idx] = val;
    pal_dirty = 1;
}
void vga_set_dac(int idx, uint8_t r, uint8_t g, uint8_t b) {
    dac[idx & 255][0] = r & 63;
    dac[idx & 255][1] = g & 63;
    dac[idx & 255][2] = b & 63;
    pal_dirty = 1;
}

const uint32_t *vga_palette_argb(void) {
    if (pal_dirty) {
        for (int i = 0; i < 16; i++) {
            const uint8_t *c = dac[atc[i] & 0x3F];
            pal_argb[i] = 0xFF000000u | (uint32_t)(c[0] * 255 / 63) << 16 | (uint32_t)(c[1] * 255 / 63) << 8 | (uint32_t)(c[2] * 255 / 63);
        }
        pal_dirty = 0;
    }
    return pal_argb;
}

uint8_t vga_read(uint32_t off) {
    if (mode != 0x0D) return 0xFF;
    off &= 0xFFFF;
    for (int p = 0; p < 4; p++) latch[p] = vga_planes[p][off];
    last_read_off = off;
    if (gc[5] & 8) { // read mode 1: colour compare
        uint8_t r = 0;
        for (int b = 0; b < 8; b++) {
            int match = 1;
            for (int p = 0; p < 4; p++) {
                if (gc[7] & (1 << p)) {
                    if (((latch[p] >> b) & 1) != ((gc[2] >> p) & 1)) match = 0;
                }
            }
            if (match) r |= 1 << b;
        }
        return r;
    }
    return latch[gc[4] & 3];
}

void vga_write(uint32_t off, uint8_t val) {
    if (mode != 0x0D) return;
    off &= 0xFFFF;
    int wmode = gc[5] & 3;
    uint8_t mapmask = seq[2] & 0x0F;
    if (wmode == 1) {
        for (int p = 0; p < 4; p++)
            if (mapmask & (1 << p)) vga_planes[p][off] = latch[p];
        if (mapmask) hires_note_latch_copy(off, last_read_off, 0xFF);
        return;
    }
    uint8_t rot = gc[3] & 7;
    if (rot) val = (uint8_t)(val >> rot | val << (8 - rot));
    uint8_t func = (gc[3] >> 3) & 3;
    uint8_t bitmask = gc[8];
    uint8_t in[4];
    if (wmode == 0) {
        for (int p = 0; p < 4; p++)
            in[p] = (gc[1] & (1 << p)) ? ((gc[0] & (1 << p)) ? 0xFF : 0) : val;
    } else if (wmode == 2) {
        for (int p = 0; p < 4; p++) in[p] = (val & (1 << p)) ? 0xFF : 0;
    } else { // write mode 3 (VGA)
        bitmask &= val;
        for (int p = 0; p < 4; p++) in[p] = (gc[0] & (1 << p)) ? 0xFF : 0;
    }
    for (int p = 0; p < 4; p++) {
        if (!(mapmask & (1 << p))) continue;
        uint8_t v = in[p];
        switch (func) {
        case 1: v &= latch[p]; break;
        case 2: v |= latch[p]; break;
        case 3: v ^= latch[p]; break;
        }
        vga_planes[p][off] = (uint8_t)((v & bitmask) | (latch[p] & ~bitmask));
    }
    if (mapmask && bitmask) hires_note_cpu_write(off, bitmask);
    else if (mapmask) hires_note_latch_copy(off, last_read_off, 0xFF); // bit mask 0 stores the latches
}

void vga_retrace_tick(void) { vga_frame_tick++; }

uint8_t vga_status_now(void) {
    uint32_t ph = vga_frame_tick % ticks_per_frame;
    uint8_t s = 0;
    if (ph < 40) s |= 0x08 | 0x01;
    else if ((vga_frame_tick & 1)) s |= 0x01;
    return s;
}

uint8_t vga_port_in(uint16_t port) {
    switch (port) {
    case 0x3C0: return atc_idx;
    case 0x3C1: return atc[atc_idx & 31];
    case 0x3C4: return seq_idx;
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3C7: return 0;
    case 0x3C8: return (uint8_t)dac_w;
    case 0x3C9: {
        uint8_t v = dac[dac_r][dac_comp];
        if (++dac_comp == 3) { dac_comp = 0; dac_r = (dac_r + 1) & 255; }
        return v;
    }
    case 0x3CC: return misc_out;
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3D4: return crtc_idx;
    case 0x3D5: return crtc[crtc_idx & 31];
    case 0x3DA:
    case 0x3BA:
        atc_flip = 0;
        return vga_status_now();
    }
    return 0xFF;
}

void vga_port_out(uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3C0:
        if (!atc_flip) atc_idx = val & 0x3F;
        else {
            if ((atc_idx & 0x1F) < 0x15) atc[atc_idx & 0x1F] = val;
            pal_dirty = 1;
        }
        atc_flip = !atc_flip;
        break;
    case 0x3C2: misc_out = val; break;
    case 0x3C4: seq_idx = val & 7; break;
    case 0x3C5: seq[seq_idx] = val; break;
    case 0x3C7: dac_r = val; dac_comp = 0; break;
    case 0x3C8: dac_w = val; dac_comp = 0; break;
    case 0x3C9:
        dac[dac_w][dac_comp] = val & 63;
        pal_dirty = 1;
        if (++dac_comp == 3) { dac_comp = 0; dac_w = (dac_w + 1) & 255; }
        break;
    case 0x3CE: gc_idx = val & 15; break;
    case 0x3CF: gc[gc_idx] = val; break;
    case 0x3D4: crtc_idx = val & 31; break;
    case 0x3D5: crtc[crtc_idx] = val; break;
    }
}

void vga_render_lowres(uint32_t *out) {
    const uint32_t *pal = vga_palette_argb();
    uint32_t start = vga_start_address();
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 320; x++) {
            uint32_t off = (start + (uint32_t)y * 40 + (uint32_t)(x >> 3)) & 0xFFFF;
            int bit = 7 - (x & 7);
            int c = ((vga_planes[0][off] >> bit) & 1) | ((vga_planes[1][off] >> bit) & 1) << 1 |
                    ((vga_planes[2][off] >> bit) & 1) << 2 | ((vga_planes[3][off] >> bit) & 1) << 3;
            out[y * 320 + x] = pal[c];
        }
    }
}
