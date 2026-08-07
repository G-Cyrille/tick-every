#include <pebble.h>

static Window *s_window;
static TextLayer *s_title_layer;
static TextLayer *s_status_layer;

/* Confirms that the watch received a message from PebbleKit JS. */
static void prv_inbox_received_handler(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "AppMessage received");
}

/* Reports AppMessage payloads that could not be delivered to the watch. */
static void prv_inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage dropped: %d", reason);
}

/* Updates the placeholder status when SELECT is pressed. */
static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_status_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Cannot handle SELECT: status layer is NULL");
    return;
  }

  text_layer_set_text(s_status_layer, "Ready to configure");
  APP_LOG(APP_LOG_LEVEL_DEBUG, "SELECT pressed");
}

/* Updates the placeholder status when UP is pressed. */
static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_status_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Cannot handle UP: status layer is NULL");
    return;
  }

  text_layer_set_text(s_status_layer, "Interval +");
  APP_LOG(APP_LOG_LEVEL_DEBUG, "UP pressed");
}

/* Updates the placeholder status when DOWN is pressed. */
static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_status_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Cannot handle DOWN: status layer is NULL");
    return;
  }

  text_layer_set_text(s_status_layer, "Interval -");
  APP_LOG(APP_LOG_LEVEL_DEBUG, "DOWN pressed");
}

/* Connects the watch buttons to the initial app interactions. */
static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Button handlers registered");
}

/* Creates the screen layers using the current platform's dynamic bounds. */
static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  if (!window_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to get root layer");
    return;
  }

  const GRect bounds = layer_get_bounds(window_layer);

  s_title_layer = text_layer_create(GRect(0, bounds.size.h / 3,
                                          bounds.size.w, 34));
  if (!s_title_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate title layer");
    return;
  }
  text_layer_set_text(s_title_layer, "Tick Every");
  text_layer_set_font(s_title_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  s_status_layer = text_layer_create(GRect(4, bounds.size.h / 3 + 38,
                                           bounds.size.w - 8, 42));
  if (!s_status_layer) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate status layer");
    return;
  }
  text_layer_set_text(s_status_layer, "Press SELECT");
  text_layer_set_font(s_status_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Layers loaded (%d x %d)",
          bounds.size.w, bounds.size.h);
}

/* Releases every layer owned by the main window. */
static void prv_window_unload(Window *window) {
  if (s_status_layer) {
    text_layer_destroy(s_status_layer);
    s_status_layer = NULL;
  }
  if (s_title_layer) {
    text_layer_destroy(s_title_layer);
    s_title_layer = NULL;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Layers unloaded");
}

/* Allocates app resources, registers callbacks, and presents the main window. */
static bool prv_init(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Initializing Tick Every");

  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_register_inbox_dropped(prv_inbox_dropped_handler);
  const AppMessageResult app_message_result = app_message_open(128, 128);
  if (app_message_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to open AppMessage: %d",
            app_message_result);
    return false;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "AppMessage opened");

  s_window = window_create();
  if (!s_window) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to allocate main window");
    return false;
  }

  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Main window pushed: %p", s_window);
  return true;
}

/* Releases app-level resources before process exit. */
static void prv_deinit(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Deinitializing Tick Every");
  app_message_deregister_callbacks();
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}

/* Runs the Pebble application event loop after successful initialization. */
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
