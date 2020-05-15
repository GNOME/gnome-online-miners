/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2014, 2015 Pranav Kant
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
 * Author: Pranav Kant <pranavk@gnome.org>
 *
 */

#include "config.h"

#include <goa/goa.h>

#include "gom-dlna-server.h"
#include "gom-dlna-servers-manager.h"
#include "gom-media-server-miner.h"

#define MINER_IDENTIFIER "gd:media-server:miner:a4a47a3e-eb55-11e3-b983-14feb59cfa0e"

struct _GomMediaServerMinerPrivate {
  GomDlnaServersManager *mngr;
};

G_DEFINE_TYPE_WITH_PRIVATE (GomMediaServerMiner, gom_media_server_miner, GOM_TYPE_MINER)


static gboolean
account_miner_job_process_photo (GomAccountMinerJob *job,
                                 TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GomDlnaPhotoItem *photo,
                                 GCancellable *cancellable,
                                 GError **error)
{
  const gchar *photo_id;
  gchar *identifier;
  const gchar *class = "nmm:Photo";
  gchar *resource = NULL;
  gboolean resource_exists;
  gchar **tmp_arr;

  tmp_arr = g_strsplit_set (photo->path, "/", -1);
  photo_id = tmp_arr[g_strv_length (tmp_arr) - 1];
  identifier = g_strdup_printf ("media-server:%s", photo_id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (previous_resources, identifier);

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

  /* the resource changed - just set all the properties again */
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:url", photo->url);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:mimeType", photo->mimetype);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:title", photo->name);

  if (*error != NULL)
    goto out;

 out:
  g_free (resource);
  g_free (identifier);
  g_strfreev (tmp_arr);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}


static void
query_media_server (GomAccountMinerJob *job,
                    TrackerSparqlConnection *connection,
                    GHashTable *previous_resources,
                    const gchar *datasource_urn,
                    GCancellable *cancellable,
                    GError **error)
{
  GomMediaServerMiner *self = GOM_MEDIA_SERVER_MINER (job->miner);
  GomMediaServerMinerPrivate *priv = self->priv;
  GError *local_error = NULL;
  GoaMediaServer *media_server;
  GList *l;
  GList *photos_list;
  GoaObject *object;
  GomDlnaServer *dlna_server;
  const gchar *udn;

  object = GOA_OBJECT (g_hash_table_lookup (job->services, "photos"));
  if (object == NULL)
    {
      /* FIXME: use proper #defines and enumerated types */
      g_set_error (error,
                   g_quark_from_static_string ("gom-error"),
                   0,
                   "Can not query without a service");
      return;
    }

  media_server = goa_object_get_media_server (object);
  udn = goa_media_server_get_udn (media_server);
  dlna_server = gom_dlna_servers_manager_get_server (priv->mngr, udn);
  if (dlna_server == NULL)
    return; /* Server is offline. */

  photos_list = gom_dlna_server_get_photos (dlna_server);
  for (l = photos_list; l != NULL; l = l->next)
    {
      GomDlnaPhotoItem *photo = (GomDlnaPhotoItem *) l->data;

      account_miner_job_process_photo (job,
                                       connection,
                                       previous_resources,
                                       datasource_urn,
                                       photo,
                                       cancellable,
                                       &local_error);
      if (local_error != NULL)
        {
          g_warning ("Unable to process photo: %s", local_error->message);
          g_clear_error (&local_error);
        }
    }

  g_list_free_full (photos_list, (GDestroyNotify) gom_dlna_photo_item_free);
  g_object_unref (media_server);
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GHashTable *services;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  if (gom_miner_supports_type (self, "photos"))
    g_hash_table_insert (services, "photos", g_object_ref (object));

  return services;
}

static void
gom_media_server_miner_init (GomMediaServerMiner *miner)
{
  miner->priv = gom_media_server_miner_get_instance_private (miner);
  miner->priv->mngr = gom_dlna_servers_manager_dup_singleton ();
}

static void
gom_media_server_miner_class_init (GomMediaServerMinerClass *klass)
{
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);

  miner_class->goa_provider_type = "media-server";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 1;

  miner_class->create_services = create_services;
  miner_class->query = query_media_server;
}
