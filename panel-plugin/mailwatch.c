/*
 *  xfce4-mailwatch-plugin - a mail notification applet for the xfce4 panel
 *  Copyright (c) 2005 Brian Tarricone <bjt23@cornell.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License ONLY.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <glib.h>
#include <gtk/gtk.h>

#include <libxfce4util/libxfce4util.h>
#include <libxfcegui4/libxfcegui4.h>

#include "mailwatch.h"

#define DEFAULT_TIMEOUT (60*10)
#define BORDER          8

#if !GTK_CHECK_VERSION(2, 6, 0)
#define GTK_STOCK_EDIT GTK_STOCK_PROPERTIES
#endif

typedef struct
{
    XfceMailwatchMailbox *mailbox;
    gchar *mailbox_name;
    guint num_new_messages;
} XfceMailwatchMailboxData;

struct _XfceMailwatch
{
    gint watch_timeout;
    gchar *config_file;
    
    GList *mailbox_types;  /* XfceMailwatchMailboxType * */
    GList *mailboxes;      /* XfceMailwatchMailboxData * */
    
    GMutex *mailboxes_mx;
    
    GList *tc_callbacks;
    GList *tc_data;
    
    /* config GUI */
    GtkWidget *config_treeview;
    GtkWidget *mbox_types_lbl;
};

/* fwd decl from other modules... */
extern XfceMailwatchMailboxType builtin_mailbox_type_imap;
extern XfceMailwatchMailboxType builtin_mailbox_type_maildir;
extern XfceMailwatchMailboxType builtin_mailbox_type_mbox;

XfceMailwatchMailboxType *builtin_mailbox_types[] = {
    &builtin_mailbox_type_imap,
    &builtin_mailbox_type_maildir,
    &builtin_mailbox_type_mbox,
    NULL
};
#define N_BUILTIN_MAILBOX_TYPES (sizeof(builtin_mailbox_types)/sizeof(builtin_mailbox_types[0]))

static GList *
mailwatch_load_mailbox_types()
{
    GList *mailbox_types = NULL;
    gint i;
    
    for(i = 0; builtin_mailbox_types[i]; i++)
        mailbox_types = g_list_prepend(mailbox_types, builtin_mailbox_types[i]);
    mailbox_types = g_list_reverse(mailbox_types);
    
    return mailbox_types;
}

XfceMailwatch *
xfce_mailwatch_new()
{
    XfceMailwatch *mailwatch = g_new0(XfceMailwatch, 1);
    
    if(!g_thread_supported())
        g_thread_init(NULL);
    if(!g_thread_supported()) {
        xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
        g_critical(_("xfce4-mailwatch-plugin: Unable to initialise GThread support.  This is likely a problem with your GLib install."));
        return NULL;
    }
    
    mailwatch->watch_timeout = DEFAULT_TIMEOUT;
    mailwatch->mailbox_types = mailwatch_load_mailbox_types();
    mailwatch->mailboxes_mx = g_mutex_new();
    
    return mailwatch;
}

void
xfce_mailwatch_destroy(XfceMailwatch *mailwatch)
{
    GList *stuff_to_free, *l;
    
    g_return_if_fail(mailwatch);
    
    /* lock it, bitch! */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    /* just clear out the mailbox list.  we have to call free_mailbox_func for
     * each mailbox outside the mailboxes_mx lock so we don't cause deadlocks */
    stuff_to_free = mailwatch->mailboxes;
    mailwatch->mailboxes = NULL;
    
    /* we are SO done. */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    for(l = stuff_to_free; l; l = l->next) {
        XfceMailwatchMailboxData *mdata = l->data;
        
        mdata->mailbox->type->free_mailbox_func(mdata->mailbox);
        g_free(mdata->mailbox_name);
        g_free(mdata);
    }
    if(stuff_to_free)
        g_list_free(stuff_to_free);
    
    /* really.  SO SO done. */
    g_mutex_free(mailwatch->mailboxes_mx);
    
    g_list_free(mailwatch->mailbox_types);
    g_free(mailwatch->config_file);
    
    g_free(mailwatch);
}

void
xfce_mailwatch_set_config_file(XfceMailwatch *mailwatch, const gchar *filename)
{
    g_return_if_fail(mailwatch && filename);
    
    g_free(mailwatch->config_file);
    mailwatch->config_file = g_strdup(filename);
}

G_CONST_RETURN gchar *
xfce_mailwatch_get_config_file(XfceMailwatch *mailwatch)
{
    g_return_val_if_fail(mailwatch, NULL);
    
    return mailwatch->config_file;
}

gboolean
xfce_mailwatch_load_config(XfceMailwatch *mailwatch)
{
    XfceRc *rcfile;
    gchar buf[32];
    GList *l;
    gint i, j, nmailboxes;
    
    g_return_val_if_fail(mailwatch, FALSE);
    g_return_val_if_fail(mailwatch->config_file, FALSE);
    g_return_val_if_fail(!mailwatch->mailboxes, FALSE);  /* FIXME: yeah? */
    
    rcfile = xfce_rc_config_open(XFCE_RESOURCE_CONFIG, mailwatch->config_file, TRUE);
    if(!rcfile)
        return TRUE;  /* assume no config file exists yet? */
    
    xfce_rc_set_group(rcfile, "mailwatch");
    mailwatch->watch_timeout = xfce_rc_read_int_entry(rcfile, "watch_timeout",
            DEFAULT_TIMEOUT);
    nmailboxes = xfce_rc_read_int_entry(rcfile, "nmailboxes", 0);
    
    DBG("nmailboxes = %d", nmailboxes);
    
    /* lock mutex - doesn't matter yet, but once we start creating mailboxes,
     * it will. */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    for(i = 0; i < nmailboxes; i++) {
        const gchar *mailbox_id, *mailbox_name;
        XfceMailwatchMailbox *mailbox = NULL;
        XfceMailwatchMailboxData *mdata;
        gchar **cfg_entries;
        GList *config_params = NULL;
        
        g_snprintf(buf, 32, "mailbox_name%d", i);
        mailbox_name = xfce_rc_read_entry(rcfile, buf, NULL);
        DBG("first mailbox is '%s'", mailbox_name);
        if(!mailbox_name)
            continue;
        
        g_snprintf(buf, 32, "mailbox%d", i);
        mailbox_id = xfce_rc_read_entry(rcfile, buf, NULL);
        if(!mailbox_id)
            continue;
        
        if(!xfce_rc_has_group(rcfile, buf))
            continue;
        xfce_rc_set_group(rcfile, buf);
        
        for(l = mailwatch->mailbox_types; l; l = l->next) {
            XfceMailwatchMailboxType *mtype = l->data;
            if(!strcmp(mtype->id, mailbox_id)) {
                mailbox = mtype->new_mailbox_func(mailwatch, mtype);
                if(!mailbox->type)
                    mailbox->type = mtype;
                mailbox->type->set_activated_func(mailbox, FALSE);
                break;
            }
        }
        if(!mailbox)
            continue;
        
        mdata = g_new0(XfceMailwatchMailboxData, 1);
        mdata->mailbox = mailbox;
        mdata->mailbox_name = g_strdup(mailbox_name);
        mailwatch->mailboxes = g_list_append(mailwatch->mailboxes, mdata);
        
        cfg_entries = xfce_rc_get_entries(rcfile, buf);
        if(!cfg_entries)
            continue;
        
        for(j = 0; cfg_entries[j]; j++) {
            XfceMailwatchParam *param;
            gchar **keyvalue = g_strsplit(cfg_entries[j], "=", 2);
            
            if(!keyvalue)
                continue;
            if(!keyvalue[0] || !keyvalue[1]) {
                g_strfreev(keyvalue);
                continue;
            }
            
            DBG("got key='%s', value='%s'", keyvalue[0], keyvalue[1]);
            
            param = g_new(XfceMailwatchParam, 1);
            param->key = keyvalue[0];
            param->value = keyvalue[1];
            g_free(keyvalue);  /* yes, not using g_strfreev() is correct */
            
            config_params = g_list_append(config_params, param);
        }
        g_strfreev(cfg_entries);
        
        mailbox->type->restore_param_list_func(mailbox, config_params);
        mailbox->type->set_activated_func(mailbox, TRUE);
        for(l = config_params; l; l = l->next) {
            XfceMailwatchParam *param = l->data;
            g_free(param->key);
            g_free(param->value);
            g_free(param);
        }
        if(config_params)
            g_list_free(config_params);
    }
    
    /* we're done, unlock mutex */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    xfce_rc_close(rcfile);
    
    return TRUE;
}

gboolean
xfce_mailwatch_save_config(XfceMailwatch *mailwatch)
{
    XfceRc *rcfile;
    gchar *config_file, *config_file_new, buf[32];
    GList *l;
    gint i;
    
    g_return_val_if_fail(mailwatch, FALSE);
    g_return_val_if_fail(mailwatch->config_file, FALSE);
    
    config_file = xfce_resource_save_location(XFCE_RESOURCE_CONFIG,
            mailwatch->config_file, TRUE);
    if(!config_file)
        return FALSE;
    
    config_file_new = g_strconcat(config_file, ".new", NULL);
    unlink(config_file_new);
    
    rcfile = xfce_rc_simple_open(config_file_new, FALSE);
    if(!rcfile) {
        g_free(config_file);
        g_free(config_file_new);
        return FALSE;
    }
    
    /* we probably don't need to lock here, but it can't hurt */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    /* write out global config and index */
    xfce_rc_set_group(rcfile, "mailwatch");
    xfce_rc_write_int_entry(rcfile, "watch_timeout", mailwatch->watch_timeout);
    xfce_rc_write_int_entry(rcfile, "nmailboxes",
            g_list_length(mailwatch->mailboxes));
    for(l = mailwatch->mailboxes, i = 0; l; l = l->next, i++) {
        XfceMailwatchMailboxData *mdata = l->data;
        
        g_snprintf(buf, 32, "mailbox%d", i);
        xfce_rc_write_entry(rcfile, buf, mdata->mailbox->type->id);
        g_snprintf(buf, 32, "mailbox_name%d", i);
        xfce_rc_write_entry(rcfile, buf, mdata->mailbox_name);
    }
    
    /* write out config data for each mailbox */
    for(l = mailwatch->mailboxes, i = 0; l; l = l->next, i++) {
        XfceMailwatchMailboxData *mdata = l->data;
        GList *config_data, *m;
        
        g_snprintf(buf, 32, "mailbox%d", i);
        xfce_rc_set_group(rcfile, buf);
        
        config_data = mdata->mailbox->type->save_param_list_func(mdata->mailbox);
        for(m = config_data; m; m = m->next) {
            XfceMailwatchParam *param = m->data;
            
            xfce_rc_write_entry(rcfile, param->key, param->value);
            g_free(param->key);
            g_free(param->value);
            g_free(param);
        }
        if(config_data)
            g_list_free(config_data);
    }
    
    /* and we're done, unlock */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    xfce_rc_close(rcfile);
    
    if(rename(config_file_new, config_file)) {
        g_free(config_file);
        g_free(config_file_new);
        return FALSE;
    }
    
    g_free(config_file);
    g_free(config_file_new);
    
    return TRUE;
}

guint
xfce_mailwatch_get_timeout(XfceMailwatch *mailwatch)
{
    g_return_val_if_fail(mailwatch, 0);
    
    return mailwatch->watch_timeout;
}

void
xfce_mailwatch_set_timeout(XfceMailwatch *mailwatch, guint seconds)
{
    GList *cl, *dl;
    
    g_return_if_fail(mailwatch);
    
    mailwatch->watch_timeout = seconds;
    
    for(cl = mailwatch->tc_callbacks, dl = mailwatch->tc_data;
        cl && dl;
        cl = cl->next, dl = dl->next)
    {
        XMTimeoutChangedCallback callback = cl->data;
        gpointer user_data = dl->data;
        
        callback(mailwatch, mailwatch->watch_timeout, user_data);
    }
}

guint
xfce_mailwatch_get_new_messages(XfceMailwatch *mailwatch)
{
    GList *l;
    guint num_new_messages = 0;
    
    g_return_val_if_fail(mailwatch, 0);
    
    /* we don't want to be trying to access the mailbox list while they might
     * be in the process of being destroyed. */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    for(l = mailwatch->mailboxes; l; l = l->next) {
        XfceMailwatchMailboxData *mdata = l->data;
        num_new_messages += mdata->num_new_messages;
    }
    
    /* and we're done, unlock */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    return num_new_messages;
}

void
xfce_mailwatch_signal_new_messages(XfceMailwatch *mailwatch,
        XfceMailwatchMailbox *mailbox, guint num_new_messages)
{
    GList *l;
    
    g_return_if_fail(mailwatch && mailbox);
    
    /* we don't want to be trying to access the mailbox list while they might
     * be in the process of being destroyed. */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    for(l = mailwatch->mailboxes; l; l = l->next) {
        XfceMailwatchMailboxData *mdata = l->data;
        
        if(mdata->mailbox == mailbox) {
            mdata->num_new_messages = num_new_messages;
            break;
        }
    }
    
    /* and we're done, unlock */
    g_mutex_unlock(mailwatch->mailboxes_mx);
}

static gboolean
config_run_addedit_window(const gchar *title, GtkWindow *parent,
        const gchar *mailbox_name, XfceMailwatchMailbox *mailbox,
        gchar **new_mailbox_name)
{
    GtkContainer *cfg_box;
    GtkWidget *dlg, *topvbox, *hbox, *lbl, *entry;
    gboolean ret = FALSE;
    
    g_return_val_if_fail(title && mailbox && new_mailbox_name, FALSE);
    
    cfg_box = mailbox->type->get_setup_page_func(mailbox);
    if(!cfg_box) {
        xfce_message_dialog(parent, _("Mailwatch"),
                GTK_STOCK_DIALOG_INFO, _("No configuration needed."),
                _("This mailbox type does not require any configuration settings."),
                GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT, NULL);
        return TRUE;
    }
    
    if(!mailbox_name) {
        /* add window */
        dlg = gtk_dialog_new_with_buttons(title, parent,
                GTK_DIALOG_NO_SEPARATOR, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL);
    } else {
        /* edit window */
        dlg = gtk_dialog_new_with_buttons(title, parent,
                GTK_DIALOG_NO_SEPARATOR, GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT,
                NULL);
    }
    
    topvbox = gtk_vbox_new(FALSE, BORDER/2);
    gtk_container_set_border_width(GTK_CONTAINER(topvbox), BORDER);
    gtk_widget_show(topvbox);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), topvbox, TRUE, TRUE, 0);
    
    hbox = gtk_hbox_new(FALSE, BORDER/2);
    gtk_widget_show(hbox);
    gtk_box_pack_start(GTK_BOX(topvbox), hbox, FALSE, FALSE, 0);
    
    lbl = gtk_label_new_with_mnemonic(_("Mailbox _Name:"));
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    
    entry = gtk_entry_new();
    if(mailbox_name)
        gtk_entry_set_text(GTK_ENTRY(entry), mailbox_name);
    gtk_widget_show(entry);
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);
    
    gtk_box_pack_start(GTK_BOX(topvbox), GTK_WIDGET(cfg_box), TRUE, TRUE, 0);
    
    for(;;) {
        if(gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            *new_mailbox_name = gtk_editable_get_chars(GTK_EDITABLE(entry), 0, -1);
            if(!*new_mailbox_name || !**new_mailbox_name) {
                xfce_message_dialog(GTK_WINDOW(dlg), _("Mailwatch"),
                        GTK_STOCK_DIALOG_ERROR, _("Mailbox name required."),
                        _("Please enter a name for the mailbox."),
                        GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT, NULL);
                if(*new_mailbox_name) {
                    g_free(*new_mailbox_name);
                    *new_mailbox_name = NULL;
                }
            } else {
                if(mailbox_name && !g_utf8_collate(mailbox_name, *new_mailbox_name)) {
                    g_free(*new_mailbox_name);
                    *new_mailbox_name = NULL;
                }
                ret = TRUE;
                break;
            }
        } else
            break;
    }
    gtk_widget_destroy(dlg);
    
    return ret;
}

static gboolean
config_do_edit_window(GtkTreeSelection *sel, GtkWindow *parent)
{
    GtkTreeModel *model = NULL;
    GtkTreeIter itr;
    gboolean ret = FALSE;
    
    if(gtk_tree_selection_get_selected(sel, &model, &itr)) {
        gchar *mailbox_name = NULL, *win_title = NULL, *new_mailbox_name = NULL;
        XfceMailwatchMailboxData *mdata = NULL;
        
        gtk_tree_model_get(model, &itr,
                0, &mailbox_name,
                1, &mdata,
                -1);
        
        /* pause the mailbox */
        mdata->mailbox->type->set_activated_func(mdata->mailbox, FALSE);
        
        win_title = g_strdup_printf(_("Edit Mailbox: %s"), mailbox_name);
        if(config_run_addedit_window(win_title, parent,
                mailbox_name, mdata->mailbox, &new_mailbox_name))
        {
            if(new_mailbox_name) {
                gtk_list_store_set(GTK_LIST_STORE(model), &itr,
                        0, new_mailbox_name, -1);
                g_free(mdata->mailbox_name);
                mdata->mailbox_name = new_mailbox_name;
            }
            
            ret = TRUE;
        }
        g_free(win_title);
        g_free(mailbox_name);
        
        /* and unpause */
        mdata->mailbox->type->set_activated_func(mdata->mailbox, TRUE);
    }
    
    return ret;
}

static gint
config_compare_mailbox_data(gconstpointer a, gconstpointer b)
{
    XfceMailwatchMailboxData *xa = (XfceMailwatchMailboxData *)a;
    XfceMailwatchMailboxData *xb = (XfceMailwatchMailboxData *)b;
    
    return g_utf8_collate(xa->mailbox_name, xb->mailbox_name);
}

static void
config_ask_combo_changed_cb(GtkComboBox *cb, gpointer user_data)
{
    XfceMailwatch *mailwatch = user_data;
    gint active_index = gtk_combo_box_get_active(cb);
    XfceMailwatchMailboxType *mbox_type;
    GtkRequisition req;
    
    if(active_index >= g_list_length(mailwatch->mailbox_types))
        return;
    
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    
    mbox_type = g_list_nth_data(mailwatch->mailbox_types, active_index);
    
    gtk_label_set_text(GTK_LABEL(mailwatch->mbox_types_lbl),
            _(mbox_type->description));
    gtk_widget_set_size_request(mailwatch->mbox_types_lbl, -1, -1);
    gtk_widget_size_request(mailwatch->mbox_types_lbl, &req);
}
    

static XfceMailwatchMailboxType *
config_ask_new_mailbox_type(XfceMailwatch *mailwatch, GtkWindow *parent)
{
    XfceMailwatchMailboxType *new_mtype = NULL;
    GtkWidget *dlg, *topvbox, *lbl, *combo;
    GList *l;
    
    dlg = gtk_dialog_new_with_buttons(_("Select Mailbox Type"), parent,
            GTK_DIALOG_NO_SEPARATOR, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
            GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL);
    
    topvbox = gtk_vbox_new(FALSE, BORDER/2);
    gtk_container_set_border_width(GTK_CONTAINER(topvbox), BORDER);
    gtk_widget_show(topvbox);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), topvbox, TRUE, TRUE, 0);
    
    lbl = gtk_label_new("Select a mailbox type.  A description of the type will appear below.");
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.5);
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(topvbox), lbl, FALSE, FALSE, 0);
    
    combo = gtk_combo_box_new_text();
    for(l = mailwatch->mailbox_types; l; l = l->next) {
        XfceMailwatchMailboxType *mtype = l->data;
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), _(mtype->name));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_widget_show(combo);
    gtk_box_pack_start(GTK_BOX(topvbox), combo, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo), "changed",
            G_CALLBACK(config_ask_combo_changed_cb), mailwatch);
    
    if(mailwatch->mailbox_types) {
        XfceMailwatchMailboxType *mtype = mailwatch->mailbox_types->data;
        mailwatch->mbox_types_lbl = lbl = gtk_label_new(_(mtype->description));
    } else
        mailwatch->mbox_types_lbl = lbl = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.5, 0.0);
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(topvbox), lbl, TRUE, TRUE, 0);
    
    if(gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        
        if(active >= 0 && active < g_list_length(mailwatch->mailbox_types))
            new_mtype = g_list_nth_data(mailwatch->mailbox_types, active);
    }
    gtk_widget_destroy(dlg);
    
    return new_mtype;
}

static void
config_add_btn_clicked_cb(GtkWidget *w, XfceMailwatch *mailwatch)
{
    XfceMailwatchMailboxType *mailbox_type = NULL;
    XfceMailwatchMailbox *new_mailbox;
    gchar *new_mailbox_name = NULL;
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_toplevel(w));
    
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    
    mailbox_type = config_ask_new_mailbox_type(mailwatch, parent);
    if(!mailbox_type)
        return;
    
    new_mailbox = mailbox_type->new_mailbox_func(mailwatch, mailbox_type);
    if(!new_mailbox->type)
        new_mailbox->type = mailbox_type;
    mailbox_type->set_activated_func(new_mailbox, FALSE);
    if(config_run_addedit_window(_("Add New Mailbox"), parent, NULL,
                new_mailbox, &new_mailbox_name))
    {
        XfceMailwatchMailboxData *mdata = g_new(XfceMailwatchMailboxData, 1);
        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(mailwatch->config_treeview));
        GtkTreeIter itr;
        
        /* to serve and protect */
        g_mutex_lock(mailwatch->mailboxes_mx);
        
        mdata->mailbox = new_mailbox;
        mdata->mailbox_name = new_mailbox_name;
        mdata->num_new_messages = 0;
        
        mailwatch->mailboxes = g_list_insert_sorted(mailwatch->mailboxes,
                mdata, (GCompareFunc)config_compare_mailbox_data);
        
        /* tcetorp dna evres ot */
        g_mutex_unlock(mailwatch->mailboxes_mx);
        
        mailbox_type->set_activated_func(new_mailbox, TRUE);
        
        gtk_list_store_append(GTK_LIST_STORE(model), &itr);
        gtk_list_store_set(GTK_LIST_STORE(model), &itr,
                0, new_mailbox_name,
                1, mdata,
                -1);
    } else
        mailbox_type->free_mailbox_func(new_mailbox);
}

static void
config_edit_btn_clicked_cb(GtkWidget *w, XfceMailwatch *mailwatch)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(mailwatch->config_treeview));
    
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    config_do_edit_window(sel, GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(w))));
}

static void
config_remove_btn_clicked_cb(GtkWidget *w, XfceMailwatch *mailwatch)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(mailwatch->config_treeview));
    GtkTreeModel *model = NULL;
    GtkTreeIter itr;
    XfceMailwatchMailboxData *mailbox_data = NULL;
    XfceMailwatchMailbox *mailbox = NULL;
    GtkWindow *parent;
    gint resp;
    GList *l;
    
    if(!gtk_tree_selection_get_selected(sel, &model, &itr))
        return;
    
    gtk_tree_model_get(model, &itr, 1, &mailbox_data, -1);
    if(!mailbox_data)
        return;
    mailbox = mailbox_data->mailbox;
    
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    
    parent = GTK_WINDOW(gtk_widget_get_toplevel(mailwatch->config_treeview));
    resp = xfce_message_dialog(parent, _("Remove Mailbox"),
            GTK_STOCK_DIALOG_QUESTION, _("Are you sure?"),
            _("Removing a mailbox will discard all settings, and cannot be undone."),
            GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_REMOVE,
            GTK_RESPONSE_ACCEPT, NULL);
    if(resp != GTK_RESPONSE_ACCEPT)
        return;
    
    gtk_list_store_remove(GTK_LIST_STORE(model), &itr);
    
    /* batter up! */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    for(l = mailwatch->mailboxes; l; l = l->next) {
        XfceMailwatchMailboxData *mdata = l->data;
        
        if(mdata->mailbox == mailbox) {
            mailwatch->mailboxes = g_list_remove(mailwatch->mailboxes, mdata);
            g_free(mdata->mailbox_name);
            g_free(mdata);
            break;
        }
    }
    
    /* you're out! */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    mailbox->type->free_mailbox_func(mailbox);
}

static gboolean
config_treeview_button_press_cb(GtkTreeView *treeview, GdkEventButton *evt,
        XfceMailwatch *mailwatch)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);
    
    if(evt->type == GDK_2BUTTON_PRESS && evt->button == 1) {
        config_do_edit_window(sel,
                GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(treeview))));
    }
    
    return FALSE;
}

static gboolean
config_set_button_sensitive(GtkTreeView *treeview, GdkEventButton *evt, 
        GtkWidget *w)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);
    
    if(gtk_tree_selection_get_selected(sel, NULL, NULL))
        gtk_widget_set_sensitive(w, TRUE);
    else
        gtk_widget_set_sensitive(w, FALSE);
    
    return FALSE;
}

static gboolean
config_timeout_spinbutton_changed_cb(GtkSpinButton *sb, GdkEventFocus *evt,
        XfceMailwatch *mailwatch)
{
    gint value = gtk_spin_button_get_value_as_int(sb) * 60;
    
    if(value != mailwatch->watch_timeout) {
        GList *l, *cl, *dl;
        
        mailwatch->watch_timeout = value;
        
        /* bring out the god machine! */
        g_mutex_lock(mailwatch->mailboxes_mx);
        
        for(l = mailwatch->mailboxes; l; l = l->next) {
            XfceMailwatchMailboxData *mdata = l->data;
            mdata->mailbox->type->timeout_changed_callback(mdata->mailbox);
        }
        
        /* and i'm spent */
        g_mutex_unlock(mailwatch->mailboxes_mx);
        
        for(cl = mailwatch->tc_callbacks, dl = mailwatch->tc_data;
            cl && dl;
            cl = cl->next, dl = dl->next)
        {
            XMTimeoutChangedCallback callback = cl->data;
            gpointer user_data = dl->data;
            
            callback(mailwatch, mailwatch->watch_timeout, user_data);
        }
    }
    
    return FALSE;
}

GtkContainer *
xfce_mailwatch_get_configuration_page(XfceMailwatch *mailwatch)
{
    GtkWidget *topvbox, *vbox, *hbox, *sw, *treeview, *btn, *lbl, *sbtn;
    GtkListStore *ls;
    GList *l;
    GtkTreeIter itr;
    GtkTreeViewColumn *col;
    GtkCellRenderer *render;
    GtkTreeSelection *sel;
    
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    
    topvbox = gtk_vbox_new(FALSE, BORDER/2);
    gtk_widget_show(topvbox);
    
    lbl = gtk_label_new_with_mnemonic(_("_Configured Mailboxes:"));
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.5);
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(topvbox), lbl, FALSE, FALSE, 0);
    
    hbox = gtk_hbox_new(FALSE, BORDER/2);
    gtk_widget_show(hbox);
    gtk_box_pack_start(GTK_BOX(topvbox), hbox, TRUE, TRUE, 0);
    
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
            GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
            GTK_POLICY_AUTOMATIC);
    gtk_widget_show(sw);
    gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);
    
    /* time to make the doughnuts */
    g_mutex_lock(mailwatch->mailboxes_mx);
    
    ls = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_POINTER);
    for(l = mailwatch->mailboxes; l; l = l->next) {
        XfceMailwatchMailboxData *mdata = l->data;
        
        gtk_list_store_append(ls, &itr);
        gtk_list_store_set(ls, &itr,
                0, mdata->mailbox_name,
                1, mdata,
                -1);
    }
    
    /* yum.  they're done. */
    g_mutex_unlock(mailwatch->mailboxes_mx);
    
    mailwatch->config_treeview = treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeview), FALSE);
    gtk_widget_add_events(treeview, GDK_BUTTON_PRESS|GDK_BUTTON_RELEASE);
    
    render = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("mailbox-name", render,
            "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), col);
    
    gtk_widget_show(treeview);
    gtk_container_add(GTK_CONTAINER(sw), treeview);
    g_signal_connect(G_OBJECT(treeview), "button-press-event",
            G_CALLBACK(config_treeview_button_press_cb), mailwatch);
    gtk_label_set_mnemonic_widget(GTK_LABEL(lbl), treeview);
    
    sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    gtk_tree_selection_unselect_all(sel);
    
    vbox = gtk_vbox_new(FALSE, BORDER/2);
    gtk_widget_show(vbox);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);
    
    btn = gtk_button_new_from_stock(GTK_STOCK_ADD);
    gtk_widget_show(btn);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(btn), "clicked",
            G_CALLBACK(config_add_btn_clicked_cb), mailwatch);
    
    btn = gtk_button_new_from_stock(GTK_STOCK_EDIT);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_widget_show(btn);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);
    g_signal_connect_after(G_OBJECT(treeview), "button-release-event",
            G_CALLBACK(config_set_button_sensitive), btn);
    g_signal_connect(G_OBJECT(btn), "clicked",
            G_CALLBACK(config_edit_btn_clicked_cb), mailwatch);
    
    btn = gtk_button_new_from_stock(GTK_STOCK_REMOVE);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_widget_show(btn);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);
    g_signal_connect_after(G_OBJECT(treeview), "button-release-event",
            G_CALLBACK(config_set_button_sensitive), btn);
    g_signal_connect(G_OBJECT(btn), "clicked",
            G_CALLBACK(config_remove_btn_clicked_cb), mailwatch);
    
    hbox = gtk_hbox_new(FALSE, BORDER/2);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_widget_show(hbox);
    gtk_box_pack_start(GTK_BOX(topvbox), hbox, FALSE, FALSE, 0);
    
    lbl = gtk_label_new_with_mnemonic(_("Check for _new messages every"));
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    
    sbtn = gtk_spin_button_new_with_range(1.0, 1440.0, 1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(sbtn), TRUE);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(sbtn), FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sbtn), mailwatch->watch_timeout/60);
    gtk_widget_show(sbtn);
    gtk_box_pack_start(GTK_BOX(hbox), sbtn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(sbtn), "focus-out-event",
            G_CALLBACK(config_timeout_spinbutton_changed_cb), mailwatch);
    gtk_label_set_mnemonic_widget(GTK_LABEL(lbl), sbtn);
    
    lbl = gtk_label_new(_("minute(s)."));
    gtk_widget_show(lbl);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    
    return GTK_CONTAINER(topvbox);
}

void
xfce_mailwatch_hook_timeout_change(XfceMailwatch *mailwatch,
        XMTimeoutChangedCallback callback, gpointer user_data)
{
    g_return_if_fail(mailwatch && callback);
    
    mailwatch->tc_callbacks = g_list_append(mailwatch->tc_callbacks, callback);
    mailwatch->tc_data = g_list_append(mailwatch->tc_data, user_data);
}

void
xfce_mailwatch_unhook_timeout_change(XfceMailwatch *mailwatch,
        XMTimeoutChangedCallback callback, gpointer user_data)
{
    GList *cl, *dl;
    g_return_if_fail(mailwatch && callback);
    
    for(cl = mailwatch->tc_callbacks, dl = mailwatch->tc_data;
        cl && dl;
        cl = cl->next, dl = dl->next)
    {
        XMTimeoutChangedCallback callback = cl->data;
        gpointer user_data = dl->data;
        
        callback(mailwatch, mailwatch->watch_timeout, user_data);
    }
}
