// Real-mode 8086/80186 interpreter. PUSHF/POPF behave like a 386 in real mode so
// the game's CPU probe picks its fastest code paths.
#include "machine.h"
#include <stdio.h>

Cpu cpu;
uint8_t mem[0x110000];
uint8_t hookmap[0x100000];

#define R8(i) (cpu.r.b[(((i) & 3) << 1) | ((i) >> 2)])
#define R16(i) (cpu.r.w[i])
#define FLAG(f) ((cpu.flags & (f)) != 0)
#define LIN(s, o) ((((uint32_t)(s) << 4) + (uint16_t)(o)) & 0xFFFFF)

static inline uint8_t rd8l(uint32_t a) {
    if (a >= 0xA0000 && a < 0xB0000) return vga_read(a - 0xA0000);
    return mem[a];
}
static inline void wr8l(uint32_t a, uint8_t v) {
    if (a >= 0xA0000 && a < 0xB0000) { vga_write(a - 0xA0000, v); return; }
    if (a >= 0xC0000) return;
    mem[a] = v;
}
static inline uint8_t rd8(uint16_t s, uint16_t o) { return rd8l(LIN(s, o)); }
static inline void wr8(uint16_t s, uint16_t o, uint8_t v) { wr8l(LIN(s, o), v); }
static inline uint16_t rd16(uint16_t s, uint16_t o) {
    return rd8l(LIN(s, o)) | (uint16_t)rd8l(LIN(s, (uint16_t)(o + 1))) << 8;
}
static inline void wr16(uint16_t s, uint16_t o, uint16_t v) {
    wr8l(LIN(s, o), (uint8_t)v);
    wr8l(LIN(s, (uint16_t)(o + 1)), (uint8_t)(v >> 8));
}
static inline uint8_t fetch8(void) { return mem[LIN(cpu.seg[CS], cpu.ip++)]; }
static inline uint16_t fetch16(void) {
    uint16_t v = fetch8();
    return v | (uint16_t)fetch8() << 8;
}
static inline void push16(uint16_t v) {
    cpu.r.w[SP] -= 2;
    wr16(cpu.seg[SS], cpu.r.w[SP], v);
}
static inline uint16_t pop16(void) {
    uint16_t v = rd16(cpu.seg[SS], cpu.r.w[SP]);
    cpu.r.w[SP] += 2;
    return v;
}

static uint8_t parity[256];
static int parity_ready;
static void init_parity(void) {
    for (int i = 0; i < 256; i++) {
        int c = 0;
        for (int b = 0; b < 8; b++) c += (i >> b) & 1;
        parity[i] = (c & 1) ? 0 : F_PF;
    }
    parity_ready = 1;
}

static inline void set_szp8(uint8_t r) {
    cpu.flags &= ~(F_SF | F_ZF | F_PF);
    cpu.flags |= parity[r] | (r ? 0 : F_ZF) | (r & 0x80 ? F_SF : 0);
}
static inline void set_szp16(uint16_t r) {
    cpu.flags &= ~(F_SF | F_ZF | F_PF);
    cpu.flags |= parity[r & 0xFF] | (r ? 0 : F_ZF) | (r & 0x8000 ? F_SF : 0);
}

// ALU: op 0..7 = add or adc sbb and sub xor cmp. Returns the result.
static uint8_t alu8(int op, uint8_t a, uint8_t b) {
    unsigned r;
    int c = FLAG(F_CF);
    switch (op) {
    case 0: case 2:
        r = (unsigned)a + b + (op == 2 ? c : 0);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        if (r & 0x100) cpu.flags |= F_CF;
        if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
        if ((a ^ r) & (b ^ r) & 0x80) cpu.flags |= F_OF;
        break;
    case 3: case 5: case 7:
        r = (unsigned)a - b - (op == 3 ? c : 0);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        if (r & 0x100) cpu.flags |= F_CF;
        if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
        if ((a ^ b) & (a ^ r) & 0x80) cpu.flags |= F_OF;
        break;
    default:
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        break;
    }
    set_szp8((uint8_t)r);
    return (uint8_t)r;
}
static uint16_t alu16(int op, uint16_t a, uint16_t b) {
    unsigned r;
    int c = FLAG(F_CF);
    switch (op) {
    case 0: case 2:
        r = (unsigned)a + b + (op == 2 ? c : 0);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        if (r & 0x10000) cpu.flags |= F_CF;
        if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
        if ((a ^ r) & (b ^ r) & 0x8000) cpu.flags |= F_OF;
        break;
    case 3: case 5: case 7:
        r = (unsigned)a - b - (op == 3 ? c : 0);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        if (r & 0x10000) cpu.flags |= F_CF;
        if ((a ^ b ^ r) & 0x10) cpu.flags |= F_AF;
        if ((a ^ b) & (a ^ r) & 0x8000) cpu.flags |= F_OF;
        break;
    default:
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        cpu.flags &= ~(F_CF | F_AF | F_OF);
        break;
    }
    set_szp16((uint16_t)r);
    return (uint16_t)r;
}
static uint8_t inc8(uint8_t a) {
    uint8_t r = a + 1;
    cpu.flags &= ~(F_AF | F_OF);
    if ((r & 0xF) == 0) cpu.flags |= F_AF;
    if (r == 0x80) cpu.flags |= F_OF;
    set_szp8(r);
    return r;
}
static uint8_t dec8(uint8_t a) {
    uint8_t r = a - 1;
    cpu.flags &= ~(F_AF | F_OF);
    if ((r & 0xF) == 0xF) cpu.flags |= F_AF;
    if (r == 0x7F) cpu.flags |= F_OF;
    set_szp8(r);
    return r;
}
static uint16_t inc16(uint16_t a) {
    uint16_t r = a + 1;
    cpu.flags &= ~(F_AF | F_OF);
    if ((r & 0xF) == 0) cpu.flags |= F_AF;
    if (r == 0x8000) cpu.flags |= F_OF;
    set_szp16(r);
    return r;
}
static uint16_t dec16(uint16_t a) {
    uint16_t r = a - 1;
    cpu.flags &= ~(F_AF | F_OF);
    if ((r & 0xF) == 0xF) cpu.flags |= F_AF;
    if (r == 0x7FFF) cpu.flags |= F_OF;
    set_szp16(r);
    return r;
}

// Shifts/rotates: op 0..7 = rol ror rcl rcr shl shr sal sar.
static uint32_t shift(int op, uint32_t v, int count, int bits) {
    uint32_t mask = bits == 8 ? 0xFF : 0xFFFF, msb = bits == 8 ? 0x80 : 0x8000;
    count &= 31;
    if (!count) return v;
    v &= mask;
    int cf = FLAG(F_CF), of = FLAG(F_OF);
    switch (op) {
    case 0:
        for (int i = 0; i < count; i++) { cf = (v & msb) != 0; v = ((v << 1) | cf) & mask; }
        of = ((v & msb) != 0) ^ cf;
        break;
    case 1:
        for (int i = 0; i < count; i++) { cf = v & 1; v = (v >> 1) | (cf ? msb : 0); }
        of = ((v & msb) != 0) ^ ((v & (msb >> 1)) != 0);
        break;
    case 2:
        for (int i = 0; i < count; i++) { int n = (v & msb) != 0; v = ((v << 1) | cf) & mask; cf = n; }
        of = ((v & msb) != 0) ^ cf;
        break;
    case 3:
        for (int i = 0; i < count; i++) { int n = v & 1; v = (v >> 1) | (cf ? msb : 0); cf = n; }
        of = ((v & msb) != 0) ^ ((v & (msb >> 1)) != 0);
        break;
    case 4: case 6:
        for (int i = 0; i < count; i++) { cf = (v & msb) != 0; v = (v << 1) & mask; }
        of = ((v & msb) != 0) ^ cf;
        goto szp;
    case 5:
        of = (v & msb) != 0;
        for (int i = 0; i < count; i++) { cf = v & 1; v >>= 1; }
        goto szp;
    case 7:
        for (int i = 0; i < count; i++) { cf = v & 1; v = (v >> 1) | (v & msb); }
        of = 0;
    szp:
        if (bits == 8) set_szp8((uint8_t)v); else set_szp16((uint16_t)v);
        break;
    }
    cpu.flags &= ~(F_CF | F_OF);
    if (cf) cpu.flags |= F_CF;
    if (of) cpu.flags |= F_OF;
    return v;
}

// ModRM decoding.
static int seg_override;
static int mod, reg_f, rm;
static uint16_t ea_off, ea_seg;

static void decode_modrm(void) {
    uint8_t m = fetch8();
    mod = m >> 6;
    reg_f = (m >> 3) & 7;
    rm = m & 7;
    if (mod == 3) return;
    uint16_t off = 0;
    int def = DS;
    switch (rm) {
    case 0: off = R16(BX) + R16(SI); break;
    case 1: off = R16(BX) + R16(DI); break;
    case 2: off = R16(BP) + R16(SI); def = SS; break;
    case 3: off = R16(BP) + R16(DI); def = SS; break;
    case 4: off = R16(SI); break;
    case 5: off = R16(DI); break;
    case 6:
        if (mod == 0) off = fetch16();
        else { off = R16(BP); def = SS; }
        break;
    case 7: off = R16(BX); break;
    }
    if (mod == 1) off += (int8_t)fetch8();
    else if (mod == 2) off += fetch16();
    ea_off = off;
    ea_seg = cpu.seg[seg_override >= 0 ? seg_override : def];
}
static inline uint8_t rm_rd8(void) { return mod == 3 ? R8(rm) : rd8(ea_seg, ea_off); }
static inline void rm_wr8(uint8_t v) { if (mod == 3) R8(rm) = v; else wr8(ea_seg, ea_off, v); }
static inline uint16_t rm_rd16(void) { return mod == 3 ? R16(rm) : rd16(ea_seg, ea_off); }
static inline void rm_wr16(uint16_t v) { if (mod == 3) R16(rm) = v; else wr16(ea_seg, ea_off, v); }

void cpu_interrupt(int n) {
    push16(cpu.flags | 2);
    push16(cpu.seg[CS]);
    push16(cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    cpu.ip = mem[n * 4] | mem[n * 4 + 1] << 8;
    cpu.seg[CS] = mem[n * 4 + 2] | mem[n * 4 + 3] << 8;
    cpu.halted = 0;
}

static inline int cond(int c) {
    int r;
    switch (c >> 1) {
    case 0: r = FLAG(F_OF); break;
    case 1: r = FLAG(F_CF); break;
    case 2: r = FLAG(F_ZF); break;
    case 3: r = FLAG(F_CF) || FLAG(F_ZF); break;
    case 4: r = FLAG(F_SF); break;
    case 5: r = FLAG(F_PF); break;
    case 6: r = FLAG(F_SF) != FLAG(F_OF); break;
    default: r = FLAG(F_ZF) || FLAG(F_SF) != FLAG(F_OF); break;
    }
    return (c & 1) ? !r : r;
}

static void divide_error(uint16_t start_ip) {
    cpu.ip = start_ip;
    cpu_interrupt(0);
}

static int string_op(int op, int rep) {
    int width = (op & 1) ? 2 : 1;
    int delta = FLAG(F_DF) ? -width : width;
    int src = seg_override >= 0 ? seg_override : DS;
    int base = op & 0xFE;
    int n = 0;
    if (rep && R16(CX) == 0) return 0;
    for (;;) {
        switch (base) {
        case 0xA4: // movs
            if (width == 1) wr8(cpu.seg[ES], R16(DI), rd8(cpu.seg[src], R16(SI)));
            else wr16(cpu.seg[ES], R16(DI), rd16(cpu.seg[src], R16(SI)));
            R16(SI) += delta; R16(DI) += delta;
            break;
        case 0xA6: // cmps
            if (width == 1) alu8(7, rd8(cpu.seg[src], R16(SI)), rd8(cpu.seg[ES], R16(DI)));
            else alu16(7, rd16(cpu.seg[src], R16(SI)), rd16(cpu.seg[ES], R16(DI)));
            R16(SI) += delta; R16(DI) += delta;
            break;
        case 0xAA: // stos
            if (width == 1) wr8(cpu.seg[ES], R16(DI), R8(0));
            else wr16(cpu.seg[ES], R16(DI), R16(AX));
            R16(DI) += delta;
            break;
        case 0xAC: // lods
            if (width == 1) R8(0) = rd8(cpu.seg[src], R16(SI));
            else R16(AX) = rd16(cpu.seg[src], R16(SI));
            R16(SI) += delta;
            break;
        case 0xAE: // scas
            if (width == 1) alu8(7, R8(0), rd8(cpu.seg[ES], R16(DI)));
            else alu16(7, R16(AX), rd16(cpu.seg[ES], R16(DI)));
            R16(DI) += delta;
            break;
        case 0x6C: // ins
            if (width == 1) wr8(cpu.seg[ES], R16(DI), port_in(R16(DX)));
            else wr16(cpu.seg[ES], R16(DI), port_in(R16(DX)) | port_in(R16(DX)) << 8);
            R16(DI) += delta;
            break;
        case 0x6E: // outs
            port_out(R16(DX), rd8(cpu.seg[src], R16(SI)));
            if (width == 2) port_out(R16(DX), rd8(cpu.seg[src], (uint16_t)(R16(SI) + 1)));
            R16(SI) += delta;
            break;
        }
        n++;
        if (!rep) break;
        if (--R16(CX) == 0) break;
        if (base == 0xA6 || base == 0xAE) {
            int z = FLAG(F_ZF);
            if ((rep == 0xF3 && !z) || (rep == 0xF2 && z)) break;
        }
    }
    return n;
}

int cpu_run(int budget) {
    int used = 0;
    if (!parity_ready) init_parity();
    while (used < budget) {
        uint32_t lin = LIN(cpu.seg[CS], cpu.ip);
        if (hookmap[lin]) {
            hook_dispatch(lin);
            lin = LIN(cpu.seg[CS], cpu.ip);
        }
        if (cpu.halted) return budget;
        uint16_t start_ip = cpu.ip;
        used++;
        seg_override = -1;
        int rep = 0;
        uint8_t op;
    prefix:
        op = fetch8();
        switch (op) {
        case 0x26: seg_override = ES; goto prefix;
        case 0x2E: seg_override = CS; goto prefix;
        case 0x36: seg_override = SS; goto prefix;
        case 0x3E: seg_override = DS; goto prefix;
        case 0xF0: goto prefix;
        case 0xF2: case 0xF3: rep = op; goto prefix;
        }
        if (op < 0x40 && (op & 7) < 6) {
            int alu = op >> 3, form = op & 7;
            switch (form) {
            case 0: { decode_modrm(); uint8_t v = alu8(alu, rm_rd8(), R8(reg_f)); if (alu != 7) rm_wr8(v); break; }
            case 1: { decode_modrm(); uint16_t v = alu16(alu, rm_rd16(), R16(reg_f)); if (alu != 7) rm_wr16(v); break; }
            case 2: { decode_modrm(); uint8_t v = alu8(alu, R8(reg_f), rm_rd8()); if (alu != 7) R8(reg_f) = v; break; }
            case 3: { decode_modrm(); uint16_t v = alu16(alu, R16(reg_f), rm_rd16()); if (alu != 7) R16(reg_f) = v; break; }
            case 4: { uint8_t i = fetch8(); uint8_t v = alu8(alu, R8(0), i); if (alu != 7) R8(0) = v; break; }
            case 5: { uint16_t i = fetch16(); uint16_t v = alu16(alu, R16(AX), i); if (alu != 7) R16(AX) = v; break; }
            }
            continue;
        }
        switch (op) {
        case 0x06: push16(cpu.seg[ES]); break;
        case 0x07: cpu.seg[ES] = pop16(); break;
        case 0x0E: push16(cpu.seg[CS]); break;
        case 0x16: push16(cpu.seg[SS]); break;
        case 0x17: cpu.seg[SS] = pop16(); break;
        case 0x1E: push16(cpu.seg[DS]); break;
        case 0x1F: cpu.seg[DS] = pop16(); break;
        case 0x27: { // daa
            uint8_t a = R8(0); int cf = FLAG(F_CF), af = FLAG(F_AF);
            if ((a & 0xF) > 9 || af) { R8(0) += 6; af = 1; } else af = 0;
            if (a > 0x99 || cf) { R8(0) += 0x60; cf = 1; } else cf = 0;
            cpu.flags &= ~(F_CF | F_AF);
            if (cf) cpu.flags |= F_CF;
            if (af) cpu.flags |= F_AF;
            set_szp8(R8(0));
            break;
        }
        case 0x2F: { // das
            uint8_t a = R8(0); int cf = FLAG(F_CF), af = FLAG(F_AF);
            if ((a & 0xF) > 9 || af) { R8(0) -= 6; af = 1; } else af = 0;
            if (a > 0x99 || cf) { R8(0) -= 0x60; cf = 1; } else cf = 0;
            cpu.flags &= ~(F_CF | F_AF);
            if (cf) cpu.flags |= F_CF;
            if (af) cpu.flags |= F_AF;
            set_szp8(R8(0));
            break;
        }
        case 0x37: // aaa
            if ((R8(0) & 0xF) > 9 || FLAG(F_AF)) { R16(AX) += 0x106; cpu.flags |= F_AF | F_CF; }
            else cpu.flags &= ~(F_AF | F_CF);
            R8(0) &= 0xF;
            break;
        case 0x3F: // aas
            if ((R8(0) & 0xF) > 9 || FLAG(F_AF)) { R16(AX) -= 6; R8(4) -= 1; cpu.flags |= F_AF | F_CF; }
            else cpu.flags &= ~(F_AF | F_CF);
            R8(0) &= 0xF;
            break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            R16(op & 7) = inc16(R16(op & 7)); break;
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            R16(op & 7) = dec16(R16(op & 7)); break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            push16(R16(op & 7)); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            R16(op & 7) = pop16(); break;
        case 0x60: { // pusha
            uint16_t sp = R16(SP);
            push16(R16(AX)); push16(R16(CX)); push16(R16(DX)); push16(R16(BX));
            push16(sp); push16(R16(BP)); push16(R16(SI)); push16(R16(DI));
            break;
        }
        case 0x61: // popa
            R16(DI) = pop16(); R16(SI) = pop16(); R16(BP) = pop16(); pop16();
            R16(BX) = pop16(); R16(DX) = pop16(); R16(CX) = pop16(); R16(AX) = pop16();
            break;
        case 0x68: push16(fetch16()); break;
        case 0x6A: push16((uint16_t)(int16_t)(int8_t)fetch8()); break;
        case 0x69: case 0x6B: {
            decode_modrm();
            int32_t a = (int16_t)rm_rd16();
            int32_t b = op == 0x69 ? (int16_t)fetch16() : (int8_t)fetch8();
            int32_t r = a * b;
            R16(reg_f) = (uint16_t)r;
            cpu.flags &= ~(F_CF | F_OF);
            if (r != (int16_t)r) cpu.flags |= F_CF | F_OF;
            break;
        }
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            used += string_op(op, rep);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t d = (int8_t)fetch8();
            if (cond(op & 15)) cpu.ip += d;
            break;
        }
        case 0x80: case 0x82: {
            decode_modrm();
            uint8_t i = fetch8();
            uint8_t v = alu8(reg_f, rm_rd8(), i);
            if (reg_f != 7) rm_wr8(v);
            break;
        }
        case 0x81: {
            decode_modrm();
            uint16_t i = fetch16();
            uint16_t v = alu16(reg_f, rm_rd16(), i);
            if (reg_f != 7) rm_wr16(v);
            break;
        }
        case 0x83: {
            decode_modrm();
            uint16_t i = (uint16_t)(int16_t)(int8_t)fetch8();
            uint16_t v = alu16(reg_f, rm_rd16(), i);
            if (reg_f != 7) rm_wr16(v);
            break;
        }
        case 0x84: decode_modrm(); alu8(4, rm_rd8(), R8(reg_f)); break;
        case 0x85: decode_modrm(); alu16(4, rm_rd16(), R16(reg_f)); break;
        case 0x86: { decode_modrm(); uint8_t t = rm_rd8(); rm_wr8(R8(reg_f)); R8(reg_f) = t; break; }
        case 0x87: { decode_modrm(); uint16_t t = rm_rd16(); rm_wr16(R16(reg_f)); R16(reg_f) = t; break; }
        case 0x88: decode_modrm(); rm_wr8(R8(reg_f)); break;
        case 0x89: decode_modrm(); rm_wr16(R16(reg_f)); break;
        case 0x8A: decode_modrm(); R8(reg_f) = rm_rd8(); break;
        case 0x8B: decode_modrm(); R16(reg_f) = rm_rd16(); break;
        case 0x8C: decode_modrm(); rm_wr16(cpu.seg[reg_f & 3]); break;
        case 0x8D: decode_modrm(); R16(reg_f) = ea_off; break;
        case 0x8E: decode_modrm(); cpu.seg[reg_f & 3] = rm_rd16(); break;
        case 0x8F: { decode_modrm(); uint16_t v = pop16(); rm_wr16(v); break; }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = R16(AX); R16(AX) = R16(op & 7); R16(op & 7) = t; break;
        }
        case 0x98: R16(AX) = (uint16_t)(int16_t)(int8_t)R8(0); break;
        case 0x99: R16(DX) = (R16(AX) & 0x8000) ? 0xFFFF : 0; break;
        case 0x9A: {
            uint16_t o = fetch16(), s = fetch16();
            push16(cpu.seg[CS]); push16(cpu.ip);
            cpu.seg[CS] = s; cpu.ip = o;
            break;
        }
        case 0x9B: break;
        case 0x9C: push16((cpu.flags & 0x7FD5) | 2); break;
        case 0x9D: cpu.flags = (pop16() & 0x7FD5) | 2; break;
        case 0x9E: cpu.flags = (cpu.flags & 0xFF00) | (R8(4) & 0xD5) | 2; break;
        case 0x9F: R8(4) = (uint8_t)(cpu.flags & 0xD5) | 2; break;
        case 0xA0: { uint16_t o = fetch16(); R8(0) = rd8(cpu.seg[seg_override >= 0 ? seg_override : DS], o); break; }
        case 0xA1: { uint16_t o = fetch16(); R16(AX) = rd16(cpu.seg[seg_override >= 0 ? seg_override : DS], o); break; }
        case 0xA2: { uint16_t o = fetch16(); wr8(cpu.seg[seg_override >= 0 ? seg_override : DS], o, R8(0)); break; }
        case 0xA3: { uint16_t o = fetch16(); wr16(cpu.seg[seg_override >= 0 ? seg_override : DS], o, R16(AX)); break; }
        case 0xA8: alu8(4, R8(0), fetch8()); break;
        case 0xA9: alu16(4, R16(AX), fetch16()); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            R8(op & 7) = fetch8(); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            R16(op & 7) = fetch16(); break;
        case 0xC0: { decode_modrm(); int c = fetch8(); rm_wr8((uint8_t)shift(reg_f, rm_rd8(), c, 8)); break; }
        case 0xC1: { decode_modrm(); int c = fetch8(); rm_wr16((uint16_t)shift(reg_f, rm_rd16(), c, 16)); break; }
        case 0xC2: { uint16_t n = fetch16(); cpu.ip = pop16(); R16(SP) += n; break; }
        case 0xC3: cpu.ip = pop16(); break;
        case 0xC4: decode_modrm(); R16(reg_f) = rd16(ea_seg, ea_off); cpu.seg[ES] = rd16(ea_seg, (uint16_t)(ea_off + 2)); break;
        case 0xC5: decode_modrm(); R16(reg_f) = rd16(ea_seg, ea_off); cpu.seg[DS] = rd16(ea_seg, (uint16_t)(ea_off + 2)); break;
        case 0xC6: { decode_modrm(); uint8_t i = fetch8(); rm_wr8(i); break; }
        case 0xC7: { decode_modrm(); uint16_t i = fetch16(); rm_wr16(i); break; }
        case 0xC8: {
            uint16_t sz = fetch16(); int lvl = fetch8() & 31;
            push16(R16(BP));
            uint16_t fp = R16(SP);
            for (int i = 1; i < lvl; i++) { R16(BP) -= 2; push16(rd16(cpu.seg[SS], R16(BP))); }
            if (lvl) push16(fp);
            R16(BP) = fp; R16(SP) -= sz;
            break;
        }
        case 0xC9: R16(SP) = R16(BP); R16(BP) = pop16(); break;
        case 0xCA: { uint16_t n = fetch16(); cpu.ip = pop16(); cpu.seg[CS] = pop16(); R16(SP) += n; break; }
        case 0xCB: cpu.ip = pop16(); cpu.seg[CS] = pop16(); break;
        case 0xCC: cpu_interrupt(3); break;
        case 0xCD: { uint8_t n = fetch8(); cpu_interrupt(n); break; }
        case 0xCE: if (FLAG(F_OF)) cpu_interrupt(4); break;
        case 0xCF: cpu.ip = pop16(); cpu.seg[CS] = pop16(); cpu.flags = (pop16() & 0x7FD5) | 2; break;
        case 0xD0: decode_modrm(); rm_wr8((uint8_t)shift(reg_f, rm_rd8(), 1, 8)); break;
        case 0xD1: decode_modrm(); rm_wr16((uint16_t)shift(reg_f, rm_rd16(), 1, 16)); break;
        case 0xD2: decode_modrm(); rm_wr8((uint8_t)shift(reg_f, rm_rd8(), R8(1), 8)); break;
        case 0xD3: decode_modrm(); rm_wr16((uint16_t)shift(reg_f, rm_rd16(), R8(1), 16)); break;
        case 0xD4: {
            uint8_t b = fetch8();
            if (!b) { divide_error(start_ip); break; }
            R8(4) = R8(0) / b; R8(0) = R8(0) % b;
            set_szp8(R8(0));
            break;
        }
        case 0xD5: {
            uint8_t b = fetch8();
            R8(0) = (uint8_t)(R8(0) + R8(4) * b); R8(4) = 0;
            set_szp8(R8(0));
            break;
        }
        case 0xD6: R8(0) = FLAG(F_CF) ? 0xFF : 0; break;
        case 0xD7: R8(0) = rd8(cpu.seg[seg_override >= 0 ? seg_override : DS], (uint16_t)(R16(BX) + R8(0))); break;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            decode_modrm(); break;
        case 0xE0: { int8_t d = (int8_t)fetch8(); if (--R16(CX) && !FLAG(F_ZF)) cpu.ip += d; break; }
        case 0xE1: { int8_t d = (int8_t)fetch8(); if (--R16(CX) && FLAG(F_ZF)) cpu.ip += d; break; }
        case 0xE2: { int8_t d = (int8_t)fetch8(); if (--R16(CX)) cpu.ip += d; break; }
        case 0xE3: { int8_t d = (int8_t)fetch8(); if (!R16(CX)) cpu.ip += d; break; }
        case 0xE4: R8(0) = port_in(fetch8()); break;
        case 0xE5: { uint8_t p = fetch8(); R8(0) = port_in(p); R8(4) = port_in(p + 1); break; }
        case 0xE6: port_out(fetch8(), R8(0)); break;
        case 0xE7: { uint8_t p = fetch8(); port_out(p, R8(0)); port_out(p + 1, R8(4)); break; }
        case 0xE8: { int16_t d = (int16_t)fetch16(); push16(cpu.ip); cpu.ip += d; break; }
        case 0xE9: { int16_t d = (int16_t)fetch16(); cpu.ip += d; break; }
        case 0xEA: { uint16_t o = fetch16(), s = fetch16(); cpu.ip = o; cpu.seg[CS] = s; break; }
        case 0xEB: { int8_t d = (int8_t)fetch8(); cpu.ip += d; break; }
        case 0xEC: R8(0) = port_in(R16(DX)); break;
        case 0xED: R8(0) = port_in(R16(DX)); R8(4) = port_in(R16(DX) + 1); break;
        case 0xEE: port_out(R16(DX), R8(0)); break;
        case 0xEF: port_out(R16(DX), R8(0)); port_out(R16(DX) + 1, R8(4)); break;
        case 0xF4: cpu.halted = 1; return budget;
        case 0xF5: cpu.flags ^= F_CF; break;
        case 0xF6: {
            decode_modrm();
            switch (reg_f) {
            case 0: case 1: alu8(4, rm_rd8(), fetch8()); break;
            case 2: rm_wr8(~rm_rd8()); break;
            case 3: { uint8_t v = rm_rd8(); rm_wr8(alu8(5, 0, v)); break; }
            case 4: {
                R16(AX) = (uint16_t)R8(0) * rm_rd8();
                cpu.flags &= ~(F_CF | F_OF);
                if (R8(4)) cpu.flags |= F_CF | F_OF;
                break;
            }
            case 5: {
                int16_t v = (int16_t)(int8_t)R8(0) * (int8_t)rm_rd8();
                R16(AX) = (uint16_t)v;
                cpu.flags &= ~(F_CF | F_OF);
                if (v != (int8_t)v) cpu.flags |= F_CF | F_OF;
                break;
            }
            case 6: {
                uint8_t d = rm_rd8();
                if (!d || R16(AX) / d > 0xFF) { divide_error(start_ip); break; }
                uint16_t a = R16(AX);
                R8(0) = (uint8_t)(a / d); R8(4) = (uint8_t)(a % d);
                break;
            }
            case 7: {
                int8_t d = (int8_t)rm_rd8();
                int16_t a = (int16_t)R16(AX);
                if (!d) { divide_error(start_ip); break; }
                int q = a / d;
                if (q > 127 || q < -128) { divide_error(start_ip); break; }
                R8(0) = (uint8_t)q; R8(4) = (uint8_t)(a % d);
                break;
            }
            }
            break;
        }
        case 0xF7: {
            decode_modrm();
            switch (reg_f) {
            case 0: case 1: { uint16_t v = rm_rd16(); alu16(4, v, fetch16()); break; }
            case 2: rm_wr16(~rm_rd16()); break;
            case 3: { uint16_t v = rm_rd16(); rm_wr16(alu16(5, 0, v)); break; }
            case 4: {
                uint32_t v = (uint32_t)R16(AX) * rm_rd16();
                R16(AX) = (uint16_t)v; R16(DX) = (uint16_t)(v >> 16);
                cpu.flags &= ~(F_CF | F_OF);
                if (R16(DX)) cpu.flags |= F_CF | F_OF;
                break;
            }
            case 5: {
                int32_t v = (int32_t)(int16_t)R16(AX) * (int16_t)rm_rd16();
                R16(AX) = (uint16_t)v; R16(DX) = (uint16_t)((uint32_t)v >> 16);
                cpu.flags &= ~(F_CF | F_OF);
                if (v != (int16_t)v) cpu.flags |= F_CF | F_OF;
                break;
            }
            case 6: {
                uint16_t d = rm_rd16();
                uint32_t a = (uint32_t)R16(DX) << 16 | R16(AX);
                if (!d || a / d > 0xFFFF) { divide_error(start_ip); break; }
                R16(AX) = (uint16_t)(a / d); R16(DX) = (uint16_t)(a % d);
                break;
            }
            case 7: {
                int16_t d = (int16_t)rm_rd16();
                int32_t a = (int32_t)((uint32_t)R16(DX) << 16 | R16(AX));
                if (!d) { divide_error(start_ip); break; }
                int32_t q = a / d;
                if (q > 32767 || q < -32768) { divide_error(start_ip); break; }
                R16(AX) = (uint16_t)q; R16(DX) = (uint16_t)(a % d);
                break;
            }
            }
            break;
        }
        case 0xF8: cpu.flags &= ~F_CF; break;
        case 0xF9: cpu.flags |= F_CF; break;
        case 0xFA: cpu.flags &= ~F_IF; break;
        case 0xFB: cpu.flags |= F_IF; break;
        case 0xFC: cpu.flags &= ~F_DF; break;
        case 0xFD: cpu.flags |= F_DF; break;
        case 0xFE:
            decode_modrm();
            if (reg_f == 0) rm_wr8(inc8(rm_rd8()));
            else if (reg_f == 1) rm_wr8(dec8(rm_rd8()));
            else log_msg("bad FE/%d at %04X:%04X\n", reg_f, cpu.seg[CS], start_ip);
            break;
        case 0xFF: {
            decode_modrm();
            switch (reg_f) {
            case 0: rm_wr16(inc16(rm_rd16())); break;
            case 1: rm_wr16(dec16(rm_rd16())); break;
            case 2: { uint16_t t = rm_rd16(); push16(cpu.ip); cpu.ip = t; break; }
            case 3: {
                uint16_t o = rd16(ea_seg, ea_off), s = rd16(ea_seg, (uint16_t)(ea_off + 2));
                push16(cpu.seg[CS]); push16(cpu.ip);
                cpu.ip = o; cpu.seg[CS] = s;
                break;
            }
            case 4: cpu.ip = rm_rd16(); break;
            case 5: {
                uint16_t o = rd16(ea_seg, ea_off), s = rd16(ea_seg, (uint16_t)(ea_off + 2));
                cpu.ip = o; cpu.seg[CS] = s;
                break;
            }
            case 6: { uint16_t v = rm_rd16(); push16(v); break; }
            default: log_msg("bad FF/7 at %04X:%04X\n", cpu.seg[CS], start_ip); break;
            }
            break;
        }
        default:
            log_msg("unimplemented opcode %02X at %04X:%04X\n", op, cpu.seg[CS], start_ip);
            cpu.ip = start_ip;
            cpu_interrupt(6);
            break;
        }
    }
    return used;
}
