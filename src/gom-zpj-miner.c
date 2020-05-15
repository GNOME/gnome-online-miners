/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2012 Red Hat, Inc.
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
#include <zpj/zpj.h>

#include "gom-zpj-miner.h"
#include "gom-utils.h"

#define MINER_IDENTIFIER "gd:zpj:miner:30058620-777c-47a3-a19c-a6cdf4a315c4"

G_DEFINE_TYPE (GomZpjMiner, gom_zpj_miner, GOM_TYPE_MINER)

static gboolean
account_miner_job_process_entry (GomAccountMinerJob *job,
                                 TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 ZpjSkydriveEntry *entry,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GDateTime *created_time, *updated_time;
  gchar *contact_resource;
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL, *id, *name;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;

  id = zpj_skydrive_entry_get_id (entry);

  identifier = g_strdup_printf ("%swindows-live:skydrive:%s",
                                ZPJ_IS_SKYDRIVE_FOLDER (entry) ? "gd:collection:" : "",
                                id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (previous_resources, identifier);

  name = zpj_skydrive_entry_get_name (entry);

  if (ZPJ_IS_SKYDRIVE_FILE (entry))
    class = gom_filename_to_rdf_type (name);
  else if (ZPJ_IS_SKYDRIVE_FOLDER (entry))
    class = "nfo:DataContainer";

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

  updated_time = zpj_skydrive_entry_get_updated_time (entry);
  new_mtime = g_date_time_to_unix (updated_time);
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
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:url", identifier);

  if (*error != NULL)
    goto out;

  if (ZPJ_IS_SKYDRIVE_FILE (entry))
    {
      gchar *parent_resource_urn, *parent_identifier;
      gchar *mime;
      const gchar *parent_id;

      parent_id = zpj_skydrive_entry_get_parent_id (entry);
      parent_identifier = g_strconcat ("gd:collection:windows-live:skydrive:", parent_id, NULL);
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

      mime = g_content_type_guess (name, NULL, 0, NULL);
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
    }

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:description", zpj_skydrive_entry_get_description (entry));

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nfo:fileName", name);

  if (*error != NULL)
    goto out;

  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     datasource_urn, zpj_skydrive_entry_get_from_name (entry));

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

  created_time = zpj_skydrive_entry_get_created_time (entry);
  date = gom_iso8601_from_timestamp (g_date_time_to_unix (created_time));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:contentCreated", date);
  g_free (date);

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
account_miner_job_traverse_folder (GomAccountMinerJob *job,
                                   TrackerSparqlConnection *connection,
                                   GHashTable *previous_resources,
                                   const gchar *datasource_urn,
                                   const gchar *folder_id,
                                   GCancellable *cancellable,
                                   GError **error)
{
  GList *entries = NULL, *l;
  ZpjSkydrive *skydrive;

  skydrive = ZPJ_SKYDRIVE (g_hash_table_lookup (job->services, "documents"));
  if (skydrive == NULL)
    {
      /* FIXME: use proper #defines and enumerated types */
      g_set_error (error,
                   g_quark_from_static_string ("gom-error"),
                   0,
                   "Can not query without a service");
      goto out;
    }

  entries = zpj_skydrive_list_folder_id (skydrive,
                                         folder_id,
                                         cancellable,
                                         error);

  if (*error != NULL)
    goto out;

  for (l = entries; l != NULL; l = l->next)
    {
      ZpjSkydriveEntry *entry = (ZpjSkydriveEntry *) l->data;
      const gchar *id;

      id = zpj_skydrive_entry_get_id (entry);

      if (ZPJ_IS_SKYDRIVE_FOLDER (entry))
        {
          account_miner_job_traverse_folder (job, connection, previous_resources, datasource_urn, id, cancellable, error);
          if (*error != NULL)
            goto out;
        }
      else if (ZPJ_IS_SKYDRIVE_PHOTO (entry))
        continue;

      account_miner_job_process_entry (job, connection, previous_resources, datasource_urn, entry, cancellable, error);

      if (*error != NULL)
        {
          g_warning ("Unable to process entry %p: %s", l->data, (*error)->message);
          g_clear_error (error);
        }
    }

 out:
  if (entries != NULL)
    g_list_free_full (entries, g_object_unref);
}

static void
query_zpj (GomAccountMinerJob *job,
           TrackerSparqlConnection *connection,
           GHashTable *previous_resources,
           const gchar *datasource_urn,
           GCancellable *cancellable,
           GError **error)
{
  account_miner_job_traverse_folder (job,
                                     connection,
                                     previous_resources,
                                     datasource_urn,
                                     ZPJ_SKYDRIVE_FOLDER_SKYDRIVE,
                                     cancellable,
                                     error);
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GHashTable *services;
  ZpjGoaAuthorizer *authorizer;
  ZpjSkydrive *service;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  if (gom_miner_supports_type (self, "documents"))
    {
      authorizer = zpj_goa_authorizer_new (object);
      service = zpj_skydrive_new (ZPJ_AUTHORIZER (authorizer));

      /* the service takes ownership of the authorizer */
      g_object_unref (authorizer);
      g_hash_table_insert (services, "documents", service);
    }

  return services;
}

static void
gom_zpj_miner_init (GomZpjMiner *self)
{

}

static void
gom_zpj_miner_class_init (GomZpjMinerClass *klass)
{
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);

  miner_class->goa_provider_type = "windows_live";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_services = create_services;
  miner_class->query = query_zpj;
}
