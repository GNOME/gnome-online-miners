/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2013 Red Hat, Inc.
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

#include <goa/goa.h>

#include "gom-owncloud-miner.h"
#include "gom-utils.h"

#define MINER_IDENTIFIER "gd:owncloud:miner:8a409711-8fea-4eda-a417-f140ffc6d8f3"

G_DEFINE_TYPE (GomOwncloudMiner, gom_owncloud_miner, GOM_TYPE_MINER)

struct _GomOwncloudMinerPrivate {
  GVolumeMonitor *monitor;
};

#define FILE_ATTRIBUTES \
  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," \
  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
  G_FILE_ATTRIBUTE_STANDARD_NAME "," \
  G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
  G_FILE_ATTRIBUTE_TIME_MODIFIED

typedef struct {
  GError **error;
  GMainLoop *loop;
  GomAccountMinerJob *job;
} SyncData;

static gboolean
account_miner_job_process_file (GomAccountMinerJob *job,
                                TrackerSparqlConnection *connection,
                                GHashTable *previous_resources,
                                const gchar *datasource_urn,
                                GFile *file,
                                GFileInfo *info,
                                GFile *parent,
                                GCancellable *cancellable,
                                GError **error)
{
  GChecksum *checksum = NULL;
  GDateTime *modification_time;
  GFileType type;
  GTimeVal tv;
  gboolean mtime_changed;
  gboolean resource_exists;
  const gchar *class;
  const gchar *display_name;
  const gchar *id;
  const gchar *name;
  gchar *identifier = NULL;
  gchar *resource = NULL;
  gchar *uri = NULL;
  gint64 new_mtime;

  type = g_file_info_get_file_type (info);
  uri = g_file_get_uri (file);

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, uri, -1);
  id = g_checksum_get_string (checksum);
  identifier = g_strdup_printf ("%sowncloud:%s", (type == G_FILE_TYPE_DIRECTORY ? "gd:collection:" : ""), id);
  g_checksum_reset (checksum);

  /* remove from the list of the previous resources */
  g_hash_table_remove (previous_resources, identifier);

  name = g_file_info_get_name (info);
  if (type == G_FILE_TYPE_REGULAR)
    class = gom_filename_to_rdf_type (name);
  else
    class = "nfo:DataContainer";

  if (class == NULL)
    goto out;

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     TRACKER_DOCUMENTS_GRAPH, identifier,
     "nfo:RemoteDataObject", class, NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (connection, datasource_urn,
                                 resource_exists, TRACKER_DOCUMENTS_GRAPH, resource,
                                 cancellable, error);

  if (*error != NULL)
    goto out;

  g_file_info_get_modification_time (info, &tv);
  modification_time = g_date_time_new_from_timeval_local (&tv);
  new_mtime = g_date_time_to_unix (modification_time);
  mtime_changed = gom_tracker_update_mtime (connection, new_mtime,
                                            resource_exists, TRACKER_DOCUMENTS_GRAPH, resource,
                                            cancellable, error);

  if (*error != NULL)
    goto out;

  /* avoid updating the DB if the entry already exists and has not
   * been modified since our last run.
   */
  if (!mtime_changed)
    goto out;

  /* the resource changed - just set all the properties again */
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_DOCUMENTS_GRAPH, resource,
     "nie:url", uri);

  if (*error != NULL)
    goto out;

  if (type == G_FILE_TYPE_REGULAR)
    {
      const gchar *mime;
      const gchar *parent_id;
      gchar *parent_identifier;
      gchar *parent_resource_urn;
      gchar *parent_uri;

      if (parent != NULL)
        {
          parent_uri = g_file_get_uri (parent);
          g_checksum_update (checksum, parent_uri, -1);
          parent_id = g_checksum_get_string (checksum);
          parent_identifier = g_strconcat ("gd:collection:owncloud:", parent_id, NULL);
          parent_resource_urn = gom_tracker_sparql_connection_ensure_resource
            (connection, cancellable, error,
             NULL,
             GOM_GRAPH, parent_identifier,
             "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
          g_checksum_reset (checksum);
          g_free (parent_identifier);
          g_free (parent_uri);

          if (*error != NULL)
            goto out;

          gom_tracker_sparql_connection_insert_or_replace_triple
            (connection,
             cancellable, error,
             TRACKER_DOCUMENTS_GRAPH, resource,
             "nie:isPartOf", parent_resource_urn);
          g_free (parent_resource_urn);

          if (*error != NULL)
            goto out;
        }

      mime = g_file_info_get_content_type (info);
      if (mime != NULL)
        {
          gom_tracker_sparql_connection_insert_or_replace_triple
            (connection,
             cancellable, error,
             TRACKER_DOCUMENTS_GRAPH, resource,
             "nie:mimeType", mime);

          if (*error != NULL)
            goto out;
        }
    }

  display_name = g_file_info_get_display_name (info);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_DOCUMENTS_GRAPH, resource,
     "nfo:fileName", display_name);

  if (*error != NULL)
    goto out;

 out:
  if (checksum != NULL)
    g_checksum_free (checksum);
  g_free (identifier);
  g_free (resource);
  g_free (uri);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static void
account_miner_job_traverse_dir (GomAccountMinerJob *job,
                                TrackerSparqlConnection *connection,
                                GHashTable *previous_resources,
                                const gchar *datasource_urn,
                                GFile *dir,
                                gboolean is_root,
                                GCancellable *cancellable,
                                GError **error)
{
  GError *local_error = NULL;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  gchar *dir_uri;

  dir_uri = g_file_get_uri (dir);
  enumerator = g_file_enumerate_children (dir,
                                          FILE_ATTRIBUTES,
                                          G_FILE_QUERY_INFO_NONE,
                                          cancellable,
                                          &local_error);
  if (local_error != NULL)
    goto out;

  while ((info = g_file_enumerator_next_file (enumerator, cancellable, &local_error)) != NULL)
    {
      GFile *child;
      GFileType type;
      const gchar *name;
      gchar *uri;

      type = g_file_info_get_file_type (info);
      name = g_file_info_get_name (info);
      child = g_file_get_child (dir, name);

      if (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_DIRECTORY)
        {
          account_miner_job_process_file (job,
                                          connection,
                                          previous_resources,
                                          datasource_urn,
                                          child,
                                          info,
                                          is_root ? NULL : dir,
                                          cancellable,
                                          &local_error);
          if (local_error != NULL)
            {
              uri = g_file_get_uri (child);
              g_warning ("Unable to process %s: %s", uri, local_error->message);
              g_free (uri);
              g_clear_error (&local_error);
            }
        }

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          account_miner_job_traverse_dir (job,
                                          connection,
                                          previous_resources,
                                          datasource_urn,
                                          child,
                                          FALSE,
                                          cancellable,
                                          &local_error);
          if (local_error != NULL)
            {
              uri = g_file_get_uri (child);
              g_warning ("Unable to traverse %s: %s", uri, local_error->message);
              g_free (uri);
              g_clear_error (&local_error);
            }
        }

      g_object_unref (child);
      g_object_unref (info);
    }

  if (local_error != NULL)
    goto out;

 out:
  if (local_error != NULL)
    g_propagate_error (error, local_error);

  g_clear_object (&enumerator);
  g_free (dir_uri);
}

static gboolean
is_matching_volume (GVolume *volume, GoaObject *object)
{
  GoaAccount *account;
  GoaFiles *files;
  gboolean retval = FALSE;
  const gchar *presentation_identity;
  const gchar *uri;
  gchar *name;
  gchar *uuid;

  account = goa_object_peek_account (object);
  presentation_identity = goa_account_get_presentation_identity (account);

  files = goa_object_peek_files (object);
  uri = goa_files_get_uri (files);

  name = g_volume_get_name (volume);
  uuid = g_volume_get_uuid (volume);

  if (g_strcmp0 (name, presentation_identity) == 0 && g_strcmp0 (uuid, uri) == 0)
    retval = TRUE;

  g_free (name);
  g_free (uuid);
  return retval;
}

static void
volume_added_cb (GVolumeMonitor *monitor, GVolume *volume, gpointer user_data)
{
  SyncData *data = (SyncData *) user_data;
  GoaObject *object;

  object = GOA_OBJECT (g_hash_table_lookup (data->job->services, "documents"));
  if (!is_matching_volume (volume, object))
    return;

  g_object_set_data_full (G_OBJECT (object), "volume", g_object_ref (volume), g_object_unref);
  g_main_loop_quit (data->loop);
}

static void
volume_mount_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVolume *volume = G_VOLUME (source_object);
  SyncData *data = (SyncData *) user_data;

  g_volume_mount_finish (volume, res, data->error);
  g_main_loop_quit (data->loop);
}

static void
query_owncloud (GomAccountMinerJob *job,
                TrackerSparqlConnection *connection,
                GHashTable *previous_resources,
                const gchar *datasource_urn,
                GCancellable *cancellable,
                GError **error)
{
  GomOwncloudMiner *self = GOM_OWNCLOUD_MINER (job->miner);
  GomOwncloudMinerPrivate *priv = self->priv;
  GoaObject *object;
  GFile *root;
  GList *l;
  GList *volumes;
  GMainContext *context;
  GMount *mount;
  GVolume *volume;
  SyncData data;
  gboolean found = FALSE;

  object = GOA_OBJECT (g_hash_table_lookup (job->services, "documents"));
  if (object == NULL)
    {
      /* FIXME: use proper #defines and enumerated types */
      g_set_error (error,
                   g_quark_from_static_string ("gom-error"),
                   0,
                   "Can not query without a service");
      return;
    }

  data.job = job;
  volumes = g_volume_monitor_get_volumes (priv->monitor);

  /* find the volume corresponding to this GoaObject */
  for (l = volumes; l != NULL && !found; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);

      if (is_matching_volume (volume, object))
        {
          g_object_set_data_full (G_OBJECT (object), "volume", g_object_ref (volume), g_object_unref);
          found = TRUE;
        }
    }

  /* wait for GVfs to add the volume, if it hasn't already */
  if (!found)
    {
      context = g_main_context_get_thread_default ();
      data.loop = g_main_loop_new (context, FALSE);

      g_signal_connect (priv->monitor, "volume-added", G_CALLBACK (volume_added_cb), &data);
      g_main_loop_run (data.loop);
      g_main_loop_unref (data.loop);
    }

  /* mount the volume if needed */
  volume = G_VOLUME (g_object_get_data (G_OBJECT (object), "volume"));
  mount = g_volume_get_mount (volume);
  if (mount == NULL)
    {
      g_autofree gchar *volume_name;

      volume_name = g_volume_get_name (volume);
      g_debug ("Mounting Online Account volume %s", volume_name);

      data.error = error;

      context = g_main_context_new ();
      g_main_context_push_thread_default (context);
      data.loop = g_main_loop_new (context, FALSE);

      g_volume_mount (volume, G_MOUNT_MOUNT_NONE, NULL, cancellable, volume_mount_cb, &data);
      g_main_loop_run (data.loop);

      g_main_loop_unref (data.loop);
      g_main_context_pop_thread_default (context);
      g_main_context_unref (context);

      if (*error != NULL)
        return;

      mount = g_volume_get_mount (volume);
    }

  root = g_mount_get_root (mount);
  g_debug ("Got volume from gnome-online-accounts: root is %s", g_file_peek_path (root));
  account_miner_job_traverse_dir (job, connection, previous_resources, datasource_urn, root, TRUE, cancellable, error);

  g_object_unref (root);
  g_object_unref (mount);
  g_list_free_full (volumes, g_object_unref);
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GHashTable *services;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  if (gom_miner_supports_type (self, "documents"))
    g_hash_table_insert (services, "documents", g_object_ref (object));

  return services;
}

static void
gom_owncloud_miner_dispose (GObject *object)
{
  GomOwncloudMiner *self = GOM_OWNCLOUD_MINER (object);

  g_clear_object (&self->priv->monitor);

  G_OBJECT_CLASS (gom_owncloud_miner_parent_class)->dispose (object);
}

static void
gom_owncloud_miner_init (GomOwncloudMiner *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GOM_TYPE_OWNCLOUD_MINER, GomOwncloudMinerPrivate);
  self->priv->monitor = g_volume_monitor_get ();
}

static void
gom_owncloud_miner_class_init (GomOwncloudMinerClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);

  oclass->dispose = gom_owncloud_miner_dispose;

  miner_class->goa_provider_type = "owncloud";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_services = create_services;
  miner_class->query = query_owncloud;

  g_type_class_add_private (klass, sizeof (GomOwncloudMinerPrivate));
}
