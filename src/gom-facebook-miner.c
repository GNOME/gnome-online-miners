/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2013 Álvaro Peña
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
 * Author: Álvaro Peña <alvaropg@gmail.com>
 *
 */

#include "config.h"

#include <goa/goa.h>
#include <gfbgraph/gfbgraph.h>
#include <gfbgraph/gfbgraph-goa-authorizer.h>

#include "gom-facebook-miner.h"

#define MINER_IDENTIFIER "gd:facebook:miner:9972c7ff-a30f-4dd4-bc77-1adf9dd14364"

G_DEFINE_TYPE (GomFacebookMiner, gom_facebook_miner, GOM_TYPE_MINER)

static gboolean
account_miner_job_process_photo (GomAccountMinerJob *job,
                                 TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GFBGraphPhoto *photo,
                                 const gchar *parent_resource_urn,
                                 const gchar *creator,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GTimeVal new_mtime;
  const gchar *photo_id;
  const gchar *photo_name;
  const gchar *photo_created_time;
  const gchar *photo_updated_time;
  const gchar *photo_link;
  gchar *identifier;
  const gchar *class = "nmm:Photo";
  gchar *resource = NULL;
  gboolean resource_exists, mtime_changed;
  gchar *contact_resource;

  photo_id = gfbgraph_node_get_id (GFBGRAPH_NODE (photo));
  photo_link = gfbgraph_node_get_link (GFBGRAPH_NODE (photo));
  photo_created_time = gfbgraph_node_get_created_time (GFBGRAPH_NODE (photo));
  photo_name = gfbgraph_photo_get_name (photo);

  identifier = g_strdup_printf ("facebook:%s", photo_id);

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
                                 resource_exists, identifier, resource,
                                 cancellable, error);
  if (*error != NULL)
    goto out;

  photo_updated_time = gfbgraph_node_get_updated_time (GFBGRAPH_NODE (photo));
  if (!g_time_val_from_iso8601 (photo_updated_time, &new_mtime))
    g_warning ("Can't convert updated time from ISO 8601 (%s) to a GTimeVal struct",
               photo_updated_time);
  else
    {
      mtime_changed = gom_tracker_update_mtime (connection, new_mtime.tv_sec,
                                                resource_exists, identifier, resource,
                                                cancellable, error);
      if (*error != NULL)
        goto out;

      /* avoid updating the DB if the entry already exists and has not
       * been modified since our last run.
       */
      if (!mtime_changed)
        goto out;
  }

  /* the resource changed - just set all the properties again */
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:url", photo_link);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:isPartOf", parent_resource_urn);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:mimeType", "image/jpeg");

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:title", photo_name);

  if (*error != NULL)
    goto out;

  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     datasource_urn, creator);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nco:creator", contact_resource);

  g_free (contact_resource);
  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:contentCreated", photo_created_time);

  if (*error != NULL)
    goto out;

 out:
  g_free (resource);
  g_free (identifier);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

/* TODO: Until GFBGraph parse the "from" node section, we require the
 *  album creator (generally the logged user)
 */
static gboolean
account_miner_job_process_album (GomAccountMinerJob *job,
                                 TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GFBGraphAlbum *album,
                                 const gchar *creator,
                                 GCancellable *cancellable,
                                 GError **error)
{
  const gchar *album_id;
  const gchar *album_name;
  const gchar *album_description;
  const gchar *album_link;
  const gchar *album_created_time;
  gchar *identifier;
  const gchar *class = "nfo:DataContainer";
  gchar *resource = NULL;
  gboolean resource_exists;
  gchar *contact_resource;
  GList *l;
  GList *photos = NULL;
  GFBGraphAuthorizer *authorizer;

  authorizer = GFBGRAPH_AUTHORIZER (g_hash_table_lookup (job->services, "photos"));
  album_id = gfbgraph_node_get_id (GFBGRAPH_NODE (album));
  album_link = gfbgraph_node_get_link (GFBGRAPH_NODE (album));
  album_created_time = gfbgraph_node_get_created_time (GFBGRAPH_NODE (album));
  album_name = gfbgraph_album_get_name (album);
  album_description = gfbgraph_album_get_description (album);

  identifier = g_strdup_printf ("photos:collection:facebook:%s", album_id);

  /* remove from the list of the previous resources */
  g_hash_table_remove (previous_resources, identifier);

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     datasource_urn, identifier,
     "nfo:RemoteDataObject", class,
     NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (connection, datasource_urn,
                                 resource_exists, identifier, resource,
                                 cancellable, error);

  if (*error != NULL)
    goto out;

  /* TODO: Check updated time to avoid updating the album if has not
   * been modified since our last run
   */

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:url", album_link);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:description", album_description);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:title", album_name);

  if (*error != NULL)
    goto out;

  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     datasource_urn, creator);

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

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:contentCreated", album_created_time);

  if (*error != NULL)
    goto out;

  /* Album photos */
  photos = gfbgraph_node_get_connection_nodes (GFBGRAPH_NODE (album),
                                               GFBGRAPH_TYPE_PHOTO,
                                               authorizer,
                                               error);
  if (*error != NULL)
    goto out;

  for (l = photos; l != NULL; l = l->next)
    {
      GError *local_error = NULL;
      GFBGraphPhoto *photo = GFBGRAPH_PHOTO (l->data);

      account_miner_job_process_photo (job,
                                       connection,
                                       previous_resources,
                                       datasource_urn,
                                       photo,
                                       resource,
                                       creator,
                                       cancellable,
                                       &local_error);
      if (local_error != NULL)
        {
          const gchar *photo_id;

          photo_id = gfbgraph_node_get_id (GFBGRAPH_NODE (photo));
          g_warning ("Unable to process %s: %s", photo_id, local_error->message);
          g_clear_error (&local_error);
        }
    }

 out:
  g_free (resource);
  g_free (identifier);

  g_list_free_full (photos, g_object_unref);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static void
query_facebook (GomAccountMinerJob *job,
                TrackerSparqlConnection *connection,
                GHashTable *previous_resources,
                const gchar *datasource_urn,
                GCancellable *cancellable,
                GError **error)
{
  GFBGraphAuthorizer *authorizer;
  GFBGraphUser *me = NULL;
  const gchar *me_name;
  GList *albums = NULL;
  GList *l = NULL;
  GError *local_error = NULL;

  authorizer = GFBGRAPH_AUTHORIZER (g_hash_table_lookup (job->services, "photos"));
  if (authorizer == NULL)
    {
      /* FIXME: use proper #defines and enumerated types */
      g_set_error (&local_error,
                   g_quark_from_static_string ("gom-error"),
                   0,
                   "Can not query without a service");
      goto out;
    }

  me = gfbgraph_user_get_me (authorizer, &local_error);
  if (local_error != NULL)
    goto out;

  me_name = gfbgraph_user_get_name (me);

  albums = gfbgraph_user_get_albums (me, authorizer, &local_error);
  if (local_error != NULL)
    goto out;

  for (l = albums; l != NULL; l = l->next)
    {
      GFBGraphAlbum *album = GFBGRAPH_ALBUM (l->data);

      account_miner_job_process_album (job,
                                       connection,
                                       previous_resources,
                                       datasource_urn,
                                       album,
                                       me_name,
                                       cancellable,
                                       &local_error);
      if (local_error != NULL)
        {
          const gchar *album_id;

          album_id = gfbgraph_node_get_id (GFBGRAPH_NODE (album));
          g_warning ("Unable to process %s: %s", album_id, local_error->message);
          g_clear_error (&local_error);
        }
    }

 out:
  if (local_error != NULL)
    g_propagate_error (error, local_error);

  g_list_free_full (albums, g_object_unref);
  g_clear_object (&me);
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GFBGraphGoaAuthorizer *authorizer;
  GError *error = NULL;
  GHashTable *services;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  authorizer = gfbgraph_goa_authorizer_new (object);

  if (gom_miner_supports_type (self, "photos"))
    {
      gfbgraph_authorizer_refresh_authorization (GFBGRAPH_AUTHORIZER (authorizer), NULL, &error);
      if (error != NULL)
        {
          g_warning ("Error refreshing authorization (%d): %s", error->code, error->message);
          g_error_free (error);
        }

      g_hash_table_insert (services, "photos", authorizer);
    }

  return services;
}

static void
gom_facebook_miner_init (GomFacebookMiner *miner)
{
}

static void
gom_facebook_miner_class_init (GomFacebookMinerClass *klass)
{
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);

  miner_class->goa_provider_type = "facebook";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 2;

  miner_class->create_services = create_services;
  miner_class->query = query_facebook;
}
