/* gtkatspicomponent.c: AT-SPI Component implementation
 *
 * Copyright 2020 Red Hat, Inc.
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

#include "gtkatspicomponentprivate.h"

#include "gtkatspicontextprivate.h"
#include "gtkatspiprivate.h"
#include "gtkatspiutilsprivate.h"
#include "gtkaccessibleprivate.h"

#include "a11y/atspi/atspi-component.h"

#include "gtkdebug.h"

#include <gio/gio.h>

static void
translate_coordinates_to_widget (GtkWidget      *widget,
                                 AtspiCoordType  coordtype,
                                 int             xi,
                                 int             yi,
                                 int            *xo,
                                 int            *yo)
{
  double x = xi;
  double y = yi;

  switch (coordtype)
    {
    case ATSPI_COORD_TYPE_SCREEN:
      g_warning ("Not suppporting screen coordinates, reported positions will be wrong");
      G_GNUC_FALLTHROUGH;

    case ATSPI_COORD_TYPE_WINDOW:
      gtk_widget_translate_coordinates (GTK_WIDGET (gtk_widget_get_root (widget)),
                                        widget,
                                        x, y,
                                        &x, &y);
      break;

    case ATSPI_COORD_TYPE_PARENT:
      gtk_widget_translate_coordinates (gtk_widget_get_parent (widget),
                                        widget,
                                        x, y,
                                        &x, &y);
      break;

    default:
      g_assert_not_reached ();
    }

  *xo = (int)x;
  *yo = (int)y;
}

static void
translate_coordinates_from_widget (GtkWidget      *widget,
                                   AtspiCoordType  coordtype,
                                   int             xi,
                                   int             yi,
                                   int            *xo,
                                   int            *yo)
{
  double x = xi;
  double y = yi;

  switch (coordtype)
    {
    case ATSPI_COORD_TYPE_SCREEN:
      g_warning ("Not suppporting screen coordinates, reported positions will be wrong");
      G_GNUC_FALLTHROUGH;

    case ATSPI_COORD_TYPE_WINDOW:
      gtk_widget_translate_coordinates (widget,
                                        GTK_WIDGET (gtk_widget_get_root (widget)),
                                        x, y,
                                        &x, &y);
      break;

    case ATSPI_COORD_TYPE_PARENT:
      gtk_widget_translate_coordinates (widget,
                                        gtk_widget_get_parent (widget),
                                        x, y,
                                        &x, &y);
      break;

    default:
      g_assert_not_reached ();
    }

  *xo = (int)x;
  *yo = (int)y;
}

static void
component_handle_method (GDBusConnection       *connection,
                         const gchar           *sender,
                         const gchar           *object_path,
                         const gchar           *interface_name,
                         const gchar           *method_name,
                         GVariant              *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer               user_data)
{
  GtkATContext *self = user_data;
  GtkAccessible *accessible = gtk_at_context_get_accessible (self);
  GtkWidget *widget = GTK_WIDGET (accessible);

g_print ("%s %s\n", interface_name, method_name);
  if (g_strcmp0 (method_name, "Contains") == 0)
    {
      int x, y;
      AtspiCoordType coordtype;
      gboolean ret;

      g_variant_get (parameters, "(iiu)", &x, &y, &coordtype);

      translate_coordinates_to_widget (widget, coordtype, x, y, &x, &y);

      ret = gtk_widget_contains (widget, x, y);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", ret));
    }
  else if (g_strcmp0 (method_name, "GetAccessibleAtPoint") == 0)
    {
      int x, y;
      AtspiCoordType coordtype;
      GtkWidget *child;

      g_variant_get (parameters, "(iiu)", &x, &y, &coordtype);

      translate_coordinates_to_widget (widget, coordtype, x, y, &x, &y);

      child = gtk_widget_pick (widget, x, y, GTK_PICK_DEFAULT);
      if (!child)
        {
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(@(so))", gtk_at_spi_null_ref ()));
        }
      else
        {
          GtkAtSpiContext *ctx = GTK_AT_SPI_CONTEXT (gtk_accessible_get_at_context (GTK_ACCESSIBLE (child)));

          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(@(so))", gtk_at_spi_context_to_ref (ctx)));
        }
    }
  else if (g_strcmp0 (method_name, "GetExtents") == 0)
    {
      AtspiCoordType coordtype;
      int x, y;
      int width = gtk_widget_get_width (widget);
      int height = gtk_widget_get_height (widget);

      g_variant_get (parameters, "(u)", &coordtype);

      translate_coordinates_from_widget (widget, coordtype, 0, 0, &x, &y);

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("((iiii))", x, y, width, height));
    }
  else if (g_strcmp0 (method_name, "GetPosition") == 0)
    {
      AtspiCoordType coordtype;
      int x, y;

      g_variant_get (parameters, "(u)", &coordtype);

      translate_coordinates_from_widget (widget, coordtype, 0, 0, &x, &y);

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(ii)", x, y));
    }
  else if (g_strcmp0 (method_name, "GetSize") == 0)
    {
      int width = gtk_widget_get_width (widget);
      int height = gtk_widget_get_height (widget);

      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(ii)", width, height));
    }
  else if (g_strcmp0 (method_name, "GetLayer") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "GetMDIZOrder") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "GrabFocus") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "GetAlpha") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "SetExtents") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "SetPosition") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "SetSize") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "ScrollTo") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
  else if (g_strcmp0 (method_name, "ScrollToPoint") == 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "");
    }
}

static const GDBusInterfaceVTable component_vtable = {
  component_handle_method,
};

const GDBusInterfaceVTable *
gtk_atspi_get_component_vtable (GtkWidget *widget)
{
  return &component_vtable;
}
