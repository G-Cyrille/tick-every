#include <pebble.h>

#include "timer_logic.h"

/* Key 1 stored the retired finite duration; key 4 stores the repeat interval. */
#define PERSIST_KEY_TIMER 4
#define PERSIST_KEY_DELAY 2
#define PERSIST_KEY_HAPTICS 3

#define DEFAULT_TIMER_SECONDS 5U
#define DEFAULT_DELAY_SECONDS 0U
#define DEFAULT_HAPTICS_ENABLED true

#define LONG_PRESS_MS 700U
#define ANIMATION_STEP_MS 70U
#define PULSE_FRAME_COUNT 7U
/* 32 pulses use 63 alternating vibration/gap segments, our hard API limit. */
#define HAPTIC_SEGMENT_CAPACITY 63U
#define HAPTIC_MAX_PULSES ((HAPTIC_SEGMENT_CAPACITY + 1U) / 2U)
#define HAPTIC_DEADLINE_MARGIN_MS 20U

typedef enum {
  APP_STATE_SET_TIMER = 0,
  APP_STATE_SET_DELAY,
  APP_STATE_SET_HAPTICS,
  APP_STATE_READY,
  APP_STATE_WAITING,
  APP_STATE_RUNNING,
  APP_STATE_PAUSED,
  APP_STATE_STOP_CONFIRM
} AppState;

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
static unsigned int s_elapsed_seconds;
static uint64_t s_cycle;
static bool s_haptic_limit_logged;

/* A running segment starts after each resume; exact prior elapsed is preserved. */
static WallClockTime s_wait_started_at;
static WallClockTime s_run_segment_started_at;
static uint64_t s_elapsed_before_segment_ms;

/* Pebble consumes custom patterns asynchronously, so storage must outlive calls. */
static uint32_t s_haptic_durations[HAPTIC_SEGMENT_CAPACITY];

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
  }
  return "unknown";
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

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Settings loaded: timer=%u delay=%u haptics=%d",
          s_timer_seconds, s_delay_seconds, s_haptics_enabled ? 1 : 0);
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
  pattern_duration_ms = (unsigned int)code.long_vibrations * 180U +
                        (unsigned int)code.short_vibrations * 40U +
                        (pulse_count - 1U) * 20U;
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
    s_haptic_durations[segment_count++] = 180U;
    if (segment_count < pulse_count * 2U - 1U) {
      s_haptic_durations[segment_count++] = 20U;
    }
  }
  for (index = 0U; index < code.short_vibrations; ++index) {
    s_haptic_durations[segment_count++] = 40U;
    if (segment_count < pulse_count * 2U - 1U) {
      s_haptic_durations[segment_count++] = 20U;
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
  s_state_before_stop_confirm = s_state;
  prv_cancel_runtime_timer();
  prv_set_state(APP_STATE_STOP_CONFIRM);
}

/* Stops without changing the persisted configuration. */
static void prv_confirm_stop(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Timer stopped: elapsed=%u cycle=%llu",
          s_elapsed_seconds, (unsigned long long)s_cycle);
  prv_cancel_runtime_timer();
  vibes_cancel();
  prv_cancel_animation();
  s_elapsed_seconds = 0U;
  s_elapsed_before_segment_ms = 0U;
  s_cycle = 0U;
  s_haptic_limit_logged = false;
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

/* Draws an endless moving activity rail or a setup breadcrumb. */
static void prv_draw_progress(GContext *ctx, GRect bounds, int16_t margin,
                              int16_t y) {
  const int16_t width = bounds.size.w - margin * 2;
  const GColor track = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  const GColor fill = PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack);
  unsigned int step = 0U;
  unsigned int index;

  if (s_state == APP_STATE_RUNNING || s_state == APP_STATE_PAUSED ||
      s_state == APP_STATE_STOP_CONFIRM) {
    const int16_t marker_width = width / 4;
    const int16_t travel = width - marker_width;
    const int16_t marker_x = margin + (int16_t)(
        ((uint32_t)travel * (s_elapsed_seconds % 8U)) / 7U);
    graphics_context_set_fill_color(ctx, track);
    graphics_fill_rect(ctx, GRect(margin, y, width, 5), 2, GCornersAll);
    graphics_context_set_stroke_color(
        ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
    graphics_draw_round_rect(ctx, GRect(margin, y, width, 5), 2);
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(marker_x, y, marker_width, 5), 2,
                       GCornersAll);
    return;
  }

  if (s_state <= APP_STATE_READY) {
    step = (unsigned int)s_state;
  } else if (s_state == APP_STATE_WAITING) {
    step = 3U;
  }
  for (index = 0U; index < 4U; ++index) {
    const int16_t x = bounds.size.w / 2 - 24 + (int16_t)index * 16;
    graphics_context_set_fill_color(ctx, index <= step ? fill : track);
    graphics_fill_circle(ctx, GPoint(x, y + 2), index == step ? 4 : 3);
    graphics_context_set_stroke_color(ctx, fill);
    graphics_draw_circle(ctx, GPoint(x, y + 2), index == step ? 4 : 3);
  }
}

/* Adds an expanding, cheerful halo for active starts and cycle boundaries. */
static void prv_draw_animation(GContext *ctx, GRect bounds, int16_t center_y) {
  if (s_animation_kind == ANIMATION_PULSE && s_animation_frame > 0U) {
    const unsigned int progressed = PULSE_FRAME_COUNT - s_animation_frame;
    const int16_t radius = (int16_t)(25U + progressed * 3U);
    const GColor halo = PBL_IF_COLOR_ELSE(
        (progressed % 2U) ? GColorChromeYellow : GColorCyan, GColorBlack);
    graphics_context_set_stroke_color(ctx, halo);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, GPoint(bounds.size.w / 2, center_y), radius);
  }
}

/* Renders the complete interface on one lightweight platform-adaptive layer. */
static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const int16_t margin = PBL_IF_ROUND_ELSE(18, 6);
  const int16_t content_width = bounds.size.w - margin * 2;
  const int16_t header_y = PBL_IF_ROUND_ELSE(8, 2);
  const int16_t state_y = header_y + 18;
  const int16_t main_y = (int16_t)(bounds.size.h * 27 / 100);
  const int16_t main_height = PBL_IF_ROUND_ELSE(48, 44);
  const int16_t progress_y = (int16_t)(bounds.size.h * 57 / 100);
  const int16_t info_y = progress_y + 9;
  const int16_t hint_y = bounds.size.h - PBL_IF_ROUND_ELSE(54, 40);
  const int16_t hint_top_margin = PBL_IF_ROUND_ELSE(30, margin);
  const int16_t hint_bottom_margin = PBL_IF_ROUND_ELSE(40, margin);
  const GColor background = PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite);
  const GColor foreground = PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack);
  const GColor accent = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
  const GColor muted = PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack);
  const GFont tiny_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  const GFont bold_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  const GFont hint_top_font = tiny_font;
  const GFont value_font = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  const GFont compact_value_font =
      fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);
  GFont main_font = value_font;
  char state_text[24];
  char value_text[24];
  char info_text[48];
  char hint_top[40];
  char hint_bottom[40];

  graphics_context_set_fill_color(ctx, background);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  snprintf(state_text, sizeof(state_text), "TIMER");
  snprintf(value_text, sizeof(value_text), "%u s", s_timer_seconds);
  snprintf(info_text, sizeof(info_text), "Intervalle répétitif");
  snprintf(hint_top, sizeof(hint_top), "Haut/Bas : régler");
  snprintf(hint_bottom, sizeof(hint_bottom), "Select >   Back <");

  switch (s_state) {
    case APP_STATE_SET_TIMER:
      prv_format_time(s_timer_seconds, value_text, sizeof(value_text));
      if (s_timer_seconds >= 3600U) {
        main_font = compact_value_font;
      }
      break;
    case APP_STATE_SET_DELAY:
      snprintf(state_text, sizeof(state_text), "DÉLAI");
      if (s_delay_seconds == 0U) {
        snprintf(value_text, sizeof(value_text), "Aucun");
        main_font = compact_value_font;
      } else {
        snprintf(value_text, sizeof(value_text), "%u s", s_delay_seconds);
      }
      snprintf(info_text, sizeof(info_text), "Attente silencieuse");
      snprintf(hint_bottom, sizeof(hint_bottom), "Select >   Back <");
      break;
    case APP_STATE_SET_HAPTICS:
      snprintf(state_text, sizeof(state_text), "VIBRATIONS");
      snprintf(value_text, sizeof(value_text), "%s",
               s_haptics_enabled ? "Oui" : "Non");
      main_font = compact_value_font;
      snprintf(info_text, sizeof(info_text), "Code décimal");
      snprintf(hint_bottom, sizeof(hint_bottom), "Select >   Back <");
      break;
    case APP_STATE_READY:
      snprintf(state_text, sizeof(state_text), "PRÊT");
      prv_format_time(s_timer_seconds, value_text, sizeof(value_text));
      if (s_timer_seconds >= 3600U) {
        main_font = compact_value_font;
      }
      snprintf(info_text, sizeof(info_text), "Toutes les %u s%s",
               s_timer_seconds, s_delay_seconds ? " + délai" : "");
      snprintf(hint_top, sizeof(hint_top), "Tenir Select : lancer");
      snprintf(hint_bottom, sizeof(hint_bottom), "Back : modifier");
      break;
    case APP_STATE_WAITING: {
      const WallClockTime now = prv_now();
      const uint32_t delay_ms = s_delay_seconds * 1000U;
      const uint32_t waited_ms = prv_time_elapsed_ms(s_wait_started_at, now);
      const uint32_t remaining_ms = waited_ms < delay_ms
          ? delay_ms - waited_ms : 0U;
      const unsigned int remaining = (remaining_ms + 999U) / 1000U;
      snprintf(state_text, sizeof(state_text), "DÉMARRAGE DANS");
      snprintf(value_text, sizeof(value_text), "%u", remaining);
      snprintf(info_text, sizeof(info_text), "Puis 2 vibrations");
      snprintf(hint_top, sizeof(hint_top), "Patientez…");
      snprintf(hint_bottom, sizeof(hint_bottom), "Back : arrêter");
      break;
    }
    case APP_STATE_RUNNING:
    case APP_STATE_PAUSED: {
      snprintf(state_text, sizeof(state_text), "%s",
               s_state == APP_STATE_PAUSED ? "PAUSE" : "ACTIF");
      prv_format_time(s_elapsed_seconds, value_text, sizeof(value_text));
      if (s_elapsed_seconds >= 3600U) {
        main_font = compact_value_font;
      }
      snprintf(info_text, sizeof(info_text), "Cycle %llu · toutes les %u s",
               (unsigned long long)s_cycle, s_timer_seconds);
      snprintf(hint_top, sizeof(hint_top), "Select : %s",
               s_state == APP_STATE_PAUSED ? "reprendre" : "pause");
      snprintf(hint_bottom, sizeof(hint_bottom), "Back : arrêter");
      break;
    }
    case APP_STATE_STOP_CONFIRM:
      snprintf(state_text, sizeof(state_text), "CONFIRMATION");
      snprintf(value_text, sizeof(value_text), "Stop ?");
      main_font = compact_value_font;
      snprintf(info_text, sizeof(info_text), "Cycle %llu · %u s",
               (unsigned long long)s_cycle, s_elapsed_seconds);
      snprintf(hint_top, sizeof(hint_top), "Select : oui");
      snprintf(hint_bottom, sizeof(hint_bottom), "Back : non");
      break;
  }

  prv_draw_animation(ctx, bounds, main_y + main_height / 2 - 2);
  prv_draw_text(ctx, "TICK EVERY", tiny_font,
                GRect(margin, header_y, content_width, 18),
                GTextAlignmentCenter, foreground);
  prv_draw_text(ctx, state_text, bold_font,
                GRect(margin, state_y, content_width, 24),
                GTextAlignmentCenter, accent);
  prv_draw_text(ctx, value_text, main_font,
                GRect(margin, main_y, content_width, main_height),
                GTextAlignmentCenter, foreground);
  prv_draw_progress(ctx, bounds, margin, progress_y);
  prv_draw_text(ctx, info_text, tiny_font,
                GRect(margin, info_y, content_width, 20),
                GTextAlignmentCenter, muted);
  prv_draw_text(ctx, hint_top, hint_top_font,
                GRect(hint_top_margin, hint_y,
                      bounds.size.w - hint_top_margin * 2, 19),
                GTextAlignmentCenter, foreground);
  prv_draw_text(ctx, hint_bottom, tiny_font,
                GRect(hint_bottom_margin, hint_y + 20,
                      bounds.size.w - hint_bottom_margin * 2, 18),
                GTextAlignmentCenter, foreground);
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
      break;
  }
}

/* A deliberate long press starts only from the ready screen. */
static void prv_select_long_click_handler(ClickRecognizerRef recognizer,
                                          void *context) {
  if (s_state == APP_STATE_READY) {
    prv_start_timer();
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
      prv_set_state(s_state_before_stop_confirm);
      prv_reconcile_runtime();
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

  s_window = window_create();
  if (!s_window) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate main window");
    return false;
  }
  window_set_background_color(
      s_window, PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite));
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });

  tick_timer_service_subscribe(SECOND_UNIT, prv_tick_handler);
  window_stack_push(s_window, true);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Main window pushed");
  return true;
}

/* Releases app-level callbacks and ownership before process exit. */
static void prv_deinit(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Deinitializing Tick Every");
  tick_timer_service_unsubscribe();
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
