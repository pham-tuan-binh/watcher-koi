#pragma once

/**
 * The pond on the SenseCAP Watcher: the render interface in pond.h filled
 * in with the panel's DMA, the board's clock and random numbers, the sound
 * module and the log, plus the timer that ticks it.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Bring the pond up on the panel and start ticking it. Call with the
/// LVGL lock held: the tick is an LVGL timer, so the pond and the touch
/// input share a task and nothing needs locking between them.
void pond_port_init(void);

/// Draw one frame in every `n` ticks (1 to 4). The world keeps its pace.
void pond_port_set_draw_every(int n);

#ifdef __cplusplus
}
#endif
