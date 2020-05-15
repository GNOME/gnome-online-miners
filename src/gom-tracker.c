/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (c) 2011, 2012 Red Hat, Inc.
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

#include <glib.h>

#include "gom-tracker.h"
#include "gom-utils.h"

static gboolean
gom_tracker_sparql_connection_get_string_attribute (TrackerSparqlConnection *connection,
                                                    GCancellable *cancellable,
                                                    GError **error,
                                                    const gchar *graph,
                                                    const gchar *resource,
                                                    const gchar *attribute,
                                                    gchar **value)
{
  GString *select = g_string_new (NULL);
  TrackerSparqlCursor *cursor;
  const gchar *string_value = NULL;
  gboolean res;

  g_string_append_printf (select, "SELECT ?val { GRAPH <%s> { <%s> %s ?val } }",
                          graph, resource, attribute);
  cursor = tracker_sparql_connection_query (connection,
                                            select->str,
                                            cancellable, error);
  g_string_free (select, TRUE);

  if (*error != NULL)
    goto out;

  res = tracker_sparql_cursor_next (cursor, cancellable, error);

  if (*error != NULL)
    goto out;

  if (res)
    {
      string_value = tracker_sparql_cursor_get_string (cursor, 0, NULL);
      goto out;
    }

 out:
  if (string_value != NULL && value != NULL)
    *value = g_strdup (string_value);
  else if (string_value == NULL)
    res = FALSE;

  g_clear_object (&cursor);
  return res;
}

gchar *
gom_tracker_sparql_connection_ensure_resource (TrackerSparqlConnection *connection,
                                               GCancellable *cancellable,
                                               GError **error,
                                               gboolean *resource_exists,
                                               const gchar *graph,
                                               const gchar *identifier,
                                               const gchar *class,
                                               ...)
{
  GString *select, *inner;
  gchar *insert;
  va_list args;
  const gchar *arg;
  TrackerSparqlCursor *cursor;
  gboolean res;
  gchar *retval = NULL;
  gchar *graph_str;
  GVariant *insert_res;
  GVariantIter *iter;
  gchar *key = NULL, *val = NULL;
  gboolean exists = FALSE;

  g_return_val_if_fail (graph != NULL, NULL);

  /* build the inner query with all the classes */
  va_start (args, class);
  inner = g_string_new (NULL);

  for (arg = class; arg != NULL; arg = va_arg (args, const gchar *))
    g_string_append_printf (inner, " a %s; ", arg);

  g_string_append_printf (inner, "nao:identifier \"%s\"", identifier);

  va_end (args);

  /* query if such a resource is already in the DB */
  select = g_string_new (NULL);
  g_string_append_printf (select,
                          "SELECT ?urn WHERE { GRAPH <%s> { ?urn %s } }", graph, inner->str);

  cursor = tracker_sparql_connection_query (connection,
                                            select->str,
                                            cancellable, error);

  g_string_free (select, TRUE);

  if (*error != NULL)
    goto out;

  res = tracker_sparql_cursor_next (cursor, cancellable, error);

  if (*error != NULL)
    goto out;

  if (res)
    {
      /* return the found resource */
      retval = g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL));
      exists = TRUE;
      g_debug ("Found resource in the store: %s", retval);
      goto out;
    }

  /* not found, create the resource */
  insert = g_strdup_printf ("INSERT INTO <%s> { _:res %s }",
                            graph, inner->str);
  insert_res =
    tracker_sparql_connection_update_blank (connection, insert,
                                            G_PRIORITY_DEFAULT, NULL, error);
  g_free (insert);

  if (*error != NULL)
    goto out;

  /* the result is an "aaa{ss}" variant */
  g_variant_get (insert_res, "aaa{ss}", &iter);
  g_variant_iter_next (iter, "aa{ss}", &iter);
  g_variant_iter_next (iter, "a{ss}", &iter);
  g_variant_iter_next (iter, "{ss}", &key, &val);

  g_variant_iter_free (iter);
  g_variant_unref (insert_res);

  if (g_strcmp0 (key, "res") == 0)
    {
      retval = val;
    }
  else
    {
      g_free (val);
      goto out;
    }

  g_debug ("Created a new resource: %s", retval);

 out:
  if (resource_exists)
    *resource_exists = exists;

  g_clear_object (&cursor);
  return retval;
}

gboolean
gom_tracker_sparql_connection_insert_or_replace_triple (TrackerSparqlConnection *connection,
                                                        GCancellable *cancellable,
                                                        GError **error,
                                                        const gchar *graph,
                                                        const gchar *resource,
                                                        const gchar *property_name,
                                                        const gchar *property_value)
{
  GString *insert;
  gchar *quoted;
  gboolean retval = TRUE;

  g_return_val_if_fail (graph != NULL, FALSE);

  /* the "null" value must not be quoted */
  if (property_value == NULL)
    quoted = g_strdup ("null");
  else
    quoted = g_strdup_printf ("\"%s\"", property_value);

  insert = g_string_new (NULL);
  g_string_append_printf
    (insert,
     "INSERT OR REPLACE INTO <%s> { <%s> a nie:InformationElement, nie:DataObject ; %s %s }",
     graph, resource, property_name, quoted);
  g_free (quoted);

  g_debug ("Insert or replace triple: query %s", insert->str);

  tracker_sparql_connection_update (connection, insert->str,
                                    G_PRIORITY_DEFAULT, cancellable,
                                    error);

  g_string_free (insert, TRUE);

  if (*error != NULL)
    retval = FALSE;

  return retval;
}

gboolean
gom_tracker_sparql_connection_set_triple (TrackerSparqlConnection *connection,
                                          GCancellable *cancellable,
                                          GError **error,
                                          const gchar *graph,
                                          const gchar *resource,
                                          const gchar *property_name,
                                          const gchar *property_value)
{
  GString *delete;
  gboolean retval = TRUE;

  delete = g_string_new (NULL);
  g_string_append_printf
    (delete,
     "DELETE { <%s> %s ?val } WHERE { <%s> %s ?val }", resource,
     property_name, resource, property_name);

  tracker_sparql_connection_update (connection, delete->str,
                                    G_PRIORITY_DEFAULT, cancellable,
                                    error);

  g_string_free (delete, TRUE);
  if (*error != NULL)
    {
      retval = FALSE;
      goto out;
    }

  retval =
    gom_tracker_sparql_connection_insert_or_replace_triple (connection,
                                                            cancellable, error,
                                                            graph, resource,
                                                            property_name, property_value);

 out:
  return retval;
}

gboolean
gom_tracker_sparql_connection_toggle_favorite (TrackerSparqlConnection *connection,
                                               GCancellable *cancellable,
                                               GError **error,
                                               const gchar *graph,
                                               const gchar *resource,
                                               gboolean favorite)
{
  GString *update;
  const gchar *op_str = NULL;
  gboolean retval = TRUE;

  if (favorite)
    op_str = "INSERT OR REPLACE INTO";
  else
    op_str = "DELETE FROM";

  update = g_string_new (NULL);
  g_string_append_printf
    (update,
     "%s <%s> { <%s> nao:hasTag nao:predefined-tag-favorite }",
     op_str, graph, resource);

  g_debug ("Toggle favorite: query %s", update->str);

  tracker_sparql_connection_update (connection, update->str,
                                    G_PRIORITY_DEFAULT, cancellable,
                                    error);

  g_string_free (update, TRUE);

  if (*error != NULL)
    retval = FALSE;

  return retval;
}

gchar*
gom_tracker_utils_ensure_contact_resource (TrackerSparqlConnection *connection,
                                           GCancellable *cancellable,
                                           GError **error,
                                           const gchar *email,
                                           const gchar *fullname)
{
  GString *select, *insert;
  TrackerSparqlCursor *cursor = NULL;
  gchar *retval = NULL, *mail_uri = NULL;
  gboolean res;
  GVariant *insert_res;
  GVariantIter *iter;
  gchar *key = NULL, *val = NULL;

  mail_uri = g_strconcat ("mailto:", email, NULL);
  select = g_string_new (NULL);
  g_string_append_printf (select,
                          "SELECT ?urn WHERE { "
                          "  GRAPH <%s> { "
                          "    ?urn a nco:Contact . "
                          "    ?urn nco:hasEmailAddress ?mail . "
                          "    FILTER (fn:contains(?mail, \"%s\" )) "
                          "  }"
                          "}", TRACKER_CONTACTS_GRAPH, mail_uri);

  cursor = tracker_sparql_connection_query (connection,
                                            select->str,
                                            cancellable, error);

  g_string_free (select, TRUE);

  if (*error != NULL)
    goto out;

  res = tracker_sparql_cursor_next (cursor, cancellable, error);

  if (*error != NULL)
    goto out;

  if (res)
    {
      /* return the found resource */
      retval = g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL));
      g_debug ("Found resource in the store: %s", retval);
      goto out;
    }

  /* not found, create the resource */
  insert = g_string_new (NULL);

  g_string_append_printf (insert,
                          "INSERT INTO <%s> { <%s> a nco:EmailAddress ; nco:emailAddress \"%s\" . "
                          "_:res a nco:Contact ; nco:hasEmailAddress <%s> ; nco:fullname \"%s\" . }",
                          TRACKER_CONTACTS_GRAPH,
                          mail_uri, email,
                          mail_uri, fullname);

  insert_res =
    tracker_sparql_connection_update_blank (connection, insert->str,
                                            G_PRIORITY_DEFAULT, cancellable, error);

  g_string_free (insert, TRUE);

  if (*error != NULL)
    goto out;

  /* the result is an "aaa{ss}" variant */
  g_variant_get (insert_res, "aaa{ss}", &iter);
  g_variant_iter_next (iter, "aa{ss}", &iter);
  g_variant_iter_next (iter, "a{ss}", &iter);
  g_variant_iter_next (iter, "{ss}", &key, &val);

  g_variant_iter_free (iter);
  g_variant_unref (insert_res);

  if (g_strcmp0 (key, "res") == 0)
    {
      retval = val;
    }
  else
    {
      g_free (val);
      goto out;
    }

  g_debug ("Created a new contact resource: %s", retval);

 out:
  g_clear_object (&cursor);
  g_free (mail_uri);

  return retval;
}

gchar *
gom_tracker_utils_ensure_equipment_resource (TrackerSparqlConnection *connection,
                                             GCancellable *cancellable,
                                             GError **error,
                                             const gchar *graph,
                                             const gchar *make,
                                             const gchar *model)
{
  GError *local_error;
  TrackerSparqlCursor *cursor = NULL;
  gboolean res;
  gchar *equip_uri = NULL;
  gchar *insert = NULL;
  gchar *retval = NULL;
  gchar *select = NULL;

  g_return_val_if_fail (TRACKER_IS_SPARQL_CONNECTION (connection), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (make != NULL || model != NULL, NULL);

  equip_uri = tracker_sparql_escape_uri_printf ("urn:equipment:%s:%s:",
                                                make != NULL ? make : "",
                                                model != NULL ? model : "");
  select = g_strdup_printf ("ASK { GRAPH <%s> { <%s> a nfo:Equipment } }", graph, equip_uri);

  local_error = NULL;
  cursor = tracker_sparql_connection_query (connection, select, cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  local_error = NULL;
  res = tracker_sparql_cursor_next (cursor, cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  if (res)
    {
      if (tracker_sparql_cursor_get_boolean (cursor, 0))
        {
          /* The resource exists */
          retval = equip_uri;
          equip_uri = NULL;
          g_debug ("Found resource in the store: %s", retval);
          goto out;
        }
    }

  /* not found, create the resource */
  insert = g_strdup_printf ("INSERT INTO <%s> { <%s> a nfo:Equipment ; nfo:manufacturer \"%s\" ; nfo:model \"%s\" }",
                            graph,
                            equip_uri,
                            make,
                            model);

  local_error = NULL;
  tracker_sparql_connection_update (connection, insert, G_PRIORITY_DEFAULT, cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  retval = equip_uri;
  equip_uri = NULL;

  g_debug ("Created a new equipment resource: %s", retval);

 out:
  g_clear_object (&cursor);
  g_free (equip_uri);
  g_free (insert);
  g_free (select);

  return retval;
}

void
gom_tracker_update_datasource (TrackerSparqlConnection  *connection,
                               const gchar              *datasource_urn,
                               gboolean                  resource_exists,
                               const gchar              *graph,
                               const gchar              *resource,
                               GCancellable             *cancellable,
                               GError                  **error)
{
  gboolean set_datasource;

  /* only set the datasource again if it has changed; this avoids touching the
   * DB completely if the entry didn't change at all, since we later also check
   * the mtime. */
  set_datasource = TRUE;
  if (resource_exists)
    {
      gboolean res;
      gchar *old_value;

      res = gom_tracker_sparql_connection_get_string_attribute
        (connection, cancellable, error,
         graph, resource, "nie:dataSource", &old_value);
      g_clear_error (error);

      if (res)
        {
          res = g_str_equal (old_value, datasource_urn);
          g_free (old_value);
        }

      if (res)
        set_datasource = FALSE;
    }

  if (set_datasource)
    gom_tracker_sparql_connection_set_triple
      (connection, cancellable, error,
       graph, resource,
       "nie:dataSource", datasource_urn);
}

gboolean
gom_tracker_update_mtime (TrackerSparqlConnection  *connection,
                          gint64                    new_mtime,
                          gboolean                  resource_exists,
                          const gchar              *graph,
                          const gchar              *resource,
                          GCancellable             *cancellable,
                          GError                  **error)
{
  GTimeVal old_mtime;
  gboolean res;
  gchar *old_value;
  gchar *date;

  if (resource_exists)
    {
      res = gom_tracker_sparql_connection_get_string_attribute
        (connection, cancellable, error,
         graph, resource, "nie:contentLastModified", &old_value);
      g_clear_error (error);

      if (res)
        {
          res = g_time_val_from_iso8601 (old_value, &old_mtime);
          g_free (old_value);
        }

      if (res && (new_mtime == old_mtime.tv_sec))
        return FALSE;
    }

  date = gom_iso8601_from_timestamp (new_mtime);
  gom_tracker_sparql_connection_insert_or_replace_triple
    (connection, cancellable, error,
     graph, resource,
     "nie:contentLastModified", date);
  g_free (date);

  return TRUE;
}
