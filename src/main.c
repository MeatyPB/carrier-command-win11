// Carrier Command for Windows 11: SDL2 front end and main loop.
#include "machine.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int hires_keep_dither;
void machine_service_mouse(void);
void machine_kb_latch(void);

// SDL scancode -> PC/XT set 1 scancode.
static int xt_code(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_END: case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_DELETE: case SDL_SCANCODE_KP_PERIOD: return 0x53;
    default: return 0;
    }
}

static int ascii_for(SDL_Scancode s) {
    SDL_Keycode k = SDL_GetKeyFromScancode(s);
    if (k >= 'a' && k <= 'z') return k;
    if (k >= 32 && k < 127) return k;
    if (s == SDL_SCANCODE_RETURN || s == SDL_SCANCODE_KP_ENTER) return 13;
    if (s == SDL_SCANCODE_ESCAPE) return 27;
    if (s == SDL_SCANCODE_BACKSPACE) return 8;
    if (s == SDL_SCANCODE_TAB) return 9;
    return 0;
}

// Minimal PNG writer (stored deflate) for verification screenshots.
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t b[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(b, 1, 4, f);
    fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(0xFFFFFFFFu, (const uint8_t *)t, 4);
    c = crc(c, d, n) ^ 0xFFFFFFFFu;
    uint8_t e[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(e, 1, 4, f);
}
static void write_png(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * w + x];
            raw[o++] = (uint8_t)(c >> 16); raw[o++] = (uint8_t)(c >> 8); raw[o++] = (uint8_t)c;
        }
    }
    size_t z_cap = raw_n + raw_n / 65535 * 5 + 16;
    uint8_t *z = (uint8_t *)malloc(z_cap);
    size_t zn = 0, left = raw_n, pos = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    while (left) {
        uint32_t n = left > 65535 ? 65535 : (uint32_t)left;
        z[zn++] = left == n;
        z[zn++] = (uint8_t)n; z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n; z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + pos, n);
        zn += n; pos += n; left -= n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = b << 16 | a;
    z[zn++] = (uint8_t)(ad >> 24); z[zn++] = (uint8_t)(ad >> 16); z[zn++] = (uint8_t)(ad >> 8); z[zn++] = (uint8_t)ad;
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// Headless test script: lines of "<frame> key <xt-hex> <0|1> <ascii>", "<frame> mouse <dx> <dy>",
// "<frame> button <n> <0|1>", "<frame> shot <file>", "<frame> quit".
typedef struct { int frame; char kind; int a, b, c; char path[260]; } ScriptEv;
static ScriptEv script[4096];
static int script_n, script_pos;
static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof line, f) && script_n < 4096) {
        ScriptEv *e = &script[script_n];
        char kind[16] = {0};
        if (line[0] == '#' || sscanf(line, "%d %15s", &e->frame, kind) != 2) continue;
        char *rest = strstr(line, kind) + strlen(kind);
        if (!strcmp(kind, "key")) { e->kind = 'k'; sscanf(rest, "%x %d %d", &e->a, &e->b, &e->c); }
        else if (!strcmp(kind, "mouse")) { e->kind = 'm'; sscanf(rest, "%d %d", &e->a, &e->b); }
        else if (!strcmp(kind, "button")) { e->kind = 'b'; sscanf(rest, "%d %d", &e->a, &e->b); }
        else if (!strcmp(kind, "shot")) { e->kind = 's'; sscanf(rest, "%259s", e->path); }
        else if (!strcmp(kind, "toggle")) e->kind = 't';
        else if (!strcmp(kind, "quit")) e->kind = 'q';
        else continue;
        script_n++;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    static char default_game[1024];
    const char *game_dir = default_game;
    const char *script_path = NULL;
    const char *switches = " /V3 /I3 /C3";
    int scale = 6, headless = 0, fullscreen = 1, nograb = 0;
    double mips = 20.0, mouse_sens = 0.5;
    {
        char *base = SDL_GetBasePath();
        snprintf(default_game, sizeof default_game, "%sgame", base ? base : "");
        SDL_free(base);
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game_dir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_path = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mips") && i + 1 < argc) mips = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sens") && i + 1 < argc) mouse_sens = atof(argv[++i]);
        else if (!strcmp(argv[i], "--switches") && i + 1 < argc) switches = argv[++i];
        else if (!strcmp(argv[i], "--dither")) hires_keep_dither = 1;
        else if (!strcmp(argv[i], "--classic")) hires_modern = 0;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--windowed")) fullscreen = 0;
        else if (!strcmp(argv[i], "--nograb")) nograb = 1;
    }
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (script_path) load_script(script_path);

    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(headless ? SDL_INIT_TIMER : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) != 0) {
        log_msg("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    hires_init(scale);
    machine_init(game_dir, switches);
    hires_install_hooks();

    int fb_w = 320 * scale, fb_h = 200 * scale;
    uint32_t *fb = (uint32_t *)malloc((size_t)fb_w * fb_h * 4);
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    SDL_AudioDeviceID audio = 0;
    int grabbed = 0;
    if (!headless) {
        win = SDL_CreateWindow("Carrier Command", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                   (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        SDL_RenderSetLogicalSize(ren, 320 * 4, 240 * 4); // 4:3 as the CRT displayed 320x200
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
        SDL_AudioSpec want = {0}, have;
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        if (!nograb) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
            grabbed = 1;
        }
    }

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter(), run_start = last;
    double ticks_due = 0;
    const int instr_per_tick = (int)(mips * 1000000.0 / 44100.0);
    int frame = 0, quit = 0;
    double mouse_rx = 0, mouse_ry = 0;
    static int16_t audio_buf[8192];
    double audio_energy = 0, audio_peak = 0;
    int audio_window = 0, audio_loud_secs = 0, audio_total_secs = 0;
    int ab = 0;

    while (!quit && !machine_exit_requested) {
        if (!headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_QUIT: quit = 1; break;
                case SDL_KEYDOWN: case SDL_KEYUP: {
                    SDL_Scancode s = e.key.keysym.scancode;
                    if (e.type == SDL_KEYDOWN && s == SDL_SCANCODE_F12) {
                        grabbed = !grabbed;
                        SDL_SetRelativeMouseMode(grabbed ? SDL_TRUE : SDL_FALSE);
                        break;
                    }
                    if (s == SDL_SCANCODE_T && (e.key.keysym.mod & KMOD_CTRL)) {
                        if (e.type == SDL_KEYDOWN && !e.key.repeat) hires_modern = !hires_modern;
                        break;
                    }
                    if (e.type == SDL_KEYDOWN && s == SDL_SCANCODE_F11) {
                        Uint32 fl = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                        SDL_SetWindowFullscreen(win, fl ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        break;
                    }
                    if (e.key.repeat) break;
                    int xt = xt_code(s);
                    if (xt) machine_key(xt, e.type == SDL_KEYDOWN, ascii_for(s));
                    break;
                }
                case SDL_MOUSEMOTION:
                    if (grabbed) {
                        mouse_rx += e.motion.xrel * mouse_sens;
                        mouse_ry += e.motion.yrel * mouse_sens;
                        int ix = (int)mouse_rx, iy = (int)mouse_ry;
                        mouse_rx -= ix;
                        mouse_ry -= iy;
                        machine_mouse_move(ix, iy);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                    if (!grabbed && e.type == SDL_MOUSEBUTTONDOWN) {
                        grabbed = 1;
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                        break;
                    }
                    int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                    machine_mouse_button(b, e.type == SDL_MOUSEBUTTONDOWN);
                    break;
                }
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && grabbed) {
                        grabbed = 0;
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                    }
                    break;
                }
            }
        }

        // Advance virtual time.
        uint64_t now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / (double)freq;
        last = now;
        if (dt > 0.1) dt = 0.1;
        ticks_due += headless ? 44100.0 / 70.0 : dt * 44100.0;
        while (ticks_due >= 1.0 && !machine_exit_requested) {
            ticks_due -= 1.0;
            machine_check_irq();
            machine_service_mouse();
            cpu_run(instr_per_tick);
            machine_tick();
            int16_t smp = machine_audio_sample();
            audio_energy += (double)smp * smp;
            if (++audio_window == 44100) {
                double rms = sqrt(audio_energy / 44100.0);
                if (rms > audio_peak) audio_peak = rms;
                if (rms > 200.0) audio_loud_secs++;
                audio_total_secs++;
                audio_energy = 0;
                audio_window = 0;
            }
            if (audio && ab < 8192) audio_buf[ab++] = smp;
        }
        if (audio && ab) {
            if (SDL_GetQueuedAudioSize(audio) < 44100) SDL_QueueAudio(audio, audio_buf, (Uint32)ab * 2);
            ab = 0;
        }

        hires_render(fb);
        if (!headless) {
            SDL_UpdateTexture(tex, NULL, fb, fb_w * 4);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        frame++;
        while (script_pos < script_n && script[script_pos].frame <= frame) {
            ScriptEv *e = &script[script_pos++];
            switch (e->kind) {
            case 'k': machine_key(e->a, e->b, e->c); break;
            case 'm': machine_mouse_move(e->a, e->b); break;
            case 'b': machine_mouse_button(e->a, e->b); break;
            case 's': write_png(e->path, fb, fb_w, fb_h); break;
            case 't': hires_modern = !hires_modern; break;
            case 'q': quit = 1; break;
            }
        }
    }
    log_msg("exiting at frame %d, CS:IP %04X:%04X, average %.1f fps, sound in %d of %d seconds, peak rms %.0f\n", frame, cpu.seg[CS], cpu.ip, frame / ((double)(SDL_GetPerformanceCounter() - run_start) / (double)freq), audio_loud_secs, audio_total_secs, audio_peak);
    SDL_Quit();
    return 0;
}
