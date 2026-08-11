#include <pebble.h>
#include <message_keys.auto.h>

#include "session_history.h"
#include "timer_logic.h"

/* Key 1 stored the retired finite duration; key 4 stores the repeat interval. */
#define PERSIST_KEY_TIMER 4
#define PERSIST_KEY_DELAY 2
#define PERSIST_KEY_HAPTICS 3
#define PERSIST_KEY_LANGUAGE 5
#define PERSIST_KEY_STATISTICS_ENABLED 6
#define PERSIST_KEY_HISTORY_A_FIRST 100
#define PERSIST_KEY_HISTORY_B_FIRST 110

#define DEFAULT_TIMER_SECONDS 5U
#define DEFAULT_DELAY_SECONDS 0U
#define DEFAULT_HAPTICS_ENABLED true
#define DEFAULT_LANGUAGE LANGUAGE_ENGLISH
#define DEFAULT_STATISTICS_ENABLED false

#define LONG_PRESS_MS 700U
#define ANIMATION_STEP_MS 70U
#define PULSE_FRAME_COUNT 7U
/* 32 pulses use 63 alternating vibration/gap segments, our hard API limit. */
#define HAPTIC_SEGMENT_CAPACITY 63U
#define HAPTIC_MAX_PULSES ((HAPTIC_SEGMENT_CAPACITY + 1U) / 2U)
#define HAPTIC_DEADLINE_MARGIN_MS 20U
#define HAPTIC_SHORT_PULSE_MS 120U
#define HAPTIC_LONG_PULSE_MS 300U
#define HAPTIC_GAP_MS 50U

typedef enum {
  APP_STATE_SET_TIMER = 0,
  APP_STATE_SET_DELAY,
  APP_STATE_SET_HAPTICS,
  APP_STATE_READY,
  APP_STATE_WAITING,
  APP_STATE_RUNNING,
  APP_STATE_PAUSED,
  APP_STATE_STOP_CONFIRM,
  APP_STATE_HISTORY
} AppState;

typedef enum {
  LANGUAGE_ENGLISH = 0,
  LANGUAGE_FRENCH
} Language;

typedef enum {
  ANIMATION_NONE = 0,
  ANIMATION_PULSE
} AnimationKind;

/* Keeps the sub-second phase that time(NULL) would otherwise discard. */
typedef struct {
  time_t seconds;
  uint16_t milliseconds;
} WallClockTime;

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_animation_timer;
static AppTimer *s_runtime_timer;

static AppState s_state = APP_STATE_SET_TIMER;
static AppState s_state_before_stop_confirm = APP_STATE_RUNNING;
static AnimationKind s_animation_kind = ANIMATION_NONE;
static unsigned int s_animation_frame;

static unsigned int s_timer_seconds = DEFAULT_TIMER_SECONDS;
static unsigned int s_delay_seconds = DEFAULT_DELAY_SECONDS;
static bool s_haptics_enabled = DEFAULT_HAPTICS_ENABLED;
static bool s_statistics_enabled = DEFAULT_STATISTICS_ENABLED;
static Language s_language = DEFAULT_LANGUAGE;
static unsigned int s_elapsed_seconds;
static uint64_t s_cycle;
static bool s_haptic_limit_logged;
static TickHistory s_history;
static uint8_t s_history_bank;
static unsigned int s_history_offset;
static bool s_session_active_started;
static bool s_statistics_enabled_at_session_start;
static bool s_stop_snapshot_valid;
static bool s_history_send_pending;
static bool s_history_send_in_flight;
static unsigned int s_history_send_chunk;
static uint8_t s_history_send_generation;
static WallClockTime s_session_started_at;
static WallClockTime s_stop_snapshot_at;
static uint64_t s_stop_snapshot_active_ms;
static uint64_t s_stop_snapshot_cycle;

/* A running segment starts after each resume; exact prior elapsed is preserved. */
static WallClockTime s_wait_started_at;
static WallClockTime s_run_segment_started_at;
static uint64_t s_elapsed_before_segment_ms;

/* Pebble consumes custom patterns asynchronously, so storage must outlive calls. */
static uint32_t s_haptic_durations[HAPTIC_SEGMENT_CAPACITY];
static uint8_t s_history_chunks[TICK_HISTORY_CHUNK_COUNT]
                               [PERSIST_DATA_MAX_LENGTH];

static void prv_runtime_timer_callback(void *context);
static void prv_reconcile_runtime(void);

/* Returns a stable label for transition logs. */
static const char *prv_state_name(AppState state) {
  switch (state) {
    case APP_STATE_SET_TIMER: return "timer";
    case APP_STATE_SET_DELAY: return "delay";
    case APP_STATE_SET_HAPTICS: return "haptics";
    case APP_STATE_READY: return "ready";
    case APP_STATE_WAITING: return "waiting";
    case APP_STATE_RUNNING: return "running";
    case APP_STATE_PAUSED: return "paused";
    case APP_STATE_STOP_CONFIRM: return "stop-confirm";
    case APP_STATE_HISTORY: return "history";
  }
  return "unknown";
}

/* Selects the active translation without allocating localized strings. */
static const char *prv_text(const char *english, const char *french) {
  return s_language == LANGUAGE_FRENCH ? french : english;
}

/* Invalidates the single custom layer when it is currently loaded. */
static void prv_mark_dirty(void) {
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

/* Changes state in one place so every transition remains visible in logs. */
static void prv_set_state(AppState next_state) {
  if (s_state != next_state) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "State: %s -> %s",
            prv_state_name(s_state), prv_state_name(next_state));
    s_state = next_state;
  }
  prv_mark_dirty();
}

/* Cancels the only animation timer owned by the app. */
static void prv_cancel_animation(void) {
  if (s_animation_timer) {
    app_timer_cancel(s_animation_timer);
    s_animation_timer = NULL;
  }
  s_animation_kind = ANIMATION_NONE;
  s_animation_frame = 0U;
}

/* Cancels the exact runtime wake-up without touching visual animations. */
static void prv_cancel_runtime_timer(void) {
  if (s_runtime_timer) {
    app_timer_cancel(s_runtime_timer);
    s_runtime_timer = NULL;
  }
}

/* Advances the pulse drawing without allocating extra layers. */
static void prv_animation_timer_callback(void *context) {
  s_animation_timer = NULL;
  if (s_animation_frame > 0U) {
    --s_animation_frame;
  }
  prv_mark_dirty();

  if (s_animation_frame > 0U && s_canvas_layer) {
    s_animation_timer = app_timer_register(ANIMATION_STEP_MS,
                                           prv_animation_timer_callback, NULL);
    if (!s_animation_timer) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to continue animation timer");
      s_animation_kind = ANIMATION_NONE;
      s_animation_frame = 0U;
    }
  } else {
    s_animation_kind = ANIMATION_NONE;
  }
}

/* Starts one short visual animation, replacing any animation in progress. */
static void prv_start_animation(AnimationKind kind) {
  prv_cancel_animation();
  s_animation_kind = kind;
  s_animation_frame = PULSE_FRAME_COUNT;
  prv_mark_dirty();
  if (!s_canvas_layer) {
    return;
  }
  s_animation_timer = app_timer_register(ANIMATION_STEP_MS,
                                         prv_animation_timer_callback, NULL);
  if (!s_animation_timer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate animation timer");
    s_animation_kind = ANIMATION_NONE;
    s_animation_frame = 0U;
  }
}

/* Persists one integer and logs storage failures instead of hiding them. */
static void prv_persist_int_checked(int key, int value, const char *name) {
  const int result = persist_write_int(key, value);
  if (result < 0) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to persist %s: %d", name, result);
  } else {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Persisted %s=%d", name, value);
  }
}

/* Writes a complete generation into the inactive A/B bank, first chunk last. */
static bool prv_persist_history(void) {
  const uint8_t target_bank = (uint8_t)(s_history_bank ^ 1U);
  const int bank_first_key = target_bank == 0U
      ? PERSIST_KEY_HISTORY_A_FIRST : PERSIST_KEY_HISTORY_B_FIRST;
  size_t chunk_sizes[TICK_HISTORY_CHUNK_COUNT];
  unsigned int chunk;
  int result;

  for (chunk = 0U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    chunk_sizes[chunk] = tick_history_encode_chunk(
        &s_history, chunk, s_history_chunks[chunk],
        sizeof(s_history_chunks[chunk]));
    if (chunk == 0U && chunk_sizes[chunk] == 0U) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to encode history commit chunk");
      return false;
    }
  }

  for (chunk = TICK_HISTORY_CHUNK_COUNT - 1U; chunk > 0U; --chunk) {
    if (chunk_sizes[chunk] == 0U) {
      continue;
    }
    result = persist_write_data(bank_first_key + (int)chunk,
                                s_history_chunks[chunk], chunk_sizes[chunk]);
    if (result != (int)chunk_sizes[chunk]) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Failed to persist history chunk %u: %d", chunk, result);
      return false;
    }
  }

  result = persist_write_data(bank_first_key, s_history_chunks[0],
                              chunk_sizes[0]);
  if (result != (int)chunk_sizes[0]) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "Failed to persist history commit chunk: %d", result);
    return false;
  }

  s_history_bank = target_bank;
  for (chunk = 1U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    const int key = bank_first_key + (int)chunk;
    if (chunk_sizes[chunk] == 0U && persist_exists(key)) {
      result = persist_delete(key);
      if (result < 0) {
        APP_LOG(APP_LOG_LEVEL_ERROR,
                "Failed to delete unused history chunk %u: %d",
                chunk, result);
      }
    }
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "History persisted: count=%u generation=%u",
          s_history.count, s_history.generation);
  return true;
}

/* Loads one committed A/B bank without changing the active in-memory history. */
static bool prv_load_history_bank(int first_key, TickHistory *history) {
  const uint8_t *chunks[TICK_HISTORY_CHUNK_COUNT];
  size_t sizes[TICK_HISTORY_CHUNK_COUNT];
  unsigned int chunk;

  if (!persist_exists(first_key)) {
    return false;
  }
  for (chunk = 0U; chunk < TICK_HISTORY_CHUNK_COUNT; ++chunk) {
    const int key = first_key + (int)chunk;
    chunks[chunk] = NULL;
    sizes[chunk] = 0U;
    if (persist_exists(key)) {
      const int size = persist_read_data(key, s_history_chunks[chunk],
                                         sizeof(s_history_chunks[chunk]));
      if (size < 0) {
        return false;
      }
      chunks[chunk] = s_history_chunks[chunk];
      sizes[chunk] = (size_t)size;
    }
  }
  return tick_history_decode_chunks(chunks, sizes, history);
}

/* Selects the newest complete generation and ignores a torn inactive bank. */
static void prv_load_history(void) {
  TickHistory history_b;
  const bool valid_a = prv_load_history_bank(PERSIST_KEY_HISTORY_A_FIRST,
                                              &s_history);
  const bool valid_b = prv_load_history_bank(PERSIST_KEY_HISTORY_B_FIRST,
                                              &history_b);

  if (!valid_a && !valid_b) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "No valid history bank; creating empty A");
    tick_history_init(&s_history);
    s_history_bank = 1U;
    prv_persist_history();
    return;
  }
  if (valid_b && (!valid_a || tick_history_generation_is_newer(
                                  history_b.generation,
                                  s_history.generation))) {
    s_history = history_b;
    s_history_bank = 1U;
  } else if (valid_a) {
    s_history_bank = 0U;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "History loaded: count=%u generation=%u",
          s_history.count, s_history.generation);
}

/* Loads settings and repairs missing or corrupt persisted values with defaults. */
static void prv_load_settings(void) {
  int value;

  if (persist_exists(PERSIST_KEY_TIMER)) {
    value = persist_read_int(PERSIST_KEY_TIMER);
    if (value >= 0 && tick_timer_is_selectable((unsigned int)value)) {
      s_timer_seconds = (unsigned int)value;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid persisted timer: %d", value);
      s_timer_seconds = DEFAULT_TIMER_SECONDS;
      prv_persist_int_checked(PERSIST_KEY_TIMER, (int)s_timer_seconds,
                              "timer");
    }
  } else {
    s_timer_seconds = DEFAULT_TIMER_SECONDS;
    prv_persist_int_checked(PERSIST_KEY_TIMER, (int)s_timer_seconds, "timer");
  }

  if (persist_exists(PERSIST_KEY_DELAY)) {
    value = persist_read_int(PERSIST_KEY_DELAY);
    if (value >= 0 && tick_delay_is_selectable((unsigned int)value)) {
      s_delay_seconds = (unsigned int)value;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid persisted delay: %d", value);
      s_delay_seconds = DEFAULT_DELAY_SECONDS;
      prv_persist_int_checked(PERSIST_KEY_DELAY, (int)s_delay_seconds,
                              "delay");
    }
  } else {
    s_delay_seconds = DEFAULT_DELAY_SECONDS;
    prv_persist_int_checked(PERSIST_KEY_DELAY, (int)s_delay_seconds, "delay");
  }

  if (persist_exists(PERSIST_KEY_HAPTICS)) {
    value = persist_read_int(PERSIST_KEY_HAPTICS);
    if (value == 0 || value == 1) {
      s_haptics_enabled = value == 1;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid persisted haptics: %d", value);
      s_haptics_enabled = DEFAULT_HAPTICS_ENABLED;
      prv_persist_int_checked(PERSIST_KEY_HAPTICS,
                              s_haptics_enabled ? 1 : 0, "haptics");
    }
  } else {
    s_haptics_enabled = DEFAULT_HAPTICS_ENABLED;
    prv_persist_int_checked(PERSIST_KEY_HAPTICS,
                            s_haptics_enabled ? 1 : 0, "haptics");
  }

  if (persist_exists(PERSIST_KEY_LANGUAGE)) {
    value = persist_read_int(PERSIST_KEY_LANGUAGE);
    if (value == LANGUAGE_ENGLISH || value == LANGUAGE_FRENCH) {
      s_language = (Language)value;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid persisted language: %d", value);
      s_language = DEFAULT_LANGUAGE;
      prv_persist_int_checked(PERSIST_KEY_LANGUAGE, (int)s_language,
                              "language");
    }
  } else {
    s_language = DEFAULT_LANGUAGE;
    prv_persist_int_checked(PERSIST_KEY_LANGUAGE, (int)s_language,
                            "language");
  }

  if (persist_exists(PERSIST_KEY_STATISTICS_ENABLED)) {
    value = persist_read_int(PERSIST_KEY_STATISTICS_ENABLED);
    if (value == 0 || value == 1) {
      s_statistics_enabled = value == 1;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Invalid persisted statistics toggle: %d", value);
      s_statistics_enabled = DEFAULT_STATISTICS_ENABLED;
      prv_persist_int_checked(PERSIST_KEY_STATISTICS_ENABLED,
                              s_statistics_enabled ? 1 : 0, "statistics");
    }
  } else {
    s_statistics_enabled = DEFAULT_STATISTICS_ENABLED;
    prv_persist_int_checked(PERSIST_KEY_STATISTICS_ENABLED,
                            s_statistics_enabled ? 1 : 0, "statistics");
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG,
          "Settings loaded: timer=%u delay=%u haptics=%d language=%d stats=%d",
          s_timer_seconds, s_delay_seconds, s_haptics_enabled ? 1 : 0,
          (int)s_language, s_statistics_enabled ? 1 : 0);
}

/* Formats elapsed or configured time compactly for every display size. */
static void prv_format_time(unsigned int seconds, char *buffer,
                            size_t buffer_size) {
  const unsigned int hours = seconds / 3600U;
  const unsigned int minutes = (seconds % 3600U) / 60U;
  const unsigned int remaining_seconds = seconds % 60U;

  if (hours > 0U) {
    snprintf(buffer, buffer_size, "%u:%02u:%02u", hours, minutes,
             remaining_seconds);
  } else {
    snprintf(buffer, buffer_size, "%u:%02u", minutes, remaining_seconds);
  }
}

/* Keeps an unbounded cycle count readable once exact digits exceed the dial. */
static void prv_format_cycle(uint64_t cycle, char *buffer,
                             size_t buffer_size) {
  uint64_t divisor;
  const char *suffix;

  if (cycle < 1000000ULL) {
    snprintf(buffer, buffer_size, "%llu", (unsigned long long)cycle);
    return;
  }
  if (cycle < 1000000000ULL) {
    divisor = 1000000ULL;
    suffix = "M";
  } else if (cycle < 1000000000000ULL) {
    divisor = 1000000000ULL;
    suffix = "B";
  } else if (cycle < 1000000000000000ULL) {
    divisor = 1000000000000ULL;
    suffix = "T";
  } else {
    snprintf(buffer, buffer_size, "999T+");
    return;
  }

  snprintf(buffer, buffer_size, "%llu.%llu%s",
           (unsigned long long)(cycle / divisor),
           (unsigned long long)((cycle % divisor) / (divisor / 10ULL)),
           suffix);
}

/* Formats the configured interval for narrow round-screen context lines. */
static void prv_format_interval_short(unsigned int seconds, char *buffer,
                                      size_t buffer_size) {
  if (seconds == 3600U) {
    snprintf(buffer, buffer_size, "1 h");
  } else if (seconds >= 60U) {
    snprintf(buffer, buffer_size, "%u:%02u", seconds / 60U, seconds % 60U);
  } else {
    snprintf(buffer, buffer_size, "%u s", seconds);
  }
}

/* Reads one coherent wall-clock timestamp including its millisecond phase. */
static WallClockTime prv_now(void) {
  WallClockTime now;
  time_ms(&now.seconds, &now.milliseconds);
  return now;
}

/* Adds a short delay to a wall-clock timestamp without losing phase. */
static WallClockTime prv_time_add_ms(WallClockTime start,
                                     uint32_t milliseconds) {
  WallClockTime result = start;
  result.seconds += (time_t)(milliseconds / 1000U);
  result.milliseconds += (uint16_t)(milliseconds % 1000U);
  if (result.milliseconds >= 1000U) {
    result.milliseconds -= 1000U;
    ++result.seconds;
  }
  return result;
}

/* Measures a non-negative interval without losing long-running timer time. */
static uint64_t prv_time_elapsed_ms(WallClockTime start, WallClockTime end) {
  time_t seconds;
  int32_t milliseconds;

  if (end.seconds < start.seconds ||
      (end.seconds == start.seconds &&
       end.milliseconds < start.milliseconds)) {
    return 0U;
  }

  seconds = end.seconds - start.seconds;
  milliseconds = (int32_t)end.milliseconds - (int32_t)start.milliseconds;
  if (milliseconds < 0) {
    --seconds;
    milliseconds += 1000;
  }
  return (uint64_t)seconds * 1000U + (uint32_t)milliseconds;
}

/* Saturates the unbounded runtime counters to the 136-year history fields. */
static uint32_t prv_u64_to_u32(uint64_t value) {
  return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* Tries one queued full snapshot; a busy outbox is retried after its callback. */
static void prv_try_send_history(void) {
  DictionaryIterator *iterator;
  DictionaryResult dictionary_result;
  AppMessageResult message_result;
  size_t wire_size;

  if (!s_history_send_pending || s_history_send_in_flight) {
    return;
  }
  if (s_history_send_chunk == 0U ||
      s_history_send_generation != s_history.generation) {
    s_history_send_chunk = 0U;
    s_history_send_generation = s_history.generation;
  }
  wire_size = tick_history_encode_chunk(
      &s_history, s_history_send_chunk,
      s_history_chunks[s_history_send_chunk],
      sizeof(s_history_chunks[s_history_send_chunk]));
  if (wire_size == 0U) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to encode history page %u",
            s_history_send_chunk);
    return;
  }
  message_result = app_message_outbox_begin(&iterator);
  if (message_result != APP_MSG_OK || iterator == NULL) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "History outbox unavailable: %d",
            (int)message_result);
    return;
  }
  dictionary_result = dict_write_data(iterator, MESSAGE_KEY_HISTORY_DATA,
                                      s_history_chunks[s_history_send_chunk],
                                      wire_size);
  if (dictionary_result != DICT_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to write history tuple: %d",
            (int)dictionary_result);
    return;
  }
  message_result = app_message_outbox_send();
  if (message_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send history: %d",
            (int)message_result);
    return;
  }
  s_history_send_pending = false;
  s_history_send_in_flight = true;
}

/* Coalesces save/request events into at most one additional full snapshot. */
static void prv_request_history_send(void) {
  s_history_send_pending = true;
  prv_try_send_history();
}

/* Persists the stop snapshot only for an active, explicitly stopped session. */
static void prv_save_stop_snapshot(void) {
  TickSession session;
  uint64_t total_ms;
  uint32_t active_seconds;
  uint32_t total_seconds;

  if (!s_statistics_enabled_at_session_start || !s_statistics_enabled ||
      !s_session_active_started ||
      !s_stop_snapshot_valid || s_stop_snapshot_at.seconds <= 0) {
    return;
  }

  total_ms = prv_time_elapsed_ms(s_session_started_at, s_stop_snapshot_at);
  active_seconds = prv_u64_to_u32(s_stop_snapshot_active_ms / 1000U);
  total_seconds = prv_u64_to_u32(total_ms / 1000U);
  if (total_seconds < active_seconds) {
    total_seconds = active_seconds;
  }

  session.ended_at = prv_u64_to_u32((uint64_t)s_stop_snapshot_at.seconds);
  session.total_duration_seconds = total_seconds;
  session.active_duration_seconds = active_seconds;
  session.cycles = prv_u64_to_u32(s_stop_snapshot_cycle);
  session.interval_seconds = (uint16_t)s_timer_seconds;
  session.delay_seconds = (uint8_t)s_delay_seconds;
  session.flags = s_haptics_enabled ? TICK_SESSION_FLAG_HAPTICS : 0U;

  if (!tick_history_add(&s_history, &session)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Rejected completed session history record");
    return;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG,
          "Session saved: total=%lu active=%lu cycles=%lu count=%u",
          (unsigned long)session.total_duration_seconds,
          (unsigned long)session.active_duration_seconds,
          (unsigned long)session.cycles, s_history.count);
  if (prv_persist_history()) {
    prv_request_history_send();
  }
}

/* Computes exact active elapsed time while excluding all time spent paused. */
static uint64_t prv_current_elapsed_ms(WallClockTime now) {
  uint64_t elapsed_ms = s_elapsed_before_segment_ms;
  if (s_state == APP_STATE_RUNNING) {
    const uint64_t segment_ms = prv_time_elapsed_ms(
        s_run_segment_started_at, now);
    if (segment_ms > UINT64_MAX - elapsed_ms) {
      return UINT64_MAX;
    }
    elapsed_ms += segment_ms;
  }
  return elapsed_ms;
}

/* Arms the next display/cycle/deadline boundary relative to launch time. */
static void prv_schedule_runtime_timer(void) {
  const WallClockTime now = prv_now();
  uint64_t elapsed_ms;
  uint64_t delay_total_ms;
  unsigned int delay_ms;

  prv_cancel_runtime_timer();
  if (s_state == APP_STATE_WAITING) {
    elapsed_ms = prv_time_elapsed_ms(s_wait_started_at, now);
    delay_total_ms = (uint64_t)s_delay_seconds * 1000U;
    if (elapsed_ms >= delay_total_ms) {
      delay_ms = 1U;
    } else {
      const uint64_t remaining_ms = delay_total_ms - elapsed_ms;
      delay_ms = tick_next_second_delay_ms(elapsed_ms);
      if (remaining_ms < delay_ms) {
        delay_ms = (unsigned int)remaining_ms;
      }
    }
  } else if (s_state == APP_STATE_RUNNING) {
    elapsed_ms = prv_current_elapsed_ms(now);
    delay_ms = tick_next_second_delay_ms(elapsed_ms);
  } else {
    return;
  }
  s_runtime_timer = app_timer_register(delay_ms,
                                       prv_runtime_timer_callback, NULL);
  if (!s_runtime_timer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate runtime timer");
  }
}

/*
 * Enqueues the decimal code when it fits both hard limits: at most 32 pulses
 * (63 static API segments) and completion 20 ms before the next cycle.
 * Oversize codes are skipped; the repeating timer itself always continues.
 */
static void prv_vibrate_for_cycle(uint64_t cycle,
                                  unsigned int available_ms) {
  TickCyclePattern code;
  unsigned int pulse_count;
  unsigned int segment_count = 0U;
  unsigned int pattern_duration_ms;
  unsigned int index;

  if (!s_haptics_enabled) {
    return;
  }
  if (cycle > UINT32_MAX ||
      !tick_cycle_pattern((uint32_t)cycle, &code) ||
      code.long_vibrations > HAPTIC_MAX_PULSES ||
      code.short_vibrations > HAPTIC_MAX_PULSES - code.long_vibrations) {
    if (!s_haptic_limit_logged) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Haptic skipped: cycle exceeds 32 pulses / 63 segments");
      s_haptic_limit_logged = true;
    }
    return;
  }

  pulse_count = (unsigned int)(code.long_vibrations +
                               code.short_vibrations);
  pattern_duration_ms =
      (unsigned int)code.long_vibrations * HAPTIC_LONG_PULSE_MS +
      (unsigned int)code.short_vibrations * HAPTIC_SHORT_PULSE_MS +
      (pulse_count - 1U) * HAPTIC_GAP_MS;
  if (available_ms <= HAPTIC_DEADLINE_MARGIN_MS ||
      pattern_duration_ms > available_ms - HAPTIC_DEADLINE_MARGIN_MS) {
    if (!s_haptic_limit_logged) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Haptic skipped: %u ms pattern, %u ms until next cycle",
              pattern_duration_ms, available_ms);
      s_haptic_limit_logged = true;
    }
    return;
  }

  s_haptic_limit_logged = false;
  vibes_cancel();
  for (index = 0U; index < code.long_vibrations; ++index) {
    s_haptic_durations[segment_count++] = HAPTIC_LONG_PULSE_MS;
    if (segment_count < pulse_count * 2U - 1U) {
      s_haptic_durations[segment_count++] = HAPTIC_GAP_MS;
    }
  }
  for (index = 0U; index < code.short_vibrations; ++index) {
    s_haptic_durations[segment_count++] = HAPTIC_SHORT_PULSE_MS;
    if (segment_count < pulse_count * 2U - 1U) {
      s_haptic_durations[segment_count++] = HAPTIC_GAP_MS;
    }
  }

  vibes_enqueue_custom_pattern((VibePattern) {
    .durations = s_haptic_durations,
    .num_segments = segment_count,
  });
}

/* Reconciles wall-clock elapsed time and emits at most the newest missed cycle. */
static void prv_update_running_timer(WallClockTime now) {
  const uint64_t elapsed_ms = prv_current_elapsed_ms(now);
  const unsigned int elapsed = tick_elapsed_seconds_from_ms(elapsed_ms);
  const uint64_t reached = tick_cycles_reached_ms(elapsed_ms,
                                                   s_timer_seconds);

  s_elapsed_seconds = elapsed;
  if (reached > s_cycle) {
    const uint64_t timer_ms = (uint64_t)s_timer_seconds * 1000U;
    const uint64_t phase_ms = elapsed_ms % timer_ms;
    const unsigned int available_ms = (unsigned int)(
        phase_ms == 0U ? timer_ms : timer_ms - phase_ms);
    s_cycle = reached;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer cycle=%llu elapsed=%u",
            (unsigned long long)s_cycle, s_elapsed_seconds);
    /* A late tick represents current state; old patterns are deliberately skipped. */
    prv_vibrate_for_cycle(s_cycle, available_ms);
    prv_start_animation(ANIMATION_PULSE);
  }
  prv_mark_dirty();
}

/* Begins active timing at its logical wall-clock start and signals it twice. */
static void prv_begin_active_timer(WallClockTime logical_start) {
  prv_cancel_runtime_timer();
  s_elapsed_before_segment_ms = 0U;
  s_elapsed_seconds = 0U;
  s_cycle = 0U;
  s_run_segment_started_at = logical_start;
  s_session_active_started = true;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer active start");
  prv_set_state(APP_STATE_RUNNING);
  vibes_double_pulse();
  prv_start_animation(ANIMATION_PULSE);
  prv_update_running_timer(prv_now());
  if (s_state == APP_STATE_RUNNING) {
    prv_schedule_runtime_timer();
  }
}

/* Resets runtime counters and starts either the silent delay or active timer. */
static void prv_start_timer(void) {
  const WallClockTime now = prv_now();
  s_elapsed_seconds = 0U;
  s_elapsed_before_segment_ms = 0U;
  s_cycle = 0U;
  s_haptic_limit_logged = false;
  s_session_started_at = now;
  s_session_active_started = false;
  s_statistics_enabled_at_session_start = s_statistics_enabled;
  s_stop_snapshot_valid = false;

  APP_LOG(APP_LOG_LEVEL_DEBUG,
          "Repeating timer start requested: timer=%u delay=%u",
          s_timer_seconds, s_delay_seconds);
  if (s_delay_seconds > 0U) {
    s_wait_started_at = now;
    prv_set_state(APP_STATE_WAITING);
    prv_start_animation(ANIMATION_PULSE);
    prv_schedule_runtime_timer();
  } else {
    prv_begin_active_timer(now);
  }
}

/* Freezes elapsed time at the wall clock instant of the pause. */
static void prv_pause_timer(void) {
  const WallClockTime now = prv_now();

  /* Reconcile first so Pause cannot swallow a cycle boundary. */
  prv_update_running_timer(now);
  if (s_state != APP_STATE_RUNNING) {
    return;
  }
  prv_cancel_runtime_timer();
  s_elapsed_before_segment_ms = prv_current_elapsed_ms(now);
  s_elapsed_seconds = tick_elapsed_seconds_from_ms(
      s_elapsed_before_segment_ms);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer pause: elapsed=%u", s_elapsed_seconds);
  prv_set_state(APP_STATE_PAUSED);
}

/* Starts a new wall-clock segment while retaining elapsed time before pause. */
static void prv_resume_timer(void) {
  s_run_segment_started_at = prv_now();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer resume: elapsed=%u",
          s_elapsed_seconds);
  prv_set_state(APP_STATE_RUNNING);
  prv_start_animation(ANIMATION_PULSE);
  prv_schedule_runtime_timer();
}

/* Opens the explicit stop guard and remembers which state Back should restore. */
static void prv_ask_to_stop(void) {
  const WallClockTime now = prv_now();

  if (s_state == APP_STATE_WAITING) {
    const uint32_t delay_ms = s_delay_seconds * 1000U;
    if (prv_time_elapsed_ms(s_wait_started_at, now) >= delay_ms) {
      s_run_segment_started_at = prv_time_add_ms(s_wait_started_at, delay_ms);
      s_elapsed_before_segment_ms = 0U;
      s_session_active_started = true;
      prv_set_state(APP_STATE_RUNNING);
    }
  }
  s_state_before_stop_confirm = s_state;
  s_stop_snapshot_valid = false;

  if (s_state == APP_STATE_RUNNING) {
    prv_update_running_timer(now);
    s_elapsed_before_segment_ms = prv_current_elapsed_ms(now);
    s_elapsed_seconds = tick_elapsed_seconds_from_ms(
        s_elapsed_before_segment_ms);
  }
  if (s_state == APP_STATE_RUNNING || s_state == APP_STATE_PAUSED) {
    s_stop_snapshot_at = now;
    s_stop_snapshot_active_ms = s_elapsed_before_segment_ms;
    s_stop_snapshot_cycle = tick_cycles_reached_ms(
        s_stop_snapshot_active_ms, s_timer_seconds);
    s_cycle = s_stop_snapshot_cycle;
    s_stop_snapshot_valid = s_session_active_started;
  }
  prv_cancel_runtime_timer();
  prv_set_state(APP_STATE_STOP_CONFIRM);
}

/* Cancels the guard while excluding confirmation time from active duration. */
static void prv_cancel_stop(void) {
  const AppState restored_state = s_state_before_stop_confirm;
  s_stop_snapshot_valid = false;

  if (restored_state == APP_STATE_RUNNING) {
    s_run_segment_started_at = prv_now();
    prv_set_state(APP_STATE_RUNNING);
    prv_schedule_runtime_timer();
  } else {
    prv_set_state(restored_state);
    prv_reconcile_runtime();
  }
}

/* Stops without changing the persisted configuration. */
static void prv_confirm_stop(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer stopped: elapsed=%u cycle=%llu",
          s_elapsed_seconds, (unsigned long long)s_cycle);
  prv_cancel_runtime_timer();
  vibes_cancel();
  prv_cancel_animation();
  prv_save_stop_snapshot();
  s_elapsed_seconds = 0U;
  s_elapsed_before_segment_ms = 0U;
  s_cycle = 0U;
  s_haptic_limit_logged = false;
  s_session_active_started = false;
  s_stop_snapshot_valid = false;
  prv_set_state(APP_STATE_READY);
}

/* Draws centered text with one compact helper. */
static void prv_draw_text(GContext *ctx, const char *text, GFont font,
                          GRect rect, GTextAlignment alignment,
                          GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis,
                     alignment, NULL);
}

/* Adds a restrained halo behind the main value at active pulse boundaries. */
static void prv_draw_animation(GContext *ctx, GRect bounds, int16_t center_y) {
  if (s_animation_kind == ANIMATION_PULSE && s_animation_frame > 0U) {
    const unsigned int progressed = PULSE_FRAME_COUNT - s_animation_frame;
    const int16_t radius = (int16_t)(18U + progressed * 2U);
    const GColor halo = PBL_IF_COLOR_ELSE(
        (progressed % 2U) ? GColorChromeYellow : GColorPictonBlue,
        GColorBlack);
    graphics_context_set_stroke_color(ctx, halo);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_circle(ctx, GPoint(bounds.size.w / 2, center_y), radius);
  }
}

/* Gives round displays a watch-like focal ring instead of an empty center. */
static void prv_draw_round_dial(GContext *ctx, GRect bounds, int16_t center_y,
                                GColor accent) {
  if (!PBL_IF_ROUND_ELSE(true, false)) {
    return;
  }

  const bool large_round = bounds.size.w >= 240;
  const int16_t radius = bounds.size.w / 2 - (large_round ? 14 : 13);
  const int16_t dot_radius = large_round ? 3 : 2;
  const GPoint center = GPoint(bounds.size.w / 2, center_y);
  const GColor ring = PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack);

  graphics_context_set_stroke_color(ctx, ring);
  graphics_context_set_stroke_width(ctx, large_round ? 3 : 2);
  graphics_draw_circle(ctx, center, radius);

  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_circle(ctx, GPoint(center.x, center.y - radius), dot_radius);
  graphics_fill_circle(ctx, GPoint(center.x + radius, center.y), dot_radius);
  graphics_fill_circle(ctx, GPoint(center.x - radius, center.y), dot_radius);
}

/* Draws one explicit state band, with the current setup step when relevant. */
static void prv_draw_state_band(GContext *ctx, GRect rect,
                                const char *state_text,
                                const char *step_text, GColor background,
                                GColor foreground, GFont font) {
  const int16_t horizontal_padding = 7;
  graphics_context_set_fill_color(ctx, background);
  graphics_fill_rect(ctx, rect, 5, GCornersAll);

  if (step_text[0] != '\0') {
    const int16_t step_width = PBL_IF_ROUND_ELSE(38, 48);
    prv_draw_text(ctx, state_text, font,
                  GRect(rect.origin.x + horizontal_padding, rect.origin.y,
                        rect.size.w - step_width - horizontal_padding,
                        rect.size.h),
                  GTextAlignmentLeft, foreground);
    prv_draw_text(ctx, step_text, font,
                  GRect(rect.origin.x + rect.size.w - step_width,
                        rect.origin.y, step_width - horizontal_padding,
                        rect.size.h),
                  GTextAlignmentRight, foreground);
  } else {
    prv_draw_text(ctx, state_text, font, rect, GTextAlignmentCenter,
                  foreground);
  }
}

/* Reserves two stable, high-contrast rows for the available button actions. */
static void prv_draw_actions(GContext *ctx, GRect bounds, int16_t y,
                             const char *first_action,
                             const char *second_action, GFont font) {
  const bool round = PBL_IF_ROUND_ELSE(true, false);
  const bool large_round = round && bounds.size.w >= 240;
  const int16_t margin = round ? (large_round ? 50 : 32) : 0;
  const int16_t width = bounds.size.w - margin * 2;
  const int16_t second_margin = round ? (large_round ? 85 : 57) : margin;
  const int16_t second_width = bounds.size.w - second_margin * 2;
  const int16_t row_height = large_round ? 27 : 20;
  const int16_t second_row_height = large_round ? 22 : 17;
  const int16_t row_gap = round ? (large_round ? 2 : 1) : 0;
  const GColor background = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  const GColor foreground = GColorBlack;

  graphics_context_set_fill_color(ctx, background);
  graphics_context_set_stroke_color(ctx, foreground);
  if (round) {
    if (first_action[0] != '\0') {
      graphics_fill_rect(ctx, GRect(margin, y, width, row_height), 6,
                         GCornersAll);
      graphics_draw_round_rect(ctx, GRect(margin, y, width, row_height), 6);
    }
    if (second_action[0] != '\0') {
      graphics_fill_rect(ctx,
                         GRect(second_margin, y + row_height + row_gap,
                               second_width, second_row_height),
                         6, GCornersAll);
      graphics_draw_round_rect(ctx,
                               GRect(second_margin,
                                     y + row_height + row_gap, second_width,
                                     second_row_height),
                               6);
    }
  } else {
    graphics_fill_rect(ctx, GRect(margin, y, width, row_height * 2), 0,
                       GCornerNone);
    graphics_draw_line(ctx, GPoint(margin, y), GPoint(margin + width, y));
    graphics_draw_line(ctx, GPoint(margin, y + row_height),
                       GPoint(margin + width, y + row_height));
  }

  if (first_action[0] != '\0') {
    prv_draw_text(ctx, first_action, font,
                  GRect(margin + 2, y, width - 4, row_height),
                  GTextAlignmentCenter, foreground);
  }
  if (second_action[0] != '\0') {
    const GFont second_font = large_round
        ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD) : font;
    prv_draw_text(ctx, second_action, second_font,
                  GRect(second_margin + 2, y + row_height + row_gap,
                        second_width - 4, second_row_height),
                  GTextAlignmentCenter, foreground);
  }
}

/* Formats one local end date without depending on the watch locale. */
static void prv_format_history_date(uint32_t ended_at, char *buffer,
                                    size_t buffer_size) {
  const time_t timestamp = (time_t)ended_at;
  const struct tm *local = localtime(&timestamp);
  if (local == NULL) {
    snprintf(buffer, buffer_size, "--/-- --:--");
    return;
  }
  snprintf(buffer, buffer_size, "%02u/%02u %02u:%02u",
           (unsigned int)local->tm_mday % 100U,
           (unsigned int)(local->tm_mon + 1) % 100U,
           (unsigned int)local->tm_hour % 100U,
           (unsigned int)local->tm_min % 100U);
}

/* Draws the newest-first history as one compact scrollable list. */
static void prv_draw_history(GContext *ctx, GRect bounds) {
  const bool round = PBL_IF_ROUND_ELSE(true, false);
  const bool tall = bounds.size.h >= 200;
  const unsigned int visible_rows = tall ? 4U : 3U;
  const int16_t margin = round ? (bounds.size.w >= 200 ? 29 : 22) : 4;
  const int16_t band_margin = round ? (bounds.size.w >= 200 ? 42 : 34) : 4;
  const int16_t band_y = round ? (tall ? 18 : 14) : 0;
  const int16_t band_height = 27;
  const int16_t content_top = band_y + band_height + 5;
  const int16_t footer_height = round ? 26 : 22;
  const int16_t footer_y = bounds.size.h - footer_height;
  const int16_t footer_margin = round ? (tall ? 50 : 42) : margin;
  const int16_t row_height = (footer_y - content_top) / (int16_t)visible_rows;
  const GFont title_font = fonts_get_system_font(
      round && !tall ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  const GFont date_font = fonts_get_system_font(
      round && !tall ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  const GFont detail_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  char position[16];
  unsigned int row;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  if (s_history.count == 0U) {
    position[0] = '\0';
  } else {
    snprintf(position, sizeof(position), "%u-%u/%u", s_history_offset + 1U,
             s_history_offset + visible_rows < s_history.count
                 ? s_history_offset + visible_rows : s_history.count,
             s_history.count);
  }
  prv_draw_state_band(
      ctx, GRect(band_margin, band_y, bounds.size.w - band_margin * 2,
                 band_height),
      prv_text("HISTORY", "HISTORIQUE"), position,
      PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorBlack),
      PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite), title_font);

  if (s_history.count == 0U) {
    const char *empty = prv_text("NO SAVED SESSIONS", "AUCUNE SESSION");
    const char *status = s_statistics_enabled
        ? prv_text("SAVING IS ON", "SAUVEGARDE ACTIVE")
        : prv_text("SAVING IS OFF", "SAUVEGARDE COUPÉE");
    prv_draw_text(ctx, empty,
                  fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                  GRect(margin, content_top + 20,
                        bounds.size.w - margin * 2, 28),
                  GTextAlignmentCenter, GColorBlack);
    prv_draw_text(ctx, status, detail_font,
                  GRect(margin, content_top + 48,
                        bounds.size.w - margin * 2, 24),
                  GTextAlignmentCenter, GColorBlack);
  } else {
    for (row = 0U; row < visible_rows; ++row) {
      const unsigned int index = s_history_offset + row;
      GRect row_rect;
      char date_text[20];
      char duration_text[16];
      char detail_text[40];
      const TickSession *session;

      if (index >= s_history.count) {
        break;
      }
      session = &s_history.sessions[index];
      row_rect = GRect(margin, content_top + (int16_t)row * row_height,
                       bounds.size.w - margin * 2, row_height - 2);
      graphics_context_set_fill_color(
          ctx, PBL_IF_COLOR_ELSE(row % 2U == 0U ? GColorLightGray
                                                : GColorWhite,
                                GColorWhite));
      graphics_fill_rect(ctx, row_rect, 5, GCornersAll);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      graphics_draw_round_rect(ctx, row_rect, 5);
      graphics_context_set_fill_color(
          ctx, PBL_IF_COLOR_ELSE(row % 2U == 0U ? GColorJaegerGreen
                                                : GColorPictonBlue,
                                GColorBlack));
      graphics_fill_rect(ctx,
                         GRect(row_rect.origin.x, row_rect.origin.y, 4,
                               row_rect.size.h),
                         2, GCornersLeft);

      prv_format_history_date(session->ended_at, date_text,
                              sizeof(date_text));
      prv_format_time(session->total_duration_seconds, duration_text,
                      sizeof(duration_text));
      snprintf(detail_text, sizeof(detail_text), "%s · %luC · %us",
               duration_text, (unsigned long)session->cycles,
               session->interval_seconds);
      prv_draw_text(ctx, date_text, date_font,
                    GRect(row_rect.origin.x + 8, row_rect.origin.y - 1,
                          row_rect.size.w - 12, row_rect.size.h / 2 + 3),
                    GTextAlignmentLeft, GColorBlack);
      prv_draw_text(ctx, detail_text, detail_font,
                    GRect(row_rect.origin.x + 8,
                          row_rect.origin.y + row_rect.size.h / 2 - 3,
                          row_rect.size.w - 12, row_rect.size.h / 2 + 2),
                    GTextAlignmentLeft, GColorBlack);
    }
  }

  graphics_context_set_fill_color(
      ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_fill_rect(ctx, GRect(footer_margin, footer_y,
                                bounds.size.w - footer_margin * 2,
                                footer_height),
                     round ? 6 : 0, round ? GCornersAll : GCornerNone);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, GRect(footer_margin, footer_y,
                                      bounds.size.w - footer_margin * 2,
                                      footer_height), round ? 6 : 0);
  prv_draw_text(ctx,
                round ? prv_text("UP/DN · BACK", "H/B · BACK")
                      : prv_text("UP/DOWN · BACK", "HAUT/BAS · BACK"),
                detail_font,
                GRect(footer_margin + 2, footer_y,
                      bounds.size.w - footer_margin * 2 - 4, footer_height),
                GTextAlignmentCenter, GColorBlack);
}

/* Renders the complete interface on one lightweight platform-adaptive layer. */
static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const bool round = PBL_IF_ROUND_ELSE(true, false);
  const bool large_round = round && bounds.size.w >= 240;
  const int16_t margin = round ? (large_round ? 38 : 27) : 5;
  const int16_t band_margin = round ? (large_round ? 57 : 38) : margin;
  const int16_t content_width = bounds.size.w - margin * 2;
  const int16_t band_y = round ? (large_round ? 26 : 18) : 0;
  const int16_t band_height = large_round ? 36 : 27;
  const int16_t footer_y = round
      ? (large_round ? 187 : 123)
      : bounds.size.h - 44;
  const int16_t block_height = round ? (large_round ? 105 : 73) : 92;
  const int16_t content_top = band_y + band_height + 2;
  const int16_t available_height = footer_y - content_top;
  const int16_t main_y = round
      ? (large_round ? 78 : 55)
      : content_top + (available_height - block_height) / 2;
  const int16_t main_height = round ? (large_round ? 76 : 52) : 48;
  const int16_t info_height = large_round ? 29 : 22;
  const int16_t info_first_y = main_y + main_height;
  const int16_t info_second_y = info_first_y + info_height;
  const GColor background = GColorWhite;
  const GColor foreground = GColorBlack;
  GColor band_background = PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorBlack);
  GColor band_foreground = PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite);
  const GFont label_font = fonts_get_system_font(
      round && !large_round ? FONT_KEY_GOTHIC_14_BOLD
                            : FONT_KEY_GOTHIC_18_BOLD);
  const GFont info_font = fonts_get_system_font(
      large_round ? FONT_KEY_GOTHIC_24_BOLD
                  : (round ? FONT_KEY_GOTHIC_14_BOLD
                           : FONT_KEY_GOTHIC_18_BOLD));
  const GFont action_font = fonts_get_system_font(
      large_round ? FONT_KEY_GOTHIC_24_BOLD
                  : (round ? FONT_KEY_GOTHIC_14_BOLD
                           : FONT_KEY_GOTHIC_18_BOLD));
  const GFont value_font = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  const GFont compact_value_font =
      fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);
  const GFont round_word_font = fonts_get_system_font(
      large_round ? FONT_KEY_BITHAM_42_BOLD : FONT_KEY_GOTHIC_28_BOLD);
  GFont main_font = value_font;
  char state_text[24];
  char step_text[8];
  char value_text[24];
  char info_first[48];
  char info_second[48];
  char elapsed_text[16];
  char cycle_text[16];
  char interval_text[16];
  char first_action[40];
  char second_action[40];

  graphics_context_set_fill_color(ctx, background);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_state == APP_STATE_HISTORY) {
    prv_draw_history(ctx, bounds);
    return;
  }

  snprintf(state_text, sizeof(state_text), "TIMER");
  snprintf(step_text, sizeof(step_text), "1/4");
  prv_format_time(s_timer_seconds, value_text, sizeof(value_text));
  snprintf(info_first, sizeof(info_first), "%s",
           prv_text("INTERVAL", "INTERVALLE"));
  snprintf(info_second, sizeof(info_second), "%s",
           prv_text("HOLD = HISTORY", "TENIR = HIST."));
  snprintf(first_action, sizeof(first_action), "%s",
           prv_text("UP/DOWN SET", "HAUT/BAS RÉGLER"));
  snprintf(second_action, sizeof(second_action), "%s",
           prv_text("SELECT NEXT", "SELECT SUIVANT"));

  switch (s_state) {
    case APP_STATE_SET_TIMER:
      main_font = value_font;
      if (s_timer_seconds >= 3600U) {
        main_font = compact_value_font;
      }
      break;
    case APP_STATE_SET_DELAY:
      snprintf(state_text, sizeof(state_text), "%s",
               prv_text("DELAY", "DÉLAI"));
      snprintf(step_text, sizeof(step_text), "2/4");
      if (s_delay_seconds == 0U) {
        snprintf(value_text, sizeof(value_text), "%s",
                 prv_text("NONE", "AUCUN"));
        main_font = compact_value_font;
      } else {
        snprintf(value_text, sizeof(value_text), "%u s", s_delay_seconds);
        main_font = value_font;
      }
      snprintf(info_first, sizeof(info_first), "%s",
               prv_text("QUIET START", "DÉPART SILENCIEUX"));
      snprintf(info_second, sizeof(info_second), "%s",
               prv_text("THEN 2 PULSES", "PUIS 2 PULSES"));
      snprintf(first_action, sizeof(first_action), "%s",
               prv_text("UP/DOWN SET", "HAUT/BAS RÉGLER"));
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("SELECT NEXT", "SELECT SUIVANT"));
      break;
    case APP_STATE_SET_HAPTICS:
      snprintf(state_text, sizeof(state_text), "%s",
               prv_text("HAPTICS", "VIBRATIONS"));
      snprintf(step_text, sizeof(step_text), "3/4");
      snprintf(value_text, sizeof(value_text), "%s",
               s_haptics_enabled ? prv_text("YES", "OUI")
                                  : prv_text("NO", "NON"));
      main_font = compact_value_font;
      snprintf(info_first, sizeof(info_first), "%s",
               prv_text("CYCLE NUMBER", "NUMÉRO DU CYCLE"));
      snprintf(info_second, sizeof(info_second), "%s",
               prv_text("DECIMAL CODE", "CODE DÉCIMAL"));
      snprintf(first_action, sizeof(first_action), "%s",
               prv_text("UP/DOWN YES/NO", "HAUT/BAS OUI/NON"));
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("SELECT NEXT", "SELECT SUIVANT"));
      break;
    case APP_STATE_READY:
      snprintf(state_text, sizeof(state_text), "%s",
               prv_text("READY", "PRÊT"));
      snprintf(step_text, sizeof(step_text), "4/4");
      if (s_timer_seconds >= 3600U) {
        prv_format_time(s_timer_seconds, value_text, sizeof(value_text));
        main_font = compact_value_font;
      } else {
        snprintf(value_text, sizeof(value_text), "%u s", s_timer_seconds);
        main_font = value_font;
      }
      if (s_delay_seconds == 0U) {
        snprintf(info_first, sizeof(info_first), "%s",
                 prv_text("STARTS NOW", "DÉPART IMMÉDIAT"));
      } else {
        snprintf(info_first, sizeof(info_first), "%s %u s",
                 prv_text("DELAY", "DÉLAI"), s_delay_seconds);
      }
      snprintf(info_second, sizeof(info_second), "%s %s",
               prv_text("HAPTICS", "VIBRATIONS"),
               s_haptics_enabled ? prv_text("YES", "OUI")
                                  : prv_text("NO", "NON"));
      snprintf(first_action, sizeof(first_action), "%s",
               prv_text("HOLD SELECT", "TENIR SELECT"));
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("BACK EDIT", "BACK MODIFIER"));
      break;
    case APP_STATE_WAITING: {
      const WallClockTime now = prv_now();
      const uint32_t delay_ms = s_delay_seconds * 1000U;
      const uint32_t waited_ms = prv_time_elapsed_ms(s_wait_started_at, now);
      const uint32_t remaining_ms = waited_ms < delay_ms
          ? delay_ms - waited_ms : 0U;
      const unsigned int remaining = (remaining_ms + 999U) / 1000U;
      snprintf(state_text, sizeof(state_text), "%s",
               prv_text("STARTING", "DÉMARRAGE"));
      step_text[0] = '\0';
      snprintf(value_text, sizeof(value_text), "%u", remaining);
      snprintf(info_first, sizeof(info_first), "%s",
               prv_text("QUIET", "SILENCE"));
      snprintf(info_second, sizeof(info_second), "%s",
               prv_text("THEN 2 PULSES", "PUIS 2 PULSES"));
      first_action[0] = '\0';
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("BACK STOP", "BACK ARRÊT"));
      break;
    }
    case APP_STATE_RUNNING:
    case APP_STATE_PAUSED: {
      snprintf(state_text, sizeof(state_text), "%s",
               s_state == APP_STATE_PAUSED ? prv_text("PAUSED", "PAUSE")
                                           : prv_text("RUNNING", "ACTIF"));
      step_text[0] = '\0';
      prv_format_time(s_elapsed_seconds, value_text, sizeof(value_text));
      if (s_elapsed_seconds >= 3600U) {
        main_font = compact_value_font;
      }
      snprintf(info_first, sizeof(info_first), "%s %llu",
               prv_text("CYCLE", "CYCLE"),
               (unsigned long long)s_cycle);
      snprintf(info_second, sizeof(info_second), "%s %u s",
               prv_text("EVERY", "TOUTES LES"), s_timer_seconds);
      snprintf(first_action, sizeof(first_action),
               s_state == APP_STATE_PAUSED
                   ? prv_text("SELECT RESUME", "SELECT REPRENDRE")
                   : prv_text("SELECT PAUSE", "SELECT PAUSE"));
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("BACK STOP", "BACK ARRÊT"));
      band_background = PBL_IF_COLOR_ELSE(
          s_state == APP_STATE_PAUSED ? GColorChromeYellow
                                      : GColorJaegerGreen,
          GColorBlack);
      break;
    }
    case APP_STATE_STOP_CONFIRM:
      snprintf(state_text, sizeof(state_text), "%s",
               prv_text("STOP?", "ARRÊTER ?"));
      step_text[0] = '\0';
      snprintf(value_text, sizeof(value_text), "STOP ?");
      main_font = compact_value_font;
      snprintf(info_first, sizeof(info_first), "CYCLE %llu",
               (unsigned long long)s_cycle);
      prv_format_time(s_elapsed_seconds, info_second, sizeof(info_second));
      snprintf(first_action, sizeof(first_action), "%s",
               prv_text("SELECT YES", "SELECT OUI"));
      snprintf(second_action, sizeof(second_action), "%s",
               prv_text("BACK NO", "BACK NON"));
      band_background = PBL_IF_COLOR_ELSE(GColorRed, GColorBlack);
      band_foreground = PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite);
      break;
    case APP_STATE_HISTORY:
      /* Rendered by the early history branch above. */
      break;
  }

  /* Round screens use the center as a dial and keep button labels short. */
  if (round) {
    switch (s_state) {
      case APP_STATE_SET_TIMER:
        snprintf(info_first, sizeof(info_first), "%s",
                 prv_text("HOLD = HISTORY", "TENIR = HIST."));
        snprintf(first_action, sizeof(first_action), "%s",
                 prv_text("UP/DOWN", "HAUT/BAS"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("NEXT", "SUIVANT"));
#ifdef PBL_PLATFORM_GABBRO
        main_font = strlen(value_text) <= 5U
            ? fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM)
            : compact_value_font;
#endif
        break;
      case APP_STATE_SET_DELAY:
        if (s_delay_seconds == 0U) {
          main_font = round_word_font;
        }
        snprintf(info_first, sizeof(info_first), "%s",
                 prv_text("QUIET · THEN 2", "SILENCE · PUIS 2"));
        snprintf(first_action, sizeof(first_action), "%s",
                 prv_text("UP/DOWN", "HAUT/BAS"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("NEXT", "SUIVANT"));
        break;
      case APP_STATE_SET_HAPTICS:
        main_font = round_word_font;
        snprintf(state_text, sizeof(state_text), "%s",
                 prv_text("HAPTIC", "VIBR."));
        snprintf(info_first, sizeof(info_first), "%s",
                 prv_text("COUNTS CYCLES", "COMPTE LES CYCLES"));
        snprintf(first_action, sizeof(first_action), "%s",
                 prv_text("UP/DOWN", "HAUT/BAS"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("NEXT", "SUIVANT"));
        break;
      case APP_STATE_READY:
        prv_format_time(s_timer_seconds, value_text, sizeof(value_text));
#ifdef PBL_PLATFORM_GABBRO
        main_font = strlen(value_text) <= 5U
            ? fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM)
            : compact_value_font;
#endif
        if (s_delay_seconds == 0U) {
          snprintf(info_first, sizeof(info_first), "%s · %s %s",
                   prv_text("NOW", "MAINT."), prv_text("HAPTIC", "VIB."),
                   s_haptics_enabled ? prv_text("YES", "OUI")
                                      : prv_text("NO", "NON"));
        } else {
          snprintf(info_first, sizeof(info_first), "%s %u · %s %s",
                   prv_text("DELAY", "DÉLAI"), s_delay_seconds,
                   prv_text("VIBE", "VIB."),
                   s_haptics_enabled ? prv_text("YES", "OUI")
                                      : prv_text("NO", "NON"));
        }
        snprintf(first_action, sizeof(first_action), "%s",
                 prv_text("HOLD START", "TENIR"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("EDIT", "MODIF."));
        break;
      case APP_STATE_WAITING:
        snprintf(info_first, sizeof(info_first), "%s",
                 prv_text("SECONDS · QUIET", "SECONDES · SILENCE"));
        first_action[0] = '\0';
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("STOP", "ARRÊT"));
#ifdef PBL_PLATFORM_GABBRO
        main_font = fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM);
#endif
        break;
      case APP_STATE_RUNNING:
      case APP_STATE_PAUSED:
        prv_format_cycle(s_cycle, value_text, sizeof(value_text));
        prv_format_time(s_elapsed_seconds, elapsed_text, sizeof(elapsed_text));
        prv_format_interval_short(s_timer_seconds, interval_text,
                                  sizeof(interval_text));
        snprintf(info_first, sizeof(info_first), "%s · %s %s",
                 elapsed_text, prv_text("EVERY", "CHAQUE"), interval_text);
        snprintf(first_action, sizeof(first_action), "%s",
                 s_state == APP_STATE_PAUSED
                     ? prv_text("RESUME", "REPRENDRE")
                     : prv_text("PAUSE", "PAUSE"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("STOP", "ARRÊT"));
#ifdef PBL_PLATFORM_GABBRO
        main_font = strlen(value_text) <= 4U
            ? fonts_get_system_font(FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM)
            : value_font;
#else
        if (strlen(value_text) > 4U) {
          main_font = compact_value_font;
        }
#endif
        break;
      case APP_STATE_STOP_CONFIRM:
        main_font = round_word_font;
        prv_format_time(s_elapsed_seconds, elapsed_text, sizeof(elapsed_text));
        prv_format_cycle(s_cycle, cycle_text, sizeof(cycle_text));
        snprintf(info_first, sizeof(info_first), "%s · %s", cycle_text,
                 elapsed_text);
        snprintf(first_action, sizeof(first_action), "%s",
                 prv_text("YES", "OUI"));
        snprintf(second_action, sizeof(second_action), "%s",
                 prv_text("NO", "NON"));
        break;
      case APP_STATE_HISTORY:
        /* Rendered by the early history branch above. */
        break;
    }
    info_second[0] = '\0';
  }

  prv_draw_round_dial(ctx, bounds, bounds.size.h / 2, band_background);
  prv_draw_animation(ctx, bounds,
                     round ? bounds.size.h / 2
                           : main_y + main_height / 2 - 2);
  prv_draw_state_band(ctx,
                      GRect(band_margin, band_y,
                            bounds.size.w - band_margin * 2, band_height),
                      state_text, step_text, band_background, band_foreground,
                      label_font);
  prv_draw_text(ctx, value_text, main_font,
                GRect(margin, main_y, content_width, main_height),
                GTextAlignmentCenter, foreground);
  prv_draw_text(ctx, info_first, info_font,
                GRect(margin, info_first_y, content_width, info_height),
                GTextAlignmentCenter, foreground);
  prv_draw_text(ctx, info_second, info_font,
                GRect(margin, info_second_y, content_width, info_height),
                GTextAlignmentCenter, foreground);
  prv_draw_actions(ctx, bounds, footer_y, first_action, second_action,
                   action_font);
}

/* Select advances setup, controls timing, or confirms a pending stop. */
static void prv_select_click_handler(ClickRecognizerRef recognizer,
                                     void *context) {
  switch (s_state) {
    case APP_STATE_SET_TIMER:
      prv_set_state(APP_STATE_SET_DELAY);
      break;
    case APP_STATE_SET_DELAY:
      prv_set_state(APP_STATE_SET_HAPTICS);
      break;
    case APP_STATE_SET_HAPTICS:
      prv_set_state(APP_STATE_READY);
      break;
    case APP_STATE_READY:
      /* Starting is intentionally reserved for the 700 ms long press. */
      prv_start_animation(ANIMATION_PULSE);
      break;
    case APP_STATE_RUNNING:
      prv_pause_timer();
      break;
    case APP_STATE_PAUSED:
      prv_resume_timer();
      break;
    case APP_STATE_STOP_CONFIRM:
      prv_confirm_stop();
      break;
    case APP_STATE_WAITING:
    case APP_STATE_HISTORY:
      break;
  }
}

/* A deliberate long press starts the timer or opens first-screen history. */
static void prv_select_long_click_handler(ClickRecognizerRef recognizer,
                                          void *context) {
  if (s_state == APP_STATE_READY) {
    prv_start_timer();
  } else if (s_state == APP_STATE_SET_TIMER) {
    s_history_offset = 0U;
    prv_set_state(APP_STATE_HISTORY);
  }
}

/* Up selects the next value for the current setup field. */
static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == APP_STATE_SET_TIMER) {
    s_timer_seconds = tick_timer_adjust_up(s_timer_seconds);
    prv_persist_int_checked(PERSIST_KEY_TIMER, (int)s_timer_seconds, "timer");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_SET_DELAY) {
    s_delay_seconds = tick_delay_next(s_delay_seconds);
    prv_persist_int_checked(PERSIST_KEY_DELAY, (int)s_delay_seconds, "delay");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_SET_HAPTICS) {
    s_haptics_enabled = true;
    prv_persist_int_checked(PERSIST_KEY_HAPTICS, 1, "haptics");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_HISTORY && s_history_offset > 0U) {
    --s_history_offset;
    prv_mark_dirty();
  }
}

/* Down selects the previous value for the current setup field. */
static void prv_down_click_handler(ClickRecognizerRef recognizer,
                                   void *context) {
  if (s_state == APP_STATE_SET_TIMER) {
    s_timer_seconds = tick_timer_adjust_down(s_timer_seconds);
    prv_persist_int_checked(PERSIST_KEY_TIMER, (int)s_timer_seconds, "timer");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_SET_DELAY) {
    s_delay_seconds = tick_delay_previous(s_delay_seconds);
    prv_persist_int_checked(PERSIST_KEY_DELAY, (int)s_delay_seconds, "delay");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_SET_HAPTICS) {
    s_haptics_enabled = false;
    prv_persist_int_checked(PERSIST_KEY_HAPTICS, 0, "haptics");
    prv_mark_dirty();
  } else if (s_state == APP_STATE_HISTORY &&
             s_history_offset + 1U < s_history.count) {
    ++s_history_offset;
    prv_mark_dirty();
  }
}

/* Back walks setup backward, guards a stop, or exits from the first screen. */
static void prv_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_state) {
    case APP_STATE_SET_TIMER:
      window_stack_pop(true);
      break;
    case APP_STATE_SET_DELAY:
      prv_set_state(APP_STATE_SET_TIMER);
      break;
    case APP_STATE_SET_HAPTICS:
      prv_set_state(APP_STATE_SET_DELAY);
      break;
    case APP_STATE_READY:
      prv_set_state(APP_STATE_SET_HAPTICS);
      break;
    case APP_STATE_WAITING:
    case APP_STATE_RUNNING:
    case APP_STATE_PAUSED:
      prv_ask_to_stop();
      break;
    case APP_STATE_STOP_CONFIRM:
      prv_cancel_stop();
      break;
    case APP_STATE_HISTORY:
      s_history_offset = 0U;
      prv_set_state(APP_STATE_SET_TIMER);
      break;
  }
}

/* Connects all physical buttons to the state machine. */
static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_PRESS_MS,
                              prv_select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Button handlers registered");
}

/* Reconciles the exact wall clock, then arms the next relative boundary. */
static void prv_reconcile_runtime(void) {
  const WallClockTime now = prv_now();
  if (s_state == APP_STATE_WAITING) {
    const uint32_t delay_ms = s_delay_seconds * 1000U;
    if (prv_time_elapsed_ms(s_wait_started_at, now) >= delay_ms) {
      prv_begin_active_timer(prv_time_add_ms(s_wait_started_at, delay_ms));
      return;
    } else {
      prv_mark_dirty();
      prv_schedule_runtime_timer();
    }
  } else if (s_state == APP_STATE_RUNNING) {
    prv_update_running_timer(now);
    if (s_state == APP_STATE_RUNNING) {
      prv_schedule_runtime_timer();
    }
  }
}

/* Handles the phase-accurate AppTimer wake-up for display and cycle changes. */
static void prv_runtime_timer_callback(void *context) {
  s_runtime_timer = NULL;
  prv_reconcile_runtime();
}

/* Provides a once-per-second fallback if an AppTimer allocation ever fails. */
static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_reconcile_runtime();
}

/* Reads the two integer tuple variants without accepting overflow or strings. */
static bool prv_tuple_to_int32(const Tuple *tuple, int32_t *value) {
  if (tuple == NULL || value == NULL) {
    return false;
  }
  if (tuple->type == TUPLE_INT) {
    *value = tuple->value->int32;
    return true;
  }
  if (tuple->type == TUPLE_UINT && tuple->value->uint32 <= INT32_MAX) {
    *value = (int32_t)tuple->value->uint32;
    return true;
  }
  return false;
}

/* Applies settings and returns a fresh history snapshot when requested. */
static void prv_inbox_received(DictionaryIterator *iterator, void *context) {
  Tuple *language_tuple = dict_find(iterator, MESSAGE_KEY_LANGUAGE);
  Tuple *statistics_tuple = dict_find(iterator,
                                      MESSAGE_KEY_SAVE_STATISTICS);
  Tuple *history_request_tuple = dict_find(iterator,
                                           MESSAGE_KEY_HISTORY_REQUEST);
  int32_t value;

  if (language_tuple != NULL) {
    if (!prv_tuple_to_int32(language_tuple, &value) ||
        (value != LANGUAGE_ENGLISH && value != LANGUAGE_FRENCH)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid AppMessage language tuple");
    } else {
      s_language = (Language)value;
      prv_persist_int_checked(PERSIST_KEY_LANGUAGE, (int)s_language,
                              "language");
      APP_LOG(APP_LOG_LEVEL_DEBUG, "AppMessage language applied: %ld",
              (long)value);
      prv_mark_dirty();
    }
  }

  if (statistics_tuple != NULL) {
    if (!prv_tuple_to_int32(statistics_tuple, &value) ||
        (value != 0 && value != 1)) {
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Invalid AppMessage statistics toggle tuple");
    } else {
      s_statistics_enabled = value == 1;
      prv_persist_int_checked(PERSIST_KEY_STATISTICS_ENABLED,
                              s_statistics_enabled ? 1 : 0, "statistics");
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Statistics saving applied: %d",
              s_statistics_enabled ? 1 : 0);
      prv_mark_dirty();
    }
  }

  if (history_request_tuple != NULL) {
    if (!prv_tuple_to_int32(history_request_tuple, &value) || value != 1) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid history request tuple");
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "History snapshot requested");
      prv_request_history_send();
    }
  }
}

/* Reports transport failures while leaving the current language untouched. */
static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage inbox dropped: %d", (int)reason);
}

/* Logs successful history/settings transport for emulator verification. */
static void prv_outbox_sent(DictionaryIterator *iterator, void *context) {
  s_history_send_in_flight = false;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "History page sent: %u",
          s_history_send_chunk);
  if (s_history_send_generation != s_history.generation) {
    s_history_send_chunk = 0U;
    s_history_send_pending = true;
  } else if (tick_history_chunk_size(&s_history,
                                     s_history_send_chunk + 1U) > 0U) {
    ++s_history_send_chunk;
    s_history_send_pending = true;
  } else {
    s_history_send_chunk = 0U;
  }
  prv_try_send_history();
}

/* Keeps history local when the phone is absent; a later request resynchronizes. */
static void prv_outbox_failed(DictionaryIterator *iterator,
                              AppMessageResult reason, void *context) {
  s_history_send_in_flight = false;
  s_history_send_pending = true;
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage outbox failed: %d", (int)reason);
}

/* Allocates the sole drawing layer using the platform's current bounds. */
static void prv_window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  if (!root_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to get root layer");
    return;
  }

  const GRect bounds = layer_get_bounds(root_layer);
  s_canvas_layer = layer_create(bounds);
  if (!s_canvas_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate canvas layer");
    return;
  }
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(root_layer, s_canvas_layer);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Layer loaded (%d x %d)", bounds.size.w,
          bounds.size.h);
}

/* Cancels callbacks and releases the window-owned custom layer. */
static void prv_window_unload(Window *window) {
  prv_cancel_runtime_timer();
  prv_cancel_animation();
  if (s_canvas_layer) {
    layer_destroy(s_canvas_layer);
    s_canvas_layer = NULL;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Layer unloaded");
}

/* Allocates app resources, loads settings, and presents the single window. */
static bool prv_init(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Initializing Tick Every");
  prv_load_settings();
  prv_load_history();

  s_window = window_create();
  if (!s_window) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate main window");
    return false;
  }
  window_set_background_color(s_window, GColorWhite);
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  const AppMessageResult app_message_result = app_message_open(124, 320);
  if (app_message_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to open AppMessage: %d",
            (int)app_message_result);
  } else {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "AppMessage opened");
  }

  tick_timer_service_subscribe(SECOND_UNIT, prv_tick_handler);
  window_stack_push(s_window, true);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Main window pushed");
  return true;
}

/* Releases app-level callbacks and ownership before process exit. */
static void prv_deinit(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Deinitializing Tick Every");
  tick_timer_service_unsubscribe();
  app_message_deregister_callbacks();
  vibes_cancel();
  prv_cancel_runtime_timer();
  prv_cancel_animation();
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}

/* Runs the Pebble event loop only after successful initialization. */
int main(void) {
  if (!prv_init()) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Initialization failed");
    prv_deinit();
    return 1;
  }
  app_event_loop();
  prv_deinit();
  return 0;
}
