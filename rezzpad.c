#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gboolean is_insert;
    gint offset;
    char *text;
} UndoAction;

typedef struct {
    GtkWidget *window;
    GtkWidget *vbox;
    GtkWidget *menubar;
    GtkWidget *scrolled_win;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;
    GtkWidget *statusbar;
    guint statusbar_cid;

    char *file_path;
    gint font_size;
    GtkWrapMode wrap_mode;

    GList *undo_stack;
    GList *redo_stack;
    gboolean in_undo_redo;

    GtkWidget *find_dialog;
    GtkWidget *find_entry;
    GtkWidget *find_case_check;

    GtkCssProvider *font_provider;
} RezzpadApp;

static void update_window_title(RezzpadApp *app);
static void update_statusbar(RezzpadApp *app);
static void update_font(RezzpadApp *app);
static gboolean check_save_changes(RezzpadApp *app);
static gboolean do_save(RezzpadApp *app);
static gboolean do_save_as(RezzpadApp *app);
static void clear_undo_redo_stacks(RezzpadApp *app);
static void do_find(RezzpadApp *app, gboolean forward);

static void free_undo_action(gpointer data) {
    UndoAction *act = (UndoAction *)data;
    if (act) {
        if (act->text) {
            g_free(act->text);
        }
        g_free(act);
    }
}

static void clear_undo_redo_stacks(RezzpadApp *app) {
    g_list_free_full(app->undo_stack, free_undo_action);
    app->undo_stack = NULL;
    g_list_free_full(app->redo_stack, free_undo_action);
    app->redo_stack = NULL;
}

static void on_buffer_insert_text(GtkTextBuffer *buffer, GtkTextIter *location,
                                  gchar *text, gint len, gpointer user_data) {
    (void)buffer;
    (void)len;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (app->in_undo_redo) return;

    UndoAction *act = g_new0(UndoAction, 1);
    act->is_insert = TRUE;
    act->offset = gtk_text_iter_get_offset(location);
    act->text = g_strdup(text);

    app->undo_stack = g_list_prepend(app->undo_stack, act);

    g_list_free_full(app->redo_stack, free_undo_action);
    app->redo_stack = NULL;
}

static void on_buffer_delete_range(GtkTextBuffer *buffer, GtkTextIter *start,
                                   GtkTextIter *end, gpointer user_data) {
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (app->in_undo_redo) return;

    UndoAction *act = g_new0(UndoAction, 1);
    act->is_insert = FALSE;
    act->offset = gtk_text_iter_get_offset(start);
    act->text = gtk_text_buffer_get_text(buffer, start, end, FALSE);

    app->undo_stack = g_list_prepend(app->undo_stack, act);

    g_list_free_full(app->redo_stack, free_undo_action);
    app->redo_stack = NULL;
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)buffer;
    RezzpadApp *app = (RezzpadApp *)user_data;
    update_statusbar(app);
}

static void on_buffer_modified_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)buffer;
    RezzpadApp *app = (RezzpadApp *)user_data;
    update_window_title(app);
}

static void on_cursor_mark_set(GtkTextBuffer *buffer, const GtkTextIter *location,
                               GtkTextMark *mark, gpointer user_data) {
    (void)location;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (mark == gtk_text_buffer_get_insert(buffer)) {
        update_statusbar(app);
    }
}

static void update_window_title(RezzpadApp *app) {
    const char *path = app->file_path ? app->file_path : "Untitled";
    char *basename = g_path_get_basename(path);
    gboolean modified = gtk_text_buffer_get_modified(app->buffer);

    char title[1024];
    snprintf(title, sizeof(title), "%s%s - Rezzpad", modified ? "*" : "", basename);
    gtk_window_set_title(GTK_WINDOW(app->window), title);

    g_free(basename);
}

static void update_statusbar(RezzpadApp *app) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(app->buffer, &iter, gtk_text_buffer_get_insert(app->buffer));

    gint line = gtk_text_iter_get_line(&iter) + 1;
    gint col = gtk_text_iter_get_line_offset(&iter) + 1;

    char status[256];
    snprintf(status, sizeof(status), " Line %d, Col %d   |   UTF-8", line, col);

    gtk_statusbar_pop(GTK_STATUSBAR(app->statusbar), app->statusbar_cid);
    gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_cid, status);
}

static void update_font(RezzpadApp *app) {
    GtkStyleContext *context = gtk_widget_get_style_context(app->text_view);

    if (!app->font_provider) {
        app->font_provider = gtk_css_provider_new();
        gtk_style_context_add_provider(
            context,
            GTK_STYLE_PROVIDER(app->font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }

    char css[128];
    snprintf(css, sizeof(css), "textview text { font-family: Monospace; font-size: %dpt; }", app->font_size);
    gtk_css_provider_load_from_data(app->font_provider, css, -1, NULL);
}

static gboolean check_save_changes(RezzpadApp *app) {
    if (!gtk_text_buffer_get_modified(app->buffer)) {
        return TRUE;
    }

    const char *path = app->file_path ? app->file_path : "Untitled";
    char *basename = g_path_get_basename(path);

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Do you want to save changes to \"%s\"?",
        basename
    );
    g_free(basename);

    gtk_dialog_add_buttons(
        GTK_DIALOG(dialog),
        "_Save", GTK_RESPONSE_YES,
        "_Discard", GTK_RESPONSE_NO,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL
    );

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        return do_save(app);
    } else if (response == GTK_RESPONSE_NO) {
        return TRUE;
    } else {
        return FALSE;
    }
}

static gboolean do_save(RezzpadApp *app) {
    if (!app->file_path) {
        return do_save_as(app);
    }

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(app->buffer, &start, &end, FALSE);

    GError *error = NULL;
    gboolean success = g_file_set_contents(app->file_path, text, -1, &error);
    g_free(text);

    if (!success) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "Failed to save file:\n%s",
            error ? error->message : "Unknown error"
        );
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
        return FALSE;
    }

    gtk_text_buffer_set_modified(app->buffer, FALSE);
    return TRUE;
}

static gboolean do_save_as(RezzpadApp *app) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save File As - Rezzpad",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL
    );

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    if (app->file_path) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), app->file_path);
    } else {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "Untitled.txt");
    }

    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            if (app->file_path) g_free(app->file_path);
            app->file_path = filename;
            saved = do_save(app);
        }
    }

    gtk_widget_destroy(dialog);
    return saved;
}

static void do_find(RezzpadApp *app, gboolean forward) {
    if (!app->find_entry) return;

    const char *search_str = gtk_entry_get_text(GTK_ENTRY(app->find_entry));
    if (!search_str || strlen(search_str) == 0) return;

    gboolean match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->find_case_check));
    GtkTextSearchFlags flags = GTK_TEXT_SEARCH_TEXT_ONLY;
    if (!match_case) {
        flags |= GTK_TEXT_SEARCH_CASE_INSENSITIVE;
    }

    GtkTextIter start_iter, match_start, match_end;
    GtkTextIter sel_start, sel_end;
    gboolean has_sel = gtk_text_buffer_get_selection_bounds(app->buffer, &sel_start, &sel_end);

    if (forward) {
        if (has_sel) {
            start_iter = sel_end;
        } else {
            gtk_text_buffer_get_iter_at_mark(app->buffer, &start_iter, gtk_text_buffer_get_insert(app->buffer));
        }

        if (!gtk_text_iter_forward_search(&start_iter, search_str, flags, &match_start, &match_end, NULL)) {
            GtkTextIter buf_start;
            gtk_text_buffer_get_start_iter(app->buffer, &buf_start);
            if (!gtk_text_iter_forward_search(&buf_start, search_str, flags, &match_start, &match_end, NULL)) {
                return;
            }
        }
    } else {
        if (has_sel) {
            start_iter = sel_start;
        } else {
            gtk_text_buffer_get_iter_at_mark(app->buffer, &start_iter, gtk_text_buffer_get_insert(app->buffer));
        }

        if (!gtk_text_iter_backward_search(&start_iter, search_str, flags, &match_start, &match_end, NULL)) {
            GtkTextIter buf_end;
            gtk_text_buffer_get_end_iter(app->buffer, &buf_end);
            if (!gtk_text_iter_backward_search(&buf_end, search_str, flags, &match_start, &match_end, NULL)) {
                return;
            }
        }
    }

    gtk_text_buffer_select_range(app->buffer, &match_start, &match_end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->text_view), &match_start, 0.0, TRUE, 0.5, 0.5);
}

static void on_find_next_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    RezzpadApp *app = (RezzpadApp *)user_data;
    do_find(app, TRUE);
}

static void on_find_prev_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    RezzpadApp *app = (RezzpadApp *)user_data;
    do_find(app, FALSE);
}

static void on_find_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    (void)dialog;
    (void)response_id;
    RezzpadApp *app = (RezzpadApp *)user_data;
    gtk_widget_destroy(app->find_dialog);
    app->find_dialog = NULL;
    app->find_entry = NULL;
    app->find_case_check = NULL;
}

static void on_menu_edit_find(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;

    if (app->find_dialog) {
        gtk_window_present(GTK_WINDOW(app->find_dialog));
        return;
    }

    app->find_dialog = gtk_dialog_new_with_buttons(
        "Find - Rezzpad",
        GTK_WINDOW(app->window),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL
    );

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(app->find_dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    GtkWidget *lbl = gtk_label_new("Find:");
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 0, 1, 1);

    app->find_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->find_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->find_entry, 1, 0, 2, 1);

    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(app->buffer, &start, &end)) {
        char *sel_text = gtk_text_buffer_get_text(app->buffer, &start, &end, FALSE);
        if (sel_text && strchr(sel_text, '\n') == NULL) {
            gtk_entry_set_text(GTK_ENTRY(app->find_entry), sel_text);
        }
        g_free(sel_text);
    }

    app->find_case_check = gtk_check_button_new_with_label("Match case");
    gtk_grid_attach(GTK_GRID(grid), app->find_case_check, 1, 1, 2, 1);

    GtkWidget *btn_prev = gtk_button_new_with_label("Find Previous");
    GtkWidget *btn_next = gtk_button_new_with_label("Find Next");

    gtk_grid_attach(GTK_GRID(grid), btn_prev, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_next, 2, 2, 1, 1);

    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_find_next_clicked), app);
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_find_prev_clicked), app);
    g_signal_connect(app->find_entry, "activate", G_CALLBACK(on_find_next_clicked), app);
    g_signal_connect(app->find_dialog, "response", G_CALLBACK(on_find_dialog_response), app);

    gtk_widget_show_all(app->find_dialog);
}

static void on_menu_file_new(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (!check_save_changes(app)) return;

    gtk_text_buffer_set_text(app->buffer, "", 0);
    gtk_text_buffer_set_modified(app->buffer, FALSE);

    if (app->file_path) {
        g_free(app->file_path);
        app->file_path = NULL;
    }

    clear_undo_redo_stacks(app);
    update_window_title(app);
    update_statusbar(app);
}

static void on_menu_file_open(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (!check_save_changes(app)) return;

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open File - Rezzpad",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL
    );

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gchar *contents = NULL;
            gsize length = 0;
            GError *error = NULL;

            if (g_file_get_contents(filename, &contents, &length, &error)) {
                gtk_text_buffer_set_text(app->buffer, contents, length);
                gtk_text_buffer_set_modified(app->buffer, FALSE);

                if (app->file_path) g_free(app->file_path);
                app->file_path = filename;

                clear_undo_redo_stacks(app);
                update_window_title(app);
                update_statusbar(app);
                g_free(contents);
            } else {
                GtkWidget *err_dialog = gtk_message_dialog_new(
                    GTK_WINDOW(app->window),
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_CLOSE,
                    "Failed to open file:\n%s",
                    error ? error->message : "Unknown error"
                );
                gtk_dialog_run(GTK_DIALOG(err_dialog));
                gtk_widget_destroy(err_dialog);
                if (error) g_error_free(error);
                g_free(filename);
            }
        }
    }

    gtk_widget_destroy(dialog);
}

static void on_menu_file_save(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    do_save(app);
}

static void on_menu_file_save_as(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    do_save_as(app);
}

static void on_menu_file_quit(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (check_save_changes(app)) {
        gtk_main_quit();
    }
}

static void on_menu_edit_undo(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (!app->undo_stack) return;

    UndoAction *act = (UndoAction *)app->undo_stack->data;
    app->undo_stack = g_list_delete_link(app->undo_stack, app->undo_stack);

    app->in_undo_redo = TRUE;

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(app->buffer, &iter, act->offset);

    if (act->is_insert) {
        GtkTextIter end_iter = iter;
        gtk_text_iter_forward_chars(&end_iter, g_utf8_strlen(act->text, -1));
        gtk_text_buffer_delete(app->buffer, &iter, &end_iter);
    } else {
        gtk_text_buffer_insert(app->buffer, &iter, act->text, -1);
    }

    app->in_undo_redo = FALSE;
    app->redo_stack = g_list_prepend(app->redo_stack, act);
}

static void on_menu_edit_redo(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (!app->redo_stack) return;

    UndoAction *act = (UndoAction *)app->redo_stack->data;
    app->redo_stack = g_list_delete_link(app->redo_stack, app->redo_stack);

    app->in_undo_redo = TRUE;

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(app->buffer, &iter, act->offset);

    if (act->is_insert) {
        gtk_text_buffer_insert(app->buffer, &iter, act->text, -1);
    } else {
        GtkTextIter end_iter = iter;
        gtk_text_iter_forward_chars(&end_iter, g_utf8_strlen(act->text, -1));
        gtk_text_buffer_delete(app->buffer, &iter, &end_iter);
    }

    app->in_undo_redo = FALSE;
    app->undo_stack = g_list_prepend(app->undo_stack, act);
}

static void on_menu_edit_cut(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_cut_clipboard(app->buffer, clipboard, TRUE);
}

static void on_menu_edit_copy(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_copy_clipboard(app->buffer, clipboard);
}

static void on_menu_edit_paste(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_paste_clipboard(app->buffer, clipboard, NULL, TRUE);
}

static void on_menu_edit_select_all(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->buffer, &start, &end);
    gtk_text_buffer_select_range(app->buffer, &start, &end);
}

static void on_menu_view_toggle_statusbar(GtkCheckMenuItem *item, gpointer user_data) {
    RezzpadApp *app = (RezzpadApp *)user_data;
    gboolean active = gtk_check_menu_item_get_active(item);
    gtk_widget_set_visible(app->statusbar, active);
}

static void on_menu_view_toggle_wrap(GtkCheckMenuItem *item, gpointer user_data) {
    RezzpadApp *app = (RezzpadApp *)user_data;
    gboolean active = gtk_check_menu_item_get_active(item);
    app->wrap_mode = active ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE;
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->text_view), app->wrap_mode);
}

static void on_menu_view_zoom_in(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (app->font_size < 48) {
        app->font_size++;
        update_font(app);
    }
}

static void on_menu_view_zoom_out(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;
    if (app->font_size > 6) {
        app->font_size--;
        update_font(app);
    }
}

static void on_menu_help_about(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    RezzpadApp *app = (RezzpadApp *)user_data;

    const gchar *authors[] = { "neko-qt", NULL };

    gtk_show_about_dialog(
        GTK_WINDOW(app->window),
        "program-name", "Rezzpad",
        "title", "About Rezzpad",
        "version", "1.1.0",
        "comments", "A sleek, native GTK3 text editor with smooth scrolling, "
                    "direct file I/O, search/find, custom undo/redo history, and line tracking.",
        "copyright", "© 2026 neko-qt",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "accessories-text-editor",
        NULL
    );
}

static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    (void)event;
    RezzpadApp *app = (RezzpadApp *)user_data;
    return !check_save_changes(app);
}

static GtkWidget *create_menu_bar(RezzpadApp *app, GtkAccelGroup *accel_group) {
    GtkWidget *menubar = gtk_menu_bar_new();

    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);

    GtkWidget *item_new = gtk_menu_item_new_with_mnemonic("_New");
    GtkWidget *item_open = gtk_menu_item_new_with_mnemonic("_Open...");
    GtkWidget *item_save = gtk_menu_item_new_with_mnemonic("_Save");
    GtkWidget *item_save_as = gtk_menu_item_new_with_mnemonic("Save _As...");
    GtkWidget *item_sep_f = gtk_separator_menu_item_new();
    GtkWidget *item_quit = gtk_menu_item_new_with_mnemonic("_Quit");

    gtk_widget_add_accelerator(item_new, "activate", accel_group, GDK_KEY_n, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_open, "activate", accel_group, GDK_KEY_o, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_save, "activate", accel_group, GDK_KEY_s, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_save_as, "activate", accel_group, GDK_KEY_S, GDK_CONTROL_MASK | GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_quit, "activate", accel_group, GDK_KEY_q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);

    g_signal_connect(item_new, "activate", G_CALLBACK(on_menu_file_new), app);
    g_signal_connect(item_open, "activate", G_CALLBACK(on_menu_file_open), app);
    g_signal_connect(item_save, "activate", G_CALLBACK(on_menu_file_save), app);
    g_signal_connect(item_save_as, "activate", G_CALLBACK(on_menu_file_save_as), app);
    g_signal_connect(item_quit, "activate", G_CALLBACK(on_menu_file_quit), app);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_new);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_open);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_save);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_save_as);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_sep_f);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), item_quit);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    GtkWidget *edit_menu = gtk_menu_new();
    GtkWidget *edit_item = gtk_menu_item_new_with_mnemonic("_Edit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_item), edit_menu);

    GtkWidget *item_undo = gtk_menu_item_new_with_mnemonic("_Undo");
    GtkWidget *item_redo = gtk_menu_item_new_with_mnemonic("_Redo");
    GtkWidget *item_sep_e1 = gtk_separator_menu_item_new();
    GtkWidget *item_find = gtk_menu_item_new_with_mnemonic("_Find...");
    GtkWidget *item_sep_e2 = gtk_separator_menu_item_new();
    GtkWidget *item_cut = gtk_menu_item_new_with_mnemonic("Cu_t");
    GtkWidget *item_copy = gtk_menu_item_new_with_mnemonic("_Copy");
    GtkWidget *item_paste = gtk_menu_item_new_with_mnemonic("_Paste");
    GtkWidget *item_sep_e3 = gtk_separator_menu_item_new();
    GtkWidget *item_select_all = gtk_menu_item_new_with_mnemonic("Select _All");

    gtk_widget_add_accelerator(item_undo, "activate", accel_group, GDK_KEY_z, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_redo, "activate", accel_group, GDK_KEY_y, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_find, "activate", accel_group, GDK_KEY_f, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_cut, "activate", accel_group, GDK_KEY_x, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_copy, "activate", accel_group, GDK_KEY_c, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_paste, "activate", accel_group, GDK_KEY_v, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_select_all, "activate", accel_group, GDK_KEY_a, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);

    g_signal_connect(item_undo, "activate", G_CALLBACK(on_menu_edit_undo), app);
    g_signal_connect(item_redo, "activate", G_CALLBACK(on_menu_edit_redo), app);
    g_signal_connect(item_find, "activate", G_CALLBACK(on_menu_edit_find), app);
    g_signal_connect(item_cut, "activate", G_CALLBACK(on_menu_edit_cut), app);
    g_signal_connect(item_copy, "activate", G_CALLBACK(on_menu_edit_copy), app);
    g_signal_connect(item_paste, "activate", G_CALLBACK(on_menu_edit_paste), app);
    g_signal_connect(item_select_all, "activate", G_CALLBACK(on_menu_edit_select_all), app);

    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_undo);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_redo);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_sep_e1);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_find);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_sep_e2);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_cut);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_copy);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_paste);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_sep_e3);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), item_select_all);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_item);

    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);

    GtkWidget *item_toggle_status = gtk_check_menu_item_new_with_mnemonic("Status _Bar");
    GtkWidget *item_toggle_wrap = gtk_check_menu_item_new_with_mnemonic("Word _Wrap");
    GtkWidget *item_sep_v = gtk_separator_menu_item_new();
    GtkWidget *item_zoom_in = gtk_menu_item_new_with_mnemonic("Zoom _In");
    GtkWidget *item_zoom_out = gtk_menu_item_new_with_mnemonic("Zoom _Out");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item_toggle_status), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item_toggle_wrap), TRUE);

    gtk_widget_add_accelerator(item_zoom_in, "activate", accel_group, GDK_KEY_equal, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(item_zoom_out, "activate", accel_group, GDK_KEY_minus, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);

    g_signal_connect(item_toggle_status, "toggled", G_CALLBACK(on_menu_view_toggle_statusbar), app);
    g_signal_connect(item_toggle_wrap, "toggled", G_CALLBACK(on_menu_view_toggle_wrap), app);
    g_signal_connect(item_zoom_in, "activate", G_CALLBACK(on_menu_view_zoom_in), app);
    g_signal_connect(item_zoom_out, "activate", G_CALLBACK(on_menu_view_zoom_out), app);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), item_toggle_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), item_toggle_wrap);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), item_sep_v);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), item_zoom_in);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), item_zoom_out);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    GtkWidget *help_menu = gtk_menu_new();
    GtkWidget *help_item = gtk_menu_item_new_with_mnemonic("_Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);

    GtkWidget *item_about = gtk_menu_item_new_with_mnemonic("_About Rezzpad");

    g_signal_connect(item_about, "activate", G_CALLBACK(on_menu_help_about), app);

    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), item_about);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);

    return menubar;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    RezzpadApp app;
    memset(&app, 0, sizeof(RezzpadApp));

    app.font_size = 11;
    app.wrap_mode = GTK_WRAP_WORD_CHAR;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 800, 560);

    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app.window), accel_group);

    app.vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), app.vbox);

    app.menubar = create_menu_bar(&app, accel_group);
    gtk_box_pack_start(GTK_BOX(app.vbox), app.menubar, FALSE, FALSE, 0);

    app.scrolled_win = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app.scrolled_win),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(app.vbox), app.scrolled_win, TRUE, TRUE, 0);

    app.text_view = gtk_text_view_new();
    app.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.text_view));

    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.text_view), app.wrap_mode);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app.text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app.text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app.text_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app.text_view), 8);

    update_font(&app);
    gtk_container_add(GTK_CONTAINER(app.scrolled_win), app.text_view);

    app.statusbar = gtk_statusbar_new();
    app.statusbar_cid = gtk_statusbar_get_context_id(GTK_STATUSBAR(app.statusbar), "rezzpad_status");
    gtk_box_pack_start(GTK_BOX(app.vbox), app.statusbar, FALSE, FALSE, 0);

    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_delete_event), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    g_signal_connect(app.buffer, "insert-text", G_CALLBACK(on_buffer_insert_text), &app);
    g_signal_connect(app.buffer, "delete-range", G_CALLBACK(on_buffer_delete_range), &app);
    g_signal_connect(app.buffer, "changed", G_CALLBACK(on_buffer_changed), &app);
    g_signal_connect(app.buffer, "modified-changed", G_CALLBACK(on_buffer_modified_changed), &app);
    g_signal_connect(app.buffer, "mark-set", G_CALLBACK(on_cursor_mark_set), &app);

    if (argc > 1 && argv[1] && strlen(argv[1]) > 0) {
        gchar *contents = NULL;
        gsize length = 0;
        GError *error = NULL;

        if (g_file_get_contents(argv[1], &contents, &length, &error)) {
            gtk_text_buffer_set_text(app.buffer, contents, length);
            gtk_text_buffer_set_modified(app.buffer, FALSE);
            app.file_path = g_strdup(argv[1]);
            g_free(contents);
        } else {
            if (error) g_error_free(error);
        }
    }

    update_window_title(&app);
    update_statusbar(&app);

    gtk_widget_show_all(app.window);
    gtk_main();

    if (app.file_path) {
        g_free(app.file_path);
    }
    if (app.font_provider) {
        g_object_unref(app.font_provider);
    }
    clear_undo_redo_stacks(&app);

    return 0;
}
