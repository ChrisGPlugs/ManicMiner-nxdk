/*
 * main.c - SDL2 front end for the Manic Miner port.
 *
 * Builds for the original Xbox with nxdk (NXDK_SDL = y) and, for development,
 * for any desktop with SDL2 (see Makefile.host).
 *
 * The game (mm_game.c) runs as straight-line code; every 69888 emulated
 * T-states (one Spectrum frame, 50 Hz) zx.c calls frame_hook() below, which
 * turns that frame's beeper activity into PCM, reads the controller, draws the
 * Spectrum screen, and paces everything to real time.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL.h>

#ifdef NXDK
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#define DATA_DIR "D:\\"
#else
#define DATA_DIR ""
#endif

#include "zx.h"
#include "mm_mem.h"
#include "mm_game.h"
#include "mm_font.h"
#ifdef MM128
#include "ay.h"
#include "mm128_load.h"
#define GAME_TITLE "Manic Miner 128"
#else
#define GAME_TITLE "Manic Miner"
#endif

#define AUDIO_RATE   48000
#define WINDOW_W     640
#define WINDOW_H     480

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *pad;
static uint32_t frame_pixels[ZX_SCREEN_W * ZX_SCREEN_H];
static uint64_t frame_count;
static uint64_t perf_next;          /* wall-clock pacing when there is no audio */

/* ------------------------------------------------------------------------ */
/* Beeper -> PCM                                                            */
/* ------------------------------------------------------------------------ */
static int16_t audio_buf[2 * 2048];
static double  samples_carry;
static uint8_t frame_start_level;
static float   hp_prev_in, hp_prev_out;
#ifdef MM128
static ay_t    ay;
#endif

static float level_value(uint8_t level)
{
    /* EAR (bit 4) drives the speaker; MIC (bit 3) leaks in slightly. */
    return ((level & 0x10) ? 1.0f : 0.0f) + ((level & 0x08) ? 0.06f : 0.0f);
}

static void render_audio(void)
{
    double exact = (double)AUDIO_RATE * ZX_T_PER_FRAME / ZX_CPU_HZ + samples_carry;
    int n = (int)exact;
    samples_carry = exact - n;
    if (n > 2048) n = 2048;

    double t_per_sample = (double)ZX_T_PER_FRAME / n;
    int ev = 0;
    uint8_t level = frame_start_level;
#ifdef MM128
    int ay_ev = 0;
#endif
    for (int i = 0; i < n; i++) {
        /* Box-filter the speaker level over this sample's T-state span. */
        double t0 = i * t_per_sample, t1 = t0 + t_per_sample, t = t0;
        double acc = 0.0;
        while (ev < zx_beep_count && zx_beeps[ev].t < t1) {
            double te = zx_beeps[ev].t > t ? zx_beeps[ev].t : t;
            acc += level_value(level) * (te - t);
            t = te;
            level = zx_beeps[ev].level;
            ev++;
        }
        acc += level_value(level) * (t1 - t);
        float x = (float)(acc / t_per_sample);
#ifdef MM128
        /* AY register writes take effect at the sample they fall in. */
        while (ay_ev < zx_ay_count && zx_ay_log[ay_ev].t < t1) {
            ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val);
            ay_ev++;
        }
        x = x * 0.6f + ay_sample(&ay) * 1.6f;
#endif
        /* DC-blocking high-pass so a speaker left "on" is silent. */
        float y = x - hp_prev_in + 0.995f * hp_prev_out;
        hp_prev_in = x;
        hp_prev_out = y;
        int s = (int)(y * 9000.0f);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        audio_buf[2 * i] = audio_buf[2 * i + 1] = (int16_t)s;
    }
    frame_start_level = zx_beep_level;
#ifdef MM128
    while (ay_ev < zx_ay_count) { ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val); ay_ev++; }
#endif

    if (audio_dev)
        SDL_QueueAudio(audio_dev, audio_buf, (Uint32)(n * 2 * sizeof(int16_t)));
#ifndef NXDK
    {
        /* MM_PCM=<file>: also append raw 48 kHz s16 stereo to a file. */
        static FILE *pcm;
        static int tried;
        if (!tried) { tried = 1; if (getenv("MM_PCM")) pcm = fopen(getenv("MM_PCM"), "wb"); }
        if (pcm) fwrite(audio_buf, sizeof(int16_t), (size_t)n * 2, pcm);
    }
#endif
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */
/* Keyboard keys driven by the controller are tracked separately from the
 * physical keyboard (desktop) so releasing one doesn't cancel the other. */
static uint8_t pad_keys[8], kb_keys[8], macro_keys[8];

static void set_bit(uint8_t *rows, int row, int bit, int down)
{
    if (down) rows[row] |= (uint8_t)(1u << bit);
    else      rows[row] &= (uint8_t)~(1u << bit);
}

/* Cheat-code macro: types the cheat one key at a time. */
static const struct { uint8_t row, bit; } cheat_keys[] = {
#ifndef MM128
    /* 6031769 */
    { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 0 }, { ZX_ROW_1_5, 2 }, { ZX_ROW_1_5, 0 },
    { ZX_ROW_0_6, 3 }, { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 1 },
#else
    /* TYPEWRITER */
    { ZX_ROW_Q_T, 4 }, { ZX_ROW_P_Y, 4 }, { ZX_ROW_P_Y, 0 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 1 },
    { ZX_ROW_Q_T, 3 }, { ZX_ROW_P_Y, 2 }, { ZX_ROW_Q_T, 4 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 3 },
#endif
};
#define CHEAT_LEN ((int)(sizeof cheat_keys / sizeof cheat_keys[0]))
#ifndef MM128
#define TELEPORT_ROW ZX_ROW_0_6
#define TELEPORT_BIT 4          /* hold '6' */
#define CHEAT_DONE   7
#else
#define TELEPORT_ROW ZX_ROW_0_6
#define TELEPORT_BIT 1          /* hold '9' */
#define CHEAT_DONE   10
#endif
static int macro_step = -1, macro_timer, teleport_timer, teleport_target;

static void run_macros(void)
{
    memset(macro_keys, 0, sizeof macro_keys);
    if (macro_step >= 0) {
        /* Hold each digit for 8 frames then release for 8: the game checks
         * the keys once per main-loop pass (~3.5 frames). */
        if (macro_timer < 8) set_bit(macro_keys, cheat_keys[macro_step].row, cheat_keys[macro_step].bit, 1);
        if (++macro_timer >= 16) {
            macro_timer = 0;
            if (++macro_step >= CHEAT_LEN) macro_step = -1;
        }
    }
    if (teleport_timer > 0) {
        /* '6' ('9' on 128) plus the binary cavern number on keys 1-5. */
        set_bit(macro_keys, TELEPORT_ROW, TELEPORT_BIT, 1);
        for (int b = 0; b < 5; b++)
            if (teleport_target & (1 << b)) set_bit(macro_keys, ZX_ROW_1_5, b, 1);
        teleport_timer--;
    }
}

static void pad_button(int button, int down)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_B:
        set_bit(pad_keys, ZX_ROW_SPACE_B, 2, down);        /* M: jump / resume */
        break;
    case SDL_CONTROLLER_BUTTON_START:
        set_bit(pad_keys, ZX_ROW_ENTER_H, 0, down);        /* ENTER: start game */
#ifdef MM128
        set_bit(pad_keys, ZX_ROW_1_5, 3, down);            /* 4: leave the menu */
#endif
        break;
    case SDL_CONTROLLER_BUTTON_Y:
#ifndef MM128
        set_bit(pad_keys, ZX_ROW_ENTER_H, 4, down);        /* H: music on/off */
#else
        set_bit(pad_keys, ZX_ROW_1_5, 0, down);            /* 1: menu / lives */
#endif
        break;
    case SDL_CONTROLLER_BUTTON_X:
        set_bit(pad_keys, ZX_ROW_A_G, 0, down);            /* A: pause */
        break;
    case SDL_CONTROLLER_BUTTON_BACK:
        set_bit(pad_keys, ZX_ROW_SHIFT_V, 0, down);        /* SHIFT+SPACE: quit */
        set_bit(pad_keys, ZX_ROW_SPACE_B, 0, down);
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:               /* White on the Xbox pad */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:              /* Black on the Xbox pad */
        if (down) {
            if (mem[KeyCounter] != CHEAT_DONE) {
                if (macro_step < 0) { macro_step = 0; macro_timer = 0; }
            } else {
                int cav = mem[CurrentCavernNumber];
#ifndef MM128
                cav += (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1 : 19;
#else
                /* Key '1' (bit 0 of the cavern number) also opens the
                 * options menu in this version, so step by two caverns. */
                cav = (cav & ~1) + ((button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 2 : 18);
#endif
                teleport_target = cav % 20;
                teleport_timer = 8;
            }
        }
        break;
    default:
        break;
    }
}

static void read_pad_axes(void)
{
    uint8_t joy = 0;
    int left = 0, right = 0, fire = 0;
#ifdef MM128
    if (pad) {
        /* Menu options 2 (air) and 3 (music) on the triggers. */
        set_bit(pad_keys, ZX_ROW_1_5, 1, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000);
        set_bit(pad_keys, ZX_ROW_1_5, 2, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);
    }
#endif
    if (pad) {
        int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        left  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -12000;
        right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 12000;
        fire  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ||
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    }
    if (right) joy |= 0x01;
    if (left)  joy |= 0x02;
    if (fire)  joy |= 0x10;
    zx_kempston = joy;
    /* Also press O / P so that left/right resume a paused game. */
    set_bit(pad_keys, ZX_ROW_P_Y, 1, left);
    set_bit(pad_keys, ZX_ROW_P_Y, 0, right);
}

#ifndef NXDK
/* Desktop: map the PC keyboard straight onto the Spectrum matrix. */
static void keyboard_key(SDL_Keycode k, int down)
{
    static const struct { SDL_Keycode key; uint8_t row, bit; } map[] = {
        { SDLK_LSHIFT, 0, 0 }, { SDLK_RSHIFT, 0, 0 }, { SDLK_z, 0, 1 }, { SDLK_x, 0, 2 }, { SDLK_c, 0, 3 }, { SDLK_v, 0, 4 },
        { SDLK_a, 1, 0 }, { SDLK_s, 1, 1 }, { SDLK_d, 1, 2 }, { SDLK_f, 1, 3 }, { SDLK_g, 1, 4 },
        { SDLK_q, 2, 0 }, { SDLK_w, 2, 1 }, { SDLK_e, 2, 2 }, { SDLK_r, 2, 3 }, { SDLK_t, 2, 4 },
        { SDLK_1, 3, 0 }, { SDLK_2, 3, 1 }, { SDLK_3, 3, 2 }, { SDLK_4, 3, 3 }, { SDLK_5, 3, 4 },
        { SDLK_0, 4, 0 }, { SDLK_9, 4, 1 }, { SDLK_8, 4, 2 }, { SDLK_7, 4, 3 }, { SDLK_6, 4, 4 },
        { SDLK_p, 5, 0 }, { SDLK_o, 5, 1 }, { SDLK_i, 5, 2 }, { SDLK_u, 5, 3 }, { SDLK_y, 5, 4 },
        { SDLK_RETURN, 6, 0 }, { SDLK_l, 6, 1 }, { SDLK_k, 6, 2 }, { SDLK_j, 6, 3 }, { SDLK_h, 6, 4 },
        { SDLK_SPACE, 7, 0 }, { SDLK_LCTRL, 7, 1 }, { SDLK_RCTRL, 7, 1 }, { SDLK_m, 7, 2 }, { SDLK_n, 7, 3 }, { SDLK_b, 7, 4 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].key == k) set_bit(kb_keys, map[i].row, map[i].bit, down);
    if (k == SDLK_LEFT)  set_bit(kb_keys, ZX_ROW_P_Y, 1, down);
    if (k == SDLK_RIGHT) set_bit(kb_keys, ZX_ROW_P_Y, 0, down);
    if (k == SDLK_UP)    set_bit(kb_keys, ZX_ROW_SPACE_B, 2, down);
}
#endif

static void poll_input(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            exit(0);
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                SDL_GameControllerClose(pad);
                pad = NULL;
                memset(pad_keys, 0, sizeof pad_keys);
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (!pad) pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
            pad_button(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
            break;
#ifndef NXDK
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) exit(0);
                keyboard_key(e.key.keysym.sym, e.type == SDL_KEYDOWN);
            }
            break;
#endif
        default:
            break;
        }
    }
    read_pad_axes();
    run_macros();
    for (int row = 0; row < 8; row++)
        zx_keys[row] = (uint8_t)(0x1F & ~(pad_keys[row] | kb_keys[row] | macro_keys[row]));
}

/* ------------------------------------------------------------------------ */
/* Video and pacing                                                         */
/* ------------------------------------------------------------------------ */
static void present(void)
{
    zx_render(frame_pixels, ZX_SCREEN_W, (int)((frame_count >> 4) & 1));
    SDL_UpdateTexture(texture, NULL, frame_pixels, ZX_SCREEN_W * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void pace(void)
{
    if (audio_dev) {
        /* Keep about three frames of audio queued: that's the clock. */
        const Uint32 frame_bytes = (Uint32)(AUDIO_RATE / 50) * 4;
        while (SDL_GetQueuedAudioSize(audio_dev) > 3 * frame_bytes)
            SDL_Delay(1);
        return;
    }
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t frame = freq * ZX_T_PER_FRAME / ZX_CPU_HZ;
    uint64_t now = SDL_GetPerformanceCounter();
    if (!perf_next || now > perf_next + 5 * frame) perf_next = now;
    perf_next += frame;
    while (SDL_GetPerformanceCounter() < perf_next) {
        uint64_t left_ms = (perf_next - SDL_GetPerformanceCounter()) * 1000 / freq;
        if (left_ms > 1) SDL_Delay((Uint32)(left_ms - 1));
    }
}

#ifndef NXDK
/* Desktop-only debugging aids (environment variables):
 *   MM_FAST=1              don't pace to real time
 *   MM_EXIT_AFTER=<n>      quit after n frames
 *   MM_SHOTS=<f1,f2,...>   save shot_<frame>.bmp at those frames
 *   MM_ENTER_AT=<f>        hold ENTER for a few frames from frame f
 *   MM_KEYS=<f>:<c>,...    hold key c ('1'-'9', '0', a-z, or '#' for ENTER)
 *                          for 10 frames starting at frame f */
static int  dbg_fast;
static long dbg_exit_after = -1, dbg_enter_at = -1;
static const char *dbg_shots;
static const char *dbg_keys;

static void dbg_key_char(char c, int down)
{
    static const char *rows[8] = { "#zxcv", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " #mnb" };
    if (c == '#') { set_bit(kb_keys, ZX_ROW_ENTER_H, 0, down); return; }
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 5; b++)
            if (rows[r][b] == c) set_bit(kb_keys, r, b, down);
}

static void debug_frame(void)
{
    if (dbg_enter_at >= 0)
        set_bit(kb_keys, ZX_ROW_ENTER_H, 0, (long)frame_count >= dbg_enter_at && (long)frame_count < dbg_enter_at + 10);
    if (dbg_keys) {
        for (const char *p = dbg_keys; *p; ) {
            long f = strtol(p, (char **)&p, 10);
            if (*p != ':' || !p[1]) break;
            char c = p[1];
            p += 2;
            if ((long)frame_count == f) dbg_key_char(c, 1);
            if ((long)frame_count == f + 10) dbg_key_char(c, 0);
            if (*p == ',') p++;
        }
    }
    if (dbg_shots) {
        char want[32];
        snprintf(want, sizeof want, ",%llu,", (unsigned long long)frame_count);
        char list[512];
        snprintf(list, sizeof list, ",%s,", dbg_shots);
        if (strstr(list, want)) {
            SDL_Surface *s = SDL_CreateRGBSurfaceFrom(frame_pixels, ZX_SCREEN_W, ZX_SCREEN_H, 32, ZX_SCREEN_W * 4,
                                                      0x00FF0000, 0x0000FF00, 0x000000FF, 0);
            char name[64];
            snprintf(name, sizeof name, "shot_%llu.bmp", (unsigned long long)frame_count);
            SDL_SaveBMP(s, name);
            SDL_FreeSurface(s);
        }
    }
    if (dbg_exit_after >= 0 && (long)frame_count >= dbg_exit_after) exit(0);
}
#endif

static void frame_hook(void)
{
    render_audio();
    poll_input();
#ifndef NXDK
    debug_frame();
#endif
    present();
#ifndef NXDK
    if (!dbg_fast)
#endif
    pace();
    frame_count++;
}

/* ------------------------------------------------------------------------ */
/* Start-up                                                                 */
/* ------------------------------------------------------------------------ */
static long load_file(const char *path, uint8_t *dst, long max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n;
}

/* Show an error on the Spectrum screen for a while, then quit. */
static void fatal_screen(const char *line1, const char *line2)
{
    memset(mem + 0x4000, 0, 0x1800);
    memset(mem + 0x5800, 0x47, 0x300);
    zx_border = 2;
    const char *lines[2] = { line1, line2 };
    for (int l = 0; l < 2; l++) {
        const char *s = lines[l];
        int row = 10 + l * 2;
        for (int col = 0; s[col] && col < 32; col++) {
            uint8_t ch = (uint8_t)s[col];
            for (int py = 0; py < 8; py++) {
                uint16_t addr = (uint16_t)(0x4000 | ((row & 0x18) << 8) | (py << 8) | ((row & 7) << 5) | col);
                mem[addr] = mem[ROM_FONT + (ch - 32) * 8 + py];
            }
        }
    }
    for (int i = 0; i < 50 * 15; i++) {
        poll_input();
        present();
        SDL_Delay(20);
    }
    exit(1);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
#ifdef NXDK
    XVideoSetMode(WINDOW_W, WINDOW_H, 32, REFRESH_DEFAULT);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
            return 1;
    }
    window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN);
    if (!window) return 1;
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) return 1;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                ZX_SCREEN_W, ZX_SCREEN_H);
    if (!texture) return 1;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && !pad) pad = SDL_GameControllerOpen(i);

    zx_reset();
    memset(mem, 0, sizeof mem);
#ifdef MM128
    ay_init(&ay, AY_CLOCK_128K, AUDIO_RATE);
#endif

    /* Optional 48K ROM for the authentic character set. */
    static uint8_t rom[0x4000];
    int have_rom = load_file(DATA_DIR "48.rom", rom, sizeof rom) == (long)sizeof rom;
    mm_install_font(have_rom ? rom : NULL);

    static uint8_t image[65536];
#ifndef MM128
    /* The game: a 64K memory image of the original, loaded at address 0. */
    long n = load_file(DATA_DIR "ManicMiner.bin", image, sizeof image);
    if (n != 65536)
        fatal_screen(n < 0 ? "ManicMiner.bin not found." : "ManicMiner.bin is not 64K.",
                     "See README for where to get it.");
    if (image[AppEntry] != 0xF3 || memcmp(image + CentralCavernData + 0x200 + 9, "Central Cavern", 14) != 0)
        fatal_screen("ManicMiner.bin doesn't look", "like Manic Miner (48K).");
#else
    /* The game: the original tape, unpacked exactly as its loader does. */
    static uint8_t tap[65536];
    long n = load_file(DATA_DIR "ManicMiner128.tap", tap, sizeof tap);
    if (n < 0)
        fatal_screen("ManicMiner128.tap not found.", "See README for where to get it.");
    int r = mm128_build_image(tap, n, image);
    if (r == MM128_ERR_TAP)
        fatal_screen("ManicMiner128.tap: not a", "valid TAP file.");
    if (r != MM128_OK)
        fatal_screen("Unsupported Manic Miner 128", "tape (SP version needed).");
#endif
    memcpy(mem + 0x4000, image + 0x4000, 0xC000);   /* keep our ROM area */

#ifndef NXDK
    dbg_fast = getenv("MM_FAST") != NULL;
    if (getenv("MM_EXIT_AFTER")) dbg_exit_after = atol(getenv("MM_EXIT_AFTER"));
    if (getenv("MM_ENTER_AT")) dbg_enter_at = atol(getenv("MM_ENTER_AT"));
    dbg_shots = getenv("MM_SHOTS");
    dbg_keys = getenv("MM_KEYS");
#endif

    zx_frame_hook = frame_hook;
    mm_run();          /* never returns */
    return 0;
}
