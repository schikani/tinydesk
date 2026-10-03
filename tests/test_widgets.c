/*
 * test_widgets.c - text boxes, lists, scrollbars, checkboxes, buttons,
 * focus, the message box and the input box, driven by synthetic events.
 */
#include "td_test.h"
#include "tinydesk/td.h"

static uint32_t s_now = 10000;
static int fake_read(void *ctx) { (void)ctx; return -1; }
static int fake_write(void *ctx, const uint8_t *b, int n) { (void)ctx; (void)b; return n; }
static uint32_t fake_millis(void *ctx) { (void)ctx; return s_now; }
static void fake_sleep(void *ctx, uint32_t ms) { (void)ctx; s_now += ms; }
static const td_hal_t s_hal = { fake_read, fake_write, fake_millis, fake_sleep, NULL };

static void key(uint32_t k, uint8_t mods)
{
    td_event_t ev = { 0 };
    ev.type = TD_EV_KEY;
    ev.key = k;
    ev.mods = mods;
    ev.time_ms = s_now;
    td_wm_dispatch(&ev);
}

static void type(const char *s)
{
    while (*s) key((uint8_t)*s++, 0);
}

static void mouse(int action, int button, int x, int y)
{
    td_event_t ev = { 0 };
    ev.type = TD_EV_MOUSE;
    ev.action = (uint8_t)action;
    ev.button = (uint8_t)button;
    ev.x = (int16_t)x;
    ev.y = (int16_t)y;
    ev.time_ms = s_now;
    s_now += 500;   /* never a double click */
    td_wm_dispatch(&ev);
}

static void click(int x, int y)
{
    mouse(TD_MOUSE_PRESS, TD_BUTTON_LEFT, x, y);
    mouse(TD_MOUSE_RELEASE, TD_BUTTON_LEFT, x, y);
}

static void double_click(int x, int y)
{
    td_event_t ev = { 0 };
    ev.type = TD_EV_MOUSE;
    ev.button = TD_BUTTON_LEFT;
    ev.x = (int16_t)x;
    ev.y = (int16_t)y;
    for (int i = 0; i < 2; i++) {
        ev.time_ms = s_now;
        ev.action = TD_MOUSE_PRESS;
        td_wm_dispatch(&ev);
        ev.action = TD_MOUSE_RELEASE;
        td_wm_dispatch(&ev);
        s_now += 100;              /* well inside the double-click time */
    }
    s_now += 1000;
}

/* A bracketed paste, as a terminal sends it, through the input parser. */
static void paste(const char *text)
{
    static td_input_t in;
    td_input_init(&in);
    const char *parts[3] = { "\x1b[200~", text, "\x1b[201~" };
    for (int i = 0; i < 3; i++)
        for (const char *p = parts[i]; *p; p++) td_input_feed(&in, (uint8_t)*p, s_now);
    td_event_t ev;
    while (td_event_pop(&ev)) {
        td_wm_dispatch(&ev);
        if (ev.type == TD_EV_PASTE) td_input_paste_done(&in);
    }
}

static bool text_is(const td_widget_t *w, const char *expect)
{
    const char *got = td_widget_text(w);
    if (strcmp(got, expect) == 0) return true;
    printf("  text: \"%s\", expected \"%s\"\n", got, expect);
    return false;
}

static td_buffer_t s_back;
static const td_cell_t *cell(int x, int y) { return td_buffer_cell(&s_back, x, y); }

/* A fresh desktop with one 40x12 window; its client area starts at (1, 1). */
static td_window_t *new_window(void)
{
    td_wm_init(80, 25);
    td_window_desc_t d = { .title = "W", .rect = td_rect(0, 0, 40, 12), .flags = TD_WIN_DEFAULT };
    return td_win_create(&d);
}

static td_widget_t *find_button(td_window_t *w, const char *caption)
{
    for (td_widget_t *wd = w->widgets; wd; wd = wd->next)
        if (wd->type == TD_WT_BUTTON && strcmp(td_widget_text(wd), caption) == 0) return wd;
    return NULL;
}

static int s_activated;
static void on_activate(td_widget_t *w, void *user) { (void)w; (void)user; s_activated++; }

/* ------------------------------------------------------------ text box */

static void test_textbox(void)
{
    td_window_t *w = new_window();
    td_widget_t *tb = td_textbox(w, 0, 0, 10, 20, on_activate, NULL);
    CHECK(w->focus == tb);

    type("hello");
    CHECK(text_is(tb, "hello"));
    CHECK_EQ(tb->value, 5);                    /* the cursor follows */

    /* Insert in the middle, delete at both ends. */
    key(TD_KEY_LEFT, 0);
    key(TD_KEY_LEFT, 0);
    type("XY");
    CHECK(text_is(tb, "helXYlo"));
    key(TD_KEY_HOME, 0);
    key(TD_KEY_DELETE, 0);
    key(TD_KEY_END, 0);
    key(TD_KEY_BACKSPACE, 0);
    CHECK(text_is(tb, "elXYl"));

    /* Nothing to delete before the start or after the end. */
    key(TD_KEY_HOME, 0);
    key(TD_KEY_BACKSPACE, 0);
    CHECK_EQ(tb->value, 0);
    key(TD_KEY_END, 0);
    key(TD_KEY_DELETE, 0);
    CHECK(text_is(tb, "elXYl"));

    /* Ctrl and Alt combinations are not typed. */
    key('a', TD_MOD_CTRL);
    key('b', TD_MOD_ALT);
    CHECK(text_is(tb, "elXYl"));

    /* The length limit holds, and the view scrolls to keep the cursor in
     * the box. */
    type("0123456789abcdefghij");
    CHECK_EQ((int)strlen(td_widget_text(tb)), 20);
    CHECK_EQ(tb->scroll, 20 - 10 + 1);
    key(TD_KEY_HOME, 0);
    CHECK_EQ(tb->scroll, 0);

    s_activated = 0;
    key(TD_KEY_ENTER, 0);
    CHECK_EQ(s_activated, 1);

    /* Setting the text puts the cursor at its end. */
    td_widget_set_text(tb, "abc");
    CHECK(text_is(tb, "abc"));
    CHECK_EQ(tb->value, 3);
    CHECK_EQ(tb->scroll, 0);

    /* A click places the cursor; past the end it goes to the end. */
    td_rect_t r = td_widget_rect(tb);
    click(r.x + 1, r.y);
    CHECK_EQ(tb->value, 1);
    click(r.x + 8, r.y);
    CHECK_EQ(tb->value, 3);
}

static void test_textbox_own_buffer(void)
{
    td_window_t *w = new_window();
    td_widget_t *tb = td_textbox(w, 0, 0, 30, 10, NULL, NULL);
    static char url[100] = "https://";
    td_textbox_set_buffer(tb, url, (int)sizeof(url));
    CHECK(td_widget_text(tb) == url);
    CHECK_EQ(tb->value, 8);

    /* Longer than TD_TEXT_MAX, up to the buffer's size. */
    for (int i = 0; i < 120; i++) type("x");
    CHECK_EQ((int)strlen(url), (int)sizeof(url) - 1);
}

static void test_textbox_paste(void)
{
    td_window_t *w = new_window();
    td_widget_t *tb = td_textbox(w, 0, 0, 10, 8, NULL, NULL);
    type("<>");
    key(TD_KEY_LEFT, 0);

    /* Only the first line; a tab becomes a space, control characters go. */
    paste("ab\tc\x01" "d\r\nsecond line");
    CHECK(text_is(tb, "<ab cd>"));

    /* Up to the length limit. */
    paste("123456");
    CHECK(text_is(tb, "<ab cd1>"));
}

/* ---------------------------------------------------------------- list */

static char s_item[16];
static const char *list_item(td_widget_t *w, int index, int *fg, void *user)
{
    (void)w;
    (void)fg;
    (void)user;
    snprintf(s_item, sizeof(s_item), "item %d", index);
    return s_item;
}

static void test_list(void)
{
    td_window_t *w = new_window();
    td_widget_t *l = td_list(w, td_rect(0, 0, 20, 5), list_item, on_activate, NULL);

    /* An empty list has no selection, and Enter does nothing. */
    CHECK_EQ(td_list_selected(l), -1);
    s_activated = 0;
    key(TD_KEY_ENTER, 0);
    CHECK_EQ(s_activated, 0);

    td_list_set_count(l, 50);
    CHECK_EQ(td_list_selected(l), 0);

    /* The keys move the selection, and the view follows it. */
    for (int i = 0; i < 3; i++) key(TD_KEY_DOWN, 0);
    CHECK_EQ(td_list_selected(l), 3);
    CHECK_EQ(l->scroll, 0);
    key(TD_KEY_PGDN, 0);
    CHECK_EQ(td_list_selected(l), 8);
    CHECK_EQ(l->scroll, 4);
    key(TD_KEY_END, 0);
    CHECK_EQ(td_list_selected(l), 49);
    CHECK_EQ(l->scroll, 45);
    key(TD_KEY_PGUP, 0);
    CHECK_EQ(td_list_selected(l), 44);
    CHECK_EQ(l->scroll, 44);
    key(TD_KEY_HOME, 0);
    CHECK_EQ(td_list_selected(l), 0);
    CHECK_EQ(l->scroll, 0);
    key(TD_KEY_UP, 0);
    CHECK_EQ(td_list_selected(l), 0);
    td_list_select(l, 100);
    CHECK_EQ(td_list_selected(l), 49);

    /* Shrinking the list keeps the selection and the view inside it. */
    td_list_set_count(l, 10);
    CHECK_EQ(td_list_selected(l), 9);
    CHECK_EQ(l->scroll, 5);

    /* The wheel scrolls without moving the selection. */
    td_list_select(l, 0);
    td_rect_t r = td_widget_rect(l);
    mouse(TD_MOUSE_PRESS, TD_BUTTON_WHEEL_DOWN, r.x + 1, r.y + 1);
    CHECK_EQ(l->scroll, 3);
    mouse(TD_MOUSE_PRESS, TD_BUTTON_WHEEL_DOWN, r.x + 1, r.y + 1);
    CHECK_EQ(l->scroll, 5);
    mouse(TD_MOUSE_PRESS, TD_BUTTON_WHEEL_UP, r.x + 1, r.y + 1);
    CHECK_EQ(l->scroll, 2);
    CHECK_EQ(td_list_selected(l), 0);

    /* A click selects the row under it; a double click opens it. */
    s_activated = 0;
    click(r.x + 1, r.y + 2);
    CHECK_EQ(td_list_selected(l), 4);
    CHECK_EQ(s_activated, 0);
    double_click(r.x + 1, r.y + 3);
    CHECK_EQ(td_list_selected(l), 5);
    CHECK_EQ(s_activated, 1);

    /* The selected row is drawn highlighted. */
    td_wm_compose(&s_back);
    CHECK_EQ(cell(r.x, r.y + 3)->bg, td_theme()->select_bg);
    CHECK_EQ(cell(r.x + 1, r.y)->ch, 'i');
}

static void test_scrollbar(void)
{
    td_window_t *w = new_window();
    td_widget_t *l = td_list(w, td_rect(0, 0, 20, 5), list_item, NULL, NULL);
    td_widget_t *sb = td_scrollbar(w, 20, 0, 5, l);
    td_list_set_count(l, 50);
    td_rect_t r = td_widget_rect(sb);

    click(r.x, r.y + 4);                       /* down arrow */
    CHECK_EQ(l->scroll, 1);
    click(r.x, r.y);                           /* up arrow */
    CHECK_EQ(l->scroll, 0);
    click(r.x, r.y + 3);                       /* bottom of the track */
    CHECK_EQ(l->scroll, 45);
    click(r.x, r.y + 1);                       /* top of the track */
    CHECK_EQ(l->scroll, 0);

    /* Dragging along the track scrolls with the mouse. */
    mouse(TD_MOUSE_PRESS, TD_BUTTON_LEFT, r.x, r.y + 1);
    mouse(TD_MOUSE_DRAG, TD_BUTTON_LEFT, r.x, r.y + 3);
    CHECK_EQ(l->scroll, 45);
    mouse(TD_MOUSE_RELEASE, TD_BUTTON_LEFT, r.x, r.y + 3);

    /* The scrollbar never moves the selection. */
    CHECK_EQ(td_list_selected(l), 0);
}

/* ------------------------------------------ checkbox, button, progress */

static int s_toggles;
static void on_toggle(td_widget_t *w, void *user) { (void)w; (void)user; s_toggles++; }

static void test_checkbox_button_progress(void)
{
    td_window_t *w = new_window();
    td_widget_t *cb = td_checkbox(w, 0, 0, "Option", false, on_toggle, NULL);
    td_widget_t *btn = td_button(w, 0, 2, "Go", on_activate, NULL);

    /* Space and a click toggle a checkbox; td_checkbox_set does not report. */
    s_toggles = 0;
    key(' ', 0);
    CHECK(td_checkbox_get(cb));
    td_rect_t r = td_widget_rect(cb);
    click(r.x + 5, r.y);
    CHECK(!td_checkbox_get(cb));
    CHECK_EQ(s_toggles, 2);
    td_checkbox_set(cb, true);
    CHECK(td_checkbox_get(cb));
    CHECK_EQ(s_toggles, 2);

    /* A button fires on release, and only if the mouse is still on it. */
    s_activated = 0;
    r = td_widget_rect(btn);
    mouse(TD_MOUSE_PRESS, TD_BUTTON_LEFT, r.x + 1, r.y);
    CHECK(btn->pressed);
    CHECK_EQ(s_activated, 0);
    mouse(TD_MOUSE_DRAG, TD_BUTTON_LEFT, r.x + 20, r.y + 5);
    CHECK(!btn->pressed);
    mouse(TD_MOUSE_RELEASE, TD_BUTTON_LEFT, r.x + 20, r.y + 5);
    CHECK_EQ(s_activated, 0);
    click(r.x + 1, r.y);
    CHECK_EQ(s_activated, 1);
    key(' ', 0);                               /* Space works like Enter */
    CHECK_EQ(s_activated, 2);

    /* A new caption resizes the button: "[ caption ]". */
    td_widget_set_text(btn, "Longer");
    CHECK_EQ(btn->rect.w, 6 + 4);

    /* Progress stays within 0..100 %. */
    td_widget_t *p = td_progress(w, 0, 4, 10);
    td_progress_set(p, 150);
    CHECK_EQ(p->value, 100);
    td_progress_set(p, -5);
    CHECK_EQ(p->value, 0);
    td_progress_set(p, 50);
    td_wm_compose(&s_back);
    r = td_widget_rect(p);
    CHECK_EQ(cell(r.x + 4, r.y)->ch, 0x2588);  /* five of ten cells filled */
    CHECK_EQ(cell(r.x + 5, r.y)->ch, 0x2591);
}

/* --------------------------------------------------------------- focus */

static void test_focus(void)
{
    td_window_t *w = new_window();
    td_widget_t *label = td_label(w, 0, 0, 10, "Label");
    td_widget_t *a = td_button(w, 0, 1, "A", NULL, NULL);
    td_widget_t *b = td_checkbox(w, 0, 2, "B", false, NULL, NULL);
    td_widget_t *c = td_textbox(w, 0, 3, 10, 10, NULL, NULL);

    /* Labels never take the focus; Tab follows creation order and wraps. */
    CHECK(w->focus == a);
    td_widget_focus(label);
    CHECK(w->focus == a);
    key(TD_KEY_TAB, 0);
    CHECK(w->focus == b);
    key(TD_KEY_TAB, 0);
    CHECK(w->focus == c);
    key(TD_KEY_TAB, 0);
    CHECK(w->focus == a);
    key(TD_KEY_TAB, TD_MOD_SHIFT);
    CHECK(w->focus == c);

    /* Hidden widgets are skipped; hiding the focused one moves the focus on. */
    td_widget_set_visible(b, false);
    key(TD_KEY_TAB, 0);
    CHECK(w->focus == a);
    key(TD_KEY_TAB, 0);
    CHECK(w->focus == c);
    td_widget_set_visible(c, false);
    CHECK(w->focus == a);
}

/* ---------------------------------------------------------- dialogs */

static int s_answer;
static void on_answer(int button, void *user) { (void)user; s_answer = button; }

static td_window_t *s_next;
static void on_answer_open_next(int button, void *user)
{
    (void)user;
    if (button == 0) s_next = td_msgbox("Next", "Second question", "OK", NULL, NULL);
}

static td_window_t *ask(void) { return td_msgbox("Q", "Line one\nLine two", "Yes|No|Cancel", on_answer, NULL); }

static void test_msgbox(void)
{
    td_window_t *w = new_window();

    /* Enter chooses the focused button, the first one. */
    s_answer = -2;
    td_window_t *m = ask();
    CHECK(m && (m->flags & TD_WIN_MODAL));
    CHECK(td_win_focused() == m);
    key(TD_KEY_ENTER, 0);
    CHECK_EQ(s_answer, 0);
    CHECK(!td_win_is_open(m));
    CHECK(td_win_focused() == w);

    /* Tab moves to the next button. */
    ask();
    key(TD_KEY_TAB, 0);
    key(TD_KEY_ENTER, 0);
    CHECK_EQ(s_answer, 1);

    /* A click on a button. */
    m = ask();
    td_rect_t r = td_widget_rect(find_button(m, "Cancel"));
    click(r.x + 1, r.y);
    CHECK_EQ(s_answer, 2);
    CHECK(!td_win_is_open(m));

    /* Esc and the [x] button answer -1. */
    s_answer = -2;
    m = ask();
    key(TD_KEY_ESC, 0);
    CHECK_EQ(s_answer, -1);
    s_answer = -2;
    m = ask();
    click(m->rect.x + m->rect.w - 3, m->rect.y);
    CHECK_EQ(s_answer, -1);
    CHECK(!td_win_is_open(m));

    /* While it is open, the window behind cannot take the focus. */
    m = ask();
    click(w->rect.x + 5, w->rect.y + 5);
    CHECK(td_win_focused() == m);
    key(TD_KEY_ESC, 0);

    /* The box is closed before its callback runs, so the callback can open
     * the next one. */
    s_next = NULL;
    td_msgbox("Q", "First question", "OK", on_answer_open_next, NULL);
    key(TD_KEY_ENTER, 0);
    CHECK(td_win_is_open(s_next));
    CHECK(td_win_focused() == s_next);
}

static char s_entered[64];
static int s_entries;
static void on_text(const char *text, void *user)
{
    (void)user;
    snprintf(s_entered, sizeof(s_entered), "%s", text);
    s_entries++;
}

static void test_inputbox(void)
{
    new_window();
    s_entries = 0;

    /* It starts with the given text; Enter hands over the text and closes. */
    td_window_t *ib = td_inputbox("Name", "Your name:", "Bo", on_text, NULL);
    CHECK(ib && td_win_focused() == ib);
    type("b");
    key(TD_KEY_ENTER, 0);
    CHECK(strcmp(s_entered, "Bob") == 0);
    CHECK_EQ(s_entries, 1);
    CHECK(!td_win_is_open(ib));

    /* OK does the same. */
    ib = td_inputbox("Name", "Your name:", "Al", on_text, NULL);
    td_rect_t r = td_widget_rect(find_button(ib, "OK"));
    click(r.x + 1, r.y);
    CHECK(strcmp(s_entered, "Al") == 0);
    CHECK_EQ(s_entries, 2);

    /* Cancel and Esc hand over nothing. */
    ib = td_inputbox("Name", "Your name:", "", on_text, NULL);
    r = td_widget_rect(find_button(ib, "Cancel"));
    click(r.x + 1, r.y);
    CHECK(!td_win_is_open(ib));
    ib = td_inputbox("Name", "Your name:", "", on_text, NULL);
    key(TD_KEY_ESC, 0);
    CHECK(!td_win_is_open(ib));
    CHECK_EQ(s_entries, 2);

    /* A password box shows stars, never the text. */
    ib = td_passwordbox("Login", "Password:", on_text, NULL);
    type("pw");
    td_wm_compose(&s_back);
    r = td_widget_rect(ib->focus);
    CHECK_EQ(cell(r.x, r.y)->ch, '*');
    CHECK_EQ(cell(r.x + 1, r.y)->ch, '*');
    key(TD_KEY_ENTER, 0);
    CHECK(strcmp(s_entered, "pw") == 0);
}

/* ------------------------------------------------------------- pools */

static void test_pools_keep_room_for_a_message_box(void)
{
    /* The last window slot is kept for a message box. */
    td_wm_init(80, 25);
    td_window_desc_t d = { .title = "W", .rect = td_rect(0, 0, 20, 5), .flags = TD_WIN_DEFAULT };
    int opened = 0;
    while (opened <= TD_MAX_WINDOWS && td_win_create(&d)) opened++;
    CHECK_EQ(opened, TD_MAX_WINDOWS - 1);
    CHECK(td_msgbox("Full", "Too many windows", "OK", NULL, NULL) != NULL);

    /* A window that wants more widgets than the shared pool can spare is
     * marked incomplete, and a message box still gets its widgets. */
    td_window_t *w = new_window();
    int made = 0;
    while (made <= TD_MAX_WIDGETS && td_label(w, 0, 0, 5, "x")) made++;
    CHECK(w->incomplete);
    CHECK(made > 0 && made < TD_MAX_WIDGETS);
    td_window_t *m = td_msgbox("Full", "No widgets left", "OK", NULL, NULL);
    CHECK(m != NULL && find_button(m, "OK") != NULL);
}

int main(void)
{
    td_init(&s_hal);   /* no size answer: 80x25 */
    test_textbox();
    test_textbox_own_buffer();
    test_textbox_paste();
    test_list();
    test_scrollbar();
    test_checkbox_button_progress();
    test_focus();
    test_msgbox();
    test_inputbox();
    test_pools_keep_room_for_a_message_box();
    return TD_TEST_RESULT();
}
