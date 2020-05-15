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
 * Author: Marek Chalupa <mchalupa@redhat.com>
 */

#include "config.h"

#include <goa/goa.h>
#include <grilo.h>

#include "gom-flickr-miner.h"
#include "gom-utils.h"

#define MINER_IDENTIFIER "gd:flickr:miner:3c63f509-23e8-4283-8aed-154bb55ef07b"

G_DEFINE_TYPE (GomFlickrMiner, gom_flickr_miner, GOM_TYPE_MINER)

struct _GomFlickrMinerPrivate {
  GQueue *boxes;
};

typedef enum {
  OP_FETCH_ALL,
  OP_CREATE_HIEARCHY
} OpType;

typedef struct {
  GrlMedia  *media;
  GrlMedia  *parent;
} FlickrEntry;

typedef struct {
  FlickrEntry *parent_entry;
  GCancellable *cancellable;
  GHashTable *previous_resources;
  GMainLoop *loop;
  GomAccountMinerJob *job;
  GrlSource *source;
  TrackerSparqlConnection *connection;
  const gchar *datasource_urn;
  const gchar *source_id;
} SyncData;

static void account_miner_job_browse_container (GomAccountMinerJob *job,
                                                TrackerSparqlConnection *connection,
                                                GHashTable *previous_resources,
                                                const gchar *datasource_urn,
                                                FlickrEntry *entry,
                                                GCancellable *cancellable);

static FlickrEntry *
create_entry (GrlMedia *media, GrlMedia *parent)
{
  FlickrEntry *entry;

  entry = g_slice_new0 (FlickrEntry);

  entry->media  = (media != NULL) ? g_object_ref (media) : NULL;
  entry->parent = (parent != NULL) ? g_object_ref (parent) : NULL;

  return entry;
}

static void
free_entry (FlickrEntry *entry)
{
  g_clear_object (&entry->media);
  g_clear_object (&entry->parent);
  g_slice_free (FlickrEntry, entry);
}

static GrlOperationOptions *
get_grl_options (GrlSource *source)
{
  GrlCaps *caps;
  GrlOperationOptions *opts = NULL;

  caps = grl_source_get_caps (source, GRL_OP_BROWSE);
  opts = grl_operation_options_new (caps);

  g_return_val_if_fail (opts != NULL, NULL);

  grl_operation_options_set_resolution_flags (opts, GRL_RESOLVE_FAST_ONLY);

  return opts;
}

static gboolean
account_miner_job_process_entry (GomAccountMinerJob *job,
                                 TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 OpType op_type,
                                 FlickrEntry *entry,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GDateTime *created_time, *modification_date;
  gchar *contact_resource;
  gchar *mime;
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL, *id;
  const gchar *url;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;

  if (op_type == OP_CREATE_HIEARCHY && entry->parent == NULL && !grl_media_is_container (entry->media))
    return TRUE;

  id = grl_media_get_id (entry->media);
  identifier = g_strdup_printf ("%sflickr:%s",
                                grl_media_is_container (entry->media) ?
                                "photos:collection:" : "",
                                id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (previous_resources, identifier);

  if (grl_media_is_container (entry->media))
    class = "nfo:DataContainer";
  else
    class = "nmm:Photo";

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     TRACKER_PICTURES_GRAPH, identifier,
     "nfo:RemoteDataObject", class, NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (connection, datasource_urn,
                                 resource_exists, TRACKER_PICTURES_GRAPH, resource,
                                 cancellable, error);

  if (*error != NULL)
    goto out;

  if (entry->parent != NULL)
    {
      gchar *parent_resource_urn, *parent_identifier;
      const gchar *parent_id;

      parent_identifier = g_strconcat ("photos:collection:flickr:",
                                        grl_media_get_id (entry->parent) , NULL);
      parent_resource_urn = gom_tracker_sparql_connection_ensure_resource
        (connection, cancellable, error,
         NULL,
         TRACKER_PICTURES_GRAPH, parent_identifier,
         "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
      g_free (parent_identifier);

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         TRACKER_PICTURES_GRAPH, resource,
         "nie:isPartOf", parent_resource_urn);
      g_free (parent_resource_urn);

      if (*error != NULL)
        goto out;
    }

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:title", grl_media_get_title (entry->media));

  if (*error != NULL)
    goto out;

  if (op_type == OP_CREATE_HIEARCHY)
    goto out;

  /* only GRL_METADATA_KEY_CREATION_DATE is
   * implemented, GRL_METADATA_KEY_MODIFICATION_DATE is not
   */
  created_time = modification_date = grl_media_get_creation_date (entry->media);
  new_mtime = g_date_time_to_unix (modification_date);
  mtime_changed = gom_tracker_update_mtime (connection, new_mtime,
                                            resource_exists, TRACKER_PICTURES_GRAPH, resource,
                                            cancellable, error);

  if (*error != NULL)
    goto out;

  /* avoid updating the DB if the entry already exists and has not
   * been modified since our last run.
   */
  if (!mtime_changed)
    goto out;

  /* the resource changed - just set all the properties again */
  if (created_time != NULL)
    {
      date = gom_iso8601_from_timestamp (g_date_time_to_unix (created_time));
      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         TRACKER_PICTURES_GRAPH, resource,
         "nie:contentCreated", date);
      g_free (date);
    }

  if (*error != NULL)
    goto out;

  url = grl_media_get_url (entry->media);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:url", url);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:description", grl_media_get_description (entry->media));

  if (*error != NULL)
    goto out;

  mime = g_content_type_guess (url, NULL, 0, NULL);
  if (mime != NULL)
    {
      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         TRACKER_PICTURES_GRAPH, resource,
         "nie:mimeType", mime);
      g_free (mime);

      if (*error != NULL)
        goto out;
    }

  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, grl_media_get_author (entry->media));

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nco:creator", contact_resource);
  g_free (contact_resource);

  if (*error != NULL)
    goto out;

 out:
  g_free (resource);
  g_free (identifier);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static void
source_browse_cb (GrlSource *source,
                  guint operation_id,
                  GrlMedia *media,
                  guint remaining,
                  gpointer user_data,
                  const GError *error)
{
  GError *local_error = NULL;
  SyncData *data = (SyncData *) user_data;
  GomFlickrMiner *self = GOM_FLICKR_MINER (data->job->miner);

  if (error != NULL)
    {
      g_warning ("Unable to browse source %p: %s", source, error->message);
      return;
    }

  if (media != NULL)
    {
      FlickrEntry *entry;

      entry = create_entry (media, data->parent_entry->media);
      account_miner_job_process_entry (data->job,
                                       data->connection,
                                       data->previous_resources,
                                       data->datasource_urn,
                                       OP_CREATE_HIEARCHY,
                                       entry,
                                       data->cancellable,
                                       &local_error);
      if (local_error != NULL)
        {
          g_warning ("Unable to process entry %p: %s", media, local_error->message);
          g_error_free (local_error);
        }

      if (grl_media_is_container (media))
        g_queue_push_tail (self->priv->boxes, entry);
      else
        free_entry (entry);
    }

  if (remaining == 0)
    g_main_loop_quit (data->loop);
}

static void
account_miner_job_browse_container (GomAccountMinerJob *job,
                                    TrackerSparqlConnection *connection,
                                    GHashTable *previous_resources,
                                    const gchar *datasource_urn,
                                    FlickrEntry *entry,
                                    GCancellable *cancellable)
{
  GMainContext *context;
  GrlSource *source;
  GrlOperationOptions *opts;
  const GList *keys;
  SyncData data;

  data.cancellable = cancellable;
  data.connection = connection;
  data.datasource_urn = datasource_urn;
  data.parent_entry = entry;
  data.job = job;
  data.previous_resources = previous_resources;

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);
  data.loop = g_main_loop_new (context, FALSE);

  source = GRL_SOURCE (g_hash_table_lookup (data.job->services, "photos"));

  keys = grl_source_supported_keys (source);
  opts = get_grl_options (source);

  grl_source_browse (source,
                     entry->media,
                     keys,
                     opts,
                     source_browse_cb,
                     &data);
  g_main_loop_run (data.loop);

  g_object_unref (opts);
  g_main_loop_unref (data.loop);
  g_main_context_pop_thread_default (context);
  g_main_context_unref (context);
}

static void
source_search_cb (GrlSource *source,
                  guint operation_id,
                  GrlMedia *media,
                  guint remaining,
                  gpointer user_data,
                  const GError *error)
{
  GError *local_error = NULL;
  SyncData *data = (SyncData *) user_data;

  if (error != NULL)
    {
      g_warning ("Unable to search source %p: %s", source, error->message);
      return;
    }

  if (media != NULL)
    {
      FlickrEntry *entry;

      entry = create_entry (media, NULL);
      account_miner_job_process_entry (data->job,
                                       data->connection,
                                       data->previous_resources,
                                       data->datasource_urn,
                                       OP_FETCH_ALL,
                                       entry,
                                       data->cancellable,
                                       &local_error);
      if (local_error != NULL)
        {
          g_warning ("Unable to process entry %p: %s", media, local_error->message);
          g_error_free (local_error);
        }

      free_entry (entry);
    }

  if (remaining == 0)
    g_main_loop_quit (data->loop);
}

static void
query_flickr (GomAccountMinerJob *job,
              TrackerSparqlConnection *connection,
              GHashTable *previous_resources,
              const gchar *datasource_urn,
              GCancellable *cancellable,
              GError **error)
{
  GomFlickrMiner *self = GOM_FLICKR_MINER (job->miner);
  GomFlickrMinerPrivate *priv = self->priv;
  FlickrEntry *entry;
  const GList *keys;
  GMainContext *context;
  GrlOperationOptions *opts;
  GrlSource *source;
  SyncData data;

  source = GRL_SOURCE (g_hash_table_lookup (job->services, "photos"));
  if (source == NULL)
  {
    /* FIXME: use proper #defines and enumerated types */
    g_set_error (error,
                 g_quark_from_static_string ("gom-error"),
                 0,
                 "Can not query without a service");
    return;
  }

  /* grl_source_browse does not fetch photos that are not part of a
   * set. So, use grl_source_search to fetch all photos and then allot
   * each photo to any set that it might be a part of.
   */

  data.cancellable = cancellable;
  data.connection = connection;
  data.datasource_urn = datasource_urn;
  data.job = job;
  data.previous_resources = previous_resources;
  context = g_main_context_new ();
  g_main_context_push_thread_default (context);
  data.loop = g_main_loop_new (context, FALSE);

  keys = grl_source_supported_keys (source);
  opts = get_grl_options (source);
  grl_source_search (source, NULL, keys, opts, source_search_cb, &data);
  g_main_loop_run (data.loop);

  g_object_unref (opts);
  g_main_loop_unref (data.loop);
  g_main_context_pop_thread_default (context);
  g_main_context_unref (context);

  entry = create_entry (NULL, NULL);
  account_miner_job_browse_container (job, connection, previous_resources, datasource_urn, entry, cancellable);
  free_entry (entry);

  while (!g_queue_is_empty (priv->boxes))
    {
      entry = (FlickrEntry *) g_queue_pop_head (priv->boxes);
      account_miner_job_browse_container (job, connection, previous_resources, datasource_urn, entry, cancellable);
      free_entry (entry);
    }
}

static void
source_added_cb (GrlRegistry *registry, GrlSource *source, gpointer user_data)
{
  SyncData *data = (SyncData *) user_data;
  gchar *source_id;

  g_object_get (source, "source-id", &source_id, NULL);
  if (g_strcmp0 (source_id, data->source_id) != 0)
    goto out;

  data->source = g_object_ref (source);
  g_main_loop_quit (data->loop);

 out:
  g_free (source_id);
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GHashTable *services;
  GoaAccount *acc;
  GrlRegistry *registry;
  GrlSource *source = NULL;
  gchar *source_id = NULL;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  acc = goa_object_peek_account (object);
  if (acc == NULL)
    goto out;

  if (gom_miner_supports_type (self, "photos"))
    {
      source_id = g_strdup_printf ("grl-flickr-%s", goa_account_get_id (acc));

      registry = grl_registry_get_default ();

      g_debug ("Looking for source %s", source_id);
      source = grl_registry_lookup_source (registry, source_id);
      if (source == NULL)
        {
          GMainContext *context;
          SyncData data;

          context = g_main_context_get_thread_default ();
          data.loop = g_main_loop_new (context, FALSE);
          data.source_id = source_id;

          g_signal_connect (registry, "source-added", G_CALLBACK (source_added_cb), &data);
          g_main_loop_run (data.loop);
          g_main_loop_unref (data.loop);

          /* we steal the ref from data */
          source = data.source;
        }
      else
        {
          /* freeing job calls unref upon this object */
          g_object_ref (source);
        }
      g_free (source_id);
      g_hash_table_insert (services, "photos", source);
    }

 out:
  return services;
}

static void
gom_flickr_miner_finalize (GObject *object)
{
  GomFlickrMiner *self = GOM_FLICKR_MINER (object);

  g_queue_free_full (self->priv->boxes, (GDestroyNotify) free_entry);

  G_OBJECT_CLASS (gom_flickr_miner_parent_class)->finalize (object);
}

static void
gom_flickr_miner_init (GomFlickrMiner *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GOM_TYPE_FLICKR_MINER, GomFlickrMinerPrivate);
  self->priv->boxes = g_queue_new ();
}

static void
gom_flickr_miner_class_init (GomFlickrMinerClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);
  GrlRegistry *registry;
  GError *error = NULL;

  oclass->finalize = gom_flickr_miner_finalize;

  miner_class->goa_provider_type = "flickr";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_services = create_services;
  miner_class->query = query_flickr;

  grl_init (NULL, NULL);
  registry = grl_registry_get_default ();
  grl_registry_load_all_plugins (registry, FALSE, &error);

  if (error != NULL || !grl_registry_activate_plugin_by_id (registry, "grl-flickr", &error))
    {
      g_error ("%s", error->message);
      g_error_free (error);
    }

  g_type_class_add_private (klass, sizeof (GomFlickrMinerPrivate));
}
