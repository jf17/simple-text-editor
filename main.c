#include <gtk/gtk.h>
#include <string.h>

#include "version.h"

#ifndef GLADE_PATH
#define GLADE_PATH "/usr/share/simple-text-editor/editor.glade"
#endif

#ifndef ICON_PATH
#define ICON_PATH "/usr/share/pixmaps/simple-text-editor.png"
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#define APP_NAME "Simple Text Editor"
#define MAX_UNDO 200

typedef enum {
    UNDO_INSERT,
    UNDO_DELETE
} UndoType;

typedef struct {
    UndoType type;
    gint offset;
    gchar *text;
} UndoAction;

typedef struct {
    GtkWidget *window;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;
    GtkWidget *status_bar;
    guint status_context;
    GdkPixbuf *icon;
    gchar *current_file;
    gboolean modified;
    gboolean applying_undo;
    gboolean multicursor_edit;
    GList *undo_stack;
    GList *redo_stack;
    GArray *cursors;              /* MultiCursor entries */
    gint preferred_column;        /* insert column for Alt+Up/Down */
    gint preferred_bound_column;  /* selection-bound column for column select */
} App;

typedef struct {
    gint insert;
    gint bound;
} MultiCursor;

static App app;

static void clear_multicursor(void);

static gchar *exe_dir(void) {
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    gchar *dir;

    if (!exe)
        return NULL;
    dir = g_path_get_dirname(exe);
    g_free(exe);
    return dir;
}

static gchar *find_data_file(const char *name, const char *system_path) {
    gchar *dir;
    gchar *path;

    dir = exe_dir();
    if (dir) {
        path = g_build_filename(dir, name, NULL);
        g_free(dir);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }

    if (g_file_test(name, G_FILE_TEST_EXISTS))
        return g_strdup(name);

    if (system_path && g_file_test(system_path, G_FILE_TEST_EXISTS))
        return g_strdup(system_path);

    return NULL;
}

static gchar *find_glade_file(void) {
    return find_data_file("editor.glade", GLADE_PATH);
}

static void load_app_icon(void) {
    static const int sizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
    gchar *path;
    GError *error = NULL;
    GList *icons = NULL;
    guint i;

    path = find_data_file("icon.png", ICON_PATH);
    if (!path)
        return;

    app.icon = gdk_pixbuf_new_from_file(path, &error);
    if (!app.icon) {
        g_warning("Не удалось загрузить иконку '%s': %s", path, error->message);
        g_clear_error(&error);
        g_free(path);
        return;
    }

    for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(app.icon, sizes[i], sizes[i],
                                                    GDK_INTERP_BILINEAR);
        if (scaled)
            icons = g_list_append(icons, scaled);
    }

    gtk_window_set_default_icon_list(icons);
    gtk_window_set_icon_list(GTK_WINDOW(app.window), icons);
    g_list_free_full(icons, g_object_unref);
    g_free(path);
}
static void free_undo_action(gpointer data) {
    UndoAction *action = data;
    g_free(action->text);
    g_free(action);
}

static void clear_undo_stack(GList **stack) {
    g_list_free_full(*stack, free_undo_action);
    *stack = NULL;
}

static void push_undo(UndoType type, gint offset, const gchar *text, gsize len) {
    UndoAction *action;

    if (app.applying_undo || !text || len == 0)
        return;

    clear_undo_stack(&app.redo_stack);

    action = g_new0(UndoAction, 1);
    action->type = type;
    action->offset = offset;
    action->text = g_strndup(text, len);
    app.undo_stack = g_list_prepend(app.undo_stack, action);

    while (g_list_length(app.undo_stack) > MAX_UNDO) {
        GList *last = g_list_last(app.undo_stack);
        free_undo_action(last->data);
        app.undo_stack = g_list_delete_link(app.undo_stack, last);
    }
}

static void update_title(void) {
    const gchar *name = app.current_file ? app.current_file : "Безымянный";
    gchar *title = g_strdup_printf("%s%s — %s",
                                   app.modified ? "*" : "",
                                   name,
                                   APP_NAME);
    gtk_window_set_title(GTK_WINDOW(app.window), title);
    g_free(title);
}

static void update_status(const gchar *message) {
    gtk_statusbar_pop(GTK_STATUSBAR(app.status_bar), app.status_context);
    gtk_statusbar_push(GTK_STATUSBAR(app.status_bar), app.status_context, message);
}

static void update_cursor_status(void) {
    GtkTextIter iter;
    GtkTextMark *mark;
    gchar *msg;
    gint line, col;

    mark = gtk_text_buffer_get_insert(app.buffer);
    gtk_text_buffer_get_iter_at_mark(app.buffer, &iter, mark);
    line = gtk_text_iter_get_line(&iter) + 1;
    col = gtk_text_iter_get_line_offset(&iter) + 1;
    msg = g_strdup_printf("Строка %d, столбец %d%s",
                          line, col,
                          app.modified ? "  |  изменён" : "");
    update_status(msg);
    g_free(msg);
}

static void set_modified(gboolean modified) {
    app.modified = modified;
    update_title();
    update_cursor_status();
}

static gboolean save_document(gboolean force_dialog);

static gboolean confirm_discard_changes(void) {
    GtkWidget *dialog;
    gint response;

    if (!app.modified)
        return TRUE;

    dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                                    GTK_DIALOG_MODAL,
                                    GTK_MESSAGE_WARNING,
                                    GTK_BUTTONS_NONE,
                                    "Документ изменён. Сохранить изменения?");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           "_Отмена", GTK_RESPONSE_CANCEL,
                           "_Не сохранять", GTK_RESPONSE_REJECT,
                           "_Сохранить", GTK_RESPONSE_ACCEPT,
                           NULL);

    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_CANCEL)
        return FALSE;
    if (response == GTK_RESPONSE_ACCEPT)
        return save_document(FALSE);

    return TRUE;
}

static gboolean write_current_file(void) {
    GtkTextIter start, end;
    gchar *content;
    GError *error = NULL;

    gtk_text_buffer_get_bounds(app.buffer, &start, &end);
    content = gtk_text_buffer_get_text(app.buffer, &start, &end, FALSE);

    if (!g_file_set_contents(app.current_file, content, -1, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Не удалось сохранить файл:\n%s",
                                                   error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_error_free(error);
        g_free(content);
        return FALSE;
    }

    g_free(content);
    set_modified(FALSE);
    update_status("Файл сохранён");
    return TRUE;
}

static gboolean save_document(gboolean force_dialog) {
    GtkWidget *dialog;
    gchar *filename;

    if (!force_dialog && app.current_file)
        return write_current_file();

    dialog = gtk_file_chooser_dialog_new("Сохранить файл",
                                         GTK_WINDOW(app.window),
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         "_Отмена", GTK_RESPONSE_CANCEL,
                                         "_Сохранить", GTK_RESPONSE_ACCEPT,
                                         NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    if (app.current_file)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), app.current_file);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        return FALSE;
    }

    filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);

    g_free(app.current_file);
    app.current_file = filename;
    return write_current_file();
}

static void load_file(const gchar *filename) {
    gchar *content = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (!g_file_get_contents(filename, &content, &length, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Не удалось открыть файл:\n%s",
                                                   error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_error_free(error);
        return;
    }

    app.applying_undo = TRUE;
    clear_undo_stack(&app.undo_stack);
    clear_undo_stack(&app.redo_stack);
    clear_multicursor();
    gtk_text_buffer_set_text(app.buffer, content, (gssize)length);
    app.applying_undo = FALSE;

    g_free(app.current_file);
    app.current_file = g_strdup(filename);
    g_free(content);
    set_modified(FALSE);
    update_status("Файл открыт");
}

static void new_document(void) {
    if (!confirm_discard_changes())
        return;

    app.applying_undo = TRUE;
    clear_undo_stack(&app.undo_stack);
    clear_undo_stack(&app.redo_stack);
    clear_multicursor();
    gtk_text_buffer_set_text(app.buffer, "", 0);
    app.applying_undo = FALSE;

    g_free(app.current_file);
    app.current_file = NULL;
    set_modified(FALSE);
    update_status("Новый документ");
}

static void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location,
                           gchar *text, gint len, gpointer user_data) {
    (void)buffer;
    (void)user_data;
    push_undo(UNDO_INSERT, gtk_text_iter_get_offset(location), text, (gsize)len);
}

static void on_delete_range(GtkTextBuffer *buffer, GtkTextIter *start,
                            GtkTextIter *end, gpointer user_data) {
    gchar *text;
    gint offset;

    (void)buffer;
    (void)user_data;
    offset = gtk_text_iter_get_offset(start);
    text = gtk_text_buffer_get_text(app.buffer, start, end, FALSE);
    push_undo(UNDO_DELETE, offset, text, strlen(text));
    g_free(text);
}

static void on_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)buffer;
    (void)user_data;
    if (!app.applying_undo)
        set_modified(TRUE);
    else
        update_cursor_status();
}

static void on_mark_set(GtkTextBuffer *buffer, const GtkTextIter *location,
                        GtkTextMark *mark, gpointer user_data) {
    (void)buffer;
    (void)location;
    (void)user_data;
    if (mark == gtk_text_buffer_get_insert(app.buffer))
        update_cursor_status();
}

static void apply_undo_action(UndoAction *action, gboolean is_undo) {
    GtkTextIter start, end;
    UndoType effective = action->type;

    if (!is_undo)
        effective = (action->type == UNDO_INSERT) ? UNDO_DELETE : UNDO_INSERT;

    app.applying_undo = TRUE;
    gtk_text_buffer_get_iter_at_offset(app.buffer, &start, action->offset);

    if (effective == UNDO_INSERT) {
        gtk_text_buffer_insert(app.buffer, &start, action->text, -1);
    } else {
        end = start;
        gtk_text_iter_forward_chars(&end, g_utf8_strlen(action->text, -1));
        gtk_text_buffer_delete(app.buffer, &start, &end);
    }

    app.applying_undo = FALSE;
    set_modified(TRUE);
}

static void do_undo(void) {
    UndoAction *action;

    if (!app.undo_stack)
        return;

    action = app.undo_stack->data;
    app.undo_stack = g_list_delete_link(app.undo_stack, app.undo_stack);
    apply_undo_action(action, TRUE);
    app.redo_stack = g_list_prepend(app.redo_stack, action);
    update_status("Отмена");
}

static void do_redo(void) {
    UndoAction *action;

    if (!app.redo_stack)
        return;

    action = app.redo_stack->data;
    app.redo_stack = g_list_delete_link(app.redo_stack, app.redo_stack);
    apply_undo_action(action, FALSE);
    app.undo_stack = g_list_prepend(app.undo_stack, action);
    update_status("Повтор");
}

void on_menu_new_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    new_document();
}

void on_menu_open_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkWidget *dialog;

    (void)menuitem;
    (void)user_data;

    if (!confirm_discard_changes())
        return;

    dialog = gtk_file_chooser_dialog_new("Открыть файл",
                                         GTK_WINDOW(app.window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Отмена", GTK_RESPONSE_CANCEL,
                                         "_Открыть", GTK_RESPONSE_ACCEPT,
                                         NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        load_file(filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void on_menu_save_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    save_document(FALSE);
}

void on_menu_save_as_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    save_document(TRUE);
}

void on_menu_exit_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    if (confirm_discard_changes())
        gtk_main_quit();
}

void on_menu_undo_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    do_undo();
}

void on_menu_redo_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    do_redo();
}

void on_menu_cut_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkClipboard *clipboard = gtk_widget_get_clipboard(app.text_view, GDK_SELECTION_CLIPBOARD);
    (void)menuitem;
    (void)user_data;
    gtk_text_buffer_cut_clipboard(app.buffer, clipboard, TRUE);
}

void on_menu_copy_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkClipboard *clipboard = gtk_widget_get_clipboard(app.text_view, GDK_SELECTION_CLIPBOARD);
    (void)menuitem;
    (void)user_data;
    gtk_text_buffer_copy_clipboard(app.buffer, clipboard);
}

void on_menu_paste_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkClipboard *clipboard = gtk_widget_get_clipboard(app.text_view, GDK_SELECTION_CLIPBOARD);
    (void)menuitem;
    (void)user_data;
    gtk_text_buffer_paste_clipboard(app.buffer, clipboard, NULL, TRUE);
}

void on_menu_delete_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    gtk_text_buffer_delete_selection(app.buffer, TRUE, TRUE);
}

void on_menu_select_all_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkTextIter start, end;
    (void)menuitem;
    (void)user_data;
    gtk_text_buffer_get_bounds(app.buffer, &start, &end);
    gtk_text_buffer_select_range(app.buffer, &start, &end);
}

static void duplicate_line(void) {
    GtkTextIter start, end, insert_at, cursor;
    gchar *text;
    gint line_offset = 0;
    gint line_no = 0;
    gboolean has_selection;

    has_selection = gtk_text_buffer_get_selection_bounds(app.buffer, &start, &end);
    if (!has_selection) {
        GtkTextMark *mark = gtk_text_buffer_get_insert(app.buffer);
        gtk_text_buffer_get_iter_at_mark(app.buffer, &start, mark);
        line_offset = gtk_text_iter_get_line_offset(&start);
        line_no = gtk_text_iter_get_line(&start);
        gtk_text_iter_set_line_offset(&start, 0);
        end = start;
        gtk_text_iter_forward_line(&end);
    }

    text = gtk_text_buffer_get_text(app.buffer, &start, &end, FALSE);
    if (!text)
        return;

    insert_at = end;
    if (!has_selection && text[0] != '\0' && !g_str_has_suffix(text, "\n")) {
        gchar *with_nl = g_strconcat("\n", text, NULL);
        g_free(text);
        text = with_nl;
    } else if (!has_selection && text[0] == '\0') {
        g_free(text);
        text = g_strdup("\n");
    }

    gtk_text_buffer_insert(app.buffer, &insert_at, text, -1);

    if (!has_selection) {
        gtk_text_buffer_get_iter_at_line_offset(app.buffer, &cursor, line_no + 1, line_offset);
        gtk_text_buffer_place_cursor(app.buffer, &cursor);
    }

    g_free(text);
    update_status("Строка скопирована");
}

void on_menu_duplicate_line_activate(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    (void)user_data;
    duplicate_line();
}

static gchar *ensure_trailing_newline(gchar *text) {
    if (!text || text[0] == '\0' || g_str_has_suffix(text, "\n"))
        return text;
    {
        gchar *with_nl = g_strconcat(text, "\n", NULL);
        g_free(text);
        return with_nl;
    }
}

static void move_line(gint direction) {
    GtkTextIter cur_start, cur_end, oth_start, oth_end, block_start, block_end, cursor;
    GtkTextMark *mark;
    gchar *cur_text;
    gchar *oth_text;
    gchar *combined;
    gint line_no, line_offset, target, line_count;

    clear_multicursor();

    mark = gtk_text_buffer_get_insert(app.buffer);
    gtk_text_buffer_get_iter_at_mark(app.buffer, &cursor, mark);
    line_no = gtk_text_iter_get_line(&cursor);
    line_offset = gtk_text_iter_get_line_offset(&cursor);
    line_count = gtk_text_buffer_get_line_count(app.buffer);
    target = line_no + direction;

    if (target < 0 || target >= line_count)
        return;

    gtk_text_buffer_get_iter_at_line(app.buffer, &cur_start, line_no);
    cur_end = cur_start;
    gtk_text_iter_forward_line(&cur_end);

    gtk_text_buffer_get_iter_at_line(app.buffer, &oth_start, target);
    oth_end = oth_start;
    gtk_text_iter_forward_line(&oth_end);

    cur_text = gtk_text_buffer_get_text(app.buffer, &cur_start, &cur_end, FALSE);
    oth_text = gtk_text_buffer_get_text(app.buffer, &oth_start, &oth_end, FALSE);
    if (!cur_text || !oth_text) {
        g_free(cur_text);
        g_free(oth_text);
        return;
    }

    if (direction > 0) {
        block_start = cur_start;
        block_end = oth_end;
        oth_text = ensure_trailing_newline(oth_text);
        combined = g_strconcat(oth_text, cur_text, NULL);
    } else {
        block_start = oth_start;
        block_end = cur_end;
        cur_text = ensure_trailing_newline(cur_text);
        combined = g_strconcat(cur_text, oth_text, NULL);
    }

    gtk_text_buffer_delete(app.buffer, &block_start, &block_end);
    gtk_text_buffer_insert(app.buffer, &block_start, combined, -1);

    g_free(cur_text);
    g_free(oth_text);
    g_free(combined);

    gtk_text_buffer_get_iter_at_line_offset(app.buffer, &cursor, target, line_offset);
    gtk_text_buffer_place_cursor(app.buffer, &cursor);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app.text_view), mark);
    update_status(direction > 0 ? "Строка перемещена вниз" : "Строка перемещена вверх");
}

static gint cursor_lo(const MultiCursor *c) {
    return MIN(c->insert, c->bound);
}

static gint cursor_hi(const MultiCursor *c) {
    return MAX(c->insert, c->bound);
}

static gboolean cursor_has_selection(const MultiCursor *c) {
    return c->insert != c->bound;
}

static gint cmp_cursor(gconstpointer a, gconstpointer b) {
    const MultiCursor *ca = a;
    const MultiCursor *cb = b;
    gint d = cursor_lo(ca) - cursor_lo(cb);
    if (d != 0)
        return d;
    return cursor_hi(ca) - cursor_hi(cb);
}

static gint get_primary_offset(void) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(app.buffer, &iter, gtk_text_buffer_get_insert(app.buffer));
    return gtk_text_iter_get_offset(&iter);
}

static void sync_primary_cursor(const MultiCursor *c) {
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_offset(app.buffer, &ins, c->insert);
    gtk_text_buffer_get_iter_at_offset(app.buffer, &bound, c->bound);
    gtk_text_buffer_select_range(app.buffer, &ins, &bound);
}

static void clear_multicursor(void) {
    if (app.cursors)
        g_array_set_size(app.cursors, 0);
    app.preferred_column = -1;
    app.preferred_bound_column = -1;
    if (app.text_view)
        gtk_widget_queue_draw(app.text_view);
}

static gboolean multicursor_active(void) {
    return app.cursors && app.cursors->len > 1;
}

static void update_multicursor_status(void) {
    if (multicursor_active()) {
        guint selected = 0;
        guint i;
        gchar *msg;
        for (i = 0; i < app.cursors->len; i++) {
            if (cursor_has_selection(&g_array_index(app.cursors, MultiCursor, i)))
                selected++;
        }
        if (selected > 0)
            msg = g_strdup_printf("Мультикурсор: %u (выделение: %u)", app.cursors->len, selected);
        else
            msg = g_strdup_printf("Мультикурсор: %u", app.cursors->len);
        update_status(msg);
        g_free(msg);
    }
}

static gint clamp_line_offset(gint line, gint column) {
    GtkTextIter iter, end;
    gint max_col;

    gtk_text_buffer_get_iter_at_line(app.buffer, &iter, line);
    end = iter;
    if (!gtk_text_iter_ends_line(&end))
        gtk_text_iter_forward_to_line_end(&end);
    max_col = gtk_text_iter_get_line_offset(&end);
    if (column < 0)
        column = 0;
    if (column > max_col)
        column = max_col;
    return column;
}

static MultiCursor cursor_on_line(gint line, gint insert_col, gint bound_col) {
    MultiCursor c;
    GtkTextIter iter;

    insert_col = clamp_line_offset(line, insert_col);
    bound_col = clamp_line_offset(line, bound_col);
    gtk_text_buffer_get_iter_at_line_offset(app.buffer, &iter, line, insert_col);
    c.insert = gtk_text_iter_get_offset(&iter);
    gtk_text_buffer_get_iter_at_line_offset(app.buffer, &iter, line, bound_col);
    c.bound = gtk_text_iter_get_offset(&iter);
    return c;
}

static void seed_cursors_from_primary(void) {
    MultiCursor c;
    GtkTextIter ins, bound;

    if (!app.cursors)
        app.cursors = g_array_new(FALSE, FALSE, sizeof(MultiCursor));
    if (app.cursors->len > 0)
        return;

    gtk_text_buffer_get_iter_at_mark(app.buffer, &ins, gtk_text_buffer_get_insert(app.buffer));
    gtk_text_buffer_get_iter_at_mark(app.buffer, &bound, gtk_text_buffer_get_selection_bound(app.buffer));
    c.insert = gtk_text_iter_get_offset(&ins);
    c.bound = gtk_text_iter_get_offset(&bound);
    g_array_append_val(app.cursors, c);

    app.preferred_column = gtk_text_iter_get_line_offset(&ins);
    app.preferred_bound_column = gtk_text_iter_get_line_offset(&bound);
}

static void add_cursor_vertical(gint direction) {
    MultiCursor neu;
    gint extreme_line, new_line, line_count;
    guint i;
    gboolean exists = FALSE;

    seed_cursors_from_primary();

    extreme_line = 0;
    for (i = 0; i < app.cursors->len; i++) {
        GtkTextIter it;
        MultiCursor *c = &g_array_index(app.cursors, MultiCursor, i);
        gtk_text_buffer_get_iter_at_offset(app.buffer, &it, c->insert);
        if (i == 0)
            extreme_line = gtk_text_iter_get_line(&it);
        else if (direction > 0)
            extreme_line = MAX(extreme_line, gtk_text_iter_get_line(&it));
        else
            extreme_line = MIN(extreme_line, gtk_text_iter_get_line(&it));
    }

    new_line = extreme_line + direction;
    line_count = gtk_text_buffer_get_line_count(app.buffer);
    if (new_line < 0 || new_line >= line_count)
        return;

    if (app.preferred_column < 0)
        app.preferred_column = 0;
    if (app.preferred_bound_column < 0)
        app.preferred_bound_column = app.preferred_column;

    neu = cursor_on_line(new_line, app.preferred_column, app.preferred_bound_column);

    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor *c = &g_array_index(app.cursors, MultiCursor, i);
        if (c->insert == neu.insert && c->bound == neu.bound) {
            exists = TRUE;
            break;
        }
    }
    if (!exists)
        g_array_append_val(app.cursors, neu);

    g_array_sort(app.cursors, cmp_cursor);
    sync_primary_cursor(&neu);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app.text_view),
                                       gtk_text_buffer_get_insert(app.buffer));
    gtk_widget_queue_draw(app.text_view);
    update_multicursor_status();
}

static void extend_multicursor_selection(gint direction) {
    guint i;
    gint primary = get_primary_offset();
    MultiCursor *primary_cursor = NULL;

    if (!multicursor_active())
        return;

    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor *c = &g_array_index(app.cursors, MultiCursor, i);
        GtkTextIter iter;
        gint line, col;

        gtk_text_buffer_get_iter_at_offset(app.buffer, &iter, c->insert);
        line = gtk_text_iter_get_line(&iter);
        col = gtk_text_iter_get_line_offset(&iter) + direction;
        col = clamp_line_offset(line, col);
        gtk_text_buffer_get_iter_at_line_offset(app.buffer, &iter, line, col);
        c->insert = gtk_text_iter_get_offset(&iter);

        if (c->insert == primary || c->bound == primary ||
            (cursor_lo(c) <= primary && cursor_hi(c) >= primary))
            primary_cursor = c;
    }

    /* Keep preferred columns in sync with primary insert/bound */
    if (primary_cursor) {
        GtkTextIter ins, bound;
        gtk_text_buffer_get_iter_at_offset(app.buffer, &ins, primary_cursor->insert);
        gtk_text_buffer_get_iter_at_offset(app.buffer, &bound, primary_cursor->bound);
        app.preferred_column = gtk_text_iter_get_line_offset(&ins);
        app.preferred_bound_column = gtk_text_iter_get_line_offset(&bound);
        sync_primary_cursor(primary_cursor);
    } else {
        sync_primary_cursor(&g_array_index(app.cursors, MultiCursor, app.cursors->len - 1));
    }

    gtk_widget_queue_draw(app.text_view);
    update_multicursor_status();
}

static gint find_primary_index(void) {
    gint primary = get_primary_offset();
    guint i;
    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor *c = &g_array_index(app.cursors, MultiCursor, i);
        if (c->insert == primary)
            return (gint)i;
    }
    return app.cursors->len ? (gint)app.cursors->len - 1 : -1;
}

static void multicursor_insert(const gchar *text) {
    GArray *new_cursors;
    gint char_len, shift = 0, primary_idx, new_primary = -1;
    guint i;

    if (!multicursor_active())
        return;

    char_len = g_utf8_strlen(text, -1);
    primary_idx = find_primary_index();
    g_array_sort(app.cursors, cmp_cursor);
    /* Re-find primary after sort */
    primary_idx = find_primary_index();
    new_cursors = g_array_sized_new(FALSE, FALSE, sizeof(MultiCursor), app.cursors->len);
    app.multicursor_edit = TRUE;

    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor old = g_array_index(app.cursors, MultiCursor, i);
        MultiCursor neu;
        GtkTextIter start, end;
        gint lo = cursor_lo(&old) + shift;
        gint hi = cursor_hi(&old) + shift;
        gint deleted;

        gtk_text_buffer_get_iter_at_offset(app.buffer, &start, lo);
        gtk_text_buffer_get_iter_at_offset(app.buffer, &end, hi);
        deleted = hi - lo;
        if (deleted > 0)
            gtk_text_buffer_delete(app.buffer, &start, &end);

        gtk_text_buffer_get_iter_at_offset(app.buffer, &start, lo);
        gtk_text_buffer_insert(app.buffer, &start, text, -1);
        neu.insert = gtk_text_iter_get_offset(&start);
        neu.bound = neu.insert;
        g_array_append_val(new_cursors, neu);

        if ((gint)i == primary_idx)
            new_primary = neu.insert;

        shift += char_len - deleted;
    }

    app.multicursor_edit = FALSE;
    g_array_free(app.cursors, TRUE);
    app.cursors = new_cursors;
    app.preferred_bound_column = app.preferred_column;
    if (new_primary >= 0) {
        MultiCursor c = { new_primary, new_primary };
        sync_primary_cursor(&c);
    }
    gtk_widget_queue_draw(app.text_view);
    update_multicursor_status();
}

static void multicursor_delete_selections_or(gboolean backspace) {
    GArray *new_cursors;
    gint shift = 0, primary_idx, new_primary = -1;
    guint i;
    gboolean any_selection = FALSE;

    if (!multicursor_active())
        return;

    for (i = 0; i < app.cursors->len; i++) {
        if (cursor_has_selection(&g_array_index(app.cursors, MultiCursor, i))) {
            any_selection = TRUE;
            break;
        }
    }

    primary_idx = find_primary_index();
    g_array_sort(app.cursors, cmp_cursor);
    primary_idx = find_primary_index();
    new_cursors = g_array_sized_new(FALSE, FALSE, sizeof(MultiCursor), app.cursors->len);
    app.multicursor_edit = TRUE;

    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor old = g_array_index(app.cursors, MultiCursor, i);
        MultiCursor neu;
        GtkTextIter start, end;
        gint lo = cursor_lo(&old) + shift;
        gint hi = cursor_hi(&old) + shift;
        gint deleted = 0;

        if (any_selection) {
            if (hi > lo) {
                gtk_text_buffer_get_iter_at_offset(app.buffer, &start, lo);
                gtk_text_buffer_get_iter_at_offset(app.buffer, &end, hi);
                deleted = hi - lo;
                gtk_text_buffer_delete(app.buffer, &start, &end);
            }
            neu.insert = lo;
            neu.bound = lo;
        } else if (backspace) {
            gtk_text_buffer_get_iter_at_offset(app.buffer, &end, lo);
            start = end;
            if (gtk_text_iter_backward_char(&start)) {
                deleted = 1;
                gtk_text_buffer_delete(app.buffer, &start, &end);
                lo = gtk_text_iter_get_offset(&start);
            }
            neu.insert = lo;
            neu.bound = lo;
        } else {
            gtk_text_buffer_get_iter_at_offset(app.buffer, &start, lo);
            end = start;
            if (gtk_text_iter_forward_char(&end)) {
                deleted = 1;
                gtk_text_buffer_delete(app.buffer, &start, &end);
            }
            neu.insert = lo;
            neu.bound = lo;
        }

        g_array_append_val(new_cursors, neu);
        if ((gint)i == primary_idx)
            new_primary = neu.insert;
        shift -= deleted;
    }

    app.multicursor_edit = FALSE;
    g_array_free(app.cursors, TRUE);
    app.cursors = new_cursors;
    app.preferred_bound_column = app.preferred_column;
    if (new_primary >= 0) {
        MultiCursor c = { new_primary, new_primary };
        sync_primary_cursor(&c);
    }
    gtk_widget_queue_draw(app.text_view);
    update_multicursor_status();
}

static gboolean on_draw_cursors(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    GtkStyleContext *style;
    GdkRGBA fg, sel;
    gint primary;
    guint i;

    (void)user_data;

    if (!multicursor_active())
        return FALSE;

    style = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &fg);
    if (!gtk_style_context_lookup_color(style, "theme_selected_bg_color", &sel)) {
        sel = fg;
        sel.alpha = 0.35;
    } else {
        sel.alpha = 0.45;
    }
    primary = get_primary_offset();

    for (i = 0; i < app.cursors->len; i++) {
        MultiCursor *c = &g_array_index(app.cursors, MultiCursor, i);
        GtkTextIter ins, lo, hi;
        GdkRectangle loc, loc2;
        gint x, y, x2;

        gtk_text_buffer_get_iter_at_offset(app.buffer, &ins, c->insert);

        if (cursor_has_selection(c)) {
            gtk_text_buffer_get_iter_at_offset(app.buffer, &lo, cursor_lo(c));
            gtk_text_buffer_get_iter_at_offset(app.buffer, &hi, cursor_hi(c));
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(widget), &lo, &loc);
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(widget), &hi, &loc2);
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget),
                                                  GTK_TEXT_WINDOW_TEXT,
                                                  loc.x, loc.y, &x, &y);
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget),
                                                  GTK_TEXT_WINDOW_TEXT,
                                                  loc2.x, loc2.y, &x2, NULL);
            if (x2 < x) {
                gint tmp = x;
                x = x2;
                x2 = tmp;
            }
            cairo_set_source_rgba(cr, sel.red, sel.green, sel.blue, sel.alpha);
            cairo_rectangle(cr, x, y, MAX(x2 - x, 2), loc.height);
            cairo_fill(cr);
        }

        if (c->insert != primary) {
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(widget), &ins, &loc);
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget),
                                                  GTK_TEXT_WINDOW_TEXT,
                                                  loc.x, loc.y, &x, &y);
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.85);
            cairo_rectangle(cr, x, y, 2, loc.height);
            cairo_fill(cr);
        }
    }

    return FALSE;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget;
    (void)event;
    (void)user_data;
    if (multicursor_active())
        clear_multicursor();
    return FALSE;
}

static gboolean is_modifier_key(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
    case GDK_KEY_ISO_Level3_Shift:
    case GDK_KEY_Mode_switch:
    case GDK_KEY_Num_Lock:
    case GDK_KEY_Caps_Lock:
    case GDK_KEY_Scroll_Lock:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    GdkModifierType mods;
    guint32 ch;

    (void)widget;
    (void)user_data;

    mods = event->state & gtk_accelerator_get_default_mod_mask();

    /* Modifier-only presses must not reset multi-cursor (e.g. holding Shift) */
    if (is_modifier_key(event->keyval))
        return FALSE;

    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) {
            move_line(1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) {
            move_line(-1);
            return TRUE;
        }
    }

    if (mods == GDK_MOD1_MASK) {
        if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) {
            add_cursor_vertical(1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) {
            add_cursor_vertical(-1);
            return TRUE;
        }
    }

    if (event->keyval == GDK_KEY_Escape && multicursor_active()) {
        clear_multicursor();
        update_status("Мультикурсор сброшен");
        return TRUE;
    }

    if (!multicursor_active())
        return FALSE;

    if ((mods & GDK_SHIFT_MASK) && !(mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
        if (event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left) {
            extend_multicursor_selection(-1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right) {
            extend_multicursor_selection(1);
            return TRUE;
        }
    }

    if (event->keyval == GDK_KEY_BackSpace) {
        multicursor_delete_selections_or(TRUE);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_Delete) {
        multicursor_delete_selections_or(FALSE);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        multicursor_insert("\n");
        return TRUE;
    }

    if (event->keyval == GDK_KEY_Tab) {
        multicursor_insert("\t");
        return TRUE;
    }

    ch = gdk_keyval_to_unicode(event->keyval);
    if (ch && !g_unichar_iscntrl(ch) && !(mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
        gchar utf8[6];
        gint len = g_unichar_to_utf8(ch, utf8);
        utf8[len] = '\0';
        multicursor_insert(utf8);
        return TRUE;
    }

    /* Navigation and other keys drop multi-cursor and use default behavior */
    clear_multicursor();
    return FALSE;
}

void on_menu_about_activate(GtkMenuItem *menuitem, gpointer user_data) {
    const gchar *authors[] = {"Simple Text Editor contributors", NULL};

    (void)menuitem;
    (void)user_data;

    gtk_show_about_dialog(GTK_WINDOW(app.window),
                          "program-name", APP_NAME,
                          "version", APP_VERSION,
                          "comments", "Простой текстовый редактор на GTK3 и Glade",
                          "authors", authors,
                          "logo", app.icon,
                          "license-type", GTK_LICENSE_APACHE_2_0,
                          "website", "https://github.com/jf17/simple-text-editor",
                          NULL);
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    (void)event;
    (void)user_data;
    if (confirm_discard_changes()) {
        gtk_main_quit();
        return FALSE;
    }
    return TRUE;
}

static void add_accel(GtkWidget *item, GtkAccelGroup *accel,
                      guint key, GdkModifierType mods) {
    gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
}

static void connect_menus(GtkBuilder *builder, GtkAccelGroup *accel) {
    GtkWidget *item;

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_new"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_new_activate), NULL);
    add_accel(item, accel, GDK_KEY_n, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_open"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_open_activate), NULL);
    add_accel(item, accel, GDK_KEY_o, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_save"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_save_activate), NULL);
    add_accel(item, accel, GDK_KEY_s, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_save_as"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_save_as_activate), NULL);
    add_accel(item, accel, GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_exit"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_exit_activate), NULL);
    add_accel(item, accel, GDK_KEY_q, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_undo"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_undo_activate), NULL);
    add_accel(item, accel, GDK_KEY_z, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_redo"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_redo_activate), NULL);
    add_accel(item, accel, GDK_KEY_z, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    add_accel(item, accel, GDK_KEY_y, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_cut"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_cut_activate), NULL);
    add_accel(item, accel, GDK_KEY_x, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_copy"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_copy_activate), NULL);
    add_accel(item, accel, GDK_KEY_c, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_paste"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_paste_activate), NULL);
    add_accel(item, accel, GDK_KEY_v, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_delete"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_delete_activate), NULL);
    add_accel(item, accel, GDK_KEY_Delete, 0);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_select_all"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_select_all_activate), NULL);
    add_accel(item, accel, GDK_KEY_a, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_duplicate_line"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_duplicate_line_activate), NULL);
    add_accel(item, accel, GDK_KEY_d, GDK_CONTROL_MASK);

    item = GTK_WIDGET(gtk_builder_get_object(builder, "menu_about"));
    g_signal_connect(item, "activate", G_CALLBACK(on_menu_about_activate), NULL);
}

int main(int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkAccelGroup *accel;
    GError *error = NULL;
    gchar *glade_file;

    g_set_prgname("simple-text-editor");
    g_set_application_name(APP_NAME);
    gtk_init(&argc, &argv);
    memset(&app, 0, sizeof(app));

    glade_file = find_glade_file();
    if (!glade_file) {
        g_printerr("Ошибка: файл интерфейса editor.glade не найден\n");
        return 1;
    }

    builder = gtk_builder_new();
    if (!gtk_builder_add_from_file(builder, glade_file, &error)) {
        g_printerr("Ошибка при загрузке файла интерфейса: %s\n", error->message);
        g_clear_error(&error);
        g_free(glade_file);
        return 1;
    }
    g_free(glade_file);

    app.window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    app.text_view = GTK_WIDGET(gtk_builder_get_object(builder, "text_view"));
    app.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.text_view));
    app.status_bar = GTK_WIDGET(gtk_builder_get_object(builder, "status_bar"));
    app.status_context = gtk_statusbar_get_context_id(GTK_STATUSBAR(app.status_bar), "main");
    load_app_icon();

    accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app.window), accel);
    connect_menus(builder, accel);

    g_signal_connect(app.buffer, "insert-text", G_CALLBACK(on_insert_text), NULL);
    g_signal_connect(app.buffer, "delete-range", G_CALLBACK(on_delete_range), NULL);
    g_signal_connect(app.buffer, "changed", G_CALLBACK(on_changed), NULL);
    g_signal_connect(app.buffer, "mark-set", G_CALLBACK(on_mark_set), NULL);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(app.text_view, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(app.text_view, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_signal_connect_after(app.text_view, "draw", G_CALLBACK(on_draw_cursors), NULL);

    app.preferred_column = -1;
    app.preferred_bound_column = -1;
    set_modified(FALSE);
    update_status("Готово");

    if (argc > 1)
        load_file(argv[1]);

    gtk_widget_show_all(app.window);
    gtk_widget_grab_focus(app.text_view);
    gtk_main();

    clear_undo_stack(&app.undo_stack);
    clear_undo_stack(&app.redo_stack);
    if (app.cursors)
        g_array_free(app.cursors, TRUE);
    g_free(app.current_file);
    if (app.icon)
        g_object_unref(app.icon);
    g_object_unref(builder);

    return 0;
}
