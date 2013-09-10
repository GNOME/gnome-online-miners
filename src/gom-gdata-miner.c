/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2011, 2012, 2013 Red Hat, Inc.
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
#define STARRED_CATEGORY_TERM "http://schemas.google.com/g/2005/labels#starred"
#define PARENT_LINK_REL "http://schemas.google.com/docs/2007#parent"

G_DEFINE_TYPE (GomGDataMiner, gom_gdata_miner, GOM_TYPE_MINER)

static gboolean
account_miner_job_process_entry (GomAccountMinerJob *job,
                                 GDataDocumentsEntry *doc_entry,
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
      identifier = g_strdup_printf ("gd:collection:%s", gdata_link_get_uri (link));
    }
  else
    identifier = g_strdup (gdata_entry_get_id (entry));

  /* remove from the list of the previous resources */
  g_hash_table_remove (job->previous_resources, identifier);

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
    (job->connection,
     job->cancellable, error,
     &resource_exists,
     job->datasource_urn, identifier,
     "nfo:RemoteDataObject", class, NULL);

  if (*error != NULL)
    goto out;

  gom_tracker_update_datasource (job->connection, job->datasource_urn,
                                 resource_exists, identifier, resource,
                                 job->cancellable, error);

  if (*error != NULL)
    goto out;

  new_mtime = gdata_entry_get_updated (entry);
  mtime_changed = gom_tracker_update_mtime (job->connection, new_mtime,
                                            resource_exists, identifier, resource,
                                            job->cancellable, error);

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
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:url", alternate_uri);

  if (*error != NULL)
    goto out;

  /* fake a drawing mimetype, so Documents can get the correct icon */
  if (GDATA_IS_DOCUMENTS_DRAWING (doc_entry))
    mimetype_override = "application/vnd.sun.xml.draw";
  else if (GDATA_IS_DOCUMENTS_PDF (doc_entry))
    mimetype_override = "application/pdf";

  gom_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:mimeType", mimetype_override);

  if (*error != NULL)
    goto out;

  parents = gdata_entry_look_up_links (entry, PARENT_LINK_REL);
  for (l = parents; l != NULL; l = l->next)
    {
      gchar *parent_resource_urn, *parent_resource_id;

      parent = l->data;
      parent_resource_id =
        g_strdup_printf ("gd:collection:%s", gdata_link_get_uri (parent));

      parent_resource_urn = gom_tracker_sparql_connection_ensure_resource
        (job->connection, job->cancellable, error,
         NULL,
         job->datasource_urn, parent_resource_id,
         "nfo:RemoteDataObject", "nfo:DataContainer", NULL);
      g_free (parent_resource_id);

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (job->connection,
         job->cancellable, error,
         job->datasource_urn, resource,
         "nie:isPartOf", parent_resource_urn);
      g_free (parent_resource_urn);

      if (*error != NULL)
        goto out;
    }

  categories = gdata_entry_get_categories (entry);
  for (l = categories; l != NULL; l = l->next)
    {
      category = l->data;
      if (g_strcmp0 (gdata_category_get_term (category), STARRED_CATEGORY_TERM) == 0)
        {
          starred = TRUE;
          break;
        }
    }

  gom_tracker_sparql_connection_toggle_favorite
    (job->connection,
     job->cancellable, error,
     resource, starred);

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:description", gdata_entry_get_summary (entry));

  if (*error != NULL)
    goto out;

  gom_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
     "nie:title", gdata_entry_get_title (entry));

  if (*error != NULL)
    goto out;

  authors = gdata_entry_get_authors (entry);
  for (l = authors; l != NULL; l = l->next)
    {
      gchar *contact_resource;

      author = l->data;

      contact_resource = gom_tracker_utils_ensure_contact_resource (job->connection,
                                                                    job->cancellable, error,
                                                                    gdata_author_get_email_address (author),
                                                                    gdata_author_get_name (author));

      if (*error != NULL)
        goto out;

      gom_tracker_sparql_connection_insert_or_replace_triple
        (job->connection,
         job->cancellable, error,
         job->datasource_urn, resource,
         "nco:creator", contact_resource);

      if (*error != NULL)
        goto out;

      g_free (contact_resource);
    }

  access_rules = gdata_access_handler_get_rules (GDATA_ACCESS_HANDLER (entry),
                                                 GDATA_SERVICE (job->service),
                                                 job->cancellable,
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

      contact_resource = gom_tracker_utils_ensure_contact_resource (job->connection,
                                                                    job->cancellable, error,
                                                                    scope_value,
                                                                    "");

      gom_tracker_sparql_connection_insert_or_replace_triple
        (job->connection,
         job->cancellable, error,
         job->datasource_urn, resource,
         "nco:contributor", contact_resource);

      g_free (contact_resource);

      if (*error != NULL)
        goto out;
    }

  date = gom_iso8601_from_timestamp (gdata_entry_get_published (entry));
  gom_tracker_sparql_connection_insert_or_replace_triple
    (job->connection,
     job->cancellable, error,
     job->datasource_urn, resource,
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

static void
query_gdata (GomAccountMinerJob *job,
             GError **error)
{
  GDataDocumentsQuery *query;
  GDataDocumentsFeed *feed;
  GList *entries, *l;

  query = gdata_documents_query_new (NULL);
  gdata_documents_query_set_show_folders (query, TRUE);
  feed = gdata_documents_service_query_documents
    (GDATA_DOCUMENTS_SERVICE (job->service), query,
     job->cancellable, NULL, NULL, error);

  g_object_unref (query);

  if (feed == NULL)
    return;

  entries = gdata_feed_get_entries (GDATA_FEED (feed));
  for (l = entries; l != NULL; l = l->next)
    {
      account_miner_job_process_entry (job, l->data, error);

      if (*error != NULL)
        {
          g_warning ("Unable to process entry %p: %s", l->data, (*error)->message);
          g_clear_error (error);
        }
    }

  g_object_unref (feed);
}

static GObject *
create_service (GomMiner *self,
                GoaObject *object)
{
  GDataGoaAuthorizer *authorizer;
  GDataDocumentsService *service;

  authorizer = gdata_goa_authorizer_new (object);
  service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));

  /* the service takes ownership of the authorizer */
  g_object_unref (authorizer);

  return G_OBJECT (service);
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
  miner_class->version = 3;

  miner_class->create_service = create_service;
  miner_class->query = query_gdata;
}
