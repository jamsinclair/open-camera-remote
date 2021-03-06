#include <pebble.h>
#include "comm/comm.h"
#include "windows/alert_window.h"

#define CAMERA_STATE_READY "camera_state_ready"
#define CAMERA_STATE_IN_PROGRESS "camera_state_in_progress"
#define CAMERA_STATE_CANCELLED "camera_state_cancelled"

static Window *s_main_window;

// Layers
static TextLayer *s_start_layer;
static TextLayer *s_main_layer;
static TextLayer *s_countdown_layer;
static Layer *s_canvas_layer;

// Color Globals
static GColor BG_COLOR;
static GColor TEXT_COLOR;

// Misc Globals
static int s_timer_value = 0;
static int s_timer_countdown_value = 0;
static char* s_current_text_layer;
static char* s_camera_state;
static AppTimer *countdown_timer;

/********************************* Helper Methods ************************************/
char* intToStrPointer(int i) {
  size_t needed = snprintf(NULL, 0, "%d", i);
  char *buffer = malloc(needed+1);
  snprintf(buffer, sizeof(buffer), "%d", i);
  return buffer;
}

static int roundFloat(float num) {
  return num < 0 ? (num - 0.5) : (num + 0.5);
}

// Takes X or Y axis values, determines offset to center the inner object
// within the parent object
static int getCenterOffset(int parentObj, int innerObj) {
  return (parentObj - innerObj) / 2;
}

/********************************* Camera Graphic ************************************/

// Returns the center offset for camera graphic which is also the top section height of graphic
static int getCameraGraphicCenterOffset(GRect bounds) {
  // To account for the top section of camera graphic we need to offset centers with this value
  return roundFloat(bounds.size.w * PBL_IF_RECT_ELSE(0.1, 0.08));
}

static void draw_camera_background(GContext *ctx, GRect bounds) {
  GRect main_layer_bounds = layer_get_bounds(text_layer_get_layer(s_main_layer));

  int camera_width = roundFloat(bounds.size.w * PBL_IF_RECT_ELSE(0.8, 0.65));
  // Camera graphic's height is 80% of its own width
  int camera_height = roundFloat(camera_width * 0.8);
  int camera_top_width = roundFloat(camera_width / 2);
  int camera_top_height = getCameraGraphicCenterOffset(bounds);
  // Ensure camera lens will cover the rect main text layer
  int camera_lens_radius = (main_layer_bounds.size.w + 16) / 2;

  int camera_center_height = camera_height - camera_top_height;

  // Camera Body
  graphics_context_set_fill_color(ctx, TEXT_COLOR);
  graphics_fill_rect(ctx, GRect(getCenterOffset(bounds.size.w, camera_width),
      getCenterOffset(bounds.size.h, camera_center_height), camera_width, camera_height),
    4,
    GCornersAll);

  // Camera Body Top
  graphics_fill_rect(ctx, GRect(getCenterOffset(bounds.size.w, camera_top_width),
      getCenterOffset(bounds.size.h, camera_center_height) - camera_top_height, camera_top_width, camera_top_height),
    4,
    GCornerTopLeft | GCornerTopRight);

  // Camera Lens
  graphics_context_set_fill_color(ctx, BG_COLOR);
  graphics_fill_circle(ctx, GPoint(bounds.size.w / 2, (bounds.size.h + camera_top_height) / 2), camera_lens_radius);
}

static void canvas_update_proc(Layer *this_layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(this_layer);

  draw_camera_background(ctx, bounds);
}

/********************************* Layers ************************************/

static void init_start_layer(GRect bounds) {
  const int text_height = 95;

  GEdgeInsets text_insets = PBL_IF_RECT_ELSE(GEdgeInsets(getCenterOffset(bounds.size.h, text_height), 4),
    GEdgeInsets(getCenterOffset(bounds.size.h, text_height), bounds.size.w / 6, 0, bounds.size.w / 6));

  s_start_layer = text_layer_create(grect_inset(bounds, text_insets));
  text_layer_set_overflow_mode(s_start_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_start_layer, "Press up and down to set timer\n\nMiddle button to capture");
  text_layer_set_text_alignment(s_start_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_start_layer, TEXT_COLOR);
  text_layer_set_background_color(s_start_layer, BG_COLOR);
  text_layer_set_font(s_start_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
}

static void init_main_layer(GRect bounds) {
  int text_width = 46;
  int text_height = 42;
  int camera_center_offset = getCameraGraphicCenterOffset(bounds);

  // Center main text layer to the camera graphic center
  s_main_layer = text_layer_create(GRect(getCenterOffset(bounds.size.w, text_width),
    getCenterOffset(bounds.size.h + camera_center_offset, text_height),
    text_width,
    text_height)
  );

  text_layer_set_overflow_mode(s_main_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_main_layer, intToStrPointer(s_timer_value));
  text_layer_set_text_alignment(s_main_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_main_layer, TEXT_COLOR);
  text_layer_set_background_color(s_main_layer, BG_COLOR);
  text_layer_set_font(s_main_layer, fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS));

  layer_set_hidden(text_layer_get_layer(s_main_layer), true);
}

static void init_countdown_layer(GRect bounds) {
  const int text_height = 42;

  // Display the text above the countdown message above camera body graphic
  // @TODO find a nicer way of doing this, magic numbers, magic numbers
  int top_offset = roundFloat(PBL_IF_RECT_ELSE(bounds.size.w * (0.8 * 0.8), bounds.size.w * (0.8 * 0.65)) - text_height - getCameraGraphicCenterOffset(bounds));

  s_countdown_layer = text_layer_create(GRect(0, top_offset, bounds.size.w, text_height));
  text_layer_set_overflow_mode(s_countdown_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_countdown_layer, "Taking picture in...");
  text_layer_set_text_alignment(s_countdown_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_countdown_layer, TEXT_COLOR);
  text_layer_set_background_color(s_countdown_layer, BG_COLOR);
  text_layer_set_font(s_countdown_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_set_hidden(text_layer_get_layer(s_countdown_layer), true);
}

static void init_canvas_layer(GRect bounds) {
  s_canvas_layer = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
  layer_set_hidden(s_canvas_layer, true);
}

/********************************* States ************************************/

static bool showing_start_layer() {
  return (strcmp(s_current_text_layer, "start") == 0);
}

static bool is_camera_state_ready() {
  return (strcmp(s_camera_state, CAMERA_STATE_READY) == 0);
}

static bool is_camera_state_in_progress() {
  return (strcmp(s_camera_state, CAMERA_STATE_IN_PROGRESS) == 0);
}

static bool is_camera_state_cancelled() {
  return (strcmp(s_camera_state, CAMERA_STATE_CANCELLED) == 0);
}

static void ensure_main_text_layer_showing() {
  if (showing_start_layer()) {
    layer_set_hidden(text_layer_get_layer(s_start_layer), true);
    layer_set_hidden(text_layer_get_layer(s_main_layer), false);
    layer_set_hidden(s_canvas_layer, false);

    s_current_text_layer = "main";

    APP_LOG(APP_LOG_LEVEL_INFO, "Main text layer is now showing");
  }
}

static void set_camera_state_layer_ready() {
  s_camera_state = CAMERA_STATE_READY;

  text_layer_set_text(s_main_layer, intToStrPointer(s_timer_value));
  layer_set_hidden(text_layer_get_layer(s_countdown_layer), true);
  layer_set_hidden(s_canvas_layer, false);
}

static void set_camera_state_layer_in_progress() {
  s_camera_state = CAMERA_STATE_IN_PROGRESS;

  layer_set_hidden(text_layer_get_layer(s_countdown_layer), false);
  layer_set_hidden(s_canvas_layer, true);
}

/********************************* App Message Error Handlers ************************************/

void message_timeout_handler(void *data) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send timed out");

  // Failed to send message to app, display alert window
  alert_window_push();
}

/********************************* Countdown Camera ************************************/

void camera_countdown_loop() {
  if (s_timer_countdown_value <= 0) {
    // Set a fallback timeout in case no message sent from Companion App on success of picture taken
    countdown_timer = app_timer_register(5000, &set_camera_state_layer_ready, NULL);
    return;
  }

  if (is_camera_state_cancelled()) {
    text_layer_set_text(s_countdown_layer, "Timer cancelled.");
    app_timer_register(2000, &set_camera_state_layer_ready, NULL);
    return;
  }

  s_timer_countdown_value--;
  text_layer_set_text(s_main_layer, intToStrPointer(s_timer_countdown_value));
  countdown_timer = app_timer_register(1000, &camera_countdown_loop, NULL);
}

static void start_camera_countdown() {
  // Increment by 1, as the loop immeddiately decrements
  s_timer_countdown_value = s_timer_value + 1;
  text_layer_set_text(s_countdown_layer, "Taking picture in...");
  set_camera_state_layer_in_progress();
  camera_countdown_loop();
}

void on_picture_taken() {
  APP_LOG(APP_LOG_LEVEL_INFO, "Picture Taken by Companion App");

  if (!is_camera_state_in_progress()) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Picture taken message received but pebble camera state was not in progress");
    return;
  }

  // Cancel the fallback timer if set
  if (countdown_timer) {
    app_timer_cancel(countdown_timer);
  }

  text_layer_set_text(s_countdown_layer, "Picture Taken");

  if (s_timer_countdown_value > 0) {
    text_layer_set_text(s_main_layer, "0");
  }

  vibes_double_pulse();

  app_timer_register(2000, &set_camera_state_layer_ready, NULL);
}

/********************************* Buttons ************************************/

static void increment_camera_timer() {
  if (s_timer_value < 30) {
    s_timer_value++;
    text_layer_set_text(s_main_layer, intToStrPointer(s_timer_value));
  }
}

static void decrement_camera_timer() {
  if (s_timer_value > 0) {
    s_timer_value--;
    text_layer_set_text(s_main_layer, intToStrPointer(s_timer_value));
  }
}

static void start_layer_click_handler() {
  APP_LOG(APP_LOG_LEVEL_INFO, "Start Click Handler Activated");

  ensure_main_text_layer_showing();
  s_camera_state = CAMERA_STATE_READY;
  send_int_app_message_with_callback(KEY_APP_STATUS_CHECK, s_timer_value, &message_timeout_handler);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (showing_start_layer()) {
    start_layer_click_handler();
    return;
  }

  if (is_camera_state_ready()) {
    increment_camera_timer();
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (showing_start_layer()) {
    start_layer_click_handler();
    return;
  }

  if (is_camera_state_ready()) {
    start_camera_countdown();
    send_int_app_message_with_callback(KEY_CAPTURE, s_timer_value, &message_timeout_handler);
  } else if (is_camera_state_in_progress()) {
    s_camera_state = CAMERA_STATE_CANCELLED;
    send_int_app_message_with_callback(KEY_CAPTURE, s_timer_value, &message_timeout_handler);
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (showing_start_layer()) {
    start_layer_click_handler();
    return;
  }

  if (is_camera_state_ready()) {
    decrement_camera_timer();
  }
}

static void click_config_provider(void *context) {
  // Register the ClickHandlers
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, down_click_handler);
}

/********************************* Main Window ************************************/

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  window_set_background_color(window, BG_COLOR);
  window_set_click_config_provider(s_main_window, click_config_provider);

  init_start_layer(bounds);
  init_main_layer(bounds);
  init_countdown_layer(bounds);
  init_canvas_layer(bounds);

  s_current_text_layer = "start";

  layer_add_child(window_layer, s_canvas_layer);
  layer_add_child(window_layer, text_layer_get_layer(s_start_layer));
  layer_add_child(window_layer, text_layer_get_layer(s_main_layer));
  layer_add_child(window_layer, text_layer_get_layer(s_countdown_layer));

  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_start_layer);
  text_layer_destroy(s_main_layer);
  text_layer_destroy(s_countdown_layer);
  layer_destroy(s_canvas_layer);
}

static void init(void) {
  BG_COLOR = COLOR_FALLBACK(GColorBlueMoon, GColorBlack);
  TEXT_COLOR = GColorWhite;

  init_comm();
  register_picture_taken_callback(&on_picture_taken);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  window_destroy(s_main_window);
  deinit_comm();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
