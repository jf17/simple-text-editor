#include <gtk/gtk.h>
#include <string.h>

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
    GList *undo_stack;
    GList *redo_stack;
} App;

static App app;

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
                          "license-type", GTK_LICENSE_MIT_X11,
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

    set_modified(FALSE);
    update_status("Готово");

    if (argc > 1)
        load_file(argv[1]);

    gtk_widget_show_all(app.window);
    gtk_widget_grab_focus(app.text_view);
    gtk_main();

    clear_undo_stack(&app.undo_stack);
    clear_undo_stack(&app.redo_stack);
    g_free(app.current_file);
    if (app.icon)
        g_object_unref(app.icon);
    g_object_unref(builder);

    return 0;
}
