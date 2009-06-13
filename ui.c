/*
 * Copyright (C) 2008 Felipe Contreras.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <dbus/dbus.h>

#include <string.h>

#include "gst-backend.h"

static gchar *filename;
static GtkWidget *video_output;
static GtkWidget *pause_button;
static GtkWidget *scale;
static guint64 duration;
static GtkWindow *window;
static GtkWidget *controls;
static DBusConnection *session;

#define DURATION_IS_VALID(x) (x != 0 && x != (guint64) -1)

static void
inhibit_screensaver (gboolean inhibit)
{
    DBusMessage *msg, *reply;
    DBusMessageIter iter;
    static dbus_uint32_t cookie = 0;
    const char *value;

    if (inhibit) {
        if (cookie != 0)
            return;

        msg = dbus_message_new_method_call ("org.gnome.ScreenSaver",
                                           "/", "org.gnome.ScreenSaver", "Inhibit");

        value = "gst-player";
        dbus_message_append_args (msg, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
        value = "Playing full-screen";
        dbus_message_append_args (msg, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);

        reply = dbus_connection_send_with_reply_and_block (session, msg, -1, NULL);
        if (!reply) {
            g_warning ("error sending org.gnome.ScreenSaver.Inhibit\n");
            return;
        }

        dbus_message_unref (msg);

        dbus_message_get_args (reply, NULL, DBUS_TYPE_UINT32, &cookie, NULL);
    } else {
        if (cookie == 0)
            return;

        msg = dbus_message_new_method_call ("org.gnome.ScreenSaver",
                                            "/", "org.gnome.ScreenSaver", "UnInhibit");
        dbus_message_append_args (msg, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID);
        
        if (!dbus_connection_send (session, msg, NULL))
            g_warning ("error sending org.gnome.ScreenSaver.UnInhibit\n");

        dbus_message_unref (msg);
        cookie = 0;
    }
}

static void
toggle_paused (void)
{
    static gboolean paused = FALSE;
    if (paused)
    {
        backend_resume ();
        gtk_button_set_label (GTK_BUTTON (pause_button), "Pause");
        paused = FALSE;
    }
    else
    {
        backend_pause ();
        gtk_button_set_label (GTK_BUTTON (pause_button), "Resume");
        paused = TRUE;
    }
}

static void
toggle_fullscreen (void)
{
    if (gdk_window_get_state (GTK_WIDGET (window)->window) == GDK_WINDOW_STATE_FULLSCREEN)
    {
        gtk_window_unfullscreen (window);
        gtk_widget_show (controls);
        inhibit_screensaver (FALSE);
    }
    else
    {
        gtk_window_fullscreen (window);
        gtk_widget_hide (controls);
        inhibit_screensaver (TRUE);
    }
}

static void
pause_cb (GtkWidget *widget,
          gpointer data)
{
    toggle_paused ();
}

static void
reset_cb (GtkWidget *widget,
          gpointer data)
{
    backend_reset ();
}

static gboolean
delete_event (GtkWidget *widget,
              GdkEvent *event,
              gpointer data)
{
    return FALSE;
}

static void
destroy (GtkWidget *widget,
         gpointer data)
{
    gtk_main_quit ();
}

static gboolean
key_press (GtkWidget *widget,
           GdkEventKey *event,
           gpointer data)
{
    switch (event->keyval)
    {
        case GDK_P:
        case GDK_p:
        case GDK_space:
            toggle_paused ();
            break;
        case GDK_F:
        case GDK_f:
            toggle_fullscreen ();
            break;
        case GDK_R:
        case GDK_r:
            backend_reset ();
            break;
        case GDK_Right:
            backend_seek (10);
            break;
        case GDK_Left:
            backend_seek (-10);
            break;
        case GDK_Q:
        case GDK_q:
            gtk_main_quit ();
            break;
        default:
            break;
    }

    return TRUE;
}

static void
seek_cb (GtkRange *range,
         GtkScrollType scroll,
         gdouble value,
         gpointer data)
{
    guint64 to_seek;

    if (!DURATION_IS_VALID (duration))
        duration = backend_query_duration ();

    if (!DURATION_IS_VALID (duration))
        return;

    to_seek = (value / 100) * duration;

#if 0
    g_print ("value: %f\n", value);
    g_print ("duration: %llu\n", duration);
    g_print ("seek: %llu\n", to_seek);
#endif

    backend_seek_absolute (to_seek);
}

static void
start (void)
{
    GtkWidget *button;
    GtkWidget *vbox;

    window = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));

    g_signal_connect (G_OBJECT (window), "delete_event",
                      G_CALLBACK (delete_event), NULL);

    g_signal_connect (G_OBJECT (window), "destroy",
                      G_CALLBACK (destroy), NULL);

    g_signal_connect (G_OBJECT (window), "key-press-event",
                      G_CALLBACK (key_press), NULL);

    gtk_container_set_border_width (GTK_CONTAINER (window), 0);

    vbox = gtk_vbox_new (FALSE, 0);

    gtk_container_add (GTK_CONTAINER (window), vbox);

    gtk_widget_show (vbox);

    controls = gtk_hbox_new (FALSE, 0);

    gtk_box_pack_end (GTK_BOX (vbox), controls, FALSE, FALSE, 2);

    gtk_widget_show (controls);

    {
        GdkColor color;

        gdk_color_parse ("black", &color);
        video_output = gtk_drawing_area_new ();
        gtk_widget_modify_bg (GTK_WIDGET (video_output), GTK_STATE_NORMAL, &color);

        gtk_box_pack_start (GTK_BOX (vbox), video_output, TRUE, TRUE, 0);

        gtk_widget_set_size_request (video_output, 0x200, 0x100);

        gtk_widget_show (video_output);
    }

    {
        button = gtk_button_new_with_label ("Pause");

        g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (pause_cb), NULL);

        gtk_box_pack_start (GTK_BOX (controls), button, FALSE, FALSE, 2);

        gtk_widget_show (button);

        pause_button = button;
    }

    {
        button = gtk_button_new_with_label ("Reset");

        g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (reset_cb), NULL);

        gtk_box_pack_start (GTK_BOX (controls), button, FALSE, FALSE, 2);

        gtk_widget_show (button);
    }

    {
        GtkObject *adjustment;
        adjustment = gtk_adjustment_new (0, 0, 101, 1, 5, 1);
        scale = gtk_hscale_new (GTK_ADJUSTMENT (adjustment));

        gtk_box_pack_end (GTK_BOX (controls), scale, TRUE, TRUE, 2);

        g_signal_connect (G_OBJECT (scale), "change-value",
                          G_CALLBACK (seek_cb), NULL);

        gtk_widget_show (scale);
    }

    gtk_widget_show (GTK_WIDGET (window));

    {
        DBusError error;

        dbus_error_init (&error);
        session = dbus_bus_get (DBUS_BUS_SESSION, &error);
        if (!session)
            g_warning ("%s: %s\n", error.name, error.message);
    }
}

static gboolean
init (gpointer data)
{
    backend_set_window (GINT_TO_POINTER (GDK_WINDOW_XWINDOW (video_output->window)));

    if (filename)
        backend_play (filename);

    return FALSE;
}

static gboolean
timeout (gpointer data)
{
    guint64 pos;

    pos = backend_query_position ();
    if (!DURATION_IS_VALID (duration))
        duration = backend_query_duration ();

    if (!DURATION_IS_VALID (duration))
        return TRUE;

#if 0
    g_debug ("duration=%f", duration / ((double) 60 * 1000 * 1000 * 1000));
    g_debug ("position=%llu", pos);
#endif

    /** @todo use events for seeking instead of checking for bad positions. */
    if (pos != 0)
    {
        double value;
        value = (pos * (((double) 100) / duration));
        gtk_range_set_value (GTK_RANGE (scale), value);
    }

    return TRUE;
}

int
main (int argc,
      char *argv[])
{
    gtk_init (&argc, &argv);
    backend_init (&argc, &argv);

    start ();

    if (argc > 1)
    {
        filename = g_strdup (argv[1]);
    }

    toggle_fullscreen ();
    g_idle_add (init, NULL);
    g_timeout_add (1000, timeout, NULL);

    gtk_main ();

    g_free (filename);

    backend_deinit ();

    return 0;
}
