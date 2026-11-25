/*
 * difftest128.c - check the Manic Miner 128 build against the original.
 *
 * "ref" runs the unpacked tape image on a Z80 emulator with a 50 Hz IM2
 * interrupt (the AY music player), "port" runs the C translation. Both get
 * the same scripted input and print, at every pass through the main loop
 * (34622), a hash of the game's memory and the T-states since the last pass.
 * Each AY music tick is also logged (register set after the tick) so the
 * music can be compared tick for tick.
 *
 *   difftest128 ref|port <script> <passes> <ay_log_file>
 *
 * script: "demo" or "play:<seed>". MM_TAP points at the tape file.
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
#include "../src/mm128_load.h"
#include "z80ref/z80.h"

#define MAINLOOP_ADDR 34622
#define AFTER_PLAY    40103     /* POP IY, right after CALL 49158 */

static long pass_no = -1, max_passes;
static int scripted_play, idle_game;
static uint32_t rng;
static uint64_t last_t;
static FILE *aylog;

static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* ---- Input script ------------------------------------------------------- */
static int hold_left, hold_right, hold_jump, hold_enter, hold_joy;
/* TYPEWRITER: Q-T row bits / P-Y row bits */
static const struct { int row, bit; } typewriter[10] = {
    { ZX_ROW_Q_T, 4 }, { ZX_ROW_P_Y, 4 }, { ZX_ROW_P_Y, 0 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 1 },
    { ZX_ROW_Q_T, 3 }, { ZX_ROW_P_Y, 2 }, { ZX_ROW_Q_T, 4 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 3 },
};
static uint8_t menu_answers[4];
static int menu_answer_count;

static void set_inputs(long pass)
{
    zx_keys_clear();
    zx_kempston = 0;
    menu_answer_count = 0;
    if (!scripted_play) return;
    if (idle_game) { if (pass < 0) zx_key(ZX_ROW_ENTER_H, 0, 1); return; }
    if (pass < 0) { zx_key(ZX_ROW_ENTER_H, 0, 1); return; }

    long phase = pass % 600;
    if (phase >= 10 && phase < 20) {
        zx_key(typewriter[phase - 10].row, typewriter[phase - 10].bit, 1);
        return;
    }
    if (phase == 25 || phase == 26) {
        /* Teleport: hold '9' + cavern bits on 1-5. An odd cavern needs '1',
         * which also opens the options menu (answered below). */
        int cav = (int)((pass / 600) % 20);
        zx_key(ZX_ROW_0_6, 1, 1);
        for (int b = 0; b < 5; b++)
            if (cav & (1 << b)) zx_key(ZX_ROW_1_5, b, 1);
        /* Menu: first answer is whatever is held ('1' = lives toggle),
         * then resume with '4'. */
        menu_answers[0] = 0x17;            /* '4' */
        menu_answer_count = 1;
        return;
    }
    if (phase == 300) {
        /* Open the menu with '1' and toggle air ('2') and music ('3'),
         * then resume; at phase 450 the same again (toggling back). */
        zx_key(ZX_ROW_1_5, 0, 1);
        menu_answers[0] = 0x1D;            /* '2' */
        menu_answers[1] = 0x1B;            /* '3' */
        menu_answers[2] = 0x17;            /* '4' */
        menu_answer_count = 3;
        return;
    }
    if (phase == 450) {
        zx_key(ZX_ROW_1_5, 0, 1);
        menu_answers[0] = 0x1B;            /* '3' */
        menu_answers[1] = 0x1D;            /* '2' */
        menu_answers[2] = 0x1E;            /* '1' (lives) */
        menu_answers[3] = 0x17;            /* '4' */
        menu_answer_count = 4;
        return;
    }
    if (hold_left == 0 && hold_right == 0 && (rnd() % 4) == 0) {
        if (rnd() & 1) hold_left = 1 + rnd() % 12; else hold_right = 1 + rnd() % 12;
    }
    if (hold_jump == 0 && (rnd() % 7) == 0) hold_jump = 1 + rnd() % 2;
    if (hold_enter == 0 && (rnd() % 97) == 0) hold_enter = 1;
    if (hold_joy == 0 && (rnd() % 50) == 0) hold_joy = 1 + rnd() % 8;
    if (hold_left)  { hold_left--;  zx_key(ZX_ROW_P_Y, 1, 1); }        /* O */
    if (hold_right) { hold_right--; zx_key(ZX_ROW_P_Y, 0, 1); }        /* P */
    if (hold_jump)  { hold_jump--;  zx_key(ZX_ROW_SPACE_B, 2, 1); }    /* M */
    if (hold_enter) { hold_enter--; zx_key(ZX_ROW_ENTER_H, 0, 1); }
    if (hold_joy)   { hold_joy--;   zx_kempston = (uint8_t)(rnd() & 0x13); }
    uint32_t r = rnd() % 400;
    if (r == 0) { zx_key(ZX_ROW_A_G, 0, 1); zx_key(ZX_ROW_P_Y, 0, 1); }  /* pause + P */
    if (r == 1) zx_key(ZX_ROW_SHIFT_V, 1, 1);                          /* Z */
    if (r == 2) zx_key(ZX_ROW_0_6, 0, 1);                              /* 0 = jump */
    if (r == 3) zx_key(ZX_ROW_0_6, 3, 1);                              /* 7 = right */
    if (r == 4) zx_key(ZX_ROW_0_6, 4, 1);                              /* 6 = left */
}

/* Runs of back-to-back reads of the 1-5 row are the menu's key loop (the
 * first is MoveWilly2's own check for '1'). Answer them from the script;
 * both implementations make exactly the same sequence of IN calls. */
static uint16_t prev_port;
static int run_len;
static uint8_t in_filter(uint16_t port, uint8_t v)
{
    if (port == 0xF7FE && prev_port == 0xF7FE) run_len++;
    else run_len = 0;
    prev_port = port;
    if (port == 0xF7FE && run_len >= 2 && menu_answer_count) {
        int i = run_len - 2;
        if (i >= menu_answer_count) i = menu_answer_count - 1;
        return (uint8_t)(0xA0 | menu_answers[i]);
    }
    return v;
}

/* ---- Hashing ------------------------------------------------------------ */
static int excluded(uint32_t a)
{
    if (a >= 0x5B00 && a < 0x5C00) return 1;        /* printer buffer */
    if (a >= 40114 && a < 40192) return 1;           /* stack (below MemTop) */
    if (a >= 49264 && a <= 49339 + 1) return 1;      /* music player state */
    if (a >= 49980 && a <= 49988) return 1;          /* player's patched operands */
    return 0;
}
static uint64_t mem_hash(const uint8_t *m)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0x4000; a < 0x10000; a++)
        if (!excluded(a)) h = (h ^ m[a]) * 1099511628211ull;
    return h;
}
static void at_mainloop(const uint8_t *m, uint64_t t)
{
    pass_no++;
    printf("%ld %016llx %llu %d %d %d %d\n", pass_no, (unsigned long long)mem_hash(m),
           (unsigned long long)(t - last_t), m[0x8407], m[0x8457], m[0x845A], m[40113]);
    last_t = t;
    if (pass_no >= max_passes) { fflush(stdout); if (aylog) fclose(aylog); exit(0); }
    set_inputs(pass_no);
}
static const uint8_t *dbg_mem;
static void log_ay(const uint8_t *regs)
{
    if (getenv("ST_DEBUG") && dbg_mem) {
        fprintf(aylog, "  st:");
        for (int a = 49264; a <= 49326; a++) fprintf(aylog, "%02x", dbg_mem[a]);
        fprintf(aylog, " %02x%02x\n", dbg_mem[49980], dbg_mem[49988]);
    }
    /* Channel A's mixer bits come from a stale register when A is silent:
     * mask them in that case (inaudible). */
    uint8_t r7 = regs[7];
    if ((regs[8] & 0x1F) == 0) r7 &= (uint8_t)~0x09;
    for (int i = 0; i < 14; i++) fprintf(aylog, "%02x", i == 7 ? r7 : regs[i]);
    fputc('\n', aylog);
}

/* ---- Port side ---------------------------------------------------------- */
static void port_hook(void) { at_mainloop(mem, zx_t); }
static void port_tick(void) { log_ay(zx_ay_regs); }
static int ref_sp = -1, ref_pc = -1;
static void check_stall(uint64_t t)
{
    if (t - last_t > 60ull * 50 * ZX_T_PER_FRAME) {
        printf("STALL after pass %ld", pass_no);
        if (ref_sp >= 0) printf(" (ref SP=%d PC=%d)", ref_sp, ref_pc);
        printf("\n");
        exit(3);
    }
}
static void port_frame(void) { check_stall(zx_t); }

/* ---- Reference side ---------------------------------------------------- */
static uint8_t refmem[65536], ref_ay[16];
static int ref_ay_sel;
static uint8_t rb(void *u, uint16_t a) { (void)u; return refmem[a]; }
static void wb(void *u, uint16_t a, uint8_t v) { (void)u; if (a >= 0x4000) refmem[a] = v; }
static uint8_t pin(z80 *z, uint16_t port)
{
    (void)z;
    if ((port & 0xC002) == 0xC000) return ref_ay[ref_ay_sel & 15];
    return zx_in(port);
}
static void pout(z80 *z, uint16_t port, uint8_t v)
{
    (void)z;
    if ((port & 0xC002) == 0xC000) { ref_ay_sel = v; return; }
    if ((port & 0xC002) == 0x8000) { ref_ay[ref_ay_sel & 15] = v; return; }
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: difftest128 ref|port script passes aylog\n"); return 2; }
    const char *tap_path = getenv("MM_TAP") ? getenv("MM_TAP") : "ManicMiner128.tap";
    static uint8_t tap[65536];
    FILE *f = fopen(tap_path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", tap_path); return 2; }
    long n = (long)fread(tap, 1, sizeof tap, f);
    fclose(f);
    if (!strcmp(argv[2], "idle")) { scripted_play = 1; idle_game = 1; }
    if (!strncmp(argv[2], "play:", 5)) { scripted_play = 1; rng = (uint32_t)atoi(argv[2] + 5); }
    max_passes = atol(argv[3]);
    aylog = fopen(argv[4], "w");

    zx_reset();
    /* The reference Z80 core has no ULA contention: compare like with like. */
    zx_contention = (getenv("MM_CONTEND") && *getenv("MM_CONTEND") == '1') ? 1 : 0;
    zx_set_model(ZX_MODEL_128K);
    if (mm128_build_image(tap, n, mem) != MM128_OK) { fprintf(stderr, "bad tape\n"); return 2; }
    memset(mem, 0, 0x4000);
    mm_install_font(NULL);
    zx_in_filter = in_filter;
    set_inputs(-1);

    if (!strcmp(argv[1], "port")) {
        mm_mainloop_hook = port_hook;
        mm_music_tick_hook = port_tick;
        dbg_mem = mem;
        zx_frame_hook = port_frame;
        mm_run();
        return 1;
    }

    memcpy(refmem, mem, 65536);
    dbg_mem = refmem;
    refmem[0x38] = 0xFB; refmem[0x39] = 0xC9;      /* IM 1: EI; RET */
    z80 z;
    z80_init(&z);
    z.read_byte = rb; z.write_byte = wb; z.port_in = pin; z.port_out = pout;
    z.pc = 33792; z.sp = 0x752E; z.interrupt_mode = 1;
    unsigned long next_int = ZX_T_PER_FRAME;
    static uint16_t ring[256]; static uint16_t ring_sp[256]; unsigned ri = 0;
    long trace_pass = getenv("TRACE_PASS") ? atol(getenv("TRACE_PASS")) : -1;
    for (;;) {
        if (trace_pass >= 0 && pass_no >= trace_pass) {
            ring[ri & 255] = z.pc; ring_sp[ri & 255] = z.sp; ri++;
            if ((z.pc < 0x4000 && z.pc != 0x38 && z.pc != 0x39) || z.sp < 39000 || z.sp > 40192) {
                for (unsigned k = ri - 120; k != ri; k++) fprintf(stderr, "%u PC=%d SP=%d\n", k, ring[k & 255], ring_sp[k & 255]);
                exit(4);
            }
        }
        if (z.pc == MAINLOOP_ADDR) at_mainloop(refmem, z.cyc);
        if (z.pc == AFTER_PLAY) log_ay(ref_ay);
        if ((z.cyc & 0xFFFFF) < 24) { ref_sp = z.sp; ref_pc = z.pc; check_stall(z.cyc); }
        if (z.pc == MAINLOOP_ADDR && getenv("SP_TRACE") && (pass_no % 500) == 0) fprintf(stderr, "pass %ld SP=%d\n", pass_no, z.sp);
        z80_step(&z);
        if (z.cyc >= next_int) { next_int += ZX_T_PER_FRAME; if (z.iff1) z80_gen_int(&z, 0xFF); }
    }
}
