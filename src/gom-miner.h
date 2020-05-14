/*
 * GNOME Online Miners - crawls through your online content
 * Copyright (C) 2012 Red Hat, Inc.
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
 * Author: Jasper St. Pierre <jstpierre@mecheye.net>
 *
 */

#ifndef __GOM_MINER_H__
#define __GOM_MINER_H__

#include <glib-object.h>
#include <goa/goa.h>

#include "gom-tracker.h"
#include "gom-utils.h"

G_BEGIN_DECLS

#define GOM_TYPE_MINER (gom_miner_get_type ())

#define GOM_MINER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   GOM_TYPE_MINER, GomMiner))

#define GOM_MINER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
   GOM_TYPE_MINER, GomMinerClass))

#define GOM_IS_MINER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
   GOM_TYPE_MINER))

#define GOM_IS_MINER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
   GOM_TYPE_MINER))

#define GOM_MINER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
   GOM_TYPE_MINER, GomMinerClass))

typedef struct _GomMiner        GomMiner;
typedef struct _GomMinerClass   GomMinerClass;
typedef struct _GomMinerPrivate GomMinerPrivate;

typedef struct {
  GomMiner *miner;
  TrackerSparqlConnection *connection;
  gchar *bus_name;

  GoaAccount *account;
  GHashTable *services;
  GTask *task;
  GTask *parent_task;

  GHashTable *previous_resources;
  gchar *datasource_urn;
  gchar *root_element_urn;
} GomAccountMinerJob;

struct _GomMiner
{
  GObject parent;

  GomMinerPrivate *priv;
};

struct _GomMinerClass
{
  GObjectClass parent_class;

  char *goa_provider_type;
  char *miner_identifier;
  gint  version;

  gpointer (*create_service) (GomMiner *self, GoaObject *object, const gchar *type);

  GHashTable * (*create_services) (GomMiner *self,
                                   GoaObject *object);

  void (*destroy_service) (GomMiner *self, gpointer service);

  void (*insert_shared_content) (GomMiner *self,
                                 gpointer service,
                                 TrackerSparqlConnection *connection,
                                 const gchar *datasource_urn,
                                 const gchar *shared_id,
                                 const gchar *shared_type,
                                 const gchar *source_urn,
                                 GCancellable *cancellable,
                                 GError **error);

  void (*query) (GomAccountMinerJob *job,
                 TrackerSparqlConnection *connection,
                 GHashTable *previous_resources,
                 const gchar *datasource_urn,
                 GCancellable *cancellable,
                 GError **error);
};

GType gom_miner_get_type (void);

const gchar * gom_miner_get_display_name (GomMiner *self);

void gom_miner_insert_shared_content_async (GomMiner *self,
                                            const gchar *account_id,
                                            const gchar *shared_id,
                                            const gchar *shared_type,
                                            const gchar *source_urn,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean gom_miner_insert_shared_content_finish (GomMiner *self, GAsyncResult *res, GError **error);

void gom_miner_refresh_db_async (GomMiner *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

gboolean gom_miner_refresh_db_finish (GomMiner *self,
                                      GAsyncResult *res,
                                      GError **error);

void gom_miner_set_index_types (GomMiner *self, const gchar **index_types);

const gchar ** gom_miner_get_index_types (GomMiner *self);

gboolean gom_miner_supports_type (GomMiner *self, gchar *type);

G_END_DECLS

#endif /* __GOM_MINER_H__ */
