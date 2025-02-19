/* GIO testing utilities
 *
 * Copyright (C) 2008-2010 Red Hat, Inc.
 * Copyright (C) 2012 Collabora Ltd. <http://www.collabora.co.uk/>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: David Zeuthen <davidz@redhat.com>
 *          Xavier Claessens <xavier.claessens@collabora.co.uk>
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <gstdio.h>
#ifdef G_OS_UNIX
#include <unistd.h>
#endif
#ifdef G_OS_WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

#include <glib.h>

#include "gdbusconnection.h"
#include "gdbusprivate.h"
#include "gfile.h"
#include "gioenumtypes.h"
#include "gtestdbus.h"

#include "glibintl.h"

#if defined(G_OS_UNIX) && !defined(G_PLATFORM_WASM)
#include "glib-unix.h"
#endif

/* -------------------------------------------------------------------------- */
/* Utility: Wait until object has a single ref  */

typedef struct
{
  GMainLoop *loop;
  gboolean   timed_out;
} WeakNotifyData;

static gboolean
on_weak_notify_timeout (gpointer user_data)
{
  WeakNotifyData *data = user_data;
  data->timed_out = TRUE;
  g_main_loop_quit (data->loop);
  return FALSE;
}

static gboolean
unref_on_idle (gpointer object)
{
  g_object_unref (object);
  return FALSE;
}

static gboolean
_g_object_unref_and_wait_weak_notify (gpointer object)
{
  WeakNotifyData data;
  guint timeout_id;

  data.loop = g_main_loop_new (NULL, FALSE);
  data.timed_out = FALSE;

  g_object_weak_ref (object, (GWeakNotify) g_main_loop_quit, data.loop);

  /* Drop the strong ref held by the caller in an idle callback. This is to
   * make sure the mainloop is already running when weak notify happens (when
   * all other strong ref holders have dropped theirs). */
  g_idle_add (unref_on_idle, object);

  /* Make sure we don't block forever */
  timeout_id = g_timeout_add (30 * 1000, on_weak_notify_timeout, &data);

  g_main_loop_run (data.loop);

  if (data.timed_out)
    {
      g_warning ("Weak notify timeout, object ref_count=%d",
          G_OBJECT (object)->ref_count);
    }
  else
    {
      g_source_remove (timeout_id);
    }

  g_main_loop_unref (data.loop);
  return data.timed_out;
}

/* -------------------------------------------------------------------------- */
/* Utilities to cleanup the mess in the case unit test process crash */

#ifdef G_OS_WIN32

/* This could be interesting to expose in public API */
static void
_g_test_watcher_add_pid (GPid pid)
{
  static gsize started = 0;
  HANDLE job;

  if (g_once_init_enter (&started))
    {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

      job = CreateJobObjectW (NULL, NULL);
      memset (&info, 0, sizeof (info));
      info.BasicLimitInformation.LimitFlags = 0x2000 /* JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE */;

      if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof (info)))
	g_warning ("Can't enable JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: %s", g_win32_error_message (GetLastError()));

      g_once_init_leave (&started,(gsize)job);
    }

  job = (HANDLE)started;

  if (!AssignProcessToJobObject(job, pid))
    g_warning ("Can't assign process to job: %s", g_win32_error_message (GetLastError()));
}

static void
_g_test_watcher_remove_pid (GPid pid)
{
  /* No need to unassign the process from the job object as the process
     will be killed anyway */
}

#else

#define ADD_PID_FORMAT "add pid %d\n"
#define REMOVE_PID_FORMAT "remove pid %d\n"

static void
watch_parent (gint fd)
{
  GIOChannel *channel;
  GPollFD fds[1];
  GArray *pids_to_kill;

  channel = g_io_channel_unix_new (fd);

  fds[0].fd = fd;
  fds[0].events = G_IO_HUP | G_IO_IN;
  fds[0].revents = 0;

  pids_to_kill = g_array_new (FALSE, FALSE, sizeof (guint));

  do
    {
      gint num_events;
      gchar *command = NULL;
      guint pid;
      guint n;
      GError *error = NULL;

      num_events = g_poll (fds, 1, -1);
      if (num_events == 0)
        continue;

      if (fds[0].revents & G_IO_HUP)
        {
          /* Parent quit, cleanup the mess and exit */
          for (n = 0; n < pids_to_kill->len; n++)
            {
              pid = g_array_index (pids_to_kill, guint, n);
              g_printerr ("cleaning up pid %d\n", pid);
              kill (pid, SIGTERM);
            }

          g_array_unref (pids_to_kill);
          g_io_channel_shutdown (channel, FALSE, &error);
          g_assert_no_error (error);
          g_io_channel_unref (channel);

          exit (0);
        }

      /* Read the command from the input */
      g_io_channel_read_line (channel, &command, NULL, NULL, &error);
      g_assert_no_error (error);

      /* Check for known commands */
      if (sscanf (command, ADD_PID_FORMAT, &pid) == 1)
        {
          g_array_append_val (pids_to_kill, pid);
        }
      else if (sscanf (command, REMOVE_PID_FORMAT, &pid) == 1)
        {
          for (n = 0; n < pids_to_kill->len; n++)
            {
              if (g_array_index (pids_to_kill, guint, n) == pid)
                {
                  g_array_remove_index (pids_to_kill, n);
                  pid = 0;
                  break;
                }
            }
          if (pid != 0)
            {
              g_warning ("unknown pid %d to remove", pid);
            }
        }
      else
        {
          g_warning ("unknown command from parent '%s'", command);
        }

      g_free (command);
    }
  while (TRUE);
}

static GIOChannel *
watcher_init (void)
{
  static gsize started = 0;
  static GIOChannel *channel = NULL;
  int errsv;

  if (g_once_init_enter (&started))
    {
      gint pipe_fds[2];

      /* fork a child to clean up when we are killed */
      if (pipe (pipe_fds) != 0)
        {
          errsv = errno;
          g_warning ("pipe() failed: %s", g_strerror (errsv));
          g_assert_not_reached ();
        }

      /* flush streams to avoid buffers being duplicated in the child and
       * flushed by both the child and parent later
       *
       * FIXME: This is a workaround for the fact that watch_parent() uses
       * non-async-signal-safe API. See
       * https://gitlab.gnome.org/GNOME/glib/-/issues/2322#note_1034330
       */
      fflush (stdout);
      fflush (stderr);

      switch (fork ())
        {
        case -1:
          errsv = errno;
          g_warning ("fork() failed: %s", g_strerror (errsv));
          g_assert_not_reached ();
          break;

        case 0:
          /* child */
          close (pipe_fds[1]);
          watch_parent (pipe_fds[0]);
          break;

        default:
          /* parent */
          close (pipe_fds[0]);
          channel = g_io_channel_unix_new (pipe_fds[1]);
        }

      g_once_init_leave (&started, 1);
    }

  return channel;
}

static void
watcher_send_command (const gchar *command)
{
  GIOChannel *channel;
  GError *error = NULL;
  GIOStatus status;

  channel = watcher_init ();

  do
   status = g_io_channel_write_chars (channel, command, -1, NULL, &error);
  while (status == G_IO_STATUS_AGAIN);
  g_assert_no_error (error);

  g_io_channel_flush (channel, &error);
  g_assert_no_error (error);
}

/* This could be interesting to expose in public API */
static void
_g_test_watcher_add_pid (GPid pid)
{
  gchar *command;

  command = g_strdup_printf (ADD_PID_FORMAT, (guint) pid);
  watcher_send_command (command);
  g_free (command);
}

static void
_g_test_watcher_remove_pid (GPid pid)
{
  gchar *command;

  command = g_strdup_printf (REMOVE_PID_FORMAT, (guint) pid);
  watcher_send_command (command);
  g_free (command);
}

#endif

/* -------------------------------------------------------------------------- */
/* GTestDBus object implementation */

/**
 * SECTION:gtestdbus
 * @short_description: D-Bus testing helper
 * @include: gio/gio.h
 *
 * A helper class for testing code which uses D-Bus without touching the user's
 * session bus.
 *
 * Note that #GTestDBus modifies the user’s environment, calling setenv().
 * This is not thread-safe, so all #GTestDBus calls should be completed before
 * threads are spawned, or should have appropriate locking to ensure no access
 * conflicts to environment variables shared between #GTestDBus and other
 * threads.
 *
 * ## Creating unit tests using GTestDBus
 * 
 * Testing of D-Bus services can be tricky because normally we only ever run
 * D-Bus services over an existing instance of the D-Bus daemon thus we
 * usually don't activate D-Bus services that are not yet installed into the
 * target system. The #GTestDBus object makes this easier for us by taking care
 * of the lower level tasks such as running a private D-Bus daemon and looking
 * up uninstalled services in customizable locations, typically in your source
 * code tree.
 *
 * The first thing you will need is a separate service description file for the
 * D-Bus daemon. Typically a `services` subdirectory of your `tests` directory
 * is a good place to put this file.
 *
 * The service file should list your service along with an absolute path to the
 * uninstalled service executable in your source tree. Using autotools we would
 * achieve this by adding a file such as `my-server.service.in` in the services
 * directory and have it processed by configure.
 * |[
 *     [D-BUS Service]
 *     Name=org.gtk.GDBus.Examples.ObjectManager
 *     Exec=@abs_top_builddir@/gio/tests/gdbus-example-objectmanager-server
 * ]|
 * You will also need to indicate this service directory in your test
 * fixtures, so you will need to pass the path while compiling your
 * test cases. Typically this is done with autotools with an added
 * preprocessor flag specified to compile your tests such as:
 * |[
 *     -DTEST_SERVICES=\""$(abs_top_builddir)/tests/services"\"
 * ]|
 *     Once you have a service definition file which is local to your source tree,
 * you can proceed to set up a GTest fixture using the #GTestDBus scaffolding.
 *
 * An example of a test fixture for D-Bus services can be found
 * here:
 * [gdbus-test-fixture.c](https://gitlab.gnome.org/GNOME/glib/-/blob/HEAD/gio/tests/gdbus-test-fixture.c)
 *
 * Note that these examples only deal with isolating the D-Bus aspect of your
 * service. To successfully run isolated unit tests on your service you may need
 * some additional modifications to your test case fixture. For example; if your
 * service uses GSettings and installs a schema then it is important that your test service
 * not load the schema in the ordinary installed location (chances are that your service
 * and schema files are not yet installed, or worse; there is an older version of the
 * schema file sitting in the install location).
 *
 * Most of the time we can work around these obstacles using the
 * environment. Since the environment is inherited by the D-Bus daemon
 * created by #GTestDBus and then in turn inherited by any services the
 * D-Bus daemon activates, using the setup routine for your fixture is
 * a practical place to help sandbox your runtime environment. For the
 * rather typical GSettings case we can work around this by setting
 * `GSETTINGS_SCHEMA_DIR` to the in tree directory holding your schemas
 * in the above fixture_setup() routine.
 *
 * The GSettings schemas need to be locally pre-compiled for this to work. This can be achieved
 * by compiling the schemas locally as a step before running test cases, an autotools setup might
 * do the following in the directory holding schemas:
 * |[
 *     all-am:
 *             $(GLIB_COMPILE_SCHEMAS) .
 *
 *     CLEANFILES += gschemas.compiled
 * ]|
 */

typedef struct _GTestDBusClass   GTestDBusClass;
typedef struct _GTestDBusPrivate GTestDBusPrivate;

/**
 * GTestDBus:
 *
 * The #GTestDBus structure contains only private data and
 * should only be accessed using the provided API.
 *
 * Since: 2.34
 */
struct _GTestDBus {
  GObject parent;

  GTestDBusPrivate *priv;
};

struct _GTestDBusClass {
  GObjectClass parent_class;
};

struct _GTestDBusPrivate
{
  GTestDBusFlags flags;
  GPtrArray *service_dirs;
  GPid bus_pid;
  gchar *bus_address;
  gboolean up;
};

enum
{
  PROP_0,
  PROP_FLAGS,
};

G_DEFINE_TYPE_WITH_PRIVATE (GTestDBus, g_test_dbus, G_TYPE_OBJECT)

static void
g_test_dbus_init (GTestDBus *self)
{
  self->priv = g_test_dbus_get_instance_private (self);
  self->priv->service_dirs = g_ptr_array_new_with_free_func (g_free);
}

static void
g_test_dbus_dispose (GObject *object)
{
  GTestDBus *self = (GTestDBus *) object;

  if (self->priv->up)
    g_test_dbus_down (self);

  G_OBJECT_CLASS (g_test_dbus_parent_class)->dispose (object);
}

static void
g_test_dbus_finalize (GObject *object)
{
  GTestDBus *self = (GTestDBus *) object;

  g_ptr_array_unref (self->priv->service_dirs);
  g_free (self->priv->bus_address);

  G_OBJECT_CLASS (g_test_dbus_parent_class)->finalize (object);
}

static void
g_test_dbus_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  GTestDBus *self = (GTestDBus *) object;

  switch (property_id)
    {
      case PROP_FLAGS:
        g_value_set_flags (value, g_test_dbus_get_flags (self));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
g_test_dbus_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  GTestDBus *self = (GTestDBus *) object;

  switch (property_id)
    {
      case PROP_FLAGS:
        self->priv->flags = g_value_get_flags (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
g_test_dbus_class_init (GTestDBusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = g_test_dbus_dispose;
  object_class->finalize = g_test_dbus_finalize;
  object_class->get_property = g_test_dbus_get_property;
  object_class->set_property = g_test_dbus_set_property;

  /**
   * GTestDBus:flags:
   *
   * #GTestDBusFlags specifying the behaviour of the D-Bus session.
   *
   * Since: 2.34
   */
  g_object_class_install_property (object_class, PROP_FLAGS,
    g_param_spec_flags ("flags",
                        P_("D-Bus session flags"),
                        P_("Flags specifying the behaviour of the D-Bus session"),
                        G_TYPE_TEST_DBUS_FLAGS, G_TEST_DBUS_NONE,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS));

}

static gchar *
write_config_file (GTestDBus *self)
{
  GString *contents;
  gint fd;
  guint i;
  GError *error = NULL;
  gchar *path = NULL;

  fd = g_file_open_tmp ("g-test-dbus-XXXXXX", &path, &error);
  g_assert_no_error (error);

  contents = g_string_new (NULL);
  g_string_append (contents,
      "<busconfig>\n"
      "  <type>session</type>\n"
#ifdef G_OS_WIN32
      "  <listen>nonce-tcp:</listen>\n"
#else
      "  <listen>unix:tmpdir=/tmp</listen>\n"
#endif
		   );

  for (i = 0; i < self->priv->service_dirs->len; i++)
    {
      const gchar *dir_path = g_ptr_array_index (self->priv->service_dirs, i);

      g_string_append_printf (contents,
          "  <servicedir>%s</servicedir>\n", dir_path);
    }

  g_string_append (contents,
      "  <policy context=\"default\">\n"
      "    <!-- Allow everything to be sent -->\n"
      "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
      "    <!-- Allow everything to be received -->\n"
      "    <allow eavesdrop=\"true\"/>\n"
      "    <!-- Allow anyone to own anything -->\n"
      "    <allow own=\"*\"/>\n"
      "  </policy>\n"
      "</busconfig>\n");

  close (fd);
  g_file_set_contents_full (path, contents->str, contents->len,
                            G_FILE_SET_CONTENTS_NONE,
                            0600, &error);
  g_assert_no_error (error);

  g_string_free (contents, TRUE);

  return path;
}

static gboolean
make_pipe (gint     pipe_fds[2],
           GError **error)
{
#if defined(G_OS_UNIX) && !defined(G_PLATFORM_WASM)
  return g_unix_open_pipe (pipe_fds, FD_CLOEXEC, error);
#elif defined(G_OS_WIN32)
  if (_pipe (pipe_fds, 4096, _O_BINARY) < 0)
    {
      int errsv = errno;

      g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                   _("Failed to create pipe for communicating with child process (%s)"),
                   g_strerror (errsv));
      return FALSE;
    }
  return TRUE;
#else
  g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
               _("Pipes are not supported in this platform"));
  return FALSE;
#endif
}

static void
start_daemon (GTestDBus *self)
{
  const gchar *argv[] = {"dbus-daemon", "--print-address", "--config-file=foo", NULL};
  gint pipe_fds[2] = {-1, -1};
  gchar *config_path;
  gchar *config_arg;
  gchar *print_address;
  GIOChannel *channel;
  gsize termpos;
  GError *error = NULL;

  if (g_getenv ("G_TEST_DBUS_DAEMON") != NULL)
    argv[0] = (gchar *)g_getenv ("G_TEST_DBUS_DAEMON");

  make_pipe (pipe_fds, &error);
  g_assert_no_error (error);

  print_address = g_strdup_printf ("--print-address=%d", pipe_fds[1]);
  argv[1] = print_address;
  g_assert_no_error (error);

  /* Write config file and set its path in argv */
  config_path = write_config_file (self);
  config_arg = g_strdup_printf ("--config-file=%s", config_path);
  argv[2] = config_arg;

  /* Spawn dbus-daemon */
  g_spawn_async_with_pipes_and_fds (NULL,
                                    argv,
                                    NULL,
                                    /* We Need this to get the pid returned on win32 */
                                    G_SPAWN_DO_NOT_REAP_CHILD |
                                    G_SPAWN_SEARCH_PATH |
                                    /* dbus-daemon will not abuse our descriptors, and
                                     * passing this means we can use posix_spawn() for speed */
                                    G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                                    NULL, NULL,
                                    -1, -1, -1,
                                    &pipe_fds[1], &pipe_fds[1], 1,
                                    &self->priv->bus_pid,
                                    NULL, NULL, NULL,
                                    &error);
  g_assert_no_error (error);

  _g_test_watcher_add_pid (self->priv->bus_pid);

  /* Read bus address from pipe */
  channel = g_io_channel_unix_new (pipe_fds[0]);
  pipe_fds[0] = -1;
  g_io_channel_set_close_on_unref (channel, TRUE);
  g_io_channel_read_line (channel, &self->priv->bus_address, NULL,
      &termpos, &error);
  g_assert_no_error (error);
  self->priv->bus_address[termpos] = '\0';
  close (pipe_fds[1]);
  pipe_fds[1] = -1;

  /* start dbus-monitor */
  if (g_getenv ("G_DBUS_MONITOR") != NULL)
    {
      gchar *command;

      command = g_strdup_printf ("dbus-monitor --address %s",
          self->priv->bus_address);
      g_spawn_command_line_async (command, NULL);
      g_free (command);

      g_usleep (500 * 1000);
    }

  /* Cleanup */
  g_io_channel_shutdown (channel, FALSE, &error);
  g_assert_no_error (error);
  g_io_channel_unref (channel);

  /* Don't use g_file_delete since it calls into gvfs */
  if (g_unlink (config_path) != 0)
    g_assert_not_reached ();

  g_free (print_address);
  g_free (config_path);
  g_free (config_arg);
}

static void
stop_daemon (GTestDBus *self)
{
#ifdef G_OS_WIN32
  if (!TerminateProcess (self->priv->bus_pid, 0))
    g_warning ("Can't terminate process: %s", g_win32_error_message (GetLastError()));
#else
  kill (self->priv->bus_pid, SIGTERM);
#endif
  _g_test_watcher_remove_pid (self->priv->bus_pid);
  g_spawn_close_pid (self->priv->bus_pid);
  self->priv->bus_pid = 0;

  g_free (self->priv->bus_address);
  self->priv->bus_address = NULL;
}

/**
 * g_test_dbus_new:
 * @flags: a #GTestDBusFlags
 *
 * Create a new #GTestDBus object.
 *
 * Returns: (transfer full): a new #GTestDBus.
 */
GTestDBus *
g_test_dbus_new (GTestDBusFlags flags)
{
  return g_object_new (G_TYPE_TEST_DBUS,
      "flags", flags,
      NULL);
}

/**
 * g_test_dbus_get_flags:
 * @self: a #GTestDBus
 *
 * Get the flags of the #GTestDBus object.
 *
 * Returns: the value of #GTestDBus:flags property
 */
GTestDBusFlags
g_test_dbus_get_flags (GTestDBus *self)
{
  g_return_val_if_fail (G_IS_TEST_DBUS (self), G_TEST_DBUS_NONE);

  return self->priv->flags;
}

/**
 * g_test_dbus_get_bus_address:
 * @self: a #GTestDBus
 *
 * Get the address on which dbus-daemon is running. If g_test_dbus_up() has not
 * been called yet, %NULL is returned. This can be used with
 * g_dbus_connection_new_for_address().
 *
 * Returns: (nullable): the address of the bus, or %NULL.
 */
const gchar *
g_test_dbus_get_bus_address (GTestDBus *self)
{
  g_return_val_if_fail (G_IS_TEST_DBUS (self), NULL);

  return self->priv->bus_address;
}

/**
 * g_test_dbus_add_service_dir:
 * @self: a #GTestDBus
 * @path: path to a directory containing .service files
 *
 * Add a path where dbus-daemon will look up .service files. This can't be
 * called after g_test_dbus_up().
 */
void
g_test_dbus_add_service_dir (GTestDBus *self,
    const gchar *path)
{
  g_return_if_fail (G_IS_TEST_DBUS (self));
  g_return_if_fail (self->priv->bus_address == NULL);

  g_ptr_array_add (self->priv->service_dirs, g_strdup (path));
}

/**
 * g_test_dbus_up:
 * @self: a #GTestDBus
 *
 * Start a dbus-daemon instance and set DBUS_SESSION_BUS_ADDRESS. After this
 * call, it is safe for unit tests to start sending messages on the session bus.
 *
 * If this function is called from setup callback of g_test_add(),
 * g_test_dbus_down() must be called in its teardown callback.
 *
 * If this function is called from unit test's main(), then g_test_dbus_down()
 * must be called after g_test_run().
 */
void
g_test_dbus_up (GTestDBus *self)
{
  g_return_if_fail (G_IS_TEST_DBUS (self));
  g_return_if_fail (self->priv->bus_address == NULL);
  g_return_if_fail (!self->priv->up);

  start_daemon (self);

  g_test_dbus_unset ();
  g_setenv ("DBUS_SESSION_BUS_ADDRESS", self->priv->bus_address, TRUE);
  self->priv->up = TRUE;
}


/**
 * g_test_dbus_stop:
 * @self: a #GTestDBus
 *
 * Stop the session bus started by g_test_dbus_up().
 *
 * Unlike g_test_dbus_down(), this won't verify the #GDBusConnection
 * singleton returned by g_bus_get() or g_bus_get_sync() is destroyed. Unit
 * tests wanting to verify behaviour after the session bus has been stopped
 * can use this function but should still call g_test_dbus_down() when done.
 */
void
g_test_dbus_stop (GTestDBus *self)
{
  g_return_if_fail (G_IS_TEST_DBUS (self));
  g_return_if_fail (self->priv->bus_address != NULL);

  stop_daemon (self);
}

/**
 * g_test_dbus_down:
 * @self: a #GTestDBus
 *
 * Stop the session bus started by g_test_dbus_up().
 *
 * This will wait for the singleton returned by g_bus_get() or g_bus_get_sync()
 * to be destroyed. This is done to ensure that the next unit test won't get a
 * leaked singleton from this test.
 */
void
g_test_dbus_down (GTestDBus *self)
{
  GDBusConnection *connection;

  g_return_if_fail (G_IS_TEST_DBUS (self));
  g_return_if_fail (self->priv->up);

  connection = _g_bus_get_singleton_if_exists (G_BUS_TYPE_SESSION);
  if (connection != NULL)
    g_dbus_connection_set_exit_on_close (connection, FALSE);

  if (self->priv->bus_address != NULL)
    stop_daemon (self);

  if (connection != NULL)
    _g_object_unref_and_wait_weak_notify (connection);

  g_test_dbus_unset ();
  _g_bus_forget_singleton (G_BUS_TYPE_SESSION);
  self->priv->up = FALSE;
}

/**
 * g_test_dbus_unset:
 *
 * Unset DISPLAY and DBUS_SESSION_BUS_ADDRESS env variables to ensure the test
 * won't use user's session bus.
 *
 * This is useful for unit tests that want to verify behaviour when no session
 * bus is running. It is not necessary to call this if unit test already calls
 * g_test_dbus_up() before acquiring the session bus.
 */
void
g_test_dbus_unset (void)
{
  g_unsetenv ("DISPLAY");
  g_unsetenv ("DBUS_SESSION_BUS_ADDRESS");
  g_unsetenv ("DBUS_STARTER_ADDRESS");
  g_unsetenv ("DBUS_STARTER_BUS_TYPE");
  /* avoid using XDG_RUNTIME_DIR/bus */
  g_unsetenv ("XDG_RUNTIME_DIR");
}
