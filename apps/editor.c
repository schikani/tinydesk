/*
 * editor.c - a small text editor window.
 *
 *   arrows, Home/End, PgUp/PgDn, Ctrl+Home/End,      move (hold Shift to
 *   Ctrl+Left/Right (by word)                          select)
 *   mouse: click places the cursor, drag or Shift+click selects,
 *          double-click selects a word, the wheel scrolls, right-click menu
 *   typing, Enter, Tab (4 spaces), Backspace, Delete   edit (replace or
 *                                                       delete a selection)
 *   Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
 *   (also Ctrl+Insert, Shift+Delete, Shift+Insert)
 *   Ctrl+Z undo, Ctrl+Y redo
 *   Ctrl+S save, Ctrl+W or Esc close (asks about unsaved changes)
 *
 * The text lives in one flat buffer that is allocated while the window is
 * open (TD_EDITOR_MAX bytes), with the undo history next to it. Files larger
 * than that open read-only. Line endings are stored as LF; CR characters are
 * dropped when loading.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td_apps.h"

#ifndef TD_EDITOR_MAX
#define TD_EDITOR_MAX 16384
#endif
#ifndef TD_EDITOR_UNDO
#define TD_EDITOR_UNDO 6144      /* bytes of undo text kept */
#endif
#define UNDO_OPS 128
#define PATH_LEN TD_PATH_MAX
#define DOUBLE_CLICK_MS 400

/* One recorded change. Undo walks back one group (a user action: a typed
 * word, a paste, a cut...) at a time. */
typedef struct {
    int pos, len;        /* where, how many bytes */
    int text;            /* offset of the bytes in the pool */
    int cur;             /* cursor before the action */
    uint16_t group;
    bool ins;            /* insertion (else deletion) */
} undo_op_t;

typedef enum { K_OTHER, K_TYPE, K_BACKSPACE, K_DELETE } edit_kind_t;

typedef struct {
    undo_op_t ops[UNDO_OPS];
    int n;               /* ops recorded */
    int top;             /* ops applied; ops[top..n) can be redone */
    char pool[TD_EDITOR_UNDO];
    int used;
    uint16_t group;
    edit_kind_t kind;    /* kind of the last action, for merging */
    bool merge;          /* the next edit may join the last group */
    bool after_space;    /* last typed char was a space: a new word starts */
    int saved;           /* top when the file was last saved; -1: unknown */
    uint16_t broken;     /* group that did not fit: record none of it */
} undo_t;

typedef struct {
    char *buf;
    int len;
    int cur;            /* cursor as a byte offset */
    int anchor;         /* other end of the selection, -1 for none */
    int top;            /* first visible line */
    int left;           /* first visible column */
    int goal_col;       /* column kept while moving up and down */
    bool follow;        /* scroll to the cursor on the next draw */
    bool modified;
    bool readonly;
    bool close_after_save;
    bool mouse_down;
    uint32_t click_ms;
    int click_pos;
    char path[PATH_LEN];     /* "" for a new, unnamed file */
    char status[48];
    uint32_t status_ms;
    int view_rows;      /* text rows in the window at the last draw */
    undo_t *undo;
} editor_t;

static editor_t E;
static td_window_t *s_win;

static const td_fs_ops_t *fs(void) { return td_sysinfo()->fs; }

static void set_status(const char *msg)
{
    snprintf(E.status, sizeof(E.status), "%s", msg);
    E.status_ms = td_millis();
    td_wm_invalidate();
}

static const char *file_name(void)
{
    if (!E.path[0]) return "untitled";
    const char *slash = strrchr(E.path, '/');
    return slash ? slash + 1 : E.path;
}

static void update_title(void)
{
    char title[TD_TITLE_MAX];
    /* "*" + name + " - Editor": the name is cut to fit the title. */
    snprintf(title, sizeof(title), "%s%.*s - Editor", E.modified ? "*" : "", (int)sizeof(title) - 11, file_name());
    td_win_set_title(s_win, title);
}

/* ------------------------------------------------------------ clipboard */

static char *s_clip;
static int s_clip_len;

bool td_clipboard_set(const char *text, int len)
{
    char *copy = malloc((size_t)len + 1);
    if (!copy) return false;
    memcpy(copy, text, (size_t)len);
    copy[len] = '\0';
    free(s_clip);
    s_clip = copy;
    s_clip_len = len;
    return true;
}

const char *td_clipboard_get(int *len)
{
    if (len) *len = s_clip ? s_clip_len : 0;
    return s_clip;
}

void td_clipboard_clear(void)
{
    free(s_clip);
    s_clip = NULL;
    s_clip_len = 0;
}

/* ------------------------------------------------ positions in the text */

static bool is_cont(char c) { return ((unsigned char)c & 0xC0u) == 0x80u; }

static int next_pos(int p)
{
    if (p >= E.len) return E.len;
    p++;
    while (p < E.len && is_cont(E.buf[p])) p++;
    return p;
}

static int prev_pos(int p)
{
    if (p <= 0) return 0;
    p--;
    while (p > 0 && is_cont(E.buf[p])) p--;
    return p;
}

static int line_start(int p)
{
    while (p > 0 && E.buf[p - 1] != '\n') p--;
    return p;
}

static int line_end(int p)
{
    while (p < E.len && E.buf[p] != '\n') p++;
    return p;
}

/* Column (in characters) of position p within its line. */
static int col_of(int p)
{
    int c = 0;
    for (int i = line_start(p); i < p; i++)
        if (!is_cont(E.buf[i])) c++;
    return c;
}

static int line_of(int p)
{
    int n = 0;
    for (int i = 0; i < p; i++)
        if (E.buf[i] == '\n') n++;
    return n;
}

static int line_count(void) { return line_of(E.len) + 1; }

/* Start of line n (clamped to the last line). */
static int nth_line(int n)
{
    int p = 0;
    for (int line = 0; line < n; line++) {
        int e = line_end(p);
        if (e >= E.len) break;
        p = e + 1;
    }
    return p;
}

/* Position at column col of the line starting at ls (or its end). */
static int at_col(int ls, int col)
{
    int p = ls, e = line_end(ls);
    for (int c = 0; c < col && p < e; c++) p = next_pos(p);
    return p;
}

/* Letters, digits, '_' and anything non-ASCII count as word characters. */
static bool word_char(char c)
{
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || u == '_' || (u >= '0' && u <= '9') || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z');
}

static int word_left(int p)
{
    while (p > 0 && !word_char(E.buf[p - 1])) p--;
    while (p > 0 && word_char(E.buf[p - 1])) p--;
    return p;
}

static int word_right(int p)
{
    while (p < E.len && word_char(E.buf[p])) p++;
    while (p < E.len && !word_char(E.buf[p])) p++;
    return p;
}

/* ---------------------------------------------------------- selection */

static bool has_sel(void) { return E.anchor >= 0 && E.anchor != E.cur; }

static void sel_range(int *a, int *b)
{
    *a = E.anchor < E.cur ? E.anchor : E.cur;
    *b = E.anchor < E.cur ? E.cur : E.anchor;
}

/* Number of characters selected. */
static int sel_chars(void)
{
    int a, b, n = 0;
    sel_range(&a, &b);
    for (int i = a; i < b; i++)
        if (!is_cont(E.buf[i])) n++;
    return n;
}

/* --------------------------------------------------- raw buffer edits */

static void raw_insert(int pos, const char *text, int n)
{
    memmove(E.buf + pos + n, E.buf + pos, (size_t)(E.len - pos));
    memcpy(E.buf + pos, text, (size_t)n);
    E.len += n;
    E.buf[E.len] = '\0';   /* keeps UTF-8 decoding in bounds */
}

static void raw_erase(int from, int to)
{
    memmove(E.buf + from, E.buf + to, (size_t)(E.len - to));
    E.len -= to - from;
    E.buf[E.len] = '\0';
}

static void after_change(void)
{
    E.modified = !E.undo || E.undo->top != E.undo->saved;
    update_title();
    E.follow = true;
    td_wm_invalidate();
}

/* ------------------------------------------------------------ undo */

/* Forget the oldest action to make room. */
static void undo_drop_oldest(undo_t *u)
{
    int k = 0;
    while (k < u->n && u->ops[k].group == u->ops[0].group) k++;
    int bytes = k < u->n ? u->ops[k].text : u->used;
    memmove(u->pool, u->pool + bytes, (size_t)(u->used - bytes));
    u->used -= bytes;
    memmove(u->ops, u->ops + k, (size_t)(u->n - k) * sizeof(undo_op_t));
    u->n -= k;
    u->top = u->top > k ? u->top - k : 0;
    for (int i = 0; i < u->n; i++) u->ops[i].text -= bytes;
    u->saved = u->saved >= k ? u->saved - k : -1;
}

/* Start a user action. Typing, Backspace and Delete runs join the previous
 * action (typing one word at a time); anything else starts a new one. */
static void undo_begin(edit_kind_t kind, bool space)
{
    undo_t *u = E.undo;
    if (!u) return;
    bool join = u->merge && kind != K_OTHER && kind == u->kind && u->top == u->n && u->n > 0;
    if (join && kind == K_TYPE && u->after_space && !space) join = false;   /* next word */
    if (!join) u->group++;
    u->kind = kind;
    u->after_space = space;
    u->merge = kind != K_OTHER;
}

static void undo_record(bool ins, int pos, const char *text, int len, int cur_before)
{
    undo_t *u = E.undo;
    if (!u || len <= 0 || u->broken == u->group) return;
    if (u->top < u->n) {                      /* a new edit ends the redo chain */
        u->used = u->ops[u->top].text;
        u->n = u->top;
        if (u->saved > u->top) u->saved = -1;
    }
    if (len > TD_EDITOR_UNDO) {               /* too big to keep: start over */
        u->n = u->top = u->used = 0;
        u->saved = -1;
        u->broken = u->group;
        return;
    }

    /* Extend the last op when this continues it. */
    undo_op_t *last = u->n > 0 ? &u->ops[u->n - 1] : NULL;
    if (last && last->group == u->group && last->ins == ins && last->text + last->len == u->used &&
        u->used + len <= TD_EDITOR_UNDO) {
        if (ins && last->pos + last->len == pos) {                       /* typing on */
            memcpy(u->pool + u->used, text, (size_t)len);
        } else if (!ins && pos + len == last->pos) {                     /* Backspace */
            memmove(u->pool + last->text + len, u->pool + last->text, (size_t)last->len);
            memcpy(u->pool + last->text, text, (size_t)len);
            last->pos = pos;
        } else if (!ins && pos == last->pos) {                           /* Delete */
            memcpy(u->pool + u->used, text, (size_t)len);
        } else {
            last = NULL;
        }
        if (last) {
            last->len += len;
            u->used += len;
            return;
        }
    }

    while (u->n > 0 && (u->n >= UNDO_OPS || u->used + len > TD_EDITOR_UNDO)) {
        if (u->ops[0].group == u->group) {    /* only this action is left */
            u->n = u->top = u->used = 0;
            u->saved = -1;
            u->broken = u->group;
            return;
        }
        undo_drop_oldest(u);
    }
    undo_op_t *op = &u->ops[u->n++];
    op->pos = pos;
    op->len = len;
    op->text = u->used;
    op->cur = cur_before;
    op->group = u->group;
    op->ins = ins;
    memcpy(u->pool + u->used, text, (size_t)len);
    u->used += len;
    u->top = u->n;
}

static void undo(void)
{
    undo_t *u = E.undo;
    if (!u || u->top == 0) {
        set_status("Nothing to undo");
        return;
    }
    uint16_t g = u->ops[u->top - 1].group;
    while (u->top > 0 && u->ops[u->top - 1].group == g) {
        const undo_op_t *op = &u->ops[--u->top];
        if (op->ins) raw_erase(op->pos, op->pos + op->len);
        else raw_insert(op->pos, u->pool + op->text, op->len);
        E.cur = op->cur;
    }
    u->merge = false;
    E.anchor = -1;
    after_change();
}

static void redo(void)
{
    undo_t *u = E.undo;
    if (!u || u->top == u->n) {
        set_status("Nothing to redo");
        return;
    }
    uint16_t g = u->ops[u->top].group;
    while (u->top < u->n && u->ops[u->top].group == g) {
        const undo_op_t *op = &u->ops[u->top++];
        if (op->ins) {
            raw_insert(op->pos, u->pool + op->text, op->len);
            E.cur = op->pos + op->len;
        } else {
            raw_erase(op->pos, op->pos + op->len);
            E.cur = op->pos;
        }
    }
    u->merge = false;
    E.anchor = -1;
    after_change();
}

/* ---------------------------------------------------------- editing */

static bool writable(void)
{
    if (E.readonly) set_status("Read-only: the file is too large to edit");
    return !E.readonly;
}

/* Delete [from, to) as part of the current action. */
static void erase_recorded(int from, int to)
{
    if (from >= to) return;
    undo_record(false, from, E.buf + from, to - from, E.cur);
    raw_erase(from, to);
    E.cur = from;
}

/* Delete the selection (if any) as part of the current action. */
static void erase_selection(void)
{
    if (!has_sel()) return;
    int a, b;
    sel_range(&a, &b);
    erase_recorded(a, b);
    E.anchor = -1;
}

/* Type or paste text, replacing the selection. */
static void insert_text(const char *text, int n, edit_kind_t kind)
{
    if (!writable() || n <= 0) return;
    int a = E.cur, b = E.cur;
    if (has_sel()) sel_range(&a, &b);
    if (E.len - (b - a) + n > TD_EDITOR_MAX) {
        set_status("The file is full");
        return;
    }
    bool space = n == 1 && (text[0] == ' ' || text[0] == '\n');
    bool replace = has_sel();
    undo_begin(replace ? K_OTHER : kind, space);
    if (replace && kind == K_TYPE && E.undo) {    /* typing on joins this step */
        E.undo->kind = K_TYPE;
        E.undo->merge = true;
    }
    erase_selection();
    undo_record(true, E.cur, text, n, E.cur);
    raw_insert(E.cur, text, n);
    E.cur += n;
    E.anchor = -1;
    after_change();
}

/* Backspace / Delete: the selection, or one character. */
static void delete_key(bool backwards)
{
    if (!writable()) return;
    if (has_sel()) {
        undo_begin(K_OTHER, false);
        erase_selection();
    } else {
        int from = backwards ? prev_pos(E.cur) : E.cur;
        int to = backwards ? E.cur : next_pos(E.cur);
        if (from >= to) return;
        undo_begin(backwards ? K_BACKSPACE : K_DELETE, false);
        erase_recorded(from, to);
    }
    after_change();
}

static void copy_selection(void)
{
    if (!has_sel()) {
        set_status("Nothing selected");
        return;
    }
    int a, b;
    sel_range(&a, &b);
    if (td_clipboard_set(E.buf + a, b - a)) {
        td_host_clipboard_set(E.buf + a, b - a);    /* the PC's clipboard, where the terminal allows */
        char msg[32];
        snprintf(msg, sizeof(msg), "Copied %d characters", sel_chars());
        set_status(msg);
    } else {
        set_status("Not enough memory to copy");
    }
}

static void cut_selection(void)
{
    if (!has_sel() || !writable()) {
        if (!has_sel()) set_status("Nothing selected");
        return;
    }
    copy_selection();
    undo_begin(K_OTHER, false);
    erase_selection();
    after_change();
}

static void paste(void)
{
    int n = 0;
    const char *text = td_clipboard_get(&n);
    if (!text || n == 0) {
        set_status("The clipboard is empty");
        return;
    }
    insert_text(text, n, K_OTHER);
}

/* Text pasted in the terminal (from the PC's clipboard): insert it as
 * one step, with CR / CRLF line ends turned into LF. */
static void paste_from_terminal(const td_event_t *ev)
{
    int n = 0;
    const char *text = td_paste_text(&n);
    if (!text || n <= 0) return;
    char *buf = malloc((size_t)n);
    if (!buf) {
        set_status("Not enough memory to paste");
        return;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (text[i] == '\r') {
            buf[m++] = '\n';
            if (i + 1 < n && text[i + 1] == '\n') i++;
        } else if (text[i] != '\0') {
            buf[m++] = text[i];
        }
    }
    insert_text(buf, m, K_OTHER);
    td_clipboard_set(buf, m);                  /* Ctrl+V pastes it again */
    free(buf);
    char msg[48];
    snprintf(msg, sizeof(msg), "Pasted %d characters%s", m, ev->x ? " (cut: too long)" : "");
    set_status(msg);
}

static void select_all(void)
{
    E.anchor = 0;
    E.cur = E.len;
    E.follow = true;
    td_wm_invalidate();
}

static void move_lines(int delta)
{
    int line = line_of(E.cur) + delta;
    if (line < 0) line = 0;
    if (line >= line_count()) line = line_count() - 1;
    E.cur = at_col(nth_line(line), E.goal_col);
}

/* ------------------------------------------------------ load & save */

static bool load(const char *path)
{
    E.len = 0;
    E.buf[0] = '\0';
    E.readonly = false;
    if (!path || !path[0]) return true;
    int n = fs()->read(path, E.buf, TD_EDITOR_MAX + 1);
    if (n < 0) return false;
    if (n > TD_EDITOR_MAX) {
        n = TD_EDITOR_MAX;
        E.readonly = true;
    }
    int out = 0;
    for (int i = 0; i < n; i++)
        if (E.buf[i] != '\r') E.buf[out++] = E.buf[i];
    E.len = out;
    E.buf[E.len] = '\0';
    return true;
}

static void close_now(void)
{
    E.modified = false;
    td_win_close(s_win);
}

static bool save_to(const char *path)
{
    if (fs()->write(path, E.buf, E.len) != 0) {
        set_status("Save failed");
        return false;
    }
    /* save() passes E.path itself; copying a string onto itself is
     * undefined (glibc empties it, and the next save asks for a name). */
    if (path != E.path) snprintf(E.path, sizeof(E.path), "%s", path);
    if (E.undo) {
        E.undo->saved = E.undo->top;
        E.undo->merge = false;       /* typing after a save is a new step */
    }
    E.modified = false;
    update_title();
    set_status("Saved");
    td_desktop_refresh();
    if (E.close_after_save) close_now();
    return true;
}

static void save_as_answer(const char *name, void *user)
{
    (void)user;
    char path[PATH_LEN];
    if (!td_valid_name(name)) {
        set_status("Invalid file name");
        E.close_after_save = false;
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", td_desktop_dir(), name);
    if (fs()->exists && fs()->exists(path)) {
        set_status("A file with that name already exists");
        E.close_after_save = false;
        return;
    }
    save_to(path);
}

static void save(void)
{
    if (E.readonly) {
        set_status("Read-only: not saved");
        return;
    }
    if (E.path[0]) save_to(E.path);
    else td_inputbox("Save as", "File name (saved on the Desktop):", "untitled.txt", save_as_answer, NULL);
}

static void unsaved_answer(int button, void *user)
{
    (void)user;
    if (button == 0) {          /* Save */
        E.close_after_save = true;
        save();
    } else if (button == 1) {   /* Discard */
        close_now();
    }
}

/* [x], window menu Close, Esc and Ctrl+W come here first. */
static bool on_close_request(td_window_t *win)
{
    (void)win;
    if (!E.modified) return true;
    char text[TD_TEXT_MAX];
    snprintf(text, sizeof(text), "Save changes to %.24s?", file_name());
    td_msgbox("Editor", text, "Save|Discard|Cancel", unsaved_answer, NULL);
    return false;
}

/* ---------------------------------------------------------- drawing */

static void on_draw(td_window_t *win, int w, int h)
{
    const td_theme_t *t = td_theme();
    int rows = h - 1;               /* the last row is the status bar */
    E.view_rows = rows > 1 ? rows : 1;
    int cl = line_of(E.cur), cc = col_of(E.cur);

    if (E.follow) {
        if (cl < E.top) E.top = cl;
        if (cl >= E.top + E.view_rows) E.top = cl - E.view_rows + 1;
        if (cc < E.left) E.left = cc;
        if (cc >= E.left + w) E.left = cc - w + 1;
        E.follow = false;
    }

    int sa = 0, sb = 0;
    if (has_sel()) sel_range(&sa, &sb);

    td_fill(td_rect(0, 0, w, rows), ' ', t->input_fg, t->input_bg);
    int p = nth_line(E.top);
    for (int row = 0; row < rows; row++) {
        if (E.top + row >= line_count()) break;
        int e = line_end(p);
        const char *s = E.buf + p;
        int col = 0;
        while (s < E.buf + e) {
            int at = (int)(s - E.buf);
            uint32_t cp = td_utf8_next(&s);
            if (cp == 0) {              /* a NUL byte: show it, step over it */
                cp = '?';
                s++;
            }
            if (s > E.buf + e) break;
            if (cp == '\t') cp = ' ';
            else if (cp < 0x20u) cp = '?';
            bool sel = at >= sa && at < sb;
            if (col >= E.left && col - E.left < w)
                td_putc(col - E.left, row, cp, sel ? t->select_fg : t->input_fg, sel ? t->select_bg : t->input_bg, 0);
            col++;
        }
        /* A selected line break shows as one highlighted cell. */
        if (e < E.len && e >= sa && e < sb && col >= E.left && col - E.left < w)
            td_putc(col - E.left, row, ' ', t->select_fg, t->select_bg, 0);
        p = e + 1;
    }

    if (win == td_win_focused() && cl >= E.top && cl < E.top + rows && cc >= E.left && cc - E.left < w) {
        uint32_t ch = ' ';
        if (E.cur < E.len && E.buf[E.cur] != '\n') {
            const char *s = E.buf + E.cur;
            ch = td_utf8_next(&s);
            if (ch < 0x20u) ch = ' ';
        }
        td_putc(cc - E.left, cl - E.top, ch, t->input_fg, t->input_bg, TD_REVERSE);
    }

    /* Status bar. */
    char bar[176], sel[24] = "";
    bool recent = E.status[0] && td_millis() - E.status_ms < 4000;
    if (has_sel()) snprintf(sel, sizeof(sel), "  %d selected", sel_chars());
    snprintf(bar, sizeof(bar), " Ln %d, Col %d%s%s  %s", cl + 1, cc + 1, sel, E.readonly ? "  [read-only]" : "",
             recent ? E.status : "^S Save  ^Z Undo  ^Y Redo  ^C Copy  ^X Cut  ^V Paste");
    td_fill(td_rect(0, rows, w, 1), ' ', t->title_inactive_fg, t->title_inactive_bg);
    td_textn(0, rows, bar, w, t->title_inactive_fg, t->title_inactive_bg, 0);
}

/* ------------------------------------------------------------ input */

static bool on_key(const td_event_t *ev)
{
    uint32_t k = ev->key;
    bool ctrl = (ev->mods & TD_MOD_CTRL) != 0;
    bool shift = (ev->mods & TD_MOD_SHIFT) != 0;

    if (ctrl && k >= 'a' && k <= 'z') {
        switch (k) {
        case 's': save(); return true;
        case 'w': case 'q': td_win_request_close(s_win); return true;
        case 'a': select_all(); return true;
        case 'c': copy_selection(); return true;
        case 'x': cut_selection(); return true;
        case 'v': paste(); return true;
        case 'z': if (shift) redo(); else undo(); return true;
        case 'y': redo(); return true;
        default: break;
        }
    }
    if (k == TD_KEY_ESC) {
        td_win_request_close(s_win);
        return true;
    }
    if (k == TD_KEY_INSERT) {
        if (ctrl) copy_selection();
        else if (shift) paste();
        return true;
    }
    if (k == TD_KEY_DELETE && shift) {
        cut_selection();
        return true;
    }

    /* Movement; Shift extends the selection. */
    bool keep_goal = false, moved = true;
    int before = E.cur;
    bool sel = has_sel();
    int sa = 0, sb = 0;
    if (sel) sel_range(&sa, &sb);
    switch (k) {
    case TD_KEY_LEFT:
        if (sel && !shift) E.cur = sa;
        else E.cur = ctrl ? word_left(E.cur) : prev_pos(E.cur);
        break;
    case TD_KEY_RIGHT:
        if (sel && !shift) E.cur = sb;
        else E.cur = ctrl ? word_right(E.cur) : next_pos(E.cur);
        break;
    case TD_KEY_UP: move_lines(-1); keep_goal = true; break;
    case TD_KEY_DOWN: move_lines(1); keep_goal = true; break;
    case TD_KEY_PGUP: move_lines(-E.view_rows); keep_goal = true; break;
    case TD_KEY_PGDN: move_lines(E.view_rows); keep_goal = true; break;
    case TD_KEY_HOME: E.cur = ctrl ? 0 : line_start(E.cur); break;
    case TD_KEY_END: E.cur = ctrl ? E.len : line_end(E.cur); break;
    default: moved = false; break;
    }
    if (moved) {
        if (shift) {
            if (E.anchor < 0 || !sel) E.anchor = before;
        } else {
            E.anchor = -1;
        }
        if (E.undo) E.undo->merge = false;   /* typing elsewhere is a new step */
        if (!keep_goal) E.goal_col = col_of(E.cur);
        E.follow = true;
        td_wm_invalidate();
        return true;
    }

    switch (k) {
    case TD_KEY_ENTER: insert_text("\n", 1, K_TYPE); break;
    case TD_KEY_TAB: insert_text("    ", 4, K_TYPE); break;
    case TD_KEY_BACKSPACE: delete_key(true); break;
    case TD_KEY_DELETE: delete_key(false); break;
    default:
        if (ctrl || (ev->mods & TD_MOD_ALT) || k < 0x20 || k >= TD_KEY_BASE) return false;
        {
            uint8_t u[4];
            insert_text((const char *)u, td_utf8_encode(k, u), K_TYPE);
        }
        break;
    }
    E.goal_col = col_of(E.cur);
    E.follow = true;
    td_wm_invalidate();
    return true;
}

/* ------------------------------------------------------ context menu */

enum { M_UNDO, M_REDO, M_SEP1, M_CUT, M_COPY, M_PASTE, M_DELETE, M_SEP2, M_ALL };

static void menu_chosen(int item, void *user)
{
    (void)user;
    if (!td_win_is_open(s_win)) return;
    switch (item) {
    case M_UNDO: undo(); break;
    case M_REDO: redo(); break;
    case M_CUT: cut_selection(); break;
    case M_COPY: copy_selection(); break;
    case M_PASTE: paste(); break;
    case M_DELETE: if (has_sel()) delete_key(false); break;
    case M_ALL: select_all(); break;
    default: break;
    }
}

static void context_menu(int x, int y)
{
    static const char *const items[] = {
        "Undo       Ctrl+Z", "Redo       Ctrl+Y", "-", "Cut        Ctrl+X", "Copy       Ctrl+C",
        "Paste      Ctrl+V", "Delete     Del", "-", "Select all Ctrl+A",
    };
    td_rect_t c = td_win_client(s_win);
    td_menu_popup(c.x + x, c.y + y, items, (int)(sizeof(items) / sizeof(items[0])), menu_chosen, NULL);
}

/* ------------------------------------------------------------- mouse */

/* Text position under client cell (x, y), scrolling when y is outside. */
static int pos_at(int x, int y)
{
    if (y < 0) {
        E.top += y;
        if (E.top < 0) E.top = 0;
        y = 0;
    } else if (y >= E.view_rows) {
        E.top += y - E.view_rows + 1;
        if (E.top > line_count() - 1) E.top = line_count() - 1;
        y = E.view_rows - 1;
    }
    int line = E.top + y;
    if (line >= line_count()) line = line_count() - 1;
    return at_col(nth_line(line), E.left + (x < 0 ? 0 : x));
}

static bool on_mouse(const td_event_t *ev)
{
    if (ev->button == TD_BUTTON_WHEEL_UP || ev->button == TD_BUTTON_WHEEL_DOWN) {
        E.top += ev->button == TD_BUTTON_WHEEL_UP ? -3 : 3;
        if (E.top > line_count() - 1) E.top = line_count() - 1;
        if (E.top < 0) E.top = 0;
        td_wm_invalidate();
        return true;
    }
    if (ev->action == TD_MOUSE_PRESS && ev->button == TD_BUTTON_RIGHT) {
        if (ev->y < E.view_rows && !has_sel()) E.cur = pos_at(ev->x, ev->y);
        context_menu(ev->x, ev->y);
        td_wm_invalidate();
        return true;
    }
    if (ev->action == TD_MOUSE_PRESS && ev->button == TD_BUTTON_LEFT && ev->y < E.view_rows) {
        int p = pos_at(ev->x, ev->y);
        uint32_t now = td_millis();
        if (p == E.click_pos && now - E.click_ms < DOUBLE_CLICK_MS) {
            /* Double-click: select the word (or run of other characters). */
            int a = p, b = p;
            bool wc = p < E.len && word_char(E.buf[p]);
            while (a > 0 && word_char(E.buf[a - 1]) == wc && E.buf[a - 1] != '\n') a--;
            while (b < E.len && word_char(E.buf[b]) == wc && E.buf[b] != '\n') b++;
            E.anchor = a;
            E.cur = b;
            E.click_ms = 0;
            E.mouse_down = false;
        } else {
            if (ev->mods & TD_MOD_SHIFT) {
                if (E.anchor < 0) E.anchor = E.cur;
            } else {
                E.anchor = p;
            }
            E.cur = p;
            E.click_ms = now;
            E.click_pos = p;
            E.mouse_down = true;
        }
        if (E.undo) E.undo->merge = false;
        E.goal_col = col_of(E.cur);
        td_wm_invalidate();
        return true;
    }
    if (ev->action == TD_MOUSE_DRAG && E.mouse_down) {
        E.cur = pos_at(ev->x, ev->y);
        E.goal_col = col_of(E.cur);
        td_wm_invalidate();
        return true;
    }
    if (ev->action == TD_MOUSE_RELEASE && E.mouse_down) {
        E.mouse_down = false;
        if (E.anchor == E.cur) E.anchor = -1;
        td_wm_invalidate();
    }
    return true;
}

static bool on_event(td_window_t *win, const td_event_t *ev)
{
    (void)win;
    if (ev->type == TD_EV_KEY) return on_key(ev);
    if (ev->type == TD_EV_PASTE) {
        paste_from_terminal(ev);
        return true;
    }
    if (ev->type == TD_EV_MOUSE) return on_mouse(ev);
    return false;
}

/* A file dropped on the Editor is opened (unless there are unsaved changes). */
static bool on_drop(td_window_t *win, int x, int y, const td_drag_item_t *item)
{
    (void)win;
    (void)x;
    (void)y;
    if (!item->is_dir) td_editor_open(item->path);
    return true;
}

static void on_close(td_window_t *win)
{
    (void)win;
    free(E.buf);
    free(E.undo);
    E.buf = NULL;
    E.undo = NULL;
    s_win = NULL;
}

/* -------------------------------------------------------------- api */

void td_editor_open(const char *path)
{
    if (!fs() || !fs()->read || !fs()->write) {
        td_msgbox("Editor", "No filesystem on this platform.", "OK", NULL, NULL);
        return;
    }
    if (td_win_is_open(s_win)) {
        if (path && strcmp(path, E.path) == 0) {
            td_win_focus(s_win);
            return;
        }
        if (E.modified) {
            td_win_focus(s_win);
            set_status("Save or close this file first");
            return;
        }
        td_win_close(s_win);
    }

    memset(&E, 0, sizeof(E));
    E.anchor = -1;
    E.click_pos = -1;
    E.buf = malloc(TD_EDITOR_MAX + 1);
    E.undo = calloc(1, sizeof(undo_t));   /* without it: no undo, still edits */
    if (!E.buf) {
        free(E.undo);
        E.undo = NULL;
        td_msgbox("Editor", "Not enough memory.", "OK", NULL, NULL);
        return;
    }
    if (!load(path)) {
        free(E.buf);
        free(E.undo);
        E.buf = NULL;
        E.undo = NULL;
        td_msgbox("Editor", "The file could not be read.", "OK", NULL, NULL);
        return;
    }
    if (path) snprintf(E.path, sizeof(E.path), "%s", path);

    td_window_desc_t d = {
        .title = "Editor",
        .rect = td_rect(-1, -1, 72, 20),
        .flags = TD_WIN_DEFAULT | TD_WIN_RAW_KEYS,
        .min_w = 30,
        .min_h = 6,
        .on_draw = on_draw,
        .on_event = on_event,
        .on_close = on_close,
        .on_close_request = on_close_request,
        .on_drop = on_drop,
    };
    s_win = td_win_create(&d);
    if (!s_win) {
        free(E.buf);
        free(E.undo);
        E.buf = NULL;
        E.undo = NULL;
        return;
    }
    E.follow = true;
    update_title();
    if (E.readonly) set_status("File too large: read-only");
    else if (!E.undo) set_status("Low memory: undo is off");
}

static void launch(void)
{
    if (td_win_is_open(s_win)) td_win_focus(s_win);
    else td_editor_open(NULL);
}

static const td_app_t s_app = { "Editor", launch, "\xC2\xB6_" };

void td_editor_register(void) { td_app_register(&s_app); }
