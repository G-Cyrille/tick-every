#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "timer_logic.h"

#define ARRAY_LENGTH(values) (sizeof(values) / sizeof((values)[0]))

static unsigned int s_assertion_count;

/* Stops at the first mismatch while preserving the failing source line. */
static void expect_equal(uint64_t actual, uint64_t expected,
                         const char *expression, int line) {
  ++s_assertion_count;
  if (actual != expected) {
    fprintf(stderr, "line %d: %s: expected %" PRIu64 ", got %" PRIu64 "\n",
            line, expression, expected, actual);
    exit(EXIT_FAILURE);
  }
}

#define EXPECT_EQ(actual, expected) \
  expect_equal((uint64_t)(actual), (uint64_t)(expected), #actual, __LINE__)

/* Independently identifies each value on the 1 s / 5 s / 15 s grid. */
static int is_selectable_timer(unsigned int value) {
  if (value < TICK_TIMER_MIN_SECONDS || value > TICK_TIMER_MAX_SECONDS) {
    return 0;
  }
  if (value <= 30U) {
    return 1;
  }
  if (value <= 120U) {
    return (value - 30U) % 5U == 0U;
  }
  return (value - 120U) % 15U == 0U;
}

/* Verifies grid thresholds and every value around the supported range. */
static void test_selectable_timers(void) {
  unsigned int value;

  EXPECT_EQ(tick_timer_is_selectable(0U), 0U);
  EXPECT_EQ(tick_timer_is_selectable(1U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(30U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(31U), 0U);
  EXPECT_EQ(tick_timer_is_selectable(35U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(120U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(121U), 0U);
  EXPECT_EQ(tick_timer_is_selectable(135U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(3600U), 1U);
  EXPECT_EQ(tick_timer_is_selectable(3601U), 0U);

  for (value = 0U; value <= 3700U; ++value) {
    EXPECT_EQ(tick_timer_is_selectable(value), is_selectable_timer(value));
  }
}

/* Returns the first legal timer strictly above the input. */
static unsigned int expected_timer_up(unsigned int value) {
  unsigned int candidate;
  for (candidate = TICK_TIMER_MIN_SECONDS;
       candidate <= TICK_TIMER_MAX_SECONDS; ++candidate) {
    if (candidate > value && is_selectable_timer(candidate)) {
      return candidate;
    }
  }
  return TICK_TIMER_MAX_SECONDS;
}

/* Returns the first legal timer strictly below the input. */
static unsigned int expected_timer_down(unsigned int value) {
  unsigned int candidate = TICK_TIMER_MAX_SECONDS;
  if (value > TICK_TIMER_MAX_SECONDS) {
    return TICK_TIMER_MAX_SECONDS;
  }
  while (candidate > TICK_TIMER_MIN_SECONDS) {
    if (candidate < value && is_selectable_timer(candidate)) {
      return candidate;
    }
    --candidate;
  }
  return TICK_TIMER_MIN_SECONDS;
}

/* Exercises threshold steps, saturation, off-grid repair, and round-trips. */
static void test_timer_adjustments(void) {
  unsigned int value;

  EXPECT_EQ(tick_timer_adjust_up(0U), 1U);
  EXPECT_EQ(tick_timer_adjust_down(0U), 1U);
  EXPECT_EQ(tick_timer_adjust_up(29U), 30U);
  EXPECT_EQ(tick_timer_adjust_up(30U), 35U);
  EXPECT_EQ(tick_timer_adjust_down(30U), 29U);
  EXPECT_EQ(tick_timer_adjust_down(31U), 30U);
  EXPECT_EQ(tick_timer_adjust_up(119U), 120U);
  EXPECT_EQ(tick_timer_adjust_up(120U), 135U);
  EXPECT_EQ(tick_timer_adjust_down(120U), 115U);
  EXPECT_EQ(tick_timer_adjust_down(121U), 120U);
  EXPECT_EQ(tick_timer_adjust_up(3599U), 3600U);
  EXPECT_EQ(tick_timer_adjust_up(3600U), 3600U);
  EXPECT_EQ(tick_timer_adjust_down(3601U), 3600U);

  for (value = 0U; value <= 3700U; ++value) {
    const unsigned int up = tick_timer_adjust_up(value);
    const unsigned int down = tick_timer_adjust_down(value);
    EXPECT_EQ(up, expected_timer_up(value));
    EXPECT_EQ(down, expected_timer_down(value));
    EXPECT_EQ(is_selectable_timer(up), 1U);
    EXPECT_EQ(is_selectable_timer(down), 1U);
  }

  for (value = TICK_TIMER_MIN_SECONDS;
       value <= TICK_TIMER_MAX_SECONDS; ++value) {
    if (!is_selectable_timer(value)) {
      continue;
    }
    if (value < TICK_TIMER_MAX_SECONDS) {
      EXPECT_EQ(tick_timer_adjust_down(tick_timer_adjust_up(value)), value);
    }
    if (value > TICK_TIMER_MIN_SECONDS) {
      EXPECT_EQ(tick_timer_adjust_up(tick_timer_adjust_down(value)), value);
    }
  }
}

/* Checks fixed delay choices, saturation, and off-grid movement. */
static void test_delays(void) {
  static const unsigned int delays[] = {0U, 5U, 10U, 15U, 30U, 60U};
  unsigned int index;

  EXPECT_EQ(tick_delay_is_selectable(0U), 1U);
  EXPECT_EQ(tick_delay_is_selectable(6U), 0U);
  EXPECT_EQ(tick_delay_is_selectable(60U), 1U);
  EXPECT_EQ(tick_delay_next(0U), 5U);
  EXPECT_EQ(tick_delay_next(6U), 10U);
  EXPECT_EQ(tick_delay_next(60U), 60U);
  EXPECT_EQ(tick_delay_previous(0U), 0U);
  EXPECT_EQ(tick_delay_previous(6U), 5U);
  EXPECT_EQ(tick_delay_previous(61U), 60U);

  for (index = 0U; index + 1U < ARRAY_LENGTH(delays); ++index) {
    EXPECT_EQ(tick_delay_next(delays[index]), delays[index + 1U]);
    EXPECT_EQ(tick_delay_previous(delays[index + 1U]), delays[index]);
  }
}

/* Verifies required examples and uint32 decimal-decomposition bounds. */
static void test_cycle_patterns(void) {
  TickCyclePattern pattern = {99U, 99U};

  EXPECT_EQ(tick_cycle_pattern(0U, &pattern), 0U);
  EXPECT_EQ(pattern.long_vibrations, 99U);
  EXPECT_EQ(pattern.short_vibrations, 99U);
  EXPECT_EQ(tick_cycle_pattern(1U, NULL), 0U);

  EXPECT_EQ(tick_cycle_pattern(1U, &pattern), 1U);
  EXPECT_EQ(pattern.long_vibrations, 0U);
  EXPECT_EQ(pattern.short_vibrations, 1U);
  EXPECT_EQ(tick_cycle_pattern(9U, &pattern), 1U);
  EXPECT_EQ(pattern.long_vibrations, 0U);
  EXPECT_EQ(pattern.short_vibrations, 9U);
  EXPECT_EQ(tick_cycle_pattern(12U, &pattern), 1U);
  EXPECT_EQ(pattern.long_vibrations, 1U);
  EXPECT_EQ(pattern.short_vibrations, 2U);
  EXPECT_EQ(tick_cycle_pattern(55U, &pattern), 1U);
  EXPECT_EQ(pattern.long_vibrations, 5U);
  EXPECT_EQ(pattern.short_vibrations, 5U);
  EXPECT_EQ(tick_cycle_pattern(UINT32_MAX, &pattern), 1U);
  EXPECT_EQ(pattern.long_vibrations, UINT32_MAX / 10U);
  EXPECT_EQ(pattern.short_vibrations, UINT32_MAX % 10U);
}

/* Checks the required D=10/X=5 timeline, late wakes, and no final clamp. */
static void test_repeating_cycles(void) {
  const uint64_t delay_ms = 10000U;

  EXPECT_EQ(tick_cycles_reached_ms(0U, 5U), 0U);
  EXPECT_EQ(tick_cycles_reached_ms(4999U, 5U), 0U);
  EXPECT_EQ(tick_cycles_reached_ms(5000U, 5U), 1U);   /* wall D+5 = 15 s */
  EXPECT_EQ(tick_cycles_reached_ms(10000U, 5U), 2U);  /* wall D+10 = 20 s */
  EXPECT_EQ(tick_cycles_reached_ms(15000U, 5U), 3U);  /* wall D+15 = 25 s */
  EXPECT_EQ(delay_ms + 5000U, 15000U);
  EXPECT_EQ(tick_cycles_reached_ms(27999U, 5U), 5U);  /* one late update */
  EXPECT_EQ(tick_cycles_reached_ms(100000U, 5U), 20U); /* no duration clamp */
  EXPECT_EQ(tick_cycles_reached_ms(1000U, 0U), 0U);
  EXPECT_EQ(tick_cycles_reached_ms(UINT64_MAX, 3600U),
            UINT64_MAX / 3600000U);
}

/* Ensures exact phases, saturation, and overflow-safe scheduling arithmetic. */
static void test_elapsed_and_scheduling(void) {
  EXPECT_EQ(tick_elapsed_seconds_from_ms(0U), 0U);
  EXPECT_EQ(tick_elapsed_seconds_from_ms(999U), 0U);
  EXPECT_EQ(tick_elapsed_seconds_from_ms(1000U), 1U);
  EXPECT_EQ(tick_elapsed_seconds_from_ms(3599999U), 3599U);
  EXPECT_EQ(tick_elapsed_seconds_from_ms((uint64_t)UINT_MAX * 1000U),
            UINT_MAX);
  EXPECT_EQ(tick_elapsed_seconds_from_ms(UINT64_MAX), UINT_MAX);

  EXPECT_EQ(tick_next_second_delay_ms(0U), 1000U);
  EXPECT_EQ(tick_next_second_delay_ms(50U), 950U);
  EXPECT_EQ(tick_next_second_delay_ms(950U), 50U);
  EXPECT_EQ(tick_next_second_delay_ms(1000U), 1000U);
  EXPECT_EQ(tick_next_second_delay_ms(4999U), 1U);
  EXPECT_EQ(tick_next_second_delay_ms(UINT64_MAX),
            1000U - (unsigned int)(UINT64_MAX % 1000U));
}

/* Runs the host-side pure-C regression suite. */
int main(void) {
  test_selectable_timers();
  test_timer_adjustments();
  test_delays();
  test_cycle_patterns();
  test_repeating_cycles();
  test_elapsed_and_scheduling();
  printf("timer_logic: %u assertions passed\n", s_assertion_count);
  return EXIT_SUCCESS;
}
