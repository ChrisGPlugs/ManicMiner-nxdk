/*
 * difftest.c - check the C port against the original Z80 code.
 *
 * Runs ManicMiner.bin on a Z80 emulator ("ref") or runs the C translation
 * ("port") with the same scripted keyboard/joystick input, and prints a hash
 * of the game's memory (display file, buffers, variables, cavern data) plus
 * the T-state clock at every pass through the main loop (0x870E). Diff the two
 * outputs: any divergence in game state shows up at the first differing pass.
 *
 *   difftest ref  <script> <passes> [dump_pass dump_file]
 *   difftest port <script> <passes> [dump_pass dump_file]
 *
 * script: "finale" (as play, but warps Willy into the open portal of The
 *         Final Barrier at pass 5 to run the ending), "demo" (no input: title tune, banner, then demo mode through all
 *         caverns) or "play:<seed>" (random play, cheat code + teleports).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/zx.h"
#include "../src/mm_game.h"
#include "../src/mm_font.h"
#include "z80ref/z80.h"

#define MAINLOOP_ADDR 0x870E

static uint8_t bin[65536];
static long    pass_no = -1;
static long    max_passes;
static long    dump_pass = -1;
static const char *dump_file;
static int     scripted_play, idle_game;
static int     finale;
static uint32_t rng;
static uint64_t last_t;

static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* ---- Input script ------------------------------------------------------- */
static int  hold_left, hold_right, hold_jump, hold_enter, hold_joy;
static const char cheat_seq[] = "6031769";

static void press_digit(int d)
{
    /* 1-5: row 3 bits 0..4; 6-0: row 4, '0'=bit0 '9'=bit1 '8'=bit2 '7'=bit3 '6'=bit4 */
    if (d >= 1 && d <= 5) zx_key(ZX_ROW_1_5, d - 1, 1);
    else zx_key(ZX_ROW_0_6, d == 0 ? 0 : 10 - d, 1);
}

static void set_inputs(long pass)
{
    zx_keys_clear();
    zx_kempston = 0;
    if (!scripted_play) return;
    if (idle_game) { if (pass < 0) zx_key(ZX_ROW_ENTER_H, 0, 1); return; }              /* demo: never touch anything */

    if (pass < 0) { zx_key(ZX_ROW_ENTER_H, 0, 1); return; }   /* start game */

    long phase = pass % 600;
    if (phase >= 10 && phase < 17) {        /* type 6031769, one key per pass */
        press_digit(cheat_seq[phase - 10] - '0');
        return;
    }
    if (phase == 20 || phase == 21) {       /* teleport: hold 6 + cavern bits */
        int cav = (int)((pass / 600) % 20);
        zx_key(ZX_ROW_0_6, 4, 1);
        for (int b = 0; b < 5; b++)
            if (cav & (1 << b)) zx_key(ZX_ROW_1_5, b, 1);
        return;
    }
    if (hold_left == 0 && hold_right == 0 && (rnd() % 4) == 0) {
        if (rnd() & 1) hold_left = 1 + rnd() % 12; else hold_right = 1 + rnd() % 12;
    }
    if (hold_jump == 0 && (rnd() % 7) == 0) hold_jump = 1 + rnd() % 2;
    if (hold_enter == 0 && (rnd() % 97) == 0) hold_enter = 1;
    if (hold_joy == 0 && (rnd() % 50) == 0) hold_joy = 1 + rnd() % 8;

    if (hold_left)  { hold_left--;  zx_key(ZX_ROW_P_Y, 1, 1); }          /* O */
    if (hold_right) { hold_right--; zx_key(ZX_ROW_P_Y, 0, 1); }          /* P */
    if (hold_jump)  { hold_jump--;  zx_key(ZX_ROW_SPACE_B, 2, 1); }      /* M */
    if (hold_enter) { hold_enter--; zx_key(ZX_ROW_ENTER_H, 0, 1); }      /* ENTER */
    if (hold_joy)   { hold_joy--;   zx_kempston = (uint8_t)(rnd() & 0x13); }
    /* Pause (A) together with P: the pause loop sees P and resumes at once,
     * so the pause path is exercised without stalling the script. */
    if ((rnd() % 400) == 0) { zx_key(ZX_ROW_A_G, 0, 1); zx_key(ZX_ROW_P_Y, 0, 1); }
    if ((rnd() % 400) == 1) zx_key(ZX_ROW_SHIFT_V, 1, 1);                /* Z = jump */
}

/* ---- Hash of the compared memory --------------------------------------- */
static uint64_t mem_hash(const uint8_t *m)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0x4000; a < 0x10000; a++) {
        if (a >= 0x9C00 && a < 0x9D00) continue;    /* stack */
        if (a >= 0x5B00 && a < 0x5C00) continue;    /* printer buffer, unused */
        h = (h ^ m[a]) * 1099511628211ull;
    }
    return h;
}

static void at_mainloop(uint8_t *m, uint64_t t)
{
    pass_no++;
    if (finale && pass_no == 5) {
        /* Put Willy in the flashing portal of The Final Barrier (no cheat),
         * so the next pass runs the end-of-game celebration. */
        m[0x8407] = 19;                       /* CurrentCavernNumber */
        m[0x806C] = m[0x80B0];                /* WillysLocInAttrBuffer = portal */
        m[0x806D] = m[0x80B1];
        m[0x808F] |= 0x80;                    /* portal flashing */
        uint16_t portal = (uint16_t)(m[0x80B0] | (m[0x80B1] << 8));
        m[0x8068] = (uint8_t)((((portal - 0x5C00) >> 5) & 15) * 16);   /* pixel y x2 */
        m[0x806B] = 0;                        /* standing */
        m[0x806A] = 0;                        /* not moving */
    }
    if (pass_no == dump_pass) {
        FILE *f = fopen(dump_file, "wb");
        fwrite(m, 1, 65536, f);
        fclose(f);
    }
    /* pass, hash, T-states since last pass, cavern, lives, game mode */
    printf("%ld %016llx %llu %d %d %d\n", pass_no, (unsigned long long)mem_hash(m),
           (unsigned long long)(t - last_t), m[0x8407], m[0x8457], m[0x845A]);
    last_t = t;
    if (pass_no >= max_passes) { fflush(stdout); exit(0); }
    set_inputs(pass_no);
}

/* ---- Port side ---------------------------------------------------------- */
static void port_hook(void) { at_mainloop(mem, zx_t); }

/* Watchdog: if no main-loop pass happens for ~60 emulated seconds the
 * script has wedged the game (e.g. title screen with no input forever). */
static void check_stall(uint64_t t)
{
    if (t - last_t > 60ull * 50 * ZX_T_PER_FRAME) {
        printf("STALL after pass %ld\n", pass_no);
        exit(3);
    }
}
static void port_frame(void) { check_stall(zx_t); }

/* ---- Reference side ---------------------------------------------------- */
static uint8_t refmem[65536];
static uint8_t  rb(void *u, uint16_t a) { (void)u; return refmem[a]; }
static void     wb(void *u, uint16_t a, uint8_t v) { (void)u; if (a >= 0x4000) refmem[a] = v; }
static uint8_t  pin(z80 *z, uint16_t port) { (void)z; return zx_in(port); }
static void     pout(z80 *z, uint16_t port, uint8_t v) { (void)z; (void)port; (void)v; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: difftest ref|port script passes [dump_pass file]\n"); return 2; }
    const char *bin_path = getenv("MM_BIN") ? getenv("MM_BIN") : "ManicMiner.bin";
    FILE *f = fopen(bin_path, "rb");
    if (!f || fread(bin, 1, 65536, f) != 65536) { fprintf(stderr, "cannot read %s\n", bin_path); return 2; }
    fclose(f);
    if (!strcmp(argv[2], "idle")) { scripted_play = 1; idle_game = 1; }
    if (!strncmp(argv[2], "play:", 5)) { scripted_play = 1; rng = (uint32_t)atoi(argv[2] + 5); }
    if (!strcmp(argv[2], "finale")) { scripted_play = 1; finale = 1; rng = 99; }
    max_passes = atol(argv[3]);
    if (argc >= 6) { dump_pass = atol(argv[4]); dump_file = argv[5]; }

    zx_reset();
    /* The reference Z80 core has no ULA contention: compare like with like. */
    zx_contention = (getenv("MM_CONTEND") && *getenv("MM_CONTEND") == '1') ? 1 : 0;
    memcpy(mem, bin, 65536);
    mm_install_font(NULL);
    set_inputs(-1);

    if (!strcmp(argv[1], "port")) {
        mm_mainloop_hook = port_hook;
        zx_frame_hook = port_frame;
        mm_run();
        return 1;
    }

    memcpy(refmem, mem, 65536);
    z80 z;
    z80_init(&z);
    z.read_byte = rb; z.write_byte = wb; z.port_in = pin; z.port_out = pout;
    z.pc = 0x8400;
    for (;;) {
        if (z.pc == MAINLOOP_ADDR) at_mainloop(refmem, z.cyc);
        if ((z.cyc & 0xFFFFF) < 24) check_stall(z.cyc);
        z80_step(&z);
    }
}
