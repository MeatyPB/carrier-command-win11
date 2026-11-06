// Shared state for the Carrier Command native runtime.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };
enum { F_CF = 1, F_PF = 4, F_AF = 16, F_ZF = 64, F_SF = 128, F_TF = 256, F_IF = 512, F_DF = 1024, F_OF = 2048 };

typedef struct {
    union {
        uint16_t w[8];
        uint8_t b[16];
    } r;
    uint16_t seg[4];
    uint16_t ip;
    uint16_t flags;
    int halted;
} Cpu;

extern Cpu cpu;
extern uint8_t mem[0x110000];
extern uint8_t hookmap[0x100000]; // nonzero: hook_dispatch runs before the instruction at this address

// cpu.c
int cpu_run(int budget); // returns instructions consumed
void cpu_interrupt(int n);

// machine.c
void hook_dispatch(uint32_t lin);
uint8_t port_in(uint16_t port);
void port_out(uint16_t port, uint8_t val);
void machine_init(const char *game_dir, const char *cmdline);
void machine_tick(void);     // one audio-rate tick of virtual time
void machine_check_irq(void);
void machine_mouse_move(int dx, int dy);
void machine_mouse_button(int button, int down);
void machine_key(int scancode, int down, int ascii);
extern int machine_exit_requested;
extern uint32_t machine_load_seg; // paragraph the EXE image was loaded at
int16_t machine_audio_sample(void);
void log_msg(const char *fmt, ...);

// vga.c
uint8_t vga_read(uint32_t off);
void vga_write(uint32_t off, uint8_t val);
uint8_t vga_port_in(uint16_t port);
void vga_port_out(uint16_t port, uint8_t val);
void vga_set_mode(int mode);
void vga_set_palette_reg(int idx, uint8_t val);
void vga_set_dac(int idx, uint8_t r, uint8_t g, uint8_t b);
int vga_get_mode(void);
uint16_t vga_start_address(void);
int vga_planar_active(void);
void vga_render_lowres(uint32_t *out); // 320x200 ARGB of the displayed page
const uint32_t *vga_palette_argb(void); // 16 ARGB entries through the attribute controller
extern uint8_t vga_planes[4][0x10000];
extern uint32_t vga_frame_tick;
void vga_retrace_tick(void);
uint8_t vga_status_now(void);

// textures.c
enum {
    TEX_TERRAIN, TEX_TERRAIN_FINE, TEX_ROCK, TEX_SAND, TEX_CONCRETE, TEX_DECK, TEX_NONSKID, TEX_HULL,
    TEX_METAL, TEX_PAINT, TEX_PANEL, TEX_WAVES, TEX_CLOUDS, TEX_COUNT
};
enum { TEX_LEVELS = 6 };
void tex_init(void);
uint8_t tex_sample(int tile, int level, int32_t u, int32_t v);
int tex_level_for(float texels_per_pixel);

// hires.c
extern int hires_scale;
extern int hires_modern; // 0 shows the flat classic colours
void hires_init(int scale);
void hires_install_hooks(void);
int hires_hook(uint32_t lin);
void hires_note_cpu_write(uint32_t off, uint8_t mask);
void hires_note_latch_copy(uint32_t dst, uint32_t src, uint8_t mask);
void hires_render(uint32_t *out); // hires_scale*320 x hires_scale*200 ARGB
void hires_mode_set(void);
