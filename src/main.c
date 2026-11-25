/*
 * main.c - SDL2 front end for the Manic Miner port.
 *
 * Builds for the original Xbox with nxdk (NXDK_SDL = y) and, for development,
 * for any desktop with SDL2 (see Makefile.host).
 *
 * The game (mm_game.c) runs as straight-line code; every emulated Spectrum
 * frame (50 Hz) zx.c calls frame_hook() below, which turns that frame's
 * beeper/AY activity into PCM, reads the controller, draws the Spectrum
 * screen, and paces everything to real time.
 *
 * Three builds share this file:
 *   default          Manic Miner (Bug-Byte, 48K) only
 *   MM_ONLY_128      Manic Miner 128 only
 *   MM_COMBINED      both engines; picks one at start-up from the data files
 *                    present, with a chooser styled on the game's title screen
 *                    when both are there
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
#include <hal/audio.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#define DATA_DIR "D:\\"
#else
#define DATA_DIR ""
#endif

#if defined(MM_COMBINED)
#define HAVE_48  1
#define HAVE_128 1
#define GAME_TITLE "Manic Miner"
#elif defined(MM_ONLY_128)
#define HAVE_128 1
#define GAME_TITLE "Manic Miner 128"
#else
#define HAVE_48  1
#define GAME_TITLE "Manic Miner"
#endif

#include "zx.h"
#include "mm_mem.h"          /* Bug-Byte addresses (shared ones are identical) */
#include "mm_game.h"
#include "mm_font.h"
#ifdef HAVE_128
#include "ay.h"
#include "mm128_load.h"
#endif

/* Which game is running (fixed in the single builds). */
#if defined(HAVE_128) && !defined(HAVE_48)
static const int v128 = 1;
#elif defined(HAVE_128)
static int v128;
#else
static const int v128 = 0;
#endif

/* Manic Miner 128 addresses the front end needs (see mm_mem.h, -DMM128). */
#define MM128_PatchLivesDec   35147
#define MM128_MenuLivesYesNo  38004

#define AUDIO_RATE   48000
#define WINDOW_W     640
#define WINDOW_H     480

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;
#ifndef NXDK
static SDL_AudioDeviceID audio_dev;
#endif
static SDL_GameController *pad;
#ifndef NXDK
static uint32_t frame_pixels[ZX_SCREEN_W * ZX_SCREEN_H];
#endif
static uint64_t frame_count;
static uint64_t perf_next;          /* wall-clock pacing when there is no audio */

/* Audio queue targets. The amount of audio queued and not yet played is
 * the game's clock: the Spectrum is emulated only as fast as its sound is
 * played. */
#define AUDIO_QUEUE_TARGET_MS  80   /* pace the game to keep ~80 ms queued */
#define AUDIO_QUEUE_LOW_MS     40   /* below this, skip drawing to catch up */

/* Diagnostics overlay (left-stick click / F1): queued audio, underruns,
 * draw time, frames drawn and skipped. */
static int      diag_on;
static unsigned diag_underruns, diag_skipped, diag_drawn;
static unsigned diag_draw_us;

#if defined(HAVE_48) && defined(HAVE_128)
/* Chooser input, latched by poll_input so a quick tap is never missed. */
static int ui_up, ui_down, ui_select;
static int ui_stick_y;
#endif

/* ------------------------------------------------------------------------ */
/* Audio output                                                             */
/* ------------------------------------------------------------------------ */
static int audio_ok;
/* Diagnostics measured over ~2 s windows of wall-clock time. Integers
 * only: nxdk's printf has no floating-point formatting. */
static uint64_t stats_t0_us, stats_samples, stats_frames, stats_t0_z, stats_t0_contended;
static unsigned stats_rate_hz, stats_fps_x100, stats_contention_pct10;

#ifdef NXDK
/* The Xbox's AC97 controller fed directly, one buffer per Spectrum frame,
 * in a ring of 32 DMA descriptors. How much is still queued comes from the
 * controller's own Current Index Value register, so pacing follows the
 * 48 kHz hardware exactly, without SDL's audio thread or driver callbacks. */
#define XA_RING      32
#define XA_BUF_BYTES (2048 * 4)
static uint8_t *xa_buf[XA_RING];
static uint16_t xa_samples[XA_RING];
static unsigned xa_next;                     /* descriptors submitted, total */
static volatile uint8_t *const xa_regs = (volatile uint8_t *)0xFEC00000;

static void audio_open(void)
{
    XAudioInit(16, 2, NULL, NULL);           /* we feed it ourselves */
    for (int i = 0; i < XA_RING; i++) {
        xa_buf[i] = MmAllocateContiguousMemoryEx(XA_BUF_BYTES, 0, 0xFFFFFFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!xa_buf[i]) return;
    }
    audio_ok = 1;
    /* Start with three frames of silence queued, then run. */
    static int16_t silence[960 * 2];
    for (int i = 0; i < 3; i++) {
        unsigned slot = xa_next & 31;
        memcpy(xa_buf[slot], silence, sizeof silence);
        xa_samples[slot] = 960;
        XAudioProvideSamples(xa_buf[slot], (unsigned short)sizeof silence, 0);
        xa_next++;
    }
    XAudioPlay();
}

/* Buffers handed to the controller and not yet finished. */
static int xa_in_flight(void)
{
    uint8_t civ = xa_regs[0x114] & 31;       /* PCM out: current index */
    uint8_t sr  = xa_regs[0x116];            /* PCM out: status */
    if (sr & 1) return 0;                    /* DMA halted: ran dry */
    return (int)((xa_next - civ) & 31);
}

static unsigned audio_queued_ms(void)
{
    if (!audio_ok) return 0;
    int n = xa_in_flight();
    unsigned samples = 0;
    for (int i = 0; i < n; i++) samples += xa_samples[(xa_next - 1 - i) & 31];
    return samples * 1000u / AUDIO_RATE;
}

static void audio_submit(const int16_t *buf, int frames)
{
    if (!audio_ok) return;
    int halted = (xa_regs[0x116] & 1) != 0;
    if (frame_count > 50 && (halted || xa_in_flight() == 0)) diag_underruns++;
    if (!halted && xa_in_flight() >= XA_RING - 2) return; /* never overrun the ring */
    unsigned slot = xa_next & 31;
    memcpy(xa_buf[slot], buf, (size_t)frames * 4);
    xa_samples[slot] = (uint16_t)frames;
    XAudioProvideSamples(xa_buf[slot], (unsigned short)(frames * 4), 0);
    xa_next++;
    if (halted) XAudioPlay();                /* ran dry: restart the DMA */
}
#else
static void audio_open(void)
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) { SDL_PauseAudioDevice(audio_dev, 0); audio_ok = 1; }
}

static unsigned audio_queued_ms(void)
{
    return audio_ok ? SDL_GetQueuedAudioSize(audio_dev) / (AUDIO_RATE / 1000 * 4) : 0;
}

static void audio_submit(const int16_t *buf, int frames)
{
    if (!audio_ok) return;
    if (frame_count > 50 && SDL_GetQueuedAudioSize(audio_dev) == 0) diag_underruns++;
    SDL_QueueAudio(audio_dev, buf, (Uint32)(frames * 2 * sizeof(int16_t)));
}
#endif

/* Wall-clock time in microseconds. On the Xbox this is the kernel's
 * system time, independent of the audio hardware and of the performance
 * counter. */
static uint64_t now_us(void)
{
#ifdef NXDK
    LARGE_INTEGER t;
    KeQuerySystemTime(&t);
    return (uint64_t)t.QuadPart / 10;
#else
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1e6 / (double)SDL_GetPerformanceFrequency());
#endif
}

/* Per-frame statistics: playback rate (samples consumed per second; while
 * the queue level is steady, submitted == consumed), emulated frames per
 * second, and the share of emulated time spent in ULA contention. */
static void stats_frame(int samples)
{
    uint64_t now = now_us();
    if (!stats_t0_us) {
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
        return;
    }
    stats_samples += (uint64_t)samples;
    stats_frames++;
    uint64_t dt = now - stats_t0_us;
    if (dt >= 2000000) {
        stats_rate_hz = (unsigned)(stats_samples * 1000000 / dt);
        stats_fps_x100 = (unsigned)(stats_frames * 100000000 / dt);
        uint64_t dz = zx_t - stats_t0_z, dc = zx_contended_t - stats_t0_contended;
        stats_contention_pct10 = dz ? (unsigned)(dc * 1000 / dz) : 0;
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
    }
}

/* ------------------------------------------------------------------------ */
/* Beeper -> PCM                                                            */
/* ------------------------------------------------------------------------ */
static int16_t audio_buf[2 * 2048];
static double  samples_carry;
static uint8_t frame_start_level;
static float   hp_prev_in, hp_prev_out;
#ifdef HAVE_128
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
#ifdef HAVE_128
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
#ifdef HAVE_128
        if (v128) {
            /* AY register writes take effect at the sample they fall in. */
            while (ay_ev < zx_ay_count && zx_ay_log[ay_ev].t < t1) {
                ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val);
                ay_ev++;
            }
            x = x * 0.6f + ay_sample(&ay) * 1.6f;
        }
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
#ifdef HAVE_128
    if (v128)
        while (ay_ev < zx_ay_count) { ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val); ay_ev++; }
#endif

    audio_submit(audio_buf, n);
    stats_frame(n);
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
typedef struct { uint8_t row, bit; } zx_keypos_t;
static const zx_keypos_t cheat_keys_48[] = {
    /* 6031769 */
    { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 0 }, { ZX_ROW_1_5, 2 }, { ZX_ROW_1_5, 0 },
    { ZX_ROW_0_6, 3 }, { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 1 },
};
static const zx_keypos_t cheat_keys_128[] = {
    /* TYPEWRITER (Software Projects) */
    { ZX_ROW_Q_T, 4 }, { ZX_ROW_P_Y, 4 }, { ZX_ROW_P_Y, 0 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 1 },
    { ZX_ROW_Q_T, 3 }, { ZX_ROW_P_Y, 2 }, { ZX_ROW_Q_T, 4 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 3 },
};
#define CHEAT_KEYS   (v128 ? cheat_keys_128 : cheat_keys_48)
#define CHEAT_LEN    (v128 ? 10 : 7)
#define CHEAT_DONE   (v128 ? 10 : 7)       /* KeyCounter value once typed */
#define TELEPORT_BIT (v128 ? 1 : 4)        /* hold '9' (128) or '6' (48K) */
static int macro_step = -1, macro_timer;

/* Teleport macro. The game's teleport check reads the 0-6 row (activator
 * '6', or '9' on Manic Miner 128) and then, immediately after, the 1-5 row
 * for the cavern number. Rather than holding keys down, the macro answers
 * exactly that pair of port reads: the activator shows as pressed on 0-6
 * row reads, and the cavern number appears only on the 1-5 read that
 * directly follows one of those. Every other read of the 1-5 row sees the
 * keys up, so on Manic Miner 128 key '1' never opens Mihai's options menu
 * and key '5' never nudges Willy left. One teleport per press. */
static int teleport_pending, teleport_target, teleport_frames;
static uint16_t last_in_port;
static int last_in_was_forced_activator;

static uint8_t teleport_in_filter(uint16_t port, uint8_t v)
{
    int after_activator = (last_in_port == 0xEFFE) && last_in_was_forced_activator;
    last_in_port = port;
    last_in_was_forced_activator = 0;
    if (!teleport_pending) return v;
    if (port == 0xEFFE) {
        last_in_was_forced_activator = 1;
        return (uint8_t)(v & ~(1u << TELEPORT_BIT));
    }
    if (port == 0xF7FE && after_activator) {
        teleport_pending = 0;
        return (uint8_t)((v & 0xE0) | (0x1F & ~teleport_target));
    }
    return v;
}

static void start_teleport(int forward)
{
    if (mem[KeyCounter] != CHEAT_DONE) return;
    int cav = mem[CurrentCavernNumber] + (forward ? 1 : 19);
    teleport_target = cav % 20;
    teleport_pending = 1;
    teleport_frames = 0;
}

static void run_macros(void)
{
    memset(macro_keys, 0, sizeof macro_keys);
    if (macro_step >= 0) {
        /* Hold each digit for 8 frames then release for 8: the game checks
         * the keys once per main-loop pass (~3.5 frames). */
        if (macro_timer < 8) set_bit(macro_keys, CHEAT_KEYS[macro_step].row, CHEAT_KEYS[macro_step].bit, 1);
        if (++macro_timer >= 16) {
            macro_timer = 0;
            if (++macro_step >= CHEAT_LEN) macro_step = -1;
        }
    }
    /* Give up if the game never reached its teleport check (e.g. a
     * teleport requested on the title screen or during the game-over
     * sequence). */
    if (teleport_pending && ++teleport_frames > 25) teleport_pending = 0;
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
        if (v128) set_bit(pad_keys, ZX_ROW_1_5, 3, down);  /* 4: leave the menu */
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        if (!v128) set_bit(pad_keys, ZX_ROW_ENTER_H, 4, down);  /* H: music on/off */
        else       set_bit(pad_keys, ZX_ROW_1_5, 0, down);      /* 1: menu / lives */
        break;
    case SDL_CONTROLLER_BUTTON_X:
        set_bit(pad_keys, ZX_ROW_A_G, 0, down);            /* A: pause */
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        if (down) diag_on = !diag_on;                      /* diagnostics */
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
                start_teleport(button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
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
#ifdef NXDK
    /* In-game reset: LT + RT + Back + (Start or Black) returns to the
     * dashboard. Both forms, to suit both types of Xbox controller. */
    if (pad &&
        SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 &&
        SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000 &&
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) &&
        (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) ||
         SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))) {
        XAudioPause();
        HalReturnToFirmware(HalRebootRoutine);   /* 1; not the quick reboot (2) */
    }
#endif
    if (v128 && pad) {
        /* Menu options 2 (air) and 3 (music) on the triggers. */
        set_bit(pad_keys, ZX_ROW_1_5, 1, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000);
        set_bit(pad_keys, ZX_ROW_1_5, 2, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);
    }
    if (pad) {
        int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        left  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -12000;
        right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 12000;
        fire  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ||
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    }
#if defined(HAVE_48) && defined(HAVE_128)
    if (pad) {
        int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        int dir = y < -16000 ? -1 : (y > 16000 ? 1 : 0);
        if (dir != ui_stick_y) { if (dir < 0) ui_up = 1; if (dir > 0) ui_down = 1; }
        ui_stick_y = dir;
    }
#endif
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
#if defined(HAVE_48) && defined(HAVE_128)
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) ui_up = 1;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) ui_down = 1;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
                    e.cbutton.button == SDL_CONTROLLER_BUTTON_START) ui_select = 1;
            }
#endif
            break;
#ifndef NXDK
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) exit(0);
                if (e.key.keysym.sym == SDLK_F1 && e.type == SDL_KEYDOWN) diag_on = !diag_on;
#if defined(HAVE_48) && defined(HAVE_128)
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_DOWN) ui_down = 1;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_UP) ui_up = 1;
#endif
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
/* Has the picture changed since it was last drawn? Manic Miner redraws
 * the screen once per main-loop pass (~3.5 frames), so most frames are
 * identical and needn't be drawn again. */
static int screen_changed(int flash_phase)
{
    static uint8_t last[6912];
    static int last_border = -1, last_flash = -1;
    int has_flash = 0;
    for (int i = 0; i < 768; i++)
        if (mem[0x5800 + i] & 0x80) { has_flash = 1; break; }
    int flash_key = has_flash ? flash_phase : -1;
    if (last_border == zx_border && last_flash == flash_key && !memcmp(last, mem + 0x4000, sizeof last))
        return 0;
    memcpy(last, mem + 0x4000, sizeof last);
    last_border = zx_border;
    last_flash = flash_key;
    return 1;
}

/* Text for the diagnostics overlay, drawn with the game's own font. */
static void draw_text(uint32_t *dst, int pitch, int scale, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    for (; *s; s++, x += 8 * scale) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 32 || ch > 127) ch = '?';
        for (int py = 0; py < 8 * scale; py++) {
            uint8_t bits = mem[ROM_FONT + (ch - 32) * 8 + py / scale];
            uint32_t *row = dst + (y + py) * pitch + x;
            for (int px = 0; px < 8 * scale; px++)
                row[px] = (bits & (0x80 >> (px / scale))) ? fg : bg;
        }
    }
}

/* The overlay: a box of labelled lines, drawn inside the TV-safe area. */
static void draw_diag(uint32_t *dst, int pitch, int scale, int x0, int y0)
{
    char lines[6][48];
    unsigned exp_fps_x100 = (unsigned)((uint64_t)ZX_CPU_HZ * 100 / ZX_T_PER_FRAME);
    snprintf(lines[0], 48, " SPEED %3u.%02u FPS (REAL %u.%02u) ", stats_fps_x100 / 100, stats_fps_x100 % 100,
             exp_fps_x100 / 100, exp_fps_x100 % 100);
    snprintf(lines[1], 48, " AUDIO RATE %5u HZ (48000)    ", stats_rate_hz);
    snprintf(lines[2], 48, " AUDIO QUEUED %3u MS  GAPS %-5u", audio_queued_ms(), diag_underruns);
    snprintf(lines[3], 48, " DRAW %2u.%u MS  SKIPPED %-8u", diag_draw_us / 1000, (diag_draw_us / 100) % 10, diag_skipped);
    snprintf(lines[4], 48, " ULA CONTENTION %2u.%u%%          ", stats_contention_pct10 / 10, stats_contention_pct10 % 10);
    snprintf(lines[5], 48, "                               ");
    for (int i = 0; i < 6; i++)
        draw_text(dst, pitch, scale, x0, y0 + i * 10 * scale, lines[i], 0xFFFFFFFF, 0xFF0000A0);
}

#ifdef NXDK
/* Draw straight into the Xbox framebuffer at 2x: one pass, sequential
 * writes, no SDL surface conversion or scaling. */
static void present(void)
{
    int flash = (int)((frame_count >> 4) & 1);
    int changed = screen_changed(flash);
    if (changed || diag_on) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        VIDEO_MODE vm = XVideoGetMode();
        uint32_t *fb = (uint32_t *)XVideoGetFB();
        if (changed) {
            static uint32_t line[ZX_SCREEN_W * ZX_SCREEN_H];
            zx_render(line, ZX_SCREEN_W, flash);
            static uint32_t row2x[WINDOW_W];
            for (int y = 0; y < ZX_SCREEN_H; y++) {
                const uint32_t *src = line + y * ZX_SCREEN_W;
                for (int x = 0; x < ZX_SCREEN_W; x++) row2x[2 * x] = row2x[2 * x + 1] = src[x];
                memcpy(fb + (2 * y) * vm.width, row2x, sizeof row2x);
                memcpy(fb + (2 * y + 1) * vm.width, row2x, sizeof row2x);
            }
            diag_drawn++;
        }
        if (diag_on) {
            draw_diag(fb, vm.width, 2, 64, 64);
        }
        XVideoFlushFB();
        double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        diag_draw_us = (unsigned)((diag_draw_us * 9 + (unsigned)(ms * 1000.0)) / 10);
    }
}
#else
static void present(void)
{
    int flash = (int)((frame_count >> 4) & 1);
    if (!screen_changed(flash) && !diag_on) return;
    uint64_t t0 = SDL_GetPerformanceCounter();
    zx_render(frame_pixels, ZX_SCREEN_W, flash);
    if (diag_on) {
        draw_diag(frame_pixels, ZX_SCREEN_W, 1, 32, 32);
    }
    SDL_UpdateTexture(texture, NULL, frame_pixels, ZX_SCREEN_W * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    diag_drawn++;
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    diag_draw_us = (unsigned)((diag_draw_us * 9 + (unsigned)(ms * 1000.0)) / 10);
}
#endif

static void pace(void)
{
    if (audio_ok) {
        /* The audio queue is the clock: keep ~80 ms queued. */
        while (audio_queued_ms() > AUDIO_QUEUE_TARGET_MS)
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
 *                          for 10 frames starting at frame f
 *   MM_TELEPORT_AT=<f>,... press Black (teleport forward) at frame f
 *   MM_REPORT=1            print cavern / cheat / menu-patch state on exit
 *   MM_DIAG=1              start with the diagnostics overlay on (F1) */
static int  dbg_fast;
static long dbg_exit_after = -1, dbg_enter_at = -1;
static const char *dbg_shots;
static const char *dbg_keys;
static const char *dbg_teleport;

static void dbg_report(void)
{
    fprintf(stderr, "frame %llu cavern %d cheat %d lives %d",
            (unsigned long long)frame_count, mem[CurrentCavernNumber], mem[KeyCounter], mem[LivesRemaining]);
    if (v128)
        fprintf(stderr, " lives-patch %d (53 = normal) menu-text '%.3s'", mem[MM128_PatchLivesDec],
                (const char *)mem + MM128_MenuLivesYesNo);
    fprintf(stderr, "\n");
}

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
    if (dbg_teleport) {
        char want[32], list[512];
        snprintf(want, sizeof want, ",%llu,", (unsigned long long)frame_count);
        snprintf(list, sizeof list, ",%s,", dbg_teleport);
        if (strstr(list, want)) start_teleport(1);
    }
    if (getenv("MM_REPORT") && frame_count % 100 == 0) dbg_report();
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
    /* If the audio queue is running low, the game must catch up: emulate
     * this frame without drawing it. Sound must never wait for pictures. */
    if (audio_ok && frame_count > 50 && audio_queued_ms() < AUDIO_QUEUE_LOW_MS)
        diag_skipped++;
    else
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


#if defined(HAVE_48) && defined(HAVE_128)
/* ------------------------------------------------------------------------ */
/* Chooser (combined build, both games present)                             */
/*                                                                          */
/* Built from Manic Miner's own title screen, which is in ManicMiner.bin:   */
/* the house-and-sun picture, the MANIC MINER band and the piano keyboard,  */
/* Willy walking at the top right, and the Blue Danube on the beeper with   */
/* the piano keys lighting up (the same note loop and key mapping as        */
/* PlayTheThemeTune). The bottom third holds the two choices, a scrolling   */
/* banner and a hint line.                                                  */
/* ------------------------------------------------------------------------ */

static uint16_t ui_scr(int row, int col)
{
    return (uint16_t)(0x4000 | ((row & 0x18) << 8) | ((row & 7) << 5) | col);
}

static void ui_print(int row, int col, const char *s)
{
    for (; *s && col < 32; s++, col++) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 32 || ch > 127) ch = ' ';
        for (int py = 0; py < 8; py++)
            mem[ui_scr(row, col) + (py << 8)] = mem[ROM_FONT + (ch - 32) * 8 + py];
    }
}

static void ui_attr_row(int row, uint8_t a) { memset(mem + 0x5800 + row * 32, a, 32); }

/* DrawASprite in overwrite mode (16x16, display-file addressing). */
static void ui_sprite(uint16_t hl, uint16_t de)
{
    for (int b = 0; b < 16; b++) {
        mem[hl] = mem[de++];
        mem[(hl & 0xFF00) | (uint8_t)(hl + 1)] = mem[de++];
        uint8_t h = (uint8_t)((hl >> 8) + 1), l = (uint8_t)hl;
        if (!(h & 7)) {
            h = (uint8_t)(h - 8);
            l = (uint8_t)(l + 0x20);
            if (!(l & 0xE0)) h = (uint8_t)(h + 8);
        }
        hl = (uint16_t)((h << 8) | l);
    }
}

static void ui_options(int sel)
{
    static const char *names[2] = {
        " MANIC MINER       BUG-BYTE 48K ",
        " MANIC MINER 128       AY MUSIC ",
    };
    for (int i = 0; i < 2; i++) {
        int row = 17 + 2 * i;
        char line[40];
        snprintf(line, sizeof line, "%s", names[i]);
        line[0] = (i == sel) ? '>' : ' ';
        ui_print(row, 0, line);
        ui_attr_row(row, (i == sel) ? 0x30 : 0x46);   /* cavern-name bar / bright yellow */
    }
}

/* Handle latched input; returns 1 if a game was chosen. */
static int ui_input(int *sel)
{
    int up = ui_up, down = ui_down, go = ui_select;
    ui_up = ui_down = ui_select = 0;
    /* Spectrum keys too: 7 = up, 6 = down (cursor keys), ENTER / 0 = go. */
    static uint8_t prev_06 = 0x1F, prev_enter = 0x1F;
    uint8_t k06 = zx_keys[ZX_ROW_0_6], ken = zx_keys[ZX_ROW_ENTER_H];
    if ((prev_06 & 0x08) && !(k06 & 0x08)) up = 1;
    if ((prev_06 & 0x10) && !(k06 & 0x10)) down = 1;
    if (((prev_06 & 0x01) && !(k06 & 0x01)) || ((prev_enter & 0x01) && !(ken & 0x01))) go = 1;
    prev_06 = k06; prev_enter = ken;
    if (up || down) { *sel ^= 1; ui_options(*sel); }
    return go;
}

/* CalcAFAForPianoKey */
static uint16_t ui_piano(uint8_t a)
{
    a = (uint8_t)(a - 8);
    a = (uint8_t)((a >> 3) | (a << 5));
    a = (uint8_t)(~a | 0xE0);
    return (uint16_t)(0x5900 | a);
}

/* One note of the theme tune (PlayTheThemeTune's loop, T-state timed). */
static void ui_note(uint16_t iy)
{
    uint8_t c = mem[iy], d = mem[iy + 1], e = mem[iy + 2], b = 0;
    mem[ui_piano(d)] = 80;
    uint16_t key = ui_piano(e);
    mem[key] = 40;
    uint8_t a = (uint8_t)key;
    zx_tick(150);
    for (;;) {
        zx_out(0x00FE, a);
        zx_tick(11 + 4);
        if (--d == 0) { d = mem[iy + 1]; a ^= 24; zx_tick(7 + 19 + 7); } else zx_tick(12);
        zx_tick(4);
        if (--e == 0) { e = mem[iy + 2]; a ^= 24; zx_tick(7 + 19 + 7); } else zx_tick(12);
        if (--b) { zx_tick(13); continue; }
        zx_tick(8 + 4);
        if (--c) { zx_tick(12); continue; }
        zx_tick(7);
        break;
    }
    mem[ui_piano(mem[iy + 1])] = 56;
    mem[ui_piano(mem[iy + 2])] = 56;
    zx_tick(200);
}

static int launcher(void)
{
    int sel = 0;
    static const char banner[] =
        "                                "
        ".  .  .  .  .  .  .  CHOOSE YOUR MINE  .  .  .  "
        "MANIC MINER . . BUG-BYTE ltd. 1983 . . By Matthew Smith  .  .  .  "
        "MANIC MINER 128 . . AY music by Mihai Novitchi . . fixes by Bob Fossil  .  .  .  "
        "D-pad to choose . . A or START to play . . "
        "LT+RT+BACK+START to reset  .  .  .  .  .  .  .  "
        "                                ";
    int banner_len = (int)sizeof banner - 1;

    /* The title screen, as Start draws it. */
    memset(mem + 0x4000, 0, 0x1800);
    memcpy(mem + 0x4000, mem + TitleScreenDataTop, 0x1000);
    memcpy(mem + 0x5800, mem + TheFinalBarrierData, 0x100);
    memcpy(mem + 0x5900, mem + BottomAttributes, 0x200);
    ui_sprite(0x483D, WillySpriteData1);
    /* Bottom third: the chooser. */
    for (int row = 16; row < 24; row++) ui_attr_row(row, 0x00);
    ui_options(sel);
    ui_print(23, 0, "  D-PAD: CHOOSE   A/START: PLAY ");
    ui_attr_row(23, 0x45);
    ui_attr_row(21, 0x46);
    ui_up = ui_down = ui_select = 0;

    for (;;) {
        /* The Blue Danube, as on the title screen. */
        for (uint16_t iy = TitleScreenTuneData; mem[iy] != 0xFF; iy = (uint16_t)(iy + 3)) {
            ui_note(iy);
            if (ui_input(&sel)) goto chosen;
        }
        /* Then the banner scrolls past while Willy walks. */
        for (int i = 0; i + 32 <= banner_len; i++) {
            char window[33];
            memcpy(window, banner + i, 32);
            window[32] = 0;
            ui_print(21, 0, window);
            ui_sprite(0x483D, (uint16_t)(WillySpriteData | (((i & 6) >> 3 | (i & 6) << 5) & 0xFF)));
            zx_tick(100 * (256 * 13 + 16));      /* ~0.1 s, as Start4 */
            if (ui_input(&sel)) goto chosen;
        }
    }
chosen:
    /* Wait for the button to be released so the game's own title screen
     * doesn't see it as "start" straight away. */
    for (int i = 0; i < 100; i++) {
        zx_tick((int32_t)zx_t_per_frame);
        int held = !(zx_keys[ZX_ROW_ENTER_H] & 1) || !(zx_keys[ZX_ROW_0_6] & 1) ||
                   !(zx_keys[ZX_ROW_SPACE_B] & 4) || (zx_kempston & 0x10);
        if (!held) break;
    }
    zx_out(0x00FE, 0);
    return sel;
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
#ifdef NXDK
    /* Video goes straight to the framebuffer and audio straight to the AC97
     * controller; SDL handles the controllers. */
    XVideoSetMode(WINDOW_W, WINDOW_H, 32, REFRESH_DEFAULT);
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
        return 1;
    memset(XVideoGetFB(), 0, (size_t)WINDOW_W * WINDOW_H * 4);
    XVideoFlushFB();
    (void)window; (void)renderer; (void)texture;
#else
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
#endif

    audio_open();

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && !pad) pad = SDL_GameControllerOpen(i);

    /* Optional 48K ROM for the authentic character set. */
    static uint8_t rom[0x4000];
    int have_rom = load_file(DATA_DIR "48.rom", rom, sizeof rom) == (long)sizeof rom;

    zx_reset();
    memset(mem, 0, sizeof mem);
    mm_install_font(have_rom ? rom : NULL);

#ifndef NXDK
    dbg_fast = getenv("MM_FAST") != NULL;
    if (getenv("MM_EXIT_AFTER")) dbg_exit_after = atol(getenv("MM_EXIT_AFTER"));
    if (getenv("MM_ENTER_AT")) dbg_enter_at = atol(getenv("MM_ENTER_AT"));
    dbg_shots = getenv("MM_SHOTS");
    dbg_keys = getenv("MM_KEYS");
    dbg_teleport = getenv("MM_TELEPORT_AT");
    if (getenv("MM_DIAG")) diag_on = 1;
#endif
    zx_frame_hook = frame_hook;

    /* Look for the games. */
#ifdef HAVE_48
    static uint8_t image48[65536];
#endif
#ifdef HAVE_128
    static uint8_t image128[65536];
#endif
    int ok48 = 0, ok128 = 0;
    const char *err1 = NULL, *err2 = NULL;
#ifdef HAVE_48
    {
        long n = load_file(DATA_DIR "ManicMiner.bin", image48, sizeof image48);
        if (n == 65536 && image48[AppEntry] == 0xF3 &&
            !memcmp(image48 + CentralCavernData + 0x200 + 9, "Central Cavern", 14))
            ok48 = 1;
        else if (n >= 0) { err1 = "ManicMiner.bin isn't the 64K"; err2 = "Manic Miner image (see README)."; }
    }
#endif
#ifdef HAVE_128
    {
        static uint8_t tap[65536];
        long n = load_file(DATA_DIR "ManicMiner128.tap", tap, sizeof tap);
        if (n >= 0) {
            int r = mm128_build_image(tap, n, image128);
            if (r == MM128_OK) ok128 = 1;
            else if (r == MM128_ERR_TAP) { err1 = "ManicMiner128.tap: not a"; err2 = "valid TAP file."; }
            else { err1 = "Unsupported Manic Miner 128"; err2 = "tape (SP version needed)."; }
        }
    }
#endif
    if (!ok48 && !ok128) {
        if (!err1) {
#if defined(HAVE_48) && defined(HAVE_128)
            err1 = "Put ManicMiner.bin and/or"; err2 = "ManicMiner128.tap by the XBE.";
#elif defined(HAVE_128)
            err1 = "ManicMiner128.tap not found."; err2 = "See README for where to get it.";
#else
            err1 = "ManicMiner.bin not found."; err2 = "See README for where to get it.";
#endif
        }
        fatal_screen(err1, err2);
    }

    int choose128 = ok128 && !ok48;
#if defined(HAVE_48) && defined(HAVE_128)
    if (ok48 && ok128) {
        /* Both present: the chooser, built from Manic Miner's own title
         * screen (so it needs the 48K image in memory). */
        memcpy(mem + 0x4000, image48 + 0x4000, 0xC000);
        zx_set_model(ZX_MODEL_48K);
        choose128 = launcher();
    }
    v128 = choose128;
#endif
    (void)choose128;

    /* Start the chosen game from a clean machine. */
    zx_reset();
    stats_t0_us = 0;
    memset(mem, 0, sizeof mem);
    mm_install_font(have_rom ? rom : NULL);
    zx_in_filter = teleport_in_filter;
#ifdef HAVE_128
    if (v128) {
        zx_set_model(ZX_MODEL_128K);
        ay_init(&ay, AY_CLOCK_128K, AUDIO_RATE);
        memcpy(mem + 0x4000, image128 + 0x4000, 0xC000);   /* keep our ROM area */
        mm_run128();                                        /* never returns */
    }
#endif
#ifdef HAVE_48
    zx_set_model(ZX_MODEL_48K);
    memcpy(mem + 0x4000, image48 + 0x4000, 0xC000);
    mm_run48();                                             /* never returns */
#endif
    return 0;
}
