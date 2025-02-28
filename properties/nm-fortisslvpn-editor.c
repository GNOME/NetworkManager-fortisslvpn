/***************************************************************************
 * Copyright (C) 2015,2017 Lubomir Rintel <lkundrak@v3.sk>
 * Copyright (C) 2008 Dan Williams, <dcbw@redhat.com>
 * Copyright (C) 2008 - 2011 Red Hat, Inc.
 * Based on work by David Zeuthen, <davidz@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **************************************************************************/

#include "nm-default.h"

#include "nm-fortisslvpn-editor.h"
#include "nm-fortissl-properties.h"

#include "nma-cert-chooser.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

/*****************************************************************************/

#if !GTK_CHECK_VERSION(4,0,0)
#define gtk_editable_set_text(editable,text)	gtk_entry_set_text(GTK_ENTRY(editable), (text))
#define gtk_editable_get_text(editable)		gtk_entry_get_text(GTK_ENTRY(editable))
#define gtk_widget_get_root(widget)		gtk_widget_get_toplevel(widget)
#define gtk_window_set_hide_on_close(window, hide)						\
	G_STMT_START {										\
		G_STATIC_ASSERT(hide);								\
		g_signal_connect_swapped (G_OBJECT (window), "delete-event",			\
		                          G_CALLBACK (gtk_widget_hide_on_delete), window);	\
	} G_STMT_END
#endif

/*****************************************************************************/

typedef struct {
	GtkBuilder *builder;
	GtkWidget *widget;
	GtkWindowGroup *window_group;
	gboolean window_added;
	gboolean new_connection;
	gchar *trusted_cert;
	gchar *realm;
	NMSettingSecretFlags otp_flags;
} FortisslvpnEditorPrivate;

static void fortisslvpn_editor_interface_init (NMVpnEditorInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (FortisslvpnEditor, fortisslvpn_editor, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR,
                                               fortisslvpn_editor_interface_init))

#define FORTISSLVPN_EDITOR_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), FORTISSLVPN_TYPE_EDITOR, FortisslvpnEditorPrivate))

/*****************************************************************************/

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
	g_signal_emit_by_name (FORTISSLVPN_EDITOR (user_data), "changed");
}

static void
setup_password_widget (FortisslvpnEditor *self,
                       const char *entry_name,
                       NMSettingVpn *s_vpn,
                       const char *secret_name,
                       gboolean new_connection)
{
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	GtkWidget *widget;
	const char *value;

	widget = (GtkWidget *) gtk_builder_get_object (priv->builder, entry_name);
	g_assert (widget);

	if (s_vpn) {
		value = nm_setting_vpn_get_secret (s_vpn, secret_name);
		gtk_editable_set_text (GTK_EDITABLE (widget), value ? value : "");
	}

	g_signal_connect (widget, "changed", G_CALLBACK (stuff_changed_cb), self);
}

static void
show_toggled_cb (GtkCheckButton *button, FortisslvpnEditor *self)
{
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	GtkWidget *widget;
	gboolean visible;

	visible = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	g_assert (widget);
	gtk_entry_set_visibility (GTK_ENTRY (widget), visible);
}

static void
password_storage_changed_cb (GObject *entry,
                             GParamSpec *pspec,
                             gpointer user_data)
{
	FortisslvpnEditor *self = FORTISSLVPN_EDITOR (user_data);

	stuff_changed_cb (NULL, self);
}

static void
init_password_icon (FortisslvpnEditor *self,
                    NMSettingVpn *s_vpn,
                    const char *secret_key,
                    const char *entry_name)
{
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	GtkWidget *entry;
	const char *value = NULL;
	NMSettingSecretFlags pw_flags = NM_SETTING_SECRET_FLAG_NONE;

	/* If there's already a password and the password type can't be found in
	 * the VPN settings, default to saving it.  Otherwise, always ask for it.
	 */
	entry = GTK_WIDGET (gtk_builder_get_object (priv->builder, entry_name));
	g_assert (entry);

	nma_utils_setup_password_storage (entry, 0, (NMSetting *) s_vpn, secret_key,
	                                  TRUE, FALSE);

	/* If there's no password and no flags in the setting,
	 * initialize flags as "always-ask".
	 */
	if (s_vpn)
		nm_setting_get_secret_flags (NM_SETTING (s_vpn), secret_key, &pw_flags, NULL);
	value = gtk_editable_get_text (GTK_EDITABLE (entry));
	if ((!value || !*value) && (pw_flags == NM_SETTING_SECRET_FLAG_NONE))
		nma_utils_update_password_storage (entry, NM_SETTING_SECRET_FLAG_NOT_SAVED,
		                                   (NMSetting *) s_vpn, secret_key);

	g_signal_connect (entry, "notify::secondary-icon-name",
	                  G_CALLBACK (password_storage_changed_cb), self);
}

static void
advanced_dialog_response_cb (GtkWidget *dialog, gint response, gpointer user_data)
{
	FortisslvpnEditor *self = FORTISSLVPN_EDITOR (user_data);
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	GtkEditable *trusted_cert_entry = GTK_EDITABLE (gtk_builder_get_object (priv->builder, "trusted_cert_entry"));
	GtkEditable *realm_entry = GTK_EDITABLE (gtk_builder_get_object (priv->builder, "realm_entry"));
	GtkSwitch *use_otp = GTK_SWITCH (gtk_builder_get_object (priv->builder, "use_otp"));

	g_return_if_fail (trusted_cert_entry);
	g_return_if_fail (realm_entry);

	gtk_widget_hide (dialog);
	gtk_window_set_transient_for (GTK_WINDOW (dialog), NULL);

	if (response == GTK_RESPONSE_OK) {
		g_free (priv->trusted_cert);
		priv->trusted_cert = g_strdup (gtk_editable_get_text (trusted_cert_entry));
		priv->realm = g_strdup (gtk_editable_get_text (realm_entry));
		stuff_changed_cb (NULL, self);

		if (gtk_switch_get_active (use_otp))
			priv->otp_flags |= NM_SETTING_SECRET_FLAG_NOT_SAVED;
		else
			priv->otp_flags &= ~NM_SETTING_SECRET_FLAG_NOT_SAVED;
	} else {
		gtk_editable_set_text (trusted_cert_entry, priv->trusted_cert);
		gtk_editable_set_text (realm_entry, priv->realm);
		gtk_switch_set_active (use_otp,
		                       priv->otp_flags & NM_SETTING_SECRET_FLAG_NOT_SAVED);
	}
}

static void
advanced_button_clicked_cb (GtkWidget *button, gpointer user_data)
{
	FortisslvpnEditor *self = FORTISSLVPN_EDITOR (user_data);
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	GtkWidget *dialog = GTK_WIDGET (gtk_builder_get_object (priv->builder, "advanced_dialog"));
	void *root;

	g_assert (dialog);

	root = gtk_widget_get_root (priv->widget);
	if (GTK_IS_WINDOW(root)) {
		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (root));
		if (!priv->window_added) {
			gtk_window_group_add_window (priv->window_group, GTK_WINDOW (root));
			gtk_window_group_add_window (priv->window_group, GTK_WINDOW (dialog));
			priv->window_added = TRUE;
		}
	}

	gtk_widget_show (dialog);
}

static gboolean
init_editor_plugin (FortisslvpnEditor *self, NMConnection *connection, GError **error)
{
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	GtkWidget *widget;
	GtkSizeGroup *group;
	const char *value;

	s_vpn = (NMSettingVpn *) nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	group = GTK_SIZE_GROUP (gtk_builder_get_object (priv->builder, "group"));

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	g_return_val_if_fail (widget, FALSE);

	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_GATEWAY);
		if (value && strlen (value))
			gtk_editable_set_text (GTK_EDITABLE (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_entry"));
	g_return_val_if_fail (widget, FALSE);

	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_USER);
		if (value && strlen (value))
			gtk_editable_set_text (GTK_EDITABLE (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "trusted_cert_entry"));
	g_return_val_if_fail (widget, FALSE);
	if (s_vpn) {
		priv->trusted_cert = g_strdup (nm_setting_vpn_get_data_item (s_vpn,
		                                                             NM_FORTISSLVPN_KEY_TRUSTED_CERT));
		if (!priv->trusted_cert)
			priv->trusted_cert = g_strdup ("");
		gtk_editable_set_text (GTK_EDITABLE (widget), priv->trusted_cert);
	}

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "realm_entry"));
	g_return_val_if_fail (widget, FALSE);
	if (s_vpn) {
		priv->realm = g_strdup (nm_setting_vpn_get_data_item (s_vpn,
		                                                      NM_FORTISSLVPN_KEY_REALM));
		if (!priv->realm)
			priv->realm = g_strdup ("");
		gtk_editable_set_text (GTK_EDITABLE (widget), priv->realm);
	}

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "use_otp"));
	g_return_val_if_fail (widget, FALSE);

	if (s_vpn) {
		nm_setting_get_secret_flags (NM_SETTING (s_vpn),
		                             NM_FORTISSLVPN_KEY_OTP, &priv->otp_flags,
		                             NULL);
		gtk_switch_set_active (GTK_SWITCH (widget),
		                       priv->otp_flags & NM_SETTING_SECRET_FLAG_NOT_SAVED);
	}

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "show_passwords_checkbutton"));
	g_return_val_if_fail (widget != NULL, FALSE);
	g_signal_connect (G_OBJECT (widget), "toggled",
	                  (GCallback) show_toggled_cb,
	                  self);

	/* Fill the VPN passwords *before* initializing the PW type combo, since
	 * knowing if there is a password when initializing the type combo is helpful.
	 */
	setup_password_widget (self,
	                       "user_password_entry",
	                       s_vpn,
	                       NM_FORTISSLVPN_KEY_PASSWORD,
	                       priv->new_connection);

	init_password_icon (self,
	                    s_vpn,
	                    NM_FORTISSLVPN_KEY_PASSWORD,
	                    "user_password_entry");

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "ca_chooser"));
	g_return_val_if_fail (widget, FALSE);

	nma_cert_chooser_add_to_size_group (NMA_CERT_CHOOSER (widget), group);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_CA);
		if (value && strlen (value)) {
			nma_cert_chooser_set_cert (NMA_CERT_CHOOSER (widget), value,
			                           NM_SETTING_802_1X_CK_SCHEME_PATH);
		}
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "cert_chooser"));
	g_return_val_if_fail (widget, FALSE);

	nma_cert_chooser_add_to_size_group (NMA_CERT_CHOOSER (widget), group);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_CERT);
		if (value && strlen (value)) {
			nma_cert_chooser_set_cert (NMA_CERT_CHOOSER (widget), value,
			                           NM_SETTING_802_1X_CK_SCHEME_PATH);
		}
		value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_KEY);
		if (value && strlen (value)) {
			nma_cert_chooser_set_key (NMA_CERT_CHOOSER (widget), value,
			                          NM_SETTING_802_1X_CK_SCHEME_PATH);
		}
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "advanced_dialog"));
	g_return_val_if_fail (widget, FALSE);

	g_signal_connect (G_OBJECT (widget), "response", G_CALLBACK (advanced_dialog_response_cb), self);
	gtk_window_set_hide_on_close (GTK_WINDOW (widget), TRUE);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "advanced_button"));
	g_return_val_if_fail (widget, FALSE);

	g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (advanced_button_clicked_cb), self);

	return TRUE;
}

static GObject *
get_widget (NMVpnEditor *iface)
{
	FortisslvpnEditor *self = FORTISSLVPN_EDITOR (iface);
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);

	return G_OBJECT (priv->widget);
}

static void
save_password_and_flags (NMSettingVpn *s_vpn,
                         GtkBuilder *builder,
                         const char *entry_name,
                         const char *secret_key)
{
	NMSettingSecretFlags flags;
	const char *password;
	GtkWidget *entry;

	/* Get secret flags */
	entry = GTK_WIDGET (gtk_builder_get_object (builder, entry_name));
	flags = nma_utils_menu_to_secret_flags (entry);

	/* Save password and convert flags to legacy data items */
	switch (flags) {
	case NM_SETTING_SECRET_FLAG_NONE:
	case NM_SETTING_SECRET_FLAG_AGENT_OWNED:
		password = gtk_editable_get_text (GTK_EDITABLE (entry));
		if (password && strlen (password))
			nm_setting_vpn_add_secret (s_vpn, secret_key, password);
		break;
	default:
		break;
	}

	/* Set new secret flags */
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), secret_key, flags, NULL);
}

static gboolean
update_connection (NMVpnEditor *iface,
                   NMConnection *connection,
                   GError **error)
{
	FortisslvpnEditor *self = FORTISSLVPN_EDITOR (iface);
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (self);
	NMSettingVpn *s_vpn;
	GtkWidget *widget;
	const char *str;

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_FORTISSLVPN, NULL);

	/* Gateway */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_editable_get_text (GTK_EDITABLE (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_FORTISSLVPN_KEY_GATEWAY, str);

	/* Username */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_entry"));
	str = gtk_editable_get_text (GTK_EDITABLE (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_FORTISSLVPN_KEY_USER, str);

	/* User password and flags */
	save_password_and_flags (s_vpn,
	                         priv->builder,
	                         "user_password_entry",
	                         NM_FORTISSLVPN_KEY_PASSWORD);

	/* Trusted certificate */
	if (priv->trusted_cert && strlen (priv->trusted_cert))
		nm_setting_vpn_add_data_item (s_vpn,
		                              NM_FORTISSLVPN_KEY_TRUSTED_CERT,
		                              priv->trusted_cert);

	/* Realm */
	if (priv->realm && strlen (priv->realm))
		nm_setting_vpn_add_data_item (s_vpn,
		                              NM_FORTISSLVPN_KEY_REALM,
		                              priv->realm);

	/* Use OTP */
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_FORTISSLVPN_KEY_OTP, priv->otp_flags, NULL);

	if (!nm_fortisslvpn_properties_validate (s_vpn, error))
		return FALSE;

	nm_connection_add_setting (connection, NM_SETTING (s_vpn));

	return TRUE;
}

static void
is_new_func (const char *key, const char *value, gpointer user_data)
{
	gboolean *is_new = user_data;

	/* If there are any VPN data items the connection isn't new */
	*is_new = FALSE;
}

NMVpnEditor *
nm_fortisslvpn_editor_new (NMConnection *connection, GError **error)
{
	NMVpnEditor *object;
	FortisslvpnEditorPrivate *priv;
	gboolean new = TRUE;
	NMSettingVpn *s_vpn;

	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	object = g_object_new (FORTISSLVPN_TYPE_EDITOR, NULL);
	if (!object) {
		g_set_error (error, NMV_EDITOR_PLUGIN_ERROR, 0, "could not create fortisslvpn object");
		return NULL;
	}

	priv = FORTISSLVPN_EDITOR_GET_PRIVATE (object);

	priv->builder = gtk_builder_new ();

	gtk_builder_set_translation_domain (priv->builder, GETTEXT_PACKAGE);

	if (!gtk_builder_add_from_resource (priv->builder, "/org/freedesktop/network-manager-fortisslvpn/nm-fortisslvpn-dialog.ui", error)) {
		g_object_unref (object);
		g_return_val_if_reached (NULL);
	}

	priv->widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "fortisslvpn_grid"));
	if (!priv->widget) {
		g_set_error (error, NMV_EDITOR_PLUGIN_ERROR, 0, "could not load UI widget");
		g_object_unref (object);
		return NULL;
	}
	g_object_ref_sink (priv->widget);

	priv->window_group = gtk_window_group_new ();

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn)
		nm_setting_vpn_foreach_data_item (s_vpn, is_new_func, &new);
	priv->new_connection = new;

	if (!init_editor_plugin (FORTISSLVPN_EDITOR (object), connection, error)) {
		g_object_unref (object);
		return NULL;
	}

	return object;
}

static void
dispose (GObject *object)
{
	FortisslvpnEditor *plugin = FORTISSLVPN_EDITOR (object);
	FortisslvpnEditorPrivate *priv = FORTISSLVPN_EDITOR_GET_PRIVATE (plugin);
	GtkWidget *widget;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "user_password_entry"));
	if (widget) {
		g_signal_handlers_disconnect_by_func (G_OBJECT (widget),
						      (GCallback) password_storage_changed_cb,
						      plugin);
	}

	if (priv->window_group)
		g_object_unref (priv->window_group);

	if (priv->widget)
		g_object_unref (priv->widget);

	if (priv->builder)
		g_object_unref (priv->builder);

	G_OBJECT_CLASS (fortisslvpn_editor_parent_class)->dispose (object);
}

static void
fortisslvpn_editor_init (FortisslvpnEditor *plugin)
{
}

static void
fortisslvpn_editor_class_init (FortisslvpnEditorClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	g_type_class_add_private (req_class, sizeof (FortisslvpnEditorPrivate));

	object_class->dispose = dispose;
}

static void
fortisslvpn_editor_interface_init (NMVpnEditorInterface *iface_class)
{
	/* interface implementation */
	iface_class->get_widget = get_widget;
	iface_class->update_connection = update_connection;
}

/*****************************************************************************/

#include "nm-fortisslvpn-editor-plugin.h"

G_MODULE_EXPORT NMVpnEditor *
nm_vpn_editor_factory_fortisslvpn (NMVpnEditorPlugin *editor_plugin,
                                   NMConnection *connection,
                                   GError **error)
{
	g_return_val_if_fail (!error || !*error, NULL);

	g_type_ensure (NMA_TYPE_CERT_CHOOSER);
	return nm_fortisslvpn_editor_new (connection, error);
}
