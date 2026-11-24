/*
 * zx.h - the bits of a 48K ZX Spectrum that the Manic Miner port needs.
 *
 * The game code (mm_game.c) works directly on a 64K memory array laid out
 * exactly like the Spectrum's: ROM at 0x0000, display file at 0x4000,
 * attributes at 0x5800, and the game itself from 0x5C00 upwards. It talks to
 * the "hardware" through zx_in()/zx_out() and advances a Z80 T-state clock
 * with zx_tick(), so the original timing (and the beeper music) is preserved.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZX_H
#define ZX_H

#include <stdint.h>

#define ZX_CPU_HZ        3500000u
#define ZX_T_PER_FRAME   69888u          /* 48K Spectrum: 224 T * 312 lines */

/* Full 64K address space, original memory map. */
extern uint8_t mem[65536];

/* T-state clock. */
extern uint64_t zx_t;
extern uint64_t zx_next_frame;
void zx_frame(void);                     /* called every ZX_T_PER_FRAME T-states */

/* Signed so the translation can express "JR not taken" style corrections. */
static inline void zx_tick(int32_t t)
{
    zx_t = (uint64_t)((int64_t)zx_t + t);
    if (zx_t >= zx_next_frame)
        zx_frame();
}

/* Port I/O, as seen by the game. */
uint8_t zx_in(uint16_t port);
/* Optional filter on every IN (used by the comparison test). */
extern uint8_t (*zx_in_filter)(uint16_t port, uint8_t value);
void    zx_out(uint16_t port, uint8_t value);

/* Keyboard matrix: 8 half-rows, bits 0-4, 0 = pressed (like the real ULA). */
extern uint8_t zx_keys[8];
/* Kempston joystick: bit0 right, 1 left, 2 down, 3 up, 4 fire (1 = active). */
extern uint8_t zx_kempston;
extern int     zx_kempston_present;

/* Half-row indices for zx_keys[] (address line A8..A15 that selects them). */
enum {
    ZX_ROW_SHIFT_V = 0,   /* 0xFEFE: SHIFT Z X C V   */
    ZX_ROW_A_G     = 1,   /* 0xFDFE: A S D F G       */
    ZX_ROW_Q_T     = 2,   /* 0xFBFE: Q W E R T       */
    ZX_ROW_1_5     = 3,   /* 0xF7FE: 1 2 3 4 5       */
    ZX_ROW_0_6     = 4,   /* 0xEFFE: 0 9 8 7 6       */
    ZX_ROW_P_Y     = 5,   /* 0xDFFE: P O I U Y       */
    ZX_ROW_ENTER_H = 6,   /* 0xBFFE: ENTER L K J H   */
    ZX_ROW_SPACE_B = 7    /* 0x7FFE: SPACE SYM M N B */
};
void zx_key(int row, int bit, int down);
void zx_keys_clear(void);

/* Border colour (last value written to port 0xFE, bits 0-2). */
extern uint8_t zx_border;

/* Beeper: speaker level changes recorded with T-state offsets within the
 * current frame. The host turns them into PCM in its frame hook. */
#define ZX_BEEPER_MAX 32768
typedef struct { uint32_t t; uint8_t level; } zx_beep_t;
extern zx_beep_t zx_beeps[ZX_BEEPER_MAX];
extern int       zx_beep_count;
extern uint8_t   zx_beep_level;          /* level at start of frame / now */

/* Render the display file + border into a 320x240 ARGB8888 buffer
 * (32-pixel border left/right, 24-pixel border top/bottom). flash_phase
 * selects the inverted half of the FLASH cycle. */
#define ZX_SCREEN_W 320
#define ZX_SCREEN_H 240
void zx_render(uint32_t *pixels, int pitch_pixels, int flash_phase);

/* Hook the host installs: called once per emulated frame (50 Hz), at the
 * end of the frame (beeper/AY events for the frame are complete). */
extern void (*zx_frame_hook)(void);

/* Maskable interrupt, raised at the start of every frame. The game code
 * keeps zx_iff/zx_im up to date at its DI/EI/IM instructions; the game
 * installs zx_int_hook to run its interrupt handler (Manic Miner 128's IM2
 * music player). The original Manic Miner runs with interrupts off. */
extern int  zx_iff, zx_im;
extern void (*zx_int_hook)(void);

/* AY-3-8912 (128K sound chip): register writes recorded with T-state
 * offsets within the current frame, plus the current register file. */
#define ZX_AY_MAX 4096
typedef struct { uint32_t t; uint8_t reg, val; } zx_ay_t;
extern zx_ay_t zx_ay_log[ZX_AY_MAX];
extern int     zx_ay_count;
extern uint8_t zx_ay_regs[16];
void zx_ay_write(uint8_t reg, uint8_t val);

void zx_reset(void);

#endif
