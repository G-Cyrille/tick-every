#ifndef TIMER_LOGIC_H
#define TIMER_LOGIC_H

#include <stdint.h>

#define TICK_TIMER_MIN_SECONDS 1U
#define TICK_TIMER_MAX_SECONDS 3600U
#define TICK_HAPTIC_SHORT_PULSE_MS 120U
#define TICK_HAPTIC_LONG_PULSE_MS 300U
#define TICK_HAPTIC_GAP_MS 100U

/* Describes the base-five haptic code for one completed timer cycle. */
typedef struct {
  uint32_t long_vibrations;
  uint32_t short_vibrations;
} TickCyclePattern;

/* Returns 1 only for values exposed by the three-part timer grid. */
int tick_timer_is_selectable(unsigned int timer_seconds);

/*
 * Advances to the next selectable timer, saturating at 3600 s.
 * The grid uses 1 s steps through 30, 5 s through 120, then 15 s.
 * An off-grid input advances to the first selectable value above it.
 */
unsigned int tick_timer_adjust_up(unsigned int timer_seconds);

/* Moves to the preceding timer grid value, saturating at 1 s. */
unsigned int tick_timer_adjust_down(unsigned int timer_seconds);

/* Returns 1 only for one of the six start-delay choices. */
int tick_delay_is_selectable(unsigned int delay_seconds);

/* Returns the first supported delay strictly above the input, capped at 60 s. */
unsigned int tick_delay_next(unsigned int delay_seconds);

/* Returns the first supported delay strictly below the input, floored at 0 s. */
unsigned int tick_delay_previous(unsigned int delay_seconds);

/*
 * Splits every non-zero uint32 cycle into groups of five and a remainder.
 * One long vibration represents five cycles; short vibrations represent the
 * remaining one to four cycles.
 * Physical pattern-size limits are deliberately enforced by the watch app.
 */
int tick_cycle_pattern(uint32_t cycle, TickCyclePattern *out_pattern);

/* Returns the alternating ON/OFF segment count, or zero for an empty code. */
uint64_t tick_cycle_pattern_segment_count(const TickCyclePattern *pattern);

/* Returns the exact duration of one base-five pattern, including quiet gaps. */
uint64_t tick_cycle_pattern_duration_ms(const TickCyclePattern *pattern);

/* Returns elapsed / timer without a final-duration clamp. */
uint64_t tick_cycles_reached_ms(uint64_t elapsed_ms,
                                unsigned int timer_seconds);

/* Floors an exact millisecond measurement for display, saturating safely. */
unsigned int tick_elapsed_seconds_from_ms(uint64_t elapsed_ms);

/* Returns 1..1000 ms to the next second relative to the timer's own phase. */
unsigned int tick_next_second_delay_ms(uint64_t elapsed_ms);

#endif
