/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#ifdef HAVE_MALLINFO
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>

#include <clutter/clutter.h>
#include <clutter/x11/clutter-x11.h>
#include <dbus/dbus-glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <girepository.h>
#include <meta/main.h>
#include <meta/meta-plugin.h>
#include <meta/prefs.h>

#include "shell-a11y.h"
#include "shell-global.h"
#include "shell-perf-log.h"
#include "st.h"

extern GType gnome_shell_plugin_get_type (void);

#define SHELL_DBUS_SERVICE "org.gnome.Shell"
#define MAGNIFIER_DBUS_SERVICE "org.gnome.Magnifier"

static void
shell_dbus_init (void)
{
  GError *error = NULL;
  DBusGConnection *session;
  DBusGProxy *bus;
  guint32 request_name_result;

  /** TODO:
   * In the future we should use GDBus for this.  However, in
   * order to do that, we need to port all of the JavaScript
   * code.  Otherwise, the name will be claimed on the wrong
   * connection.
   */
  session = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);

  bus = dbus_g_proxy_new_for_name (session,
                                   DBUS_SERVICE_DBUS,
                                   DBUS_PATH_DBUS,
                                   DBUS_INTERFACE_DBUS);

  if (!dbus_g_proxy_call (bus, "RequestName", &error,
                          G_TYPE_STRING, SHELL_DBUS_SERVICE,
                          G_TYPE_UINT, 0,
                          G_TYPE_INVALID,
                          G_TYPE_UINT, &request_name_result,
                          G_TYPE_INVALID))
    {
      g_print ("failed to acquire org.gnome.Shell: %s\n", error->message);
      /* If we somehow got started again, it's not an error to be running
       * already.  So just exit 0.
       */
      exit (0);
    }

  /* Also grab org.gnome.Panel to replace any existing panel process,
   * unless a special environment variable is passed.  The environment
   * variable is used by the gnome-shell (no --replace) launcher in
   * Xephyr */
  if (!dbus_g_proxy_call (bus, "RequestName", &error, G_TYPE_STRING,
                          "org.gnome.Panel", G_TYPE_UINT,
                          DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                          G_TYPE_INVALID, G_TYPE_UINT,
                          &request_name_result, G_TYPE_INVALID))
    {
      g_print ("failed to acquire org.gnome.Panel: %s\n", error->message);
      exit (1);
    }

  /* ...and the org.gnome.Magnifier service.
   */
  if (!dbus_g_proxy_call (bus, "RequestName", &error,
                          G_TYPE_STRING, MAGNIFIER_DBUS_SERVICE,
                          G_TYPE_UINT, 0,
                          G_TYPE_INVALID,
                          G_TYPE_UINT, &request_name_result,
                          G_TYPE_INVALID))
    {
      g_print ("failed to acquire %s: %s\n", MAGNIFIER_DBUS_SERVICE, error->message);
      /* Failing to acquire the magnifer service is not fatal.  Log the error,
       * but keep going. */
    }

  /* ...and the org.freedesktop.Notifications service.
   */
  if (!dbus_g_proxy_call (bus, "RequestName", &error,
                          G_TYPE_STRING, "org.freedesktop.Notifications",
                          G_TYPE_UINT, DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                          G_TYPE_INVALID,
                          G_TYPE_UINT, &request_name_result,
                          G_TYPE_INVALID))
    {
      g_print ("failed to acquire org.freedesktop.Notifications: %s\n", error->message);
    }

  g_object_unref (bus);
}

static void
constrain_tooltip (StTooltip             *tooltip,
                   const ClutterGeometry *geometry,
                   ClutterGeometry       *adjusted_geometry,
                   gpointer               data)
{
  const ClutterGeometry *tip_area = st_tooltip_get_tip_area (tooltip);
  ShellGlobal *global = shell_global_get ();
  MetaScreen *screen = shell_global_get_screen (global);
  int n_monitors = meta_screen_get_n_monitors (screen);
  int i;

  *adjusted_geometry = *geometry;

  /* A point that determines what screen we'll constrain to */
  int x = tip_area->x + tip_area->width / 2;
  int y = tip_area->y + tip_area->height / 2;

  for (i = 0; i < n_monitors; i++)
    {
      MetaRectangle rect;
      meta_screen_get_monitor_geometry (screen, i, &rect);
      if (x >= rect.x && x < rect.x + rect.width &&
          y >= rect.y && y < rect.y + rect.height)
        {
          if (adjusted_geometry->x + adjusted_geometry->width > rect.x + rect.width)
            adjusted_geometry->x = rect.x + rect.width - adjusted_geometry->width;
          if (adjusted_geometry->x < rect.x)
            adjusted_geometry->x = rect.x;

          if (adjusted_geometry->y + adjusted_geometry->height > rect.y + rect.height)
            adjusted_geometry->y = rect.y + rect.height - adjusted_geometry->height;
          if (adjusted_geometry->y < rect.y)
            adjusted_geometry->y = rect.y;

          return;
        }
    }
}

static void
update_font_options (GtkSettings *settings)
{
  StThemeContext *context;
  ClutterStage *stage;
  ClutterBackend *backend;
  gint dpi;
  gint hinting;
  gchar *hint_style_str;
  cairo_hint_style_t hint_style = CAIRO_HINT_STYLE_NONE;
  gint antialias;
  cairo_antialias_t antialias_mode = CAIRO_ANTIALIAS_NONE;
  cairo_font_options_t *options;

  g_object_get (settings,
                "gtk-xft-dpi", &dpi,
                "gtk-xft-antialias", &antialias,
                "gtk-xft-hinting", &hinting,
                "gtk-xft-hintstyle", &hint_style_str,
                NULL);

  stage = CLUTTER_STAGE (clutter_stage_get_default ());
  context = st_theme_context_get_for_stage (stage);

  if (dpi != -1)
    /* GTK stores resolution as 1024 * dots/inch */
    st_theme_context_set_resolution (context, dpi / 1024);
  else
    st_theme_context_set_default_resolution (context);

  st_tooltip_set_constrain_func (stage, constrain_tooltip, NULL, NULL);

  /* Clutter (as of 0.9) passes comprehensively wrong font options
   * override whatever set_font_flags() did above.
   *
   * http://bugzilla.openedhand.com/show_bug.cgi?id=1456
   */
  backend = clutter_get_default_backend ();
  options = cairo_font_options_create ();

  cairo_font_options_set_hint_metrics (options, CAIRO_HINT_METRICS_ON);

  if (hinting >= 0 && !hinting)
    {
      hint_style = CAIRO_HINT_STYLE_NONE;
    }
  else if (hint_style_str)
    {
      if (strcmp (hint_style_str, "hintnone") == 0)
        hint_style = CAIRO_HINT_STYLE_NONE;
      else if (strcmp (hint_style_str, "hintslight") == 0)
        hint_style = CAIRO_HINT_STYLE_SLIGHT;
      else if (strcmp (hint_style_str, "hintmedium") == 0)
        hint_style = CAIRO_HINT_STYLE_MEDIUM;
      else if (strcmp (hint_style_str, "hintfull") == 0)
        hint_style = CAIRO_HINT_STYLE_FULL;
    }

  g_free (hint_style_str);

  cairo_font_options_set_hint_style (options, hint_style);

  /* We don't want to turn on subpixel anti-aliasing; since Clutter
   * doesn't currently have the code to support ARGB masks,
   * generating them then squashing them back to A8 is pointless.
   */
  antialias_mode = (antialias < 0 || antialias) ? CAIRO_ANTIALIAS_GRAY
                                                : CAIRO_ANTIALIAS_NONE;

  cairo_font_options_set_antialias (options, antialias_mode);

  clutter_backend_set_font_options (backend, options);
  cairo_font_options_destroy (options);
}

static void
settings_notify_cb (GtkSettings *settings,
                    GParamSpec  *pspec,
                    gpointer     data)
{
  update_font_options (settings);
}

static void
shell_fonts_init (void)
{
  GtkSettings *settings;

  /* Disable text mipmapping; it causes problems on pre-GEM Intel
   * drivers and we should just be rendering text at the right
   * size rather than scaling it. If we do effects where we dynamically
   * zoom labels, then we might want to reconsider.
   */
  clutter_set_font_flags (clutter_get_font_flags () & ~CLUTTER_FONT_MIPMAPPING);

  settings = gtk_settings_get_default ();
  g_object_connect (settings,
                    "signal::notify::gtk-xft-dpi",
                    G_CALLBACK (settings_notify_cb), NULL,
                    "signal::notify::gtk-xft-antialias",
                    G_CALLBACK (settings_notify_cb), NULL,
                    "signal::notify::gtk-xft-hinting",
                    G_CALLBACK (settings_notify_cb), NULL,
                    "signal::notify::gtk-xft-hintstyle",
                    G_CALLBACK (settings_notify_cb), NULL,
                    NULL);
  update_font_options (settings);
}

static void
shell_prefs_init (void)
{
  meta_prefs_override_preference_location ("/apps/mutter/general/attach_modal_dialogs",
                                           "/desktop/gnome/shell/windows/attach_modal_dialogs");
  meta_prefs_override_preference_location ("/apps/mutter/general/workspaces_only_on_primary",
                                           "/desktop/gnome/shell/windows/workspaces_only_on_primary");
  meta_prefs_override_preference_location ("/apps/metacity/general/button_layout",
                                           "/desktop/gnome/shell/windows/button_layout");
  meta_prefs_override_preference_location ("/apps/metacity/general/edge_tiling",
                                           "/desktop/gnome/shell/windows/edge_tiling");
  meta_prefs_override_preference_location ("/apps/metacity/general/theme",
                                           "/desktop/gnome/shell/windows/theme");
}

/* This is an IBus workaround. The flow of events with IBus is that every time
 * it gets gets a key event, it:
 *
 *  Sends it to the daemon via D-Bus asynchronously
 *  When it gets an reply, synthesizes a new GdkEvent and puts it into the
 *   GDK event queue with gdk_event_put(), including
 *   IBUS_FORWARD_MASK = 1 << 25 in the state to prevent a loop.
 *
 * (Normally, IBus uses the GTK+ key snooper mechanism to get the key
 * events early, but since our key events aren't visible to GTK+ key snoopers,
 * IBus will instead get the events via the standard
 * GtkIMContext.filter_keypress() mechanism.)
 *
 * There are a number of potential problems here; probably the worst
 * problem is that IBus doesn't forward the timestamp with the event
 * so that every key event that gets delivered ends up with
 * GDK_CURRENT_TIME.  This creates some very subtle bugs; for example
 * if you have IBus running and a keystroke is used to trigger
 * launching an application, focus stealing prevention won't work
 * right. http://code.google.com/p/ibus/issues/detail?id=1184
 *
 * In any case, our normal flow of key events is:
 *
 *  GDK filter function => clutter_x11_handle_event => clutter actor
 *
 * So, if we see a key event that gets delivered via the GDK event handler
 * function - then we know it must be one of these synthesized events, and
 * we should push it back to clutter.
 *
 * To summarize, the full key event flow with IBus is:
 *
 *   GDK filter function
 *     => Mutter
 *     => gnome_shell_plugin_xevent_filter()
 *     => clutter_x11_handle_event()
 *     => clutter event delivery to actor
 *     => gtk_im_context_filter_event()
 *     => sent to IBus daemon
 *     => response received from IBus daemon
 *     => gdk_event_put()
 *     => GDK event handler
 *     => <this function>
 *     => clutter_event_put()
 *     => clutter event delivery to actor
 *
 * Anything else we see here we just pass on to the normal GDK event handler
 * gtk_main_do_event().
 */
static void
gnome_shell_gdk_event_handler (GdkEvent *event_gdk,
                               gpointer  data)
{
  if (event_gdk->type == GDK_KEY_PRESS || event_gdk->type == GDK_KEY_RELEASE)
    {
      ClutterActor *stage;
      Window stage_xwindow;

      stage = clutter_stage_get_default ();
      stage_xwindow = clutter_x11_get_stage_window (CLUTTER_STAGE (stage));

      if (GDK_WINDOW_XID (event_gdk->key.window) == stage_xwindow)
        {
          ClutterDeviceManager *device_manager = clutter_device_manager_get_default ();
          ClutterInputDevice *keyboard = clutter_device_manager_get_core_device (device_manager,
                                                                                 CLUTTER_KEYBOARD_DEVICE);

          ClutterEvent *event_clutter = clutter_event_new ((event_gdk->type == GDK_KEY_PRESS) ?
                                                           CLUTTER_KEY_PRESS : CLUTTER_KEY_RELEASE);
          event_clutter->key.time = event_gdk->key.time;
          event_clutter->key.flags = CLUTTER_EVENT_NONE;
          event_clutter->key.stage = CLUTTER_STAGE (stage);
          event_clutter->key.source = NULL;

          /* This depends on ClutterModifierType and GdkModifierType being
           * identical, which they are currently. (They both match the X
           * modifier state in the low 16-bits and have the same extensions.) */
          event_clutter->key.modifier_state = event_gdk->key.state;

          event_clutter->key.keyval = event_gdk->key.keyval;
          event_clutter->key.hardware_keycode = event_gdk->key.hardware_keycode;
          event_clutter->key.unicode_value = gdk_keyval_to_unicode (event_clutter->key.keyval);
          event_clutter->key.device = keyboard;

          clutter_event_put (event_clutter);
          clutter_event_free (event_clutter);

          return;
        }
    }

  gtk_main_do_event (event_gdk);
}


static void
malloc_statistics_callback (ShellPerfLog *perf_log,
                            gpointer      data)
{
#ifdef HAVE_MALLINFO
  struct mallinfo info = mallinfo ();

  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.arenaSize",
                                     info.arena);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.mmapSize",
                                     info.hblkhd);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.usedSize",
                                     info.uordblks);
#endif
}

static void
shell_perf_log_init (void)
{
  ShellPerfLog *perf_log = shell_perf_log_get_default ();

  /* For probably historical reasons, mallinfo() defines the returned values,
   * even those in bytes as int, not size_t. We're determined not to use
   * more than 2G of malloc'ed memory, so are OK with that.
   */
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.arenaSize",
                                   "Amount of memory allocated by malloc() with brk(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.mmapSize",
                                   "Amount of memory allocated by malloc() with mmap(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.usedSize",
                                   "Amount of malloc'ed memory currently in use",
                                   "i");

  shell_perf_log_add_statistics_callback (perf_log,
                                          malloc_statistics_callback,
                                          NULL, NULL);
}

static void
muted_log_handler (const char     *log_domain,
                   GLogLevelFlags  log_level,
                   const char     *message,
                   gpointer        data)
{
  /* Intentionally empty to discard message */
}

static gboolean
print_version (const gchar    *option_name,
               const gchar    *value,
               gpointer        data,
               GError        **error)
{
  g_print ("GNOME Shell %s\n", VERSION);
  exit (0);
}

GOptionEntry gnome_shell_options[] = {
  {
    "version", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
    print_version,
    N_("Print version"),
    NULL
  },
  { NULL }
};

int
main (int argc, char **argv)
{
  GOptionContext *ctx;
  GError *error = NULL;

  g_type_init ();

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  ctx = meta_get_option_context ();
  g_option_context_add_main_entries (ctx, gnome_shell_options, GETTEXT_PACKAGE);
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s: %s\n", argv[0], error->message);
      exit (1);
    }

  meta_plugin_type_register (gnome_shell_plugin_get_type ());

  /* Prevent meta_init() from causing gtk to load gail and at-bridge */
  g_setenv ("NO_GAIL", "1", TRUE);
  g_setenv ("NO_AT_BRIDGE", "1", TRUE);
  meta_init ();
  g_unsetenv ("NO_GAIL");
  g_unsetenv ("NO_AT_BRIDGE");

  /* FIXME: Add gjs API to set this stuff and don't depend on the
   * environment.  These propagate to child processes.
   */
  g_setenv ("GJS_DEBUG_OUTPUT", "stderr", TRUE);
  g_setenv ("GJS_DEBUG_TOPICS", "JS ERROR;JS LOG", TRUE);

  shell_dbus_init ();
  shell_a11y_init ();
  shell_fonts_init ();
  shell_perf_log_init ();
  shell_prefs_init ();

  gdk_event_handler_set (gnome_shell_gdk_event_handler, NULL, NULL);

  g_irepository_prepend_search_path (GNOME_SHELL_PKGLIBDIR);
#if HAVE_BLUETOOTH
  g_irepository_prepend_search_path (BLUETOOTH_DIR);
#endif

  /* Disable debug spew from various libraries */
  g_log_set_handler ("Gvc", G_LOG_LEVEL_DEBUG,
                     muted_log_handler, NULL);
  g_log_set_handler ("GdmUser", G_LOG_LEVEL_DEBUG,
                     muted_log_handler, NULL);
  g_log_set_handler ("Bluetooth", G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_MESSAGE,
                     muted_log_handler, NULL);

  /* Initialize the global object */
  shell_global_get ();

  return meta_run ();
}
