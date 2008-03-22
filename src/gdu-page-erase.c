/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-page-erase.c
 *
 * Copyright (C) 2007 David Zeuthen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <config.h>
#include <glib-object.h>
#include <string.h>
#include <glib/gi18n.h>
#include <polkit-gnome/polkit-gnome.h>

#include "gdu-page.h"
#include "gdu-page-erase.h"
#include "gdu-util.h"

struct _GduPageErasePrivate
{
        GduShell *shell;

        GtkWidget *main_vbox;
        GtkWidget *page_erase_label_entry;
        GtkWidget *page_erase_type_combo_box;
        GtkWidget *page_erase_secure_erase_combo_box;

        PolKitAction *pk_erase_action;
        PolKitGnomeAction *erase_action;
};

static GObjectClass *parent_class = NULL;

static void gdu_page_erase_page_iface_init (GduPageIface *iface);
G_DEFINE_TYPE_WITH_CODE (GduPageErase, gdu_page_erase, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (GDU_TYPE_PAGE,
                                                gdu_page_erase_page_iface_init))

enum {
        PROP_0,
        PROP_SHELL,
};

static char         *gdu_page_erase_get_fstype     (GduPageErase *page);
static char         *gdu_page_erase_get_fslabel    (GduPageErase *page);
static char         *gdu_page_erase_get_erase_type (GduPageErase *page);

static void
gdu_page_erase_finalize (GduPageErase *page)
{
        polkit_action_unref (page->priv->pk_erase_action);
        g_object_unref (page->priv->erase_action);

        if (page->priv->shell != NULL)
                g_object_unref (page->priv->shell);

        if (G_OBJECT_CLASS (parent_class)->finalize)
                (* G_OBJECT_CLASS (parent_class)->finalize) (G_OBJECT (page));
}

static void
gdu_page_erase_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
        GduPageErase *page = GDU_PAGE_ERASE (object);

        switch (prop_id) {
        case PROP_SHELL:
                if (page->priv->shell != NULL)
                        g_object_unref (page->priv->shell);
                page->priv->shell = g_object_ref (g_value_get_object (value));
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdu_page_erase_get_property (GObject     *object,
                             guint        prop_id,
                             GValue      *value,
                             GParamSpec  *pspec)
{
        GduPageErase *page = GDU_PAGE_ERASE (object);

        switch (prop_id) {
        case PROP_SHELL:
                g_value_set_object (value, page->priv->shell);
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
    }
}

static void
gdu_page_erase_class_init (GduPageEraseClass *klass)
{
        GObjectClass *obj_class = (GObjectClass *) klass;

        parent_class = g_type_class_peek_parent (klass);

        obj_class->finalize = (GObjectFinalizeFunc) gdu_page_erase_finalize;
        obj_class->set_property = gdu_page_erase_set_property;
        obj_class->get_property = gdu_page_erase_get_property;

        /**
         * GduPageErase:shell:
         *
         * The #GduShell instance hosting this page.
         */
        g_object_class_install_property (obj_class,
                                         PROP_SHELL,
                                         g_param_spec_object ("shell",
                                                              NULL,
                                                              NULL,
                                                              GDU_TYPE_SHELL,
                                                              G_PARAM_CONSTRUCT_ONLY |
                                                              G_PARAM_WRITABLE |
                                                              G_PARAM_READABLE));

}

static void
page_erase_type_combo_box_changed (GtkWidget *combo_box, gpointer user_data)
{
        GduPageErase *page = GDU_PAGE_ERASE (user_data);
        char *fstype;
        GduCreatableFilesystem *creatable_fs;
        gboolean label_entry_sensitive;
        gboolean can_erase;
        int max_label_len;

        label_entry_sensitive = FALSE;
        can_erase = FALSE;
        max_label_len = 0;

        fstype = gdu_util_fstype_combo_box_get_selected (combo_box);
        if (fstype != NULL) {
                creatable_fs = gdu_util_find_creatable_filesystem_for_fstype (fstype);
                if (creatable_fs != NULL) {
                        max_label_len = creatable_fs->max_label_len;
                }
                can_erase = TRUE;
        }

        if (max_label_len > 0)
                label_entry_sensitive = TRUE;

        gtk_entry_set_max_length (GTK_ENTRY (page->priv->page_erase_label_entry), max_label_len);
        gtk_widget_set_sensitive (page->priv->page_erase_label_entry, label_entry_sensitive);
        polkit_gnome_action_set_sensitive (page->priv->erase_action, can_erase);

        g_free (fstype);
}

static void
erase_action_callback (GtkAction *action, gpointer user_data)
{
        GduPageErase *page = GDU_PAGE_ERASE (user_data);
        int response;
        GtkWidget *dialog;
        char *fslabel;
        char *fstype;
        const char *fserase;
        GduDevice *device;

        device = gdu_presentable_get_device (gdu_shell_get_selected_presentable (page->priv->shell));
        g_assert (device != NULL);

        fstype = gdu_page_erase_get_fstype (page);
        fslabel = gdu_page_erase_get_fslabel (page);
        if (fslabel == NULL)
                fslabel = g_strdup ("");

        fserase = gdu_page_erase_get_erase_type (page);

        /* TODO: mention what drive the volume is on etc. */
        dialog = gtk_message_dialog_new_with_markup (GTK_WINDOW (gdu_shell_get_toplevel (page->priv->shell)),
                                                     GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     GTK_MESSAGE_WARNING,
                                                     GTK_BUTTONS_CANCEL,
                                                     _("<b><big>Are you sure you want to erase the volume?</big></b>"));

        gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
                                                    _("All data on the volume will be irrecovably erased. Make sure data important to you is backed up. This action cannot be undone."));
        /* ... until we add data recovery to g-d-u! */

        gtk_dialog_add_button (GTK_DIALOG (dialog), _("Erase"), 0);

        response = gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        if (response != 0)
                goto out;

        gdu_device_op_mkfs (device, fstype, fslabel, fserase);

out:
        g_object_unref (device);
        g_free (fstype);
        g_free (fslabel);
}

static void
gdu_page_erase_init (GduPageErase *page)
{
        int row;
        GtkWidget *label;
        GtkWidget *align;
        GtkWidget *vbox;
        GtkWidget *table;
        GtkWidget *combo_box;
        GtkWidget *entry;
        GtkWidget *button;
        GtkWidget *button_box;

        page->priv = g_new0 (GduPageErasePrivate, 1);

        page->priv->pk_erase_action = polkit_action_new ();
        polkit_action_set_action_id (page->priv->pk_erase_action, "org.freedesktop.devicekit.disks.erase");

        page->priv->erase_action = polkit_gnome_action_new_default ("erase",
                                                                    page->priv->pk_erase_action,
                                                                    _("_Erase"),
                                                                    _("Erase"));
        g_object_set (page->priv->erase_action,
                      "auth-label", _("_Erase..."),
                      "yes-icon-name", GTK_STOCK_CLEAR,
                      "no-icon-name", GTK_STOCK_CLEAR,
                      "auth-icon-name", GTK_STOCK_CLEAR,
                      "self-blocked-icon-name", GTK_STOCK_CLEAR,
                      NULL);
        g_signal_connect (page->priv->erase_action, "activate", G_CALLBACK (erase_action_callback), page);

        // TODO:
        //gtk_action_group_add_action (shell->priv->action_group, GTK_ACTION (shell->priv->erase_action));


        page->priv->main_vbox = gtk_vbox_new (FALSE, 10);
        gtk_container_set_border_width (GTK_CONTAINER (page->priv->main_vbox), 8);

        /* volume format + label */
        label = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (label), _("<b>Volume</b>"));
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        gtk_box_pack_start (GTK_BOX (page->priv->main_vbox), label, FALSE, FALSE, 0);
        vbox = gtk_vbox_new (FALSE, 5);
        align = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
        gtk_alignment_set_padding (GTK_ALIGNMENT (align), 0, 0, 24, 0);
        gtk_container_add (GTK_CONTAINER (align), vbox);
        gtk_box_pack_start (GTK_BOX (page->priv->main_vbox), align, FALSE, TRUE, 0);

        /* explanatory text */
        label = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (label), _("To erase a volume or disk, select it from the tree and then select the format and label to use."));
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, TRUE, 0);

        table = gtk_table_new (2, 2, FALSE);
        gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, FALSE, 0);

        row = 0;

        /* file system label */
        label = gtk_label_new (NULL);
        gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.5);
        gtk_label_set_markup_with_mnemonic (GTK_LABEL (label), _("_Label:"));
        gtk_table_attach (GTK_TABLE (table), label, 0, 1, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        entry = gtk_entry_new ();
        gtk_table_attach (GTK_TABLE (table), entry, 1, 2, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
        page->priv->page_erase_label_entry = entry;

        row++;

        /* type */
        label = gtk_label_new (NULL);
        gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.5);
        gtk_label_set_markup_with_mnemonic (GTK_LABEL (label), _("_Type:"));
        gtk_table_attach (GTK_TABLE (table), label, 0, 1, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        combo_box = gdu_util_fstype_combo_box_create (NULL);
        gtk_table_attach (GTK_TABLE (table), combo_box, 1, 2, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        gtk_label_set_mnemonic_widget (GTK_LABEL (label), combo_box);
        page->priv->page_erase_type_combo_box = combo_box;

        row++;

        /* type desc */
        label = gtk_label_new (NULL);
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_width_chars (GTK_LABEL (label), 40);
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        gtk_table_attach (GTK_TABLE (table), label, 1, 2, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        gdu_util_fstype_combo_box_set_desc_label (combo_box, label);

        row++;

        /* secure erase */
        label = gtk_label_new (NULL);
        gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.5);
        gtk_label_set_markup_with_mnemonic (GTK_LABEL (label), _("_Secure Erase:"));
        gtk_table_attach (GTK_TABLE (table), label, 0, 1, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        combo_box = gdu_util_secure_erase_combo_box_create ();
        gtk_table_attach (GTK_TABLE (table), combo_box, 1, 2, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        gtk_label_set_mnemonic_widget (GTK_LABEL (label), combo_box);
        page->priv->page_erase_secure_erase_combo_box = combo_box;

        row++;

        /* secure erase desc */
        label = gtk_label_new (NULL);
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_width_chars (GTK_LABEL (label), 40);
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        gtk_table_attach (GTK_TABLE (table), label, 1, 2, row, row + 1,
                          GTK_FILL, GTK_EXPAND | GTK_FILL, 2, 2);
        gdu_util_secure_erase_combo_box_set_desc_label (combo_box, label);


        /* update sensivity and length of fs label + entry */
        g_signal_connect (page->priv->page_erase_type_combo_box, "changed",
                          G_CALLBACK (page_erase_type_combo_box_changed), page);


        button_box = gtk_hbutton_box_new ();
        gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box), GTK_BUTTONBOX_START);
        gtk_box_set_spacing (GTK_BOX (button_box), 6);

        button = polkit_gnome_action_create_button (page->priv->erase_action);
        gtk_container_add (GTK_CONTAINER (button_box), button);
        gtk_box_pack_start (GTK_BOX (vbox), button_box, TRUE, TRUE, 0);
}


GduPageErase *
gdu_page_erase_new (GduShell *shell)
{
        return GDU_PAGE_ERASE (g_object_new (GDU_TYPE_PAGE_ERASE, "shell", shell, NULL));
}


static gboolean
gdu_page_erase_update (GduPage *_page, GduPresentable *presentable)
{
        GduPageErase *page = GDU_PAGE_ERASE (_page);
        gboolean ret;
        GduDevice *device;

        ret = FALSE;

        device = gdu_presentable_get_device (presentable);
        if (device == NULL)
                goto out;

        if (strcmp (gdu_device_id_get_usage (device), "filesystem") == 0) {
                if (!gdu_util_fstype_combo_box_select (page->priv->page_erase_type_combo_box,
                                                       gdu_device_id_get_type (device))) {
                        /* if fstype of device isn't in creatable, clear selection item */
                        gtk_combo_box_set_active (GTK_COMBO_BOX (page->priv->page_erase_type_combo_box), -1);
                        gtk_entry_set_text (GTK_ENTRY (page->priv->page_erase_label_entry), "");
                } else {
                        /* it was.. choose the same label */
                        gtk_entry_set_text (GTK_ENTRY (page->priv->page_erase_label_entry),
                                            gdu_device_id_get_label (device));
                }
        } else if (strlen (gdu_device_id_get_usage (device)) == 0) {
                /* couldn't identify anything; choose first in creatable fs list */
                gtk_combo_box_set_active (GTK_COMBO_BOX (page->priv->page_erase_type_combo_box), 0);
                gtk_entry_set_text (GTK_ENTRY (page->priv->page_erase_label_entry), "");
        } else {
                /* something else, not a file system, clear selection item */
                gtk_combo_box_set_active (GTK_COMBO_BOX (page->priv->page_erase_type_combo_box), -1);
                gtk_entry_set_text (GTK_ENTRY (page->priv->page_erase_label_entry), "");
        }

        ret = TRUE;
out:
        if (device != NULL)
                g_object_unref (device);

        return ret;
}

static char *
gdu_page_erase_get_fstype (GduPageErase *page)
{
        return gdu_util_fstype_combo_box_get_selected (page->priv->page_erase_type_combo_box);
}

static char *
gdu_page_erase_get_fslabel (GduPageErase *page)
{
        char *ret;

        if (GTK_WIDGET_IS_SENSITIVE (page->priv->page_erase_label_entry))
                ret = g_strdup (gtk_entry_get_text (GTK_ENTRY (page->priv->page_erase_label_entry)));
        else
                ret = NULL;

        return ret;
}

static char *
gdu_page_erase_get_erase_type (GduPageErase *page)
{
        return gdu_util_secure_erase_combo_box_get_selected (page->priv->page_erase_secure_erase_combo_box);
}

static GtkWidget *
gdu_page_erase_get_widget (GduPage *_page)
{
        GduPageErase *page = GDU_PAGE_ERASE (_page);
        return page->priv->main_vbox;
}

static char *
gdu_page_erase_get_name (GduPage *page)
{
        return g_strdup (_("Erase"));
}

static void
gdu_page_erase_page_iface_init (GduPageIface *iface)
{
        iface->get_widget = gdu_page_erase_get_widget;
        iface->get_name = gdu_page_erase_get_name;
        iface->update = gdu_page_erase_update;
}
