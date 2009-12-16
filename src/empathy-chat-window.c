/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2003-2007 Imendio AB
 * Copyright (C) 2007-2008 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Mikael Hallendal <micke@imendio.com>
 *          Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Geert-Jan Van den Bogaerde <geertjan@gnome.org>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Rômulo Fernandes Machado <romulo@castorgroup.net>
 */

#include <config.h>

#include <string.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <libnotify/notification.h>

#include <telepathy-glib/telepathy-glib.h>

#include <libempathy/empathy-contact.h>
#include <libempathy/empathy-message.h>
#include <libempathy/empathy-dispatcher.h>
#include <libempathy/empathy-chatroom-manager.h>
#include <libempathy/empathy-utils.h>

#include <libempathy-gtk/empathy-images.h>
#include <libempathy-gtk/empathy-conf.h>
#include <libempathy-gtk/empathy-contact-dialogs.h>
#include <libempathy-gtk/empathy-log-window.h>
#include <libempathy-gtk/empathy-geometry.h>
#include <libempathy-gtk/empathy-smiley-manager.h>
#include <libempathy-gtk/empathy-sound.h>
#include <libempathy-gtk/empathy-ui-utils.h>
#include <libempathy-gtk/empathy-notify-manager.h>

#include <extensions/extensions.h>

#include "empathy-chat-window.h"
#include "empathy-about-dialog.h"
#include "empathy-invite-participant-dialog.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CHAT
#include <libempathy/empathy-debug.h>

typedef struct {
	EmpathyChatWindow *window;
	EmpathyChat *chat;
} NotificationData;

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyChatWindow)
typedef struct {
	EmpathyChat *current_chat;
	GList       *chats;
	GList       *chats_new_msg;
	GList       *chats_composing;
	gboolean     page_added;
	gboolean     dnd_same_window;
	EmpathyChatroomManager *chatroom_manager;
	EmpathyNotifyManager *notify_mgr;
	GtkWidget   *dialog;
	GtkWidget   *notebook;
	NotifyNotification *notification;
	NotificationData *notification_data;

	GtkTargetList *contact_targets;
	GtkTargetList *file_targets;

	/* Menu items. */
	GtkUIManager *ui_manager;
	GtkAction   *menu_conv_insert_smiley;
	GtkAction   *menu_conv_favorite;
	GtkAction   *menu_conv_toggle_contacts;

	GtkAction   *menu_edit_cut;
	GtkAction   *menu_edit_copy;
	GtkAction   *menu_edit_paste;

	GtkAction   *menu_tabs_next;
	GtkAction   *menu_tabs_prev;
	GtkAction   *menu_tabs_left;
	GtkAction   *menu_tabs_right;
	GtkAction   *menu_tabs_detach;
} EmpathyChatWindowPriv;

static GList *chat_windows = NULL;

static const guint tab_accel_keys[] = {
	GDK_1, GDK_2, GDK_3, GDK_4, GDK_5,
	GDK_6, GDK_7, GDK_8, GDK_9, GDK_0
};

typedef enum {
	DND_DRAG_TYPE_CONTACT_ID,
	DND_DRAG_TYPE_URI_LIST,
	DND_DRAG_TYPE_TAB
} DndDragType;

static const GtkTargetEntry drag_types_dest[] = {
	{ "text/contact-id", 0, DND_DRAG_TYPE_CONTACT_ID },
	{ "GTK_NOTEBOOK_TAB", GTK_TARGET_SAME_APP, DND_DRAG_TYPE_TAB },
	{ "text/uri-list", 0, DND_DRAG_TYPE_URI_LIST },
};

static const GtkTargetEntry drag_types_dest_contact[] = {
	{ "text/contact-id", 0, DND_DRAG_TYPE_CONTACT_ID },
};

static const GtkTargetEntry drag_types_dest_file[] = {
	{ "text/uri-list", 0, DND_DRAG_TYPE_URI_LIST },
};

static void chat_window_update (EmpathyChatWindow *window);

G_DEFINE_TYPE (EmpathyChatWindow, empathy_chat_window, G_TYPE_OBJECT);

static void
chat_window_accel_cb (GtkAccelGroup    *accelgroup,
		      GObject          *object,
		      guint             key,
		      GdkModifierType   mod,
		      EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	gint                  num = -1;
	guint                 i;

	priv = GET_PRIV (window);

	for (i = 0; i < G_N_ELEMENTS (tab_accel_keys); i++) {
		if (tab_accel_keys[i] == key) {
			num = i;
			break;
		}
	}

	if (num != -1) {
		gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook), num);
	}
}

static EmpathyChatWindow *
chat_window_find_chat (EmpathyChat *chat)
{
	EmpathyChatWindowPriv *priv;
	GList                 *l, *ll;

	for (l = chat_windows; l; l = l->next) {
		priv = GET_PRIV (l->data);
		ll = g_list_find (priv->chats, chat);
		if (ll) {
			return l->data;
		}
	}

	return NULL;
}

static void
chat_window_close_clicked_cb (GtkAction   *action,
			      EmpathyChat *chat)
{
	EmpathyChatWindow *window;

	window = chat_window_find_chat (chat);
	empathy_chat_window_remove_chat (window, chat);
}

static void
chat_tab_style_set_cb (GtkWidget *hbox,
				       GtkStyle  *previous_style,
				       gpointer   user_data)
{
	GtkWidget *button;
	int char_width, h, w;
	PangoContext *context;
	PangoFontMetrics *metrics;

	button = g_object_get_data (G_OBJECT (user_data),
		"chat-window-tab-close-button");
	context = gtk_widget_get_pango_context (hbox);

	metrics = pango_context_get_metrics (context, gtk_widget_get_style (hbox)->font_desc,
		pango_context_get_language (context));
	char_width = pango_font_metrics_get_approximate_char_width (metrics);
	pango_font_metrics_unref (metrics);

	gtk_icon_size_lookup_for_settings (gtk_widget_get_settings (button),
					   GTK_ICON_SIZE_MENU, &w, &h);

	/* Request at least about 12 chars width plus at least space for the status
	 * image and the close button */
	gtk_widget_set_size_request (hbox,
		12 * PANGO_PIXELS (char_width) + 2 * w, -1);

	gtk_widget_set_size_request (button, w, h);
}

static GtkWidget *
chat_window_create_label (EmpathyChatWindow *window,
			  EmpathyChat       *chat,
			  gboolean           is_tab_label)
{
	EmpathyChatWindowPriv *priv;
	GtkWidget            *hbox;
	GtkWidget            *name_label;
	GtkWidget            *status_image;
	GtkWidget            *close_button;
	GtkWidget            *close_image;
	GtkWidget            *event_box;
	GtkWidget            *event_box_hbox;
	PangoAttrList        *attr_list;
	PangoAttribute       *attr;

	priv = GET_PRIV (window);

	/* The spacing between the button and the label. */
	hbox = gtk_hbox_new (FALSE, 0);

	event_box = gtk_event_box_new ();
	gtk_event_box_set_visible_window (GTK_EVENT_BOX (event_box), FALSE);

	name_label = gtk_label_new (NULL);
	if (is_tab_label)
		gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);

	attr_list = pango_attr_list_new ();
	attr = pango_attr_scale_new (1/1.2);
	attr->start_index = 0;
	attr->end_index = -1;
	pango_attr_list_insert (attr_list, attr);
	gtk_label_set_attributes (GTK_LABEL (name_label), attr_list);
	pango_attr_list_unref (attr_list);

	gtk_misc_set_padding (GTK_MISC (name_label), 2, 0);
	gtk_misc_set_alignment (GTK_MISC (name_label), 0.0, 0.5);
	g_object_set_data (G_OBJECT (chat),
		is_tab_label ? "chat-window-tab-label" : "chat-window-menu-label",
		name_label);

	status_image = gtk_image_new ();

	/* Spacing between the icon and label. */
	event_box_hbox = gtk_hbox_new (FALSE, 0);

	gtk_box_pack_start (GTK_BOX (event_box_hbox), status_image, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (event_box_hbox), name_label, TRUE, TRUE, 0);

	g_object_set_data (G_OBJECT (chat),
		is_tab_label ? "chat-window-tab-image" : "chat-window-menu-image",
		status_image);
	g_object_set_data (G_OBJECT (chat),
		is_tab_label ? "chat-window-tab-tooltip-widget" : "chat-window-menu-tooltip-widget",
		event_box);

	gtk_container_add (GTK_CONTAINER (event_box), event_box_hbox);
	gtk_box_pack_start (GTK_BOX (hbox), event_box, TRUE, TRUE, 0);

	if (is_tab_label) {
		close_button = gtk_button_new ();
		gtk_button_set_relief (GTK_BUTTON (close_button), GTK_RELIEF_NONE);
		g_object_set_data (G_OBJECT (chat), "chat-window-tab-close-button", close_button);

		/* We don't want focus/keynav for the button to avoid clutter, and
		 * Ctrl-W works anyway.
		 */
		gtk_widget_set_can_focus (close_button, FALSE);
		gtk_widget_set_can_default (close_button, FALSE);

		/* Set the name to make the special rc style match. */
		gtk_widget_set_name (close_button, "empathy-close-button");

		close_image = gtk_image_new_from_stock (GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU);

		gtk_container_add (GTK_CONTAINER (close_button), close_image);

		gtk_box_pack_end (GTK_BOX (hbox), close_button, FALSE, FALSE, 0);

		g_signal_connect (close_button,
				  "clicked",
				  G_CALLBACK (chat_window_close_clicked_cb),
				  chat);

		/* React to theme changes and also setup the size correctly.  */
		g_signal_connect (hbox,
				  "style-set",
				  G_CALLBACK (chat_tab_style_set_cb),
				  chat);
	}

	gtk_widget_show_all (hbox);

	return hbox;
}

static void
_submenu_notify_visible_changed_cb (GObject    *object,
				    GParamSpec *pspec,
				    gpointer    userdata)
{
	g_signal_handlers_disconnect_by_func (object,
					      _submenu_notify_visible_changed_cb,
					      userdata);
	chat_window_update (EMPATHY_CHAT_WINDOW (userdata));
}

static void
chat_window_menu_context_update (EmpathyChatWindowPriv *priv,
			      gint num_pages)
{
	gboolean first_page;
	gboolean last_page;
	gboolean is_connected;
	gint     page_num;

	page_num = gtk_notebook_get_current_page (GTK_NOTEBOOK (priv->notebook));
	first_page = (page_num == 0);
	last_page = (page_num == (num_pages - 1));
	is_connected = empathy_chat_get_tp_chat (priv->current_chat) != NULL;

	DEBUG ("Update window : Menu Contexts (Tabs & Conv)");

	gtk_action_set_sensitive (priv->menu_tabs_next, TRUE);
	gtk_action_set_sensitive (priv->menu_tabs_prev, TRUE);
	gtk_action_set_sensitive (priv->menu_tabs_detach, num_pages > 1);
	gtk_action_set_sensitive (priv->menu_tabs_left, !first_page);
	gtk_action_set_sensitive (priv->menu_tabs_right, !last_page);
	gtk_action_set_sensitive (priv->menu_conv_insert_smiley, is_connected);
}

static void
chat_window_contact_menu_update (EmpathyChatWindowPriv *priv,
				 EmpathyChatWindow     *window)
{
	GtkWidget *menu, *submenu, *orig_submenu;

	menu = gtk_ui_manager_get_widget (priv->ui_manager,
		"/chats_menubar/menu_contact");
	orig_submenu = gtk_menu_item_get_submenu (GTK_MENU_ITEM (menu));

	DEBUG ("Update window : Contact Menu");

	if (orig_submenu == NULL || !GTK_WIDGET_VISIBLE (orig_submenu)) {
		submenu = empathy_chat_get_contact_menu (priv->current_chat);
		gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu), submenu);
		gtk_widget_show (menu);
	} else {
		empathy_signal_connect_weak (orig_submenu,
					     "notify::visible",
					     (GCallback)_submenu_notify_visible_changed_cb,
					     G_OBJECT (window));
	}
}

static guint
get_all_unread_messages (EmpathyChatWindowPriv *priv)
{
	GList *l;
	guint nb = 0;

	for (l = priv->chats_new_msg; l != NULL; l = g_list_next (l)) {
		EmpathyChat *chat = l->data;

		nb += empathy_chat_get_nb_unread_messages (chat);
	}

	return nb;
}

static gchar *
get_window_title_name (EmpathyChatWindowPriv *priv)
{
	const gchar *active_name;
	guint nb_chats;
	guint current_unread_msgs;

	nb_chats = g_list_length (priv->chats);
	g_assert (nb_chats > 0);

	active_name = empathy_chat_get_name (priv->current_chat);

	current_unread_msgs = empathy_chat_get_nb_unread_messages (
			priv->current_chat);

	if (nb_chats == 1) {
		/* only one tab */
		if (current_unread_msgs == 0)
			return g_strdup (active_name);
		else
			return g_strdup_printf (ngettext (
				"%s (%d unread)",
				"%s (%d unread)", current_unread_msgs),
				active_name, current_unread_msgs);
	} else {
		guint nb_others = nb_chats - 1;
		guint all_unread_msgs;

		all_unread_msgs = get_all_unread_messages (priv);

		if (all_unread_msgs == 0) {
			/* no unread message */
			return g_strdup_printf (ngettext (
				"%s (and %u other)",
				"%s (and %u others)", nb_others),
				active_name, nb_others);
		}

		else if (all_unread_msgs == current_unread_msgs) {
			/* unread messages are in the current tab */
			return g_strdup_printf (ngettext (
				"%s (%d unread)",
				"%s (%d unread)", current_unread_msgs),
				active_name, current_unread_msgs);
		}

		else if (current_unread_msgs == 0) {
			/* unread messages are in other tabs */
			return g_strdup_printf (ngettext (
				"%s (%d unread from others)",
				"%s (%d unread from others)",
				all_unread_msgs),
				active_name, all_unread_msgs);
		}

		else {
			/* unread messages are in all the tabs */
			return g_strdup_printf (ngettext (
				"%s (%d unread from all)",
				"%s (%d unread from all)",
				all_unread_msgs),
				active_name, all_unread_msgs);
		}
	}
}

static void
chat_window_title_update (EmpathyChatWindowPriv *priv)
{
	gchar *name;

	DEBUG ("Update window : Title");

	name = get_window_title_name (priv);
	gtk_window_set_title (GTK_WINDOW (priv->dialog), name);
	g_free (name);
}

static void
chat_window_icon_update (EmpathyChatWindowPriv *priv)
{
	GdkPixbuf      *icon;
	EmpathyContact *remote_contact;
	gboolean        avatar_in_icon;
	guint           n_chats;

	n_chats = g_list_length (priv->chats);

	DEBUG ("Update window : Icon");

	/* Update window icon */
	if (priv->chats_new_msg) {
		gtk_window_set_icon_name (GTK_WINDOW (priv->dialog),
					  EMPATHY_IMAGE_MESSAGE);
	} else {
		empathy_conf_get_bool (empathy_conf_get (),
				       EMPATHY_PREFS_CHAT_AVATAR_IN_ICON,
				       &avatar_in_icon);

		if (n_chats == 1 && avatar_in_icon) {
			remote_contact = empathy_chat_get_remote_contact (priv->current_chat);
			icon = empathy_pixbuf_avatar_from_contact_scaled (remote_contact, 0, 0);
			gtk_window_set_icon (GTK_WINDOW (priv->dialog), icon);

			if (icon != NULL) {
				g_object_unref (icon);
			}
		} else {
			gtk_window_set_icon_name (GTK_WINDOW (priv->dialog), NULL);
		}
	}
}

static void
chat_window_close_button_update (EmpathyChatWindowPriv *priv,
				 gint num_pages)
{
	GtkWidget *chat;
	GtkWidget *chat_close_button;
	gint       i;

	DEBUG ("Update window : Close Button");

	if (num_pages == 1) {
		chat = gtk_notebook_get_nth_page (GTK_NOTEBOOK (priv->notebook), 0);
		chat_close_button = g_object_get_data (G_OBJECT (chat),
				"chat-window-tab-close-button");
		gtk_widget_hide (chat_close_button);
	} else {
		for (i=0; i<num_pages; i++) {
			chat = gtk_notebook_get_nth_page (GTK_NOTEBOOK (priv->notebook), i);
			chat_close_button = g_object_get_data (G_OBJECT (chat),
					"chat-window-tab-close-button");
			gtk_widget_show (chat_close_button);
		}
	}
}

static void
chat_window_update (EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	gint                   num_pages;

	num_pages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook));

	DEBUG ("Update window");

	/* Update Tab menu */
	chat_window_menu_context_update (priv,
					 num_pages);

	chat_window_contact_menu_update (priv,
					 window);

	chat_window_title_update (priv);

	chat_window_icon_update (priv);

	chat_window_close_button_update (priv,
					 num_pages);
}

static void
append_markup_printf (GString    *string,
		      const char *format,
		      ...)
{
	gchar *tmp;
	va_list args;

	va_start (args, format);

	tmp = g_markup_vprintf_escaped (format, args);
	g_string_append (string, tmp);
	g_free (tmp);

	va_end (args);
}

static void
chat_window_update_chat_tab (EmpathyChat *chat)
{
	EmpathyChatWindow     *window;
	EmpathyChatWindowPriv *priv;
	EmpathyContact        *remote_contact;
	const gchar           *name;
	const gchar           *id;
	TpAccount             *account;
	const gchar           *subject;
	const gchar           *status = NULL;
	GtkWidget             *widget;
	GString               *tooltip;
	gchar                 *markup;
	const gchar           *icon_name;
	GtkWidget             *tab_image;
	GtkWidget             *menu_image;

	window = chat_window_find_chat (chat);
	if (!window) {
		return;
	}
	priv = GET_PRIV (window);

	/* Get information */
	name = empathy_chat_get_name (chat);
	account = empathy_chat_get_account (chat);
	subject = empathy_chat_get_subject (chat);
	remote_contact = empathy_chat_get_remote_contact (chat);

	DEBUG ("Updating chat tab, name=%s, account=%s, subject=%s, remote_contact=%p",
		name, tp_proxy_get_object_path (account), subject, remote_contact);

	/* Update tab image */
	if (empathy_chat_get_tp_chat (chat) == NULL) {
		/* No TpChat, we are disconnected */
		icon_name = NULL;
	}
	else if (g_list_find (priv->chats_new_msg, chat)) {
		icon_name = EMPATHY_IMAGE_MESSAGE;
	}
	else if (g_list_find (priv->chats_composing, chat)) {
		icon_name = EMPATHY_IMAGE_TYPING;
	}
	else if (remote_contact) {
		icon_name = empathy_icon_name_for_contact (remote_contact);
	} else {
		icon_name = EMPATHY_IMAGE_GROUP_MESSAGE;
	}

	tab_image = g_object_get_data (G_OBJECT (chat), "chat-window-tab-image");
	menu_image = g_object_get_data (G_OBJECT (chat), "chat-window-menu-image");
	if (icon_name != NULL) {
		gtk_image_set_from_icon_name (GTK_IMAGE (tab_image), icon_name, GTK_ICON_SIZE_MENU);
		gtk_widget_show (tab_image);
		gtk_image_set_from_icon_name (GTK_IMAGE (menu_image), icon_name, GTK_ICON_SIZE_MENU);
		gtk_widget_show (menu_image);
	} else {
		gtk_widget_hide (tab_image);
		gtk_widget_hide (menu_image);
	}

	/* Update tab tooltip */
	tooltip = g_string_new (NULL);

	if (remote_contact) {
		id = empathy_contact_get_id (remote_contact);
		status = empathy_contact_get_presence_message (remote_contact);
	} else {
		id = name;
	}

	append_markup_printf (tooltip,
			      "<b>%s</b><small> (%s)</small>",
			      id,
			      tp_account_get_display_name (account));

	if (!EMP_STR_EMPTY (status)) {
		append_markup_printf (tooltip, "\n<i>%s</i>", status);
	}

	if (subject) {
		append_markup_printf (tooltip, "\n<b>%s</b> %s",
				      _("Topic:"), subject);
	}

	if (g_list_find (priv->chats_composing, chat)) {
		append_markup_printf (tooltip, "\n%s", _("Typing a message."));
	}

	markup = g_string_free (tooltip, FALSE);
	widget = g_object_get_data (G_OBJECT (chat), "chat-window-tab-tooltip-widget");
	gtk_widget_set_tooltip_markup (widget, markup);
	widget = g_object_get_data (G_OBJECT (chat), "chat-window-menu-tooltip-widget");
	gtk_widget_set_tooltip_markup (widget, markup);
	g_free (markup);

	/* Update tab and menu label */
	widget = g_object_get_data (G_OBJECT (chat), "chat-window-tab-label");
	gtk_label_set_text (GTK_LABEL (widget), name);
	widget = g_object_get_data (G_OBJECT (chat), "chat-window-menu-label");
	gtk_label_set_text (GTK_LABEL (widget), name);

	/* Update the window if it's the current chat */
	if (priv->current_chat == chat) {
		chat_window_update (window);
	}
}

static void
chat_window_chat_notify_cb (EmpathyChat *chat)
{
	EmpathyContact *old_remote_contact;
	EmpathyContact *remote_contact = NULL;

	old_remote_contact = g_object_get_data (G_OBJECT (chat), "chat-window-remote-contact");
	remote_contact = empathy_chat_get_remote_contact (chat);

	if (old_remote_contact != remote_contact) {
		/* The remote-contact associated with the chat changed, we need
		 * to keep track of any change of that contact and update the
		 * window each time. */
		if (remote_contact) {
			g_signal_connect_swapped (remote_contact, "notify",
						  G_CALLBACK (chat_window_update_chat_tab),
						  chat);
		}
		if (old_remote_contact) {
			g_signal_handlers_disconnect_by_func (old_remote_contact,
							      chat_window_update_chat_tab,
							      chat);
		}

		g_object_set_data_full (G_OBJECT (chat), "chat-window-remote-contact",
				   g_object_ref (remote_contact), (GDestroyNotify) g_object_unref);
	}

	chat_window_update_chat_tab (chat);
}

static void
chat_window_insert_smiley_activate_cb (EmpathySmileyManager *manager,
				       EmpathySmiley        *smiley,
				       gpointer              window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	EmpathyChat           *chat;
	GtkTextBuffer         *buffer;
	GtkTextIter            iter;

	chat = priv->current_chat;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chat->input_text_view));
	gtk_text_buffer_get_end_iter (buffer, &iter);
	gtk_text_buffer_insert (buffer, &iter, smiley->str, -1);
}

static void
chat_window_conv_activate_cb (GtkAction         *action,
			      EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	gboolean               is_room;
	gboolean               active;
	EmpathyContact        *remote_contact = NULL;

	/* Favorite room menu */
	is_room = empathy_chat_is_room (priv->current_chat);
	if (is_room) {
		const gchar *room;
		TpAccount   *account;
		gboolean     found = FALSE;
		EmpathyChatroom *chatroom;

		room = empathy_chat_get_id (priv->current_chat);
		account = empathy_chat_get_account (priv->current_chat);
		chatroom = empathy_chatroom_manager_find (priv->chatroom_manager,
						       account, room);
		if (chatroom != NULL)
			found = empathy_chatroom_is_favorite (chatroom);

		DEBUG ("This room %s favorite", found ? "is" : "is not");
		gtk_toggle_action_set_active (
			GTK_TOGGLE_ACTION (priv->menu_conv_favorite), found);
	}
	gtk_action_set_visible (priv->menu_conv_favorite, is_room);

	/* Show contacts menu */
	g_object_get (priv->current_chat,
		      "remote-contact", &remote_contact,
		      "show-contacts", &active,
		      NULL);
	if (remote_contact == NULL) {
		gtk_toggle_action_set_active (
			GTK_TOGGLE_ACTION (priv->menu_conv_toggle_contacts),
					   active);
	}
	gtk_action_set_visible (priv->menu_conv_toggle_contacts,
				(remote_contact == NULL));
	if (remote_contact != NULL) {
		g_object_unref (remote_contact);
	}
}

static void
chat_window_clear_activate_cb (GtkAction         *action,
			       EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);

	empathy_chat_clear (priv->current_chat);
}

static void
chat_window_favorite_toggled_cb (GtkToggleAction   *toggle_action,
				 EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	gboolean               active;
	TpAccount             *account;
	const gchar           *room;
	EmpathyChatroom       *chatroom;

	active = gtk_toggle_action_get_active (toggle_action);
	account = empathy_chat_get_account (priv->current_chat);
	room = empathy_chat_get_id (priv->current_chat);

	chatroom = empathy_chatroom_manager_find (priv->chatroom_manager,
						  account, room);

	if (chatroom == NULL) {
		const gchar *name;

		name = empathy_chat_get_name (priv->current_chat);
		chatroom = empathy_chatroom_new_full (account, room, name, FALSE);
		empathy_chatroom_manager_add (priv->chatroom_manager, chatroom);
		g_object_unref (chatroom);
	}

	empathy_chatroom_set_favorite (chatroom, active);
}

static void
chat_window_contacts_toggled_cb (GtkToggleAction   *toggle_action,
				 EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	gboolean               active;

	active = gtk_toggle_action_get_active (toggle_action);

	empathy_chat_set_show_contacts (priv->current_chat, active);
}

static void
chat_window_upgrade_to_muc (EmpathyChat    *chat,
                            EmpathyContact *invitee)
{
	EmpathyDispatcher     *dispatcher = empathy_dispatcher_dup_singleton ();
	EmpathyTpChat         *tp_chat;
	TpConnection          *connection;
	TpChannel             *channel;
	GHashTable            *props;
	GPtrArray             *channels;
	char                  *invitees[3] = { NULL, };

	tp_chat = empathy_chat_get_tp_chat (chat);
	connection = empathy_tp_chat_get_connection (tp_chat);
	channel = empathy_tp_chat_get_channel (tp_chat);

	/* Ensure a MUC channel */
	channels = g_ptr_array_sized_new (1);
	g_ptr_array_add (channels, (char *) tp_proxy_get_object_path (channel));

	invitees[0] = (char *) tp_channel_get_identifier (channel);
	invitees[1] = (char *) empathy_contact_get_id (invitee);

	props = tp_asv_new (
	    TP_IFACE_CHANNEL ".ChannelType", G_TYPE_STRING,
	        TP_IFACE_CHANNEL_TYPE_TEXT,
	    TP_IFACE_CHANNEL ".TargetHandleType", G_TYPE_UINT,
	        TP_HANDLE_TYPE_NONE,
	    EMP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialChannels",
	        TP_ARRAY_TYPE_OBJECT_PATH_LIST, channels,
	    EMP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeIDs",
	        G_TYPE_STRV, invitees,
	    /* FIXME: InvitationMessage ? */
	    NULL);

	/* Although this is a MUC, it's anonymous, so CreateChannel is valid */
	/* props now belongs to EmpathyDispatcher, don't free it */
	empathy_dispatcher_create_channel (dispatcher, connection,
			props, NULL, NULL);

	g_ptr_array_free (channels, TRUE);

	g_object_unref (dispatcher);
}

static void
chat_window_invite_participant_activate_cb (GtkAction         *action,
					    EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	GtkWidget             *dialog;
	EmpathyTpChat         *tp_chat;
	TpChannel             *channel;
	int                    response;

	priv = GET_PRIV (window);

	g_return_if_fail (priv->current_chat != NULL);

	tp_chat = empathy_chat_get_tp_chat (priv->current_chat);
	channel = empathy_tp_chat_get_channel (tp_chat);

	/* FIXME: should filter out the existing participants from the
	 * list */
	dialog = empathy_invite_participant_dialog_new (
			GTK_WINDOW (priv->dialog));
	gtk_widget_show (dialog);

	response = gtk_dialog_run (GTK_DIALOG (dialog));

	if (response == GTK_RESPONSE_OK)
	{
		EmpathyContact *contact;
		TpHandleType handle_type;

		contact = empathy_invite_participant_dialog_dup_selected_contact (EMPATHY_INVITE_PARTICIPANT_DIALOG (dialog));
		tp_channel_get_handle (channel, &handle_type);

		if (handle_type == TP_HANDLE_TYPE_CONTACT)
		{
			chat_window_upgrade_to_muc (priv->current_chat,
					contact);
		}
		else
		{
			TpHandle handle = empathy_contact_get_handle (contact);
			GArray handles = {(gchar *) &handle, 1};

			tp_cli_channel_interface_group_call_add_members (
					channel, -1, &handles,
					_("Inviting to this room"),
					NULL, NULL, NULL, NULL);
		}

		g_object_unref (contact);
	}

	gtk_widget_destroy (dialog);
}

static void
chat_window_close_activate_cb (GtkAction         *action,
			       EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	priv = GET_PRIV (window);

	g_return_if_fail (priv->current_chat != NULL);

	empathy_chat_window_remove_chat (window, priv->current_chat);
}

static void
chat_window_edit_activate_cb (GtkAction         *action,
			      EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	GtkClipboard         *clipboard;
	GtkTextBuffer        *buffer;
	gboolean              text_available;

	priv = GET_PRIV (window);

	g_return_if_fail (priv->current_chat != NULL);

	if (!empathy_chat_get_tp_chat (priv->current_chat)) {
		gtk_action_set_sensitive (priv->menu_edit_copy, FALSE);
		gtk_action_set_sensitive (priv->menu_edit_cut, FALSE);
		gtk_action_set_sensitive (priv->menu_edit_paste, FALSE);
		return;
	}

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (priv->current_chat->input_text_view));
	if (gtk_text_buffer_get_has_selection (buffer)) {
		gtk_action_set_sensitive (priv->menu_edit_copy, TRUE);
		gtk_action_set_sensitive (priv->menu_edit_cut, TRUE);
	} else {
		gboolean selection;

		selection = empathy_chat_view_get_has_selection (priv->current_chat->view);

		gtk_action_set_sensitive (priv->menu_edit_cut, FALSE);
		gtk_action_set_sensitive (priv->menu_edit_copy, selection);
	}

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	text_available = gtk_clipboard_wait_is_text_available (clipboard);
	gtk_action_set_sensitive (priv->menu_edit_paste, text_available);
}

static void
chat_window_cut_activate_cb (GtkAction         *action,
			     EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT_WINDOW (window));

	priv = GET_PRIV (window);

	empathy_chat_cut (priv->current_chat);
}

static void
chat_window_copy_activate_cb (GtkAction         *action,
			      EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT_WINDOW (window));

	priv = GET_PRIV (window);

	empathy_chat_copy (priv->current_chat);
}

static void
chat_window_paste_activate_cb (GtkAction         *action,
			       EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT_WINDOW (window));

	priv = GET_PRIV (window);

	empathy_chat_paste (priv->current_chat);
}

static void
chat_window_tabs_next_activate_cb (GtkAction         *action,
				   EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;
	gint                  index_, numPages;

	priv = GET_PRIV (window);

	chat = priv->current_chat;
	index_ = gtk_notebook_get_current_page (GTK_NOTEBOOK (priv->notebook));
	numPages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook));

	if (index_ == (numPages - 1)) {
		gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook), 0);
		return;
	}

	gtk_notebook_next_page (GTK_NOTEBOOK (priv->notebook));
}

static void
chat_window_tabs_previous_activate_cb (GtkAction         *action,
				   EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;
	gint                  index_, numPages;

	priv = GET_PRIV (window);

	chat = priv->current_chat;
	index_ = gtk_notebook_get_current_page (GTK_NOTEBOOK (priv->notebook));
	numPages = gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->notebook));

	if (index_ <= 0) {
		gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook), numPages - 1);
		return;
	}

	gtk_notebook_prev_page (GTK_NOTEBOOK (priv->notebook));
}

static void
chat_window_tabs_left_activate_cb (GtkAction         *action,
				   EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;
	gint                  index_;

	priv = GET_PRIV (window);

	chat = priv->current_chat;
	index_ = gtk_notebook_get_current_page (GTK_NOTEBOOK (priv->notebook));
	if (index_ <= 0) {
		return;
	}

	gtk_notebook_reorder_child (GTK_NOTEBOOK (priv->notebook),
				    GTK_WIDGET (chat),
				    index_ - 1);
}

static void
chat_window_tabs_right_activate_cb (GtkAction         *action,
				    EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;
	gint                  index_;

	priv = GET_PRIV (window);

	chat = priv->current_chat;
	index_ = gtk_notebook_get_current_page (GTK_NOTEBOOK (priv->notebook));

	gtk_notebook_reorder_child (GTK_NOTEBOOK (priv->notebook),
				    GTK_WIDGET (chat),
				    index_ + 1);
}

static void
chat_window_detach_activate_cb (GtkAction         *action,
				EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChatWindow     *new_window;
	EmpathyChat           *chat;

	priv = GET_PRIV (window);

	chat = priv->current_chat;
	new_window = empathy_chat_window_new ();

	empathy_chat_window_move_chat (window, new_window, chat);

	priv = GET_PRIV (new_window);
	gtk_widget_show (priv->dialog);
}

static void
chat_window_help_contents_activate_cb (GtkAction         *action,
				       EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);

	empathy_url_show (priv->dialog, "ghelp:empathy");
}

static void
chat_window_help_about_activate_cb (GtkAction         *action,
				    EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);

	empathy_about_dialog_new (GTK_WINDOW (priv->dialog));
}

static gboolean
chat_window_delete_event_cb (GtkWidget        *dialog,
			     GdkEvent         *event,
			     EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (window);

	DEBUG ("Delete event received");

	g_object_ref (window);
	while (priv->chats) {
		empathy_chat_window_remove_chat (window, priv->chats->data);
	}
	g_object_unref (window);

	return TRUE;
}

static void
chat_window_composing_cb (EmpathyChat       *chat,
			  gboolean          is_composing,
			  EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	priv = GET_PRIV (window);

	if (is_composing && !g_list_find (priv->chats_composing, chat)) {
		priv->chats_composing = g_list_prepend (priv->chats_composing, chat);
	} else {
		priv->chats_composing = g_list_remove (priv->chats_composing, chat);
	}

	chat_window_update_chat_tab (chat);
}

static void
chat_window_set_urgency_hint (EmpathyChatWindow *window,
			      gboolean          urgent)
{
	EmpathyChatWindowPriv *priv;

	priv = GET_PRIV (window);

	DEBUG ("Turning %s urgency hint", urgent ? "on" : "off");
	gtk_window_set_urgency_hint (GTK_WINDOW (priv->dialog), urgent);
}

static void
free_notification_data (NotificationData *data)
{
	g_object_unref (data->chat);
	g_slice_free (NotificationData, data);
}

static void
chat_window_notification_closed_cb (NotifyNotification *notify,
				    NotificationData *cb_data)
{
	EmpathyNotificationClosedReason reason = 0;
	EmpathyChatWindowPriv *priv = GET_PRIV (cb_data->window);

#ifdef notify_notification_get_closed_reason
	reason = notify_notification_get_closed_reason (notify);
#endif
	if (reason == EMPATHY_NOTIFICATION_CLOSED_DISMISSED) {
		empathy_chat_window_present_chat (cb_data->chat);
	}

	g_object_unref (notify);
	priv->notification = NULL;
	free_notification_data (cb_data);
	priv->notification_data = NULL;
}

static void
chat_window_show_or_update_notification (EmpathyChatWindow *window,
					 EmpathyMessage *message,
					 EmpathyChat *chat)
{
	EmpathyContact *sender;
	const gchar *header;
	char *escaped;
	const char *body;
	GdkPixbuf *pixbuf;
	EmpathyChatWindowPriv *priv = GET_PRIV (window);
	gboolean res;

	if (!empathy_notify_manager_notification_is_enabled (priv->notify_mgr)) {
		return;
	} else {
		empathy_conf_get_bool (empathy_conf_get (),
				       EMPATHY_PREFS_NOTIFICATIONS_FOCUS, &res);
		if (!res) {
			return;
		}
	}

	sender = empathy_message_get_sender (message);
	header = empathy_contact_get_name (sender);
	body = empathy_message_get_body (message);
	escaped = g_markup_escape_text (body, -1);

	if (priv->notification != NULL) {
		notify_notification_update (priv->notification,
					    header, escaped, NULL);
	} else {
		NotificationData *cb_data = cb_data = g_slice_new0 (NotificationData);

		cb_data->chat = g_object_ref (chat);
		cb_data->window = window;

		priv->notification_data = cb_data;
		priv->notification = notify_notification_new (header, escaped, NULL, NULL);
		notify_notification_set_timeout (priv->notification, NOTIFY_EXPIRES_DEFAULT);

		g_signal_connect (priv->notification, "closed",
				  G_CALLBACK (chat_window_notification_closed_cb), cb_data);
	}

	pixbuf = empathy_notify_manager_get_pixbuf_for_notification (priv->notify_mgr,
		sender, EMPATHY_IMAGE_NEW_MESSAGE);

	if (pixbuf != NULL) {
		notify_notification_set_icon_from_pixbuf (priv->notification, pixbuf);
		g_object_unref (pixbuf);
	}

	notify_notification_show (priv->notification, NULL);

	g_free (escaped);
}

static void
chat_window_set_highlight_room_tab_label (EmpathyChat *chat)
{
	gchar *markup;
	GtkWidget *widget;

	if (!empathy_chat_is_room (chat))
		return;

	markup = g_markup_printf_escaped (
		"<span color=\"red\" weight=\"bold\">%s</span>",
		empathy_chat_get_name (chat));

	widget = g_object_get_data (G_OBJECT (chat), "chat-window-tab-label");
	gtk_label_set_markup (GTK_LABEL (widget), markup);
	g_free (markup);
}

static void
chat_window_new_message_cb (EmpathyChat       *chat,
			    EmpathyMessage    *message,
			    EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	gboolean              has_focus;
	gboolean              needs_urgency;
	EmpathyContact        *sender;

	priv = GET_PRIV (window);

	has_focus = empathy_chat_window_has_focus (window);

	/* - if we're the sender, we play the sound if it's specified in the
	 *   preferences and we're not away.
	 * - if we receive a message, we play the sound if it's specified in the
	 *   preferences and the window does not have focus on the chat receiving
	 *   the message.
	 */

	sender = empathy_message_get_sender (message);

	if (empathy_contact_is_user (sender)) {
		empathy_sound_play (GTK_WIDGET (priv->dialog),
				    EMPATHY_SOUND_MESSAGE_OUTGOING);
	}

	if (has_focus && priv->current_chat == chat) {
		/* window and tab are focused so consider the message to be read */
		empathy_chat_messages_read (chat);
		return;
	}

	if (!g_list_find (priv->chats_new_msg, chat)) {
		priv->chats_new_msg = g_list_prepend (priv->chats_new_msg, chat);
		chat_window_update_chat_tab (chat);
	}

	/* If empathy_chat_is_room () returns TRUE, that means it's a named MUC.
	 * If empathy_chat_get_remote_contact () returns NULL, that means it's
	 * an unamed MUC (msn-like).
	 * In case of a MUC, we set urgency only if the message contains our
	 * alias. */
	if (empathy_chat_is_room (chat) ||
	    empathy_chat_get_remote_contact (chat) == NULL) {
		needs_urgency = empathy_message_should_highlight (message);
	} else {
		needs_urgency = TRUE;
	}

	if (needs_urgency) {
		if (!has_focus) {
			chat_window_set_urgency_hint (window, TRUE);
			chat_window_set_highlight_room_tab_label (chat);
		}

		empathy_sound_play (GTK_WIDGET (priv->dialog),
		    EMPATHY_SOUND_MESSAGE_INCOMING);
		chat_window_show_or_update_notification (window, message, chat);
	}

	/* update the number of unread messages */
	chat_window_title_update (priv);
}

static GtkNotebook *
chat_window_detach_hook (GtkNotebook *source,
			 GtkWidget   *page,
			 gint         x,
			 gint         y,
			 gpointer     user_data)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChatWindow     *window, *new_window;
	EmpathyChat           *chat;

	chat = EMPATHY_CHAT (page);
	window = chat_window_find_chat (chat);

	new_window = empathy_chat_window_new ();
	priv = GET_PRIV (new_window);

	DEBUG ("Detach hook called");

	empathy_chat_window_move_chat (window, new_window, chat);

	gtk_window_move (GTK_WINDOW (priv->dialog), x, y);
	gtk_widget_show (priv->dialog);

	return NULL;
}

static void
chat_window_page_switched_cb (GtkNotebook      *notebook,
			      GtkNotebookPage  *page,
			      gint	        page_num,
			      EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;
	GtkWidget            *child;

	DEBUG ("Page switched");

	priv = GET_PRIV (window);

	child = gtk_notebook_get_nth_page (notebook, page_num);
	chat = EMPATHY_CHAT (child);

	if (priv->page_added) {
		priv->page_added = FALSE;
		empathy_chat_scroll_down (chat);
	}
	else if (priv->current_chat == chat) {
		return;
	}

	priv->current_chat = chat;
	priv->chats_new_msg = g_list_remove (priv->chats_new_msg, chat);
	empathy_chat_messages_read (chat);

	chat_window_update_chat_tab (chat);
}

static void
chat_window_page_added_cb (GtkNotebook      *notebook,
			   GtkWidget	    *child,
			   guint             page_num,
			   EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;

	priv = GET_PRIV (window);

	/* If we just received DND to the same window, we don't want
	 * to do anything here like removing the tab and then readding
	 * it, so we return here and in "page-added".
	 */
	if (priv->dnd_same_window) {
		DEBUG ("Page added (back to the same window)");
		priv->dnd_same_window = FALSE;
		return;
	}

	DEBUG ("Page added");

	/* Get chat object */
	chat = EMPATHY_CHAT (child);

	/* Connect chat signals for this window */
	g_signal_connect (chat, "composing",
			  G_CALLBACK (chat_window_composing_cb),
			  window);
	g_signal_connect (chat, "new-message",
			  G_CALLBACK (chat_window_new_message_cb),
			  window);
	g_signal_connect (chat, "notify::tp-chat",
			  G_CALLBACK (chat_window_update_chat_tab),
			  window);

	/* Set flag so we know to perform some special operations on
	 * switch page due to the new page being added.
	 */
	priv->page_added = TRUE;

	/* Get list of chats up to date */
	priv->chats = g_list_append (priv->chats, chat);

	chat_window_update_chat_tab (chat);
}

static void
chat_window_page_removed_cb (GtkNotebook      *notebook,
			     GtkWidget	      *child,
			     guint             page_num,
			     EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	EmpathyChat           *chat;

	priv = GET_PRIV (window);

	/* If we just received DND to the same window, we don't want
	 * to do anything here like removing the tab and then readding
	 * it, so we return here and in "page-added".
	 */
	if (priv->dnd_same_window) {
		DEBUG ("Page removed (and will be readded to same window)");
		return;
	}

	DEBUG ("Page removed");

	/* Get chat object */
	chat = EMPATHY_CHAT (child);

	/* Disconnect all signal handlers for this chat and this window */
	g_signal_handlers_disconnect_by_func (chat,
					      G_CALLBACK (chat_window_composing_cb),
					      window);
	g_signal_handlers_disconnect_by_func (chat,
					      G_CALLBACK (chat_window_new_message_cb),
					      window);
	g_signal_handlers_disconnect_by_func (chat,
					      G_CALLBACK (chat_window_update_chat_tab),
					      window);

	/* Keep list of chats up to date */
	priv->chats = g_list_remove (priv->chats, chat);
	priv->chats_new_msg = g_list_remove (priv->chats_new_msg, chat);
	empathy_chat_messages_read (chat);
	priv->chats_composing = g_list_remove (priv->chats_composing, chat);

	if (priv->chats == NULL) {
		g_object_unref (window);
	} else {
		chat_window_update (window);
	}
}

static gboolean
chat_window_focus_in_event_cb (GtkWidget        *widget,
			       GdkEvent         *event,
			       EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	DEBUG ("Focus in event, updating title");

	priv = GET_PRIV (window);

	priv->chats_new_msg = g_list_remove (priv->chats_new_msg, priv->current_chat);
	empathy_chat_messages_read (priv->current_chat);

	chat_window_set_urgency_hint (window, FALSE);

	/* Update the title, since we now mark all unread messages as read. */
	chat_window_update_chat_tab (priv->current_chat);

	return FALSE;
}

static gboolean
chat_window_drag_motion (GtkWidget        *widget,
			 GdkDragContext   *context,
			 int               x,
			 int               y,
			 guint             time_,
			 EmpathyChatWindow *window)
{
	GdkAtom target;
	EmpathyChatWindowPriv *priv;
	GdkAtom dest_target;

	priv = GET_PRIV (window);

	target = gtk_drag_dest_find_target (widget, context, NULL);

	dest_target = gdk_atom_intern_static_string ("text/uri-list");
	if (target == dest_target) {
		/* This is a file drag.  Ensure the contact is online and set the
		   drag type to COPY.  Note that it's possible that the tab will
		   be switched by GTK+ after a timeout from drag_motion without
		   getting another drag_motion to disable the drop.  You have
		   to hold your mouse really still.
		 */
		EmpathyContact *contact;

		priv = GET_PRIV (window);
		contact = empathy_chat_get_remote_contact (priv->current_chat);
		/* contact is NULL for multi-user chats.  We don't do
		 * file transfers to MUCs.  We also don't send files
		 * to offline contacts or contacts that don't support
		 * file transfer.
		 */
		if ((contact == NULL) || !empathy_contact_is_online (contact)) {
			gdk_drag_status (context, 0, time_);
			return FALSE;
		}
		if (!(empathy_contact_get_capabilities (contact)
			   & EMPATHY_CAPABILITIES_FT)) {
			gdk_drag_status (context, 0, time_);
			return FALSE;
		}
		gdk_drag_status (context, GDK_ACTION_COPY, time_);
		return TRUE;
	}

	dest_target = gdk_atom_intern_static_string ("text/contact-id");
	if (target == dest_target) {
		/* This is a drag of a contact from a contact list.  Set to COPY.
		   FIXME: If this drag is to a MUC window, it invites the user.
		   Otherwise, it opens a chat.  Should we use a different drag
		   type for invites?  Should we allow ASK?
		 */
		gdk_drag_status (context, GDK_ACTION_COPY, time_);
		return TRUE;
	}

	/* Otherwise, it must be a notebook tab drag.  Set to MOVE. */
	gdk_drag_status (context, GDK_ACTION_MOVE, time_);
	return TRUE;
}

static void
chat_window_drag_data_received (GtkWidget        *widget,
				GdkDragContext   *context,
				int               x,
				int               y,
				GtkSelectionData *selection,
				guint             info,
				guint             time_,
				EmpathyChatWindow *window)
{
	if (info == DND_DRAG_TYPE_CONTACT_ID) {
		EmpathyChat           *chat = NULL;
		EmpathyChatWindow     *old_window;
		TpAccount             *account = NULL;
		TpAccountManager      *account_manager;
		const gchar           *id;
		gchar                **strv;
		const gchar           *account_id;
		const gchar           *contact_id;

		id = (const gchar*) gtk_selection_data_get_data (selection);

		/* FIXME: Perhaps should be sure that the account manager is
		 * prepared before calling _ensure_account on it. */
		account_manager = tp_account_manager_dup ();

		DEBUG ("DND contact from roster with id:'%s'", id);

		strv = g_strsplit (id, ":", 2);
		if (g_strv_length (strv) == 2) {
			account_id = strv[0];
			contact_id = strv[1];
			account =
				tp_account_manager_ensure_account (account_manager, account_id);
			if (account != NULL)
				chat = empathy_chat_window_find_chat (account, contact_id);
		}

		if (account == NULL) {
			g_strfreev (strv);
			gtk_drag_finish (context, FALSE, FALSE, time_);
			return;
		}

		if (!chat) {
			TpConnection *connection;

			connection = tp_account_get_connection (account);

			if (connection) {
				empathy_dispatcher_chat_with_contact_id (
					connection, contact_id, NULL, NULL);
			}

			g_strfreev (strv);
			return;
		}
		g_object_unref (account_manager);
		g_strfreev (strv);

		old_window = chat_window_find_chat (chat);
		if (old_window) {
			if (old_window == window) {
				gtk_drag_finish (context, TRUE, FALSE, time_);
				return;
			}

			empathy_chat_window_move_chat (old_window, window, chat);
		} else {
			empathy_chat_window_add_chat (window, chat);
		}

		/* Added to take care of any outstanding chat events */
		empathy_chat_window_present_chat (chat);

		/* We should return TRUE to remove the data when doing
		 * GDK_ACTION_MOVE, but we don't here otherwise it has
		 * weird consequences, and we handle that internally
		 * anyway with add_chat () and remove_chat ().
		 */
		gtk_drag_finish (context, TRUE, FALSE, time_);
	}
	else if (info == DND_DRAG_TYPE_URI_LIST) {
		EmpathyChatWindowPriv *priv;
		EmpathyContact *contact;
		const gchar *data;

		priv = GET_PRIV (window);
		contact = empathy_chat_get_remote_contact (priv->current_chat);

		/* contact is NULL when current_chat is a multi-user chat.
		 * We don't do file transfers to MUCs, so just cancel the drag.
		 */
		if (contact == NULL) {
			gtk_drag_finish (context, TRUE, FALSE, time_);
			return;
		}

		data = (const gchar *) gtk_selection_data_get_data (selection);
		empathy_send_file_from_uri_list (contact, data);

		gtk_drag_finish (context, TRUE, FALSE, time_);
	}
	else if (info == DND_DRAG_TYPE_TAB) {
		EmpathyChat        **chat;
		EmpathyChatWindow   *old_window = NULL;

		DEBUG ("DND tab");

		chat = (void *) gtk_selection_data_get_data (selection);
		old_window = chat_window_find_chat (*chat);

		if (old_window) {
			EmpathyChatWindowPriv *priv;

			priv = GET_PRIV (window);

			if (old_window == window) {
				DEBUG ("DND tab (within same window)");
				priv->dnd_same_window = TRUE;
				gtk_drag_finish (context, TRUE, FALSE, time_);
				return;
			}

			priv->dnd_same_window = FALSE;
		}

		/* We should return TRUE to remove the data when doing
		 * GDK_ACTION_MOVE, but we don't here otherwise it has
		 * weird consequences, and we handle that internally
		 * anyway with add_chat () and remove_chat ().
		 */
		gtk_drag_finish (context, TRUE, FALSE, time_);
	} else {
		DEBUG ("DND from unknown source");
		gtk_drag_finish (context, FALSE, FALSE, time_);
	}
}

static void
chat_window_finalize (GObject *object)
{
	EmpathyChatWindow     *window;
	EmpathyChatWindowPriv *priv;

	window = EMPATHY_CHAT_WINDOW (object);
	priv = GET_PRIV (window);

	DEBUG ("Finalized: %p", object);

	g_object_unref (priv->ui_manager);
	g_object_unref (priv->chatroom_manager);
	g_object_unref (priv->notify_mgr);

	if (priv->notification != NULL) {
		notify_notification_close (priv->notification, NULL);
		g_object_unref (priv->notification);
		priv->notification = NULL;
		if (priv->notification_data != NULL)
			{
				free_notification_data (priv->notification_data);
				priv->notification_data = NULL;
			}
	}

	if (priv->contact_targets) {
		gtk_target_list_unref (priv->contact_targets);
	}
	if (priv->file_targets) {
		gtk_target_list_unref (priv->file_targets);
	}

	chat_windows = g_list_remove (chat_windows, window);
	gtk_widget_destroy (priv->dialog);

	G_OBJECT_CLASS (empathy_chat_window_parent_class)->finalize (object);
}

static void
empathy_chat_window_class_init (EmpathyChatWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = chat_window_finalize;

	g_type_class_add_private (object_class, sizeof (EmpathyChatWindowPriv));

	/* Set up a style for the close button with no focus padding. */
	gtk_rc_parse_string (
		"style \"empathy-close-button-style\"\n"
		"{\n"
		"  GtkWidget::focus-padding = 0\n"
		"  xthickness = 0\n"
		"  ythickness = 0\n"
		"}\n"
		"widget \"*.empathy-close-button\" style \"empathy-close-button-style\"");

	gtk_notebook_set_window_creation_hook (chat_window_detach_hook, NULL, NULL);
}

static void
empathy_chat_window_init (EmpathyChatWindow *window)
{
	GtkBuilder            *gui;
	GtkAccelGroup         *accel_group;
	GClosure              *closure;
	GtkWidget             *menu;
	GtkWidget             *submenu;
	guint                  i;
	GtkWidget             *chat_vbox;
	gchar                 *filename;
	EmpathySmileyManager  *smiley_manager;
	EmpathyChatWindowPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (window,
		EMPATHY_TYPE_CHAT_WINDOW, EmpathyChatWindowPriv);

	window->priv = priv;
	filename = empathy_file_lookup ("empathy-chat-window.ui", "src");
	gui = empathy_builder_get_file (filename,
				       "chat_window", &priv->dialog,
				       "chat_vbox", &chat_vbox,
				       "ui_manager", &priv->ui_manager,
				       "menu_conv_insert_smiley", &priv->menu_conv_insert_smiley,
				       "menu_conv_favorite", &priv->menu_conv_favorite,
				       "menu_conv_toggle_contacts", &priv->menu_conv_toggle_contacts,
				       "menu_edit_cut", &priv->menu_edit_cut,
				       "menu_edit_copy", &priv->menu_edit_copy,
				       "menu_edit_paste", &priv->menu_edit_paste,
				       "menu_tabs_next", &priv->menu_tabs_next,
				       "menu_tabs_prev", &priv->menu_tabs_prev,
				       "menu_tabs_left", &priv->menu_tabs_left,
				       "menu_tabs_right", &priv->menu_tabs_right,
				       "menu_tabs_detach", &priv->menu_tabs_detach,
				       NULL);
	g_free (filename);

	empathy_builder_connect (gui, window,
			      "menu_conv", "activate", chat_window_conv_activate_cb,
			      "menu_conv_clear", "activate", chat_window_clear_activate_cb,
			      "menu_conv_favorite", "toggled", chat_window_favorite_toggled_cb,
			      "menu_conv_toggle_contacts", "toggled", chat_window_contacts_toggled_cb,
			      "menu_conv_invite_participant", "activate", chat_window_invite_participant_activate_cb,
			      "menu_conv_close", "activate", chat_window_close_activate_cb,
			      "menu_edit", "activate", chat_window_edit_activate_cb,
			      "menu_edit_cut", "activate", chat_window_cut_activate_cb,
			      "menu_edit_copy", "activate", chat_window_copy_activate_cb,
			      "menu_edit_paste", "activate", chat_window_paste_activate_cb,
			      "menu_tabs_next", "activate", chat_window_tabs_next_activate_cb,
			      "menu_tabs_prev", "activate", chat_window_tabs_previous_activate_cb,
			      "menu_tabs_left", "activate", chat_window_tabs_left_activate_cb,
			      "menu_tabs_right", "activate", chat_window_tabs_right_activate_cb,
			      "menu_tabs_detach", "activate", chat_window_detach_activate_cb,
			      "menu_help_contents", "activate", chat_window_help_contents_activate_cb,
			      "menu_help_about", "activate", chat_window_help_about_activate_cb,
			      NULL);

	g_object_ref (priv->ui_manager);
	g_object_unref (gui);

	priv->chatroom_manager = empathy_chatroom_manager_dup_singleton (NULL);

	priv->notebook = gtk_notebook_new ();
	gtk_notebook_set_group (GTK_NOTEBOOK (priv->notebook), "EmpathyChatWindow");
	gtk_notebook_set_scrollable (GTK_NOTEBOOK (priv->notebook), TRUE);
	gtk_notebook_popup_enable (GTK_NOTEBOOK (priv->notebook));
	gtk_box_pack_start (GTK_BOX (chat_vbox), priv->notebook, TRUE, TRUE, 0);
	gtk_widget_show (priv->notebook);

	/* Set up accels */
	accel_group = gtk_accel_group_new ();
	gtk_window_add_accel_group (GTK_WINDOW (priv->dialog), accel_group);

	for (i = 0; i < G_N_ELEMENTS (tab_accel_keys); i++) {
		closure =  g_cclosure_new (G_CALLBACK (chat_window_accel_cb),
					   window,
					   NULL);
		gtk_accel_group_connect (accel_group,
					 tab_accel_keys[i],
					 GDK_MOD1_MASK,
					 0,
					 closure);
	}

	g_object_unref (accel_group);

	/* Set up drag target lists */
	priv->contact_targets = gtk_target_list_new (drag_types_dest_contact,
						     G_N_ELEMENTS (drag_types_dest_contact));
	priv->file_targets = gtk_target_list_new (drag_types_dest_file,
						  G_N_ELEMENTS (drag_types_dest_file));

	/* Set up smiley menu */
	smiley_manager = empathy_smiley_manager_dup_singleton ();
	submenu = empathy_smiley_menu_new (smiley_manager,
					   chat_window_insert_smiley_activate_cb,
					   window);
	menu = gtk_ui_manager_get_widget (priv->ui_manager,
		"/chats_menubar/menu_conv/menu_conv_insert_smiley");
	gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu), submenu);
	g_object_unref (smiley_manager);

	/* Set up signals we can't do with ui file since we may need to
	 * block/unblock them at some later stage.
	 */

	g_signal_connect (priv->dialog,
			  "delete_event",
			  G_CALLBACK (chat_window_delete_event_cb),
			  window);
	g_signal_connect (priv->dialog,
			  "focus_in_event",
			  G_CALLBACK (chat_window_focus_in_event_cb),
			  window);
	g_signal_connect_after (priv->notebook,
				"switch_page",
				G_CALLBACK (chat_window_page_switched_cb),
				window);
	g_signal_connect (priv->notebook,
			  "page_added",
			  G_CALLBACK (chat_window_page_added_cb),
			  window);
	g_signal_connect (priv->notebook,
			  "page_removed",
			  G_CALLBACK (chat_window_page_removed_cb),
			  window);

	/* Set up drag and drop */
	gtk_drag_dest_set (GTK_WIDGET (priv->notebook),
			   GTK_DEST_DEFAULT_ALL,
			   drag_types_dest,
			   G_N_ELEMENTS (drag_types_dest),
			   GDK_ACTION_MOVE | GDK_ACTION_COPY);

	/* connect_after to allow GtkNotebook's built-in tab switching */
	g_signal_connect_after (priv->notebook,
				"drag-motion",
				G_CALLBACK (chat_window_drag_motion),
				window);
	g_signal_connect (priv->notebook,
			  "drag-data-received",
			  G_CALLBACK (chat_window_drag_data_received),
			  window);

	chat_windows = g_list_prepend (chat_windows, window);

	/* Set up private details */
	priv->chats = NULL;
	priv->chats_new_msg = NULL;
	priv->chats_composing = NULL;
	priv->current_chat = NULL;

	priv->notify_mgr = empathy_notify_manager_dup_singleton ();
}

EmpathyChatWindow *
empathy_chat_window_new (void)
{
	return EMPATHY_CHAT_WINDOW (g_object_new (EMPATHY_TYPE_CHAT_WINDOW, NULL));
}

/* Returns the window to open a new tab in if there is only one window
 * visble, otherwise, returns NULL indicating that a new window should
 * be added.
 */
EmpathyChatWindow *
empathy_chat_window_get_default (gboolean room)
{
	GList    *l;
	gboolean  separate_windows = TRUE;

	empathy_conf_get_bool (empathy_conf_get (),
			      EMPATHY_PREFS_UI_SEPARATE_CHAT_WINDOWS,
			      &separate_windows);

	if (separate_windows) {
		/* Always create a new window */
		return NULL;
	}

	for (l = chat_windows; l; l = l->next) {
		EmpathyChatWindowPriv *priv;
		EmpathyChatWindow *chat_window;
		GtkWidget         *dialog;

		chat_window = l->data;
		priv = GET_PRIV (chat_window);

		dialog = empathy_chat_window_get_dialog (chat_window);
		if (empathy_window_get_is_visible (GTK_WINDOW (dialog))) {
			guint nb_rooms, nb_private;
			empathy_chat_window_get_nb_chats (chat_window, &nb_rooms, &nb_private);

			/* Skip the window if there aren't any rooms in it */
			if (room && nb_rooms == 0)
				continue;

			/* Skip the window if there aren't any 1-1 chats in it */
			if (!room && nb_private == 0)
				continue;

			/* Found a visible window on this desktop */
			return chat_window;
		}
	}

	return NULL;
}

GtkWidget *
empathy_chat_window_get_dialog (EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;

	g_return_val_if_fail (window != NULL, NULL);

	priv = GET_PRIV (window);

	return priv->dialog;
}

void
empathy_chat_window_add_chat (EmpathyChatWindow *window,
			      EmpathyChat	*chat)
{
	EmpathyChatWindowPriv *priv;
	GtkWidget             *label;
	GtkWidget             *popup_label;
	GtkWidget             *child;

	g_return_if_fail (window != NULL);
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	priv = GET_PRIV (window);

	/* Reference the chat object */
	g_object_ref (chat);

	/* If this window has just been created, position it */
	if (priv->chats == NULL) {
		const gchar *name = "chat-window";
		gboolean     separate_windows;

		empathy_conf_get_bool (empathy_conf_get (),
				       EMPATHY_PREFS_UI_SEPARATE_CHAT_WINDOWS,
				       &separate_windows);

		if (separate_windows) {
			name = empathy_chat_get_id (chat);
		}
		else if (empathy_chat_is_room (chat)) {
			name = "room-window";
		}

		empathy_geometry_bind (GTK_WINDOW (priv->dialog), name);
	}

	child = GTK_WIDGET (chat);
	label = chat_window_create_label (window, chat, TRUE);
	popup_label = chat_window_create_label (window, chat, FALSE);
	gtk_widget_show (child);

	g_signal_connect (chat, "notify::name",
			  G_CALLBACK (chat_window_chat_notify_cb),
			  NULL);
	g_signal_connect (chat, "notify::subject",
			  G_CALLBACK (chat_window_chat_notify_cb),
			  NULL);
	g_signal_connect (chat, "notify::remote-contact",
			  G_CALLBACK (chat_window_chat_notify_cb),
			  NULL);
	chat_window_chat_notify_cb (chat);

	gtk_notebook_append_page_menu (GTK_NOTEBOOK (priv->notebook), child, label, popup_label);
	gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (priv->notebook), child, TRUE);
	gtk_notebook_set_tab_detachable (GTK_NOTEBOOK (priv->notebook), child, TRUE);
	gtk_notebook_set_tab_label_packing (GTK_NOTEBOOK (priv->notebook), child,
					    TRUE, TRUE, GTK_PACK_START);

	DEBUG ("Chat added (%d references)", G_OBJECT (chat)->ref_count);
}

void
empathy_chat_window_remove_chat (EmpathyChatWindow *window,
				 EmpathyChat	   *chat)
{
	EmpathyChatWindowPriv *priv;
	gint                   position;
	EmpathyContact        *remote_contact;

	g_return_if_fail (window != NULL);
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	priv = GET_PRIV (window);

	g_signal_handlers_disconnect_by_func (chat,
					      chat_window_chat_notify_cb,
					      NULL);
	remote_contact = g_object_get_data (G_OBJECT (chat),
					    "chat-window-remote-contact");
	if (remote_contact) {
		g_signal_handlers_disconnect_by_func (remote_contact,
						      chat_window_update_chat_tab,
						      chat);
	}

	position = gtk_notebook_page_num (GTK_NOTEBOOK (priv->notebook),
					  GTK_WIDGET (chat));
	gtk_notebook_remove_page (GTK_NOTEBOOK (priv->notebook), position);

	DEBUG ("Chat removed (%d references)", G_OBJECT (chat)->ref_count - 1);

	g_object_unref (chat);
}

void
empathy_chat_window_move_chat (EmpathyChatWindow *old_window,
			       EmpathyChatWindow *new_window,
			       EmpathyChat       *chat)
{
	GtkWidget *widget;

	g_return_if_fail (EMPATHY_IS_CHAT_WINDOW (old_window));
	g_return_if_fail (EMPATHY_IS_CHAT_WINDOW (new_window));
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	widget = GTK_WIDGET (chat);

	DEBUG ("Chat moving with widget:%p (%d references)", widget,
		G_OBJECT (widget)->ref_count);

	/* We reference here to make sure we don't loose the widget
	 * and the EmpathyChat object during the move.
	 */
	g_object_ref (chat);
	g_object_ref (widget);

	empathy_chat_window_remove_chat (old_window, chat);
	empathy_chat_window_add_chat (new_window, chat);

	g_object_unref (widget);
	g_object_unref (chat);
}

void
empathy_chat_window_switch_to_chat (EmpathyChatWindow *window,
				    EmpathyChat	      *chat)
{
	EmpathyChatWindowPriv *priv;
	gint                  page_num;

	g_return_if_fail (window != NULL);
	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	priv = GET_PRIV (window);

	page_num = gtk_notebook_page_num (GTK_NOTEBOOK (priv->notebook),
					  GTK_WIDGET (chat));
	gtk_notebook_set_current_page (GTK_NOTEBOOK (priv->notebook),
				       page_num);
}

gboolean
empathy_chat_window_has_focus (EmpathyChatWindow *window)
{
	EmpathyChatWindowPriv *priv;
	gboolean              has_focus;

	g_return_val_if_fail (EMPATHY_IS_CHAT_WINDOW (window), FALSE);

	priv = GET_PRIV (window);

	g_object_get (priv->dialog, "has-toplevel-focus", &has_focus, NULL);

	return has_focus;
}

EmpathyChat *
empathy_chat_window_find_chat (TpAccount   *account,
			       const gchar *id)
{
	GList *l;

	g_return_val_if_fail (!EMP_STR_EMPTY (id), NULL);

	for (l = chat_windows; l; l = l->next) {
		EmpathyChatWindowPriv *priv;
		EmpathyChatWindow     *window;
		GList                *ll;

		window = l->data;
		priv = GET_PRIV (window);

		for (ll = priv->chats; ll; ll = ll->next) {
			EmpathyChat *chat;

			chat = ll->data;

			if (account == empathy_chat_get_account (chat) &&
			    !tp_strdiff (id, empathy_chat_get_id (chat))) {
				return chat;
			}
		}
	}

	return NULL;
}

void
empathy_chat_window_present_chat (EmpathyChat *chat)
{
	EmpathyChatWindow     *window;
	EmpathyChatWindowPriv *priv;

	g_return_if_fail (EMPATHY_IS_CHAT (chat));

	window = chat_window_find_chat (chat);

	/* If the chat has no window, create one */
	if (window == NULL) {
		window = empathy_chat_window_get_default (empathy_chat_is_room (chat));
		if (!window) {
			window = empathy_chat_window_new ();
		}

		empathy_chat_window_add_chat (window, chat);
	}

	priv = GET_PRIV (window);
	empathy_chat_window_switch_to_chat (window, chat);
	empathy_window_present (GTK_WINDOW (priv->dialog), TRUE);

 	gtk_widget_grab_focus (chat->input_text_view);
}

void
empathy_chat_window_get_nb_chats (EmpathyChatWindow *self,
			       guint *nb_rooms,
			       guint *nb_private)
{
	EmpathyChatWindowPriv *priv = GET_PRIV (self);
	GList *l;
	guint _nb_rooms = 0, _nb_private = 0;

	for (l = priv->chats; l != NULL; l = g_list_next (l)) {
		if (empathy_chat_is_room (EMPATHY_CHAT (l->data)))
			_nb_rooms++;
		else
			_nb_private++;
	}

	if (nb_rooms != NULL)
		*nb_rooms = _nb_rooms;
	if (nb_private != NULL)
		*nb_private = _nb_private;
}
