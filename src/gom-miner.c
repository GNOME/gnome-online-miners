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
 * Author: Cosimo Cecchi <cosimoc@redhat.com>
 * Author: Jasper St. Pierre <jstpierre@mecheye.net>
 *
 */

#include "config.h"

#include <stdio.h>

#include "gom-miner.h"

static void gom_miner_initable_interface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GomMiner, gom_miner, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, gom_miner_initable_interface_init))


struct _GomMinerPrivate {
  GoaClient *client;
  GError *client_error;

  gchar *bus_name;
  TrackerSparqlConnection *connection;
  TrackerEndpointDBus *endpoint;
  GError *connection_error;

  gchar *display_name;
  gchar **index_types;
};

typedef struct {
  GomMiner *self;
  GList *content_objects;
  GList *acc_objects;
  GList *old_datasources;
  GList *pending_jobs;
} CleanupJob;

typedef struct {
  GomMiner *self;
  gchar *account_id;
  gchar *shared_id;
  gchar *shared_type;
  gchar *source_urn;
  gpointer service;
} InsertSharedContentData;

typedef enum
{
  PROP_BUS_NAME = 1,
  N_PROPERTIES
} GomMinerProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

static GThreadPool *cleanup_pool;

static void cleanup_job (gpointer data, gpointer user_data);

static void
gom_account_miner_job_free (GomAccountMinerJob *job)
{
  g_hash_table_unref (job->services);
  g_clear_object (&job->miner);
  g_clear_object (&job->account);
  g_clear_object (&job->connection);
  g_clear_object (&job->task);
  g_clear_object (&job->parent_task);

  g_free (job->datasource_urn);
  g_free (job->root_element_urn);

  g_hash_table_unref (job->previous_resources);

  g_slice_free (GomAccountMinerJob, job);
}

static void
gom_insert_shared_content_data_free (InsertSharedContentData *data)
{
  GOM_MINER_GET_CLASS (data->self)->destroy_service (data->self, data->service);

  g_object_unref (data->self);
  g_free (data->account_id);
  g_free (data->shared_id);
  g_free (data->shared_type);
  g_free (data->source_urn);

  g_slice_free (InsertSharedContentData, data);
}

static InsertSharedContentData *
gom_insert_shared_content_data_new (GomMiner *self,
                                    const gchar *account_id,
                                    const gchar *shared_id,
                                    const gchar *shared_type,
                                    const gchar *source_urn,
                                    gpointer service)
{
  InsertSharedContentData *retval;

  retval = g_slice_new0 (InsertSharedContentData);
  retval->self = g_object_ref (self);
  retval->account_id = g_strdup (account_id);
  retval->shared_id = g_strdup (shared_id);
  retval->shared_type = g_strdup (shared_type);
  retval->source_urn = g_strdup (source_urn);
  retval->service = service;

  return retval;
}

static void
gom_miner_dispose (GObject *object)
{
  GomMiner *self = GOM_MINER (object);

  g_clear_object (&self->priv->client);
  g_clear_object (&self->priv->connection);

  g_free (self->priv->display_name);
  g_strfreev (self->priv->index_types);
  g_clear_error (&self->priv->client_error);
  g_clear_error (&self->priv->connection_error);

  G_OBJECT_CLASS (gom_miner_parent_class)->dispose (object);
}

static void
gom_miner_init_database (GomMiner      *self,
                         GCancellable  *cancellable,
                         GError       **error)
{
  TrackerSparqlConnectionFlags flags;
  g_autoptr (GFile) store_path = NULL;
  g_autoptr (GDBusConnection) bus = NULL;
  GError *inner_error = NULL;

  flags = TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER |
          TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT |
          TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STOP_WORDS |
          TRACKER_SPARQL_CONNECTION_FLAGS_FTS_IGNORE_NUMBERS;

  store_path = g_file_new_build_filename (g_get_user_cache_dir (),
                                          "gnome-online-miners",
                                          self->priv->bus_name,
                                          NULL);

  self->priv->connection = tracker_sparql_connection_new (flags,
                                                          store_path,
                                                          tracker_sparql_get_ontology_nepomuk (),
                                                          cancellable,
                                                          &inner_error);

  if (inner_error)
    {
      g_propagate_error (error, inner_error);
      return;
    }

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &inner_error);

  if (inner_error)
    {
      g_propagate_error (error, inner_error);
      return;
    }

  self->priv->endpoint = tracker_endpoint_dbus_new (self->priv->connection,
                                                    bus,
                                                    NULL, /* object path */
                                                    cancellable,
                                                    &inner_error);
  if (inner_error)
    {
      g_propagate_error (error, inner_error);
      return;
    }
}

static void
gom_miner_init_goa (GomMiner  *self,
                    GError   **error)
{
  GoaAccount *account;
  GoaObject *object;
  const gchar *provider_type;
  GList *accounts, *l;
  GomMinerClass *miner_class = GOM_MINER_GET_CLASS (self);
  GError *inner_error = NULL;

  self->priv->client = goa_client_new_sync (NULL, &inner_error);

  if (inner_error)
    {
      g_propagate_error (error, inner_error);
      return;
    }

  accounts = goa_client_get_accounts (self->priv->client);
  for (l = accounts; l != NULL; l = l->next)
    {
      object = l->data;

      account = goa_object_peek_account (object);
      if (account == NULL)
        continue;

      provider_type = goa_account_get_provider_type (account);
      if (g_strcmp0 (provider_type, miner_class->goa_provider_type) == 0)
        {
          g_free (self->priv->display_name);
          self->priv->display_name = goa_account_dup_provider_name (account);
          break;
        }
    }

  g_list_free_full (accounts, g_object_unref);
}

static gboolean
gom_miner_initable_init (GInitable     *initable,
                         GCancellable  *cancellable,
                         GError       **error)
{
  GError *inner_error = NULL;
  GomMiner *self;

  self = GOM_MINER (initable);

  gom_miner_init_database (self, cancellable, &inner_error);
  if (inner_error)
    {
      g_propagate_prefixed_error (error, inner_error, "Unable to set up Tracker database: ");
      return FALSE;
    }

  gom_miner_init_goa (self, &inner_error);
  if (inner_error)
    {
      g_propagate_prefixed_error (error, inner_error, "Unable to connect to GNOME Online Accounts: ");
      return FALSE;
    }

  return TRUE;
}


static void
gom_miner_init (GomMiner *self)
{
  GomMinerClass *klass = GOM_MINER_GET_CLASS (self);

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GOM_TYPE_MINER, GomMinerPrivate);
  self->priv->display_name = g_strdup ("");
}

static void
gom_miner_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  GomMiner *self = GOM_MINER (object);

  switch ((GomMinerProperty) property_id)
    {
      case PROP_BUS_NAME:
        g_free (self->priv->bus_name);
        self->priv->bus_name = g_value_dup_string (value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gom_miner_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  GomMiner *self = GOM_MINER (object);

  switch ((GomMinerProperty) property_id)
    {
       case PROP_BUS_NAME:
         g_value_set_string (value, self->priv->bus_name);
         break;

       default:
         G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
         break;
    }
}

static void
gom_miner_initable_interface_init (GInitableIface *iface)
{
  iface->init = gom_miner_initable_init;
}

static void
gom_miner_class_init (GomMinerClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  oclass->dispose = gom_miner_dispose;
  oclass->set_property = gom_miner_set_property;
  oclass->get_property = gom_miner_get_property;

  obj_properties[PROP_BUS_NAME] = g_param_spec_string ("bus-name",
                                                       "Bus Name",
                                                       "D-Bus name of the miner",
                                                       NULL  /* default value */,
                                                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (oclass,
                                     N_PROPERTIES,
                                     obj_properties);

  cleanup_pool = g_thread_pool_new (cleanup_job, NULL, 1, FALSE, NULL);

  g_type_class_add_private (klass, sizeof (GomMinerPrivate));
}

static void
gom_miner_check_pending_jobs (GTask *task)
{
  CleanupJob *cleanup_job;

  cleanup_job = (CleanupJob *) g_task_get_task_data (task);

  if (g_list_length (cleanup_job->pending_jobs) > 0)
    return;

  g_task_return_boolean (task, TRUE);
  g_slice_free (CleanupJob, cleanup_job);
}

static void
gom_miner_ensure_datasource (GomMiner *self,
                             const gchar *datasource_urn,
                             const gchar *root_element_urn,
                             GCancellable *cancellable,
                             GError **error)
{
  GString *datasource_insert;
  GomMinerClass *klass = GOM_MINER_GET_CLASS (self);

  datasource_insert = g_string_new (NULL);
  g_string_append_printf (datasource_insert,
                          "INSERT OR REPLACE INTO <%s> {"
                          "  <%s> a nie:DataSource ; nao:identifier \"%s\" . "
                          "  <%s> a nie:InformationElement ; nie:rootElementOf <%s> ; nie:version \"%d\""
                          "}",
                          GOM_GRAPH,
                          datasource_urn, klass->miner_identifier,
                          root_element_urn, datasource_urn, klass->version);

  tracker_sparql_connection_update (self->priv->connection,
                                    datasource_insert->str,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    error);

  g_string_free (datasource_insert, TRUE);
}

static void
gom_account_miner_job_query_existing (GomAccountMinerJob *job,
                                      GError **error)
{
  GCancellable *cancellable;
  GString *select;
  TrackerSparqlCursor *cursor;

  cancellable = g_task_get_cancellable (job->task);

  select = g_string_new (NULL);
  g_string_append_printf (select,
                          "SELECT ?urn ?id WHERE { GRAPH <%s> { ?urn nie:dataSource <%s> ; nao:identifier ?id . } }",
                          GOM_GRAPH, job->datasource_urn);

  cursor = tracker_sparql_connection_query (job->connection,
                                            select->str,
                                            cancellable,
                                            error);
  g_string_free (select, TRUE);

  if (cursor == NULL)
    return;

  while (tracker_sparql_cursor_next (cursor, cancellable, error))
    {
      const gchar *urn, *identifier;

      urn = tracker_sparql_cursor_get_string (cursor, 0, NULL);
      identifier = tracker_sparql_cursor_get_string (cursor, 1, NULL);

      if (identifier != NULL)
        {
          g_hash_table_insert (job->previous_resources,
                               g_strdup (identifier),
                               g_strdup (urn));
        }
      else
        {
          g_warning ("Missing identifier for urn %s", urn);
        }
    }

  g_object_unref (cursor);
}

static void
previous_resources_cleanup_foreach (gpointer key,
                                    gpointer value,
                                    gpointer user_data)
{
  const gchar *resource = value;
  GString *delete = user_data;

  g_string_append_printf (delete, "<%s> a rdfs:Resource . ", resource);
}

static void
gom_account_miner_job_cleanup_previous (GomAccountMinerJob *job,
                                        GError **error)
{
  GCancellable *cancellable;
  GString *delete;

  cancellable = g_task_get_cancellable (job->task);

  delete = g_string_new (NULL);
  g_string_append (delete, "DELETE { ");

  /* the resources left here are those who were in the database,
   * but were not found during the query; remove them from the database.
   */
  g_hash_table_foreach (job->previous_resources,
                        previous_resources_cleanup_foreach,
                        delete);

  g_string_append (delete, "}");

  tracker_sparql_connection_update (job->connection,
                                    delete->str,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    error);

  g_string_free (delete, TRUE);
}

static void
gom_account_miner_job_query (GomAccountMinerJob *job,
                             GError **error)
{
  GomMinerClass *miner_class = GOM_MINER_GET_CLASS (job->miner);
  GCancellable *cancellable;

  cancellable = g_task_get_cancellable (job->task);
  miner_class->query (job, job->connection, job->previous_resources, job->datasource_urn, cancellable, error);
}

static void
gom_account_miner_job (GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
  GomAccountMinerJob *job = task_data;
  GError *error = NULL;

  gom_miner_ensure_datasource (job->miner, job->datasource_urn, job->root_element_urn, cancellable, &error);

  if (error != NULL)
    goto out;

  g_debug ("account miner: Querying existing accounts stored in database");
  gom_account_miner_job_query_existing (job, &error);

  if (error != NULL)
    goto out;

  g_debug ("account miner: Querying remote server");
  gom_account_miner_job_query (job, &error);

  if (error != NULL)
    goto out;

  g_debug ("account miner: Removing stale accounts");
  gom_account_miner_job_cleanup_previous (job, &error);

  if (error != NULL)
    goto out;

 out:
  if (error != NULL)
    g_task_return_error (job->task, error);
  else
    g_task_return_boolean (job->task, TRUE);
}

static void
gom_account_miner_job_process_async (GomAccountMinerJob *job,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  GCancellable *cancellable;

  g_assert (job->task == NULL);

  cancellable = g_task_get_cancellable (job->parent_task);

  job->task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (job->task, gom_account_miner_job_process_async);
  g_task_set_task_data (job->task, job, NULL);
  g_task_run_in_thread (job->task, gom_account_miner_job);
}

static gboolean
gom_account_miner_job_process_finish (GAsyncResult *res,
                                      GError **error)
{
  GTask *task;

  g_assert (g_task_is_valid (res, NULL));
  task = G_TASK (res);

  g_assert (g_task_get_source_tag (task) == gom_account_miner_job_process_async);

  return g_task_propagate_boolean (task, error);
}

static GomAccountMinerJob *
gom_account_miner_job_new (GomMiner *self,
                           GoaObject *object,
                           GTask *parent_task)
{
  GomAccountMinerJob *retval;
  GoaAccount *account;
  GomMinerClass *miner_class = GOM_MINER_GET_CLASS (self);

  account = goa_object_get_account (object);
  g_assert (account != NULL);

  retval = g_slice_new0 (GomAccountMinerJob);
  retval->miner = g_object_ref (self);
  retval->parent_task = g_object_ref (parent_task);
  retval->account = account;
  retval->connection = g_object_ref (self->priv->connection);
  retval->previous_resources =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           (GDestroyNotify) g_free, (GDestroyNotify) g_free);

  retval->services = miner_class->create_services (self, object);
  retval->datasource_urn = g_strdup_printf ("gd:goa-account:%s",
                                            goa_account_get_id (retval->account));
  retval->root_element_urn = g_strdup_printf ("gd:goa-account:%s:root-element",
                                              goa_account_get_id (retval->account));

  return retval;
}

static void
miner_job_process_ready_cb (GObject *source,
                            GAsyncResult *res,
                            gpointer user_data)
{
  CleanupJob *cleanup_job;
  GomAccountMinerJob *account_miner_job = user_data;
  GomMiner *self = account_miner_job->miner;
  GError *error = NULL;

  cleanup_job = (CleanupJob *) g_task_get_task_data (account_miner_job->parent_task);

  gom_account_miner_job_process_finish (res, &error);

  if (error != NULL)
    {
      g_printerr ("Error while refreshing account %s: %s",
                  goa_account_get_id (account_miner_job->account), error->message);

      g_error_free (error);
    }

  cleanup_job->pending_jobs = g_list_remove (cleanup_job->pending_jobs,
                                             account_miner_job);

  gom_miner_check_pending_jobs (account_miner_job->parent_task);
  gom_account_miner_job_free (account_miner_job);
}

static void
gom_miner_setup_account (GomMiner *self,
                         GoaObject *object,
                         GTask *task)
{
  CleanupJob *cleanup_job;
  GomAccountMinerJob *account_miner_job;

  cleanup_job = (CleanupJob *) g_task_get_task_data (task);

  account_miner_job = gom_account_miner_job_new (self, object, task);
  cleanup_job->pending_jobs = g_list_prepend (cleanup_job->pending_jobs, account_miner_job);

  gom_account_miner_job_process_async (account_miner_job, miner_job_process_ready_cb, account_miner_job);
}

static gboolean
cleanup_old_accounts_done (gpointer data)
{
  GTask *task = G_TASK (data);
  CleanupJob *job;
  GList *l;
  GoaObject *object;
  GomMiner *self;

  job = (CleanupJob *) g_task_get_task_data (task);
  self = job->self;

  /* now setup all the current accounts */
  for (l = job->content_objects; l != NULL; l = l->next)
    {
      object = l->data;
      gom_miner_setup_account (self, object, task);

      g_object_unref (object);
    }

  if (job->content_objects != NULL)
    {
      g_list_free (job->content_objects);
      job->content_objects = NULL;
    }

  if (job->acc_objects != NULL)
    {
      g_list_free_full (job->acc_objects, g_object_unref);
      job->acc_objects = NULL;
    }

  if (job->old_datasources != NULL)
    {
      g_list_free_full (job->old_datasources, g_free);
      job->old_datasources = NULL;
    }

  gom_miner_check_pending_jobs (task);

  g_clear_object (&job->self);

  return FALSE;
}

static void
cleanup_job_do_cleanup (CleanupJob *job, GCancellable *cancellable)
{
  GomMiner *self = job->self;
  GList *l;
  GString *update;
  GError *error = NULL;

  if (job->old_datasources == NULL)
    return;

  update = g_string_new (NULL);

  for (l = job->old_datasources; l != NULL; l = l->next)
    {
      const gchar *resource;

      resource = l->data;
      g_debug ("Cleaning up old datasource %s", resource);

      g_string_append_printf (update,
                              "DELETE {"
                              "  ?u a rdfs:Resource"
                              "} WHERE {"
                              "  ?u nie:dataSource <%s>"
                              "}",
                              resource);
    }

  tracker_sparql_connection_update (self->priv->connection,
                                    update->str,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    &error);
  g_string_free (update, TRUE);

  if (error != NULL)
    {
      g_printerr ("Error while cleaning up old accounts: %s\n", error->message);
      g_error_free (error);
    }
}

static gint
cleanup_datasource_compare (gconstpointer a,
                            gconstpointer b)
{
  GoaObject *object = GOA_OBJECT (a);
  const gchar *datasource = b;
  gint res;

  GoaAccount *account;
  gchar *object_datasource;

  account = goa_object_peek_account (object);
  g_assert (account != NULL);

  object_datasource = g_strdup_printf ("gd:goa-account:%s", goa_account_get_id (account));
  res = g_strcmp0 (datasource, object_datasource);

  g_free (object_datasource);

  return res;
}

static void
cleanup_job (gpointer data,
             gpointer user_data)
{
  GCancellable *cancellable;
  GSource *source;
  GString *select;
  GTask *task = G_TASK (data);
  GError *error = NULL;
  TrackerSparqlCursor *cursor;
  const gchar *datasource, *old_version_str;
  gint old_version;
  GList *element;
  CleanupJob *job;
  GomMiner *self;
  GomMinerClass *klass;

  cancellable = g_task_get_cancellable (task);
  job = (CleanupJob *) g_task_get_task_data (task);
  self = job->self;
  klass = GOM_MINER_GET_CLASS (self);

  /* find all our datasources in the tracker DB */
  select = g_string_new (NULL);
  g_string_append_printf (select,
                          "SELECT ?datasource nie:version(?root) WHERE { "
                          "  GRAPH <%s> { "
                          "    ?datasource a nie:DataSource . "
                          "    ?datasource nao:identifier \"%s\" . "
                          "    OPTIONAL { ?root nie:rootElementOf ?datasource } "
                          "  }"
                          "}",
                          GOM_GRAPH,
                          klass->miner_identifier);

  cursor = tracker_sparql_connection_query (self->priv->connection,
                                            select->str,
                                            cancellable,
                                            &error);
  g_string_free (select, TRUE);

  if (error != NULL)
    {
      g_printerr ("Error while cleaning up old accounts: %s\n", error->message);
      goto out;
    }

  while (tracker_sparql_cursor_next (cursor, cancellable, NULL))
    {
      /* If the source we found is not in the current list, add
       * it to the cleanup list.
       * Note that the objects here in the list might *not* support
       * documents, in case the switch has been disabled in System Settings.
       * In fact, we only remove all the account data in case the account
       * is really removed from the panel.
       *
       * Also, cleanup sources for which the version has increased.
       */
      datasource = tracker_sparql_cursor_get_string (cursor, 0, NULL);
      element = g_list_find_custom (job->acc_objects, datasource,
                                    cleanup_datasource_compare);

      if (element == NULL)
        job->old_datasources = g_list_prepend (job->old_datasources,
                                               g_strdup (datasource));

      old_version_str = tracker_sparql_cursor_get_string (cursor, 1, NULL);
      if (old_version_str == NULL)
        old_version = 1;
      else
        sscanf (old_version_str, "%d", &old_version);

      g_debug ("Stored version: %d - new version %d", old_version, klass->version);

      if ((element == NULL) || (old_version < klass->version))
        {
          job->old_datasources = g_list_prepend (job->old_datasources,
                                                 g_strdup (datasource));
        }
    }

  g_object_unref (cursor);

  /* cleanup the DB */
  cleanup_job_do_cleanup (job, cancellable);

 out:
  source = g_idle_source_new ();
  g_source_set_name (source, "[gnome-online-miners] cleanup_old_accounts_done");
  g_task_attach_source (task, source, cleanup_old_accounts_done);
  g_source_unref (source);

  g_object_unref (task);
}

static void
gom_miner_cleanup_old_accounts (GomMiner *self,
                                GList *content_objects,
                                GList *acc_objects,
                                GTask *task)
{
  CleanupJob *job = g_slice_new0 (CleanupJob);

  job->self = g_object_ref (self);
  job->content_objects = content_objects;
  job->acc_objects = acc_objects;

  g_task_set_task_data (task, job, NULL);
  g_thread_pool_push (cleanup_pool, g_object_ref (task), NULL);
}

static void
gom_miner_refresh_db_real (GomMiner *self, GTask *task)
{
  GoaFiles *files;
  GoaPhotos *photos;
  GoaAccount *account;
  GoaObject *object;
  const gchar *provider_type;
  GList *accounts, *content_objects, *acc_objects, *l;
  GomMinerClass *miner_class = GOM_MINER_GET_CLASS (self);
  gboolean skip_photos, skip_documents;

  content_objects = NULL;
  acc_objects = NULL;

  accounts = goa_client_get_accounts (self->priv->client);
  for (l = accounts; l != NULL; l = l->next)
    {
      object = l->data;

      account = goa_object_peek_account (object);
      if (account == NULL)
        continue;

      provider_type = goa_account_get_provider_type (account);
      if (g_strcmp0 (provider_type, miner_class->goa_provider_type) != 0)
        continue;

      acc_objects = g_list_append (acc_objects, g_object_ref (object));
      skip_photos = skip_documents = TRUE;

      files = goa_object_peek_files (object);
      photos = goa_object_peek_photos (object);

      if (gom_miner_supports_type (self, "photos") && photos != NULL)
        skip_photos = FALSE;

      if (gom_miner_supports_type (self, "documents") && files != NULL)
        skip_documents = FALSE;

      if (skip_photos && skip_documents)
        continue;

      content_objects = g_list_append (content_objects, g_object_ref (object));
    }

  g_list_free_full (accounts, g_object_unref);

  gom_miner_cleanup_old_accounts (self, content_objects, acc_objects, task);
}

const gchar *
gom_miner_get_display_name (GomMiner *self)
{
  return self->priv->display_name;
}

static void
gom_miner_insert_shared_content_in_thread_func (GTask *task,
                                                gpointer source_object,
                                                gpointer task_data,
                                                GCancellable *cancellable)
{
  GomMiner *self = GOM_MINER (source_object);
  GError *error;
  InsertSharedContentData *data = (InsertSharedContentData *) task_data;
  gchar *datasource_urn = NULL;
  gchar *root_element_urn = NULL;

  datasource_urn = g_strdup_printf ("gd:goa-account:%s", data->account_id);
  root_element_urn = g_strdup_printf ("gd:goa-account:%s:root-element", data->account_id);

  error = NULL;
  gom_miner_ensure_datasource (self, datasource_urn, root_element_urn, cancellable, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  error = NULL;
  GOM_MINER_GET_CLASS (self)->insert_shared_content (self,
                                                     data->service,
                                                     self->priv->connection,
                                                     datasource_urn,
                                                     data->shared_id,
                                                     data->shared_type,
                                                     data->source_urn,
                                                     cancellable,
                                                     &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  g_task_return_boolean (task, TRUE);

 out:
  g_free (datasource_urn);
  g_free (root_element_urn);
}

void
gom_miner_insert_shared_content_async (GomMiner *self,
                                       const gchar *account_id,
                                       const gchar *shared_id,
                                       const gchar *shared_type,
                                       const gchar *source_urn,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  GTask *task = NULL;
  GoaFiles *files;
  GoaObject *object = NULL;
  GoaPhotos *photos;
  InsertSharedContentData *data;
  gpointer service;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gom_miner_insert_shared_content_async);

  if (self->priv->client_error != NULL)
    {
      g_task_return_error (task, g_error_copy (self->priv->client_error));
      goto out;
    }

  if (self->priv->connection_error != NULL)
    {
      g_task_return_error (task, g_error_copy (self->priv->connection_error));
      goto out;
    }

  object = goa_client_lookup_by_id (self->priv->client, account_id);
  if (object == NULL)
    {
      /* throw error */
      goto out;
    }

  files = goa_object_peek_files (object);
  photos = goa_object_peek_photos (object);

  if (g_strcmp0 (shared_type, "documents") == 0 && files == NULL)
    {
      /* throw error */
      goto out;
    }

  if (g_strcmp0 (shared_type, "photos") == 0 && photos == NULL)
    {
      /* throw error */
      goto out;
    }

  service = GOM_MINER_GET_CLASS (self)->create_service (self, object, shared_type);
  if (service == NULL)
    {
      /* throw error */
      goto out;
    }

  data = gom_insert_shared_content_data_new (self, account_id, shared_id, shared_type, source_urn, service);
  g_task_set_task_data (task, data, (GDestroyNotify) gom_insert_shared_content_data_free);

  g_task_run_in_thread (task, gom_miner_insert_shared_content_in_thread_func);

 out:
  g_clear_object (&object);
  g_clear_object (&task);
}

gboolean
gom_miner_insert_shared_content_finish (GomMiner *self, GAsyncResult *res, GError **error)
{
  GTask *task;

  g_assert (g_task_is_valid (res, self));
  task = G_TASK (res);

  g_assert (g_task_get_source_tag (task) == gom_miner_insert_shared_content_async);

  return g_task_propagate_boolean (task, error);
}

void
gom_miner_refresh_db_async (GomMiner *self,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  GTask *task = NULL;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, gom_miner_refresh_db_async);

  if (self->priv->client_error != NULL)
    {
      g_task_return_error (task, g_error_copy (self->priv->client_error));
      goto out;
    }

  if (self->priv->connection_error != NULL)
    {
      g_task_return_error (task, g_error_copy (self->priv->connection_error));
      goto out;
    }

  gom_miner_refresh_db_real (self, task);

 out:
  g_clear_object (&task);
}

gboolean
gom_miner_refresh_db_finish (GomMiner *self,
                             GAsyncResult *res,
                             GError **error)
{
  GTask *task;

  g_assert (g_task_is_valid (res, self));
  task = G_TASK (res);

  g_assert (g_task_get_source_tag (task) == gom_miner_refresh_db_async);

  return g_task_propagate_boolean (task, error);
}

void
gom_miner_set_index_types (GomMiner *self, const char **index_types)
{
  g_strfreev (self->priv->index_types);
  self->priv->index_types = g_strdupv ((gchar **) index_types);
}

const gchar **
gom_miner_get_index_types (GomMiner *self)
{
  return (const gchar **) self->priv->index_types;
}

gboolean
gom_miner_supports_type (GomMiner *self, gchar *type)
{
  gboolean retval = FALSE;
  guint i;

  for (i = 0; self->priv->index_types[i] != NULL; i++)
    {
      if (g_strcmp0 (self->priv->index_types[i], type) == 0)
        {
          retval = TRUE;
          break;
        }
    }

  return retval;
}
