/*
 * Copyright (C) 2014 - 2018, Sean Davis <smd.seandavis@gmail.com>
 * Copyright (C) 2014, Andrew P. <pan.pav.7c5@gmail.com>
 * Copyright (C) 2015, Robert Ancell <robert.ancell@canonical.com>
 * Copyright (C) 2015, Simon Steinbeiß <ochosi@shimmerproject.org>
 * Copyright (C) 2015 - 2021 Gooroom <gooroom@gooroom.kr>
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

#include <math.h>
#include <cairo-xlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <X11/Xatom.h>
#include <glib/gi18n.h>

#include "greeterbackground.h"

typedef enum
{
	/* Broken/uninitialized configuration */
	BACKGROUND_TYPE_INVALID,
	/* Solid color */
	BACKGROUND_TYPE_COLOR,
	/* Path to image and scaling mode */
	BACKGROUND_TYPE_IMAGE
} BackgroundType;

typedef enum
{
    /* It is not really useful, used for debugging */
	SCALING_MODE_SOURCE,
    /* Default mode for values without mode prefix */
	SCALING_MODE_ZOOMED,
	SCALING_MODE_SCALED,
	SCALING_MODE_STRETCHED
} ScalingMode;


/* Background configuration (parsed from background=... option).
   Used to fill <Background> */
typedef struct
{
    BackgroundType type;
    union
    {
        GdkRGBA color;
        struct
        {
            gchar *path;
            ScalingMode mode;
        } image;
    } options;
} BackgroundConfig;

/* Store monitor configuration */
typedef struct
{
	BackgroundConfig bg;
} MonitorConfig;

/* Actual drawing information attached to monitor.
 * Used to separate configured monitor background and user background. */
typedef struct
{
	gint ref_count;
	BackgroundType type;
	union
	{
		GdkPixbuf* image;
		GdkRGBA color;
	} options;
} Background;

typedef struct
{
	GreeterBackground* object;
	gint number;
	gchar* name;
	GdkRectangle geometry;
	GtkWindow* window;
	gulong window_draw_handler_id;

	Background* background;

} Monitor;

static const gchar* SCALING_MODE_PREFIXES[] = {
	"#source:", "#zoomed:", "#scaled:", "#stretched:", NULL };
static const Monitor INVALID_MONITOR_STRUCT = {0,};


enum
{
	BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED,
	BACKGROUND_SIGNAL_LAST
};

static guint background_signals[BACKGROUND_SIGNAL_LAST] = {0};

struct _GreeterBackground
{
	GObject parent_instance;

	struct _GreeterBackgroundPrivate* priv;
};

struct _GreeterBackgroundClass
{
	GObjectClass parent_class;
};

typedef struct _GreeterBackgroundPrivate GreeterBackgroundPrivate;

struct _GreeterBackgroundPrivate
{
	GdkScreen* screen;
	gulong monitors_changed_handler_id;

	GtkWidget* child;

    /* List of groups <GtkAccelGroup*> for greeter screens windows */
	GSList* accel_groups;

	/* Default config for unlisted monitors */
    MonitorConfig* default_monitor_config;

    /* Array of configured monitors for current screen */
	Monitor* monitors;
	gsize monitors_size;

    /* Name => <Monitor*>, "Number" => <Monitor*> */
	GHashTable* monitors_map;

	const Monitor* active_monitor;
};

G_DEFINE_TYPE_WITH_PRIVATE(GreeterBackground, greeter_background, G_TYPE_OBJECT);

/* Implemented in gooroom-greeter.c */
gpointer greeter_save_focus(GtkWidget* widget);
void greeter_restore_focus(const gpointer saved_data);

static const MonitorConfig DEFAULT_MONITOR_CONFIG =
{
    .bg =
    {
        .type = BACKGROUND_TYPE_COLOR,
        .options =
        {
            .color = {.red = 0.0, .green = 0.0, .blue = 0.0, .alpha = 1.0}
        }
    }
};




static void
background_finalize (Background* bg)
{
	switch (bg->type)
	{
		case BACKGROUND_TYPE_IMAGE:
			g_clear_object (&bg->options.image);
			break;
		case BACKGROUND_TYPE_COLOR:
			break;
		case BACKGROUND_TYPE_INVALID:
			g_return_if_reached();
	}

	bg->type = BACKGROUND_TYPE_INVALID;
}

static void
background_unref (Background** bg)
{
	if (!*bg)
		return;
	(*bg)->ref_count--;
	if ((*bg)->ref_count == 0) {
		background_finalize (*bg);
		*bg = NULL;
	}
}

static void
background_config_finalize (BackgroundConfig* config)
{
	switch(config->type)
	{
		case BACKGROUND_TYPE_IMAGE:
			g_free (config->options.image.path);
			break;
		case BACKGROUND_TYPE_COLOR:
			break;
		case BACKGROUND_TYPE_INVALID:
			g_return_if_reached();
	}

	config->type = BACKGROUND_TYPE_INVALID;
}

static void
monitor_config_free (MonitorConfig* config)
{
	background_config_finalize (&config->bg);

	g_free (config);
}

 
static void
monitor_finalize (Monitor* monitor)
{
	if (monitor->window_draw_handler_id)
		g_signal_handler_disconnect (monitor->window, monitor->window_draw_handler_id);

	background_unref (&monitor->background);

	if (monitor->window) {
		GtkWidget* child = gtk_bin_get_child (GTK_BIN (monitor->window));
		if (child) { /* remove greeter widget to avoid "destroy" signal */
			gtk_container_remove (GTK_CONTAINER (monitor->window), child);
		}
		gtk_widget_destroy (GTK_WIDGET (monitor->window));
	}

	g_free (monitor->name);

	*monitor = INVALID_MONITOR_STRUCT;
}

static void
background_config_copy (const BackgroundConfig* source, BackgroundConfig* dest)
{
	*dest = *source;

	switch(dest->type)
	{
		case BACKGROUND_TYPE_IMAGE:
			dest->options.image.path = g_strdup (source->options.image.path);
			break;
		case BACKGROUND_TYPE_COLOR:
			break;
		case BACKGROUND_TYPE_INVALID:
			g_return_if_reached ();
	}
}

static MonitorConfig *
monitor_config_copy (const MonitorConfig* source,
                     MonitorConfig* dest)
{
	if (!dest)
		dest = g_new0 (MonitorConfig, 1);

	background_config_copy (&source->bg, &dest->bg);

	return dest;
}

static GdkPixbuf*
scale_image (GdkPixbuf* source, ScalingMode mode, gint width, gint height)
{
	if(mode == SCALING_MODE_ZOOMED) {
		gint offset_x = 0;
		gint offset_y = 0;
		gint p_width = gdk_pixbuf_get_width(source);
		gint p_height = gdk_pixbuf_get_height(source);
		gdouble scale_x = (gdouble)width / p_width;
		gdouble scale_y = (gdouble)height / p_height;

		if(scale_x < scale_y) {
			scale_x = scale_y;
			offset_x = (width - (p_width * scale_x)) / 2;
		} else {
			scale_y = scale_x;
			offset_y = (height - (p_height * scale_y)) / 2;
		}

		GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE,
                                            gdk_pixbuf_get_bits_per_sample (source),
                                            width, height);
		gdk_pixbuf_composite (source, pixbuf, 0, 0, width, height,
                              offset_x, offset_y, scale_x, scale_y, GDK_INTERP_BILINEAR, 0xFF);
		return pixbuf;
	}

	if (mode == SCALING_MODE_SCALED) {
		gdouble factor;
		gint p_width, p_height;
		gint new_width, new_height;
		gint offset_x, offset_y;

		p_width = gdk_pixbuf_get_width (source);
		p_height = gdk_pixbuf_get_height (source);

		factor = MIN (width/(gdouble)p_width, height/(gdouble)p_height);

		offset_x = (width - (p_width * factor)) / 2;
		offset_y = (height - (p_height * factor)) / 2;

		new_width  = floor (p_width * factor + 0.5);
		new_height = floor (p_height * factor + 0.5);

		GdkPixbuf *new = gdk_pixbuf_scale_simple (source, new_width, new_height, GDK_INTERP_BILINEAR);

		GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE,
                                            gdk_pixbuf_get_bits_per_sample (source),
                                            width, height);

		gdk_pixbuf_composite (new, pixbuf, offset_x, offset_y, new_width, new_height,
                              offset_x, offset_y, 1.0, 1.0, GDK_INTERP_NEAREST, 0xFF);

		g_object_unref (new);

		return pixbuf;
	}

	if (mode == SCALING_MODE_STRETCHED)
		return gdk_pixbuf_scale_simple (source, width, height, GDK_INTERP_BILINEAR);

	return GDK_PIXBUF (g_object_ref (source));
}

static GdkPixbuf*
scale_image_file (const gchar* path, ScalingMode mode, gint width, gint height, GHashTable* cache)
{
	gchar* key = NULL;
	GdkPixbuf* pixbuf = NULL;

	if (cache) {
		key = g_strdup_printf ("%s\n%d %dx%d", path, mode, width, height);
		if (g_hash_table_lookup_extended (cache, key, NULL, (gpointer*)&pixbuf)) {
			g_free (key);
			return GDK_PIXBUF (g_object_ref (pixbuf));
		}
	}

	if (!cache || !g_hash_table_lookup_extended (cache, path, NULL, (gpointer*)&pixbuf)) {
		GError *error = NULL;
		pixbuf = gdk_pixbuf_new_from_file (path, &error);
		if (error) {
			g_warning ("[Background] Failed to load background: %s", error->message);
			g_clear_error (&error);
		} else if (cache)
			g_hash_table_insert (cache, g_strdup (path), g_object_ref (pixbuf));
	} else {
		pixbuf = g_object_ref (pixbuf);
	}

	if(pixbuf) {
		GdkPixbuf* scaled = scale_image (pixbuf, mode, width, height);
		if(cache)
			g_hash_table_insert (cache, g_strdup(key), g_object_ref(scaled));
		g_object_unref (pixbuf);
		pixbuf = scaled;
	}

	g_free(key);

	return pixbuf;
}

static void
greeter_background_get_cursor_position (GreeterBackground* background, gint* x, gint* y)
{
	GdkSeat *seat;
	GdkDevice* pointer;
	GdkDisplay *display;

	display = gdk_display_get_default ();
	seat = gdk_display_get_default_seat (display);
	pointer = gdk_seat_get_pointer (seat);
	gdk_device_get_position (pointer, NULL, x, y);
}

static void
greeter_background_set_cursor_position (GreeterBackground* background, gint x, gint y)
{
	GdkSeat *seat;
	GdkDevice* pointer;
	GdkDisplay* display;
	GreeterBackgroundPrivate* priv = background->priv;

	display = gdk_display_get_default ();
	seat = gdk_display_get_default_seat (display);
	pointer = gdk_seat_get_pointer (seat);
	gdk_device_warp (pointer, priv->screen, x, y);
}

static void
greeter_background_monitors_changed_cb (GdkScreen* screen, GreeterBackground* background)
{
	g_return_if_fail (GREETER_IS_BACKGROUND (background));

	greeter_background_connect (background, screen);
}

//static void
//greeter_background_child_destroyed_cb (GtkWidget* child, GreeterBackground* background)
//{
//	g_clear_object (&background->priv->child);
//	background->priv->child = NULL;
//}

static void
monitor_draw_background (const Monitor* monitor,
                         const Background* background,
                         cairo_t* cr)
{
	g_return_if_fail (monitor != NULL);
	g_return_if_fail (background != NULL);

	switch(background->type)
	{
		case BACKGROUND_TYPE_IMAGE:
			if(background->options.image) {
				GdkRGBA black = {0x0000, 0x0000, 0x0000, 0.7};
				gdk_cairo_set_source_pixbuf(cr, background->options.image, 0, 0);
				cairo_paint(cr);
				gdk_cairo_set_source_rgba (cr, &black);
				cairo_paint(cr);
			}
			break;
		case BACKGROUND_TYPE_COLOR:
			cairo_rectangle (cr, 0, 0, monitor->geometry.width, monitor->geometry.height);
			gdk_cairo_set_source_rgba (cr, &background->options.color);
			cairo_fill(cr);
			break;
		case BACKGROUND_TYPE_INVALID:
			g_return_if_reached();
	}
}

static gboolean
monitor_window_draw_cb (GtkWidget* widget,
                        cairo_t* cr,
                        const Monitor* monitor)
{
	if (!monitor->background)
		return FALSE;

	monitor_draw_background (monitor, monitor->background, cr);

	return FALSE;
}

static gboolean
background_config_initialize (BackgroundConfig* config, const gchar* value)
{
	config->type = BACKGROUND_TYPE_INVALID;

	if (!value || strlen (value) == 0)
		return FALSE;

	if (gdk_rgba_parse (&config->options.color, value)) {
		config->type = BACKGROUND_TYPE_COLOR;
    } else {
		const gchar** prefix = SCALING_MODE_PREFIXES;
		while (*prefix && !g_str_has_prefix (value, *prefix))
			++prefix;

		if (*prefix) {
			config->options.image.mode = (ScalingMode)(prefix - SCALING_MODE_PREFIXES);
			value += strlen (*prefix);
		} else {
			config->options.image.mode = SCALING_MODE_ZOOMED;
		}

		config->options.image.path = g_strdup (value);
		config->type = BACKGROUND_TYPE_IMAGE;
	}

	return TRUE;
}

static Background*
background_new (const BackgroundConfig* config, const Monitor* monitor, GHashTable* images_cache)
{
	Background bg = {0};

	switch (config->type)
	{
		case BACKGROUND_TYPE_IMAGE:
			bg.options.image = scale_image_file (config->options.image.path,
                                                 config->options.image.mode,
                                                 monitor->geometry.width,
                                                 monitor->geometry.height,
                                                 images_cache);
			if (!bg.options.image) {
				g_warning ("[Background] Failed to read wallpaper: %s", config->options.image.path);
				return NULL;
			}
			break;
		case BACKGROUND_TYPE_COLOR:
			bg.options.color = config->options.color;
			break;
		case BACKGROUND_TYPE_INVALID:
			g_return_val_if_reached (NULL);
	}

	bg.type = config->type;
	bg.ref_count = 1;

	Background* result = g_new (Background, 1);
	*result = bg;

	return result;
}

static void
greeter_background_disconnect (GreeterBackground* background)
{
	g_return_if_fail (GREETER_IS_BACKGROUND (background));
	GreeterBackgroundPrivate* priv = background->priv;

	if (priv->monitors_changed_handler_id)
		g_signal_handler_disconnect (priv->screen, priv->monitors_changed_handler_id);
	priv->monitors_changed_handler_id = 0;
	priv->screen = NULL;
	priv->active_monitor = NULL;

	gint i;
	for (i = 0; i < priv->monitors_size; ++i)
		monitor_finalize (&priv->monitors[i]);

	g_free (priv->monitors);
	priv->monitors = NULL;
	priv->monitors_size = 0;

	g_hash_table_unref (priv->monitors_map);
	priv->monitors_map = NULL;
}

static void
greeter_background_set_active_monitor (GreeterBackground* background, const Monitor* active)
{
	GreeterBackgroundPrivate* priv = background->priv;

	if (active && !active->background) {
		if (priv->active_monitor)
			return;

		active = NULL;
	}

    /* Auto */
	if (!active) {
		/* Using primary monitor */
		if (!active) {
			gint i;
			GdkDisplay *display;

			display = gdk_display_get_default ();

			for (i = 0; i < priv->monitors_size; i++) {
				GdkMonitor *gdk_monitor = gdk_display_get_monitor (display, i);
				if (gdk_monitor_is_primary (gdk_monitor)) {
					active = &priv->monitors[i];
					break;
				}
			}

			if (!active->background)
				active = NULL;
			if (active)
				g_debug ("[Background] Active monitor is not specified, using primary monitor");
		}

		/* Fallback: first enabled and/or not skipped monitor (screen always have one) */
		if (!active) {
			gint i;
			for (i = 0; i < priv->monitors_size; i++) {
				const Monitor* monitor = &priv->monitors[i];
				if (monitor->background) {
					active = monitor;
					break;
				}
			}
			if (active)
				g_debug ("[Background] Active monitor is not specified, using first enabled monitor");
		}
	}

	if (!active) {
		if (priv->active_monitor)
			g_warning ("[Background] Active monitor is not specified, failed to identify. Active monitor stays the same: %s #%d", priv->active_monitor->name, priv->active_monitor->number);
		else
			g_warning ("[Background] Active monitor is not specified, failed to identify. Active monitor stays the same: <not defined>");
		return;
	}

	if (active == priv->active_monitor)
		return;

	priv->active_monitor = active;

	g_return_if_fail (priv->active_monitor != NULL);

	if (priv->child) {
		GtkWidget* old_parent = gtk_widget_get_parent(priv->child);
		gpointer focus = greeter_save_focus (priv->child);
		if (old_parent) {
			gtk_container_remove (GTK_CONTAINER (old_parent), priv->child);
		}

		gtk_container_add (GTK_CONTAINER (active->window), priv->child);
		gtk_window_present (active->window);
		greeter_restore_focus (focus);
		g_free (focus);
	} else {
		g_warning ("[Background] Child widget is destroyed or not defined");
	}

	g_debug ("[Background] Active monitor changed to: %s #%d", active->name, active->number);
	g_signal_emit (background, background_signals[BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED], 0);

	gint x, y;
	greeter_background_get_cursor_position (background, &x, &y);
	/* Do not center cursor if it is already inside active monitor */
	if (x < active->geometry.x || x >= active->geometry.x + active->geometry.width ||
        y < active->geometry.y || y >= active->geometry.y + active->geometry.height) {
		greeter_background_set_cursor_position( background,
                                                active->geometry.x + active->geometry.width/2,
                                                active->geometry.y + active->geometry.height/2);
	}
}

static void
greeter_background_finalize (GObject *object)
{
	GreeterBackground *background = GREETER_BACKGROUND (object);

	greeter_background_disconnect (background);

	g_clear_object (&background->priv->child);

	G_OBJECT_CLASS (greeter_background_parent_class)->finalize (object);
}

static void
greeter_background_init (GreeterBackground* self)
{
	GreeterBackgroundPrivate* priv;
	priv = self->priv = greeter_background_get_instance_private (self);

	priv->screen = NULL;
	priv->monitors_changed_handler_id = 0;
	priv->accel_groups = NULL;

	priv->default_monitor_config = monitor_config_copy (&DEFAULT_MONITOR_CONFIG, NULL);

	priv->monitors = NULL;
	priv->monitors_size = 0;
	priv->monitors_map = NULL;

	priv->active_monitor = NULL;
}

static void
greeter_background_class_init (GreeterBackgroundClass* klass)
{
	GObjectClass* gobject_class = G_OBJECT_CLASS(klass);

	gobject_class->finalize = greeter_background_finalize;

	background_signals[BACKGROUND_SIGNAL_ACTIVE_MONITOR_CHANGED] =
					g_signal_new ("active-monitor-changed",
                                  G_TYPE_FROM_CLASS(gobject_class),
                                  G_SIGNAL_RUN_FIRST,
                                  0, /* class_offset */
                                  NULL /* accumulator */, NULL /* accu_data */,
                                  g_cclosure_marshal_VOID__VOID,
                                  G_TYPE_NONE, 0);
}

GreeterBackground*
greeter_background_new (GtkWidget* child)
{
	GreeterBackground* background;
	g_return_val_if_fail (child != NULL, NULL);

	background = GREETER_BACKGROUND (g_object_new (greeter_background_get_type(), NULL));

	background->priv->child = g_object_ref (child);

//	g_signal_connect (background->priv->child, "destroy",
//                      G_CALLBACK (greeter_background_child_destroyed_cb), background);

	return background;
}

void
greeter_background_set_monitor_config (GreeterBackground* background, const gchar* bg)
{
	g_return_if_fail (GREETER_IS_BACKGROUND (background));

	GreeterBackgroundPrivate* priv = background->priv;

	MonitorConfig* config = g_new0 (MonitorConfig, 1);

	if (!background_config_initialize (&config->bg, bg))
		background_config_copy (&DEFAULT_MONITOR_CONFIG.bg, &config->bg);

	if (priv->default_monitor_config)
		monitor_config_free (priv->default_monitor_config);
	priv->default_monitor_config = config;
}

void
greeter_background_connect (GreeterBackground* background, GdkScreen* screen)
{
	g_return_if_fail (GREETER_IS_BACKGROUND (background));
	g_return_if_fail (GDK_IS_SCREEN (screen));

	g_debug ("[Background] Connecting to screen: %p", screen);

	GreeterBackgroundPrivate* priv = background->priv;
	gpointer saved_focus = NULL;
	if (priv->screen) {
		if (priv->active_monitor)
			saved_focus = greeter_save_focus (priv->child);
		greeter_background_disconnect (background);
	}

	GdkDisplay *display = gdk_display_get_default ();

	priv->screen = screen;
	priv->monitors_size = gdk_display_get_n_monitors (display);
	priv->monitors = g_new0 (Monitor, priv->monitors_size);
	priv->monitors_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	g_debug("[Background] Monitors found: %" G_GSIZE_FORMAT, priv->monitors_size);

	GHashTable* images_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	cairo_region_t *screen_region = cairo_region_create ();
	gint i;

	for (i = 0; i < priv->monitors_size; ++i) {
		GdkMonitor *gdk_monitor;
		const MonitorConfig* monitor_config;
		const gchar* printable_name;
		Monitor* monitor = &priv->monitors[i];
		gdk_monitor = gdk_display_get_monitor (display, i);

		monitor->object = background;
		monitor->name = g_strdup (gdk_monitor_get_model (gdk_monitor));
		monitor->number = i;

		printable_name = monitor->name ? monitor->name : "<unknown>";

		gdk_monitor_get_geometry (gdk_monitor, &monitor->geometry);

		g_debug ("[Background] Monitor: %s #%d (%dx%d at %dx%d)%s", printable_name, i,
                 monitor->geometry.width, monitor->geometry.height,
                 monitor->geometry.x, monitor->geometry.y,
                 (i == gdk_monitor_is_primary (gdk_monitor)) ? " primary" : "");

		monitor_config = priv->default_monitor_config;

		/* Simple check to skip fully overlapped monitors.
		   Actually, it's can track only monitors in "mirrors" mode. Nothing more. */
		if (cairo_region_contains_rectangle (screen_region, &monitor->geometry) == CAIRO_REGION_OVERLAP_IN) {
			g_debug ("[Background] Skipping monitor %s #%d, its area is already used by other monitors", printable_name, i);
			continue;
		}
		cairo_region_union_rectangle (screen_region, &monitor->geometry);

		monitor->window = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
		gtk_window_set_type_hint (monitor->window, GDK_WINDOW_TYPE_HINT_DESKTOP);
		gtk_window_set_keep_below (monitor->window, TRUE);
		gtk_window_set_resizable (monitor->window, FALSE);
		gtk_widget_set_app_paintable (GTK_WIDGET (monitor->window), TRUE);
		gtk_window_set_screen (monitor->window, screen);
		gtk_widget_set_size_request (GTK_WIDGET (monitor->window),
                                     monitor->geometry.width, monitor->geometry.height);
		gtk_window_move (monitor->window, monitor->geometry.x, monitor->geometry.y);

		monitor->window_draw_handler_id = g_signal_connect (G_OBJECT (monitor->window), "draw",
                                                            G_CALLBACK (monitor_window_draw_cb),
                                                            monitor);

		GSList* item = NULL;
		for (item = priv->accel_groups; item != NULL; item = g_slist_next(item))
			gtk_window_add_accel_group (monitor->window, item->data);

//        g_signal_connect(G_OBJECT(monitor->window), "enter-notify-event",
//                         G_CALLBACK(monitor_window_enter_notify_cb), monitor);

		monitor->background = background_new (&monitor_config->bg, monitor, images_cache);
		if (!monitor->background)
			monitor->background = background_new (&DEFAULT_MONITOR_CONFIG.bg, monitor, images_cache);

		if (monitor->name)
			g_hash_table_insert (priv->monitors_map, g_strdup (monitor->name), monitor);
		g_hash_table_insert (priv->monitors_map, g_strdup_printf ("%d", i), monitor);

		gtk_widget_show_all (GTK_WIDGET (monitor->window));
	}
	g_hash_table_unref (images_cache);

	if (!priv->active_monitor)
		greeter_background_set_active_monitor (background, NULL);

	if (saved_focus) {
		greeter_restore_focus (saved_focus);
		g_free (saved_focus);
	}

	priv->monitors_changed_handler_id = g_signal_connect (G_OBJECT (screen), "monitors-changed",
			G_CALLBACK (greeter_background_monitors_changed_cb), background);
}

GdkPixbuf *
greeter_background_pixbuf_get (GreeterBackground* background)
{
	Background *bg = NULL;
    g_return_val_if_fail(GREETER_IS_BACKGROUND(background), NULL);

    GreeterBackgroundPrivate* priv = background->priv;

	if (!priv->active_monitor)
		return NULL;

	bg = priv->active_monitor->background;

	if (!bg)
		return NULL;

	return bg->options.image;
}

const GdkRectangle *
greeter_background_get_active_monitor_geometry (GreeterBackground* background)
{
    g_return_val_if_fail(GREETER_IS_BACKGROUND(background), NULL);
    GreeterBackgroundPrivate* priv = background->priv;

    return priv->active_monitor ? &priv->active_monitor->geometry : NULL;
}

void
greeter_background_add_accel_group (GreeterBackground* background,
                                    GtkAccelGroup* group)
{
	g_return_if_fail (GREETER_IS_BACKGROUND(background));
	g_return_if_fail (group != NULL);
	GreeterBackgroundPrivate* priv = background->priv;

	if (priv->monitors) {
		gint i;
		for(i = 0; i < priv->monitors_size; ++i)
			if(priv->monitors[i].window)
				gtk_window_add_accel_group (priv->monitors[i].window, group);
	}

	priv->accel_groups = g_slist_append(priv->accel_groups, group);
}

void
greeter_background_set_active_monitor_from_geometry (GreeterBackground  *background,
                                                     const GdkRectangle *geometry)
{
	gpointer value;
	GHashTableIter iter;

	g_hash_table_iter_init (&iter, background->priv->monitors_map);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		Monitor *monitor = value;
		if (gdk_rectangle_equal (&monitor->geometry, geometry)) {
			greeter_background_set_active_monitor (background, monitor);
			break;
		}
	}
}

//static gboolean
//monitor_window_enter_notify_cb(GtkWidget* widget,
//                               GdkEventCrossing* event,
//                               const Monitor* monitor)
//{
//    if(monitor->object->priv->active_monitor == monitor)
//    {
//        GdkWindow *gdkwindow = gtk_widget_get_window (widget);
//        Window window = GDK_WINDOW_XID (gdkwindow);
//        Display *display = GDK_WINDOW_XDISPLAY (gdkwindow);
//
//        static Atom wm_protocols = None;
//        static Atom wm_take_focus = None;
//
//        if (!wm_protocols)
//            wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
//        if (!wm_take_focus)
//            wm_take_focus = XInternAtom(display, "WM_TAKE_FOCUS", False);
//
//        XEvent ev = {0};
//        ev.xclient.type = ClientMessage;
//        ev.xclient.window = window;
//        ev.xclient.message_type = wm_protocols;
//        ev.xclient.format = 32;
//        ev.xclient.data.l[0] = wm_take_focus;
//        ev.xclient.data.l[1] = CurrentTime;
//        XSendEvent(display, window, False, 0L, &ev);
//    }
//    else if(monitor->object->priv->follow_cursor && greeter_background_monitor_enabled(monitor->object, monitor))
//        greeter_background_set_active_monitor(monitor->object, monitor);
//    return FALSE;
//}

/* The following code for setting a RetainPermanent background pixmap was taken
   originally from Gnome, with some fixes from MATE. see:
   https://github.com/mate-desktop/mate-desktop/blob/master/libmate-desktop/mate-bg.c */
//static cairo_surface_t*
//create_root_surface (GdkScreen* screen)
//{
//	gint number, width, height;
//	Display *display;
//	Pixmap pixmap;
//	cairo_surface_t *surface;
//
//	number = gdk_screen_get_number (screen);
//	width = gdk_screen_get_width (screen);
//	height = gdk_screen_get_height (screen);
//
//    /* Open a new connection so with Retain Permanent so the pixmap remains when the greeter quits */
//	gdk_flush ();
//	display = XOpenDisplay (gdk_display_get_name (gdk_screen_get_display (screen)));
//	if (!display) {
//		g_warning ("[Background] Failed to create root pixmap");
//		return NULL;
//	}
//
//	XSetCloseDownMode (display, RetainPermanent);
//	pixmap = XCreatePixmap (display, RootWindow (display, number), width, height, DefaultDepth (display, number));
//	XCloseDisplay (display);
//
//	/* Convert into a Cairo surface */
//	surface = cairo_xlib_surface_create (GDK_SCREEN_XDISPLAY (screen),
//                                         pixmap,
//                                         GDK_VISUAL_XVISUAL (gdk_screen_get_system_visual (screen)),
//                                         width, height);
//
//	return surface;
//}

/* Sets the "ESETROOT_PMAP_ID" property to later be used to free the pixmap */
//static void
//set_root_pixmap_id (GdkScreen* screen, Display* display, Pixmap xpixmap)
//{
//	Window xroot = RootWindow (display, gdk_screen_get_number (screen));
//	char *atom_names[] = {"_XROOTPMAP_ID", "ESETROOT_PMAP_ID"};
//	Atom atoms[G_N_ELEMENTS(atom_names)] = {0};
//
//	Atom type;
//	int format;
//	unsigned long nitems, after;
//	unsigned char *data_root, *data_esetroot;
//
//	/* Get atoms for both properties in an array, only if they exist.
//	 * This method is to avoid multiple round-trips to Xserver
//	 */
//	if (XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), True, atoms) &&
//        atoms[0] != None && atoms[1] != None) {
//		XGetWindowProperty (display, xroot, atoms[0], 0L, 1L, False, AnyPropertyType,
//                            &type, &format, &nitems, &after, &data_root);
//		if (data_root && type == XA_PIXMAP && format == 32 && nitems == 1) {
//			XGetWindowProperty (display, xroot, atoms[1], 0L, 1L, False, AnyPropertyType,
//                                &type, &format, &nitems, &after, &data_esetroot);
//			if (data_esetroot && type == XA_PIXMAP && format == 32 && nitems == 1) {
//				Pixmap xrootpmap = *((Pixmap *) data_root);
//				Pixmap esetrootpmap = *((Pixmap *) data_esetroot);
//				XFree (data_root);
//				XFree (data_esetroot);
//
//				gdk_error_trap_push ();
//				if (xrootpmap && xrootpmap == esetrootpmap) {
//					XKillClient (display, xrootpmap);
//				}
//				if (esetrootpmap && esetrootpmap != xrootpmap) {
//					XKillClient (display, esetrootpmap);
//				}
//
//				XSync (display, False);
//				gdk_error_trap_pop_ignored ();
//			}
//		}
//	}
//
//    /* Get atoms for both properties in an array, create them if needed.
//     * This method is to avoid multiple round-trips to Xserver
//     */
//	if (!XInternAtoms (display, atom_names, G_N_ELEMENTS(atom_names), False, atoms) ||
//        atoms[0] == None || atoms[1] == None) {
//		g_warning ("[Background] Could not create atoms needed to set root pixmap id/properties.\n");
//		return;
//	}
//
//    /* Set new _XROOTMAP_ID and ESETROOT_PMAP_ID properties */
//	XChangeProperty (display, xroot, atoms[0], XA_PIXMAP, 32,
//                     PropModeReplace, (unsigned char *) &xpixmap, 1);
//
//	XChangeProperty (display, xroot, atoms[1], XA_PIXMAP, 32,
//                     PropModeReplace, (unsigned char *) &xpixmap, 1);
//}

/**
* set_surface_as_root:
* @screen: the #GdkScreen to change root background on
* @surface: the #cairo_surface_t to set root background from.
* Must be an xlib surface backing a pixmap.
*
* Set the root pixmap, and properties pointing to it. We
* do this atomically with a server grab to make sure that
* we won't leak the pixmap if somebody else it setting
* it at the same time. (This assumes that they follow the
* same conventions we do). @surface should come from a call
* to create_root_surface().
**/
//static void
//set_surface_as_root (GdkScreen* screen, cairo_surface_t* surface)
//{
//	g_return_if_fail(cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_XLIB);
//
//    /* Desktop background pixmap should be created from dummy X client since most
//     * applications will try to kill it with XKillClient later when changing pixmap
//     */
//	Display *display = GDK_DISPLAY_XDISPLAY (gdk_screen_get_display (screen));
//	Pixmap pixmap_id = cairo_xlib_surface_get_drawable (surface);
//	Window xroot = RootWindow (display, gdk_screen_get_number(screen));
//
//	XGrabServer (display);
//
//	XSetWindowBackgroundPixmap (display, xroot, pixmap_id);
//	set_root_pixmap_id (screen, display, pixmap_id);
//	XClearWindow (display, xroot);
//
//	XFlush (display);
//	XUngrabServer (display);
//}
