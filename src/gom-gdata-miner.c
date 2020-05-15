/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2011, 2012, 2013, 2014, 2015 Red Hat, Inc.
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
 * Author: Cosimo Cecchi <cosimoc@redhat.com>
 *
 */

#include "config.h"

#include <gdata/gdata.h>

#include "gom-utils.h"
#include "gom-gdata-miner.h"

#define MINER_IDENTIFIER "gd:gdata:miner:86ec9bc9-c242-427f-aa19-77b5a2c9b6f0"
#define PARENT_LINK_REL "http://schemas.google.com/docs/2007#parent"

/* used by applications to identify the source of an entry */
#define PREFIX_DRIVE "google:drive:"
#define PREFIX_PICASAWEB "google:picasaweb:"

static const guint MAX_RESULTS = 50;

G_DEFINE_TYPE (GomGDataMiner, gom_gdata_miner, GOM_TYPE_MINER)

static gchar *
generate_fake_email_from_fullname (const gchar *fullname)
{
  GChecksum *checksum;
  const gchar *digest;
  gchar *retval;

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, fullname, -1);
  digest = g_checksum_get_string (checksum);
  retval = g_strdup (digest);
  g_checksum_free (checksum);
  return retval;
}

static gboolean
account_miner_job_process_entry (TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GDataDocumentsService *service,
                                 GDataDocumentsEntry *doc_entry,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GDataEntry *entry = GDATA_ENTRY (doc_entry);
  gchar *resource = NULL;
  gchar *date, *identifier;
  const gchar *class = NULL;
  const gchar *mimetype_override = NULL;
  gboolean mtime_changed, resource_exists;
  gint64 new_mtime;

  GList *authors, *l, *parents = NULL;
  GDataAuthor *author;
  GDataLink *parent;

  GDataLink *alternate;
  const gchar *alternate_uri;

  GList *categories;
  GDataCategory *category;
  gboolean starred = FALSE;

  GDataFeed *access_rules = NULL;

  if (GDATA_IS_DOCUMENTS_FOLDER (doc_entry))
    {
      GDataLink *link;

      link = gdata_entry_look_up_link (entry, GDATA_LINK_SELF);
      identifier = g_strdup_printf ("gd:collection:%s%s", PREFIX_DRIVE, gdata_link_get_uri (link));
    }
  else
    {
      const gchar *id;

      id = gdata_entry_get_id (entry);
      identifier = g_strdup_printf ("%s%s", PREFIX_DRIVE, id);
    }

  /* remove from the list of the previous resources, if any */
  if (previous_resources != NULL)
    g_hash_table_remove (previous_resources, identifier);

  if (GDATA_IS_DOCUMENTS_PRESENTATION (doc_entry))
    class = "nfo:Presentation";
  else if (GDATA_IS_DOCUMENTS_SPREADSHEET (doc_entry))
    class = "nfo:Spreadsheet";
  else if (GDATA_IS_DOCUMENTS_TEXT (doc_entry))
    class = "nfo:PaginatedTextDocument";
  else if (GDATA_IS_DOCUMENTS_DRAWING (doc_entry))
    class = "nfo:PaginatedTextDocument";
  else if (GDATA_IS_DOCUMENTS_PDF (doc_entry))
    class = "nfo:PaginatedTextDocument";
  else if (GDATA_IS_DOCUMENTS_FOLDER (doc_entry))
    class = "nfo:DataContainer";

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     TRACKER_DOCUMENTS_GRAPH, identifier,
     "nfo:RemoteDataObject", class, NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (connection, datasource_urn,
                                 resource_exists, identifier, resource,
                                 cancellable, error);

  if (*error != NULL)
    goto out;

  new_mtime = gdata_entry_get_updated (entry);
  mtime_changed = gom_tracker_update_mtime (connection, new_mtime,
                                            resource_exists, identifier, resource,
                                            cancellable, error);

  if (*error != NULL)
    goto out;

  /* avoid updating the DB if the entry already exists and has not
   * been modified since our last run.
   */
  if (!mtime_changed)
    goto out;

  /* the resource changed - just set all the properties again */
  alternate = gdata_entry_look_up_link (entry, GDATA_LINK_ALTERNATE);
  alternate_uri = gdata_link_get_uri (alternate);

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:url", alternate_uri);

  if (*error != NULL)
    goto out;

  /* fake a drawing mimetype, so Documents can get the correct icon */
  if (GDATA_IS_DOCUMENTS_DRAWING (doc_entry))
    mimetype_override = "application/vnd.sun.xml.draw";
  else if (GDATA_IS_DOCUMENTS_PDF (doc_entry))
    mimetype_override = "application/pdf";

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:mimeType", mimetype_override);

  if (*error != NULL)
    goto out;

  parents = gdata_entry_look_up_links (entry, PARENT_LINK_REL);
  for (l = parents; l != NULL; l = l->next)
    {
      gchar *parent_resource_urn, *parent_resource_id;

      parent = l->data;
      parent_resource_id =
        g_strdup_printf ("gd:collection:%s%s", PREFIX_DRIVE, gdata_link_get_uri (parent));

      parent_resource_urn = gom_tracker_sparql_connection_ensure_resource
        (connection, cancellable, error,
         NULL,
         GOM_GRAPH, parent_resource_id,
         "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
      g_free (parent_resource_id);

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         datasource_urn, resource,
         "nie:isPartOf", parent_resource_urn);
      g_free (parent_resource_urn);

      if (*error != NULL)
        goto out;
    }

  categories = gdata_entry_get_categories (entry);
  for (l = categories; l != NULL; l = l->next)
    {
      category = l->data;
      if (g_strcmp0 (gdata_category_get_term (category), GDATA_CATEGORY_SCHEMA_LABELS_STARRED) == 0)
        {
          starred = TRUE;
          break;
        }
    }

  gom_tracker_sparql_connection_toggle_favorite
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource, starred);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:description", gdata_entry_get_summary (entry));

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:title", gdata_entry_get_title (entry));

  if (*error != NULL)
    goto out;

  authors = gdata_entry_get_authors (entry);
  for (l = authors; l != NULL; l = l->next)
    {
      gchar *contact_resource;

      author = l->data;

      contact_resource = gom_tracker_utils_ensure_contact_resource (connection,
                                                                    cancellable, error,
                                                                    gdata_author_get_email_address (author),
                                                                    gdata_author_get_name (author));

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         datasource_urn, resource,
         "nco:creator", contact_resource);

      if (*error != NULL)
        goto out;

      g_free (contact_resource);
    }

  access_rules = gdata_access_handler_get_rules (GDATA_ACCESS_HANDLER (entry),
                                                 GDATA_SERVICE (service),
                                                 cancellable,
                                                 NULL, NULL, error);

  if (*error != NULL)
      goto out;

  for (l = gdata_feed_get_entries (access_rules); l != NULL; l = l->next)
    {
      GDataAccessRule *rule = l->data;
      const gchar *scope_type;
      const gchar *scope_value;
      gchar *contact_resource;

      gdata_access_rule_get_scope (rule, &scope_type, &scope_value);

      /* default scope access means the document is completely public */
      if (g_strcmp0 (scope_type, GDATA_ACCESS_SCOPE_DEFAULT) == 0)
        continue;

      /* skip domain scopes */
      if (g_strcmp0 (scope_type, GDATA_ACCESS_SCOPE_DOMAIN) == 0)
        continue;

      contact_resource = gom_tracker_utils_ensure_contact_resource (connection,
                                                                    cancellable, error,
                                                                    scope_value,
                                                                    "");

      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         datasource_urn, resource,
         "nco:contributor", contact_resource);

      g_free (contact_resource);

      if (*error != NULL)
        goto out;
    }

  date = gom_iso8601_from_timestamp (gdata_entry_get_published (entry));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:contentCreated", date);
  g_free (date);

  if (*error != NULL)
    goto out;

 out:
  g_clear_object (&access_rules);
  g_free (resource);
  g_free (identifier);

  g_list_free (parents);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static gchar *
account_miner_job_process_photo (TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GDataPicasaWebFile *photo,
                                 const gchar *parent_resource_urn,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GList *l, *media_contents;
  gchar *resource = NULL, *equipment_resource = NULL;
  gchar *contact_resource, *date, *identifier = NULL;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;
  gint64 timestamp;

  const gchar *flash_off = "http://www.tracker-project.org/temp/nmm#flash-off";
  const gchar *flash_on = "http://www.tracker-project.org/temp/nmm#flash-on";

  gboolean flash;
  const gchar *credit;
  const gchar *id;
  const gchar *make;
  const gchar *model;
  const gchar *title;
  const gchar *summary;
  const gchar *mime;

  gchar *email;
  gchar *exposure;
  gchar *focal_length;
  gchar *fstop;
  gchar *iso;
  gchar *width;
  gchar *height;

  GDataLink *alternate;
  const gchar *alternate_uri;

  id = gdata_entry_get_id (GDATA_ENTRY (photo));

  media_contents = gdata_picasaweb_file_get_contents (photo);
  for (l = media_contents; l != NULL; l = l->next)
    {
      GDataMediaContent *media_content = GDATA_MEDIA_CONTENT (l->data);
      GDataMediaMedium medium;

      medium = gdata_media_content_get_medium (media_content);
      if (medium != GDATA_MEDIA_IMAGE)
        {
          g_debug ("Skipping %s because medium(%d) is not an image", id, medium);
          goto out;
        }
    }

  identifier = g_strdup_printf ("%s%s", PREFIX_PICASAWEB, id);

  /* remove from the list of the previous resources, if any */
  if (previous_resources != NULL)
    g_hash_table_remove (previous_resources, identifier);

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     TRACKER_PICTURES_GRAPH, identifier,
     "nfo:RemoteDataObject", "nmm:Photo", NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (connection, datasource_urn,
                                 resource_exists, TRACKER_PICTURES_GRAPH, resource,
                                 cancellable, error);
  if (*error != NULL)
    goto out;

  /* Check updated time to avoid updating the DB if it has not
   * been modified since our last run
   */
  new_mtime = gdata_entry_get_updated (GDATA_ENTRY (photo));
  mtime_changed = gom_tracker_update_mtime (connection, new_mtime,
                                            resource_exists, identifier, resource,
                                            cancellable, error);

  if (*error != NULL)
    goto out;

  /* avoid updating the DB if the resource already exists and has not
   * been modified since our last run.
   */
  if (!mtime_changed)
    goto out;

  /* the resource changed - just set all the properties again */
  alternate = gdata_entry_look_up_link (GDATA_ENTRY (photo), GDATA_LINK_ALTERNATE);
  alternate_uri = gdata_link_get_uri (alternate);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:url", alternate_uri);

  if (*error != NULL)
    goto out;

  summary = gdata_entry_get_summary ((GDATA_ENTRY (photo)));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:description", summary);

  if (*error != NULL)
    goto out;

  if (parent_resource_urn != NULL)
    {
      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         datasource_urn, resource,
         "nie:isPartOf", parent_resource_urn);

      if (*error != NULL)
        goto out;
    }

  mime = gdata_media_content_get_content_type (GDATA_MEDIA_CONTENT (media_contents->data));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:mimeType", mime);

  if (*error != NULL)
    goto out;

  title = gdata_entry_get_title ((GDATA_ENTRY (photo)));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:title", title);

  if (*error != NULL)
    goto out;

  credit = gdata_picasaweb_file_get_credit (photo);
  email = generate_fake_email_from_fullname (credit);
  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     email, credit);
  g_free (email);

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

  exposure = g_strdup_printf ("%f", gdata_picasaweb_file_get_exposure (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nmm:exposureTime", exposure);
  g_free (exposure);

  if (*error != NULL)
    goto out;

  focal_length = g_strdup_printf ("%f", gdata_picasaweb_file_get_focal_length (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nmm:focalLength", focal_length);
  g_free (focal_length);

  if (*error != NULL)
    goto out;

  fstop = g_strdup_printf ("%f", gdata_picasaweb_file_get_fstop (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nmm:fnumber", fstop);
  g_free (fstop);

  if (*error != NULL)
    goto out;

  iso = g_strdup_printf ("%ld", (glong) gdata_picasaweb_file_get_iso (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nmm:isoSpeed", iso);
  g_free (iso);

  if (*error != NULL)
    goto out;

  flash = gdata_picasaweb_file_get_flash (photo);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nmm:flash", flash ? flash_on : flash_off);

  if (*error != NULL)
    goto out;

  make = gdata_picasaweb_file_get_make (photo);
  model = gdata_picasaweb_file_get_model (photo);

  if (make != NULL || model != NULL)
    {
      equipment_resource = gom_tracker_utils_ensure_equipment_resource (connection,
                                                                        cancellable,
                                                                        error,
                                                                        TRACKER_PICTURES_GRAPH,
                                                                        make,
                                                                        model);

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (connection,
         cancellable, error,
         datasource_urn, resource,
         "nfo:equipment", equipment_resource);

      if (*error != NULL)
        goto out;
    }

  width = g_strdup_printf ("%u", gdata_picasaweb_file_get_width (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nfo:width", width);
  g_free (width);

  if (*error != NULL)
    goto out;

  height = g_strdup_printf ("%u", gdata_picasaweb_file_get_height (photo));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nfo:height", height);
  g_free (height);

  if (*error != NULL)
    goto out;

  timestamp = gdata_picasaweb_file_get_timestamp (photo);
  date = gom_iso8601_from_timestamp (timestamp / 1000);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     datasource_urn, resource,
     "nie:contentCreated", date);
  g_free (date);

  if (*error != NULL)
    goto out;

 out:
  g_free (identifier);
  g_free (equipment_resource);

  if (*error != NULL)
    return NULL;

  return resource;
}

static gboolean
account_miner_job_process_album (TrackerSparqlConnection *connection,
                                 GHashTable *previous_resources,
                                 const gchar *datasource_urn,
                                 GDataPicasaWebService *service,
                                 GDataPicasaWebAlbum *album,
                                 GCancellable *cancellable,
                                 GError **error)
{
  GDataFeed *feed = NULL;
  GDataPicasaWebQuery *query;
  gchar *resource = NULL;
  gchar *contact_resource, *date, *identifier;
  gchar *email;
  gboolean resource_exists, mtime_changed;
  gint64 new_mtime;
  gint64 timestamp;

  const gchar *album_id;
  const gchar *nickname;
  const gchar *title;
  const gchar *summary;

  GList *l, *photos = NULL;

  GDataLink *alternate;
  const gchar *alternate_uri;

  album_id = gdata_entry_get_id (GDATA_ENTRY (album));
  identifier = g_strdup_printf ("photos:collection:%s%s", PREFIX_PICASAWEB, album_id);

  /* remove from the list of the previous resources, if any */
  if (previous_resources != NULL)
    g_hash_table_remove (previous_resources, identifier);

  resource = gom_tracker_sparql_connection_ensure_resource
    (connection,
     cancellable, error,
     &resource_exists,
     TRACKER_PICTURES_GRAPH, identifier,
     "nfo:RemoteDataObject", "nfo:DataContainer",
     NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource
    (connection, datasource_urn,
     resource_exists, TRACKER_PICTURES_GRAPH, resource,
     cancellable, error);

  if (*error != NULL)
    goto out;

  /* Check updated time to avoid updating the DB if it has not
   * been modified since our last run
   */
  new_mtime = gdata_entry_get_updated (GDATA_ENTRY (album));
  mtime_changed = gom_tracker_update_mtime (connection, new_mtime,
                                            resource_exists, TRACKER_PICTURES_GRAPH, resource,
                                            cancellable, error);

  if (*error != NULL)
    goto out;

  /* avoid updating the DB if the resource already exists and has not
   * been modified since our last run.
   */
  if (!mtime_changed)
    goto album_photos;

  /* the resource changed - just set all the properties again */
  alternate = gdata_entry_look_up_link (GDATA_ENTRY (album), GDATA_LINK_ALTERNATE);
  alternate_uri = gdata_link_get_uri (alternate);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:url", alternate_uri);

  if (*error != NULL)
    goto out;

  summary = gdata_entry_get_summary ((GDATA_ENTRY (album)));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:description", summary);

  if (*error != NULL)
    goto out;

  title = gdata_entry_get_title ((GDATA_ENTRY (album)));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:title", title);

  if (*error != NULL)
    goto out;

  nickname = gdata_picasaweb_album_get_nickname (album);
  email = generate_fake_email_from_fullname (nickname);
  contact_resource = gom_tracker_utils_ensure_contact_resource
    (connection,
     cancellable, error,
     email, nickname);
  g_free (email);

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

  timestamp = gdata_picasaweb_album_get_timestamp (album);
  date = gom_iso8601_from_timestamp (timestamp / 1000);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection,
     cancellable, error,
     TRACKER_PICTURES_GRAPH, resource,
     "nie:contentCreated", date);
  g_free (date);

  if (*error != NULL)
    goto out;

 album_photos:
  query = gdata_picasaweb_query_new (NULL);
  gdata_picasaweb_query_set_image_size (query, "d");
  feed = gdata_picasaweb_service_query_files (service, album, GDATA_QUERY (query),
                                              cancellable, NULL, NULL, error);

  g_object_unref (query);

  if (feed == NULL)
    goto out;

  photos = gdata_feed_get_entries (feed);
  for (l = photos; l != NULL; l = l->next)
    {
      GDataPicasaWebFile *file = GDATA_PICASAWEB_FILE (l->data);
      gchar *photo_resource_urn = NULL;

      photo_resource_urn = account_miner_job_process_photo (connection,
                                                            previous_resources,
                                                            datasource_urn,
                                                            file,
                                                            resource,
                                                            cancellable,
                                                            error);

      if (*error != NULL)
        {
          const gchar *photo_id;

          photo_id = gdata_picasaweb_file_get_id (file);
          g_warning ("Unable to process photo %s: %s", photo_id, (*error)->message);
          g_clear_error (error);
        }

      g_free (photo_resource_urn);
    }

 out:
  g_clear_object (&feed);
  g_free (resource);
  g_free (identifier);

  if (*error != NULL)
    return FALSE;

  return TRUE;
}

static void
insert_shared_content_photos (TrackerSparqlConnection *connection,
                              const gchar *datasource_urn,
                              const gchar *shared_id,
                              const gchar *source_urn,
                              GDataPicasaWebService *service,
                              GCancellable *cancellable,
                              GError **error)
{
  GError *local_error;
  GDataAuthorizationDomain *authorization_domain;
  GDataEntry *entry = NULL;
  GDataPicasaWebFile *file;
  GDataPicasaWebQuery *query = NULL;
  gchar *photo_resource_urn = NULL;

  authorization_domain = gdata_picasaweb_service_get_primary_authorization_domain ();

  query = gdata_picasaweb_query_new (NULL);
  gdata_picasaweb_query_set_image_size (query, "d");

  local_error = NULL;
  entry = gdata_service_query_single_entry (GDATA_SERVICE (service),
                                            authorization_domain,
                                            shared_id,
                                            GDATA_QUERY (query),
                                            GDATA_TYPE_PICASAWEB_FILE,
                                            cancellable,
                                            &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  file = GDATA_PICASAWEB_FILE (entry);

  local_error = NULL;
  photo_resource_urn = account_miner_job_process_photo (connection,
                                                        NULL,
                                                        datasource_urn,
                                                        file,
                                                        NULL,
                                                        cancellable,
                                                        &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  local_error = NULL;
  if (!gom_tracker_sparql_connection_insert_or_replace_triple (connection,
                                                               cancellable,
                                                               &local_error,
                                                               TRACKER_PICTURES_GRAPH,
                                                               source_urn,
                                                               "nie:relatedTo",
                                                               photo_resource_urn))
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  local_error = NULL;
  if (!gom_tracker_sparql_connection_insert_or_replace_triple (connection,
                                                               cancellable,
                                                               &local_error,
                                                               TRACKER_PICTURES_GRAPH,
                                                               photo_resource_urn,
                                                               "nie:links",
                                                               source_urn))
    {
      g_propagate_error (error, local_error);
      goto out;
    }

 out:
  g_clear_object (&entry);
  g_clear_object (&query);
  g_free (photo_resource_urn);
}

static void
insert_shared_content (GomMiner *miner,
                       gpointer service,
                       TrackerSparqlConnection *connection,
                       const gchar *datasource_urn,
                       const gchar *shared_id,
                       const gchar *shared_type,
                       const gchar *source_urn,
                       GCancellable *cancellable,
                       GError **error)
{
  if (g_strcmp0 (shared_type, "photos") == 0)
    insert_shared_content_photos (connection,
                                  datasource_urn,
                                  shared_id,
                                  source_urn,
                                  GDATA_PICASAWEB_SERVICE (service),
                                  cancellable,
                                  error);
}

static void
query_gdata_documents (GomAccountMinerJob *job,
                       TrackerSparqlConnection *connection,
                       GHashTable *previous_resources,
                       const gchar *datasource_urn,
                       GDataDocumentsService *service,
                       GCancellable *cancellable,
                       GError **error)
{
  GDataDocumentsQuery *query = NULL;
  GDataDocumentsFeed *feed = NULL;
  GList *entries, *l;
  gboolean succeeded_once = FALSE;

  query = gdata_documents_query_new_with_limits (NULL, 1, MAX_RESULTS);
  gdata_documents_query_set_show_folders (query, TRUE);

  while (TRUE)
    {
      GError *local_error;

      local_error = NULL;
      feed = gdata_documents_service_query_documents
        (service, query,
         cancellable, NULL, NULL, &local_error);
      if (local_error != NULL)
        {
          if (succeeded_once)
            {
              g_warning ("Unable to query: %s", local_error->message);
              g_error_free (local_error);
            }
          else
            {
              g_propagate_error (error, local_error);
            }

          break;
        }

      succeeded_once = TRUE;

      entries = gdata_feed_get_entries (GDATA_FEED (feed));
      if (entries == NULL)
        break;

      for (l = entries; l != NULL; l = l->next)
        {
          local_error = NULL;
          account_miner_job_process_entry (connection,
                                           previous_resources,
                                           datasource_urn,
                                           service,
                                           l->data,
                                           cancellable,
                                           &local_error);

          if (local_error != NULL)
            {
              g_warning ("Unable to process entry %p: %s", l->data, local_error->message);
              g_error_free (local_error);
            }
        }

      gdata_query_next_page (GDATA_QUERY (query));
      g_clear_object (&feed);
    }

 out:
  g_clear_object (&feed);
  g_clear_object (&query);
}

static void
query_gdata_photos (GomAccountMinerJob *job,
                    TrackerSparqlConnection *connection,
                    GHashTable *previous_resources,
                    const gchar *datasource_urn,
                    GDataPicasaWebService *service,
                    GCancellable *cancellable,
                    GError **error)
{
  GDataFeed *feed;
  GList *albums, *l;

  feed = gdata_picasaweb_service_query_all_albums (service, NULL, NULL, cancellable, NULL, NULL, error);

  if (feed == NULL)
    return;

  albums = gdata_feed_get_entries (feed);
  for (l = albums; l != NULL; l = l->next)
    {
      GDataPicasaWebAlbum *album = GDATA_PICASAWEB_ALBUM (l->data);

      account_miner_job_process_album (connection,
                                       previous_resources,
                                       datasource_urn,
                                       service,
                                       album,
                                       cancellable,
                                       error);

      if (*error != NULL)
        {
          const gchar *album_id;

          album_id = gdata_picasaweb_album_get_id (album);
          g_warning ("Unable to process album %s: %s", album_id, (*error)->message);
          g_clear_error (error);
        }
    }

  g_object_unref (feed);
}

static void
query_gdata (GomAccountMinerJob *job,
             TrackerSparqlConnection *connection,
             GHashTable *previous_resources,
             const gchar *datasource_urn,
             GCancellable *cancellable,
             GError **error)
{
  gpointer service;

  service = g_hash_table_lookup (job->services, "documents");
  if (service != NULL)
    query_gdata_documents (job,
                           connection,
                           previous_resources,
                           datasource_urn,
                           GDATA_DOCUMENTS_SERVICE (service),
                           cancellable,
                           error);

  service = g_hash_table_lookup (job->services, "photos");
  if (service != NULL)
    query_gdata_photos (job,
                        connection,
                        previous_resources,
                        datasource_urn,
                        GDATA_PICASAWEB_SERVICE (service),
                        cancellable,
                        error);
}

static gpointer
create_service (GomMiner *miner, GoaObject *object, const gchar *type)
{
  GDataGoaAuthorizer *authorizer;
  gpointer service = NULL;

  authorizer = gdata_goa_authorizer_new (object);

  if (g_strcmp0 (type, "documents") == 0)
    service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));

  if (g_strcmp0 (type, "photos") == 0)
    service = gdata_picasaweb_service_new (GDATA_AUTHORIZER (authorizer));

  g_object_unref (authorizer);
  return service;
}

static GHashTable *
create_services (GomMiner *self,
                 GoaObject *object)
{
  GDataGoaAuthorizer *authorizer;
  GHashTable *services;

  services = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, (GDestroyNotify) g_object_unref);

  authorizer = gdata_goa_authorizer_new (object);

  if (gom_miner_supports_type (self, "documents") && goa_object_peek_files (object) != NULL)
    {
      GDataDocumentsService *service;

      service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));
      g_hash_table_insert (services, "documents", service);
    }

  if (gom_miner_supports_type (self, "photos") && goa_object_peek_photos (object) != NULL)
    {
      GDataPicasaWebService *service;

      service = gdata_picasaweb_service_new (GDATA_AUTHORIZER (authorizer));
      g_hash_table_insert (services, "photos", service);
    }

  /* the service takes ownership of the authorizer */
  g_object_unref (authorizer);
  return services;
}

static void
destroy_service (GomMiner *miner, gpointer service)
{
  g_object_unref (service);
}

static void
gom_gdata_miner_init (GomGDataMiner *miner)
{
}

static void
gom_gdata_miner_class_init (GomGDataMinerClass *klass)
{
  GomMinerClass *miner_class = GOM_MINER_CLASS (klass);

  miner_class->goa_provider_type = "google";
  miner_class->miner_identifier = MINER_IDENTIFIER;
  miner_class->version = 5;

  miner_class->create_service = create_service;
  miner_class->create_services = create_services;
  miner_class->destroy_service = destroy_service;
  miner_class->insert_shared_content = insert_shared_content;
  miner_class->query = query_gdata;
}
