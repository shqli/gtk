/* gtkatspiaction.c: ATSPI Action implementation
 *
 * Copyright 2020  GNOME Foundation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkatspiactionprivate.h"

#include "gtkatspicontextprivate.h"
#include "gtkatcontextprivate.h"

#include "a11y/atspi/atspi-action.h"

#include "gtkactionable.h"
#include "gtkactionmuxerprivate.h"
#include "gtkwidgetprivate.h"

static gboolean
is_valid_action (GtkActionMuxer *muxer,
                 const char     *action_name)
{
  const GVariantType *param_type = NULL;
  gboolean enabled = FALSE;

  /* Skip disabled or parametrized actions */
  if (!gtk_action_muxer_query_action (muxer, action_name,
                                      &enabled,
                                      &param_type, NULL,
                                      NULL, NULL))
    return FALSE;

  if (!enabled || param_type != NULL)
    return FALSE;

  return TRUE;
}

static void
add_muxer_actions (GtkActionMuxer   *muxer,
                   char            **actions,
                   int               n_actions,
                   GVariantBuilder  *builder)
{
  for (int i = 0; i < n_actions; i++)
    {
      if (!is_valid_action (muxer, actions[i]))
        continue;

      g_variant_builder_add (builder, "(sss)",
                             actions[i],
                             actions[i],
                             "<VoidSymbol>");
    }
}

static const char *
get_action_at_index (GtkActionMuxer  *muxer,
                     char           **actions,
                     int              n_actions,
                     int              pos)
{
  int real_pos = 0;

  for (int i = 0; i < n_actions; i++)
    {
      if (!is_valid_action (muxer, actions[i]))
        continue;

      if (real_pos == pos)
        break;

      real_pos += 1;
    }

  return actions[real_pos];
}

static int
get_valid_actions (GtkActionMuxer  *muxer,
                   char           **actions,
                   int              n_actions)
{
  int n_enabled_actions = 0;

  for (int i = 0; i < n_actions; i++)
    {
      if (!is_valid_action (muxer, actions[i]))
        continue;

      n_enabled_actions += 1;
    }

  return n_enabled_actions;
}

static void
widget_handle_method (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  GtkAtSpiContext *self = user_data;
  GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
  GtkWidget *widget = GTK_WIDGET (accessible);
  GtkActionMuxer *muxer = _gtk_widget_get_action_muxer (widget, FALSE);

  if (muxer == NULL)
    return;

  char **actions = gtk_action_muxer_list_actions (muxer);
  int n_actions = actions != NULL ? g_strv_length (actions) : 0;

  /* XXX: We need more fields in the action API */
  if (g_strcmp0 (method_name, "GetName") == 0 ||
      g_strcmp0 (method_name, "GetLocalizedName") == 0 ||
      g_strcmp0 (method_name, "GetDescription") == 0)
    {
      int action_idx;

      g_variant_get (parameters, "(i)", &action_idx);

      const char *action = get_action_at_index (muxer, actions, n_actions, action_idx);

      if (action != NULL && gtk_widget_is_sensitive (widget))
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", action));
      else
        g_dbus_method_invocation_return_error (invocation,
                                               G_IO_ERROR,
                                               G_IO_ERROR_INVALID_ARGUMENT,
                                               "No action with index %d",
                                               action_idx);
    }
  else if (g_strcmp0 (method_name, "DoAction") == 0)
    {
      int action_idx;

      g_variant_get (parameters, "(i)", &action_idx);

      const char *action = get_action_at_index (muxer, actions, n_actions, action_idx);

      if (action != NULL && gtk_widget_is_sensitive (widget))
        {
          gboolean res = gtk_widget_activate_action_variant (widget, action, NULL);

          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", res));
        }
      else
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_INVALID_ARGUMENT,
                                                 "No action with index %d",
                                                 action_idx);
        }
    }
  else if (g_strcmp0 (method_name, "GetKeyBinding") == 0)
    {
      int action_idx;

      g_variant_get (parameters, "(i)", &action_idx);

      const char *action = get_action_at_index (muxer, actions, n_actions, action_idx);

      if (action != NULL && gtk_widget_is_sensitive (widget))
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", "<VoidSymbol>"));
      else
        g_dbus_method_invocation_return_error (invocation,
                                               G_IO_ERROR,
                                               G_IO_ERROR_INVALID_ARGUMENT,
                                               "No action with index %d",
                                               action_idx);
    }
  else if (g_strcmp0 (method_name, "GetActions") == 0)
    {
      GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(sss)"));

      if (n_actions >= 0 && gtk_widget_is_sensitive (widget))
        add_muxer_actions (muxer, actions, n_actions, &builder);

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(sss))", &builder));
    }

  g_strfreev (actions);
}

static GVariant *
widget_handle_get_property (GDBusConnection  *connection,
                            const gchar      *sender,
                            const gchar      *object_path,
                            const gchar      *interface_name,
                            const gchar      *property_name,
                            GError          **error,
                            gpointer          user_data)
{
  GtkAtSpiContext *self = user_data;
  GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
  GtkWidget *widget = GTK_WIDGET (accessible);
  GtkActionMuxer *muxer = _gtk_widget_get_action_muxer (widget, FALSE);
  GVariant *res = NULL;

  if (muxer == NULL)
    return res;

  char **actions = gtk_action_muxer_list_actions (muxer);
  int n_actions = actions != NULL ? g_strv_length (actions) : 0;

  if (g_strcmp0 (property_name, "NActions") == 0)
    res = g_variant_new ("i", get_valid_actions (muxer, actions, n_actions));
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Unknown property '%s'", property_name);

  return res;
}

static const GDBusInterfaceVTable widget_action_vtable = {
  widget_handle_method,
  widget_handle_get_property,
  NULL,
};

const GDBusInterfaceVTable *
gtk_atspi_get_action_vtable (GtkWidget *widget)
{
  return &widget_action_vtable;
}
