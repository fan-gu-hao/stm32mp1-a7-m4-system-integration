#define _DEFAULT_SOURCE
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HISTORY 256U
#define SPAN_MS 20000U
#define FW "FW=TOF_STREAM_V1"

typedef struct {
    int fd;
    char rx[512], tx[128];
    size_t used, length, sent;
    gboolean discard;
} Channel;

typedef struct {
    uint32_t ms, mm;
    gboolean valid, join;
} Point;

enum Command { NONE, START, STOP };

static struct {
    Channel ch[2];
    GtkWidget *window, *distance, *status, *bar, *graph, *stats;
    GtkWidget *start, *stop;
    Point points[HISTORY];
    unsigned int head, count;
    uint32_t seq, last_ms, samples, invalid, gaps;
    gboolean have_seq, running, synced, closing, failed, stale;
    enum Command pending;
    gint64 deadline, last_rx;
} app = { .ch = {{ .fd = -1 }, { .fd = -1 }} };

static void buttons(void)
{
    gboolean ready = app.synced && !app.pending && !app.closing;
    gtk_widget_set_sensitive(app.start, ready && !app.running);
    gtk_widget_set_sensitive(app.stop, ready && app.running);
}

static void message(const char *text)
{
    gtk_label_set_text(GTK_LABEL(app.status), text);
}

static void no_distance(void)
{
    gtk_label_set_text(GTK_LABEL(app.distance), "-- mm");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.bar), 0.0);
}

static void fail(const char *text)
{
    g_printerr("%s\n", text);
    app.failed = TRUE;
    gtk_main_quit();
}

/* 非阻塞傳送：保留尚未寫完的部分，下一次事件迴圈繼續。 */
static gboolean enqueue(Channel *c, const char *text)
{
    size_t left = c->length - c->sent;
    size_t n = strlen(text);

    if (left + n > sizeof(c->tx)) {
        fail("Command transmit queue is full");
        return FALSE;
    }

    memmove(c->tx, c->tx + c->sent, left);
    memcpy(c->tx + left, text, n);
    c->sent = 0;
    c->length = left + n;
    return TRUE;
}

static gboolean flush(Channel *c)
{
    if (c->sent == c->length)
        return TRUE;

    ssize_t n = write(c->fd, c->tx + c->sent, c->length - c->sent);

    if (n > 0) {
        c->sent += (size_t)n;
    } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
               errno != ENOMEM && errno != ENOBUFS) {
        fail("RPMsg write failed; check M4 and device permissions");
        return FALSE;
    }

    if (c->sent == c->length)
        c->sent = c->length = 0;

    return TRUE;
}

static void command(enum Command value)
{
    const char *text = value == START ? "TOF,START\n" : "TOF,STOP\n";

    if (!enqueue(&app.ch[1], text))
        return;

    app.pending = value;
    app.deadline = g_get_monotonic_time()
                 + (app.closing ? 2000000 : 5000000);

    message(value == START ? "Starting..." : "Stopping...");
    buttons();
}

static void request_close(void)
{
    if (app.closing)
        return;

    app.closing = TRUE;
    command(STOP);
}

static gboolean close_window(GtkWidget *w, GdkEvent *event, gpointer data)
{
    (void)w;
    (void)event;
    (void)data;
    request_close();
    return TRUE;
}

static gboolean close_signal(gpointer data)
{
    (void)data;
    request_close();
    return G_SOURCE_CONTINUE;
}

static gboolean number(const char **cursor, const char *key, uint32_t *value)
{
    const char *p = *cursor;
    size_t n = strlen(key);

    if (strncmp(p, key, n) != 0)
        return FALSE;

    p += n;

    if (*p < '0' || *p > '9')
        return FALSE;

    char *end;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 10);

    if (errno || v > UINT32_MAX || *end != ',')
        return FALSE;

    *value = (uint32_t)v;
    *cursor = end + 1;
    return TRUE;
}

/* 僅完整且符合版本的資料行，才能更新畫面。 */
static void data_line(const char *line)
{
    if (app.closing || app.pending == STOP ||
        (!app.running && app.pending != START))
        return;

    if (!g_str_has_prefix(line, "TOF,DATA,"))
        return;

    const char *p = line + 9;
    uint32_t seq, ms, drops, mm = 0, status = 0;

    if (!number(&p, "SEQ=", &seq) ||
        !number(&p, "MS=", &ms) ||
        !number(&p, "TX_DROP=", &drops))
        return;

    gboolean valid = FALSE;
    gboolean sensor_error = FALSE;

    if (g_str_has_prefix(p, "OK,")) {
        p += 3;

        if (!number(&p, "MM=", &mm) || mm > 65535U ||
            !number(&p, "RANGE_STATUS=", &status) || status != 0 ||
            strcmp(p, FW) != 0)
            return;

        valid = TRUE;

    } else if (g_str_has_prefix(p, "INVALID,")) {
        p += 8;

        if (!number(&p, "RANGE_STATUS=", &status) || status > 255U ||
            strcmp(p, FW) != 0)
            return;

    } else if (g_str_has_prefix(p, "ERROR,") &&
               g_str_has_suffix(p, FW)) {
        sensor_error = TRUE;
        g_printerr("Sensor error: %s\n", line);

    } else {
        return;
    }

    uint32_t delta = seq - app.seq;

    if (app.have_seq && delta == 0)
        return;

    gboolean join = app.have_seq && delta == 1U &&
                    (uint32_t)(ms - app.last_ms) <= 500U;

    if (app.have_seq && delta >= 0x80000000U) {
        app.head = app.count = 0;
        join = FALSE;
    } else if (app.have_seq && delta > 1U) {
        app.gaps += delta - 1U;
    }

    app.have_seq = TRUE;
    app.seq = seq;
    app.last_ms = ms;
    app.samples++;

    if (!valid)
        app.invalid++;

    app.last_rx = g_get_monotonic_time();
    app.stale = FALSE;

    app.points[app.head] = (Point){ms, mm, valid, join};
    app.head = (app.head + 1U) % HISTORY;

    if (app.count < HISTORY)
        app.count++;

    char text[128];

    if (valid) {
        g_snprintf(text, sizeof(text), "%u mm", (unsigned int)mm);
        gtk_label_set_text(GTK_LABEL(app.distance), text);

        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(app.bar), MIN(mm / 1000.0, 1.0));

        if (!app.pending)
            message("Measuring");
    } else {
        no_distance();

        g_snprintf(text, sizeof(text), "Invalid measurement (%u)",
                   (unsigned int)status);

        message(sensor_error
            ? "Sensor error - press START to retry"
            : text);
    }

    if (sensor_error) {
        app.running = FALSE;
        app.pending = NONE;
        buttons();
    }

    g_snprintf(text, sizeof(text),
               "Samples: %u   Invalid: %u   Gaps: %u   TX drop: %u",
               (unsigned int)app.samples,
               (unsigned int)app.invalid,
               (unsigned int)app.gaps,
               (unsigned int)drops);

    gtk_label_set_text(GTK_LABEL(app.stats), text);
    gtk_widget_queue_draw(app.graph);
}

static void control_line(const char *line)
{
    g_print("%s\n", line);

    if (app.pending == START &&
        strcmp(line, "TOF,ACK,START,PERIOD_MS=100," FW) == 0) {
        app.pending = NONE;
        app.running = TRUE;
        app.last_rx = g_get_monotonic_time();
        app.stale = FALSE;
        no_distance();
        message("Waiting for measurement...");

    } else if (app.pending == STOP &&
               strcmp(line, "TOF,ACK,STOP," FW) == 0) {
        app.pending = NONE;
        app.running = FALSE;
        app.synced = TRUE;
        no_distance();
        message("Stopped - press START");

        if (app.closing)
            gtk_main_quit();

    } else if (app.pending == START &&
               (g_str_has_prefix(line, "TOF,PROBE,") ||
                g_str_has_prefix(line, "TOF,ACK,ERROR,"))) {
        app.pending = NONE;
        app.running = FALSE;
        no_distance();
        message("Start failed - check sensor");
    }

    buttons();
}

/* 字元裝置視為位元組串流，處理拆行、合併及過長資料。 */
static gboolean receive(Channel *c, gboolean control)
{
    char buffer[512];

    for (unsigned int batch = 0; batch < 16; batch++) {
        ssize_t n = read(c->fd, buffer, sizeof(buffer));

        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return TRUE;

        if (n <= 0) {
            fail("RPMsg disconnected or read failed");
            return FALSE;
        }

        for (ssize_t i = 0; i < n; i++) {
            unsigned char b = (unsigned char)buffer[i];

            if (b == '\n') {
                if (!c->discard) {
                    if (c->used && c->rx[c->used - 1] == '\r')
                        c->used--;

                    c->rx[c->used] = 0;

                    if (control)
                        control_line(c->rx);
                    else
                        data_line(c->rx);
                }

                c->used = 0;
                c->discard = FALSE;

            } else if (!c->discard) {
                if (!b || c->used >= sizeof(c->rx) - 1U)
                    c->discard = TRUE;
                else
                    c->rx[c->used++] = (char)b;
            }
        }
    }

    return TRUE;
}

static gboolean io_tick(gpointer unused)
{
    (void)unused;

    if (!flush(&app.ch[0]))
        return G_SOURCE_REMOVE;

    if (!app.ch[0].length && !flush(&app.ch[1]))
        return G_SOURCE_REMOVE;

    if (!receive(&app.ch[1], TRUE) || !receive(&app.ch[0], FALSE))
        return G_SOURCE_REMOVE;

    gint64 now = g_get_monotonic_time();

    if (app.pending && now >= app.deadline) {
        fail("No expected M4 acknowledgement; check journal and firmware version");
        return G_SOURCE_REMOVE;
    }

    if (app.running && !app.pending && !app.stale &&
        now - app.last_rx > 2000000) {
        app.stale = TRUE;
        no_distance();
        message("No data - check sensor / M4");
        g_printerr("No measurement received for 2 seconds\n");
    }

    return G_SOURCE_CONTINUE;
}

static gboolean draw_graph(GtkWidget *widget, cairo_t *cr, gpointer unused)
{
    (void)unused;

    double width = gtk_widget_get_allocated_width(widget);
    double height = gtk_widget_get_allocated_height(widget);
    double left = 52, top = 22;
    double w = width - 68, h = height - 52;

    if (w <= 0 || h <= 0)
        return FALSE;

    uint32_t maximum = 500;
    unsigned int first = (app.head + HISTORY - app.count) % HISTORY;

    for (unsigned int i = 0; i < app.count; i++) {
        Point *p = &app.points[(first + i) % HISTORY];

        if (p->valid &&
            (uint32_t)(app.last_ms - p->ms) <= SPAN_MS &&
            p->mm > maximum)
            maximum = ((p->mm + 499U) / 500U) * 500U;
    }

    cairo_set_source_rgb(cr, 0.07, 0.11, 0.18);
    cairo_paint(cr);
    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);

    for (int i = 0; i <= 4; i++) {
        double y = top + h * i / 4.0;

        cairo_set_source_rgb(cr, 0.22, 0.28, 0.36);
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, left + w, y);
        cairo_stroke(cr);

        char text[24];
        g_snprintf(text, sizeof(text), "%u",
                   (unsigned int)(maximum * (4 - i) / 4));

        cairo_set_source_rgb(cr, 0.8, 0.85, 0.9);
        cairo_move_to(cr, 6, y + 4);
        cairo_show_text(cr, text);
    }

    cairo_move_to(cr, 6, 13);
    cairo_show_text(cr, "mm");
    cairo_move_to(cr, left, height - 8);
    cairo_show_text(cr, "-20 s");
    cairo_move_to(cr, left + w / 2 - 12, height - 8);
    cairo_show_text(cr, "-10 s");
    cairo_move_to(cr, left + w - 16, height - 8);
    cairo_show_text(cr, "0 s");

    cairo_save(cr);
    cairo_rectangle(cr, left, top, w, h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.22, 0.8, 0.95);
    cairo_set_line_width(cr, 2.0);

    gboolean pen = FALSE;

    for (unsigned int i = 0; i < app.count; i++) {
        Point *p = &app.points[(first + i) % HISTORY];
        uint32_t age = app.last_ms - p->ms;

        if (!p->valid || age > SPAN_MS) {
            pen = FALSE;
            continue;
        }

        double x = left + w * (1.0 - age / (double)SPAN_MS);
        double y = top + h * (1.0 - p->mm / (double)maximum);

        if (pen && p->join)
            cairo_line_to(cr, x, y);
        else
            cairo_move_to(cr, x, y);

        pen = TRUE;
    }

    cairo_stroke(cr);
    cairo_restore(cr);
    return FALSE;
}

static void clicked(GtkButton *button, gpointer action)
{
    (void)button;

    switch (GPOINTER_TO_INT(action)) {
    case 0:
        command(START);
        break;
    case 1:
        command(STOP);
        break;
    case 2:
        app.head = app.count = 0;
        gtk_widget_queue_draw(app.graph);
        break;
    case 3:
        request_close();
        break;
    }
}

static int open_channel(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0)
        return -1;

    struct termios t;

    if (tcgetattr(fd, &t) < 0)
        goto failed;

    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &t) < 0 ||
        tcflush(fd, TCIFLUSH) < 0)
        goto failed;

    return fd;

failed:
    {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    gtk_init(&argc, &argv);

    const char *paths[2] = {
        argc > 1 ? argv[1] : "/dev/ttyRPMSG0",
        argc > 2 ? argv[2] : "/dev/ttyRPMSG1"
    };

    for (int i = 0; i < 2; i++) {
        app.ch[i].fd = open_channel(paths[i]);

        if (app.ch[i].fd < 0) {
            g_printerr("Cannot open %s: %s\n",
                       paths[i], strerror(errno));
            if (i)
                close(app.ch[0].fd);
            return 1;
        }
    }

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window),
                         "STM32MP1 Distance Monitor");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 760, 420);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 10);

    g_signal_connect(app.window, "delete-event",
                     G_CALLBACK(close_window), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(app.window), box);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    app.distance = gtk_label_new("-- mm");
    gtk_widget_set_name(app.distance, "distance");
    gtk_box_pack_start(GTK_BOX(top), app.distance, TRUE, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = gtk_label_new("Distance Monitor");
    gtk_widget_set_name(title, "title");

    app.status = gtk_label_new("Connecting...");
    gtk_label_set_max_width_chars(GTK_LABEL(app.status), 32);
    gtk_label_set_ellipsize(GTK_LABEL(app.status), PANGO_ELLIPSIZE_END);

    gtk_box_pack_start(GTK_BOX(right), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), app.status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    app.bar = gtk_progress_bar_new();
    gtk_widget_set_tooltip_text(app.bar, "Distance: 0 to 1000 mm");
    gtk_box_pack_start(GTK_BOX(box), app.bar, FALSE, FALSE, 0);

    app.graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.graph, 320, 170);
    g_signal_connect(app.graph, "draw", G_CALLBACK(draw_graph), NULL);
    gtk_box_pack_start(GTK_BOX(box), app.graph, TRUE, TRUE, 0);

    app.stats = gtk_label_new(
        "Samples: 0   Invalid: 0   Gaps: 0   TX drop: 0");
    gtk_label_set_ellipsize(GTK_LABEL(app.stats), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), app.stats, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);

    const char *names[] = {"START", "STOP", "CLEAR", "CLOSE"};

    for (int i = 0; i < 4; i++) {
        GtkWidget *button = gtk_button_new_with_label(names[i]);
        gtk_widget_set_size_request(button, 80, 48);

        g_signal_connect(button, "clicked",
                         G_CALLBACK(clicked), GINT_TO_POINTER(i));

        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);

        if (i == 0)
            app.start = button;
        if (i == 1)
            app.stop = button;
    }

    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkCssProvider *css = gtk_css_provider_new();

    gtk_css_provider_load_from_data(css,
        "window {background-color:#142033;color:#e5edf5;}"
        "#distance {font-size:42px;font-weight:bold;}"
        "#title {font-size:18px;font-weight:bold;}"
        "button {background-image:none;background-color:#334155;"
        "color:#ffffff;font-size:16px;}"
        "button:disabled {background-color:#243246;color:#8090a0;}"
        "progressbar trough {min-height:8px;}"
        "progressbar progress {background-image:none;"
        "background-color:#38bdf8;}",
        -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(css);

    gtk_widget_show_all(app.window);
    gtk_window_maximize(GTK_WINDOW(app.window));

    enqueue(&app.ch[0], "READY\n");
    command(STOP);

    g_unix_signal_add(SIGTERM, close_signal, NULL);
    g_unix_signal_add(SIGINT, close_signal, NULL);
    g_timeout_add(20, io_tick, NULL);

    g_print("MP1 ToF GUI ready: %s, %s; %s\n",
            paths[0], paths[1], FW);

    gtk_main();

    if (app.failed) {
        ssize_t n = write(app.ch[1].fd, "\nTOF,STOP\n", 10);
        if (n != 10)
            g_printerr("Could not write the complete STOP command\n");
    }

    close(app.ch[0].fd);
    close(app.ch[1].fd);
    return app.failed ? 1 : 0;
}
