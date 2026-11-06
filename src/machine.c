// PC platform: program loader, PIC/PIT/keyboard ports, and DOS/BIOS/mouse services.
#include "machine.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PSP_SEG 0x0100
#define STUB_SEG 0xF000
#define STUB_INT(n) (0x0100 + (n))
#define STUB_MOUSE_RET 0x0400

int machine_exit_requested;
uint32_t machine_load_seg;
static char game_root[MAX_PATH];
static uint64_t tick_count;
static FILE *log_file;

void log_msg(const char *fmt, ...) {
    if (!log_file) log_file = fopen("carrier.log", "w");
    if (!log_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fflush(log_file);
}

// ---- PIC / PIT ---------------------------------------------------------------
static uint8_t pic_mask, pic_pending, pic_in_service;
static uint32_t pit_reload[3] = {0x10000, 0x10000, 0x10000};
static uint32_t pit_lobyte_pending[3];
static int pit_mode_rw[3] = {3, 3, 3};
static int pit_flip[3];
static uint8_t pit_latch_active[3];
static uint16_t pit_latch[3];
static int pit_latch_flip[3];
static uint64_t pit_phase[3]; // in 1/44100-tick fractional PIT clocks
static uint64_t pit0_accum;
static uint8_t port61;
static int speaker_out, speaker_ch2_level;
static int32_t audio_lp;

#define PIT_HZ 1193182ULL
#define TICK_HZ 44100ULL

static uint16_t pit_current(int ch) {
    uint32_t reload = pit_reload[ch] ? pit_reload[ch] : 0x10000;
    uint64_t clocks = tick_count * PIT_HZ / TICK_HZ;
    uint32_t cnt = reload - (uint32_t)(clocks % reload);
    return (uint16_t)cnt;
}

void machine_tick(void) {
    tick_count++;
    vga_retrace_tick();
    pit0_accum += PIT_HZ;
    uint64_t period = (uint64_t)(pit_reload[0] ? pit_reload[0] : 0x10000) * TICK_HZ;
    if (pit0_accum >= period) {
        pit0_accum -= period;
        pic_pending |= 1;
    }
    // Speaker: gate on bit 0 follows PIT ch2 square wave; otherwise bit 1 is direct.
    if ((port61 & 3) == 3) {
        uint32_t reload = pit_reload[2] ? pit_reload[2] : 0x10000;
        uint64_t clocks = tick_count * PIT_HZ / TICK_HZ;
        speaker_ch2_level = (clocks % reload) < reload / 2;
        speaker_out = speaker_ch2_level;
    } else {
        speaker_out = (port61 & 2) != 0;
    }
}

int16_t machine_audio_sample(void) {
    int32_t target = speaker_out ? 9000 : -9000;
    if (!(port61 & 3)) target = 0;
    audio_lp += (target - audio_lp) / 4; // takes the edge off the square wave
    // DC blocker: the game can leave the speaker bit parked high, which must stay silent.
    static int32_t last_in, out;
    out = audio_lp - last_in + out - out / 256;
    last_in = audio_lp;
    return (int16_t)out;
}

void machine_kb_latch(void);
void machine_check_irq(void) {
    if (!(cpu.flags & F_IF) && !cpu.halted) return;
    if (pic_in_service) return;
    uint8_t ready = pic_pending & ~pic_mask;
    if (!ready) return;
    int irq = (ready & 1) ? 0 : (ready & 2) ? 1 : (ready & 16) ? 4 : -1;
    if (irq < 0) return;
    pic_pending &= ~(1 << irq);
    if (irq == 1) machine_kb_latch();
    pic_in_service = 1 << irq;
    cpu.halted = 0;
    cpu_interrupt(8 + irq);
}

// ---- Keyboard ----------------------------------------------------------------
static uint8_t kb_queue[64];
static int kb_head, kb_tail;
static uint8_t kb_port60;
static uint16_t bios_keys[32];
static int bios_kh, bios_kt;

static void kb_push(uint8_t sc) {
    int n = (kb_tail + 1) % 64;
    if (n != kb_head) { kb_queue[kb_tail] = sc; kb_tail = n; }
    pic_pending |= 2;
}
static void kb_service_next(void) {
    if (kb_head != kb_tail) {
        kb_port60 = kb_queue[kb_head];
        kb_head = (kb_head + 1) % 64;
    }
}
static uint8_t scancode_ascii[256];
void machine_key(int scancode, int down, int ascii) {
    if (ascii) scancode_ascii[scancode & 0x7F] = (uint8_t)ascii;
    kb_push((uint8_t)(down ? scancode : scancode | 0x80));
}

// ---- Mouse (INT 33h) ---------------------------------------------------------
static int mouse_x, mouse_y, mouse_mx, mouse_my, mouse_buttons;
static uint16_t mouse_cb_mask, mouse_cb_seg, mouse_cb_off;
static int mouse_events, mouse_in_cb;
static uint16_t mouse_saved[13];
static int mouse_xmin, mouse_xmax = 639, mouse_ymin, mouse_ymax = 199;

void machine_mouse_move(int dx, int dy) {
    mouse_mx += dx;
    mouse_my += dy;
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < mouse_xmin) mouse_x = mouse_xmin;
    if (mouse_x > mouse_xmax) mouse_x = mouse_xmax;
    if (mouse_y < mouse_ymin) mouse_y = mouse_ymin;
    if (mouse_y > mouse_ymax) mouse_y = mouse_ymax;
    if (dx || dy) mouse_events |= 1;
}
void machine_mouse_button(int button, int down) {
    int bit = 1 << button;
    if (down) { mouse_buttons |= bit; mouse_events |= button == 0 ? 2 : 8; }
    else { mouse_buttons &= ~bit; mouse_events |= button == 0 ? 4 : 16; }
}

static void mouse_dispatch_callback(void) {
    if (mouse_in_cb || !mouse_cb_mask || !(cpu.flags & F_IF)) return;
    int ev = mouse_events & mouse_cb_mask;
    if (!ev) return;
    mouse_events &= ~mouse_cb_mask;
    for (int i = 0; i < 8; i++) mouse_saved[i] = cpu.r.w[i];
    for (int i = 0; i < 4; i++) mouse_saved[8 + i] = cpu.seg[i];
    mouse_saved[12] = cpu.flags;
    mouse_in_cb = 1;
    // Push a far return to the mouse-return stub, plus the interrupted CS:IP.
    cpu.r.w[SP] -= 2; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP]] = (uint8_t)cpu.seg[CS]; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 1] = (uint8_t)(cpu.seg[CS] >> 8);
    cpu.r.w[SP] -= 2; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP]] = (uint8_t)cpu.ip; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 1] = (uint8_t)(cpu.ip >> 8);
    cpu.r.w[SP] -= 2; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP]] = (uint8_t)STUB_SEG; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 1] = (uint8_t)(STUB_SEG >> 8);
    cpu.r.w[SP] -= 2; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP]] = (uint8_t)STUB_MOUSE_RET; mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 1] = (uint8_t)(STUB_MOUSE_RET >> 8);
    cpu.r.w[AX] = (uint16_t)ev;
    cpu.r.w[BX] = (uint16_t)mouse_buttons;
    cpu.r.w[CX] = (uint16_t)mouse_x;
    cpu.r.w[DX] = (uint16_t)mouse_y;
    cpu.r.w[SI] = (uint16_t)mouse_mx;
    cpu.r.w[DI] = (uint16_t)mouse_my;
    cpu.seg[CS] = mouse_cb_seg;
    cpu.ip = mouse_cb_off;
}

static void mouse_return_stub(void) {
    uint32_t sp = ((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP];
    uint16_t ip = mem[sp] | mem[sp + 1] << 8;
    uint16_t cs = mem[sp + 2] | mem[sp + 3] << 8;
    for (int i = 0; i < 8; i++) cpu.r.w[i] = mouse_saved[i];
    for (int i = 0; i < 4; i++) cpu.seg[i] = mouse_saved[8 + i];
    cpu.flags = mouse_saved[12];
    cpu.r.w[SP] = mouse_saved[SP];
    cpu.seg[CS] = cs;
    cpu.ip = ip;
    mouse_in_cb = 0;
}

static void int33(void) {
    switch (cpu.r.w[AX]) {
    case 0x00:
        cpu.r.w[AX] = 0xFFFF;
        cpu.r.w[BX] = 2;
        mouse_cb_mask = 0;
        mouse_x = 320; mouse_y = 100;
        break;
    case 0x03:
        cpu.r.w[BX] = (uint16_t)mouse_buttons;
        cpu.r.w[CX] = (uint16_t)mouse_x;
        cpu.r.w[DX] = (uint16_t)mouse_y;
        break;
    case 0x04: mouse_x = cpu.r.w[CX]; mouse_y = cpu.r.w[DX]; break;
    case 0x07: mouse_xmin = cpu.r.w[CX]; mouse_xmax = cpu.r.w[DX]; break;
    case 0x08: mouse_ymin = cpu.r.w[CX]; mouse_ymax = cpu.r.w[DX]; break;
    case 0x0B:
        cpu.r.w[CX] = (uint16_t)mouse_mx;
        cpu.r.w[DX] = (uint16_t)mouse_my;
        mouse_mx = mouse_my = 0;
        break;
    case 0x0C:
        mouse_cb_mask = cpu.r.w[CX];
        mouse_cb_off = cpu.r.w[DX];
        mouse_cb_seg = cpu.seg[ES];
        break;
    default: break;
    }
}

// ---- DOS (INT 21h) -----------------------------------------------------------
#define MAX_FILES 32
static FILE *files[MAX_FILES];
static uint32_t dta_lin;
static HANDLE find_handle = INVALID_HANDLE_VALUE;
static uint8_t find_attr;
static uint16_t mem_next_seg;

static void set_cf(int on) {
    // The caller's FLAGS sit on the stack under the IRET frame.
    uint32_t a = (((uint32_t)cpu.seg[SS] << 4) + (uint16_t)(cpu.r.w[SP] + 4)) & 0xFFFFF;
    if (on) mem[a] |= 1; else mem[a] &= ~1;
}
static void set_zf_stack(int on) {
    uint32_t a = (((uint32_t)cpu.seg[SS] << 4) + (uint16_t)(cpu.r.w[SP] + 4)) & 0xFFFFF;
    if (on) mem[a] |= F_ZF; else mem[a] &= ~F_ZF;
}
static void dos_err(int code) { cpu.r.w[AX] = (uint16_t)code; set_cf(1); }

static void read_dos_string(uint16_t seg, uint16_t off, char *out, int n) {
    int i = 0;
    while (i < n - 1) {
        char c = (char)mem[(((uint32_t)seg << 4) + (uint16_t)(off + i)) & 0xFFFFF];
        if (!c) break;
        out[i++] = c;
    }
    out[i] = 0;
}
// Maps any DOS path onto the game folder, dropping drive and leading separators.
static void host_path(const char *dos, char *out, size_t n) {
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    snprintf(out, n, "%s\\%s", game_root, p);
}

static void fill_dta(const WIN32_FIND_DATAA *fd) {
    uint8_t *d = &mem[dta_lin];
    d[0x15] = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20;
    memset(d + 0x16, 0, 4);
    uint32_t sz = fd->nFileSizeLow;
    d[0x1A] = (uint8_t)sz; d[0x1B] = (uint8_t)(sz >> 8); d[0x1C] = (uint8_t)(sz >> 16); d[0x1D] = (uint8_t)(sz >> 24);
    char nm[14];
    const char *src = fd->cAlternateFileName[0] ? fd->cAlternateFileName : fd->cFileName;
    int i = 0;
    for (; src[i] && i < 12; i++) nm[i] = (char)toupper((unsigned char)src[i]);
    nm[i] = 0;
    memcpy(d + 0x1E, nm, (size_t)i + 1);
}

static void int21(void) {
    uint8_t ah = cpu.r.b[1];
    uint8_t al = cpu.r.b[0];
    char dos[128], host[MAX_PATH];
    switch (ah) {
    case 0x02: log_msg("%c", cpu.r.b[4]); break;
    case 0x06: case 0x07: case 0x08: cpu.r.b[0] = 0; set_zf_stack(1); break;
    case 0x09: {
        uint32_t a = ((uint32_t)cpu.seg[DS] << 4) + cpu.r.w[DX];
        for (int i = 0; i < 2000 && mem[a + i] != '$'; i++) log_msg("%c", mem[a + i]);
        break;
    }
    case 0x0B: cpu.r.b[0] = 0; break;
    case 0x0E: cpu.r.b[0] = 26; break;
    case 0x19: cpu.r.b[0] = 2; break;
    case 0x1A: dta_lin = ((uint32_t)cpu.seg[DS] << 4) + cpu.r.w[DX]; break;
    case 0x25: {
        uint32_t v = (uint32_t)al * 4;
        mem[v] = cpu.r.b[4]; mem[v + 1] = cpu.r.b[5];
        mem[v + 2] = (uint8_t)cpu.seg[DS]; mem[v + 3] = (uint8_t)(cpu.seg[DS] >> 8);
        break;
    }
    case 0x2A: {
        SYSTEMTIME t; GetLocalTime(&t);
        cpu.r.w[CX] = t.wYear; cpu.r.b[5] = (uint8_t)t.wMonth; cpu.r.b[4] = (uint8_t)t.wDay; cpu.r.b[0] = (uint8_t)t.wDayOfWeek;
        break;
    }
    case 0x2C: {
        SYSTEMTIME t; GetLocalTime(&t);
        cpu.r.b[5] = (uint8_t)t.wHour; cpu.r.b[1] = (uint8_t)t.wMinute; cpu.r.b[6] = (uint8_t)t.wSecond; cpu.r.b[4] = (uint8_t)(t.wMilliseconds / 10);
        break;
    }
    case 0x2F: {
        cpu.seg[ES] = (uint16_t)(dta_lin >> 4);
        cpu.r.w[BX] = (uint16_t)(dta_lin & 15);
        break;
    }
    case 0x30: cpu.r.w[AX] = 0x1606; cpu.r.w[BX] = 0; cpu.r.w[CX] = 0; break; // DOS 6.22
    case 0x35: {
        uint32_t v = (uint32_t)al * 4;
        cpu.r.w[BX] = mem[v] | mem[v + 1] << 8;
        cpu.seg[ES] = mem[v + 2] | mem[v + 3] << 8;
        break;
    }
    case 0x36: cpu.r.w[AX] = 32; cpu.r.w[BX] = 0x7000; cpu.r.w[CX] = 512; cpu.r.w[DX] = 0x7FFF; break;
    case 0x3B: set_cf(0); break;
    case 0x3C: case 0x3D: {
        read_dos_string(cpu.seg[DS], cpu.r.w[DX], dos, sizeof dos);
        host_path(dos, host, sizeof host);
        int h;
        for (h = 5; h < MAX_FILES && files[h]; h++) {}
        if (h >= MAX_FILES) { dos_err(4); break; }
        const char *m = ah == 0x3C ? "w+b" : (al & 3) == 0 ? "rb" : "r+b";
        files[h] = fopen(host, m);
        log_msg("open %s (%s) -> %s\n", dos, m, files[h] ? "ok" : "FAILED");
        if (!files[h]) { dos_err(2); break; }
        cpu.r.w[AX] = (uint16_t)h;
        set_cf(0);
        break;
    }
    case 0x3E: {
        int h = cpu.r.w[BX];
        if (h >= 5 && h < MAX_FILES && files[h]) { fclose(files[h]); files[h] = NULL; }
        set_cf(0);
        break;
    }
    case 0x3F: {
        int h = cpu.r.w[BX];
        if (h < 5 || h >= MAX_FILES || !files[h]) { cpu.r.w[AX] = 0; set_cf(h >= 5); break; }
        uint32_t a = ((uint32_t)cpu.seg[DS] << 4) + cpu.r.w[DX];
        cpu.r.w[AX] = (uint16_t)fread(&mem[a], 1, cpu.r.w[CX], files[h]);
        set_cf(0);
        break;
    }
    case 0x40: {
        int h = cpu.r.w[BX];
        uint32_t a = ((uint32_t)cpu.seg[DS] << 4) + cpu.r.w[DX];
        if (h == 1 || h == 2) {
            for (int i = 0; i < cpu.r.w[CX]; i++) log_msg("%c", mem[a + i]);
            cpu.r.w[AX] = cpu.r.w[CX];
            set_cf(0);
            break;
        }
        if (h < 5 || h >= MAX_FILES || !files[h]) { dos_err(6); break; }
        cpu.r.w[AX] = (uint16_t)fwrite(&mem[a], 1, cpu.r.w[CX], files[h]);
        fflush(files[h]);
        set_cf(0);
        break;
    }
    case 0x41:
        read_dos_string(cpu.seg[DS], cpu.r.w[DX], dos, sizeof dos);
        host_path(dos, host, sizeof host);
        if (remove(host) == 0) set_cf(0); else dos_err(2);
        break;
    case 0x42: {
        int h = cpu.r.w[BX];
        if (h < 5 || h >= MAX_FILES || !files[h]) { dos_err(6); break; }
        long off = (long)((uint32_t)cpu.r.w[CX] << 16 | cpu.r.w[DX]);
        fseek(files[h], off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END);
        long pos = ftell(files[h]);
        cpu.r.w[AX] = (uint16_t)pos; cpu.r.w[DX] = (uint16_t)(pos >> 16);
        set_cf(0);
        break;
    }
    case 0x43:
        read_dos_string(cpu.seg[DS], cpu.r.w[DX], dos, sizeof dos);
        host_path(dos, host, sizeof host);
        if (GetFileAttributesA(host) == INVALID_FILE_ATTRIBUTES) dos_err(2);
        else { cpu.r.w[CX] = 0x20; set_cf(0); }
        break;
    case 0x44: // IOCTL: 0Eh/0Fh query the logical drive map, and we have just one drive
        if (al == 0x0E || al == 0x0F) cpu.r.b[0] = 0;
        else cpu.r.w[DX] = 0;
        set_cf(0);
        break;
    case 0x47: mem[((uint32_t)cpu.seg[DS] << 4) + cpu.r.w[SI]] = 0; set_cf(0); break;
    case 0x48: {
        uint16_t need = cpu.r.w[BX];
        if ((uint32_t)mem_next_seg + need > 0x9FFF) { cpu.r.w[BX] = (uint16_t)(0x9FFF - mem_next_seg); dos_err(8); break; }
        cpu.r.w[AX] = mem_next_seg;
        mem_next_seg += need;
        set_cf(0);
        break;
    }
    case 0x49: case 0x4A: set_cf(0); break;
    case 0x4C: log_msg("program exit, code %d\n", al); machine_exit_requested = 1; cpu.halted = 1; break;
    case 0x4E: case 0x4F: {
        WIN32_FIND_DATAA fd;
        BOOL ok;
        if (ah == 0x4E) {
            read_dos_string(cpu.seg[DS], cpu.r.w[DX], dos, sizeof dos);
            host_path(dos, host, sizeof host);
            find_attr = (uint8_t)cpu.r.w[CX];
            if (find_handle != INVALID_HANDLE_VALUE) FindClose(find_handle);
            find_handle = FindFirstFileA(host, &fd);
            ok = find_handle != INVALID_HANDLE_VALUE;
        } else {
            ok = find_handle != INVALID_HANDLE_VALUE && FindNextFileA(find_handle, &fd);
        }
        while (ok && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(find_attr & 0x10))
            ok = FindNextFileA(find_handle, &fd);
        if (!ok) { dos_err(18); break; }
        fill_dta(&fd);
        set_cf(0);
        break;
    }
    case 0x56: dos_err(5); break;
    case 0x62: cpu.r.w[BX] = PSP_SEG; break;
    default:
        log_msg("unhandled INT 21 AH=%02X AL=%02X at %04X:%04X\n", ah, al,
                mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 2] | mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 3] << 8,
                mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP]] | mem[((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP] + 1] << 8);
        set_cf(1);
        break;
    }
}

// ---- BIOS --------------------------------------------------------------------
static void int10(void) {
    uint8_t ah = cpu.r.b[1], al = cpu.r.b[0];
    switch (ah) {
    case 0x00:
        log_msg("set video mode %02X\n", al);
        vga_set_mode(al);
        break;
    case 0x05: break;
    case 0x0F:
        cpu.r.b[0] = (uint8_t)vga_get_mode();
        cpu.r.b[1] = vga_get_mode() == 3 ? 80 : 40;
        cpu.r.b[7] = 0;
        break;
    case 0x0E: log_msg("%c", al); break;
    case 0x10:
        switch (al) {
        case 0x00: vga_set_palette_reg(cpu.r.b[3], cpu.r.b[7]); break;
        case 0x02: {
            uint32_t a = ((uint32_t)cpu.seg[ES] << 4) + cpu.r.w[DX];
            for (int i = 0; i < 16; i++) vga_set_palette_reg(i, mem[a + i]);
            break;
        }
        case 0x10: vga_set_dac(cpu.r.w[BX], cpu.r.b[5], cpu.r.b[2], cpu.r.b[3]); break;
        case 0x12: {
            uint32_t a = ((uint32_t)cpu.seg[ES] << 4) + cpu.r.w[DX];
            for (int i = 0; i < cpu.r.w[CX]; i++)
                vga_set_dac(cpu.r.w[BX] + i, mem[a + i * 3], mem[a + i * 3 + 1], mem[a + i * 3 + 2]);
            break;
        }
        default: break;
        }
        break;
    case 0x12:
        if (cpu.r.b[3] == 0x10) { cpu.r.w[BX] = 0x0003; cpu.r.w[CX] = 0x0009; }
        break;
    case 0x1A:
        cpu.r.b[0] = 0x1A;
        cpu.r.b[3] = 8; // VGA colour
        cpu.r.b[7] = 0;
        break;
    default:
        log_msg("unhandled INT 10 AX=%04X\n", cpu.r.w[AX]);
        break;
    }
}

static void int16(void) {
    uint8_t ah = cpu.r.b[1];
    switch (ah) {
    case 0x00: case 0x10:
        if (bios_kh == bios_kt) {
            // Block: re-execute the INT once time has advanced.
            uint32_t sp = ((uint32_t)cpu.seg[SS] << 4) + cpu.r.w[SP];
            uint16_t ret = mem[sp] | mem[sp + 1] << 8;
            ret -= 2;
            mem[sp] = (uint8_t)ret; mem[sp + 1] = (uint8_t)(ret >> 8);
            mem[sp + 5] |= F_IF >> 8;
            cpu.flags |= F_IF;
            return;
        }
        cpu.r.w[AX] = bios_keys[bios_kh];
        bios_kh = (bios_kh + 1) % 32;
        break;
    case 0x01: case 0x11:
        if (bios_kh == bios_kt) set_zf_stack(1);
        else { set_zf_stack(0); cpu.r.w[AX] = bios_keys[bios_kh]; }
        break;
    case 0x02: cpu.r.b[0] = 0; break;
    }
}

static void bios_int9(void) {
    uint8_t sc = port_in(0x60);
    if (!(sc & 0x80)) {
        int n = (bios_kt + 1) % 32;
        if (n != bios_kh) {
            bios_keys[bios_kt] = (uint16_t)(sc << 8 | scancode_ascii[sc]);
            bios_kt = n;
        }
    }
    port_out(0x20, 0x20);
}

static void bios_int8(void) {
    uint32_t t = mem[0x46C] | mem[0x46D] << 8 | mem[0x46E] << 16 | (uint32_t)mem[0x46F] << 24;
    t++;
    mem[0x46C] = (uint8_t)t; mem[0x46D] = (uint8_t)(t >> 8); mem[0x46E] = (uint8_t)(t >> 16); mem[0x46F] = (uint8_t)(t >> 24);
    port_out(0x20, 0x20);
}

static void int1a(void) {
    if (cpu.r.b[1] == 0) {
        cpu.r.w[CX] = mem[0x46E] | mem[0x46F] << 8;
        cpu.r.w[DX] = mem[0x46C] | mem[0x46D] << 8;
        cpu.r.b[0] = 0;
    } else if (cpu.r.b[1] == 1) {
        mem[0x46C] = cpu.r.b[4]; mem[0x46D] = cpu.r.b[5];
        mem[0x46E] = cpu.r.b[2]; mem[0x46F] = cpu.r.b[3];
    }
}

void hook_dispatch(uint32_t lin) {
    if (hookmap[lin] == 2) {
        if (hires_hook(lin)) return;
    }
    uint32_t st = (uint32_t)STUB_SEG << 4;
    if (lin == st + STUB_MOUSE_RET) { mouse_return_stub(); return; }
    if (lin >= st + STUB_INT(0) && lin < st + STUB_INT(256)) {
        int n = (int)(lin - st - STUB_INT(0));
        switch (n) {
        case 0x08: bios_int8(); break;
        case 0x09: bios_int9(); break;
        case 0x10: int10(); break;
        case 0x11: cpu.r.w[AX] = 0x0023; break; // equipment word: colour, floppy
        case 0x12: cpu.r.w[AX] = 640; break;
        case 0x16: int16(); break;
        case 0x1A: int1a(); break;
        case 0x21: int21(); break;
        case 0x33: int33(); break;
        default: break;
        }
    }
}

// ---- Ports -------------------------------------------------------------------
uint8_t port_in(uint16_t port) {
    switch (port) {
    case 0x20: return pic_in_service;
    case 0x21: return pic_mask;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t v;
        if (pit_latch_active[ch]) {
            v = pit_latch[ch];
            if (pit_latch_flip[ch]++ == 1) { pit_latch_active[ch] = 0; pit_latch_flip[ch] = 0; }
            return pit_latch_flip[ch] == 1 ? (uint8_t)v : (uint8_t)(v >> 8);
        }
        v = pit_current(ch);
        pit_flip[ch] ^= 1;
        return pit_flip[ch] ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    case 0x60: return kb_port60;
    case 0x61: {
        // Refresh toggle bit keeps timing-loop probes moving.
        static uint8_t toggle;
        toggle ^= 0x10;
        return (uint8_t)((port61 & 0x0F) | toggle | (speaker_out ? 0x20 : 0));
    }
    case 0x201: return 0xFF;
    }
    if (port >= 0x3B0 && port <= 0x3DF) return vga_port_in(port);
    return 0xFF;
}

void port_out(uint16_t port, uint8_t val) {
    switch (port) {
    case 0x20:
        if (val == 0x20) {
            pic_in_service = 0;
            if (kb_head != kb_tail) pic_pending |= 2;
        }
        return;
    case 0x21: pic_mask = val; return;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        int rw = pit_mode_rw[ch];
        if (rw == 1) pit_reload[ch] = val ? val : 0x10000;
        else if (rw == 2) pit_reload[ch] = val ? (uint32_t)val << 8 : 0x10000;
        else if (!pit_flip[ch]) { pit_lobyte_pending[ch] = val; pit_flip[ch] = 1; }
        else {
            uint32_t r = pit_lobyte_pending[ch] | (uint32_t)val << 8;
            pit_reload[ch] = r ? r : 0x10000;
            pit_flip[ch] = 0;
        }
        return;
    }
    case 0x43: {
        int ch = val >> 6;
        if (ch == 3) return;
        int rw = (val >> 4) & 3;
        if (rw == 0) {
            pit_latch[ch] = pit_current(ch);
            pit_latch_active[ch] = 1;
            pit_latch_flip[ch] = 0;
        } else {
            pit_mode_rw[ch] = rw;
            pit_flip[ch] = 0;
        }
        return;
    }
    case 0x61: port61 = val; return;
    }
    if (port >= 0x3B0 && port <= 0x3DF) vga_port_out(port, val);
    else if (port != 0x80) {
        static int reported[0x400];
        if (port < 0x400 && !reported[port]) { reported[port] = 1; log_msg("out to unknown port %03X\n", port); }
    }
}

// ---- Loader ------------------------------------------------------------------
void machine_init(const char *game_dir, const char *cmdline) {
    strncpy(game_root, game_dir, sizeof game_root - 1);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\CARRIER.EXE", game_root);
    FILE *f = fopen(path, "rb");
    if (!f) { log_msg("cannot open %s\n", path); machine_exit_requested = 1; return; }
    static uint8_t exe[0x80000];
    size_t len = fread(exe, 1, sizeof exe, f);
    fclose(f);
#define W(o) ((uint16_t)(exe[o] | exe[(o) + 1] << 8))
    uint32_t img_len = (W(4) - 1) * 512u + W(2);
    uint32_t hdr = W(8) * 16u;
    if (len < img_len) img_len = (uint32_t)len;
    uint32_t load = PSP_SEG + 0x10;
    machine_load_seg = load;
    memcpy(&mem[load * 16], exe + hdr, img_len - hdr);
    for (int i = 0; i < W(6); i++) {
        uint32_t r = W(24) + i * 4u;
        uint32_t lin = load * 16 + W(r + 2) * 16u + W(r);
        uint16_t v = (uint16_t)(mem[lin] | mem[lin + 1] << 8);
        v = (uint16_t)(v + load);
        mem[lin] = (uint8_t)v; mem[lin + 1] = (uint8_t)(v >> 8);
    }
    // PSP.
    uint8_t *psp = &mem[PSP_SEG * 16];
    psp[0] = 0xCD; psp[1] = 0x20;
    psp[2] = 0x00; psp[3] = 0xA0; // top of memory
    size_t cl = strlen(cmdline);
    if (cl > 120) cl = 120;
    psp[0x80] = (uint8_t)cl;
    memcpy(psp + 0x81, cmdline, cl);
    psp[0x81 + cl] = 0x0D;
    dta_lin = PSP_SEG * 16 + 0x80;

    // IVT stubs: every vector lands on an IRET with a hook for the services we provide.
    uint32_t st = (uint32_t)STUB_SEG << 4;
    for (int n = 0; n < 256; n++) {
        uint16_t off = (uint16_t)STUB_INT(n);
        mem[n * 4] = (uint8_t)off; mem[n * 4 + 1] = (uint8_t)(off >> 8);
        mem[n * 4 + 2] = (uint8_t)STUB_SEG; mem[n * 4 + 3] = (uint8_t)(STUB_SEG >> 8);
        mem[st + off] = 0xCF;
        hookmap[st + off] = 1;
    }
    mem[st + STUB_MOUSE_RET] = 0xCB;
    hookmap[st + STUB_MOUSE_RET] = 1;
    // BIOS data area and ROM ID.
    mem[0x410] = 0x63;
    mem[0x449] = 3;
    mem[0x44A] = 80;
    mem[0x463] = 0xD4; mem[0x464] = 0x03;
    mem[0xFFFFE] = 0xFC;

    vga_set_mode(3);
    pic_mask = 0;
    port61 = 0;

    memset(&cpu, 0, sizeof cpu);
    cpu.seg[CS] = (uint16_t)(load + W(22));
    cpu.ip = W(20);
    cpu.seg[SS] = (uint16_t)(load + W(14));
    cpu.r.w[SP] = W(16);
    cpu.seg[DS] = cpu.seg[ES] = PSP_SEG;
    cpu.flags = 2 | F_IF;
    uint32_t minalloc = W(10);
    mem_next_seg = (uint16_t)(load + (img_len - hdr) / 16 + minalloc + 1);
    log_msg("loaded %u bytes at %04X, entry %04X:%04X\n", img_len - hdr, load, cpu.seg[CS], cpu.ip);
    (void)pit_phase;
}

void machine_service_mouse(void);
void machine_service_mouse(void) { mouse_dispatch_callback(); }

// Called when the keyboard IRQ is delivered: makes the next scancode readable.
void machine_kb_latch(void);
void machine_kb_latch(void) { kb_service_next(); }
