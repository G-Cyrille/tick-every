#include "timer_logic.h"

#include <limits.h>
#include <stddef.h>

static const unsigned int TICK_DELAYS[] = {0U, 5U, 10U, 15U, 30U, 60U};
#define TICK_DELAY_COUNT (sizeof(TICK_DELAYS) / sizeof(TICK_DELAYS[0]))

/* Validates the timer values exposed by the three-part selector. */
int tick_timer_is_selectable(unsigned int timer_seconds) {
  if (timer_seconds < TICK_TIMER_MIN_SECONDS ||
      timer_seconds > TICK_TIMER_MAX_SECONDS) {
    return 0;
  }
  if (timer_seconds <= 30U) {
    return 1;
  }
  if (timer_seconds <= 120U) {
    return (timer_seconds - 30U) % 5U == 0U;
  }
  return (timer_seconds - 120U) % 15U == 0U;
}

/* Advances to the next point on the 1 s / 5 s / 15 s timer grid. */
unsigned int tick_timer_adjust_up(unsigned int timer_seconds) {
  if (timer_seconds < TICK_TIMER_MIN_SECONDS) {
    return TICK_TIMER_MIN_SECONDS;
  }
  if (timer_seconds < 30U) {
    return timer_seconds + 1U;
  }
  if (timer_seconds < 120U) {
    return 30U + ((((timer_seconds - 30U) / 5U) + 1U) * 5U);
  }
  if (timer_seconds < TICK_TIMER_MAX_SECONDS) {
    return 120U + ((((timer_seconds - 120U) / 15U) + 1U) * 15U);
  }
  return TICK_TIMER_MAX_SECONDS;
}

/* Moves back to the preceding point on the timer grid. */
unsigned int tick_timer_adjust_down(unsigned int timer_seconds) {
  if (timer_seconds <= TICK_TIMER_MIN_SECONDS) {
    return TICK_TIMER_MIN_SECONDS;
  }
  if (timer_seconds <= 30U) {
    return timer_seconds - 1U;
  }
  if (timer_seconds <= 120U) {
    return 30U + (((timer_seconds - 31U) / 5U) * 5U);
  }
  if (timer_seconds <= TICK_TIMER_MAX_SECONDS) {
    return 120U + (((timer_seconds - 121U) / 15U) * 15U);
  }
  return TICK_TIMER_MAX_SECONDS;
}

/* Validates one of the fixed start-delay choices. */
int tick_delay_is_selectable(unsigned int delay_seconds) {
  unsigned int index;
  for (index = 0U; index < TICK_DELAY_COUNT; ++index) {
    if (TICK_DELAYS[index] == delay_seconds) {
      return 1;
    }
  }
  return 0;
}

/* Advances within the small fixed set of supported start delays. */
unsigned int tick_delay_next(unsigned int delay_seconds) {
  unsigned int index;
  for (index = 0U; index < TICK_DELAY_COUNT; ++index) {
    if (TICK_DELAYS[index] > delay_seconds) {
      return TICK_DELAYS[index];
    }
  }
  return TICK_DELAYS[TICK_DELAY_COUNT - 1U];
}

/* Moves back within the small fixed set of supported start delays. */
unsigned int tick_delay_previous(unsigned int delay_seconds) {
  unsigned int index;
  for (index = TICK_DELAY_COUNT; index > 0U; --index) {
    if (TICK_DELAYS[index - 1U] < delay_seconds) {
      return TICK_DELAYS[index - 1U];
    }
  }
  return TICK_DELAYS[0];
}

/* Encodes a cycle number as groups of five plus a one-to-four remainder. */
int tick_cycle_pattern(uint32_t cycle, TickCyclePattern *out_pattern) {
  if (out_pattern == NULL || cycle == 0U) {
    return 0;
  }
  out_pattern->long_vibrations = cycle / 5U;
  out_pattern->short_vibrations = cycle % 5U;
  return 1;
}

/* Counts one ON segment per pulse and one OFF segment between pulses. */
uint64_t tick_cycle_pattern_segment_count(const TickCyclePattern *pattern) {
  uint64_t pulse_count;
  if (pattern == NULL) {
    return 0U;
  }
  pulse_count = (uint64_t)pattern->long_vibrations +
                pattern->short_vibrations;
  return pulse_count == 0U ? 0U : pulse_count * 2U - 1U;
}

/* Includes the longer quiet gap that keeps adjacent pulses distinguishable. */
uint64_t tick_cycle_pattern_duration_ms(const TickCyclePattern *pattern) {
  uint64_t pulse_count;
  if (pattern == NULL) {
    return 0U;
  }
  pulse_count = (uint64_t)pattern->long_vibrations +
                pattern->short_vibrations;
  if (pulse_count == 0U) {
    return 0U;
  }
  return (uint64_t)pattern->long_vibrations * TICK_HAPTIC_LONG_PULSE_MS +
         (uint64_t)pattern->short_vibrations * TICK_HAPTIC_SHORT_PULSE_MS +
         (pulse_count - 1U) * TICK_HAPTIC_GAP_MS;
}

/* Counts every completed repeat boundary, including arbitrarily late wakes. */
uint64_t tick_cycles_reached_ms(uint64_t elapsed_ms,
                                unsigned int timer_seconds) {
  if (timer_seconds == 0U) {
    return 0U;
  }
  return elapsed_ms / ((uint64_t)timer_seconds * 1000U);
}

/* Converts exact elapsed time to display seconds without wrapping uint32. */
unsigned int tick_elapsed_seconds_from_ms(uint64_t elapsed_ms) {
  const uint64_t seconds = elapsed_ms / 1000U;
  return seconds > UINT_MAX ? UINT_MAX : (unsigned int)seconds;
}

/* Preserves the launch phase when arming the next whole-second wake-up. */
unsigned int tick_next_second_delay_ms(uint64_t elapsed_ms) {
  const unsigned int phase_ms = (unsigned int)(elapsed_ms % 1000U);
  return phase_ms == 0U ? 1000U : 1000U - phase_ms;
}
