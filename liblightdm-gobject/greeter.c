/*
 * Copyright (C) 2010 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version. See http://www.gnu.org/copyleft/lgpl.html the full text of the
 * license.
 */

#include <config.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <locale.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <gio/gdesktopappinfo.h>
#include <security/pam_appl.h>
#include <libxklavier/xklavier.h>

#include "lightdm/greeter.h"
#include "user-private.h"

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_NUM_USERS,
    PROP_USERS,
    PROP_DEFAULT_LANGUAGE,
    PROP_LAYOUTS,
    PROP_LAYOUT,
    PROP_SESSIONS,
    PROP_DEFAULT_SESSION_HINT,
    PROP_HIDE_USERS_HINT,
    PROP_HAS_GUEST_ACCOUNT_HINT,
    PROP_SELECT_USER_HINT,
    PROP_SELECT_GUEST_HINT,
    PROP_AUTOLOGIN_USER_HINT,
    PROP_AUTOLOGIN_GUEST_HINT,
    PROP_AUTOLOGIN_TIMEOUT_HINT,
    PROP_AUTHENTICATION_USER,
    PROP_IN_AUTHENTICATION,
    PROP_IS_AUTHENTICATED,
    PROP_CAN_SUSPEND,
    PROP_CAN_HIBERNATE,
    PROP_CAN_RESTART,
    PROP_CAN_SHUTDOWN
};

enum {
    CONNECTED,
    SHOW_PROMPT,
    SHOW_MESSAGE,
    AUTHENTICATION_COMPLETE,
    SESSION_FAILED,
    AUTOLOGIN_TIMER_EXPIRED,
    USER_ADDED,
    USER_CHANGED,
    USER_REMOVED,
    QUIT,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

struct _LdmGreeterPrivate
{
    GDBusConnection *lightdm_bus;

    GDBusConnection *system_bus;

    GIOChannel *to_server_channel, *from_server_channel;
    guint8 *read_buffer;
    gsize n_read;

    Display *display;

    gchar *hostname;

    /* File monitor for password file */
    GFileMonitor *passwd_monitor;
  
    /* TRUE if have scanned users */
    gboolean have_users;

    /* List of users */
    GList *users;
  
    gboolean have_languages;
    GList *languages;

    XklEngine *xkl_engine;
    XklConfigRec *xkl_config;
    gboolean have_layouts;
    GList *layouts;
    gchar *layout;

    gboolean have_sessions;
    GList *sessions;

    gchar *authentication_user;
    gboolean in_authentication;
    gboolean is_authenticated;
    guint32 authenticate_sequence_number;
    gboolean cancelling_authentication;
  
    GHashTable *hints;

    guint login_timeout;
};

G_DEFINE_TYPE (LdmGreeter, ldm_greeter, G_TYPE_OBJECT);

#define HEADER_SIZE 8
#define MAX_MESSAGE_LENGTH 1024

#define PASSWD_FILE      "/etc/passwd"
#define USER_CONFIG_FILE "/etc/lightdm/users.conf"

/* Messages from the greeter to the server */
typedef enum
{
    GREETER_MESSAGE_CONNECT = 0,
    GREETER_MESSAGE_LOGIN,
    GREETER_MESSAGE_LOGIN_AS_GUEST,
    GREETER_MESSAGE_CONTINUE_AUTHENTICATION,
    GREETER_MESSAGE_START_SESSION,
    GREETER_MESSAGE_CANCEL_AUTHENTICATION
} GreeterMessage;

/* Messages from the server to the greeter */
typedef enum
{
    SERVER_MESSAGE_CONNECTED = 0,
    SERVER_MESSAGE_QUIT,
    SERVER_MESSAGE_PROMPT_AUTHENTICATION,
    SERVER_MESSAGE_END_AUTHENTICATION,
    SERVER_MESSAGE_SESSION_FAILED,
} ServerMessage;

/**
 * ldm_greeter_new:
 *
 * Create a new greeter.
 *
 * Return value: the new #LdmGreeter
 **/
LdmGreeter *
ldm_greeter_new ()
{
    return g_object_new (LDM_TYPE_GREETER, NULL);
}

static gboolean
timed_login_cb (gpointer data)
{
    LdmGreeter *greeter = data;

    greeter->priv->login_timeout = 0;
    g_signal_emit (G_OBJECT (greeter), signals[AUTOLOGIN_TIMER_EXPIRED], 0);

    return FALSE;
}

static guint32
int_length ()
{
    return 4;
}

static void
write_message (LdmGreeter *greeter, guint8 *message, gsize message_length)
{
    GError *error = NULL;
    if (g_io_channel_write_chars (greeter->priv->to_server_channel, (gchar *) message, message_length, NULL, NULL) != G_IO_STATUS_NORMAL)
        g_warning ("Error writing to daemon: %s", error->message);
    else
        g_debug ("Wrote %zi bytes to daemon", message_length);
    g_clear_error (&error);
    g_io_channel_flush (greeter->priv->to_server_channel, NULL);
}

static void
write_int (guint8 *buffer, gint buffer_length, guint32 value, gsize *offset)
{
    if (*offset + 4 >= buffer_length)
        return;
    buffer[*offset] = value >> 24;
    buffer[*offset+1] = (value >> 16) & 0xFF;
    buffer[*offset+2] = (value >> 8) & 0xFF;
    buffer[*offset+3] = value & 0xFF;
    *offset += 4;
}

static void
write_string (guint8 *buffer, gint buffer_length, const gchar *value, gsize *offset)
{
    gint length = strlen (value);
    write_int (buffer, buffer_length, length, offset);
    if (*offset + length >= buffer_length)
        return;
    memcpy (buffer + *offset, value, length);
    *offset += length;
}

static guint32
read_int (LdmGreeter *greeter, gsize *offset)
{
    guint32 value;
    guint8 *buffer;
    if (greeter->priv->n_read - *offset < int_length ())
    {
        g_warning ("Not enough space for int, need %i, got %zi", int_length (), greeter->priv->n_read - *offset);
        return 0;
    }
    buffer = greeter->priv->read_buffer + *offset;
    value = buffer[0] << 24 | buffer[1] << 16 | buffer[2] << 8 | buffer[3];
    *offset += int_length ();
    return value;
}

static gchar *
read_string (LdmGreeter *greeter, gsize *offset)
{
    guint32 length;
    gchar *value;

    length = read_int (greeter, offset);
    if (greeter->priv->n_read - *offset < length)
    {
        g_warning ("Not enough space for string, need %u, got %zu", length, greeter->priv->n_read - *offset);
        return g_strdup ("");
    }

    value = g_malloc (sizeof (gchar) * (length + 1));
    memcpy (value, greeter->priv->read_buffer + *offset, length);
    value[length] = '\0';
    *offset += length;

    return value;
}

static guint32
string_length (const gchar *value)
{
    return int_length () + strlen (value);
}

static void
write_header (guint8 *buffer, gint buffer_length, guint32 id, guint32 length, gsize *offset)
{
    write_int (buffer, buffer_length, id, offset);
    write_int (buffer, buffer_length, length, offset);
}

static guint32 get_packet_length (LdmGreeter *greeter)
{
    gsize offset = 4;
    return read_int (greeter, &offset);
}

static void
handle_connected (LdmGreeter *greeter, guint32 length, gsize *offset)
{
    gchar *version;
    GString *hint_string;
    int timeout;

    version = read_string (greeter, offset);
    hint_string = g_string_new ("");
    while (*offset < length)
    {
        gchar *name, *value;
      
        name = read_string (greeter, offset);
        value = read_string (greeter, offset);
        g_hash_table_insert (greeter->priv->hints, name, value);
        g_string_append_printf (hint_string, " %s=%s", name, value);
    }

    g_debug ("Connected version=%s%s", version, hint_string->str);
    g_free (version);
    g_string_free (hint_string, TRUE);

    /* Set timeout for default login */
    timeout = ldm_greeter_get_autologin_timeout_hint (greeter);
    if (timeout)
    {
        g_debug ("Setting autologin timer for %d seconds", timeout);
        greeter->priv->login_timeout = g_timeout_add (timeout * 1000, timed_login_cb, greeter);
    }
    g_signal_emit (G_OBJECT (greeter), signals[CONNECTED], 0);
}

static void
handle_prompt_authentication (LdmGreeter *greeter, gsize *offset)
{
    guint32 sequence_number, n_messages, i;

    sequence_number = read_int (greeter, offset);
    if (sequence_number != greeter->priv->authenticate_sequence_number)
    {
        g_debug ("Ignoring prompt authentication with invalid sequence number %d", sequence_number);
        return;
    }

    if (greeter->priv->cancelling_authentication)
    {
        g_debug ("Ignoring prompt authentication as waiting for it to cancel");
        return;
    }

    n_messages = read_int (greeter, offset);
    g_debug ("Prompt user with %d message(s)", n_messages);

    for (i = 0; i < n_messages; i++)
    {
        int msg_style;
        gchar *msg;

        msg_style = read_int (greeter, offset);
        msg = read_string (greeter, offset);

        // FIXME: Should stop on prompts?
        switch (msg_style)
        {
        case PAM_PROMPT_ECHO_OFF:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_PROMPT], 0, msg, LDM_PROMPT_TYPE_SECRET);
            break;
        case PAM_PROMPT_ECHO_ON:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_PROMPT], 0, msg, LDM_PROMPT_TYPE_QUESTION);
            break;
        case PAM_ERROR_MSG:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_MESSAGE], 0, msg, LDM_MESSAGE_TYPE_ERROR);
            break;
        case PAM_TEXT_INFO:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_MESSAGE], 0, msg, LDM_MESSAGE_TYPE_INFO);
            break;
        }

        g_free (msg);
    }
}

static void
handle_end_authentication (LdmGreeter *greeter, gsize *offset)
{ 
    guint32 sequence_number, return_code;

    sequence_number = read_int (greeter, offset);
    return_code = read_int (greeter, offset);

    if (sequence_number != greeter->priv->authenticate_sequence_number)
    {
        g_debug ("Ignoring end authentication with invalid sequence number %d", sequence_number);
        return;
    }

    g_debug ("Authentication complete with return code %d", return_code);
    greeter->priv->cancelling_authentication = FALSE;
    greeter->priv->is_authenticated = (return_code == 0);
    if (!greeter->priv->is_authenticated)
    {
        g_free (greeter->priv->authentication_user);
        greeter->priv->authentication_user = NULL;
    }
    g_signal_emit (G_OBJECT (greeter), signals[AUTHENTICATION_COMPLETE], 0);
    greeter->priv->in_authentication = FALSE;
}

static void
handle_session_failed (LdmGreeter *greeter, gsize *offset)
{ 
    g_debug ("Session failed to start");
    g_signal_emit (G_OBJECT (greeter), signals[SESSION_FAILED], 0);
}

static void
handle_quit (LdmGreeter *greeter, gsize *offset)
{
    g_debug ("Got quit request from server");
    g_signal_emit (G_OBJECT (greeter), signals[QUIT], 0);
}

static gboolean
read_packet (LdmGreeter *greeter, gboolean block)
{
    gsize n_to_read, n_read;
    GError *error = NULL;

    /* Read the header, or the whole packet if we already have that */
    n_to_read = HEADER_SIZE;
    if (greeter->priv->n_read >= HEADER_SIZE)
        n_to_read += get_packet_length (greeter);

    do
    {
        GIOStatus status;
        status = g_io_channel_read_chars (greeter->priv->from_server_channel,
                                          (gchar *) greeter->priv->read_buffer + greeter->priv->n_read,
                                          n_to_read - greeter->priv->n_read,
                                          &n_read,
                                          &error);
        if (status == G_IO_STATUS_ERROR)
            g_warning ("Error reading from server: %s", error->message);
        g_clear_error (&error);
        if (status != G_IO_STATUS_NORMAL)
            break;

        g_debug ("Read %zi bytes from daemon", n_read);

        greeter->priv->n_read += n_read;
    } while (greeter->priv->n_read < n_to_read && block);

    /* Stop if haven't got all the data we want */
    if (greeter->priv->n_read != n_to_read)
        return FALSE;

    /* If have header, rerun for content */
    if (greeter->priv->n_read == HEADER_SIZE)
    {
        n_to_read = get_packet_length (greeter);
        if (n_to_read > 0)
        {
            greeter->priv->read_buffer = g_realloc (greeter->priv->read_buffer, HEADER_SIZE + n_to_read);
            return read_packet (greeter, block);
        }
    }

    return TRUE;
}

static gboolean
from_server_cb (GIOChannel *source, GIOCondition condition, gpointer data)
{
    LdmGreeter *greeter = data;
    gsize offset;
    guint32 id, length;

    if (!read_packet (greeter, FALSE))
        return TRUE;

    offset = 0;
    id = read_int (greeter, &offset);
    length = read_int (greeter, &offset);
    switch (id)
    {
    case SERVER_MESSAGE_CONNECTED:
        handle_connected (greeter, length, &offset);
        break;
    case SERVER_MESSAGE_PROMPT_AUTHENTICATION:
        handle_prompt_authentication (greeter, &offset);
        break;
    case SERVER_MESSAGE_END_AUTHENTICATION:
        handle_end_authentication (greeter, &offset);
        break;
    case SERVER_MESSAGE_SESSION_FAILED:
        handle_session_failed (greeter, &offset);
        break;
    case SERVER_MESSAGE_QUIT:
        handle_quit (greeter, &offset);
        break;
    default:
        g_warning ("Unknown message from server: %d", id);
        break;
    }

    greeter->priv->n_read = 0;

    return TRUE;
}

/**
 * ldm_greeter_connect_to_server:
 * @greeter: The greeter to connect
 *
 * Connects the greeter to the display manager.
 *
 * Return value: TRUE if successfully connected
 **/
gboolean
ldm_greeter_connect_to_server (LdmGreeter *greeter)
{
    GError *error = NULL;
    const gchar *bus_address, *fd;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;
    GBusType bus_type = G_BUS_TYPE_SYSTEM;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);

    greeter->priv->system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!greeter->priv->system_bus)
        g_warning ("Failed to connect to system bus: %s", error->message);
    g_clear_error (&error);

    bus_address = getenv ("LDM_BUS");
    if (bus_address && strcmp (bus_address, "SESSION") == 0)
        bus_type = G_BUS_TYPE_SESSION;

    greeter->priv->lightdm_bus = g_bus_get_sync (bus_type, NULL, &error);
    if (!greeter->priv->lightdm_bus)
        g_warning ("Failed to connect to LightDM bus: %s", error->message);
    g_clear_error (&error);
    if (!greeter->priv->lightdm_bus)
        return FALSE;

    fd = getenv ("LIGHTDM_TO_SERVER_FD");
    if (!fd)
    {
        g_warning ("No LIGHTDM_TO_SERVER_FD environment variable");
        return FALSE;
    }
    greeter->priv->to_server_channel = g_io_channel_unix_new (atoi (fd));
    g_io_channel_set_encoding (greeter->priv->to_server_channel, NULL, NULL);

    fd = getenv ("LIGHTDM_FROM_SERVER_FD");
    if (!fd)
    {
        g_warning ("No LIGHTDM_FROM_SERVER_FD environment variable");
        return FALSE;
    }
    greeter->priv->from_server_channel = g_io_channel_unix_new (atoi (fd));
    g_io_channel_set_encoding (greeter->priv->from_server_channel, NULL, NULL);
    g_io_add_watch (greeter->priv->from_server_channel, G_IO_IN, from_server_cb, greeter);

    g_debug ("Connecting to display manager...");
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CONNECT, string_length (VERSION), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, VERSION, &offset);
    write_message (greeter, message, offset);

    return TRUE;
}

/**
 * ldm_greeter_get_hostname:
 * @greeter: a #LdmGreeter
 *
 * Return value: The host this greeter is displaying
 **/
const gchar *
ldm_greeter_get_hostname (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);

    if (!greeter->priv->hostname)
    {
        struct utsname info;
        uname (&info);
        greeter->priv->hostname = g_strdup (info.nodename);
    }

    return greeter->priv->hostname;
}

static LdmUser *
get_user_by_name (LdmGreeter *greeter, const gchar *username)
{
    GList *link;
  
    for (link = greeter->priv->users; link; link = link->next)
    {
        LdmUser *user = link->data;
        if (strcmp (ldm_user_get_name (user), username) == 0)
            return user;
    }

    return NULL;
}
  
static gint
compare_user (gconstpointer a, gconstpointer b)
{
    LdmUser *user_a = (LdmUser *) a, *user_b = (LdmUser *) b;
    return strcmp (ldm_user_get_display_name (user_a), ldm_user_get_display_name (user_b));
}

static void
load_users (LdmGreeter *greeter)
{
    GKeyFile *config;
    gchar *value;
    gint minimum_uid;
    gchar **hidden_users, **hidden_shells;
    GList *users = NULL, *old_users, *new_users = NULL, *changed_users = NULL, *link;
    GError *error = NULL;

    g_debug ("Loading user config from %s", USER_CONFIG_FILE);

    config = g_key_file_new ();
    if (!g_key_file_load_from_file (config, USER_CONFIG_FILE, G_KEY_FILE_NONE, &error) &&
        !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Failed to load configuration from %s: %s", USER_CONFIG_FILE, error->message); // FIXME: Don't make warning on no file, just info
    g_clear_error (&error);

    if (g_key_file_has_key (config, "UserAccounts", "minimum-uid", NULL))
        minimum_uid = g_key_file_get_integer (config, "UserAccounts", "minimum-uid", NULL);
    else
        minimum_uid = 500;

    value = g_key_file_get_string (config, "UserAccounts", "hidden-users", NULL);
    if (!value)
        value = g_strdup ("nobody nobody4 noaccess");
    hidden_users = g_strsplit (value, " ", -1);
    g_free (value);

    value = g_key_file_get_string (config, "UserAccounts", "hidden-shells", NULL);
    if (!value)
        value = g_strdup ("/bin/false /usr/sbin/nologin");
    hidden_shells = g_strsplit (value, " ", -1);
    g_free (value);

    g_key_file_free (config);

    setpwent ();

    while (TRUE)
    {
        struct passwd *entry;
        LdmUser *user;
        char **tokens;
        gchar *real_name, *image_path, *image;
        int i;

        errno = 0;
        entry = getpwent ();
        if (!entry)
            break;

        /* Ignore system users */
        if (entry->pw_uid < minimum_uid)
            continue;

        /* Ignore users disabled by shell */
        if (entry->pw_shell)
        {
            for (i = 0; hidden_shells[i] && strcmp (entry->pw_shell, hidden_shells[i]) != 0; i++);
            if (hidden_shells[i])
                continue;
        }

        /* Ignore certain users */
        for (i = 0; hidden_users[i] && strcmp (entry->pw_name, hidden_users[i]) != 0; i++);
        if (hidden_users[i])
            continue;

        tokens = g_strsplit (entry->pw_gecos, ",", -1);
        if (tokens[0] != NULL && tokens[0][0] != '\0')
            real_name = g_strdup (tokens[0]);
        else
            real_name = NULL;
        g_strfreev (tokens);
      
        image_path = g_build_filename (entry->pw_dir, ".face", NULL);
        if (!g_file_test (image_path, G_FILE_TEST_EXISTS))
        {
            g_free (image_path);
            image_path = g_build_filename (entry->pw_dir, ".face.icon", NULL);
            if (!g_file_test (image_path, G_FILE_TEST_EXISTS))
            {
                g_free (image_path);
                image_path = NULL;
            }
        }
        if (image_path)
            image = g_filename_to_uri (image_path, NULL, NULL);
        else
            image = NULL;
        g_free (image_path);

        user = ldm_user_new (greeter, entry->pw_name, real_name, entry->pw_dir, image, FALSE);
        g_free (real_name);
        g_free (image);

        /* Update existing users if have them */
        for (link = greeter->priv->users; link; link = link->next)
        {
            LdmUser *info = link->data;
            if (strcmp (ldm_user_get_name (info), ldm_user_get_name (user)) == 0)
            {
                if (ldm_user_update (info, ldm_user_get_real_name (user), ldm_user_get_home_directory (user), ldm_user_get_image (user), ldm_user_get_logged_in (user)))
                    changed_users = g_list_insert_sorted (changed_users, info, compare_user);
                g_object_unref (user);
                user = info;
                break;
            }
        }
        if (!link)
        {
            /* Only notify once we have loaded the user list */
            if (greeter->priv->have_users)
                new_users = g_list_insert_sorted (new_users, user, compare_user);
        }
        users = g_list_insert_sorted (users, user, compare_user);
    }
    g_strfreev (hidden_users);
    g_strfreev (hidden_shells);

    if (errno != 0)
        g_warning ("Failed to read password database: %s", strerror (errno));

    endpwent ();

    /* Use new user list */
    old_users = greeter->priv->users;
    greeter->priv->users = users;
  
    /* Notify of changes */
    for (link = new_users; link; link = link->next)
    {
        LdmUser *info = link->data;
        g_debug ("User %s added", ldm_user_get_name (info));
        g_signal_emit (greeter, signals[USER_ADDED], 0, info);
    }
    g_list_free (new_users);
    for (link = changed_users; link; link = link->next)
    {
        LdmUser *info = link->data;
        g_debug ("User %s changed", ldm_user_get_name (info));
        g_signal_emit (greeter, signals[USER_CHANGED], 0, info);
    }
    g_list_free (changed_users);
    for (link = old_users; link; link = link->next)
    {
        GList *new_link;

        /* See if this user is in the current list */
        for (new_link = greeter->priv->users; new_link; new_link = new_link->next)
        {
            if (new_link->data == link->data)
                break;
        }

        if (!new_link)
        {
            LdmUser *info = link->data;
            g_debug ("User %s removed", ldm_user_get_name (info));
            g_signal_emit (greeter, signals[USER_REMOVED], 0, info);
            g_object_unref (info);
        }
    }
    g_list_free (old_users);
}

static void
passwd_changed_cb (GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, LdmGreeter *greeter)
{
    if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    {
        g_debug ("%s changed, reloading user list", g_file_get_path (file));
        load_users (greeter);
    }
}

static void
update_users (LdmGreeter *greeter)
{
    GFile *passwd_file;
    GError *error = NULL;

    if (greeter->priv->have_users)
        return;

    load_users (greeter);

    /* Watch for changes to user list */
    passwd_file = g_file_new_for_path (PASSWD_FILE);
    greeter->priv->passwd_monitor = g_file_monitor (passwd_file, G_FILE_MONITOR_NONE, NULL, &error);
    g_object_unref (passwd_file);
    if (!greeter->priv->passwd_monitor)
        g_warning ("Error monitoring %s: %s", PASSWD_FILE, error->message);
    else
        g_signal_connect (greeter->priv->passwd_monitor, "changed", G_CALLBACK (passwd_changed_cb), greeter);
    g_clear_error (&error);

    greeter->priv->have_users = TRUE;
}

/**
 * ldm_greeter_get_num_users:
 * @greeter: a #LdmGreeter
 *
 * Return value: The number of users able to log in
 **/
gint
ldm_greeter_get_num_users (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), 0);
    update_users (greeter);
    return g_list_length (greeter->priv->users);
}

/**
 * ldm_greeter_get_users:
 * @greeter: A #LdmGreeter
 *
 * Get a list of users to present to the user.  This list may be a subset of the
 * available users and may be empty depending on the server configuration.
 *
 * Return value: (element-type LdmUser) (transfer none): A list of #LdmUser that should be presented to the user.
 **/
GList *
ldm_greeter_get_users (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    update_users (greeter);
    return greeter->priv->users;
}

/**
 * ldm_greeter_get_user_by_name:
 * @greeter: A #LdmGreeter
 * @username: Name of user to get.
 *
 * Get infomation about a given user or NULL if this user doesn't exist.
 *
 * Return value: (transfer none): A #LdmUser entry for the given user.
 **/
LdmUser *
ldm_greeter_get_user_by_name (LdmGreeter *greeter, const gchar *username)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    g_return_val_if_fail (username != NULL, NULL);

    update_users (greeter);

    return get_user_by_name (greeter, username);
}

static void
update_languages (LdmGreeter *greeter)
{
    gchar *stdout_text = NULL, *stderr_text = NULL;
    gint exit_status;
    gboolean result;
    GError *error = NULL;

    if (greeter->priv->have_languages)
        return;

    result = g_spawn_command_line_sync ("locale -a", &stdout_text, &stderr_text, &exit_status, &error);
    if (!result || exit_status != 0)
        g_warning ("Failed to get languages, locale -a returned %d: %s", exit_status, error->message);
    else
    {
        gchar **tokens;
        int i;

        tokens = g_strsplit_set (stdout_text, "\n\r", -1);
        for (i = 0; tokens[i]; i++)
        {
            LdmLanguage *language;
            gchar *code;

            code = g_strchug (tokens[i]);
            if (code[0] == '\0')
                continue;

            /* Ignore the non-interesting languages */
            if (strcmp (code, "C") == 0 || strcmp (code, "POSIX") == 0)
                continue;

            language = ldm_language_new (code);
            greeter->priv->languages = g_list_append (greeter->priv->languages, language);
        }

        g_strfreev (tokens);
    }

    g_clear_error (&error);
    g_free (stdout_text);
    g_free (stderr_text);

    greeter->priv->have_languages = TRUE;
}

/**
 * ldm_greeter_get_default_language:
 * @greeter: A #LdmGreeter
 *
 * Get the default language.
 *
 * Return value: The default language.
 **/
const gchar *
ldm_greeter_get_default_language (LdmGreeter *greeter)
{
    gchar *lang;
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    lang = getenv ("LANG");
    if (lang)
        return lang;
    else
        return "C";
}

/**
 * ldm_greeter_get_languages:
 * @greeter: A #LdmGreeter
 *
 * Get a list of languages to present to the user.
 *
 * Return value: (element-type LdmLanguage) (transfer none): A list of #LdmLanguage that should be presented to the user.
 **/
GList *
ldm_greeter_get_languages (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    update_languages (greeter);
    return greeter->priv->languages;
}

static void
layout_cb (XklConfigRegistry *config,
           const XklConfigItem *item,
           gpointer data)
{
    LdmGreeter *greeter = data;
    LdmLayout *layout;

    layout = ldm_layout_new (item->name, item->short_description, item->description);
    greeter->priv->layouts = g_list_append (greeter->priv->layouts, layout);
}

static void
setup_display (LdmGreeter *greeter)
{
    if (!greeter->priv->display)
        greeter->priv->display = XOpenDisplay (NULL);
}

static void
setup_xkl (LdmGreeter *greeter)
{
    setup_display (greeter);
    greeter->priv->xkl_engine = xkl_engine_get_instance (greeter->priv->display);
    greeter->priv->xkl_config = xkl_config_rec_new ();
    if (!xkl_config_rec_get_from_server (greeter->priv->xkl_config, greeter->priv->xkl_engine))
        g_warning ("Failed to get Xkl configuration from server");
    greeter->priv->layout = g_strdup (greeter->priv->xkl_config->layouts[0]);
}

/**
 * ldm_greeter_get_layouts:
 * @greeter: A #LdmGreeter
 *
 * Get a list of keyboard layouts to present to the user.
 *
 * Return value: (element-type LdmLayout) (transfer none): A list of #LdmLayout that should be presented to the user.
 **/
GList *
ldm_greeter_get_layouts (LdmGreeter *greeter)
{
    XklConfigRegistry *registry;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);

    if (greeter->priv->have_layouts)
        return greeter->priv->layouts;

    setup_xkl (greeter);

    registry = xkl_config_registry_get_instance (greeter->priv->xkl_engine);
    xkl_config_registry_load (registry, FALSE);
    xkl_config_registry_foreach_layout (registry, layout_cb, greeter);
    g_object_unref (registry);
    greeter->priv->have_layouts = TRUE;

    return greeter->priv->layouts;
}

/**
 * ldm_greeter_set_layout:
 * @greeter: A #LdmGreeter
 * @layout: The layout to use
 *
 * Set the layout for this session.
 **/
void
ldm_greeter_set_layout (LdmGreeter *greeter, const gchar *layout)
{
    XklConfigRec *config;

    g_return_if_fail (LDM_IS_GREETER (greeter));
    g_return_if_fail (layout != NULL);

    g_debug ("Setting keyboard layout to %s", layout);

    setup_xkl (greeter);

    config = xkl_config_rec_new ();
    config->layouts = g_malloc (sizeof (gchar *) * 2);
    config->model = g_strdup (greeter->priv->xkl_config->model);
    config->layouts[0] = g_strdup (layout);
    config->layouts[1] = NULL;
    if (!xkl_config_rec_activate (config, greeter->priv->xkl_engine))
        g_warning ("Failed to activate XKL config");
    else
        greeter->priv->layout = g_strdup (layout);
    g_object_unref (config);
}

/**
 * ldm_greeter_get_layout:
 * @greeter: A #LdmGreeter
 *
 * Get the current keyboard layout.
 *
 * Return value: The currently active layout for this user.
 **/
const gchar *
ldm_greeter_get_layout (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    setup_xkl (greeter);
    return greeter->priv->layout;
}

static void
update_sessions (LdmGreeter *greeter)
{
    GDir *directory;
    GError *error = NULL;

    if (greeter->priv->have_sessions)
        return;

    directory = g_dir_open (XSESSIONS_DIR, 0, &error);
    if (!directory)
        g_warning ("Failed to open sessions directory: %s", error->message);
    g_clear_error (&error);
    if (!directory)
        return;

    while (TRUE)
    {
        const gchar *filename;
        GKeyFile *key_file;
        gchar *key, *path;
        gboolean result;

        filename = g_dir_read_name (directory);
        if (filename == NULL)
            break;

        if (!g_str_has_suffix (filename, ".desktop"))
            continue;

        key = g_strndup (filename, strlen (filename) - strlen (".desktop"));
        path = g_build_filename (XSESSIONS_DIR, filename, NULL);
        g_debug ("Loading session %s", path);

        key_file = g_key_file_new ();
        result = g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error);
        if (!result)
            g_warning ("Failed to load session file %s: %s:", path, error->message);
        g_clear_error (&error);

        if (result && !g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL))
        {
            gchar *domain, *name, *comment;

#ifdef G_KEY_FILE_DESKTOP_KEY_GETTEXT_DOMAIN
            domain = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_GETTEXT_DOMAIN, NULL);
#else
            domain = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, "X-GNOME-Gettext-Domain", NULL);
#endif
            name = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, domain, NULL);
            comment = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, domain, NULL);
            if (!comment)
                comment = g_strdup ("");
            if (name)
            {
                g_debug ("Loaded session %s (%s, %s)", key, name, comment);
                greeter->priv->sessions = g_list_append (greeter->priv->sessions, ldm_session_new (key, name, comment));
            }
            else
                g_warning ("Invalid session %s: %s", path, error->message);
            g_free (domain);
            g_free (name);
            g_free (comment);
        }

        g_free (key);
        g_free (path);
        g_key_file_free (key_file);
    }

    g_dir_close (directory);

    greeter->priv->have_sessions = TRUE;
}

/**
 * ldm_greeter_get_sessions:
 * @greeter: A #LdmGreeter
 *
 * Get the available sessions.
 *
 * Return value: (element-type LdmSession) (transfer none): A list of #LdmSession
 **/
GList *
ldm_greeter_get_sessions (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    update_sessions (greeter);
    return greeter->priv->sessions;
}

/**
 * ldm_greeter_get_hint:
 * @greeter: A #LdmGreeter
 * @name: The hint name to query.
 *
 * Get a hint.
 *
 * Return value: The value for this hint or NULL if not set.
 **/
const gchar *
ldm_greeter_get_hint (LdmGreeter *greeter, const gchar *name)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    return g_hash_table_lookup (greeter->priv->hints, name);
}

/**
 * ldm_greeter_get_default_session_hint:
 * @greeter: A #LdmGreeter
 *
 * Get the default session to use.
 *
 * Return value: The session name
 **/
const gchar *
ldm_greeter_get_default_session_hint (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    return ldm_greeter_get_hint (greeter, "default-session");
}

/**
 * ldm_greeter_get_hide_users_hint:
 * @greeter: A #LdmGreeter
 *
 * Check if user accounts should be shown.
 *
 * Return value: TRUE if the available users should not be shown.
 */
gboolean
ldm_greeter_get_hide_users_hint (LdmGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    value = ldm_greeter_get_hint (greeter, "hide-users");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * ldm_greeter_get_has_guest_account_hint:
 * @greeter: A #LdmGreeter
 *
 * Check if guest sessions are supported.
 *
 * Return value: TRUE if guest sessions are supported.
 */
gboolean
ldm_greeter_get_has_guest_account_hint (LdmGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    value = ldm_greeter_get_hint (greeter, "has-guest-account");
  
    return g_strcmp0 (value, "true") == 0;
}

/**
 * ldm_greeter_get_select_user_hint:
 * @greeter: A #LdmGreeter
 *
 * Get the user to select by default.
 *
 * Return value: A username
 */
const gchar *
ldm_greeter_get_select_user_hint (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    return ldm_greeter_get_hint (greeter, "select-user");
}

/**
 * ldm_greeter_get_select_guest_hint:
 * @greeter: A #LdmGreeter
 *
 * Check if the guest account should be selected by default.
 *
 * Return value: TRUE if the guest account should be selected by default.
 */
gboolean
ldm_greeter_get_select_guest_hint (LdmGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    value = ldm_greeter_get_hint (greeter, "select-guest");
  
    return g_strcmp0 (value, "true") == 0;
}

/**
 * ldm_greeter_get_autologin_user_hint:
 * @greeter: A #LdmGreeter
 *
 * Get the user account to automatically logg into when the timer expires.
 *
 * Return value: The user account to automatically log into.
 */
const gchar *
ldm_greeter_get_autologin_user_hint (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    return ldm_greeter_get_hint (greeter, "autologin-user");
}

/**
 * ldm_greeter_get_autologin_guest_hint:
 * @greeter: A #LdmGreeter
 *
 * Check if the guest account should be automatically logged into when the timer expires.
 *
 * Return value: TRUE if the guest account should be automatically logged into.
 */
gboolean
ldm_greeter_get_autologin_guest_hint (LdmGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    value = ldm_greeter_get_hint (greeter, "autologin-guest");
  
    return g_strcmp0 (value, "true") == 0;
}

/**
 * ldm_greeter_get_autologin_timeout_hint:
 * @greeter: A #LdmGreeter
 *
 * Get the number of seconds to wait before automaitcally logging in.
 *
 * Return value: The number of seconds to wait before automatically logging in or 0 for no timeout.
 */
gint
ldm_greeter_get_autologin_timeout_hint (LdmGreeter *greeter)
{
    const gchar *value;
    gint timeout = 0;

    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    value = ldm_greeter_get_hint (greeter, "autologin-timeout");
    if (value)
        timeout = atoi (value);
    if (timeout < 0)
        timeout = 0;

    return timeout;
}

/**
 * ldm_greeter_cancel_timed_login:
 * @greeter: A #LdmGreeter
 *
 * Cancel the login as the default user.
 */
void
ldm_greeter_cancel_timed_login (LdmGreeter *greeter)
{
    g_return_if_fail (LDM_IS_GREETER (greeter));

    if (greeter->priv->login_timeout)
       g_source_remove (greeter->priv->login_timeout);
    greeter->priv->login_timeout = 0;
}

/**
 * ldm_greeter_login:
 * @greeter: A #LdmGreeter
 * @username: (allow-none): A username or NULL to prompt for a username.
 *
 * Starts the authentication procedure for a user.
 **/
void
ldm_greeter_login (LdmGreeter *greeter, const char *username)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LDM_IS_GREETER (greeter));

    if (!username)
        username = "";

    greeter->priv->cancelling_authentication = FALSE;
    greeter->priv->authenticate_sequence_number++;
    greeter->priv->in_authentication = TRUE;  
    greeter->priv->is_authenticated = FALSE;
    g_free (greeter->priv->authentication_user);
    greeter->priv->authentication_user = g_strdup (username);

    g_debug ("Starting authentication for user %s...", username);
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_LOGIN, int_length () + string_length (username), &offset);
    write_int (message, MAX_MESSAGE_LENGTH, greeter->priv->authenticate_sequence_number, &offset);
    write_string (message, MAX_MESSAGE_LENGTH, username, &offset);
    write_message (greeter, message, offset);
}

/**
 * ldm_greeter_login_with_user_prompt:
 * @greeter: A #LdmGreeter
 *
 * Starts the authentication procedure, prompting the greeter for a username.
 **/
void
ldm_greeter_login_with_user_prompt (LdmGreeter *greeter)
{
    ldm_greeter_login (greeter, NULL);
}

/**
 * ldm_greeter_login_as_guest:
 * @greeter: A #LdmGreeter
 *
 * Starts the authentication procedure for the guest user.
 **/
void
ldm_greeter_login_as_guest (LdmGreeter *greeter)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LDM_IS_GREETER (greeter));

    greeter->priv->cancelling_authentication = FALSE;
    greeter->priv->authenticate_sequence_number++;
    greeter->priv->in_authentication = TRUE;
    greeter->priv->is_authenticated = FALSE;
    g_free (greeter->priv->authentication_user);
    greeter->priv->authentication_user = NULL;

    g_debug ("Starting authentication for guest account...");
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_LOGIN_AS_GUEST, int_length (), &offset);
    write_int (message, MAX_MESSAGE_LENGTH, greeter->priv->authenticate_sequence_number, &offset);
    write_message (greeter, message, offset);
}

/**
 * ldm_greeter_respond:
 * @greeter: A #LdmGreeter
 * @response: Response to a prompt
 *
 * Provide response to a prompt.
 **/
void
ldm_greeter_respond (LdmGreeter *greeter, const gchar *response)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LDM_IS_GREETER (greeter));
    g_return_if_fail (response != NULL);

    g_debug ("Providing response to display manager");
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CONTINUE_AUTHENTICATION, int_length () + string_length (response), &offset);
    // FIXME: Could be multiple responses required
    write_int (message, MAX_MESSAGE_LENGTH, 1, &offset);
    write_string (message, MAX_MESSAGE_LENGTH, response, &offset);
    write_message (greeter, message, offset);
}

/**
 * ldm_greeter_cancel_authentication:
 * @greeter: A #LdmGreeter
 *
 * Cancel the current user authentication.
 **/
void
ldm_greeter_cancel_authentication (LdmGreeter *greeter)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LDM_IS_GREETER (greeter));

    greeter->priv->cancelling_authentication = TRUE;
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CANCEL_AUTHENTICATION, 0, &offset);
    write_message (greeter, message, offset);
}

/**
 * ldm_greeter_get_in_authentication:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter is in the process of authenticating.
 *
 * Return value: TRUE if the greeter is authenticating a user.
 **/
gboolean
ldm_greeter_get_in_authentication (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return greeter->priv->in_authentication;
}

/**
 * ldm_greeter_get_is_authenticated:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter has successfully authenticated.
 *
 * Return value: TRUE if the greeter is authenticated for login.
 **/
gboolean
ldm_greeter_get_is_authenticated (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return greeter->priv->is_authenticated;
}

/**
 * ldm_greeter_get_authentication_user:
 * @greeter: A #LdmGreeter
 *
 * Get the user that is being authenticated.
 *
 * Return value: The username of the authentication user being authenticated or NULL if no authentication in progress.
 */
const gchar *
ldm_greeter_get_authentication_user (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), NULL);
    return greeter->priv->authentication_user;
}

/**
 * ldm_greeter_start_session:
 * @greeter: A #LdmGreeter
 * @session: (allow-none): The session to log into or NULL to use the default
 *
 * Start a session for the logged in user.
 **/
void
ldm_greeter_start_session (LdmGreeter *greeter, const gchar *session)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LDM_IS_GREETER (greeter));
  
    if (!session)
        session = "";

    g_debug ("Starting session %s", session);
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_START_SESSION, string_length (session), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, session, &offset);
    write_message (greeter, message, offset);
}

/**
 * ldm_greeter_start_session_with_defaults:
 * @greeter: A #LdmGreeter
 *
 * Login a user to a session using default settings for that user.
 **/
void
ldm_greeter_start_default_session (LdmGreeter *greeter)
{
    ldm_greeter_start_session (greeter, NULL);
}

static gboolean
upower_call_function (LdmGreeter *greeter, const gchar *function, gboolean has_result)
{
    GDBusProxy *proxy;
    GVariant *result;
    GError *error = NULL;
    gboolean function_result = FALSE;
  
    if (!greeter->priv->system_bus)
        return FALSE;

    proxy = g_dbus_proxy_new_sync (greeter->priv->system_bus,
                                   G_DBUS_PROXY_FLAGS_NONE,
                                   NULL,
                                   "org.freedesktop.UPower",
                                   "/org/freedesktop/UPower",
                                   "org.freedesktop.UPower",
                                   NULL, NULL);
    result = g_dbus_proxy_call_sync (proxy,
                                     function,
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &error);
    g_object_unref (proxy);

    if (!result)
        g_warning ("Error calling UPower function %s: %s", function, error->message);
    g_clear_error (&error);
    if (!result)
        return FALSE;

    if (g_variant_is_of_type (result, G_VARIANT_TYPE ("(b)")))
        g_variant_get (result, "(b)", &function_result);

    g_variant_unref (result);
    return function_result;
}

/**
 * ldm_greeter_get_can_suspend:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter is authorized to do a system suspend.
 *
 * Return value: TRUE if the greeter can suspend the system
 **/
gboolean
ldm_greeter_get_can_suspend (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return upower_call_function (greeter, "SuspendAllowed", TRUE);
}

/**
 * ldm_greeter_suspend:
 * @greeter: A #LdmGreeter
 *
 * Triggers a system suspend.
 **/
void
ldm_greeter_suspend (LdmGreeter *greeter)
{
    g_return_if_fail (LDM_IS_GREETER (greeter));
    upower_call_function (greeter, "Suspend", FALSE);
}

/**
 * ldm_greeter_get_can_hibernate:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter is authorized to do a system hibernate.
 *
 * Return value: TRUE if the greeter can hibernate the system
 **/
gboolean
ldm_greeter_get_can_hibernate (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return upower_call_function (greeter, "HibernateAllowed", TRUE);
}

/**
 * ldm_greeter_hibernate:
 * @greeter: A #LdmGreeter
 *
 * Triggers a system hibernate.
 **/
void
ldm_greeter_hibernate (LdmGreeter *greeter)
{
    g_return_if_fail (LDM_IS_GREETER (greeter));
    upower_call_function (greeter, "Hibernate", FALSE);
}

static gboolean
ck_call_function (LdmGreeter *greeter, const gchar *function, gboolean has_result)
{
    GDBusProxy *proxy;
    GVariant *result;
    GError *error = NULL;
    gboolean function_result = FALSE;

    if (!greeter->priv->system_bus)
        return FALSE;

    proxy = g_dbus_proxy_new_sync (greeter->priv->system_bus,
                                   G_DBUS_PROXY_FLAGS_NONE,
                                   NULL,
                                   "org.freedesktop.ConsoleKit",
                                   "/org/freedesktop/ConsoleKit/Manager",
                                   "org.freedesktop.ConsoleKit.Manager",
                                   NULL, NULL);
    result = g_dbus_proxy_call_sync (proxy,
                                     function,
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &error);
    g_object_unref (proxy);

    if (!result)
        g_warning ("Error calling ConsoleKit function %s: %s", function, error->message);
    g_clear_error (&error);
    if (!result)
        return FALSE;

    if (g_variant_is_of_type (result, G_VARIANT_TYPE ("(b)")))
        g_variant_get (result, "(b)", &function_result);

    g_variant_unref (result);
    return function_result;
}

/**
 * ldm_greeter_get_can_restart:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter is authorized to do a system restart.
 *
 * Return value: TRUE if the greeter can restart the system
 **/
gboolean
ldm_greeter_get_can_restart (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return ck_call_function (greeter, "CanRestart", TRUE);
}

/**
 * ldm_greeter_restart:
 * @greeter: A #LdmGreeter
 *
 * Triggers a system restart.
 **/
void
ldm_greeter_restart (LdmGreeter *greeter)
{
    g_return_if_fail (LDM_IS_GREETER (greeter));
    ck_call_function (greeter, "Restart", FALSE);
}

/**
 * ldm_greeter_get_can_shutdown:
 * @greeter: A #LdmGreeter
 *
 * Checks if the greeter is authorized to do a system shutdown.
 *
 * Return value: TRUE if the greeter can shutdown the system
 **/
gboolean
ldm_greeter_get_can_shutdown (LdmGreeter *greeter)
{
    g_return_val_if_fail (LDM_IS_GREETER (greeter), FALSE);
    return ck_call_function (greeter, "CanStop", TRUE);
}

/**
 * ldm_greeter_shutdown:
 * @greeter: A #LdmGreeter
 *
 * Triggers a system shutdown.
 **/
void
ldm_greeter_shutdown (LdmGreeter *greeter)
{
    g_return_if_fail (LDM_IS_GREETER (greeter));
    ck_call_function (greeter, "Stop", FALSE);
}

static void
ldm_greeter_init (LdmGreeter *greeter)
{
    greeter->priv = G_TYPE_INSTANCE_GET_PRIVATE (greeter, LDM_TYPE_GREETER, LdmGreeterPrivate);
    greeter->priv->read_buffer = g_malloc (HEADER_SIZE);
    greeter->priv->hints = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    g_debug ("default-language=%s", ldm_greeter_get_default_language (greeter));
}

static void
ldm_greeter_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    LdmGreeter *self;

    self = LDM_GREETER (object);

    switch (prop_id) {
    case PROP_LAYOUT:
        ldm_greeter_set_layout(self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
ldm_greeter_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    LdmGreeter *self;

    self = LDM_GREETER (object);

    switch (prop_id) {
    case PROP_HOSTNAME:
        g_value_set_string (value, ldm_greeter_get_hostname (self));
        break;
    case PROP_NUM_USERS:
        g_value_set_int (value, ldm_greeter_get_num_users (self));
        break;
    case PROP_USERS:
        break;
    case PROP_DEFAULT_LANGUAGE:
        g_value_set_string (value, ldm_greeter_get_default_language (self));
        break;
    case PROP_LAYOUTS:
        break;
    case PROP_LAYOUT:
        g_value_set_string (value, ldm_greeter_get_layout (self));
        break;
    case PROP_SESSIONS:
        break;
    case PROP_DEFAULT_SESSION_HINT:
        g_value_set_string (value, ldm_greeter_get_default_session_hint (self));
        break;
    case PROP_HIDE_USERS_HINT:
        g_value_set_boolean (value, ldm_greeter_get_hide_users_hint (self));
        break;
    case PROP_HAS_GUEST_ACCOUNT_HINT:
        g_value_set_boolean (value, ldm_greeter_get_has_guest_account_hint (self));
        break;
    case PROP_SELECT_USER_HINT:
        g_value_set_string (value, ldm_greeter_get_select_user_hint (self));
        break;
    case PROP_SELECT_GUEST_HINT:
        g_value_set_boolean (value, ldm_greeter_get_select_guest_hint (self));
        break;
    case PROP_AUTOLOGIN_USER_HINT:
        g_value_set_string (value, ldm_greeter_get_autologin_user_hint (self));
        break;
    case PROP_AUTOLOGIN_GUEST_HINT:
        g_value_set_boolean (value, ldm_greeter_get_autologin_guest_hint (self));
        break;
    case PROP_AUTOLOGIN_TIMEOUT_HINT:
        g_value_set_int (value, ldm_greeter_get_autologin_timeout_hint (self));
        break;
    case PROP_AUTHENTICATION_USER:
        g_value_set_string (value, ldm_greeter_get_authentication_user (self));
        break;
    case PROP_IN_AUTHENTICATION:
        g_value_set_boolean (value, ldm_greeter_get_in_authentication (self));
        break;
    case PROP_IS_AUTHENTICATED:
        g_value_set_boolean (value, ldm_greeter_get_is_authenticated (self));
        break;
    case PROP_CAN_SUSPEND:
        g_value_set_boolean (value, ldm_greeter_get_can_suspend (self));
        break;
    case PROP_CAN_HIBERNATE:
        g_value_set_boolean (value, ldm_greeter_get_can_hibernate (self));
        break;
    case PROP_CAN_RESTART:
        g_value_set_boolean (value, ldm_greeter_get_can_restart (self));
        break;
    case PROP_CAN_SHUTDOWN:
        g_value_set_boolean (value, ldm_greeter_get_can_shutdown (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
marshal_VOID__STRING_INT (GClosure     *closure,
                          GValue       *return_value G_GNUC_UNUSED,
                          guint         n_param_values,
                          const GValue *param_values,
                          gpointer      invocation_hint G_GNUC_UNUSED,
                          gpointer      marshal_data)
{
    typedef void (*GMarshalFunc_VOID__STRING_INT) (gpointer     data1,
                                                   gpointer     arg_1,
                                                   gint         arg_2,
                                                   gpointer     data2);
    register GMarshalFunc_VOID__STRING_INT callback;
    register GCClosure *cc = (GCClosure*) closure;
    register gpointer data1, data2;

    g_return_if_fail (n_param_values == 3);

    if (G_CCLOSURE_SWAP_DATA (closure))
    {
        data1 = closure->data;
        data2 = g_value_peek_pointer (param_values + 0);
    }
    else
    {
        data1 = g_value_peek_pointer (param_values + 0);
        data2 = closure->data;
    }
    callback = (GMarshalFunc_VOID__STRING_INT) (marshal_data ? marshal_data : cc->callback);

    callback (data1,
              (param_values + 1)->data[0].v_pointer,
              (param_values + 2)->data[0].v_int,
              data2);
}

static void
ldm_greeter_finalize (GObject *object)
{
    LdmGreeter *self = LDM_GREETER (object);

    g_hash_table_unref (self->priv->hints);

    G_OBJECT_CLASS (ldm_greeter_parent_class)->finalize (object);
}

static void
ldm_greeter_class_init (LdmGreeterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (klass, sizeof (LdmGreeterPrivate));

    object_class->set_property = ldm_greeter_set_property;
    object_class->get_property = ldm_greeter_get_property;
    object_class->finalize = ldm_greeter_finalize;

    g_object_class_install_property (object_class,
                                     PROP_NUM_USERS,
                                     g_param_spec_string ("hostname",
                                                          "hostname",
                                                          "Hostname displaying greeter for",
                                                          NULL,
                                                          G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_NUM_USERS,
                                     g_param_spec_int ("num-users",
                                                       "num-users",
                                                       "Number of login users",
                                                       0, G_MAXINT, 0,
                                                       G_PARAM_READABLE));
    /*g_object_class_install_property (object_class,
                                     PROP_USERS,
                                     g_param_spec_list ("users",
                                                        "users",
                                                        "Users that can login"));
    g_object_class_install_property (object_class,
                                     PROP_DEFAULT_LANGUAGE,
                                     g_param_spec_string ("default-language",
                                                          "default-language",
                                                          "Default language",
                                                          NULL,
                                                          G_PARAM_READWRITE));
    g_object_class_install_property (object_class,
                                     PROP_LAYOUTS,
                                     g_param_spec_list ("layouts",
                                                        "layouts",
                                                        "Available keyboard layouts"));*/
    g_object_class_install_property (object_class,
                                     PROP_LAYOUT,
                                     g_param_spec_string ("layout",
                                                          "layout",
                                                          "Current keyboard layout",
                                                          NULL,
                                                          G_PARAM_READWRITE));
    /*g_object_class_install_property (object_class,
                                     PROP_SESSIONS,
                                     g_param_spec_list ("sessions",
                                                        "sessions",
                                                        "Available sessions"));*/
    g_object_class_install_property (object_class,
                                     PROP_DEFAULT_SESSION_HINT,
                                     g_param_spec_string ("default-session-hint",
                                                          "default-session-hint",
                                                          "Default session hint",
                                                          NULL,
                                                          G_PARAM_READWRITE));

    g_object_class_install_property (object_class,
                                     PROP_HIDE_USERS_HINT,
                                     g_param_spec_boolean ("hide-users-hint",
                                                           "hide-users-hint",
                                                           "hide users hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_HAS_GUEST_ACCOUNT_HINT,
                                     g_param_spec_boolean ("has-guest-account-hint",
                                                           "has-guest-account-hint",
                                                           "Has guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SELECT_USER_HINT,
                                     g_param_spec_string ("select-user-hint",
                                                          "select-user-hint",
                                                          "Select user hint",
                                                          NULL,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SELECT_GUEST_HINT,
                                     g_param_spec_boolean ("select-guest-hint",
                                                           "select-guest-hint",
                                                           "Select guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_USER_HINT,
                                     g_param_spec_string ("autologin-user-hint",
                                                          "autologin-user-hint",
                                                          "Autologin user hint",
                                                          NULL,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_GUEST_HINT,
                                     g_param_spec_boolean ("autologin-guest-hint",
                                                           "autologin-guest-hint",
                                                           "Autologin guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_TIMEOUT_HINT,
                                     g_param_spec_int ("autologin-timeout-hint",
                                                       "autologin-timeout-hint",
                                                       "Autologin timeout hint",
                                                       0, G_MAXINT, 0,
                                                       G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTHENTICATION_USER,
                                     g_param_spec_string ("authentication-user",
                                                          "authentication-user",
                                                          "The user being authenticated",
                                                          NULL,
                                                          G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_IN_AUTHENTICATION,
                                     g_param_spec_boolean ("in-authentication",
                                                           "in-authentication",
                                                           "TRUE if a user is being authenticated",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_IS_AUTHENTICATED,
                                     g_param_spec_boolean ("is-authenticated",
                                                           "is-authenticated",
                                                           "TRUE if the selected user is authenticated",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_CAN_SUSPEND,
                                     g_param_spec_boolean ("can-suspend",
                                                           "can-suspend",
                                                           "TRUE if allowed to suspend the system",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_CAN_HIBERNATE,
                                     g_param_spec_boolean ("can-hibernate",
                                                           "can-hibernate",
                                                           "TRUE if allowed to hibernate the system",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_CAN_RESTART,
                                     g_param_spec_boolean ("can-restart",
                                                           "can-restart",
                                                           "TRUE if allowed to restart the system",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_CAN_SHUTDOWN,
                                     g_param_spec_boolean ("can-shutdown",
                                                           "can-shutdown",
                                                           "TRUE if allowed to shutdown the system",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    /**
     * LdmGreeter::connected:
     * @greeter: A #LdmGreeter
     *
     * The ::connected signal gets emitted when the greeter connects to the
     * LightDM server.
     **/
    signals[CONNECTED] =
        g_signal_new ("connected",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, connected),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    /**
     * LdmGreeter::show-prompt:
     * @greeter: A #LdmGreeter
     * @text: Prompt text
     * @type: Prompt type
     *
     * The ::show-prompt signal gets emitted when the greeter should show a
     * prompt to the user.  The given text should be displayed and an input
     * field for the user to provide a response.
     *
     * Call ldm_greeter_respond() with the resultant input or
     * ldm_greeter_cancel_authentication() to abort the authentication.
     **/
    signals[SHOW_PROMPT] =
        g_signal_new ("show-prompt",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, show_prompt),
                      NULL, NULL,
                      marshal_VOID__STRING_INT,
                      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /**
     * LdmGreeter::show-message:
     * @greeter: A #LdmGreeter
     * @text: Message text
     * @type: Message type
     *
     * The ::show-message signal gets emitted when the greeter
     * should show a message to the user.
     **/
    signals[SHOW_MESSAGE] =
        g_signal_new ("show-message",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, show_message),
                      NULL, NULL,
                      marshal_VOID__STRING_INT,
                      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /**
     * LdmGreeter::authentication-complete:
     * @greeter: A #LdmGreeter
     *
     * The ::authentication-complete signal gets emitted when the greeter
     * has completed authentication.
     *
     * Call ldm_greeter_get_is_authenticated() to check if the authentication
     * was successful.
     **/
    signals[AUTHENTICATION_COMPLETE] =
        g_signal_new ("authentication-complete",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, authentication_complete),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    /**
     * LdmGreeter::session-failed:
     * @greeter: A #LdmGreeter
     *
     * The ::session-failed signal gets emitted when the deamon has failed
     * to start the requested session.
     **/
    signals[SESSION_FAILED] =
        g_signal_new ("session-failed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, session_failed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    /**
     * LdmGreeter::autologin-timer-expired:
     * @greeter: A #LdmGreeter
     * @username: A username
     *
     * The ::timed-login signal gets emitted when the automatic login timer has expired.
     * The application should then call ldm_greeter_login().
     **/
    signals[AUTOLOGIN_TIMER_EXPIRED] =
        g_signal_new ("autologin-timer-expired",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, autologin_timer_expired),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    /**
     * LdmGreeter::user-added:
     * @greeter: A #LdmGreeter
     *
     * The ::user-added signal gets emitted when a user account is created.
     **/
    signals[USER_ADDED] =
        g_signal_new ("user-added",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, user_added),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LDM_TYPE_USER);

    /**
     * LdmGreeter::user-changed:
     * @greeter: A #LdmGreeter
     *
     * The ::user-changed signal gets emitted when a user account is modified.
     **/
    signals[USER_CHANGED] =
        g_signal_new ("user-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, user_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LDM_TYPE_USER);

    /**
     * LdmGreeter::user-removed:
     * @greeter: A #LdmGreeter
     *
     * The ::user-removed signal gets emitted when a user account is removed.
     **/
    signals[USER_REMOVED] =
        g_signal_new ("user-removed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, user_removed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, LDM_TYPE_USER);

    /**
     * LdmGreeter::quit:
     * @greeter: A #LdmGreeter
     *
     * The ::quit signal gets emitted when the greeter should exit.
     **/
    signals[QUIT] =
        g_signal_new ("quit",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LdmGreeterClass, quit),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}
