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

#ifndef __GOM_TRACKER_H__
#define __GOM_TRACKER_H__

#include <gio/gio.h>
#include <libtracker-sparql/tracker-sparql.h>

G_BEGIN_DECLS

/* The graph where we store account meta information */
#define GOM_GRAPH "tracker.api.gnome.org/ontology/v3/gnome-online-miners"

/* Graphs where we store content information */
#define TRACKER_CONTACTS_GRAPH "tracker:Contacts"
#define TRACKER_DOCUMENTS_GRAPH "tracker:Documents"
#define TRACKER_PICTURES_GRAPH "tracker:Pictures"

gchar *gom_tracker_sparql_connection_ensure_resource (TrackerSparqlConnection *connection,
                                                      GCancellable *cancellable,
                                                      GError **error,
                                                      gboolean *resource_exists,
                                                      const gchar *graph,
                                                      const gchar *identifier,
                                                      const gchar *class,
                                                      ...);

gboolean gom_tracker_sparql_connection_insert_or_replace_triple (TrackerSparqlConnection *connection,
                                                                 GCancellable *cancellable,
                                                                 GError **error,
                                                                 const gchar *graph,
                                                                 const gchar *resource,
                                                                 const gchar *property_name,
                                                                 const gchar *property_value);

gboolean gom_tracker_sparql_connection_set_triple (TrackerSparqlConnection *connection,
                                                   GCancellable *cancellable,
                                                   GError **error,
                                                   const gchar *graph,
                                                   const gchar *resource,
                                                   const gchar *property_name,
                                                   const gchar *property_value);

gboolean gom_tracker_sparql_connection_toggle_favorite (TrackerSparqlConnection *connection,
                                                        GCancellable *cancellable,
                                                        GError **error,
                                                        const gchar *graph,
                                                        const gchar *resource,
                                                        gboolean favorite);

gchar* gom_tracker_utils_ensure_contact_resource (TrackerSparqlConnection *connection,
                                                  GCancellable *cancellable,
                                                  GError **error,
                                                  const gchar *email,
                                                  const gchar *fullname);

gchar *gom_tracker_utils_ensure_equipment_resource (TrackerSparqlConnection *connection,
                                                    GCancellable *cancellable,
                                                    GError **error,
                                                    const gchar *graph,
                                                    const gchar *make,
                                                    const gchar *model);

void gom_tracker_update_datasource (TrackerSparqlConnection  *connection,
                                    const gchar              *datasource_urn,
                                    gboolean                  resource_exists,
                                    const gchar              *graph,
                                    const gchar              *resource,
                                    GCancellable             *cancellable,
                                    GError                  **error);
gboolean gom_tracker_update_mtime (TrackerSparqlConnection  *connection,
                                   gint64                    new_mtime,
                                   gboolean                  resource_exists,
                                   const gchar              *graph,
                                   const gchar              *resource,
                                   GCancellable             *cancellable,
                                   GError                  **error);

G_END_DECLS

#endif /* __GOM_TRACKER_H__ */
