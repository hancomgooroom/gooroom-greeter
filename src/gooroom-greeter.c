/*
 * Copyright (C) 2010 - 2011, Robert Ancell <robert.ancell@canonical.com>
 * Copyright (C) 2011, Gunnar Hjalmarsson <ubuntu@gunnar.cc>
 * Copyright (C) 2012 - 2013, Lionel Le Folgoc <mrpouit@ubuntu.com>
 * Copyright (C) 2012, Julien Lavergne <gilir@ubuntu.com>
 * Copyright (C) 2013 - 2015, Simon Steinbeiß <ochosi@shimmerproject.org>
 * Copyright (C) 2013 - 2018, Sean Davis <smd.seandavis@gmail.com>
 * Copyright (C) 2014, Andrew P. <pan.pav.7c5@gmail.com>
 * Copyright (C) 2015 - 2019 Gooroom <gooroom@gooroom.kr>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <glib-unix.h>

#include <locale.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <glib.h>
#include <gtk/gtkx.h>
#include <glib/gslist.h>
#include <signal.h>
#include <upower.h>

#include <pwd.h>
#include <grp.h>
#include <sys/types.h>

#include <libayatana-ido/libayatana-ido.h>
#include <libayatana-indicator/indicator-ng.h>
#include <libayatana-indicator/indicator-object.h>

#include <lightdm.h>

#include "indicator-button.h"
#include "greeterconfiguration.h"
#include "greeterbackground.h"


static LightDMGreeter *greeter;

/* Screen window */
static GtkOverlay   *screen_overlay;

/* Login window */
static GtkWidget    *login_win;
static GtkWidget    *login_win_title;
static GtkWidget    *login_win_logo_image, *login_win_login_button_icon;
static GtkLabel     *login_win_msg_label;
static GtkWidget    *login_win_username_entry, *login_win_pw_entry;
static GtkInfoBar   *login_win_infobar;
static GtkButton    *login_win_login_button;

/* Panel */
static GtkWidget    *panel_box;
static GtkWidget    *btn_shutdown, *btn_restart, *btn_suspend, *btn_hibernate;
static GtkWidget    *indicator_box;

/* Power window */
static GtkWidget    *cmd_win;
static GtkButton    *cmd_win_ok_button, *cmd_win_cancel_button;
static GtkLabel     *cmd_title_label, *cmd_msg_label;
static GtkImage     *cmd_icon_image;

static GtkWidget    *pw_set_win;
static GtkButton    *pw_set_win_ok_button, *pw_set_win_cancel_button;
static GtkLabel     *pw_set_win_title_label, *pw_set_win_prompt_label, *pw_set_win_msg_label;
static GtkEntry     *pw_set_win_prompt_entry;

static GtkWidget    *ask_win;
static GtkButton    *ask_win_ok_button, *ask_win_cancel_button;
static GtkLabel     *ask_win_msg_label, *ask_win_title_label;

static GtkWidget    *msg_win;
static GtkButton    *msg_win_ok_button;
static GtkLabel     *msg_win_msg_label, *msg_win_title_label;

static GtkWidget    *last_show_win;
static gboolean      changing_password;


/* Session */
static gchar *current_session;

/* Sesion language */
static gchar *current_language;

/* Screensaver values */
static int timeout, interval, prefer_blanking, allow_exposures;

/* Handling monitors backgrounds */
static GreeterBackground *greeter_background;

/* Authentication state */
static gboolean prompted = FALSE;
static gboolean prompt_active = FALSE, password_prompted = FALSE;

/* Pending questions */
static GSList *pending_questions = NULL;


static gchar *default_font_name,
             *default_theme_name,
             *default_icon_theme_name;

static const gchar *CMD_WIN_DATA = "cmd-win-data";
static const gchar *MSG_WIN_DATA = "msg-win-data";
static const gchar *ASK_WIN_DATA = "ask-win-data";

static UpClient *up_client = NULL;
static GPtrArray *devices = NULL;


enum {
    SYSTEM_SHUTDOWN,
    SYSTEM_RESTART,
    SYSTEM_SUSPEND,
    SYSTEM_HIBERNATE
};

typedef struct
{
    gboolean is_prompt;
    union
    {
        LightDMMessageType message;
        LightDMPromptType prompt;
    } type;
    gchar *text;
} PAMConversationMessage;

struct SavedFocusData
{
    GtkWidget *widget;
    gint editable_pos;
};



gpointer greeter_save_focus                    (GtkWidget* widget);
void     greeter_restore_focus                 (const gpointer saved_data);

void     login_win_pw_entry_activate_cb        (GtkWidget *widget);
void     pw_set_win_prompt_entry_activate_cb   (GtkWidget *widget);
void     pw_set_win_prompt_entry_changed_cb    (GtkEditable *editable,
                                                gchar       *new_text,
                                                gint         new_text_length,
                                                gpointer     position,
                                                gpointer     user_data);

void     cmd_win_button_clicked_cb             (GtkButton *button, gpointer user_data);
void     ask_win_button_clicked_cb             (GtkButton *button, gpointer user_data);
void     msg_win_button_clicked_cb             (GtkButton *button, gpointer user_data);
void     pw_set_win_button_clicked_cb          (GtkButton *button, gpointer user_data);
void     login_win_login_button_clicked_cb     (GtkButton *button, gpointer user_data);

gboolean login_win_pw_entry_key_press_cb       (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
gboolean login_win_username_entry_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
gboolean login_win_username_entry_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data);

gboolean cmd_win_key_press_event_cb            (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
gboolean ask_win_key_press_event_cb            (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
gboolean msg_win_key_press_event_cb            (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
gboolean pw_set_win_key_press_event_cb         (GtkWidget *widget, GdkEventKey *event, gpointer user_data);


static void process_prompts (LightDMGreeter *greeter);
static void start_authentication (const gchar *username);



/*
 * Translate @str according to the locale defined by LC_TIME; unlike
 * dcgettext(), the translation is still taken from the LC_MESSAGES
 * catalogue and not the LC_TIME one.
 */
static const gchar *
translate_time_format_string (const char *str)
{
	const char *locale = g_getenv ("LC_TIME");
	const char *res;
	char *sep;
	locale_t old_loc;
	locale_t loc = (locale_t)0;

	if (locale)
		loc = newlocale (LC_MESSAGES_MASK, locale, (locale_t)0);

	old_loc = uselocale (loc);

	sep = strchr (str, '\004');
	res = g_dpgettext (GETTEXT_PACKAGE, str, sep ? sep - str + 1 : 0);

	uselocale (old_loc);

	if (loc != (locale_t)0)
		freelocale (loc);

	return res;
}


/* Clock */
static gboolean
clock_timeout_thread (gpointer data)
{
	GtkLabel *clock_label = GTK_LABEL (data);

	GDateTime *dt = NULL;

	dt = g_date_time_new_now_local ();
	if (dt) {
		gchar *fm = g_date_time_format (dt, translate_time_format_string (N_("%B %-d %Y  %l:%M %p")));
		gchar *markup = g_markup_printf_escaped ("<b><span foreground=\"white\">%s</span></b>", fm);
		gtk_label_set_markup (GTK_LABEL (clock_label), markup);
		g_free (fm);
		g_free (markup);

		g_date_time_unref (dt);
	}

	return TRUE;
}

static void
on_indicator_button_toggled_cb (GtkToggleButton *button, gpointer user_data)
{
	if (!gtk_toggle_button_get_active (button))
		return;

	IndicatorObjectEntry *entry;

	XfceIndicatorButton *indic_button = XFCE_INDICATOR_BUTTON (button);

	entry = xfce_indicator_button_get_entry (indic_button);

	GList *l = NULL;
	GList *children = gtk_container_get_children (GTK_CONTAINER (entry->menu));
	for (l = children; l; l = l->next) {
		GtkWidget *item = GTK_WIDGET (l->data);
		if (item) {
			g_signal_emit_by_name (item, "activate");
			break;
		}
	}

	g_list_free (children);

	g_signal_handlers_block_by_func (button, on_indicator_button_toggled_cb, user_data);
	gtk_toggle_button_set_active (button, FALSE);
	g_signal_handlers_unblock_by_func (button, on_indicator_button_toggled_cb, user_data);
}

static gboolean
find_app_indicators (const gchar *name)
{
    gboolean found = FALSE;

    gchar **app_indicators = config_get_string_list (NULL, "app-indicators", NULL);

    if (app_indicators) {
        guint i;
        for (i = 0; app_indicators[i] != NULL; i++) {
            if (g_strcmp0 (name, app_indicators[i]) == 0) {
                found = TRUE;
                break;
            }
        }
        g_strfreev (app_indicators);
    }

    return found;
}

static void
entry_added (IndicatorObject *io, IndicatorObjectEntry *entry, gpointer user_data)
{
	g_return_if_fail (entry != NULL);

	if ((g_strcmp0 (entry->name_hint, "nm-applet") == 0) ||
        (find_app_indicators (entry->name_hint)))
	{
		GtkWidget *button;
		const gchar *io_name;

		io_name = g_object_get_data (G_OBJECT (io), "io-name");

		button = xfce_indicator_button_new (io, io_name, entry);
		gtk_box_pack_start (GTK_BOX (indicator_box), button, FALSE, FALSE, 0);

		if (entry->image != NULL)
			xfce_indicator_button_set_image (XFCE_INDICATOR_BUTTON (button), entry->image);

		if (entry->label != NULL)
			xfce_indicator_button_set_label (XFCE_INDICATOR_BUTTON (button), entry->label);

		if (g_strcmp0 (entry->name_hint, "gooroom-notice-applet") == 0) {
			g_signal_connect (G_OBJECT (button), "toggled", G_CALLBACK (on_indicator_button_toggled_cb), user_data);
		} else {
			if (entry->menu != NULL)
				xfce_indicator_button_set_menu (XFCE_INDICATOR_BUTTON (button), entry->menu);
		}

		gtk_widget_show (button);
	}
}

static void
entry_removed (IndicatorObject *io, IndicatorObjectEntry *entry, gpointer user_data)
{
	GList *children, *l = NULL;

	children = gtk_container_get_children (GTK_CONTAINER (indicator_box));
	for (l = children; l; l = l->next) {
		XfceIndicatorButton *child = XFCE_INDICATOR_BUTTON (l->data);
		if (child && (xfce_indicator_button_get_entry (child) == entry)) {
			xfce_indicator_button_destroy (child);
			break;
		}
	}

	g_list_free (children);
}

static gchar *
get_battery_icon_name (double percentage, UpDeviceState state)
{
    gchar *icon_name = NULL;
    const gchar *bat_state;

    switch (state)
    {
        case UP_DEVICE_STATE_CHARGING:
        case UP_DEVICE_STATE_PENDING_CHARGE:
            bat_state = "-charging";
            break;

        case UP_DEVICE_STATE_DISCHARGING:
        case UP_DEVICE_STATE_PENDING_DISCHARGE:
            bat_state = "";
            break;

        case UP_DEVICE_STATE_FULLY_CHARGED:
            bat_state = "-charged";
            break;

        case UP_DEVICE_STATE_EMPTY:
            return g_strdup ("battery-empty");

        default:
            bat_state = NULL;
            break;
    }
    if (!bat_state) {
        return g_strdup ("battery-error");
    }

    if (percentage >= 75) {
        icon_name = g_strdup_printf ("battery-full%s", bat_state);
    } else if (percentage >= 50 && percentage < 75) {
        icon_name = g_strdup_printf ("battery-good%s", bat_state);
    } else if (percentage >= 25 && percentage < 50) {
        icon_name = g_strdup_printf ("battery-medium%s", bat_state);
    } else if (percentage >= 10 && percentage < 25) {
        icon_name = g_strdup_printf ("battery-low%s", bat_state);
    } else {
        icon_name = g_strdup_printf ("battery-caution%s", bat_state);
    }

    return icon_name;
}

static void
on_power_device_changed_cb (UpDevice *device, GParamSpec *pspec, gpointer data)
{
    GtkImage *bat_tray = GTK_IMAGE (data);

    gchar *icon_name;
    gdouble percentage;
    UpDeviceState state;

    g_object_get (device,
                  "state", &state,
                  "percentage", &percentage,
                  NULL);

/* Sometimes the reported state is fully charged but battery is at 99%,
 * refusing to reach 100%. In these cases, just assume 100%.
 */
    if (state == UP_DEVICE_STATE_FULLY_CHARGED &&
        (100.0 - percentage <= 1.0))
      percentage = 100.0;

    icon_name = get_battery_icon_name (percentage, state);

    gtk_image_set_from_icon_name (bat_tray,
                                  icon_name,
                                  GTK_ICON_SIZE_BUTTON);

    gtk_image_set_pixel_size (bat_tray, 22);

    g_free (icon_name);
}

static void
updevice_added_cb (UpDevice *device)
{
    gboolean is_present = FALSE;
    guint device_type = UP_DEVICE_KIND_UNKNOWN;

    g_object_get (device, "kind", &device_type, NULL);
    g_object_get (device, "is-present", &is_present, NULL);

    if (device_type == UP_DEVICE_KIND_BATTERY && is_present) {
		GtkWidget *image = gtk_image_new_from_icon_name ("battery-full-symbolic", GTK_ICON_SIZE_BUTTON);
		gtk_box_pack_start (GTK_BOX (indicator_box), image, FALSE, FALSE, 0);
		gtk_widget_show (image);

		g_object_set_data (G_OBJECT (image), "updevice", device);

		on_power_device_changed_cb (device, NULL, image);
		g_signal_connect (device, "notify", G_CALLBACK (on_power_device_changed_cb), image);
    }
}

static void
on_up_client_device_added_cb (UpClient *upclient, UpDevice *device, gpointer data)
{
	updevice_added_cb (device);
}

static void
updevice_removed_cb (GtkWidget *widget, gpointer data)
{
    UpDevice *removed_device = (UpDevice *)data;

    UpDevice *device = (UpDevice*)g_object_get_data (G_OBJECT (widget), "updevice");

    if (device != removed_device)
        return;

    gtk_container_remove (GTK_CONTAINER (indicator_box), widget);

    gtk_widget_destroy (widget);
}

static void
on_up_client_device_removed_cb (UpClient *upclient, UpDevice *device, gpointer data)
{
    gtk_container_foreach (GTK_CONTAINER (indicator_box), updevice_removed_cb, device);
}

static void
hide_all_windows (void)
{
	gtk_widget_hide (ask_win);
	gtk_widget_hide (msg_win);
	gtk_widget_hide (cmd_win);
	gtk_widget_hide (login_win);
	gtk_widget_hide (pw_set_win);
}

static gboolean
grab_focus_cb (gpointer data)
{
	GtkWidget *focused = GTK_WIDGET (data);

	gtk_widget_grab_focus (focused);

    return FALSE;
}

static void
show_msg_window (const gchar *title, const gchar *msg, const gchar *ok, const gchar *data)
{
	gtk_label_set_label (msg_win_title_label, (title != NULL) ? title : "");
	gtk_label_set_markup (msg_win_msg_label, (msg != NULL) ? msg : "");
	gtk_button_set_label (msg_win_ok_button, (ok != NULL) ? ok : "");

	g_object_set_data (G_OBJECT (msg_win), MSG_WIN_DATA, (gpointer)data);

	hide_all_windows ();
	gtk_widget_show (msg_win);

	g_timeout_add (50, grab_focus_cb, msg_win_ok_button);

	last_show_win = msg_win;
}

static void
show_ask_window (const gchar *title, const gchar *msg, const gchar *ok, const gchar *cancel, const gchar *data)
{
	gtk_label_set_label (ask_win_title_label, (title != NULL) ? title : "");
	gtk_label_set_label (ask_win_msg_label, (msg != NULL) ? msg : "");
	gtk_button_set_label (ask_win_ok_button, (ok != NULL) ? ok : "");
	gtk_button_set_label (ask_win_cancel_button, (cancel != NULL) ? cancel : "");

	g_object_set_data (G_OBJECT (ask_win), ASK_WIN_DATA, (gpointer)data);

	hide_all_windows ();
	gtk_widget_show (ask_win);

	g_timeout_add (50, grab_focus_cb, ask_win_ok_button);

	last_show_win = ask_win;
}

static void
show_password_settings_window (void)
{
	gtk_entry_set_text (pw_set_win_prompt_entry, "");

	hide_all_windows ();
	gtk_widget_show (pw_set_win);

	g_timeout_add (50, grab_focus_cb, pw_set_win_prompt_entry);

	last_show_win = pw_set_win;
}

static void
show_cmd_window (const gchar* icon, const gchar* title, const gchar* message, int type)
{
    gchar *new_message = NULL;

    /* Check if there are still users logged in, count them and if so, display a warning */
    gint logged_in_users = 0;
    GList *items = lightdm_user_list_get_users (lightdm_user_list_get_instance ());
    GList *item;
    for (item = items; item; item = item->next)
    {
        LightDMUser *user = item->data;
        if (lightdm_user_get_logged_in (user))
            logged_in_users++;
    }

    if (logged_in_users > 0)
    {
        gchar *warning = g_strdup_printf (ngettext ("Warning: There is still %d user logged in.",
                                                    "Warning: There are still %d users logged in.",
                                                    logged_in_users), logged_in_users);

        new_message = g_markup_printf_escaped ("<b>%s</b>\n%s", warning, message);

        g_free (warning);
    } else {
        new_message = g_strdup (message);
    }

    gtk_button_set_label (cmd_win_ok_button, title);
    gtk_label_set_label (cmd_title_label, title);
    gtk_label_set_markup (cmd_msg_label, new_message);
    gtk_image_set_from_icon_name (cmd_icon_image, icon, GTK_ICON_SIZE_DIALOG);

    g_free (new_message);

    g_object_set_data (G_OBJECT (cmd_win), CMD_WIN_DATA, GINT_TO_POINTER (type));

    hide_all_windows ();
    gtk_widget_show (cmd_win);

	g_timeout_add (50, grab_focus_cb, cmd_win_cancel_button);
}

static void
on_command_button_clicked_cb (GtkButton *button, gpointer user_data)
{
	const char *img, *title, *msg;
	int type = GPOINTER_TO_INT (user_data);

	switch (type) {
		case SYSTEM_SHUTDOWN:
			img = "system-shutdown-symbolic";
			title = _("System Shutdown");
			msg = _("Are you sure you want to close all programs and shut down the computer?");
			break;

		case SYSTEM_RESTART:
			img = "system-restart-symbolic";
			title = _("System Restart");
			msg = _("Are you sure you want to close all programs and restart the computer?");
			break;

		case SYSTEM_SUSPEND:
			img = "system-suspend-symbolic";
			title = _("System Suspend");
			msg = _("Are you sure you want to suspend the computer?");
			break;

		case SYSTEM_HIBERNATE:
			img = "system-hibernate-symbolic";
			title = _("System Hibernate");
			msg = _("Are you sure you want to hibernate the computer?");
			break;

		default:
			return;
	}

	show_cmd_window (img, title, msg, type);
}

static void
dbus_update_activation_environment (void)
{
	gchar **argv = NULL;
	const gchar *cmd = "/usr/bin/dbus-update-activation-environment --systemd DBUS_SESSION_BUS_ADDRESS DISPLAY XAUTHORITY";
//	const gchar *cmd = "/usr/bin/dbus-update-activation-environment --systemd -all";

	g_shell_parse_argv (cmd, NULL, &argv, NULL);

	g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
}

static void
wm_start (void)
{
	gchar **argv = NULL, **envp = NULL;
	const gchar *cmd = "/usr/bin/metacity";

	GSettings *settings = g_settings_new ("org.gnome.desktop.wm.preferences");
	g_settings_set_enum (settings, "action-right-click-titlebar", 5);
	g_object_unref (settings);

	g_shell_parse_argv (cmd, NULL, &argv, NULL);

	envp = g_get_environ ();

	g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
	g_strfreev (envp);
}

static void
gf_start (void)
{
	gchar **argv = NULL, **envp = NULL;
	const gchar *cmd = "/usr/bin/gnome-flashback";

	GSettings *settings = g_settings_new ("org.gnome.gnome-flashback");
	g_settings_set_boolean (settings, "automount-manager", FALSE);
	g_settings_set_boolean (settings, "idle-monitor", FALSE);
	g_settings_set_boolean (settings, "polkit", FALSE);
	g_settings_set_boolean (settings, "shell", FALSE);
	g_settings_set_boolean (settings, "a11y-keyboard", FALSE);
	g_settings_set_boolean (settings, "audio-device-selection", FALSE);
	g_settings_set_boolean (settings, "bluetooth-applet", FALSE);
	g_settings_set_boolean (settings, "desktop-background", FALSE);
	g_settings_set_boolean (settings, "end-session-dialog", FALSE);
	g_settings_set_boolean (settings, "input-settings", FALSE);
	g_settings_set_boolean (settings, "input-sources", FALSE);
	g_settings_set_boolean (settings, "notifications", FALSE);
	g_settings_set_boolean (settings, "power-applet", FALSE);
	g_settings_set_boolean (settings, "screencast", FALSE);
	g_settings_set_boolean (settings, "screensaver", FALSE);
	g_settings_set_boolean (settings, "screenshot", FALSE);
	g_settings_set_boolean (settings, "sound-applet", FALSE);
	g_settings_set_boolean (settings, "status-notifier-watcher", FALSE);
	g_settings_set_boolean (settings, "workarounds", FALSE);
	g_object_unref (settings);

	g_shell_parse_argv (cmd, NULL, &argv, NULL);

	envp = g_get_environ ();

	g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
	g_strfreev (envp);
}

static void
notify_service_start (void)
{
	gchar **argv = NULL, **envp = NULL;
	const gchar *cmd = "/usr/bin/gsettings set apps.gooroom-notifyd notify-location 2";

	g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

	g_shell_parse_argv (GOOROOM_NOTIFYD, NULL, &argv, NULL);

	envp = g_get_environ ();

	g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
	g_strfreev (envp);
}

static void
indicator_application_service_start (void)
{
	gchar **argv = NULL, **envp = NULL;
	const gchar *cmd = "systemctl --user start ayatana-indicator-application";

	g_shell_parse_argv (cmd, NULL, &argv, NULL);

	envp = g_get_environ ();

	g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
}

static void
network_indicator_application_start (void)
{
	const gchar *cmd;
	gchar **argv = NULL, **envp = NULL;

	cmd = "/usr/bin/gsettings set org.gnome.nm-applet disable-connected-notifications true";
	g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

	cmd = "/usr/bin/gsettings set org.gnome.nm-applet disable-disconnected-notifications true";
	g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

	cmd = "/usr/bin/gsettings set org.gnome.nm-applet suppress-wireless-networks-available true";
	g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

	cmd = "nm-applet --indicator";
	g_shell_parse_argv (cmd, NULL, &argv, NULL);

	envp = g_get_environ ();

	g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

	g_strfreev (argv);
	g_strfreev (envp);
}

static void
other_indicator_application_start (void)
{
	gchar **envp = NULL;
	gchar **app_indicators = config_get_string_list (NULL, "app-indicators", NULL);

	if (!app_indicators)
		return;

	envp = g_get_environ ();

	guint i;
	for (i = 0; app_indicators[i] != NULL; i++) {
		gchar **argv = NULL;
		g_shell_parse_argv (app_indicators[i], NULL, &argv, NULL);
		g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
		g_strfreev (argv);
	}

	g_strfreev (envp);
	g_strfreev (app_indicators);
}

static void
load_clock_indicator (void)
{
	GtkWidget *clock_label = gtk_label_new ("");
	gtk_box_pack_start (GTK_BOX (indicator_box), clock_label, FALSE, FALSE, 0);
	gtk_widget_show_all (clock_label);

	/* update clock */
	clock_timeout_thread (clock_label);
	gdk_threads_add_timeout (1000, (GSourceFunc) clock_timeout_thread, clock_label);
}

static void
load_battery_indicator (void)
{
	guint i;

	up_client = up_client_new ();
	devices = up_client_get_devices2 (up_client);

	if (devices) {
		for ( i = 0; i < devices->len; i++) {
			UpDevice *device = g_ptr_array_index (devices, i);

			updevice_added_cb (device);
		}
	}

	g_signal_connect (up_client, "device-added", G_CALLBACK (on_up_client_device_added_cb), NULL);
	g_signal_connect (up_client, "device-removed", G_CALLBACK (on_up_client_device_removed_cb), NULL);
}

static void
load_module (const gchar *name)
{
	gchar                *path;
	IndicatorObject      *io;
	GList                *entries, *l = NULL;

	path = g_build_filename (INDICATOR_DIR, name, NULL);
	io = indicator_object_new_from_file (path);
	g_free (path);

	g_object_set_data (G_OBJECT (io), "io-name", g_strdup (name));

	g_signal_connect (G_OBJECT (io), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED, G_CALLBACK (entry_added), NULL);
	g_signal_connect (G_OBJECT (io), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED, G_CALLBACK (entry_removed), NULL);

	entries = indicator_object_get_entries (io);

	for (l = entries; l; l = l->next) {
		IndicatorObjectEntry *ioe = (IndicatorObjectEntry *)l->data;
		entry_added (io, ioe, NULL);
	}

	g_list_free (entries);
}


static void
load_application_indicator (void)
{
	/* load application indicator */
	if (g_file_test (INDICATOR_DIR, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
		GDir *dir = g_dir_open (INDICATOR_DIR, 0, NULL);

		const gchar *name;
		while ((name = g_dir_read_name (dir)) != NULL) {
			if (!name || !g_str_has_suffix (name, G_MODULE_SUFFIX))
				continue;

			if (!g_str_equal (name, "libayatana-application.so"))
				continue;

			load_module (name);
		}
		g_dir_close (dir);
	}
}

static void
load_indicators (void)
{
	load_clock_indicator ();
	load_battery_indicator ();
	load_application_indicator ();
}

static void
load_commands (void)
{
    gtk_widget_set_visible (btn_shutdown, lightdm_get_can_shutdown());
    gtk_widget_set_visible (btn_restart, lightdm_get_can_restart());
    gtk_widget_set_visible (btn_suspend, lightdm_get_can_suspend());
    gtk_widget_set_visible (btn_hibernate, lightdm_get_can_hibernate());

    g_signal_connect (G_OBJECT (btn_shutdown), "clicked", G_CALLBACK (on_command_button_clicked_cb), GINT_TO_POINTER (SYSTEM_SHUTDOWN));
    g_signal_connect (G_OBJECT (btn_restart), "clicked", G_CALLBACK (on_command_button_clicked_cb), GINT_TO_POINTER (SYSTEM_RESTART));
    g_signal_connect (G_OBJECT (btn_suspend), "clicked", G_CALLBACK (on_command_button_clicked_cb), GINT_TO_POINTER (SYSTEM_SUSPEND));
    g_signal_connect (G_OBJECT (btn_hibernate), "clicked", G_CALLBACK (on_command_button_clicked_cb), GINT_TO_POINTER (SYSTEM_HIBERNATE));
}

static void
read_monitor_configuration (const gchar *group, const gchar *name)
{
    g_debug ("[Configuration] Monitor configuration found: '%s'", name);

    gchar *background = config_get_string (group, CONFIG_KEY_BACKGROUND, NULL);
    greeter_background_set_monitor_config (greeter_background, name, background,
                                           config_get_bool (group, CONFIG_KEY_USER_BACKGROUND, -1),
                                           config_get_bool (group, CONFIG_KEY_LAPTOP, -1),
                                           config_get_int  (group, CONFIG_KEY_T_DURATION, -1),
                                           config_get_enum (group, CONFIG_KEY_T_TYPE,
                                                            TRANSITION_TYPE_FALLBACK,
                                                            "none",         TRANSITION_TYPE_NONE,
                                                            "linear",       TRANSITION_TYPE_LINEAR,
                                                            "ease-in-out",  TRANSITION_TYPE_EASE_IN_OUT, NULL));
    g_free (background);
}

gpointer
greeter_save_focus (GtkWidget* widget)
{
    GtkWidget *window = gtk_widget_get_toplevel(widget);
    if (!GTK_IS_WINDOW (window))
        return NULL;

    struct SavedFocusData *data = g_new0 (struct SavedFocusData, 1);
    data->widget = gtk_window_get_focus (GTK_WINDOW (window));
    data->editable_pos = GTK_IS_EDITABLE(data->widget) ? gtk_editable_get_position (GTK_EDITABLE (data->widget)) : -1;

    return data;
}

void
greeter_restore_focus (const gpointer saved_data)
{
    struct SavedFocusData *data = saved_data;

    if (!saved_data || !GTK_IS_WIDGET (data->widget))
        return;

    gtk_widget_grab_focus (data->widget);
    if (GTK_IS_EDITABLE(data->widget) && data->editable_pos > -1)
        gtk_editable_set_position(GTK_EDITABLE(data->widget), data->editable_pos);
}

static void
sigterm_cb (gpointer user_data)
{
    gboolean is_callback = GPOINTER_TO_INT (user_data);

    if (is_callback)
        g_debug ("SIGTERM received");

    if (is_callback)
    {
        gtk_main_quit ();
        #ifdef KILL_ON_SIGTERM
        /* LP: #1445461 */
        g_debug ("Killing greeter with exit()...");
        exit (EXIT_SUCCESS);
        #endif
    }
}

static void
set_message_label (LightDMMessageType type, const gchar *text)
{
    if (type == LIGHTDM_MESSAGE_TYPE_INFO)
        gtk_info_bar_set_message_type (login_win_infobar, GTK_MESSAGE_INFO);
    else
        gtk_info_bar_set_message_type (login_win_infobar, GTK_MESSAGE_ERROR);

    const gchar *str = (text != NULL) ? text : "";
    gtk_label_set_markup (login_win_msg_label, str);
}

static void
display_warning_message (LightDMMessageType type, const gchar *msg)
{
	set_message_label (type, msg);

	start_authentication (lightdm_greeter_get_authentication_user (greeter));
}

static void
show_login_window (GtkWidget *focus_widget)
{
    hide_all_windows ();
    gtk_widget_show_all (login_win);

    last_show_win = login_win;

    gtk_entry_set_text (GTK_ENTRY (login_win_pw_entry), "");
    gtk_widget_grab_focus (focus_widget);
    set_message_label (LIGHTDM_MESSAGE_TYPE_INFO, NULL);
}

/* Message label */
static gboolean
message_label_is_empty (void)
{
    return gtk_label_get_text (login_win_msg_label)[0] == '\0';
}

/* Session */
static gboolean
is_valid_session (GList* items, const gchar* session)
{
    for (; items; items = g_list_next (items))
        if (g_strcmp0 (session, lightdm_session_get_key (items->data)) == 0)
            return TRUE;
    return FALSE;
}

static gchar*
get_session (void)
{
    return g_strdup (current_session);
}

static void
set_session (const gchar *session)
{
    gchar *last_session = NULL;
    GList *sessions = lightdm_get_sessions ();

    /* Validation */
    if (!session || !is_valid_session (sessions, session))
    {
        /* previous session */
        last_session = config_get_string (STATE_SECTION_GREETER, STATE_KEY_LAST_SESSION, NULL);
        if (last_session && g_strcmp0 (session, last_session) != 0 &&
            is_valid_session (sessions, last_session))
            session = last_session;
        else
        {
            /* default */
            const gchar* default_session = lightdm_greeter_get_default_session_hint (greeter);
            if (g_strcmp0 (session, default_session) != 0 &&
                is_valid_session (sessions, default_session))
                session = default_session;
            /* first in the sessions list */
            else if (sessions)
                session = lightdm_session_get_key (sessions->data);
            /* give up */
            else
                session = NULL;
        }
    }

    g_free (current_session);
    current_session = g_strdup (session);
    g_free (last_session);
}

/* Session language */
static gchar*
get_language (void)
{
    /* if the user manually selected a language, use it */
    if (current_language)
        return g_strdup (current_language);

    return NULL;
}

static void
set_language (const gchar *language)
{
    const gchar *default_language = NULL;

    /* If failed to find this language, then try the default */
    if (lightdm_get_language ())
    {
        default_language = lightdm_language_get_code (lightdm_get_language ());
    }

    if (default_language && g_strcmp0 (default_language, language) != 0)
        set_language (default_language);
}

/* Pending questions */
static void
pam_message_finalize (PAMConversationMessage *message)
{
    g_free (message->text);
    g_free (message);
}

static void
process_prompts (LightDMGreeter *greeter)
{
	if (!pending_questions)
		return;

	/* always allow the user to change username again */
	gtk_widget_set_sensitive (login_win_username_entry, TRUE);
	gtk_widget_set_sensitive (login_win_pw_entry, TRUE);

	if (changing_password) {
		gtk_widget_set_sensitive (GTK_WIDGET (pw_set_win_prompt_entry), TRUE);
		gtk_widget_set_sensitive (GTK_WIDGET (pw_set_win_ok_button), TRUE);
	}

	/* Special case: no user selected from list, so PAM asks us for the user
	 * via a prompt. For that case, use the username field */
	if (!prompted && pending_questions && !pending_questions->next &&
        ((PAMConversationMessage *) pending_questions->data)->is_prompt &&
        ((PAMConversationMessage *) pending_questions->data)->type.prompt != LIGHTDM_PROMPT_TYPE_SECRET &&
			gtk_widget_get_visible (login_win_username_entry) &&
			lightdm_greeter_get_authentication_user (greeter) == NULL)
	{
		prompted = TRUE;
		prompt_active = TRUE;
		gtk_widget_grab_focus (login_win_username_entry);
		gtk_widget_show (login_win_pw_entry);
		return;
	}

	while (pending_questions)
	{
		PAMConversationMessage *message = (PAMConversationMessage *) pending_questions->data;
		pending_questions = g_slist_remove (pending_questions, (gconstpointer) message);

		const gchar *filter_msg_000 = "You are required to change your password immediately";
		const gchar *filter_msg_010 = g_dgettext("Linux-PAM", "You are required to change your password immediately (administrator enforced)");
		const gchar *filter_msg_020 = g_dgettext("Linux-PAM", "You are required to change your password immediately (password expired)");
		const gchar *filter_msg_030 = "Temporary Password";
		const gchar *filter_msg_040 = "Password Maxday Warning";
		const gchar *filter_msg_050 = "Account Expiration Warning";
		const gchar *filter_msg_051 = "Division Expiration Warning";
		const gchar *filter_msg_052 = "Password Expiration Warning";
		const gchar *filter_msg_060 = "Duplicate Login Notification";
		const gchar *filter_msg_070 = "Authentication Failure";
		const gchar *filter_msg_071 = "Deleted Account";
		const gchar *filter_msg_080 = "Account Locking";
		const gchar *filter_msg_090 = "Account Expiration";
		const gchar *filter_msg_100 = "Password Expiration";
		const gchar *filter_msg_110 = "Duplicate Login";
		const gchar *filter_msg_120 = "Division Expiration";
		const gchar *filter_msg_130 = "Login Trial Exceed";
		const gchar *filter_msg_140 = "Trial Period Expired";
		const gchar *filter_msg_150 = "DateTime Error";
		const gchar *filter_msg_160 = "Trial Period Warning";

		if ((strstr (message->text, filter_msg_000) != NULL) ||
		    (strstr (message->text, filter_msg_010) != NULL) ||
            (strstr (message->text, filter_msg_020) != NULL)) {
			changing_password = TRUE;
			show_ask_window (_("Password Expiration"),
                             _("Your password has expired.\n"
                               "Please change your password immediately."),
                             _("Changing Password"),
                             _("Cancel"),
                             "req_no_response");
			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_030)) {
			changing_password = TRUE;
			show_ask_window (_("Temporary Password Warning"),
                             _("Your password has been issued temporarily.\n"
                               "For security reasons, please change your password immediately."),
                             _("Changing Password"),
                             _("Cancel"),
                             "req_no_response");
			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_040)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 1) {
				if (g_str_equal (tokens[1], "1")) {
					msg = g_strdup_printf (_("Please change your password for security.\n"
                                             "If you do not change your password within %s day, "
                                             "your password expires.You can no longer log in.\n"
                                             "Do you want to change password now?"), tokens[1]);
				} else {
					msg = g_strdup_printf (_("Please change your password for security.\n"
                                             "If you do not change your password within %s days, "
                                             "your password expires.You can no longer log in.\n"
                                             "Do you want to change password now?"), tokens[1]);
				}
			} else {
				msg = g_strdup (_("Please change your password for security.\n"
                                  "If you do not change your password within a few days, "
                                  "your password expires.You can no longer log in.\n"
                                  "Do you want to change password now?"));
			}
			g_strfreev (tokens);

			show_ask_window (_("Password Maxday Warning"), msg,
					_("Change now"), _("Later"), "req_response");
			g_free (msg);

			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_050)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 2) {
				if (g_str_equal (tokens[1], "1")) {
					msg = g_strdup_printf (_("Your account will not be available after %s.\n"
                                             "Your account will expire in %s day"),
                                           tokens[1], tokens[2]);
				} else {
					msg = g_strdup_printf (_("Your account will not be available after %s.\n"
                                             "Your account will expire in %s days"),
                                           tokens[1], tokens[2]);
				}
			}
			g_strfreev (tokens);

			show_msg_window (_("Account Expiration Warning"), msg, _("Ok"), "ACCT_EXP_OK");
			g_free (msg);

			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_051)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 2) {
				if (g_str_equal (tokens[1], "1")) {
					msg = g_strdup_printf (_("Your organization will not be available after %s.\n"
                                             "Your organization will expire in %s day"),
                                           tokens[1], tokens[2]);
				} else {
					msg = g_strdup_printf (_("Your organization will not be available after %s.\n"
                                             "Your organization will expire in %s days"),
                                           tokens[1], tokens[2]);
				}
			}
			g_strfreev (tokens);

			show_msg_window (_("Division Expiration Warning"), msg, _("Ok"), "DEPT_EXP_OK");
			g_free (msg);

			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_052)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 2) {
				if (g_str_equal (tokens[1], "1")) {
					msg = g_strdup_printf (_("Your password will not be available after %s.\n"
                                             "Your password will expire in %s day"),
                                           tokens[1], tokens[2]);
				} else {
					msg = g_strdup_printf (_("Your password will not be available after %s.\n"
                                             "Your password will expire in %s days"),
                                           tokens[1], tokens[2]);
				}
			}
			g_strfreev (tokens);

			show_msg_window (_("Passowrd Expiration Warning"), msg, _("Ok"), "PASS_EXP_OK");
			g_free (msg);

			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_060)) {
			GString *msg = g_string_new (_("Duplicate logins detected with the same ID."));
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (tokens[1]) {
				gchar *markup = g_markup_printf_escaped ("<i>%s : %s</i>", _("Client ID"), tokens[1]);
				g_string_append_printf (msg, "\n\n%s", markup);
				g_free (markup);
			}
			if (tokens[2]) {
				gchar *markup = g_markup_printf_escaped ("<i>%s : %s</i>", _("Client Name"), tokens[2]);
				g_string_append_printf (msg, "\n%s", markup);
				g_free (markup);
			}
			if (tokens[3]) {
				gchar *markup = g_markup_printf_escaped ("<i>%s : %s</i>", _("IP"), tokens[3]);
				g_string_append_printf (msg, "\n%s", markup);
				g_free (markup);
			}
			if (tokens[4]) {
				gchar *markup = g_markup_printf_escaped ("<i>%s : %s</i>", _("Local IP"), tokens[4]);
				g_string_append_printf (msg, "\n%s", markup);
				g_free (markup);
			}

			show_msg_window (_("Duplicate Login Notification"), msg->str, _("Ok"), "DUPLICATE_LOGIN_OK");
			g_string_free (msg, TRUE);

			continue;
		} else if (g_str_has_prefix (message->text, filter_msg_070)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 1) {
				msg = g_strdup_printf (_("Authentication Failure\n\n"
                                         "You have %s login attempts remaining.\n"
                                         "You can no longer log in when the maximum number of login "
                                         "attempts is exceeded."), tokens[1]);
			} else {
				msg = g_strdup (_("The user could not be authenticated due to an unknown error.\n"
                                  "Please contact the administrator."));
			}
			g_strfreev (tokens);
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_071)) {
			gchar *msg = g_strdup (_("This account has deleted and is no longer available.\n"
                                     "Please contact the administrator."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_080)) {
			gchar *msg = g_strdup (_("Your account has been locked "
                                     "because you have exceeded the number of login attempts.\n"
                                     "Please try again in a moment."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_090)) {
			gchar *msg = g_strdup (_("This account has expired and is no longer available.\n"
                                     "Please contact the administrator."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_100)) {
			gchar *msg = g_strdup (_("The password for your account has expired.\n"
                                     "Please contact the administrator."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_110)) {
			gchar *msg = g_strdup (_("You are already logged in. "
                                     "Log out of the other device and try again. "
                                     "If the problem persists, please contact your administrator."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_120)) {
			gchar *msg = g_strdup (_("Due to the expiration of your organization, "
                                     "this account is no longer available.\n"
                                     "Please contact the administrator."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_130)) {
			gchar *msg = g_strdup (_("Login attempts exceeded the number of times, "
                                     "so you cannot login for a certain period of time.\n"
                                     "Please try again in a moment."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_140)) {
			gchar *msg = g_strdup (_("Trial period has expired."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_150)) {
			gchar *msg = g_strdup (_("Time error occurred."));
			display_warning_message (LIGHTDM_MESSAGE_TYPE_ERROR, msg);
			g_free (msg);
			break;
		} else if (g_str_has_prefix (message->text, filter_msg_160)) {
			gchar *msg = NULL;
			gchar **tokens = g_strsplit (message->text, ":", -1);
			if (g_strv_length (tokens) > 2) {
				if (g_str_equal (tokens[2], "0")) {
					msg = g_strdup_printf (_("The trial period is up to %s days.\n"
                                             "The trial period expires today."), tokens[1]);
				} else if (g_str_equal (tokens[2], "1")){
					msg = g_strdup_printf (_("The trial period is up to %s days.\n"
                                             "%s day left to expire."), tokens[1], tokens[2]);
				} else {
					msg = g_strdup_printf (_("The trial period is up to %s days.\n"
                                             "%s days left to expire."), tokens[1], tokens[2]);
				}
			} else {
				msg = g_strdup (_("The trial period is unknown."));
			}
			g_strfreev (tokens);

			show_msg_window (_("Trial Period Notification"), msg, _("Ok"), "TRIAL_LOGIN_OK");
			g_free (msg);
			continue;
		}

        if (!message->is_prompt)
        {
			/* FIXME: this doesn't show multiple messages, but that was
			 * already the case before. */
			if (changing_password) {
				gtk_label_set_text (pw_set_win_msg_label, message->text);
			} else {
				set_message_label (message->type.message, message->text);
			}
			continue;
        }

        if (changing_password) {
			const gchar *title;
			const gchar *prompt_label;

			/* for pam-gooroom and Linux-PAM, libpwquality */
			if ((strstr (message->text, "Current password: ") != NULL) ||
                (strstr (message->text, _("Current password: ")) != NULL)) {
				title = _("Changing Password - [Step 1]");
				prompt_label = _("Enter current password :");
			} else if ((strstr (message->text, "New password: ") != NULL) ||
                       (strstr (message->text, _("New password: ")) != NULL)) {
				title = _("Changing Password - [Step 2]");
				prompt_label = _("Enter new password :");
			} else if ((strstr (message->text, "Retype new password: ") != NULL) ||
                       (strstr (message->text, _("Retype new password: ")) != NULL)) {
				title = _("Changing Password - [Step 3]");
				prompt_label = _("Retype new password :");
			} else {
				title = NULL;
				prompt_label = NULL;
			}

			gtk_label_set_text (pw_set_win_title_label, (title != NULL) ? title : "");
			gtk_label_set_text (pw_set_win_prompt_label, (prompt_label != NULL) ? prompt_label : "");
			gtk_entry_set_text (pw_set_win_prompt_entry, "");
			gtk_widget_grab_focus (GTK_WIDGET (pw_set_win_prompt_entry));
		} else {
			gtk_widget_show (login_win_pw_entry);
			gtk_widget_grab_focus (login_win_pw_entry);
			gtk_entry_set_text (GTK_ENTRY (login_win_pw_entry), "");
#if 0
            if (message_label_is_empty () && password_prompted)
            {
                /* No message was provided beforehand and this is not the
                 * first password prompt, so use the prompt as label,
                 * otherwise the user will be completely unclear of what
                 * is going on. Actually, the fact that prompt messages are
                 * not shown is problematic in general, especially if
                 * somebody uses a custom PAM module that wants to ask
                 * something different. */
                gchar *str = message->text;
                if (g_str_has_suffix (str, ": "))
                    str = g_strndup (str, strlen (str) - 2);
                else if (g_str_has_suffix (str, ":"))
                    str = g_strndup (str, strlen (str) - 1);
                set_message_label (LIGHTDM_MESSAGE_TYPE_INFO, str);
                if (str != message->text)
                    g_free (str);
            }
#endif
        }

        prompted = TRUE;
        password_prompted = TRUE;
        prompt_active = TRUE;

        /* If we have more stuff after a prompt, assume that other prompts are pending,
         * so stop here. */
        break;
    }
}

static void
start_authentication (const gchar *username)
{
    prompted = FALSE;
    password_prompted = FALSE;
    prompt_active = FALSE;

    if (pending_questions)
    {
        g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
        pending_questions = NULL;
    }

    config_set_string (STATE_SECTION_GREETER, STATE_KEY_LAST_USER, username);

    if (g_strcmp0 (username, "*other") == 0)
    {
        gtk_widget_show (login_win_username_entry);
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
        lightdm_greeter_authenticate (greeter, NULL, NULL);
#else
        lightdm_greeter_authenticate (greeter, NULL);
#endif
    }
    else if (g_strcmp0 (username, "*guest") == 0)
    {
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
        lightdm_greeter_authenticate_as_guest (greeter, NULL);
#else
        lightdm_greeter_authenticate_as_guest (greeter);
#endif
    }
    else
    {
        LightDMUser *user;

        user = lightdm_user_list_get_user_by_name (lightdm_user_list_get_instance (), username);
        if (user)
        {
            if (!current_session)
                set_session (lightdm_user_get_session (user));
            if (!current_language)
                set_language (lightdm_user_get_language (user));
        }
        else
        {
            set_session (NULL);
            set_language (NULL);
        }
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
        lightdm_greeter_authenticate (greeter, username, NULL);
#else
        lightdm_greeter_authenticate (greeter, username);
#endif
    }
}

static void
start_session (void)
{
	gchar *language;
	gchar *session;

	language = get_language ();
	if (language)
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
		lightdm_greeter_set_language (greeter, language, NULL);
#else
	lightdm_greeter_set_language (greeter, language);
#endif
	g_free (language);

	session = get_session ();

	/* Remember last choice */
	config_set_string (STATE_SECTION_GREETER, STATE_KEY_LAST_SESSION, session);

//	greeter_background_save_xroot (greeter_background);

	if (!lightdm_greeter_start_session_sync (greeter, session, NULL))
	{
		set_message_label (LIGHTDM_MESSAGE_TYPE_ERROR, _("Failed to start session"));
		start_authentication (lightdm_greeter_get_authentication_user (greeter));
	}

	g_free (session);
}

gboolean
login_win_username_entry_focus_out_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	if (gtk_widget_get_visible (login_win))
		start_authentication (gtk_entry_get_text (GTK_ENTRY (login_win_username_entry)));

	return FALSE;
}

gboolean
login_win_username_entry_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    /* Acts as login_win_pw_entry */
    if (event->keyval == GDK_KEY_Up)
        return login_win_pw_entry_key_press_cb (widget, event, user_data);
    /* Enter activates the password entry */
    else if (event->keyval == GDK_KEY_Return && gtk_widget_get_visible (login_win_pw_entry))
    {
        gtk_widget_grab_focus (login_win_pw_entry);
        return TRUE;
    }
    else
        return FALSE;
}

void
login_win_login_button_clicked_cb (GtkButton *button, gpointer user_data)
{
    /* Reset to default screensaver values */
    if (lightdm_greeter_get_lock_hint (greeter))
        XSetScreenSaver (gdk_x11_display_get_xdisplay (gdk_display_get_default ()), timeout, interval, prefer_blanking, allow_exposures);

    gtk_widget_set_sensitive (login_win_username_entry, FALSE);
    gtk_widget_set_sensitive (login_win_pw_entry, FALSE);
    set_message_label (LIGHTDM_MESSAGE_TYPE_INFO, NULL);
    prompt_active = FALSE;

	if (lightdm_greeter_get_is_authenticated (greeter)) {
		start_session ();
	} else if (lightdm_greeter_get_in_authentication (greeter)) {
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
		lightdm_greeter_respond (greeter, gtk_entry_get_text (GTK_ENTRY (login_win_pw_entry)), NULL);
#else
		lightdm_greeter_respond (greeter, gtk_entry_get_text (GTK_ENTRY (login_win_pw_entry)));
#endif
		/* If we have questions pending, then we continue processing
		 * those, until we are done. (Otherwise, authentication will
		 * not complete.) */
		if (pending_questions)
			process_prompts (greeter);
	} else {
		start_authentication (lightdm_greeter_get_authentication_user (greeter));
	}
}

void
login_win_pw_entry_activate_cb (GtkWidget *widget)
{
    login_win_login_button_clicked_cb (login_win_login_button, NULL);
}

gboolean
login_win_pw_entry_key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if ((event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down))
    {
        /* Back to login_win_username_entry if it is available */
        if (event->keyval == GDK_KEY_Up &&
            gtk_widget_get_visible (login_win_username_entry) && widget == login_win_pw_entry)
        {
            gtk_widget_grab_focus (login_win_username_entry);
            return TRUE;
        }

        return TRUE;
    }
    return FALSE;
}

void
pw_set_win_prompt_entry_changed_cb (GtkEditable *editable,
                                    gchar       *new_text,
                                    gint         new_text_length,
                                    gpointer     position,
                                    gpointer     user_data)
{
    if (new_text_length > 0)
        gtk_label_set_text (pw_set_win_msg_label, "");
}

void
pw_set_win_button_clicked_cb (GtkButton *button, gpointer user_data)
{
    if (button == pw_set_win_cancel_button) {
        changing_password = FALSE;
        gtk_label_set_text (pw_set_win_msg_label, "");
        gtk_entry_set_text (pw_set_win_prompt_entry, "");
        show_login_window (login_win_pw_entry);
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
        return;
    }

    if (button == pw_set_win_ok_button) {
        gtk_widget_set_sensitive (GTK_WIDGET (pw_set_win_prompt_entry), FALSE);
        gtk_widget_set_sensitive (GTK_WIDGET (pw_set_win_ok_button), FALSE);
        prompt_active = FALSE;

        if (lightdm_greeter_get_in_authentication (greeter)) {
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
            lightdm_greeter_respond (greeter, gtk_entry_get_text (pw_set_win_prompt_entry), NULL);
#else
            lightdm_greeter_respond (greeter, gtk_entry_get_text (pw_set_win_prompt_entry));
#endif
            /* If we have questions pending, then we continue processing
             * those, until we are done. (Otherwise, authentication will
             * not complete.) */
            if (pending_questions)
                process_prompts (greeter);
        }
    }
}

void
pw_set_win_prompt_entry_activate_cb (GtkWidget *widget)
{
    pw_set_win_button_clicked_cb (pw_set_win_ok_button, NULL);
}

gboolean
pw_set_win_key_press_event_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape) {
        pw_set_win_button_clicked_cb (pw_set_win_cancel_button, NULL);
        return TRUE;
    }

    return FALSE;
}

void
cmd_win_button_clicked_cb (GtkButton *button, gpointer user_data)
{
	hide_all_windows ();
	gtk_widget_show_all (last_show_win);

	if (last_show_win == login_win) {
		gtk_widget_grab_focus (login_win_username_entry);
	} else if (last_show_win == msg_win) {
		gtk_widget_grab_focus (GTK_WIDGET (msg_win_ok_button));
	} else if (last_show_win == ask_win) {
		gtk_widget_grab_focus (GTK_WIDGET (ask_win_ok_button));
	} else if (last_show_win == pw_set_win) {
		gtk_widget_grab_focus (GTK_WIDGET (pw_set_win_prompt_entry));
	}

	if (button == cmd_win_cancel_button) {
		return;
	}

	int type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cmd_win), CMD_WIN_DATA));

	if (button == cmd_win_ok_button) {
		switch (type)
		{
			case SYSTEM_SHUTDOWN:
				lightdm_shutdown (NULL);
				break;

			case SYSTEM_RESTART:
				lightdm_restart (NULL);
				break;

			case SYSTEM_SUSPEND:
				lightdm_suspend (NULL);
				break;

			case SYSTEM_HIBERNATE:
				lightdm_hibernate (NULL);
				break;

			default:
				return;
		}
	}
}

gboolean
cmd_win_key_press_event_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape)
    {
        cmd_win_button_clicked_cb (cmd_win_cancel_button, NULL);
        return TRUE;
    }
    return FALSE;
}

void
ask_win_button_clicked_cb (GtkButton *button, gpointer user_data)
{
	const gchar *data = g_object_get_data (G_OBJECT (ask_win), ASK_WIN_DATA);

	if (button == ask_win_cancel_button) {
		if (g_strcmp0 (data, "req_response") == 0) {
			if (lightdm_greeter_get_in_authentication (greeter)) {
				changing_password = FALSE;
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
				lightdm_greeter_respond (greeter, "chpasswd_no", NULL);
#else
				lightdm_greeter_respond (greeter, "chpasswd_no");
#endif
				return;
			}
		}

		changing_password = FALSE;
		gtk_label_set_text (pw_set_win_msg_label, "");
		gtk_entry_set_text (pw_set_win_prompt_entry, "");
		show_login_window (login_win_pw_entry);
		start_authentication (lightdm_greeter_get_authentication_user (greeter));
    } else if (button == ask_win_ok_button) {
        if (g_strcmp0 (data, "req_response") == 0) {
            gtk_label_set_text (pw_set_win_msg_label, "");
            gtk_entry_set_text (pw_set_win_prompt_entry, "");

			changing_password = TRUE;

#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
			lightdm_greeter_respond (greeter, "chpasswd_yes", NULL);
#else
			lightdm_greeter_respond (greeter, "chpasswd_yes");
#endif
			show_password_settings_window ();
        } else {
			show_password_settings_window ();
		}
    }
}

gboolean
ask_win_key_press_event_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape) {
        ask_win_button_clicked_cb (ask_win_cancel_button, NULL);
        return TRUE;
    }

    return FALSE;
}

void
msg_win_button_clicked_cb (GtkButton *button, gpointer user_data)
{
	const gchar *response;
    const gchar *data = g_object_get_data (G_OBJECT (msg_win), MSG_WIN_DATA);

    if (g_str_equal (data, "CHPASSWD_FAILURE_OK")) {
		response = NULL;
        changing_password = FALSE;
        gtk_label_set_text (pw_set_win_msg_label, "");
        gtk_entry_set_text (pw_set_win_prompt_entry, "");
        show_login_window (login_win_pw_entry);
        start_authentication (lightdm_greeter_get_authentication_user (greeter));
    } else if (g_str_equal (data, "ACCT_EXP_OK")) {
		response = "acct_exp_ok";
    } else if (g_str_equal (data, "DEPT_EXP_OK")) {
		response = "dept_exp_ok";
    } else if (g_str_equal (data, "PASS_EXP_OK")) {
		response = "pass_exp_ok";
    } else if (g_str_equal (data, "DUPLICATE_LOGIN_OK")) {
		response = "duplicate_login_ok";
    } else if (g_str_equal (data, "TRIAL_LOGIN_OK")) {
		response = "trial_login_ok";
    } else {
		response = NULL;
	}

	if (response) {
		if (lightdm_greeter_get_in_authentication (greeter)) {
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
			lightdm_greeter_respond (greeter, response, NULL);
#else
			lightdm_greeter_respond (greeter, response);
#endif
		}
	}
}

gboolean
msg_win_key_press_event_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    if (event->keyval == GDK_KEY_Escape) {
        msg_win_button_clicked_cb (msg_win_ok_button, NULL);
        return TRUE;
    }

    return FALSE;
}

static void
show_prompt_cb (LightDMGreeter *greeter, const gchar *text, LightDMPromptType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = TRUE;
        message_obj->type.prompt = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
show_message_cb (LightDMGreeter *greeter, const gchar *text, LightDMMessageType type)
{
    PAMConversationMessage *message_obj = g_new (PAMConversationMessage, 1);
    if (message_obj)
    {
        message_obj->is_prompt = FALSE;
        message_obj->type.message = type;
        message_obj->text = g_strdup (text);
        pending_questions = g_slist_append (pending_questions, message_obj);
    }

    if (!prompt_active)
        process_prompts (greeter);
}

static void
timed_autologin_cb (LightDMGreeter *greeter)
{
    /* Don't trigger autologin if user locks screen with light-locker (thanks to Andrew P.). */
    if (!lightdm_greeter_get_lock_hint (greeter))
    {
        if (lightdm_greeter_get_is_authenticated (greeter))
        {
            /* Configured autologin user may be already selected in user list. */
            if (lightdm_greeter_get_authentication_user (greeter))
                /* Selected user matches configured autologin-user option. */
                start_session ();
            else if (lightdm_greeter_get_autologin_guest_hint (greeter))
                /* "Guest session" is selected and autologin-guest is enabled. */
                start_session ();
            else if (lightdm_greeter_get_autologin_user_hint (greeter))
            {
                /* "Guest session" is selected, but autologin-user is configured. */
                start_authentication (lightdm_greeter_get_autologin_user_hint (greeter));
                prompted = TRUE;
            }
        }
        else
#ifdef HAVE_LIBLIGHTDMGOBJECT_1_19_2
            lightdm_greeter_authenticate_autologin (greeter, NULL);
#else
            lightdm_greeter_authenticate_autologin (greeter);
#endif
    }
}

static void
authentication_complete_cb (LightDMGreeter *greeter)
{
	prompt_active = FALSE;
	gtk_entry_set_text (GTK_ENTRY (login_win_pw_entry), "");

	if (pending_questions)
	{
		g_slist_free_full (pending_questions, (GDestroyNotify) pam_message_finalize);
		pending_questions = NULL;
	}

	if (lightdm_greeter_get_is_authenticated (greeter))
	{
		if (prompted)
			start_session ();
		else
		{
			gtk_widget_hide (login_win_pw_entry);
		}
	}
	else
	{
		/* If an error message is already printed we do not print it this statement
		 * The error message probably comes from the PAM module that has a better knowledge
		 * of the failure. */
		gboolean have_pam_error = !message_label_is_empty () &&
			gtk_info_bar_get_message_type (login_win_infobar) != GTK_MESSAGE_ERROR;
		if (prompted)
		{
			if (!have_pam_error) {
				set_message_label (LIGHTDM_MESSAGE_TYPE_ERROR,
                                   _("Authentication Failure\n"
                                     "Please check the username and password and try again."));
			}
			start_authentication (lightdm_greeter_get_authentication_user (greeter));
		}
		else
		{
			g_warning ("Failed to authenticate");
			if (!have_pam_error)
				set_message_label (LIGHTDM_MESSAGE_TYPE_ERROR, _("Failed to authenticate"));
		}

		if (changing_password) {
			show_msg_window (_("Failure Of Changing Password"),
                             _("Failed to change password.\nPlease try again."),
                             _("Ok"),
                             "CHPASSWD_FAILURE_OK");
		}
	}
}

static void
apply_gtk_config (void)
{
	GError *error = NULL;
	GKeyFile *keyfile = NULL;
	gchar *gtk_settings_ini = NULL;
	gchar *gtk_settings_dir = NULL;

	gtk_settings_dir = g_build_filename (g_get_user_config_dir (), "gtk-3.0", NULL);
	if (g_mkdir_with_parents (gtk_settings_dir, 0775) < 0) {
		g_warning ("Failed to create directory %s", gtk_settings_dir);
		g_free (gtk_settings_dir);
		return;
	}

	gtk_settings_ini = g_build_filename (gtk_settings_dir, "settings.ini", NULL);

	keyfile = g_key_file_new ();

	if (error == NULL)
	{
		gchar *value;

		/* Set GTK+ settings */
		value = config_get_string (NULL, CONFIG_KEY_THEME, NULL);
		if (value)
		{
			g_key_file_set_string (keyfile, "Settings", "gtk-theme-name", value);
			g_free (value);
		}

		value = config_get_string (NULL, CONFIG_KEY_ICON_THEME, NULL);
		if (value)
		{
			g_key_file_set_string (keyfile, "Settings", "gtk-icon-theme-name", value);
			g_free (value);
		}

		value = config_get_string (NULL, CONFIG_KEY_FONT, "Sans 10");
		if (value)
		{
			g_key_file_set_string (keyfile, "Settings", "gtk-font-name", value);
			g_free (value);
		}

		if (config_has_key (NULL, CONFIG_KEY_DPI))
		{
			gint dpi = 1024 * config_get_int (NULL, CONFIG_KEY_DPI, 96);
			g_key_file_set_integer (keyfile, "Settings", "gtk-xft-dpi", dpi);
		}

		if (config_has_key (NULL, CONFIG_KEY_ANTIALIAS))
		{
			gboolean antialias = config_get_bool (NULL, CONFIG_KEY_ANTIALIAS, FALSE);
			g_key_file_set_boolean (keyfile, "Settings", "gtk-xft-antialias", antialias);
		}

		value = config_get_string (NULL, CONFIG_KEY_HINT_STYLE, NULL);
		if (value)
		{
			g_key_file_set_string (keyfile, "Settings", "gtk-xft-hintstyle", value);
			g_free (value);
		}

		value = config_get_string (NULL, CONFIG_KEY_RGBA, NULL);
		if (value)
		{
			g_key_file_set_string (keyfile, "Settings", "gtk-xft-rgba", value);
			g_free (value);
		}
		g_key_file_save_to_file (keyfile, gtk_settings_ini, NULL);
	} else {
		g_error_free (error);
	}

	g_free (gtk_settings_dir);
	g_free (gtk_settings_ini);
	g_key_file_free (keyfile);
}

int
main (int argc, char **argv)
{
	GtkBuilder *builder;
	const GList *items, *item;
	int ret = EXIT_SUCCESS;

	/* LP: #1024482 */
	g_setenv ("GDK_CORE_DEVICE_EVENTS", "1", TRUE);
	g_setenv ("GTK_MODULES", "atk-bridge", FALSE);

	/* Make nm-applet hide items the user does not have permissions to interact with */
	g_setenv ("NM_APPLET_HIDE_POLICY_ITEMS", "1", TRUE);

	dbus_update_activation_environment ();

	g_unix_signal_add (SIGTERM, (GSourceFunc)sigterm_cb, /* is_callback */ GINT_TO_POINTER (TRUE));

	/* Initialize i18n */
	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* init gtk */
	gtk_init (&argc, &argv);

	config_init ();
	apply_gtk_config ();

	/* Starting window manager */
	wm_start ();

	/* Starting gnome-flashback */
	gf_start ();

	greeter = lightdm_greeter_new ();
	g_signal_connect (greeter, "show-prompt", G_CALLBACK (show_prompt_cb), NULL);
	g_signal_connect (greeter, "show-message", G_CALLBACK (show_message_cb), NULL);
	g_signal_connect (greeter, "authentication-complete", G_CALLBACK (authentication_complete_cb), NULL);
	g_signal_connect (greeter, "autologin-timer-expired", G_CALLBACK (timed_autologin_cb), NULL);
	if (!lightdm_greeter_connect_sync (greeter, NULL)) {
		ret = EXIT_FAILURE;
		goto done;
	}

	/* Set default cursor */
	gdk_window_set_cursor (gdk_get_default_root_window (),
                           gdk_cursor_new_for_display (gdk_display_get_default (),
                           GDK_LEFT_PTR));

	/* Make the greeter behave a bit more like a screensaver if used as un/lock-screen by blanking the screen */
	if (lightdm_greeter_get_lock_hint (greeter))
	{
		Display *display = gdk_x11_display_get_xdisplay (gdk_display_get_default ());
		XGetScreenSaver (display, &timeout, &interval, &prefer_blanking, &allow_exposures);
		XForceScreenSaver (display, ScreenSaverActive);
		XSetScreenSaver (display, config_get_int (NULL, CONFIG_KEY_SCREENSAVER_TIMEOUT, 60), 0,
                         ScreenSaverActive, DefaultExposures);
	}

	builder = gtk_builder_new_from_resource ("/kr/gooroom/greeter/gooroom-greeter.ui");

	/* Screen window */
	screen_overlay = GTK_OVERLAY (gtk_builder_get_object (builder, "screen_overlay"));

	/* Login Dialog */
	login_win = GTK_WIDGET (gtk_builder_get_object (builder, "login_win"));
	login_win_title = GTK_WIDGET (gtk_builder_get_object (builder, "login_win_title"));
	login_win_logo_image = GTK_WIDGET (gtk_builder_get_object (builder, "login_win_logo_image"));
	login_win_login_button_icon = GTK_WIDGET (gtk_builder_get_object (builder, "login_win_login_button_icon"));
	login_win_username_entry = GTK_WIDGET (gtk_builder_get_object (builder, "login_win_username_entry"));
	login_win_pw_entry = GTK_WIDGET (gtk_builder_get_object (builder, "login_win_pw_entry"));
	login_win_infobar = GTK_INFO_BAR (gtk_builder_get_object (builder, "login_win_infobar"));
	login_win_msg_label = GTK_LABEL (gtk_builder_get_object (builder, "login_win_msg_label"));
	login_win_login_button = GTK_BUTTON (gtk_builder_get_object (builder, "login_win_login_button"));

	/* Bottom panel */
	panel_box = GTK_WIDGET (gtk_builder_get_object (builder, "panel_box"));

	btn_shutdown = GTK_WIDGET (gtk_builder_get_object (builder, "btn_shutdown"));
	btn_restart = GTK_WIDGET (gtk_builder_get_object (builder, "btn_restart"));
	btn_suspend = GTK_WIDGET (gtk_builder_get_object (builder, "btn_suspend"));
	btn_hibernate = GTK_WIDGET (gtk_builder_get_object (builder, "btn_hibernate"));

	/* indicator box in panel */
	indicator_box = GTK_WIDGET (gtk_builder_get_object (builder, "indicator_box"));

	/* Power dialog */
	cmd_win = GTK_WIDGET (gtk_builder_get_object (builder, "cmd_win"));
	cmd_win_ok_button = GTK_BUTTON (gtk_builder_get_object (builder, "cmd_win_ok_button"));
	cmd_win_cancel_button = GTK_BUTTON (gtk_builder_get_object (builder, "cmd_win_cancel_button"));
	cmd_title_label = GTK_LABEL (gtk_builder_get_object (builder, "cmd_title_label"));
	cmd_msg_label = GTK_LABEL (gtk_builder_get_object (builder, "cmd_msg_label"));
	cmd_icon_image = GTK_IMAGE (gtk_builder_get_object (builder, "cmd_icon_image"));

	/* Password Settings Dialog */
	pw_set_win = GTK_WIDGET (gtk_builder_get_object (builder, "pw_set_win"));
	pw_set_win_ok_button = GTK_BUTTON (gtk_builder_get_object (builder, "pw_set_win_ok_button"));
	pw_set_win_cancel_button = GTK_BUTTON (gtk_builder_get_object (builder, "pw_set_win_cancel_button"));
	pw_set_win_title_label = GTK_LABEL (gtk_builder_get_object (builder, "pw_set_win_title_label"));
	pw_set_win_msg_label = GTK_LABEL (gtk_builder_get_object (builder, "pw_set_win_msg_label"));
	pw_set_win_prompt_label = GTK_LABEL (gtk_builder_get_object (builder, "pw_set_win_prompt_label"));
	pw_set_win_prompt_entry = GTK_ENTRY (gtk_builder_get_object (builder, "pw_set_win_prompt_entry"));

	/* Question Dialog */
	ask_win = GTK_WIDGET (gtk_builder_get_object (builder, "ask_win"));
	ask_win_ok_button = GTK_BUTTON (gtk_builder_get_object (builder, "ask_win_ok_button"));
	ask_win_cancel_button = GTK_BUTTON (gtk_builder_get_object (builder, "ask_win_cancel_button"));
	ask_win_title_label = GTK_LABEL (gtk_builder_get_object (builder, "ask_win_title_label"));
	ask_win_msg_label = GTK_LABEL (gtk_builder_get_object (builder, "ask_win_msg_label"));

	msg_win = GTK_WIDGET (gtk_builder_get_object (builder, "msg_win"));
	msg_win_ok_button = GTK_BUTTON (gtk_builder_get_object (builder, "msg_win_ok_button"));
	msg_win_title_label = GTK_LABEL (gtk_builder_get_object (builder, "msg_win_title_label"));
	msg_win_msg_label = GTK_LABEL (gtk_builder_get_object (builder, "msg_win_msg_label"));

	gtk_overlay_add_overlay (screen_overlay, login_win);
	gtk_overlay_add_overlay (screen_overlay, cmd_win);
	gtk_overlay_add_overlay (screen_overlay, pw_set_win);
	gtk_overlay_add_overlay (screen_overlay, ask_win);
	gtk_overlay_add_overlay (screen_overlay, msg_win);
	gtk_overlay_add_overlay (screen_overlay, panel_box);

	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_resource (provider, "/kr/gooroom/greeter/theme.css");
	gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                               GTK_STYLE_PROVIDER (provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (provider);

	GdkPixbuf *pixbuf = NULL;
	pixbuf = gdk_pixbuf_new_from_resource ("/kr/gooroom/greeter/username.png", NULL);
	if (pixbuf) {
		gtk_entry_set_icon_from_pixbuf (GTK_ENTRY (login_win_username_entry), GTK_ENTRY_ICON_PRIMARY, pixbuf);
		g_object_unref (pixbuf);
	}
	pixbuf = gdk_pixbuf_new_from_resource ("/kr/gooroom/greeter/password.png", NULL);
	if (pixbuf) {
		gtk_entry_set_icon_from_pixbuf (GTK_ENTRY (login_win_pw_entry), GTK_ENTRY_ICON_PRIMARY, pixbuf);
		g_object_unref (pixbuf);
	}
	pixbuf = gdk_pixbuf_new_from_resource_at_scale ("/kr/gooroom/greeter/logo-letter-white.svg", -1, 35, TRUE, NULL);
	if (pixbuf) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (login_win_logo_image), pixbuf);
		g_object_unref (pixbuf);
	}
	gtk_image_set_from_resource (GTK_IMAGE (login_win_login_button_icon), "/kr/gooroom/greeter/arrow.png");

	/* Background */
	greeter_background = greeter_background_new (GTK_WIDGET (screen_overlay));

	read_monitor_configuration (CONFIG_GROUP_DEFAULT, GREETER_BACKGROUND_DEFAULT);

	gchar **config_group;
	gchar **config_groups = config_get_groups (CONFIG_GROUP_MONITOR);
	for (config_group = config_groups; *config_group; ++config_group)
	{
		const gchar *name = *config_group + sizeof (CONFIG_GROUP_MONITOR);
		while (*name && g_ascii_isspace (*name))
			++name;

		read_monitor_configuration (*config_group, name);
	}
	g_strfreev (config_groups);

	greeter_background_connect (greeter_background, gdk_screen_get_default ());

	/* set default session */
	set_session (lightdm_greeter_get_default_session_hint (greeter));

	const gchar *user_name;
	gboolean logged_in = FALSE;

	items = lightdm_user_list_get_users (lightdm_user_list_get_instance ());
	for (item = items; item; item = item->next)
	{
		LightDMUser *user = item->data;
		logged_in = lightdm_user_get_logged_in (user);
		if (logged_in) {
			user_name = lightdm_user_get_name (user);
			break;
		}
	}

	if (logged_in)
	{
		gtk_widget_hide (login_win_username_entry);
		start_authentication (user_name);
	}
	else
	{
		gtk_widget_show (login_win_username_entry);
		start_authentication ("*other");
	}

	gtk_widget_set_halign (login_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (login_win, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (cmd_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (cmd_win, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (pw_set_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (pw_set_win, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (ask_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (ask_win, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (msg_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (msg_win, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (panel_box, GTK_ALIGN_END);

	gtk_builder_connect_signals (builder, greeter);

	set_message_label (LIGHTDM_MESSAGE_TYPE_INFO, NULL);

	load_commands ();
	load_indicators ();

	/* Start the indicator applications service */
	indicator_application_service_start ();
	network_indicator_application_start ();
	other_indicator_application_start ();
	notify_service_start ();

	changing_password = FALSE;

	gtk_widget_show (GTK_WIDGET (screen_overlay));
	show_login_window (login_win_username_entry);

	g_debug ("Run Gtk loop...");
	gtk_main ();
	g_debug ("Gtk loop exits");

	g_free (default_font_name);
	g_free (default_theme_name);
	g_free (default_icon_theme_name);

	if (devices) {
		g_ptr_array_foreach (devices, (GFunc) g_object_unref, NULL);
		g_clear_pointer (&devices, g_ptr_array_unref);
	}

	if (up_client)
		g_clear_object (&up_client);

	sigterm_cb (GINT_TO_POINTER (FALSE));

done:
    return ret;
}
