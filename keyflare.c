/*
 * keyflare — floating key HUD for Wayland (cosmic-comp / Pop!_OS COSMIC)
 *
 * A glassy "pill" anchored bottom-right showing the last key / modifier combo
 * (e.g. "Ctrl + Shift + C"). Wayland layer-shell (gtk-layer-shell) OVERLAY
 * surface: always on top, anchored, undecorated, off the taskbar, not movable.
 *
 * Behaviour:
 *   - Idle = invisible: with no typing the pill fades to opacity 0; the instant
 *     you press a key it snaps to 100%.
 *   - Heat / "speedometer": typing continuously builds heat (cool -> warm ->
 *     orange -> RED). It only goes full red after ~10s of sustained typing; a
 *     pause resets it.
 *   - Hammer-bang: once hot (red), every keypress punches (key slams big then
 *     settles) like striking a hammer.
 *   - CLICK the pill -> password mode: display pauses, shows a lock, so your
 *     password keystrokes are never shown. Click again to resume.
 *
 * Keys from `libinput debug-events --show-keycodes` (direct if in the `input`
 * group, else via pkexec). Parsed in the GLib main loop.
 *
 * Build:  make      Run: ./keyflare      No prompt: sudo usermod -aG input $USER
 *
 * Tuning via env (also used by the test harness):
 *   KEYFLARE_HOT_MS    sustained-typing ms before red       (default 10000)
 *   KEYFLARE_RESET_MS  pause ms that resets the heat streak  (default 2000)
 *   KEYFLARE_DEBUG=1   trace "KEY <text> hot=<0|1>" to stderr, quit on stream end
 *   KEYFLARE_NO_LISTEN=1  build the overlay only (self-check)
 */

#define _GNU_SOURCE          /* for group_member() from <grp.h> */
#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <grp.h>
#include <unistd.h>

#define HOT_MS        10000   /* sustained typing before red */
#define RESET_MS       2000   /* pause that resets the heat streak */
#define IDLE_FADE_MS   1400   /* idle before the pill fades out */
#define BASE_SIZE     24000   /* pango font size (1/1000 pt) */
#define SPEED_SLOW_MS   350   /* inter-key gap at/above which recoil is minimal */
#define SPEED_FAST_MS    55   /* inter-key gap at/below which recoil is maximal */

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4, MOD_SUPER = 8 };

static GtkWidget *hud;        /* the glass pill (a GtkBox) */
static GtkWidget *label;      /* the key text */
static char current_text[256] = " ";

static gint64 last_key_us = 0;
static gint64 streak_start_us = 0;
static int held_mods = 0;
static gboolean muted = FALSE;

static double hud_op = 1.0;
static guint idle_id = 0, fade_id = 0;     /* idle -> fade-out */
static guint bang_id = 0; static int bang_i = 0;   /* hammer punch */

static int env_int(const char *name, int defv)
{
    const char *v = g_getenv(name);
    if (!v) return defv;
    int x = atoi(v);
    return x > 0 ? x : defv;
}

/* ---- key naming -------------------------------------------------------- */

static void nice_name(const char *raw, char *out, size_t outlen)
{
    static const struct { const char *k, *v; } map[] = {
        {"LEFTCTRL", "Ctrl"},  {"RIGHTCTRL", "Ctrl"},
        {"LEFTSHIFT", "Shift"},{"RIGHTSHIFT", "Shift"},
        {"LEFTALT", "Alt"},    {"RIGHTALT", "AltGr"},
        {"LEFTMETA", "Super"}, {"RIGHTMETA", "Super"},
        {"CAPSLOCK", "Caps"},  {"SPACE", "␣"},
        {"ENTER", "⏎"},   {"KPENTER", "⏎"},   {"BACKSPACE", "⌫"},
        {"TAB", "⇥"},     {"ESC", "Esc"},          {"DELETE", "Del"},
        {"UP", "↑"}, {"DOWN", "↓"}, {"LEFT", "←"}, {"RIGHT", "→"},
        {"PAGEUP", "PgUp"},    {"PAGEDOWN", "PgDn"},
        {"HOME", "Home"},      {"END", "End"},
        {"INSERT", "Ins"},     {"PRINT", "PrtSc"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        if (strcmp(raw, map[i].k) == 0) { g_strlcpy(out, map[i].v, outlen); return; }

    if (raw[0] == 'F' && isdigit((unsigned char)raw[1])) { g_strlcpy(out, raw, outlen); return; }
    if (raw[0] == 'K' && raw[1] == 'P' && raw[2]) { g_strlcpy(out, raw + 2, outlen); return; }
    gboolean alldigit = raw[0] != '\0';
    for (const char *q = raw; *q; q++) if (!isdigit((unsigned char)*q)) { alldigit = FALSE; break; }
    if (alldigit) { g_strlcpy(out, raw, outlen); return; }

    size_t j = 0; gboolean word_start = TRUE;
    for (const char *q = raw; *q && j < outlen - 1; q++) {
        if (*q == '_') { out[j++] = ' '; word_start = TRUE; continue; }
        out[j++] = word_start ? toupper((unsigned char)*q) : tolower((unsigned char)*q);
        word_start = FALSE;
    }
    out[j] = '\0';
}

static int mod_bit(const char *raw)
{
    if (!strcmp(raw, "LEFTCTRL")  || !strcmp(raw, "RIGHTCTRL"))  return MOD_CTRL;
    if (!strcmp(raw, "LEFTALT")   || !strcmp(raw, "RIGHTALT"))   return MOD_ALT;
    if (!strcmp(raw, "LEFTSHIFT") || !strcmp(raw, "RIGHTSHIFT")) return MOD_SHIFT;
    if (!strcmp(raw, "LEFTMETA")  || !strcmp(raw, "RIGHTMETA"))  return MOD_SUPER;
    return 0;
}

static void join_mods(char *out, size_t n, int mods)
{
    static const struct { int bit; const char *name; } order[] = {
        {MOD_CTRL, "Ctrl"}, {MOD_ALT, "Alt"}, {MOD_SHIFT, "Shift"}, {MOD_SUPER, "Super"},
    };
    out[0] = '\0';
    gboolean first = TRUE;
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        if (!(mods & order[i].bit)) continue;
        if (!first) g_strlcat(out, " + ", n);
        g_strlcat(out, order[i].name, n);
        first = FALSE;
    }
}

/* ---- rendering & animation -------------------------------------------- */

static void render_label(const char *text, double scale)
{
    char *esc = g_markup_escape_text(text, -1);
    char *m = g_strdup_printf(
        "<span font_family=\"monospace\" size=\"%d\" weight=\"600\">%s</span>",
        (int)(BASE_SIZE * scale), esc);
    gtk_label_set_markup(GTK_LABEL(label), m);
    g_free(m); g_free(esc);
}

/* gunshot recoil: instant scale punch + muzzle-flash glow, snappy decay.
 * peak scales with typing speed -> faster typing bangs harder. */
static double bang_peak = 1.10;
static const double BANG_CURVE[] = { 0.62, 0.30, 0.10, 0.0 };   /* decay after the instant peak */
static gboolean bang_step(gpointer _)
{
    (void)_;
    if (bang_i >= (int)G_N_ELEMENTS(BANG_CURVE)) {
        render_label(current_text, 1.0);
        gtk_style_context_remove_class(gtk_widget_get_style_context(label), "shot");
        bang_id = 0;
        return G_SOURCE_REMOVE;
    }
    render_label(current_text, 1.0 + (bang_peak - 1.0) * BANG_CURVE[bang_i++]);
    return G_SOURCE_CONTINUE;
}
static void gunshot(double intensity)
{
    bang_peak = 1.10 + 0.35 * intensity;            /* slow ~1.10x, fast ~1.45x */
    if (bang_id) g_source_remove(bang_id);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "shot");
    render_label(current_text, bang_peak);          /* instant peak = sharp attack */
    bang_i = 0;
    bang_id = g_timeout_add(14, bang_step, NULL);
}

/* idle -> fade the pill to invisible */
static gboolean fade_step(gpointer _)
{
    (void)_;
    hud_op -= 0.1;
    if (hud_op <= 0.0) { hud_op = 0.0; gtk_widget_set_opacity(hud, 0.0); fade_id = 0; return G_SOURCE_REMOVE; }
    gtk_widget_set_opacity(hud, hud_op);
    return G_SOURCE_CONTINUE;
}
static gboolean idle_fire(gpointer _)
{
    (void)_;
    idle_id = 0;
    if (fade_id) g_source_remove(fade_id);
    fade_id = g_timeout_add(25, fade_step, NULL);
    return G_SOURCE_REMOVE;
}
/* snap to fully visible now, and (re)arm the idle fade-out */
static void wake_fade(void)
{
    if (fade_id) { g_source_remove(fade_id); fade_id = 0; }
    hud_op = 1.0;
    gtk_widget_set_opacity(hud, 1.0);
    if (idle_id) g_source_remove(idle_id);
    idle_id = muted ? 0 : g_timeout_add(IDLE_FADE_MS, idle_fire, NULL);
}

/* speedometer: cool -> warm -> orange -> red as heat climbs */
static void apply_heat(double heat, gboolean hot)
{
    GtkStyleContext *c = gtk_widget_get_style_context(hud);
    gtk_style_context_remove_class(c, "warm");
    gtk_style_context_remove_class(c, "hotter");
    gtk_style_context_remove_class(c, "hot");
    if (hot)               gtk_style_context_add_class(c, "hot");
    else if (heat >= 0.66) gtk_style_context_add_class(c, "hotter");
    else if (heat >= 0.33) gtk_style_context_add_class(c, "warm");
}

static void show_key(const char *text)
{
    g_strlcpy(current_text, text, sizeof(current_text));

    gint64 now = g_get_monotonic_time();
    int reset_ms = env_int("KEYFLARE_RESET_MS", RESET_MS);
    int hot_ms   = env_int("KEYFLARE_HOT_MS",   HOT_MS);

    gint64 gap = last_key_us ? (now - last_key_us) / 1000 : 1000000;

    if (last_key_us == 0 || gap > reset_ms)
        streak_start_us = now;                       /* new typing streak */
    last_key_us = now;

    gint64 streak = (now - streak_start_us) / 1000;
    gboolean hot = streak >= hot_ms;
    double heat = hot ? 1.0 : (double)streak / hot_ms;

    /* faster typing -> harder recoil */
    double intensity = (double)(SPEED_SLOW_MS - gap) / (SPEED_SLOW_MS - SPEED_FAST_MS);
    intensity = CLAMP(intensity, 0.0, 1.0);

    if (g_getenv("KEYFLARE_DEBUG"))
        g_printerr("KEY %s hot=%d shot=%d\n", text, hot, (int)(intensity * 100));

    apply_heat(heat, hot);
    gunshot(intensity);
    wake_fade();
}

/* ---- input parsing ----------------------------------------------------- */

static void handle_line(const char *line)
{
    if (muted) return;                               /* password mode: show nothing */
    if (!strstr(line, "KEYBOARD_KEY")) return;
    const char *p = strstr(line, "KEY_");
    if (!p) return;

    gboolean pressed;
    if      (strstr(line, " pressed"))  pressed = TRUE;
    else if (strstr(line, " released")) pressed = FALSE;
    else return;

    p += 4;
    char tok[64]; size_t i = 0;
    while (*p && *p != ' ' && *p != '(' && i < sizeof(tok) - 1) tok[i++] = *p++;
    tok[i] = '\0';
    if (i == 0) return;

    int mb = mod_bit(tok);
    if (mb) {
        if (pressed) {
            held_mods |= mb;
            char mods[128];
            join_mods(mods, sizeof(mods), held_mods);
            show_key(mods);
        } else {
            held_mods &= ~mb;
        }
        return;
    }
    if (!pressed) return;                             /* ignore non-modifier releases */

    char key[64];
    nice_name(tok, key, sizeof(key));
    char disp[256];
    if (held_mods) {
        join_mods(disp, sizeof(disp), held_mods);
        g_strlcat(disp, " + ", sizeof(disp));
        g_strlcat(disp, key, sizeof(disp));
    } else {
        g_strlcpy(disp, key, sizeof(disp));
    }
    show_key(disp);
}

static gboolean on_libinput_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)data;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        render_label("stopped", 0.55);
        if (g_getenv("KEYFLARE_DEBUG")) g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    for (;;) {
        gchar *line = NULL; gsize len = 0;
        GIOStatus st = g_io_channel_read_line(ch, &line, &len, NULL, NULL);
        if (st == G_IO_STATUS_NORMAL && line) { handle_line(line); g_free(line); continue; }
        if (st == G_IO_STATUS_EOF) {
            g_free(line);
            if (g_getenv("KEYFLARE_DEBUG")) g_application_quit(g_application_get_default());
            return G_SOURCE_REMOVE;
        }
        g_free(line);
        break;
    }
    return G_SOURCE_CONTINUE;
}

static void start_listening(void)
{
    struct group *g = getgrnam("input");
    gboolean direct = g && (group_member(g->gr_gid) || geteuid() == 0);

    char *argv_direct[] = { "libinput", "debug-events", "--show-keycodes", NULL };
    char *argv_pkexec[] = { "pkexec", "libinput", "debug-events", "--show-keycodes", NULL };
    char **argv = direct ? argv_direct : argv_pkexec;

    gint out_fd = -1;
    GError *err = NULL;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL, NULL, NULL, &out_fd, NULL, &err)) {
        char *msg = g_markup_escape_text(err ? err->message : "spawn failed", -1);
        char *m = g_strdup_printf("<span font_family=\"monospace\" size=\"11000\">%s</span>", msg);
        gtk_label_set_markup(GTK_LABEL(label), m);
        g_free(m); g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    GIOChannel *ch = g_io_channel_unix_new(out_fd);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_libinput_io, NULL);
    g_io_channel_unref(ch);
}

/* ---- password-mode toggle (click) ------------------------------------- */

static gboolean on_click(GtkWidget *w, GdkEventButton *e, gpointer _)
{
    (void)w; (void)e; (void)_;
    muted = !muted;
    GtkStyleContext *ctx = gtk_widget_get_style_context(hud);
    if (muted) {
        if (idle_id) { g_source_remove(idle_id); idle_id = 0; }
        if (fade_id) { g_source_remove(fade_id); fade_id = 0; }
        hud_op = 1.0; gtk_widget_set_opacity(hud, 1.0);
        gtk_style_context_remove_class(ctx, "warm");
        gtk_style_context_remove_class(ctx, "hotter");
        gtk_style_context_remove_class(ctx, "hot");
        gtk_style_context_add_class(ctx, "muted");
        gtk_label_set_markup(GTK_LABEL(label),
            "<span font_family=\"monospace\" size=\"22000\">🔒</span>");
    } else {
        gtk_style_context_remove_class(ctx, "muted");
        last_key_us = 0; streak_start_us = 0; held_mods = 0;   /* reset heat */
        render_label(" ", 1.0);
        wake_fade();
    }
    return TRUE;
}

/* ---- styling, window, main -------------------------------------------- */

static const char *CSS =
    "window { background-color: transparent; }"
    "#ghost-hud {"
    "  background-image: linear-gradient(135deg, rgba(66,70,92,0.55), rgba(22,24,34,0.42));"
    "  border-radius: 18px; border: 1px solid rgba(255,255,255,0.14);"
    "  box-shadow: 0 8px 32px rgba(0,0,0,0.45), inset 0 1px 0 rgba(255,255,255,0.20);"
    "  padding: 12px 28px; min-width: 84px; min-height: 54px;"
    "  transition: background-image 220ms ease, border-color 220ms ease, box-shadow 220ms ease;"
    "}"
    "#ghost-hud.warm {"
    "  background-image: linear-gradient(135deg, rgba(200,150,60,0.55), rgba(80,54,20,0.45));"
    "  border-color: rgba(240,200,120,0.5);"
    "}"
    "#ghost-hud.hotter {"
    "  background-image: linear-gradient(135deg, rgba(230,120,40,0.6), rgba(120,50,12,0.5));"
    "  border-color: rgba(255,170,90,0.6);"
    "}"
    "#ghost-hud.hot {"
    "  background-image: linear-gradient(135deg, rgba(232,52,52,0.66), rgba(140,16,16,0.56));"
    "  border-color: rgba(255,140,140,0.7);"
    "  box-shadow: 0 8px 36px rgba(220,40,40,0.55), inset 0 1px 0 rgba(255,255,255,0.22);"
    "}"
    "#ghost-hud.muted {"
    "  background-image: linear-gradient(135deg, rgba(74,62,30,0.5), rgba(40,34,16,0.42));"
    "  border-color: rgba(240,200,120,0.5);"
    "}"
    "#click-area { background-color: transparent; }"
    ".key-label { color:#fff; font-weight:600; letter-spacing:1px; text-shadow:0 1px 4px rgba(0,0,0,0.55); }"
    /* muzzle flash on each shot */
    ".key-label.shot { color:#fff; text-shadow: 0 0 12px rgba(255,225,170,0.95), 0 1px 4px rgba(0,0,0,0.6); }";

static gboolean selftest_quit(gpointer app)
{
    GtkWidget *win = GTK_WIDGET(g_object_get_data(G_OBJECT(app), "win"));
    g_printerr("selftest: layer_window=%d mapped=%d\n",
        gtk_layer_is_layer_window(GTK_WINDOW(win)), gtk_widget_get_mapped(win));
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "keyflare");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_widget_set_can_focus(win, FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);

    if (gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "keyflare");
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, 24);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, 24);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), 0);
    } else {
        gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    }

    gtk_widget_set_app_paintable(win, TRUE);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (vis) gtk_widget_set_visual(win, vis);

    GtkWidget *click = gtk_event_box_new();
    gtk_widget_set_name(click, "click-area");
    gtk_widget_add_events(click, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(click, "button-press-event", G_CALLBACK(on_click), NULL);

    hud = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(hud, "ghost-hud");
    label = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "key-label");
    render_label(" ", 1.0);
    gtk_box_pack_start(GTK_BOX(hud), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(click), hud);
    gtk_container_add(GTK_CONTAINER(win), click);

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);

    gtk_widget_show_all(win);
    wake_fade();          /* show briefly at launch, then ghost out if idle */

    if (g_getenv("KEYFLARE_NO_LISTEN")) {
        g_object_set_data(G_OBJECT(app), "win", win);
        g_timeout_add(700, selftest_quit, app);
    } else {
        start_listening();
    }
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("local.keyflare.floating",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
