#include <pebble.h>

#define PKEY_BEST       1
#define MAX_SHOTS       10
#define TICK_MS         33      // ~30 fps
#define FEEDBACK_FRAMES 18
#define FLASH_FRAMES    6

typedef enum {
  STATE_TITLE,
  STATE_PLAY,
  STATE_RESULT,
} GameState;

typedef enum {
  SHOT_NONE,
  SHOT_GOAL,
  SHOT_SAVE,
  SHOT_MISS,
} ShotResult;

typedef struct {
  GameState state;

  int goalie_x;
  int goalie_dir;
  int goalie_speed;

  int cross_x;
  int cross_dir;
  int cross_speed;
  bool aim_high;

  int shots_taken;
  int goals;
  int best;

  ShotResult last_result;
  int flash_frames;
  int feedback_frames;

  GRect goal_rect;
  int cross_range_min;
  int cross_range_max;
} Game;

static Window   *s_window;
static Layer    *s_canvas;
static AppTimer *s_timer;
static Game      g;

static void schedule_tick(void);
static void redraw(void) { if (s_canvas) layer_mark_dirty(s_canvas); }

static int goalie_width(void)  { return 22; }
static int goalie_height(void) { return g.goal_rect.size.h - 4; }

static void compute_layout(GRect bounds) {
  const int W = bounds.size.w;
  const int H = bounds.size.h;
  int gw = (W * 7) / 10;
  if (gw % 2) gw--;
  const int gh = 46;
  const int gx = bounds.origin.x + (W - gw) / 2;
  const int gy = bounds.origin.y + H * 30 / 100;
  g.goal_rect = GRect(gx, gy, gw, gh);
  g.cross_range_min = gx - 12;
  g.cross_range_max = gx + gw + 12;
}

static void reset_round(void) {
  g.shots_taken     = 0;
  g.goals           = 0;
  g.goalie_x        = g.goal_rect.origin.x + 4;
  g.goalie_dir      = 1;
  g.goalie_speed    = 2;
  g.cross_x         = g.cross_range_min;
  g.cross_dir       = 1;
  g.cross_speed     = 3;
  g.aim_high        = false;
  g.last_result     = SHOT_NONE;
  g.flash_frames    = 0;
  g.feedback_frames = 0;
}

static void advance_positions(void) {
  const int gx_min = g.goal_rect.origin.x + 2;
  const int gx_max = g.goal_rect.origin.x + g.goal_rect.size.w - 2 - goalie_width();

  g.goalie_x += g.goalie_dir * g.goalie_speed;
  if (g.goalie_x <= gx_min) { g.goalie_x = gx_min; g.goalie_dir = 1;  }
  if (g.goalie_x >= gx_max) { g.goalie_x = gx_max; g.goalie_dir = -1; }

  g.cross_x += g.cross_dir * g.cross_speed;
  if (g.cross_x <= g.cross_range_min) { g.cross_x = g.cross_range_min; g.cross_dir = 1;  }
  if (g.cross_x >= g.cross_range_max) { g.cross_x = g.cross_range_max; g.cross_dir = -1; }
}

static ShotResult resolve_shot(void) {
  const int gx = g.goal_rect.origin.x;
  const int gw = g.goal_rect.size.w;
  if (g.cross_x < gx || g.cross_x > gx + gw) return SHOT_MISS;

  int gol_x1 = g.goalie_x;
  int gol_x2 = g.goalie_x + goalie_width();
  // High shots: goalie's glove reach shrinks the effective hitbox.
  if (g.aim_high) { gol_x1 += 5; gol_x2 -= 5; }

  if (g.cross_x >= gol_x1 && g.cross_x <= gol_x2) return SHOT_SAVE;
  return SHOT_GOAL;
}

static void apply_shot(void) {
  const ShotResult r = resolve_shot();
  g.last_result     = r;
  g.feedback_frames = FEEDBACK_FRAMES;

  if (r == SHOT_GOAL) {
    g.goals++;
    g.flash_frames = FLASH_FRAMES;
    vibes_short_pulse();
    if (g.goalie_speed < 5) g.goalie_speed++;
  }

  g.shots_taken++;
  if (g.shots_taken >= MAX_SHOTS) {
    if (g.goals > g.best) {
      g.best = g.goals;
      persist_write_int(PKEY_BEST, g.best);
    }
    g.state = STATE_RESULT;
  }
}

// ---------- drawing ----------

static void draw_goal(GContext *ctx) {
  const GRect gr = g.goal_rect;

  // Net background
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, gr, 0, GCornerNone);

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  for (int i = 4; i < gr.size.w; i += 6) {
    graphics_draw_line(ctx,
      GPoint(gr.origin.x + i, gr.origin.y + 1),
      GPoint(gr.origin.x + i, gr.origin.y + gr.size.h - 1));
  }
  for (int i = 4; i < gr.size.h; i += 6) {
    graphics_draw_line(ctx,
      GPoint(gr.origin.x + 1,             gr.origin.y + i),
      GPoint(gr.origin.x + gr.size.w - 1, gr.origin.y + i));
  }
#endif

  // Posts + crossbar in red (mono: black)
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
  graphics_fill_rect(ctx, GRect(gr.origin.x - 2,                 gr.origin.y,     3, gr.size.h + 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(gr.origin.x + gr.size.w - 1,     gr.origin.y,     3, gr.size.h + 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(gr.origin.x - 2,                 gr.origin.y - 2, gr.size.w + 4, 3), 0, GCornerNone);

  // Goal line
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
  graphics_draw_line(ctx,
    GPoint(gr.origin.x - 10,             gr.origin.y + gr.size.h + 1),
    GPoint(gr.origin.x + gr.size.w + 10, gr.origin.y + gr.size.h + 1));
}

static void draw_goalie(GContext *ctx) {
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack));
  GRect r = GRect(g.goalie_x, g.goal_rect.origin.y + 4, goalie_width(), goalie_height());
  graphics_fill_rect(ctx, r, 2, GCornersAll);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(r.origin.x + r.size.w / 2, r.origin.y + 6), 3);
}

static void draw_crosshair(GContext *ctx) {
  const int y = g.aim_high
    ? g.goal_rect.origin.y + 4
    : g.goal_rect.origin.y + g.goal_rect.size.h + 6;

  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
  graphics_context_set_fill_color(ctx,   PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
  graphics_fill_circle(ctx, GPoint(g.cross_x, y), 3);

  const int dy = g.aim_high ? -8 : 8;
  graphics_draw_line(ctx, GPoint(g.cross_x, y), GPoint(g.cross_x, y + dy));
}

static void draw_title(GContext *ctx, GRect b) {
  graphics_context_set_text_color(ctx, GColorBlack);
  GFont big   = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont tiny  = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  graphics_draw_text(ctx, "HOCKEY", big,
    GRect(b.origin.x, b.origin.y + 8, b.size.w, 34),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SELECT to start", small,
    GRect(b.origin.x, b.origin.y + b.size.h - 44, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  char buf[24];
  snprintf(buf, sizeof(buf), "Best: %d", g.best);
  graphics_draw_text(ctx, buf, tiny,
    GRect(b.origin.x, b.origin.y + b.size.h - 22, b.size.w, 18),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_hud(GContext *ctx, GRect b) {
  GFont tiny = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  graphics_context_set_text_color(ctx, GColorBlack);

  char left[16], right[16];
  const int display_shot = g.shots_taken + 1 > MAX_SHOTS ? MAX_SHOTS : g.shots_taken + 1;
  snprintf(left,  sizeof(left),  "Shot %d/%d", display_shot, MAX_SHOTS);
  snprintf(right, sizeof(right), "Goals %d",   g.goals);

  const int top = b.origin.y + 2;
  graphics_draw_text(ctx, left, tiny,
    GRect(b.origin.x + 4, top, b.size.w - 8, 16),
    GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, right, tiny,
    GRect(b.origin.x + 4, top, b.size.w - 8, 16),
    GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);

  char aim[16];
  snprintf(aim, sizeof(aim), "Aim: %s", g.aim_high ? "HIGH" : "LOW");
  graphics_draw_text(ctx, aim, tiny,
    GRect(b.origin.x, b.origin.y + b.size.h - 20, b.size.w, 18),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  if (g.feedback_frames > 0) {
    const char *msg = "";
    switch (g.last_result) {
      case SHOT_GOAL: msg = "GOAL!"; break;
      case SHOT_SAVE: msg = "SAVE";  break;
      case SHOT_MISS: msg = "MISS";  break;
      default:                       break;
    }
    GFont big = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(
      g.last_result == SHOT_GOAL ? GColorRed : GColorBlack,
      GColorBlack));
    graphics_draw_text(ctx, msg, big,
      GRect(b.origin.x, b.origin.y + b.size.h - 52, b.size.w, 30),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void draw_result(GContext *ctx, GRect b) {
  GFont big   = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont tiny  = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  graphics_context_set_text_color(ctx, GColorBlack);

  char final_score[16], best_str[16];
  snprintf(final_score, sizeof(final_score), "%d / %d", g.goals, MAX_SHOTS);
  snprintf(best_str,    sizeof(best_str),    "Best: %d", g.best);

  graphics_draw_text(ctx, "FINAL", small,
    GRect(b.origin.x, b.origin.y + 18, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, final_score, big,
    GRect(b.origin.x, b.origin.y + 42, b.size.w, 34),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, best_str, small,
    GRect(b.origin.x, b.origin.y + 82, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SELECT: play again", tiny,
    GRect(b.origin.x, b.origin.y + b.size.h - 38, b.size.w, 18),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "BACK: exit", tiny,
    GRect(b.origin.x, b.origin.y + b.size.h - 20, b.size.w, 18),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  GColor bg = PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite);
  if (g.flash_frames > 0) {
    bg = PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite);
  }
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  switch (g.state) {
    case STATE_TITLE:
      draw_goal(ctx);
      draw_goalie(ctx);
      draw_title(ctx, b);
      break;
    case STATE_PLAY:
      draw_goal(ctx);
      draw_goalie(ctx);
      draw_crosshair(ctx);
      draw_hud(ctx, b);
      break;
    case STATE_RESULT:
      draw_result(ctx, b);
      break;
  }
}

// ---------- tick ----------

static void tick(void *data) {
  s_timer = NULL;
  if (g.state == STATE_PLAY) {
    advance_positions();
    if (g.flash_frames    > 0) g.flash_frames--;
    if (g.feedback_frames > 0) g.feedback_frames--;
    redraw();
  }
  schedule_tick();
}

static void schedule_tick(void) {
  s_timer = app_timer_register(TICK_MS, tick, NULL);
}

// ---------- input ----------

static void select_click(ClickRecognizerRef rec, void *context) {
  switch (g.state) {
    case STATE_TITLE:
      reset_round();
      g.state = STATE_PLAY;
      break;
    case STATE_PLAY:
      apply_shot();
      break;
    case STATE_RESULT:
      reset_round();
      g.state = STATE_PLAY;
      break;
  }
  redraw();
}

static void up_click(ClickRecognizerRef rec, void *context) {
  if (g.state == STATE_PLAY) { g.aim_high = true;  redraw(); }
}

static void down_click(ClickRecognizerRef rec, void *context) {
  if (g.state == STATE_PLAY) { g.aim_high = false; redraw(); }
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_UP,     up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click);
}

// ---------- lifecycle ----------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, update_proc);
  layer_add_child(root, s_canvas);

  compute_layout(b);
  g.best  = persist_read_int(PKEY_BEST);
  reset_round();
  g.state = STATE_TITLE;

  schedule_tick();
}

static void window_unload(Window *window) {
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  if (s_canvas) { layer_destroy(s_canvas); s_canvas = NULL; }
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite));
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
