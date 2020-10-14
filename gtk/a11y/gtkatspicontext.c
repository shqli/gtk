/* gtkatspicontext.c: AT-SPI GtkATContext implementation
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

#include "gtkatspicontextprivate.h"

#include "gtkaccessibleprivate.h"

#include "gtkatspicacheprivate.h"
#include "gtkatspirootprivate.h"
#include "gtkatspiprivate.h"
#include "gtkatspiutilsprivate.h"
#include "gtkatspitextprivate.h"
#include "gtkatspieditabletextprivate.h"
#include "gtkatspivalueprivate.h"
#include "gtkatspiselectionprivate.h"
#include "gtkatspicomponentprivate.h"

#include "a11y/atspi/atspi-accessible.h"
#include "a11y/atspi/atspi-text.h"
#include "a11y/atspi/atspi-editabletext.h"
#include "a11y/atspi/atspi-value.h"
#include "a11y/atspi/atspi-selection.h"
#include "a11y/atspi/atspi-component.h"

#include "gtkdebug.h"
#include "gtkeditable.h"
#include "gtkentryprivate.h"
#include "gtkroot.h"
#include "gtktextview.h"
#include "gtkwindow.h"

#include <gio/gio.h>

#include <locale.h>

#if defined(GDK_WINDOWING_WAYLAND)
# include <gdk/wayland/gdkwaylanddisplay.h>
#endif
#if defined(GDK_WINDOWING_X11)
# include <gdk/x11/gdkx11display.h>
# include <gdk/x11/gdkx11property.h>
#endif

struct _GtkAtSpiContext
{
  GtkATContext parent_instance;

  /* The root object, used as a entry point */
  GtkAtSpiRoot *root;

  /* The cache object, used to retrieve ATContexts */
  GtkAtSpiCache *cache;

  /* The address for the ATSPI accessibility bus */
  char *bus_address;

  /* The object path of the ATContext on the bus */
  char *context_path;

  /* Just a pointer; the connection is owned by the GtkAtSpiRoot
   * associated to the GtkATContext
   */
  GDBusConnection *connection;

  /* Accerciser refuses to work unless we implement a GetInterface
   * call that returns a list of all implemented interfaces. We
   * collect the answer here.
   */
  GVariant *interfaces;

  guint registration_ids[20];
  guint n_registered_objects;
};

enum
{
  PROP_BUS_ADDRESS = 1,

  N_PROPS
};

static GParamSpec *obj_props[N_PROPS];

G_DEFINE_TYPE (GtkAtSpiContext, gtk_at_spi_context, GTK_TYPE_AT_CONTEXT)

static void
set_atspi_state (guint64        *states,
                 AtspiStateType  state)
{
  *states |= (G_GUINT64_CONSTANT (1) << state);
}

static void
unset_atspi_state (guint64        *states,
                   AtspiStateType  state)
{
  *states &= ~(G_GUINT64_CONSTANT (1) << state);
}

static void
collect_states (GtkAtSpiContext    *self,
                GVariantBuilder *builder)
{
  GtkATContext *ctx = GTK_AT_CONTEXT (self);
  GtkAccessibleValue *value;
  GtkAccessible *accessible;
  guint64 states = 0;

  accessible = gtk_at_context_get_accessible (ctx);

  set_atspi_state (&states, ATSPI_STATE_VISIBLE);

  if (ctx->accessible_role == GTK_ACCESSIBLE_ROLE_TEXT_BOX ||
      ctx->accessible_role == GTK_ACCESSIBLE_ROLE_SEARCH_BOX ||
      ctx->accessible_role == GTK_ACCESSIBLE_ROLE_SPIN_BUTTON)
    set_atspi_state (&states, ATSPI_STATE_EDITABLE);

  if (gtk_at_context_has_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_READ_ONLY))
    {
      value = gtk_at_context_get_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_READ_ONLY);
      if (gtk_boolean_accessible_value_get (value))
        {
          set_atspi_state (&states, ATSPI_STATE_READ_ONLY);
          unset_atspi_state (&states, ATSPI_STATE_EDITABLE);
        }
    }

  if (gtk_accessible_get_platform_state (accessible, GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSABLE))
    set_atspi_state (&states, ATSPI_STATE_FOCUSABLE);

  if (gtk_accessible_get_platform_state (accessible, GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSED))
    set_atspi_state (&states, ATSPI_STATE_FOCUSED);

  if (gtk_at_context_has_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_ORIENTATION))
    {
      value = gtk_at_context_get_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_ORIENTATION);
      if (gtk_orientation_accessible_value_get (value) == GTK_ORIENTATION_HORIZONTAL)
        set_atspi_state (&states, ATSPI_STATE_HORIZONTAL);
      else
        set_atspi_state (&states, ATSPI_STATE_VERTICAL);
    }

  if (gtk_at_context_has_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_MODAL))
    {
      value = gtk_at_context_get_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_MODAL);
      if (gtk_boolean_accessible_value_get (value))
        set_atspi_state (&states, ATSPI_STATE_MODAL);
    }

  if (gtk_at_context_has_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_MULTI_LINE))
    {
      value = gtk_at_context_get_accessible_property (ctx, GTK_ACCESSIBLE_PROPERTY_MULTI_LINE);
      if (gtk_boolean_accessible_value_get (value))
        set_atspi_state (&states, ATSPI_STATE_MULTI_LINE);
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_BUSY))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_BUSY);
      if (gtk_boolean_accessible_value_get (value))
        set_atspi_state (&states, ATSPI_STATE_BUSY);
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_CHECKED))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_CHECKED);
      switch (gtk_tristate_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_TRISTATE_TRUE:
          set_atspi_state (&states, ATSPI_STATE_CHECKED);
          break;
        case GTK_ACCESSIBLE_TRISTATE_MIXED:
          set_atspi_state (&states, ATSPI_STATE_INDETERMINATE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_FALSE:
        default:
          break;
        }
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_DISABLED))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_DISABLED);
      if (!gtk_boolean_accessible_value_get (value))
        set_atspi_state (&states, ATSPI_STATE_SENSITIVE);
    }
  else
    set_atspi_state (&states, ATSPI_STATE_SENSITIVE);

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_EXPANDED))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_EXPANDED);
      if (value->value_class->type == GTK_ACCESSIBLE_VALUE_TYPE_BOOLEAN)
        {
          set_atspi_state (&states, ATSPI_STATE_EXPANDABLE);
          if (gtk_boolean_accessible_value_get (value))
            set_atspi_state (&states, ATSPI_STATE_EXPANDED);
        }
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_INVALID))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_INVALID);
      switch (gtk_invalid_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_INVALID_TRUE:
        case GTK_ACCESSIBLE_INVALID_GRAMMAR:
        case GTK_ACCESSIBLE_INVALID_SPELLING:
          set_atspi_state (&states, ATSPI_STATE_INVALID);
          break;
        case GTK_ACCESSIBLE_INVALID_FALSE:
        default:
          break;
        }
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_PRESSED))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_PRESSED);
      switch (gtk_tristate_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_TRISTATE_TRUE:
          set_atspi_state (&states, ATSPI_STATE_PRESSED);
          break;
        case GTK_ACCESSIBLE_TRISTATE_MIXED:
          set_atspi_state (&states, ATSPI_STATE_INDETERMINATE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_FALSE:
        default:
          break;
        }
    }

  if (gtk_at_context_has_accessible_state (ctx, GTK_ACCESSIBLE_STATE_SELECTED))
    {
      value = gtk_at_context_get_accessible_state (ctx, GTK_ACCESSIBLE_STATE_SELECTED);
      if (value->value_class->type == GTK_ACCESSIBLE_VALUE_TYPE_BOOLEAN)
        {
          set_atspi_state (&states, ATSPI_STATE_SELECTABLE);
          if (gtk_boolean_accessible_value_get (value))
            set_atspi_state (&states, ATSPI_STATE_SELECTED);
        }
    }

  g_variant_builder_add (builder, "u", (guint32) (states & 0xffffffff));
  g_variant_builder_add (builder, "u", (guint32) (states >> 32));
}

static void
collect_relations (GtkAtSpiContext *self,
                   GVariantBuilder *builder)
{
  GtkATContext *ctx = GTK_AT_CONTEXT (self);
  struct {
    GtkAccessibleRelation r;
    AtspiRelationType s;
  } map[] = {
    { GTK_ACCESSIBLE_RELATION_LABELLED_BY, ATSPI_RELATION_LABELLED_BY },
    { GTK_ACCESSIBLE_RELATION_CONTROLS, ATSPI_RELATION_CONTROLLER_FOR },
    { GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, ATSPI_RELATION_DESCRIBED_BY },
    { GTK_ACCESSIBLE_RELATION_FLOW_TO, ATSPI_RELATION_FLOWS_TO},
  };
  GtkAccessibleValue *value;
  GList *list, *l;
  GtkATContext *target_ctx;
  const char *unique_name;
  int i;

  unique_name = g_dbus_connection_get_unique_name (self->connection);

  for (i = 0; i < G_N_ELEMENTS (map); i++)
    {
      if (!gtk_at_context_has_accessible_relation (ctx, map[i].r))
        continue;

      GVariantBuilder b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(so)"));

      value = gtk_at_context_get_accessible_relation (ctx, map[i].r);
      list = gtk_reference_list_accessible_value_get (value);

      for (l = list; l; l = l->next)
        {
          target_ctx = gtk_accessible_get_at_context (GTK_ACCESSIBLE (l->data));
          g_variant_builder_add (&b, "(so)",
                                 unique_name,
                                 GTK_AT_SPI_CONTEXT (target_ctx)->context_path);
        }

      g_variant_builder_add (builder, "(ua(so))", map[i].s, &b);
    }
}

static int
get_index_in_parent (GtkWidget *widget)
{
  GtkWidget *parent = gtk_widget_get_parent (widget);
  GtkWidget *child;
  int idx;

  idx = 0;
  for (child = gtk_widget_get_first_child (parent);
       child;
       child = gtk_widget_get_next_sibling (child))
    {
      if (!gtk_accessible_should_present (GTK_ACCESSIBLE (child)))
        continue;

      if (child == widget)
        return idx;

      idx++;
    }

  return -1;
}

static int
get_index_in_toplevels (GtkWidget *widget)
{
  GListModel *toplevels = gtk_window_get_toplevels ();
  guint n_toplevels = g_list_model_get_n_items (toplevels);
  GtkWidget *window;
  int idx;

  idx = 0;
  for (guint i = 0; i < n_toplevels; i++)
    {
      window = g_list_model_get_item (toplevels, i);

      g_object_unref (window);

      if (!gtk_widget_get_visible (window))
        continue;

      if (window == widget)
        return idx;

      idx += 1;
    }

  return -1;
}

static void
handle_accessible_method (GDBusConnection       *connection,
                          const gchar           *sender,
                          const gchar           *object_path,
                          const gchar           *interface_name,
                          const gchar           *method_name,
                          GVariant              *parameters,
                          GDBusMethodInvocation *invocation,
                          gpointer               user_data)
{
  GtkAtSpiContext *self = user_data;

  if (g_strcmp0 (method_name, "GetRole") == 0)
    {
      guint atspi_role = gtk_atspi_role_for_context (GTK_AT_CONTEXT (self));

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", atspi_role));
    }
  else if (g_strcmp0 (method_name, "GetRoleName") == 0)
    {
      GtkAccessibleRole role = gtk_at_context_get_accessible_role (GTK_AT_CONTEXT (self));
      const char *name = gtk_accessible_role_to_name (role, NULL);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", name));
    }
  else if (g_strcmp0 (method_name, "GetLocalizedRoleName") == 0)
    {
      GtkAccessibleRole role = gtk_at_context_get_accessible_role (GTK_AT_CONTEXT (self));
      const char *name = gtk_accessible_role_to_name (role, GETTEXT_PACKAGE);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", name));
    }
  else if (g_strcmp0 (method_name, "GetState") == 0)
    {
      GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(au)"));

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("au"));
      collect_states (self, &builder);
      g_variant_builder_close (&builder);

      g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (&builder));
    }
  else if (g_strcmp0 (method_name, "GetAttributes") == 0)
    {
      GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(a{ss})"));

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{ss}"));
      g_variant_builder_add (&builder, "{ss}", "toolkit", "GTK");

      if (gtk_at_context_has_accessible_property (GTK_AT_CONTEXT (self), GTK_ACCESSIBLE_PROPERTY_PLACEHOLDER))
        {
          GtkAccessibleValue *value;

          value = gtk_at_context_get_accessible_property (GTK_AT_CONTEXT (self), GTK_ACCESSIBLE_PROPERTY_PLACEHOLDER);

          g_variant_builder_add (&builder, "{ss}",
                                 "placeholder-text", gtk_string_accessible_value_get (value));
        }

      g_variant_builder_close (&builder);

      g_dbus_method_invocation_return_value (invocation, g_variant_builder_end (&builder));
    }
  else if (g_strcmp0 (method_name, "GetApplication") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(@(so))", gtk_at_spi_root_to_ref (self->root)));
    }
  else if (g_strcmp0 (method_name, "GetChildAtIndex") == 0)
    {
      GtkWidget *child = NULL;
      int idx, real_idx = 0;

      g_variant_get (parameters, "(i)", &idx);

      GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
      GtkWidget *widget = GTK_WIDGET (accessible);

      real_idx = 0;
      for (child = gtk_widget_get_first_child (widget);
           child;
           child = gtk_widget_get_next_sibling (child))
        {
          if (!gtk_accessible_should_present (GTK_ACCESSIBLE (child)))
            continue;

          if (real_idx == idx)
            break;

          real_idx += 1;
        }

      if (child == NULL)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_INVALID_ARGUMENT,
                                                 "No child with index %d", idx);
          return;
        }

      GtkATContext *context = gtk_accessible_get_at_context (GTK_ACCESSIBLE (child));

      const char *name = g_dbus_connection_get_unique_name (self->connection);
      const char *path = gtk_at_spi_context_get_context_path (GTK_AT_SPI_CONTEXT (context));

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("((so))", name, path));
    }
  else if (g_strcmp0 (method_name, "GetChildren") == 0)
    {
      GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(so)"));

      GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
      GtkWidget *widget = GTK_WIDGET (accessible);
      GtkWidget *child;

      for (child = gtk_widget_get_first_child (widget);
           child;
           child = gtk_widget_get_next_sibling (child))
        {
          if (!gtk_accessible_should_present (GTK_ACCESSIBLE (child)))
            continue;

          GtkATContext *context = gtk_accessible_get_at_context (GTK_ACCESSIBLE (child));

          const char *name = g_dbus_connection_get_unique_name (self->connection);
          const char *path = gtk_at_spi_context_get_context_path (GTK_AT_SPI_CONTEXT (context));

          g_variant_builder_add (&builder, "(so)", name, path);
        }

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(so))", &builder));
    }
  else if (g_strcmp0 (method_name, "GetIndexInParent") == 0)
    {
      GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
      int idx;

      if (GTK_IS_ROOT (accessible))
        idx = get_index_in_toplevels (GTK_WIDGET (accessible));
      else
        idx = get_index_in_parent (GTK_WIDGET (accessible));

      if (idx == -1)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not found");
      else
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(i)", idx));
    }
  else if (g_strcmp0 (method_name, "GetRelationSet") == 0)
    {
      GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(ua(so))"));
       collect_relations (self, &builder);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(ua(so)))", &builder));
    }
  else if (g_strcmp0 (method_name, "GetInterfaces") == 0)
    {
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(@as)", self->interfaces));
    }

}

static GVariant *
handle_accessible_get_property (GDBusConnection       *connection,
                                const gchar           *sender,
                                const gchar           *object_path,
                                const gchar           *interface_name,
                                const gchar           *property_name,
                                GError               **error,
                                gpointer               user_data)
{
  GtkAtSpiContext *self = user_data;
  GVariant *res = NULL;

  GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
  GtkWidget *widget = GTK_WIDGET (accessible);

  if (g_strcmp0 (property_name, "Name") == 0)
    res = g_variant_new_string (gtk_widget_get_name (widget));
  else if (g_strcmp0 (property_name, "Description") == 0)
    {
      char *label = gtk_at_context_get_label (GTK_AT_CONTEXT (self));
      res = g_variant_new_string (label);
      g_free (label);
    }
  else if (g_strcmp0 (property_name, "Locale") == 0)
    res = g_variant_new_string (setlocale (LC_MESSAGES, NULL));
  else if (g_strcmp0 (property_name, "AccessibleId") == 0)
    res = g_variant_new_string ("");
  else if (g_strcmp0 (property_name, "Parent") == 0)
    {
      GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (accessible));

      if (parent == NULL)
        {
          res = gtk_at_spi_root_to_ref (self->root);
        }
      else
        {
          GtkATContext *parent_context =
            gtk_accessible_get_at_context (GTK_ACCESSIBLE (parent));

          if (parent_context != NULL)
            res = g_variant_new ("(so)",
                                 g_dbus_connection_get_unique_name (self->connection),
                                 GTK_AT_SPI_CONTEXT (parent_context)->context_path);
        }

      if (res == NULL)
        res = gtk_at_spi_null_ref ();
    }
  else if (g_strcmp0 (property_name, "ChildCount") == 0)
    {
      int n_children = 0;
      GtkWidget *child;

      for (child = gtk_widget_get_first_child (widget);
           child;
           child = gtk_widget_get_next_sibling (child))
        {
          if (!gtk_accessible_should_present (GTK_ACCESSIBLE (child)))
            continue;

          n_children++;
        }

      res = g_variant_new_int32 (n_children);
    }
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Unknown property '%s'", property_name);

  return res;
}

static const GDBusInterfaceVTable accessible_vtable = {
  handle_accessible_method,
  handle_accessible_get_property,
  NULL,
};

static void
gtk_at_spi_context_register_object (GtkAtSpiContext *self)
{
  GtkWidget *widget = GTK_WIDGET (gtk_at_context_get_accessible (GTK_AT_CONTEXT (self)));
  GVariantBuilder interfaces = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_STRING_ARRAY);
  const GDBusInterfaceVTable *vtable;

  g_variant_builder_add (&interfaces, "s", atspi_accessible_interface.name);
  self->registration_ids[self->n_registered_objects] =
      g_dbus_connection_register_object (self->connection,
                                         self->context_path,
                                         (GDBusInterfaceInfo *) &atspi_accessible_interface,
                                         &accessible_vtable,
                                         self,
                                         NULL,
                                         NULL);
  self->n_registered_objects++;

  vtable = gtk_atspi_get_component_vtable (widget);
  if (vtable)
    {
      g_variant_builder_add (&interfaces, "s", atspi_component_interface.name);
      self->registration_ids[self->n_registered_objects] =
          g_dbus_connection_register_object (self->connection,
                                             self->context_path,
                                             (GDBusInterfaceInfo *) &atspi_component_interface,
                                             vtable,
                                             self,
                                             NULL,
                                             NULL);
      self->n_registered_objects++;
    }

  vtable = gtk_atspi_get_text_vtable (widget);
  if (vtable)
    {
      g_variant_builder_add (&interfaces, "s", atspi_text_interface.name);
      self->registration_ids[self->n_registered_objects] =
          g_dbus_connection_register_object (self->connection,
                                             self->context_path,
                                             (GDBusInterfaceInfo *) &atspi_text_interface,
                                             vtable,
                                             self,
                                             NULL,
                                             NULL);
      self->n_registered_objects++;
    }

  vtable = gtk_atspi_get_editable_text_vtable (widget);
  if (vtable)
    {
      g_variant_builder_add (&interfaces, "s", atspi_editable_text_interface.name);
      self->registration_ids[self->n_registered_objects] =
          g_dbus_connection_register_object (self->connection,
                                             self->context_path,
                                             (GDBusInterfaceInfo *) &atspi_editable_text_interface,
                                             vtable,
                                             self,
                                             NULL,
                                             NULL);
      self->n_registered_objects++;
    }
  vtable = gtk_atspi_get_value_vtable (widget);
  if (vtable)
    {
      g_variant_builder_add (&interfaces, "s", atspi_value_interface.name);
      self->registration_ids[self->n_registered_objects] =
          g_dbus_connection_register_object (self->connection,
                                             self->context_path,
                                             (GDBusInterfaceInfo *) &atspi_value_interface,
                                             vtable,
                                             self,
                                             NULL,
                                             NULL);
      self->n_registered_objects++;
    }

  vtable = gtk_atspi_get_selection_vtable (widget);
  if (vtable)
    {
      g_variant_builder_add (&interfaces, "s", atspi_selection_interface.name);
      self->registration_ids[self->n_registered_objects] =
          g_dbus_connection_register_object (self->connection,
                                             self->context_path,
                                             (GDBusInterfaceInfo *) &atspi_selection_interface,
                                             vtable,
                                             self,
                                             NULL,
                                             NULL);
      self->n_registered_objects++;
    }

  self->interfaces = g_variant_ref_sink (g_variant_builder_end (&interfaces));
}

static void
gtk_at_spi_context_unregister_object (GtkAtSpiContext *self)
{
  while (self->n_registered_objects > 0)
    {
      self->n_registered_objects--;
      g_dbus_connection_unregister_object (self->connection,
                                           self->registration_ids[self->n_registered_objects]);
      self->registration_ids[self->n_registered_objects] = 0;
    }
}

static void
emit_text_changed (GtkAtSpiContext *self,
                   const char      *kind,
                   int              start,
                   int              end,
                   const char      *text)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "TextChanged",
                                 g_variant_new ("(siiva{sv})",
                                                kind, start, end, g_variant_new_string (text), NULL),
                                 NULL);
}

static void
emit_text_selection_changed (GtkAtSpiContext *self,
                             const char      *kind,
                             int              cursor_position)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "TextChanged",
                                 g_variant_new ("(siiva{sv})",
                                                kind, cursor_position, 0, g_variant_new_string (""), NULL),
                                 NULL);
}

static void
emit_selection_changed (GtkAtSpiContext *self,
                        const char      *kind)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "SelectionChanged",
                                 g_variant_new ("(siiva{sv})",
                                                "", 0, 0, g_variant_new_string (""), NULL),
                                 NULL);
}

static void
emit_state_changed (GtkAtSpiContext *self,
                    const char      *name,
                    gboolean         enabled)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "StateChanged",
                                 g_variant_new ("(siiva{sv})",
                                                name, enabled, 0, g_variant_new_string ("0"), NULL),
                                 NULL);
}

static void
emit_property_changed (GtkAtSpiContext *self,
                       const char      *name,
                       GVariant        *value)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "PropertyChange",
                                 g_variant_new ("(siiva{sv})",
                                                name, 0, 0, value, NULL),
                                 NULL);
}

static void
emit_bounds_changed (GtkAtSpiContext *self,
                     int              x,
                     int              y,
                     int              width,
                     int              height)
{
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 self->context_path,
                                 "org.a11y.atspi.Event.Object",
                                 "BoundsChanged",
                                 g_variant_new ("(siiva{sv})",
                                                "", 0, 0, g_variant_new ("(iiii)", x, y, width, height), NULL),
                                 NULL);
}

static void
gtk_at_spi_context_state_change (GtkATContext                *ctx,
                                 GtkAccessibleStateChange     changed_states,
                                 GtkAccessiblePropertyChange  changed_properties,
                                 GtkAccessibleRelationChange  changed_relations,
                                 GtkAccessiblePlatformChange  changed_platform,
                                 GtkAccessibleAttributeSet   *states,
                                 GtkAccessibleAttributeSet   *properties,
                                 GtkAccessibleAttributeSet   *relations)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (ctx);
  GtkWidget *widget = GTK_WIDGET (gtk_at_context_get_accessible (ctx));
  GtkAccessibleValue *value;

  if (!gtk_widget_get_realized (widget))
    return;

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_BUSY)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_BUSY);
      emit_state_changed (self, "busy", gtk_boolean_accessible_value_get (value));
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_CHECKED)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_CHECKED);

      switch (gtk_tristate_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_TRISTATE_TRUE:
          emit_state_changed (self, "checked", TRUE);
          emit_state_changed (self, "indeterminate", FALSE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_MIXED:
          emit_state_changed (self, "checked", FALSE);
          emit_state_changed (self, "indeterminate", TRUE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_FALSE:
          emit_state_changed (self, "checked", FALSE);
          emit_state_changed (self, "indeterminate", FALSE);
        default:
          break;
        }
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_DISABLED)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_DISABLED);
      emit_state_changed (self, "sensitive", !gtk_boolean_accessible_value_get (value));
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_EXPANDED)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_EXPANDED);
      if (value->value_class->type == GTK_ACCESSIBLE_VALUE_TYPE_BOOLEAN)
        {
          emit_state_changed (self, "expandable", TRUE);
          emit_state_changed (self, "expanded",gtk_boolean_accessible_value_get (value));
        }
      else
        emit_state_changed (self, "expandable", FALSE);
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_INVALID)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_INVALID);
      switch (gtk_invalid_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_INVALID_TRUE:
        case GTK_ACCESSIBLE_INVALID_GRAMMAR:
        case GTK_ACCESSIBLE_INVALID_SPELLING:
          emit_state_changed (self, "invalid", TRUE);
          break;
        case GTK_ACCESSIBLE_INVALID_FALSE:
          emit_state_changed (self, "invalid", FALSE);
        default:
          break;
        }
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_PRESSED)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_PRESSED);
      switch (gtk_tristate_accessible_value_get (value))
        {
        case GTK_ACCESSIBLE_TRISTATE_TRUE:
          emit_state_changed (self, "pressed", TRUE);
          emit_state_changed (self, "indeterminate", FALSE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_MIXED:
          emit_state_changed (self, "pressed", FALSE);
          emit_state_changed (self, "indeterminate", TRUE);
          break;
        case GTK_ACCESSIBLE_TRISTATE_FALSE:
          emit_state_changed (self, "pressed", FALSE);
          emit_state_changed (self, "indeterminate", FALSE);
        default:
          break;
        }
    }

  if (changed_states & GTK_ACCESSIBLE_STATE_CHANGE_SELECTED)
    {
      value = gtk_accessible_attribute_set_get_value (states, GTK_ACCESSIBLE_STATE_SELECTED);
      if (value->value_class->type == GTK_ACCESSIBLE_VALUE_TYPE_BOOLEAN)
        {
          emit_state_changed (self, "selectable", TRUE);
          emit_state_changed (self, "selected",gtk_boolean_accessible_value_get (value));
        }
      else
        emit_state_changed (self, "selectable", FALSE);
    }

  if (changed_properties & GTK_ACCESSIBLE_PROPERTY_CHANGE_READ_ONLY)
    {
      gboolean readonly;

      value = gtk_accessible_attribute_set_get_value (properties, GTK_ACCESSIBLE_PROPERTY_READ_ONLY);
      readonly = gtk_boolean_accessible_value_get (value);

      emit_state_changed (self, "read-only", readonly);
      if (ctx->accessible_role == GTK_ACCESSIBLE_ROLE_TEXT_BOX)
        emit_state_changed (self, "editable", !readonly);
    }

  if (changed_properties & GTK_ACCESSIBLE_PROPERTY_CHANGE_ORIENTATION)
    {
      value = gtk_accessible_attribute_set_get_value (properties, GTK_ACCESSIBLE_PROPERTY_ORIENTATION);
      if (gtk_orientation_accessible_value_get (value) == GTK_ORIENTATION_HORIZONTAL)
        {
          emit_state_changed (self, "horizontal", TRUE);
          emit_state_changed (self, "vertical", FALSE);
        }
      else
        {
          emit_state_changed (self, "horizontal", FALSE);
          emit_state_changed (self, "vertical", TRUE);
        }
    }

  if (changed_properties & GTK_ACCESSIBLE_PROPERTY_CHANGE_MODAL)
    {
      value = gtk_accessible_attribute_set_get_value (properties, GTK_ACCESSIBLE_PROPERTY_MODAL);
      emit_state_changed (self, "modal", gtk_boolean_accessible_value_get (value));
    }

  if (changed_properties & GTK_ACCESSIBLE_PROPERTY_CHANGE_MULTI_LINE)
    {
      value = gtk_accessible_attribute_set_get_value (properties, GTK_ACCESSIBLE_PROPERTY_MULTI_LINE);
      emit_state_changed (self, "multi-line", gtk_boolean_accessible_value_get (value));
    }

  if (changed_properties & GTK_ACCESSIBLE_PROPERTY_CHANGE_LABEL)
    {
      char *label = gtk_at_context_get_label (GTK_AT_CONTEXT (self));
      GVariant *v = g_variant_new_take_string (label);
      emit_property_changed (self, "accessible-description", v);
    }

  if (changed_platform & GTK_ACCESSIBLE_PLATFORM_CHANGE_FOCUSABLE)
    {
      gboolean state = gtk_accessible_get_platform_state (GTK_ACCESSIBLE (widget),
                                                          GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSABLE);
      emit_state_changed (self, "focusable", state);
    }

  if (changed_platform & GTK_ACCESSIBLE_PLATFORM_CHANGE_FOCUSED)
    {
      gboolean state = gtk_accessible_get_platform_state (GTK_ACCESSIBLE (widget),
                                                          GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSED);
      emit_state_changed (self, "focused", state);
    }

  if (changed_platform & GTK_ACCESSIBLE_PLATFORM_CHANGE_SIZE)
    {
      double x, y;
      int width, height;

      gtk_widget_translate_coordinates (widget,
                                        GTK_WIDGET (gtk_widget_get_root (widget)),
                                        0, 0, &x, &y);
      width = gtk_widget_get_width (widget);
      height = gtk_widget_get_height (widget);
      emit_bounds_changed (self, (int)x, (int)y, width, height);
    }
}

static void
gtk_at_spi_context_dispose (GObject *gobject)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (gobject);
  GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));

  gtk_at_spi_context_unregister_object (self);
  gtk_atspi_disconnect_text_signals (GTK_WIDGET (accessible));
  gtk_atspi_disconnect_selection_signals (GTK_WIDGET (accessible));

  G_OBJECT_CLASS (gtk_at_spi_context_parent_class)->dispose (gobject);
}

static void
gtk_at_spi_context_finalize (GObject *gobject)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (gobject);

  g_free (self->bus_address);
  g_free (self->context_path);
  g_clear_pointer (&self->interfaces, g_variant_unref);

  G_OBJECT_CLASS (gtk_at_spi_context_parent_class)->finalize (gobject);
}

static void
gtk_at_spi_context_set_property (GObject      *gobject,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (gobject);

  switch (prop_id)
    {
    case PROP_BUS_ADDRESS:
      self->bus_address = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
    }
}

static void
gtk_at_spi_context_get_property (GObject    *gobject,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (gobject);

  switch (prop_id)
    {
    case PROP_BUS_ADDRESS:
      g_value_set_string (value, self->bus_address);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
    }
}

static void
gtk_at_spi_context_constructed (GObject *gobject)
{
  GtkAtSpiContext *self = GTK_AT_SPI_CONTEXT (gobject);
  GdkDisplay *display;

  g_assert (self->bus_address);

  /* Every GTK application has a single root AT-SPI object, which
   * handles all the global state, including the cache of accessible
   * objects. We use the GdkDisplay to store it, so it's guaranteed
   * to be unique per-display connection
   */
  display = gtk_at_context_get_display (GTK_AT_CONTEXT (self));
  self->root =
    g_object_get_data (G_OBJECT (display), "-gtk-atspi-root");

  if (self->root == NULL)
    {
      self->root = gtk_at_spi_root_new (self->bus_address);
      g_object_set_data_full (G_OBJECT (display), "-gtk-atspi-root",
                              self->root,
                              g_object_unref);
    }

  self->connection = gtk_at_spi_root_get_connection (self->root);

  /* We use the application's object path to build the path of each
   * accessible object exposed on the accessibility bus; the path is
   * also used to access the object cache
   */
  GApplication *application = g_application_get_default ();
  char *base_path = NULL;

  if (application != NULL)
    {
      const char *app_path = g_application_get_dbus_object_path (application);
      base_path = g_strconcat (app_path, "/a11y", NULL);
    }
  else
    {
      char *uuid = g_uuid_string_random ();
      base_path = g_strconcat ("/org/gtk/application/", uuid, "/a11y", NULL);
      g_free (uuid);
    }

  /* We use a unique id to ensure that we don't have conflicting
   * objects on the bus
   */
  char *uuid = g_uuid_string_random ();

  self->context_path = g_strconcat (base_path, "/", uuid, NULL);

  /* UUIDs use '-' as the separator, but that's not a valid character
   * for a DBus object path
   */
  size_t path_len = strlen (self->context_path);
  for (size_t i = 0; i < path_len; i++)
    {
      if (self->context_path[i] == '-')
        self->context_path[i] = '_';
    }

  g_free (base_path);
  g_free (uuid);

  GtkAccessible *accessible = gtk_at_context_get_accessible (GTK_AT_CONTEXT (self));
  gtk_atspi_connect_text_signals (GTK_WIDGET (accessible),
                                  (GtkAtspiTextChangedCallback *)emit_text_changed,
                                  (GtkAtspiTextSelectionCallback *)emit_text_selection_changed,
                                  self);
  gtk_atspi_connect_selection_signals (GTK_WIDGET (accessible),
                                       (GtkAtspiSelectionCallback *)emit_selection_changed,
                                       self);
  gtk_at_spi_context_register_object (self);

  G_OBJECT_CLASS (gtk_at_spi_context_parent_class)->constructed (gobject);
}

static void
gtk_at_spi_context_class_init (GtkAtSpiContextClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkATContextClass *context_class = GTK_AT_CONTEXT_CLASS (klass);

  gobject_class->constructed = gtk_at_spi_context_constructed;
  gobject_class->set_property = gtk_at_spi_context_set_property;
  gobject_class->get_property = gtk_at_spi_context_get_property;
  gobject_class->finalize = gtk_at_spi_context_finalize;
  gobject_class->dispose = gtk_at_spi_context_dispose;

  context_class->state_change = gtk_at_spi_context_state_change;

  obj_props[PROP_BUS_ADDRESS] =
    g_param_spec_string ("bus-address", NULL, NULL,
                         NULL,
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPS, obj_props);
}

static void
gtk_at_spi_context_init (GtkAtSpiContext *self)
{
}

#ifdef GDK_WINDOWING_X11
static char *
get_bus_address_x11 (GdkDisplay *display)
{
  GTK_NOTE (A11Y, g_message ("Acquiring a11y bus via X11..."));

  Display *xdisplay = gdk_x11_display_get_xdisplay (display);
  Atom type_return;
  int format_return;
  gulong nitems_return;
  gulong bytes_after_return;
  guchar *data = NULL;
  char *address = NULL;

  gdk_x11_display_error_trap_push (display);
  XGetWindowProperty (xdisplay, DefaultRootWindow (xdisplay),
                      gdk_x11_get_xatom_by_name_for_display (display, "AT_SPI_BUS"),
                      0L, BUFSIZ, False,
                      (Atom) 31,
                      &type_return, &format_return, &nitems_return,
                      &bytes_after_return, &data);
  gdk_x11_display_error_trap_pop_ignored (display);

  address = g_strdup ((char *) data);

  XFree (data);

  return address;
}
#endif

#if defined(GDK_WINDOWING_WAYLAND) || defined(GDK_WINDOWING_X11)
static char *
get_bus_address_dbus (GdkDisplay *display)
{
  GTK_NOTE (A11Y, g_message ("Acquiring a11y bus via DBus..."));

  GError *error = NULL;
  GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

  if (error != NULL)
    {
      g_critical ("Unable to acquire session bus: %s", error->message);
      g_error_free (error);
      return NULL;
    }

  GVariant *res =
    g_dbus_connection_call_sync (connection, "org.a11y.Bus",
                                  "/org/a11y/bus",
                                  "org.a11y.Bus",
                                  "GetAddress",
                                  NULL, NULL,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  NULL,
                                  &error);
  if (error != NULL)
    {
      g_critical ("Unable to acquire the address of the accessibility bus: %s",
                  error->message);
      g_error_free (error);
    }

  char *address = NULL;
  if (res != NULL)
    {
      g_variant_get (res, "(s)", &address);
      g_variant_unref (res);
    }

  g_object_unref (connection);

  return address;
}
#endif

static const char *
get_bus_address (GdkDisplay *display)
{
  const char *bus_address;

  bus_address = g_object_get_data (G_OBJECT (display), "-gtk-atspi-bus-address");
  if (bus_address != NULL)
    return bus_address;

  /* The bus address environment variable takes precedence; this is the
   * mechanism used by Flatpak to handle the accessibility bus portal
   * between the sandbox and the outside world
   */
  bus_address = g_getenv ("AT_SPI_BUS_ADDRESS");
  if (bus_address != NULL && *bus_address != '\0')
    {
      GTK_NOTE (A11Y, g_message ("Using ATSPI bus address from environment: %s", bus_address));
      g_object_set_data_full (G_OBJECT (display), "-gtk-atspi-bus-address",
                              g_strdup (bus_address),
                              g_free);
      goto out;
    }

#if defined(GDK_WINDOWING_WAYLAND)
  if (bus_address == NULL)
    {
      if (GDK_IS_WAYLAND_DISPLAY (display))
        {
          char *addr = get_bus_address_dbus (display);

          GTK_NOTE (A11Y, g_message ("Using ATSPI bus address from D-Bus: %s", addr));
          g_object_set_data_full (G_OBJECT (display), "-gtk-atspi-bus-address",
                                  addr,
                                  g_free);

          bus_address = addr;
        }
    }
#endif
#if defined(GDK_WINDOWING_X11)
  if (bus_address == NULL)
    {
      if (GDK_IS_X11_DISPLAY (display))
        {
          char *addr = get_bus_address_dbus (display);

          if (addr == NULL)
            {
              addr = get_bus_address_x11 (display);
              GTK_NOTE (A11Y, g_message ("Using ATSPI bus address from X11: %s", addr));
            }
          else
            {
              GTK_NOTE (A11Y, g_message ("Using ATSPI bus address from D-Bus: %s", addr));
            }

          g_object_set_data_full (G_OBJECT (display), "-gtk-atspi-bus-address",
                                  addr,
                                  g_free);

          bus_address = addr;
        }
    }
#endif

out:
  return bus_address;
}

GtkATContext *
gtk_at_spi_create_context (GtkAccessibleRole  accessible_role,
                           GtkAccessible     *accessible,
                           GdkDisplay        *display)
{
  g_return_val_if_fail (GTK_IS_ACCESSIBLE (accessible), NULL);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  const char *bus_address = get_bus_address (display);

  if (bus_address == NULL)
    return NULL;

#if defined(GDK_WINDOWING_WAYLAND)
  if (GDK_IS_WAYLAND_DISPLAY (display))
    return g_object_new (GTK_TYPE_AT_SPI_CONTEXT,
                         "accessible-role", accessible_role,
                         "accessible", accessible,
                         "display", display,
                         "bus-address", bus_address,
                         NULL);
#endif
#if defined(GDK_WINDOWING_X11)
  if (GDK_IS_X11_DISPLAY (display))
    return g_object_new (GTK_TYPE_AT_SPI_CONTEXT,
                         "accessible-role", accessible_role,
                         "accessible", accessible,
                         "display", display,
                         "bus-address", bus_address,
                         NULL);
#endif

  return NULL;
}

const char *
gtk_at_spi_context_get_context_path (GtkAtSpiContext *self)
{
  g_return_val_if_fail (GTK_IS_AT_SPI_CONTEXT (self), NULL);

  return self->context_path;
}

/*< private >
 * gtk_at_spi_context_to_ref:
 * @self: a #GtkAtSpiContext
 *
 * Returns an ATSPI object reference for the #GtkAtSpiContext.
 *
 * Returns: (transfer floating): a #GVariant with the reference
 */
GVariant *
gtk_at_spi_context_to_ref (GtkAtSpiContext *self)
{
  const char *name = g_dbus_connection_get_unique_name (self->connection);
  return g_variant_new ("(so)", name, self->context_path);
}
