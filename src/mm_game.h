/*
 * mm_game.h - entry point of the translated game.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MM_GAME_H
#define MM_GAME_H

/* Run Manic Miner from AppEntry (0x8400). Never returns; the host gets
 * control back every emulated frame through zx_frame_hook. */
void mm_run(void);

/* Optional hook called at the top of every pass through the main loop
 * (address 0x870E in the original). Used by the comparison test. */
extern void (*mm_mainloop_hook)(void);

/* Optional hook called after each AY music tick (MM128 only; test use). */
extern void (*mm_music_tick_hook)(void);

#endif
