/*
 * empathy-sanity-cleaning.c
 * Code automatically called when starting a specific version of Empathy for
 * the first time doing misc cleaning.
 *
 * Copyright (C) 2012 Collabora Ltd.
 * @author Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"

#include "empathy-sanity-cleaning.h"

#include <telepathy-glib/telepathy-glib.h>

#include <libempathy/empathy-account-settings.h>
#include <libempathy/empathy-gsettings.h>

#include <libempathy-gtk/empathy-theme-manager.h>

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include <libempathy/empathy-debug.h>

/*
 * This number has to be increased each time a new task is added or modified.
 *
 * If the number stored in gsettings is lower than it, all the tasks will
 * be executed.
 */
#define SANITY_CLEANING_NUMBER 4

static void
account_update_parameters_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccount *account = TP_ACCOUNT (source);

  if (!tp_account_update_parameters_finish (account, result, NULL, &error))
    {
      DEBUG ("Failed to update parameters of account '%s': %s",
          tp_account_get_path_suffix (account), error->message);

      g_error_free (error);
      return;
    }

  tp_account_reconnect_async (account, NULL, NULL);
}

/* Make sure XMPP accounts don't have a negative priority (bgo #671452) */
static void
fix_xmpp_account_priority (TpAccountManager *am)
{
  GList *accounts, *l;

  accounts = tp_account_manager_get_valid_accounts (am);
  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = l->data;
      GHashTable *params;
      gint priority;

      if (tp_strdiff (tp_account_get_protocol_name (account), "jabber"))
        continue;

      params = (GHashTable *) tp_account_get_parameters (account);
      if (params == NULL)
        continue;

      priority = tp_asv_get_int32 (params, "priority", NULL);
      if (priority >= 0)
        continue;

      DEBUG ("Resetting XMPP priority of account '%s' to 0",
          tp_account_get_path_suffix (account));

      params = tp_asv_new (
          "priority", G_TYPE_INT, 0,
          NULL);

      tp_account_update_parameters_async (account, params, NULL,
          account_update_parameters_cb, NULL);

      g_hash_table_unref (params);
    }

  g_list_free (accounts);
}

static void
set_facebook_account_fallback_server (TpAccountManager *am)
{
  GList *accounts, *l;

  accounts = tp_account_manager_get_valid_accounts (am);
  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = l->data;
      GHashTable *params;
      gchar *fallback_servers[] = {
          "chat.facebook.com:443",
          NULL };

      if (tp_strdiff (tp_account_get_service (account), "facebook"))
        continue;

      params = (GHashTable *) tp_account_get_parameters (account);
      if (params == NULL)
        continue;

      if (tp_asv_get_strv (params, "fallback-servers") != NULL)
        continue;

      DEBUG ("Setting chat.facebook.com:443 as a fallback on account '%s'",
          tp_account_get_path_suffix (account));

      params = tp_asv_new (
          "fallback-servers", G_TYPE_STRV, fallback_servers,
          NULL);

      tp_account_update_parameters_async (account, params, NULL,
          account_update_parameters_cb, NULL);

      g_hash_table_unref (params);
    }

  g_list_free (accounts);
}

static void
upgrade_chat_theme_settings (void)
{
  GSettings *gsettings_chat;
  gchar *theme, *new_theme = NULL;
  const char *variant = "";

  gsettings_chat = g_settings_new (EMPATHY_PREFS_CHAT_SCHEMA);

  theme = g_settings_get_string (gsettings_chat,
      EMPATHY_PREFS_CHAT_THEME);

  if (!tp_strdiff (theme, "adium")) {
    gchar *path;

    path = g_settings_get_string (gsettings_chat,
        EMPATHY_PREFS_CHAT_ADIUM_PATH);

    new_theme = empathy_theme_manager_dup_theme_name_from_path (path);
    if (new_theme == NULL)
      {
        /* Use the Classic theme as fallback */
        new_theme = g_strdup ("Classic");
      }

    g_free (path);
  } else if (!tp_strdiff (theme, "gnome")) {
    new_theme = g_strdup ("PlanetGNOME");
  } else if (!tp_strdiff (theme, "simple")) {
    new_theme = g_strdup ("Boxes");
    variant = "Simple";
  } else if (!tp_strdiff (theme, "clean")) {
    new_theme = g_strdup ("Boxes");
    variant = "Clean";
  } else if (!tp_strdiff (theme, "blue")) {
    new_theme = g_strdup ("Boxes");
    variant = "Blue";
  } else {
    /* Assume that's an Adium theme name. The theme manager will fallback to
     * 'Classic' if it can't find it. */
    goto finally;
  }

  DEBUG ("Migrating to '%s' variant '%s'", new_theme, variant);

  g_settings_set_string (gsettings_chat,
    EMPATHY_PREFS_CHAT_THEME, new_theme);
  g_settings_set_string (gsettings_chat,
    EMPATHY_PREFS_CHAT_THEME_VARIANT, variant);

finally:
  g_free (theme);
  g_free (new_theme);
  g_object_unref (gsettings_chat);
}

#ifdef HAVE_UOA
static void
uoa_account_created_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountRequest *ar = (TpAccountRequest *) source;
  TpAccount *old_account = user_data;
  TpAccount *new_account;
  GError *error = NULL;

  new_account = tp_account_request_create_account_finish (ar, result, &error);
  if (new_account == NULL)
    {
      DEBUG ("Failed to migrate account '%s' to UOA: %s",
          tp_account_get_path_suffix (old_account), error->message);
      g_clear_error (&error);
    }
  else
    {
      DEBUG ("New account %s created to superseed %s",
          tp_account_get_path_suffix (new_account),
          tp_account_get_path_suffix (old_account));
      tp_account_remove_async (old_account, NULL, NULL);
    }

  g_object_unref (old_account);
  g_object_unref (new_account);
}

static void
migrate_accounts_to_uoa (TpAccountManager *am)
{
  GList *accounts, *l;

  DEBUG ("Start migrating accounts to UOA");

  accounts = tp_account_manager_get_valid_accounts (am);
  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = l->data;
      TpAccountRequest *ar;
      GVariant *params;
      GVariant *param;
      GVariantIter iter;
      const gchar * const *supersedes;

      /* If account is already in a specific storage (like UOA or GOA),
       * don't migrate it.
       * Note that we cannot migrate GOA accounts anyway, since we can't delete
       * them it would create duplicated accounts. */
      if (!tp_str_empty (tp_account_get_storage_provider (account)))
        continue;

      DEBUG ("Migrating account %s to UOA storage\n",
          tp_account_get_path_suffix (account));

      ar = tp_account_request_new (am,
          tp_account_get_cm_name (account),
          tp_account_get_protocol_name (account),
          tp_account_get_display_name (account));
      tp_account_request_set_storage_provider (ar, EMPATHY_UOA_PROVIDER);
      tp_account_request_set_enabled (ar,
          tp_account_is_enabled (account));
      tp_account_request_set_icon_name (ar,
          tp_account_get_icon_name (account));
      tp_account_request_set_nickname (ar,
          tp_account_get_nickname (account));
      tp_account_request_set_service (ar,
          tp_account_get_service (account));

      supersedes = tp_account_get_supersedes (account);
      while (*supersedes != NULL)
        tp_account_request_add_supersedes (ar, *supersedes);
      tp_account_request_add_supersedes (ar,
          tp_proxy_get_object_path (account));

      params = tp_account_dup_parameters_vardict (account);
      g_variant_iter_init (&iter, params);
      while ((param = g_variant_iter_next_value (&iter)))
        {
          GVariant *k, *v;
          const gchar *key;

          k = g_variant_get_child_value (param, 0);
          key = g_variant_get_string (k, NULL);
          v = g_variant_get_child_value (param, 1);

          tp_account_request_set_parameter (ar, key,
              g_variant_get_variant (v));

          g_variant_unref (k);
          g_variant_unref (v);
        }

      tp_account_set_enabled_async (account, FALSE, NULL, NULL);
      tp_account_request_create_account_async (ar, uoa_account_created_cb,
          g_object_ref (account));

      g_variant_unref (params);
      g_object_unref (ar);
    }
}
#endif

static void
run_sanity_cleaning_tasks (TpAccountManager *am)
{
  DEBUG ("Starting sanity cleaning tasks");

  fix_xmpp_account_priority (am);
  set_facebook_account_fallback_server (am);
  upgrade_chat_theme_settings ();
#ifdef HAVE_UOA
  migrate_accounts_to_uoa (am);
#endif
}

static void
am_prepare_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccountManager *am = TP_ACCOUNT_MANAGER (source);

  if (!tp_proxy_prepare_finish (am, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  run_sanity_cleaning_tasks (am);
}

void empathy_sanity_checking_run_if_needed (void)
{
  GSettings *settings;
  guint number;
  TpAccountManager *am;

  settings = g_settings_new (EMPATHY_PREFS_SCHEMA);
  number = g_settings_get_uint (settings, EMPATHY_PREFS_SANITY_CLEANING_NUMBER);

  if (number == SANITY_CLEANING_NUMBER)
    goto out;

  am = tp_account_manager_dup ();

  tp_proxy_prepare_async (am, NULL, am_prepare_cb, NULL);

  g_settings_set_uint (settings, EMPATHY_PREFS_SANITY_CLEANING_NUMBER,
      SANITY_CLEANING_NUMBER);

  g_object_unref (am);
out:
  g_object_unref (settings);
}
