/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Author: Debarshi Ray <debarshir@gnome.org>
 *
 */

#include "config.h"

#include "gom-application.h"
#include "gom-dbus.h"
#include "gom-miner.h"

#define AUTOQUIT_TIMEOUT 5 /* seconds */

struct _GomApplication
{
  GApplication parent;
  GCancellable *cancellable;
  GomDBus *skeleton;
  GomMiner *miner;
  GQueue *queue;
  GType miner_type;
  gboolean refreshing;
};

struct _GomApplicationClass
{
  GApplicationClass parent_class;
};

enum
{
  PROP_0,
  PROP_MINER_TYPE
};

G_DEFINE_TYPE (GomApplication, gom_application, G_TYPE_APPLICATION);

static void gom_application_refresh_db_cb (GObject *source, GAsyncResult *res, gpointer user_data);

static void
gom_application_insert_shared_content_cb (GObject *source,
                                          GAsyncResult *res,
                                          gpointer user_data)
{
  GomApplication *self;
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (user_data);
  GError *error;

  self = GOM_APPLICATION (g_application_get_default ());
  g_application_release (G_APPLICATION (self));

  error = NULL;
  if (!gom_miner_insert_shared_content_finish (GOM_MINER (source), res, &error))
    {
      g_printerr ("Failed to insert shared content: %s\n", error->message);
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  gom_dbus_complete_insert_shared_content (self->skeleton, invocation);

 out:
  g_object_unref (invocation);
}

static gboolean
gom_application_insert_shared_content (GomApplication *self,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *account_id,
                                       const gchar *shared_id,
                                       const gchar *shared_type,
                                       const gchar *source_urn)
{
  g_application_hold (G_APPLICATION (self));
  gom_miner_insert_shared_content_async (self->miner,
                                         account_id,
                                         shared_id,
                                         shared_type,
                                         source_urn,
                                         self->cancellable,
                                         gom_application_insert_shared_content_cb,
                                         g_object_ref (invocation));
  return TRUE;
}

static void
gom_application_process_queue (GomApplication *self)
{
  GDBusMethodInvocation *invocation = NULL;
  const gchar **index_types;

  if (self->refreshing)
    goto out;

  if (g_queue_is_empty (self->queue))
    goto out;

  invocation = G_DBUS_METHOD_INVOCATION (g_queue_pop_head (self->queue));
  index_types = g_object_get_data (G_OBJECT (invocation), "index-types");
  gom_miner_set_index_types (self->miner, index_types);

  self->refreshing = TRUE;
  g_application_hold (G_APPLICATION (self));
  gom_miner_refresh_db_async (self->miner,
                              self->cancellable,
                              gom_application_refresh_db_cb,
                              g_object_ref (invocation));

 out:
  g_clear_object (&invocation);
}

static void
gom_application_refresh_db_cb (GObject *source,
                               GAsyncResult *res,
                               gpointer user_data)
{
  GomApplication *self;
  GDBusMethodInvocation *invocation = user_data;
  GError *error = NULL;

  self = GOM_APPLICATION (g_application_get_default ());
  g_application_release (G_APPLICATION (self));
  self->refreshing = FALSE;

  gom_miner_refresh_db_finish (GOM_MINER (source), res, &error);
  if (error != NULL)
    {
      g_printerr ("Failed to refresh the DB cache: %s\n", error->message);
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  gom_dbus_complete_refresh_db (self->skeleton, invocation);

 out:
  g_object_unref (invocation);
  gom_application_process_queue (self);
}

static gboolean
gom_application_refresh_db (GomApplication *self,
                            GDBusMethodInvocation *invocation,
                            const gchar *const *arg_index_types)
{
  gchar **index_types;

  index_types = g_strdupv ((gchar **) arg_index_types);
  g_object_set_data_full (G_OBJECT (invocation), "index-types", index_types, (GDestroyNotify) g_strfreev);
  g_queue_push_tail (self->queue, g_object_ref (invocation));
  gom_application_process_queue (self);
  return TRUE;
}

static gboolean
gom_application_dbus_register (GApplication *application,
                               GDBusConnection *connection,
                               const gchar *object_path,
                               GError **error)
{
  GomApplication *self = GOM_APPLICATION (application);
  gboolean retval = FALSE;

  if (!G_APPLICATION_CLASS (gom_application_parent_class)->dbus_register (application,
                                                                          connection,
                                                                          object_path,
                                                                          error))
    goto out;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection,
                                         object_path,
                                         error))
    goto out;

  retval = TRUE;

 out:
  return retval;
}

static void
gom_application_dbus_unregister (GApplication *application,
                                 GDBusConnection *connection,
                                 const gchar *object_path)
{
  GomApplication *self = GOM_APPLICATION (application);

  if (self->skeleton != NULL)
    {
      if (g_dbus_interface_skeleton_has_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton), connection))
        g_dbus_interface_skeleton_unexport_from_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                                            connection);
    }

  G_APPLICATION_CLASS (gom_application_parent_class)->dbus_unregister (application, connection, object_path);
}

static void
gom_application_shutdown (GApplication *application)
{
  GomApplication *self = GOM_APPLICATION (application);

  g_cancellable_cancel (self->cancellable);

  G_APPLICATION_CLASS (gom_application_parent_class)->shutdown (application);
}

static void
gom_application_constructed (GObject *object)
{
  GomApplication *self = GOM_APPLICATION (object);
  const gchar *display_name;
  GError *error = NULL;

  G_OBJECT_CLASS (gom_application_parent_class)->constructed (object);

  self->miner = g_initable_new (self->miner_type, NULL, &error,
                                "bus-name", g_application_get_application_id (G_APPLICATION (self)),
                                NULL);

  if (self->miner == NULL) {
    g_error (error->message);
  }

  display_name = gom_miner_get_display_name (self->miner);
  gom_dbus_set_display_name (self->skeleton, display_name);
}

static void
gom_application_dispose (GObject *object)
{
  GomApplication *self = GOM_APPLICATION (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->miner);
  g_clear_object (&self->skeleton);

  if (self->queue != NULL)
    {
      g_queue_free_full (self->queue, g_object_unref);
      self->queue = NULL;
    }

  G_OBJECT_CLASS (gom_application_parent_class)->dispose (object);
}

static void
gom_application_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
  GomApplication *self = GOM_APPLICATION (object);

  switch (prop_id)
    {
    case PROP_MINER_TYPE:
      self->miner_type = g_value_get_gtype (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gom_application_init (GomApplication *self)
{
  self->cancellable = g_cancellable_new ();

  self->skeleton = gom_dbus_skeleton_new ();
  g_signal_connect_swapped (self->skeleton,
                            "handle-insert-shared-content",
                            G_CALLBACK (gom_application_insert_shared_content),
                            self);
  g_signal_connect_swapped (self->skeleton, "handle-refresh-db", G_CALLBACK (gom_application_refresh_db), self);

  self->queue = g_queue_new ();
}

static void
gom_application_class_init (GomApplicationClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

  oclass->constructed = gom_application_constructed;
  oclass->dispose = gom_application_dispose;
  oclass->set_property = gom_application_set_property;
  application_class->dbus_register = gom_application_dbus_register;
  application_class->dbus_unregister = gom_application_dbus_unregister;
  application_class->shutdown = gom_application_shutdown;

  g_object_class_install_property (oclass,
                                   PROP_MINER_TYPE,
                                   g_param_spec_gtype ("miner-type",
                                                       "Miner type",
                                                       "A GType representing the miner class",
                                                       GOM_TYPE_MINER,
                                                       G_PARAM_CONSTRUCT_ONLY
                                                       | G_PARAM_STATIC_STRINGS
                                                       | G_PARAM_WRITABLE));
}

GApplication *
gom_application_new (const gchar *application_id,
                     GType miner_type)
{
  return g_object_new (GOM_TYPE_APPLICATION,
                       "application-id", application_id,
                       "flags", G_APPLICATION_IS_SERVICE,
                       "inactivity-timeout", AUTOQUIT_TIMEOUT,
                       "miner-type", miner_type,
                       NULL);
}
