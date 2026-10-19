#include <gtk/gtk.h>

#ifndef GLADE_PATH
#define GLADE_PATH "/usr/share/simple-text-editor/editor.glade"
#endif

GtkWidget *text_view; // Текстовый виджет

static const char *find_glade_file(void) {
    if (g_file_test("editor.glade", G_FILE_TEST_EXISTS))
        return "editor.glade";
    if (g_file_test(GLADE_PATH, G_FILE_TEST_EXISTS))
        return GLADE_PATH;
    return NULL;
}

// Функция для открытия файла
void on_menu_open_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkWidget *dialog;
    GtkTextBuffer *buffer;
    char *filename;
    gchar *content;
    gsize length;

    GtkWidget *window = GTK_WIDGET(user_data);
    dialog = gtk_file_chooser_dialog_new("Открыть файл",
                                         GTK_WINDOW(window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Отмена", GTK_RESPONSE_CANCEL,
                                         "_Открыть", GTK_RESPONSE_ACCEPT,
                                         NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        filename = gtk_file_chooser_get_filename(chooser);

        if (g_file_get_contents(filename, &content, &length, NULL)) {
            buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
            gtk_text_buffer_set_text(buffer, content, -1);
            g_free(content);
        }

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

// Функция для сохранения файла
void on_menu_save_activate(GtkMenuItem *menuitem, gpointer user_data) {
    GtkWidget *dialog;
    GtkTextBuffer *buffer;
    GtkTextIter start, end;
    gchar *content;
    char *filename;

    GtkWidget *window = GTK_WIDGET(user_data);
    dialog = gtk_file_chooser_dialog_new("Сохранить файл",
                                         GTK_WINDOW(window),
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         "_Отмена", GTK_RESPONSE_CANCEL,
                                         "_Сохранить", GTK_RESPONSE_ACCEPT,
                                         NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        filename = gtk_file_chooser_get_filename(chooser);

        buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_end_iter(buffer, &end);
        content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

        if (g_file_set_contents(filename, content, -1, NULL)) {
            g_print("Файл сохранен: %s\n", filename);
        } else {
            g_print("Ошибка при сохранении файла: %s\n", filename);
        }

        g_free(content);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

// Функция выхода
void on_menu_exit_activate(GtkMenuItem *menuitem, gpointer user_data) {
    gtk_main_quit();
}

int main(int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkWidget *window;
    GError *error = NULL;

    gtk_init(&argc, &argv);

    // Загрузка интерфейса из файла Glade
    const char *glade_file = find_glade_file();
    if (!glade_file) {
        g_printerr("Ошибка: файл интерфейса editor.glade не найден\n");
        return 1;
    }

    builder = gtk_builder_new();
    if (!gtk_builder_add_from_file(builder, glade_file, &error)) {
        g_printerr("Ошибка при загрузке файла интерфейса: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    // Получение виджетов
    window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    text_view = GTK_WIDGET(gtk_builder_get_object(builder, "text_view"));

    // Привязка сигналов
    gtk_builder_connect_signals(builder, window);

    g_signal_connect(gtk_builder_get_object(builder, "menu_open"), "activate", G_CALLBACK(on_menu_open_activate), window);
    g_signal_connect(gtk_builder_get_object(builder, "menu_save"), "activate", G_CALLBACK(on_menu_save_activate), window);
    g_signal_connect(gtk_builder_get_object(builder, "menu_exit"), "activate", G_CALLBACK(on_menu_exit_activate), NULL);

    // Отображение главного окна
    gtk_widget_show_all(window);

    gtk_main();

    return 0;
}

