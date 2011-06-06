/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <unistd.h>

#include "configuration.h"
#include "display-manager.h"
#include "xserver.h"

static gchar *config_path = CONFIG_FILE;
static GMainLoop *loop = NULL;
static gboolean test_mode = FALSE;
static GTimer *log_timer;
static FILE *log_file;
static gboolean debug = FALSE;

static DisplayManager *display_manager = NULL;

static GDBusConnection *bus = NULL;

#define LDM_BUS_NAME "org.lightdm.LightDisplayManager"

static void
log_cb (const gchar *log_domain, GLogLevelFlags log_level,
        const gchar *message, gpointer data)
{
    /* Log everything to a file */
    if (log_file) {
        const gchar *prefix;

        switch (log_level & G_LOG_LEVEL_MASK) {
        case G_LOG_LEVEL_ERROR:
            prefix = "ERROR:";
            break;
        case G_LOG_LEVEL_CRITICAL:
            prefix = "CRITICAL:";
            break;
        case G_LOG_LEVEL_WARNING:
            prefix = "WARNING:";
            break;
        case G_LOG_LEVEL_MESSAGE:
            prefix = "MESSAGE:";
            break;
        case G_LOG_LEVEL_INFO:
            prefix = "INFO:";
            break;
        case G_LOG_LEVEL_DEBUG:
            prefix = "DEBUG:";
            break;
        default:
            prefix = "LOG:";
            break;
        }

        fprintf (log_file, "[%+.2fs] %s %s\n", g_timer_elapsed (log_timer, NULL), prefix, message);
        fflush (log_file);
    }

    /* Only show debug if requested */
    if (log_level & G_LOG_LEVEL_DEBUG) {
        if (debug)
            g_log_default_handler (log_domain, log_level, message, data);
    }
    else
        g_log_default_handler (log_domain, log_level, message, data);    
}

static void
log_init (void)
{
    gchar *log_dir, *path;

    log_timer = g_timer_new ();

    /* Log to a file */
    log_dir = config_get_string (config_get_instance (), "LightDM", "log-directory");
    g_mkdir_with_parents (log_dir, 0755);
    path = g_build_filename (log_dir, "lightdm.log", NULL);
    g_free (log_dir);

    log_file = fopen (path, "w");
    g_log_set_default_handler (log_cb, NULL);

    g_debug ("Logging to %s", path);
    g_free (path);
}

static void
signal_cb (ChildProcess *process, int signum)
{
    g_debug ("Caught %s signal, exiting", g_strsignal (signum));
    g_object_unref (display_manager);
    g_main_loop_quit (loop);
}

static void
handle_display_manager_call (GDBusConnection       *connection,
                             const gchar           *sender,
                             const gchar           *object_path,
                             const gchar           *interface_name,
                             const gchar           *method_name,
                             GVariant              *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer               user_data)
{
    if (g_strcmp0 (method_name, "AddDisplay") == 0)
    {
        if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("()")))
            return;

        display_manager_add_display (display_manager);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SwitchToUser") == 0)
    {
        gchar *username;

        if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(s)")))
            return;

        g_variant_get (parameters, "(s)", &username);
        display_manager_switch_to_user (display_manager, username);
        g_dbus_method_invocation_return_value (invocation, NULL);
        g_free (username);
    }
    else if (g_strcmp0 (method_name, "SwitchToGuest") == 0)
    {
        if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("()")))
            return;

        display_manager_switch_to_guest (display_manager);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
}

static GVariant *
handle_display_manager_get_property (GDBusConnection       *connection,
                                     const gchar           *sender,
                                     const gchar           *object_path,
                                     const gchar           *interface_name,
                                     const gchar           *property_name,
                                     GError               **error,
                                     gpointer               user_data)
{
    if (g_strcmp0 (property_name, "ConfigFile") == 0)
        return g_variant_new_string (config_path);

    return NULL;
}

static void
bus_acquired_cb (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
    const gchar *display_manager_interface =
        "<node>"
        "  <interface name='org.lightdm.LightDisplayManager'>"
        "    <property name='ConfigFile' type='s' access='read'/>"
        "    <method name='AddDisplay'/>"
        "    <method name='SwitchToUser'>"
        "      <arg name='username' direction='in' type='s'/>"
        "    </method>"
        "    <method name='SwitchToGuest'/>"
        "  </interface>"
        "</node>";
    static const GDBusInterfaceVTable display_manager_vtable =
    {
        handle_display_manager_call,
        handle_display_manager_get_property
    };
    GDBusNodeInfo *display_manager_info;

    bus = connection;

    display_manager_info = g_dbus_node_info_new_for_xml (display_manager_interface, NULL);
    g_assert (display_manager_info != NULL);
    g_dbus_connection_register_object (connection,
                                       "/org/lightdm/LightDisplayManager",
                                       display_manager_info->interfaces[0],
                                       &display_manager_vtable,
                                       NULL, NULL,
                                       NULL);
}

static void
name_lost_cb (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
    if (connection)
        g_printerr ("Failed to use bus name " LDM_BUS_NAME ", do you have appropriate permissions?\n");
    else
        g_printerr ("Failed to get system bus");

    exit (EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
    FILE *pid_file;
    GOptionContext *option_context;
    gchar *pid_path = "/var/run/lightdm.pid";
    gchar *theme_dir = THEME_DIR, *theme_engine_dir = THEME_ENGINE_DIR;
    gboolean show_version = FALSE;
    GOptionEntry options[] = 
    {
        { "config", 'c', 0, G_OPTION_ARG_STRING, &config_path,
          /* Help string for command line --config flag */
          N_("Use configuration file"), NULL },
        { "debug", 'd', 0, G_OPTION_ARG_NONE, &debug,
          /* Help string for command line --debug flag */
          N_("Print debugging messages"), NULL },
        { "test-mode", 0, 0, G_OPTION_ARG_NONE, &test_mode,
          /* Help string for command line --test-mode flag */
          N_("Run as unprivileged user"), NULL },
        { "pid-file", 0, 0, G_OPTION_ARG_STRING, &pid_path,
          /* Help string for command line --pid-file flag */
          N_("File to write PID into"), NULL },
        { "theme-dir", 0, 0, G_OPTION_ARG_STRING, &theme_dir,
          /* Help string for command line --theme-dir flag */
          N_("Directory to load themes from"), NULL },
        { "theme-engine-dir", 0, 0, G_OPTION_ARG_STRING, &theme_engine_dir,
          /* Help string for command line --theme-engine-dir flag */
          N_("Directory to load theme engines from"), NULL },
        { "version", 'v', 0, G_OPTION_ARG_NONE, &show_version,
          /* Help string for command line --version flag */
          N_("Show release version"), NULL },
        { NULL }
    };
    GError *error = NULL;

    g_thread_init (NULL);
    g_type_init ();

    g_signal_connect (child_process_get_parent (), "got-signal", G_CALLBACK (signal_cb), NULL);

    option_context = g_option_context_new (/* Arguments and description for --help test */
                                           _("- Display Manager"));
    g_option_context_add_main_entries (option_context, options, GETTEXT_PACKAGE);
    if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
        fprintf (stderr, "%s\n", error->message);
        fprintf (stderr, /* Text printed out when an unknown command-line argument provided */
                 _("Run '%s --help' to see a full list of available command line options."), argv[0]);
        fprintf (stderr, "\n");
        return EXIT_FAILURE;
    }
    g_clear_error (&error);
    if (show_version)
    {
        /* NOTE: Is not translated so can be easily parsed */
        g_printerr ("%s %s\n", LIGHTDM_BINARY, VERSION);
        return EXIT_SUCCESS;
    }

    /* Write PID file */
    pid_file = fopen (pid_path, "w");
    if (pid_file)
    {
        fprintf (pid_file, "%d\n", getpid ());
        fclose (pid_file);
    }

    /* Check if root */
    if (!test_mode && getuid () != 0)
    {
        g_printerr ("Only root can run Light Display Manager.  To run as a regular user for testing run with the --test-mode flag.\n");
        return 1;
    }

    /* Test mode requires Xephry */
    if (test_mode)
    {
        gchar *xephyr_path;
      
        xephyr_path = g_find_program_in_path ("Xephyr");
        if (!xephyr_path)
        {
            g_printerr ("Test mode requires Xephyr to be installed but it cannot be found.  Please install it or update your PATH environment variable.\n");
            return 1;
        }
        g_free (xephyr_path);
    }

    loop = g_main_loop_new (NULL, FALSE);

    g_bus_own_name (test_mode ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
                    LDM_BUS_NAME,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    bus_acquired_cb,
                    NULL,
                    name_lost_cb,
                    NULL,
                    NULL);

    if (!config_load_from_file (config_get_instance (), config_path, &error))
    {
        g_warning ("Failed to load configuration from %s: %s", config_path, error->message); // FIXME: Don't make warning on no file, just info
        exit (EXIT_FAILURE);
    }
    g_clear_error (&error);

    /* Set default values */
    config_set_string (config_get_instance (), "LightDM", "log-directory", LOG_DIR);
    config_set_string (config_get_instance (), "LightDM", "theme-directory", theme_dir);
    config_set_string (config_get_instance (), "LightDM", "theme-engine-directory", theme_engine_dir);
    config_set_string (config_get_instance (), "LightDM", "authorization-directory", XAUTH_DIR);
    config_set_string (config_get_instance (), "LightDM", "cache-directory", CACHE_DIR);

    if (test_mode)
    {
        gchar *path;
        config_set_boolean (config_get_instance (), "LightDM", "test-mode", TRUE);
        path = g_build_filename (g_get_user_cache_dir (), "lightdm", "authority", NULL);
        config_set_string (config_get_instance (), "LightDM", "authorization-directory", path);
        g_free (path);
        path = g_build_filename (g_get_user_cache_dir (), "lightdm", NULL);
        config_set_string (config_get_instance (), "LightDM", "log-directory", path);
        g_free (path);
    }

    log_init ();

    g_debug ("Starting Light Display Manager %s, PID=%i", VERSION, getpid ());

    if (test_mode)
        g_debug ("Running in test mode");

    g_debug ("Loaded configuration from %s", config_path);

    display_manager = display_manager_new ();

    display_manager_start (display_manager);

    g_main_loop_run (loop);

    return 0;
}
