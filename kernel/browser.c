/* Browser core: fetch, a small HTML-to-lines renderer, history.
 *
 * Pages come from http:// (kernel/net.c), from the file system ("/docs/x.html"
 * or file:///docs/x.html; a folder becomes a link list) or are built in
 * (about:home, about:help). The renderer keeps text, headings, paragraphs,
 * lists, links, <pre> and <hr>; scripts, styles and everything else are
 * dropped. The desktop draws the lines (kernel/desktop.c, «Браузер»); the
 * shell prints them (browse URL). */
#include "browser.h"
#include "fs_limits.h"

extern void console_puts(const char *text);
extern void console_putc(char c);
extern int fs_get(const char *name, const unsigned char **data, unsigned int *length);
extern int fs_dir_entry(int index, const char *dir, char *name, unsigned int name_cap, int *is_dir);
extern int net_http_get(const char *url, char *out, unsigned int cap, unsigned int *out_len, char *loc,
                        unsigned int loc_cap);
extern int desktop_active(void);
extern void desktop_browse(const char *url);
extern void desktop_web(int action, int arg);

enum { SRC_CAP = 96 * 1024, HREF_POOL = 12 * 1024, HIST_MAX = 16, COLS_MAX = 96 };

struct web_line_s { char text[WEB_LINE_CAP]; unsigned char style; };

static char src[SRC_CAP];
static unsigned int src_len;
static int src_html;
static char cur_url[WEB_URL_MAX];
static char title[96];
static char hist[HIST_MAX][WEB_URL_MAX];
static int hist_n;
static struct web_line_s lines[WEB_LINES_MAX];
static int nlines;
static struct web_span spans[WEB_SPANS_MAX];
static int nspans;
static char href_pool[HREF_POOL];
static unsigned int href_used;
static int link_off[WEB_LINKS_MAX];
static int nlinks;
static int last_cols = 78;
static int last_marks;

/* ---------- small string helpers ---------- */
static unsigned int s_len(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}
static void s_cpy(char *d, const char *s, unsigned int cap) {
    unsigned int i = 0;
    while (s[i] && i + 1 < cap) {
        d[i] = s[i];
        i++;
    }
    if (cap) d[i] = 0;
}
static unsigned int s_app(char *d, unsigned int at, const char *s, unsigned int cap) {
    while (*s && at + 1 < cap) d[at++] = *s++;
    if (cap) d[at] = 0;
    return at;
}
static int pre(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}
static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}
/* case-insensitive prefix */
static int ipre(const char *s, const char *p) {
    while (*p) {
        if (lower(*s++) != *p++) return 0;
    }
    return 1;
}
static void put_num(int v) {
    char b[12];
    int n = 0;
    if (v < 0) {
        console_putc('-');
        v = -v;
    }
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    while (n) console_putc(b[--n]);
}
static unsigned int u_app(char *d, unsigned int at, unsigned int v, unsigned int cap) {
    char b[12];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < 11);
    while (n && at + 1 < cap) d[at++] = b[--n];
    if (cap) d[at] = 0;
    return at;
}
static int is_cont(unsigned char c) {
    return (c & 0xc0) == 0x80;
}

/* ---------- URLs ---------- */
static int has_scheme(const char *s) {
    int i = 0;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (i > 0 && s[i] >= '0' && s[i] <= '9') ||
           (i > 0 && (s[i] == '+' || s[i] == '-' || s[i] == '.'))) {
        i++;
    }
    return i > 0 && s[i] == ':';
}

int web_looks_like_url(const char *s) {
    if (pre(s, "http://") || pre(s, "https://") || pre(s, "file:") || pre(s, "about:")) return 1;
    if (s[0] == '/' && (s[1] != 0)) {
        unsigned int n = s_len(s);
        return n > 5 && (ipre(s + n - 5, ".html") || ipre(s + n - 4, ".htm"));
    }
    /* example.com, example.com/path: a dot, no space */
    int dot = 0;
    for (const char *c = s; *c; c++) {
        if (*c == ' ') return 0;
        if (*c == '.') dot = 1;
        if (*c == '/') break;
    }
    return dot && s[0] != '.';
}

/* Canonical form: "http(s)://...", "about:x", or an absolute path. */
static void canon(const char *in, char *out) {
    while (*in == ' ') in++;
    if (in[0] == 0) {
        s_cpy(out, "about:home", WEB_URL_MAX);
    } else if (pre(in, "file://")) {
        s_cpy(out, in + 7, WEB_URL_MAX);
        if (out[0] != '/') {
            out[0] = '/';
            s_cpy(out + 1, in + 7, WEB_URL_MAX - 1);
        }
    } else if (in[0] == '/' || has_scheme(in)) {
        s_cpy(out, in, WEB_URL_MAX);
    } else {
        unsigned int n = s_app(out, 0, "http://", WEB_URL_MAX);
        s_app(out, n, in, WEB_URL_MAX);
    }
    /* drop the fragment */
    for (char *c = out; *c; c++) {
        if (*c == '#') {
            *c = 0;
            break;
        }
    }
    if (out[0] == 0) s_cpy(out, "about:home", WEB_URL_MAX);
}

/* Resolve HREF against the page at BASE. */
static void resolve(const char *base, const char *href, char *out) {
    char tmp[WEB_URL_MAX];
    while (*href == ' ') href++;
    if (href[0] == '#' || href[0] == 0) {
        s_cpy(out, base, WEB_URL_MAX);
        return;
    }
    if (has_scheme(href)) {
        canon(href, out);
        return;
    }
    if (href[0] == '/' && href[1] == '/') {
        unsigned int n = s_app(tmp, 0, "http:", sizeof tmp);
        s_app(tmp, n, href, sizeof tmp);
        canon(tmp, out);
        return;
    }
    int web = pre(base, "http://") || pre(base, "https://");
    unsigned int root = 0; /* length of "scheme://host[:port]" */
    if (web) {
        root = pre(base, "https://") ? 8 : 7;
        while (base[root] && base[root] != '/' && base[root] != '?') root++;
    }
    unsigned int n = 0;
    if (href[0] == '/') {
        for (unsigned int i = 0; i < root && n + 1 < sizeof tmp; i++) tmp[n++] = base[i];
        tmp[n] = 0;
        s_app(tmp, n, href, sizeof tmp);
    } else {
        /* directory of base */
        unsigned int last = root;
        int any = 0;
        for (unsigned int i = root; base[i] && base[i] != '?'; i++) {
            if (base[i] == '/') {
                last = i;
                any = 1;
            }
        }
        if (pre(base, "about:")) {
            n = s_app(tmp, 0, "/", sizeof tmp);
        } else {
            for (unsigned int i = 0; i < (any ? last + 1 : root) && n + 1 < sizeof tmp; i++) tmp[n++] = base[i];
            tmp[n] = 0;
            if (!any) n = s_app(tmp, n, "/", sizeof tmp);
        }
        s_app(tmp, n, href, sizeof tmp);
    }
    /* fold "./" and "../" in the path part */
    char res[WEB_URL_MAX];
    unsigned int r = 0;
    unsigned int i = 0;
    for (; i < root && tmp[i]; i++) res[r++] = tmp[i];
    while (tmp[i] && r + 1 < sizeof res) {
        if (tmp[i] == '?') {
            while (tmp[i] && r + 1 < sizeof res) res[r++] = tmp[i++];
            break;
        }
        if (tmp[i] == '/' && tmp[i + 1] == '.' && (tmp[i + 2] == '/' || tmp[i + 2] == 0)) {
            i += 2;
            continue;
        }
        if (tmp[i] == '/' && tmp[i + 1] == '.' && tmp[i + 2] == '.' && (tmp[i + 3] == '/' || tmp[i + 3] == 0)) {
            while (r > root && res[r - 1] != '/') r--;
            if (r > root) r--;
            i += 3;
            continue;
        }
        res[r++] = tmp[i++];
    }
    res[r] = 0;
    if (r == root) {
        res[r++] = '/';
        res[r] = 0;
    }
    canon(res, out);
}

/* ---------- loading ---------- */
static void src_set(const char *s) {
    src_len = s_app(src, 0, s, SRC_CAP);
    src_html = 1;
}
static void src_add(const char *s) {
    src_len = s_app(src, src_len, s, SRC_CAP);
}
/* Text into src with <, >, & escaped. */
static void src_add_esc(const char *s) {
    for (; *s; s++) {
        if (*s == '<') src_add("&lt;");
        else if (*s == '>') src_add("&gt;");
        else if (*s == '&') src_add("&amp;");
        else if (src_len + 1 < SRC_CAP) {
            src[src_len++] = *s;
            src[src_len] = 0;
        }
    }
}

static void error_page(const char *url, const char *what) {
    src_set("<title>Ошибка</title><h1>Страница не открылась</h1><p>");
    src_add_esc(url);
    src_add("</p><p>");
    src_add_esc(what);
    src_add("</p><ul><li><a href=\"about:home\">Домой (about:home)</a></li>"
            "<li>Назад: Backspace или кнопка «<»</li></ul>");
    console_puts("web: error ");
    console_puts(url);
    console_puts(": ");
    console_puts(what);
    console_putc('\n');
}

static const char page_home[] =
    "<title>Браузер genesis</title>"
    "<h1>Браузер genesis</h1>"
    "<p>Адрес вводится в строке внизу (или нажмите на адрес вверху): "
    "http://..., /docs/index.html, about:help. Enter открывает.</p>"
    "<h2>Страницы на диске</h2><ul>"
    "<li><a href=\"/docs/index.html\">Добро пожаловать</a></li>"
    "<li><a href=\"/docs/web.html\">Сеть и браузер</a></li>"
    "<li><a href=\"/docs/\">Папка /docs</a></li>"
    "<li><a href=\"about:help\">Клавиши</a></li></ul>"
    "<h2>В сети</h2><ul>"
    "<li><a href=\"http://example.com/\">example.com</a></li>"
    "<li><a href=\"http://10.0.2.2:8000/\">компьютер в QEMU (10.0.2.2:8000)</a></li></ul>";

static const char page_help[] =
    "<title>Клавиши</title><h1>Клавиши браузера</h1><ul>"
    "<li>Tab / стрелки - выбрать ссылку, Enter - открыть</li>"
    "<li>PgUp / PgDn, стрелки у края - прокрутка</li>"
    "<li>Backspace или «<» - ровно на одну страницу назад</li>"
    "<li>Esc или X - закрыть окно</li>"
    "<li>Нажатие на адрес - править его в строке внизу; Enter - открыть</li>"
    "<li>«Обновить» - загрузить заново</li></ul>"
    "<p>В оболочке: <b>browse АДРЕС</b> печатает страницу текстом; "
    "<b>web back</b>, <b>web reload</b>, <b>web follow N</b>.</p>";

static int ends_with(const char *s, const char *suffix) {
    unsigned int a = s_len(s), b = s_len(suffix);
    return a >= b && ipre(s + a - b, suffix);
}

static void load_local(const char *path) {
    const unsigned char *data = 0;
    unsigned int len = 0;
    char p[WEB_URL_MAX];
    s_cpy(p, path, sizeof p);
    unsigned int n = s_len(p);
    if (n > 1 && p[n - 1] == '/') p[--n] = 0;
    if (fs_get(p, &data, &len)) {
        if (len >= SRC_CAP) len = SRC_CAP - 1;
        for (unsigned int i = 0; i < len; i++) src[i] = (char)data[i];
        src[len] = 0;
        src_len = len;
        src_html = ends_with(p, ".html") || ends_with(p, ".htm");
        for (unsigned int i = 0; i < len && i < 1024; i++) {
            if (src[i] == 0) {
                src_set("<title>");
                src_add_esc(p);
                src_add("</title><h1>Это не текст</h1><p>");
                src_add_esc(p);
                src_add(" - двоичный файл. Откройте его в «Файлах».</p>");
                return;
            }
        }
        return;
    }
    /* a folder: list it */
    char name[FS_NAME];
    int is_dir = 0;
    int found = 0;
    src_set("<title>");
    src_add_esc(p);
    src_add("</title><h1>");
    src_add_esc(p);
    src_add("</h1><ul>");
    for (int i = 0; i < FS_MAX; i++) {
        if (!fs_dir_entry(i, p, name, sizeof name, &is_dir)) continue;
        found++;
        src_add("<li><a href=\"");
        src_add_esc(p);
        if (!(p[0] == '/' && p[1] == 0)) src_add("/");
        src_add_esc(name);
        src_add(is_dir ? "/\">" : "\">");
        src_add_esc(name);
        src_add(is_dir ? "/</a></li>" : "</a></li>");
    }
    src_add("</ul>");
    if (!found) error_page(path, "Нет такого файла или папки.");
}

static void load(const char *url) {
    char u[WEB_URL_MAX];
    s_cpy(u, url, sizeof u);
    title[0] = 0;
    src_len = 0;
    src[0] = 0;
    src_html = 1;
    if (pre(u, "about:")) {
        if (pre(u, "about:help")) src_set(page_help);
        else if (pre(u, "about:blank")) src_set("<title>about:blank</title>");
        else src_set(page_home);
    } else if (u[0] == '/') {
        load_local(u);
    } else if (pre(u, "http://") || pre(u, "https://")) {
        char loc[WEB_URL_MAX];
        int status = 0;
        for (int hop = 0; hop < 5; hop++) {
            status = net_http_get(u, src, SRC_CAP, &src_len, loc, sizeof loc);
            if (status >= 300 && status < 400 && loc[0]) {
                char next[WEB_URL_MAX];
                resolve(u, loc, next);
                if (pre(next, "https://") && pre(u, "http://")) {
                    /* plain HTTP only: same page over https is a dead end */
                    char same[WEB_URL_MAX];
                    unsigned int k = s_app(same, 0, "http://", sizeof same);
                    s_app(same, k, next + 8, sizeof same);
                    if (pre(same, u) && s_len(same) == s_len(u)) {
                        error_page(u, "Сайт требует HTTPS, а TLS в genesis пока нет.");
                        status = -100;
                        break;
                    }
                }
                s_cpy(u, next, sizeof u);
                s_cpy(cur_url, u, sizeof cur_url);
                continue;
            }
            break;
        }
        if (status == -100) {
        } else if (status < 0) {
            const char *why = status == -1   ? "Неверный адрес."
                              : status == -2 ? "Сети нет: нет карты или DHCP не ответил (команда dhcp)."
                              : status == -3 ? "Имя не найдено (DNS)."
                              : status == -4 ? "Не удалось соединиться."
                              : status == -5 ? "Запрос не отправился."
                                             : "Сервер не ответил.";
            error_page(u, why);
        } else if (status >= 300 && status < 400) {
            error_page(u, "Слишком много перенаправлений.");
        } else {
            /* plain text unless it looks like HTML */
            int html = 0;
            for (unsigned int i = 0; i < src_len && i < 2048; i++) {
                if (src[i] == '<' && (ipre(src + i, "<html") || ipre(src + i, "<!doctype") || ipre(src + i, "<p") ||
                                      ipre(src + i, "<h") || ipre(src + i, "<a ") || ipre(src + i, "<body") ||
                                      ipre(src + i, "<div") || ipre(src + i, "<ul") || ipre(src + i, "<pre") ||
                                      ipre(src + i, "<title"))) {
                    html = 1;
                    break;
                }
            }
            src_html = html;
            if (status >= 400) {
                char head[128];
                unsigned int k = s_app(head, 0, "HTTP ", sizeof head);
                k = u_app(head, k, (unsigned int)status, sizeof head);
                console_puts("web: error ");
                console_puts(u);
                console_puts(": ");
                console_puts(head);
                console_putc('\n');
                if (!html) {
                    /* keep the body after a heading */
                    static char body[2048];
                    s_cpy(body, src, sizeof body);
                    src_set("<h1>");
                    src_add(head);
                    src_add("</h1><pre>");
                    src_add_esc(body);
                    src_add("</pre>");
                }
            }
        }
    } else {
        error_page(u, "Этот вид адреса не поддерживается (http://, /путь, about:).");
    }
    s_cpy(cur_url, u, sizeof cur_url);
}

/* ---------- rendering ---------- */
static int cols;
static int marks; /* text mode: "[N]" after each link */
static char cur[WEB_LINE_CAP];
static unsigned int cur_b;
static int cur_c;
static int cur_style;
static int style;
static int link_now = -1;
static int space_pending;
static int blank_pending;
static int truncated;

static void emit(void) {
    if (nlines >= WEB_LINES_MAX - 1) {
        if (!truncated) {
            truncated = 1;
            s_cpy(lines[nlines].text, "... (страница обрезана)", WEB_LINE_CAP);
            lines[nlines].style = WS_MUTED;
            nlines++;
        }
        cur_b = 0;
        cur_c = 0;
        return;
    }
    cur[cur_b] = 0;
    s_cpy(lines[nlines].text, cur, WEB_LINE_CAP);
    lines[nlines].style = (unsigned char)cur_style;
    nlines++;
    cur_b = 0;
    cur_c = 0;
    cur_style = style;
    space_pending = 0;
}

static int last_blank(void) {
    return nlines == 0 || lines[nlines - 1].text[0] == 0;
}

/* End the current line; BLANK asks for one empty line before what follows. */
static void brk(int blank) {
    if (cur_c > 0) emit();
    if (blank) blank_pending = 1;
    space_pending = 0;
}

static void begin_text(void) {
    if (blank_pending && cur_c == 0) {
        if (!last_blank()) {
            cur_style = WS_TEXT;
            emit();
        }
        blank_pending = 0;
    }
    if (cur_c == 0) cur_style = style;
}

static void add_span(int col, int chars) {
    if (link_now < 0 || chars <= 0) return;
    if (nspans > 0) {
        struct web_span *l = &spans[nspans - 1];
        if (l->link == link_now && l->line == nlines && l->col + l->chars == col) {
            l->chars = (short)(l->chars + chars);
            return;
        }
        if (l->link == link_now && l->line == nlines && l->col + l->chars + 1 == col) {
            l->chars = (short)(col + chars - l->col); /* the space between words */
            return;
        }
    }
    if (nspans >= WEB_SPANS_MAX) return;
    spans[nspans].line = (short)nlines;
    spans[nspans].col = (short)col;
    spans[nspans].chars = (short)chars;
    spans[nspans].link = (short)link_now;
    nspans++;
}

static void put_bytes(const char *w, unsigned int bytes, int chars) {
    for (unsigned int i = 0; i < bytes && cur_b + 1 < WEB_LINE_CAP; i++) cur[cur_b++] = w[i];
    cur_c += chars;
}

static void word(const char *w, unsigned int bytes, int chars) {
    if (bytes == 0) return;
    begin_text();
    if (cur_c > 0 && space_pending) {
        if (cur_c + 1 + chars > cols || cur_b + 1 + bytes + 1 >= WEB_LINE_CAP) {
            emit();
        } else {
            put_bytes(" ", 1, 1);
        }
    } else if (cur_c > 0 && (cur_c + chars > cols || cur_b + bytes + 1 >= WEB_LINE_CAP)) {
        emit();
    }
    space_pending = 0;
    /* longer than a line: hard split */
    while (chars > cols - cur_c && cur_c == 0 && chars > cols) {
        unsigned int b = 0;
        int c = 0;
        while (b < bytes && c < cols) {
            b++;
            while (b < bytes && is_cont((unsigned char)w[b])) b++;
            c++;
        }
        add_span(0, c);
        put_bytes(w, b, c);
        emit();
        w += b;
        bytes -= b;
        chars -= c;
    }
    if (bytes == 0) return;
    add_span(cur_c, chars);
    put_bytes(w, bytes, chars);
}

static void pre_char(const char *w, unsigned int bytes) {
    begin_text();
    if (cur_c >= cols || cur_b + bytes + 1 >= WEB_LINE_CAP) emit();
    add_span(cur_c, 1);
    put_bytes(w, bytes, 1);
}

/* UTF-8 for code point CP into OUT, returns bytes. */
static unsigned int utf8(unsigned int cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = '?';
    return 1;
}

/* Decode an entity at s (after '&'); writes UTF-8 to out. Returns chars used. */
static unsigned int entity(const char *s, char *out, unsigned int *ob) {
    static const struct { const char *name; const char *text; } names[] = {
        {"amp;", "&"},  {"lt;", "<"},    {"gt;", ">"},     {"quot;", "\""}, {"apos;", "'"},  {"nbsp;", " "},
        {"mdash;", "-"}, {"ndash;", "-"}, {"hellip;", "..."}, {"laquo;", "\""}, {"raquo;", "\""}, {"copy;", "(c)"},
        {"rsquo;", "'"}, {"lsquo;", "'"}, {"ldquo;", "\""}, {"rdquo;", "\""}, {"bull;", "*"},   {"middot;", "."},
    };
    if (s[0] == '#') {
        unsigned int cp = 0;
        unsigned int i = 1;
        if (s[1] == 'x' || s[1] == 'X') {
            for (i = 2; i < 10; i++) {
                char c = lower(s[i]);
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0) break;
                cp = cp * 16u + (unsigned int)d;
            }
        } else {
            for (; i < 10 && s[i] >= '0' && s[i] <= '9'; i++) cp = cp * 10u + (unsigned int)(s[i] - '0');
        }
        if (i > 1 && s[i] == ';') {
            if (cp == 0xa0) cp = ' ';
            if (cp == 0x2014 || cp == 0x2013) cp = '-';
            if (cp == 0xab || cp == 0xbb || cp == 0x201c || cp == 0x201d) cp = '"';
            if (cp == 0x2019 || cp == 0x2018) cp = '\'';
            *ob = utf8(cp, out);
            return i + 1;
        }
    }
    for (unsigned int k = 0; k < sizeof names / sizeof names[0]; k++) {
        if (pre(s, names[k].name)) {
            *ob = s_app(out, 0, names[k].text, 8);
            return s_len(names[k].name);
        }
    }
    out[0] = '&';
    *ob = 1;
    return 0;
}

static int add_link(const char *href, unsigned int n) {
    if (nlinks >= WEB_LINKS_MAX || href_used + n + 1 >= HREF_POOL) return -1;
    link_off[nlinks] = (int)href_used;
    unsigned int k = 0;
    /* decode &amp; in hrefs */
    for (unsigned int i = 0; i < n; i++) {
        if (href[i] == '&' && pre(href + i, "&amp;")) {
            href_pool[href_used + k++] = '&';
            i += 4;
            continue;
        }
        href_pool[href_used + k++] = href[i];
    }
    href_pool[href_used + k] = 0;
    href_used += k + 1;
    return nlinks++;
}

/* Value of attribute NAME inside a tag's text [a, b). */
static int attr(const char *a, const char *b, const char *name, const char **val, unsigned int *vlen) {
    unsigned int nl = s_len(name);
    for (const char *p = a; p + nl < b; p++) {
        if ((p == a || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && ipre(p, name) && (p[nl] == '=' || p[nl] == ' ')) {
            const char *q = p + nl;
            while (q < b && *q == ' ') q++;
            if (q >= b || *q != '=') continue;
            q++;
            while (q < b && *q == ' ') q++;
            char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
            const char *e = q;
            while (e < b && (quote ? *e != quote : (*e != ' ' && *e != '>'))) e++;
            *val = q;
            *vlen = (unsigned int)(e - q);
            return 1;
        }
    }
    return 0;
}

static int tag_is(const char *name, unsigned int n, const char *want) {
    unsigned int w = s_len(want);
    if (n != w) return 0;
    for (unsigned int i = 0; i < n; i++) {
        if (lower(name[i]) != want[i]) return 0;
    }
    return 1;
}

static const char *skip_to_close(const char *p, const char *end, const char *close) {
    unsigned int n = s_len(close);
    while (p + n <= end) {
        if (*p == '<' && ipre(p, close)) {
            while (p < end && *p != '>') p++;
            return p < end ? p + 1 : end;
        }
        p++;
    }
    return end;
}

static void render(void) {
    nlines = 0;
    nspans = 0;
    nlinks = 0;
    href_used = 0;
    cur_b = 0;
    cur_c = 0;
    style = WS_TEXT;
    cur_style = WS_TEXT;
    link_now = -1;
    space_pending = 0;
    blank_pending = 0;
    truncated = 0;
    int in_pre = !src_html;
    int depth = 0;
    int ol[8] = {0};
    int ol_on[8] = {0};
    int in_title = 0;
    unsigned int tlen = 0;
    if (!src_html) style = cur_style = WS_PRE;
    const char *p = src;
    const char *end = src + src_len;
    char wbuf[WEB_LINE_CAP];
    unsigned int wb = 0;
    int wc = 0;
#define FLUSH_WORD()                \
    do {                            \
        if (wb) word(wbuf, wb, wc); \
        wb = 0;                     \
        wc = 0;                     \
    } while (0)
    while (p < end) {
        char c = *p;
        if (c == '<' && src_html && p + 1 < end &&
            ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '/' || p[1] == '!')) {
            if (pre(p, "<!--")) {
                const char *q = p + 4;
                while (q + 3 <= end && !pre(q, "-->")) q++;
                p = q + 3 <= end ? q + 3 : end;
                continue;
            }
            const char *t = p + 1;
            int closing = 0;
            if (*t == '/') {
                closing = 1;
                t++;
            }
            const char *nm = t;
            while (t < end && ((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9') || *t == '!'))
                t++;
            unsigned int nn = (unsigned int)(t - nm);
            const char *close = t;
            char q = 0;
            while (close < end && (q || *close != '>')) {
                if (q && *close == q) q = 0;
                else if (!q && (*close == '"' || *close == '\'')) q = *close;
                close++;
            }
            const char *after = close < end ? close + 1 : end;
            int inline_tag = tag_is(nm, nn, "a") || tag_is(nm, nn, "b") || tag_is(nm, nn, "i") ||
                             tag_is(nm, nn, "em") || tag_is(nm, nn, "strong") || tag_is(nm, nn, "span") ||
                             tag_is(nm, nn, "code") || tag_is(nm, nn, "small") || tag_is(nm, nn, "u") ||
                             tag_is(nm, nn, "font") || tag_is(nm, nn, "abbr") || tag_is(nm, nn, "kbd") ||
                             tag_is(nm, nn, "sup") || tag_is(nm, nn, "sub") || tag_is(nm, nn, "label");
            if (!inline_tag) FLUSH_WORD();
            if (!closing && (tag_is(nm, nn, "script") || tag_is(nm, nn, "style") || tag_is(nm, nn, "svg") ||
                             tag_is(nm, nn, "template") || tag_is(nm, nn, "select") || tag_is(nm, nn, "iframe"))) {
                char cl[16];
                unsigned int k = s_app(cl, 0, "</", sizeof cl);
                for (unsigned int i = 0; i < nn && k + 1 < sizeof cl; i++) cl[k++] = lower(nm[i]);
                cl[k] = 0;
                p = skip_to_close(after, end, cl);
                continue;
            }
            if (tag_is(nm, nn, "title")) {
                in_title = !closing;
                if (!closing) tlen = 0;
            } else if (tag_is(nm, nn, "a")) {
                FLUSH_WORD(); /* the last word still belongs to the link */
                if (link_now >= 0 && marks) {
                    char m[12];
                    unsigned int k = s_app(m, 0, "[", sizeof m);
                    k = u_app(m, k, (unsigned int)link_now + 1, sizeof m);
                    k = s_app(m, k, "]", sizeof m);
                    int save = link_now;
                    link_now = -1;
                    FLUSH_WORD();
                    word(m, k, (int)k);
                    link_now = save;
                }
                link_now = -1;
                if (!closing) {
                    const char *v;
                    unsigned int vl;
                    if (attr(t, close, "href", &v, &vl)) {
                        FLUSH_WORD();
                        link_now = add_link(v, vl);
                    }
                } else {
                    FLUSH_WORD();
                }
            } else if (inline_tag) {
                /* no break */
            } else if (tag_is(nm, nn, "br")) {
                begin_text();
                if (cur_c > 0) emit();
                else if (!in_pre) emit();
            } else if (nn == 2 && lower(nm[0]) == 'h' && nm[1] >= '1' && nm[1] <= '6') {
                brk(1);
                style = closing ? WS_TEXT : WS_HEAD;
                cur_style = style;
            } else if (tag_is(nm, nn, "p") || tag_is(nm, nn, "blockquote") || tag_is(nm, nn, "table") ||
                       tag_is(nm, nn, "dl") || tag_is(nm, nn, "figure") || tag_is(nm, nn, "form")) {
                brk(1);
            } else if (tag_is(nm, nn, "ul") || tag_is(nm, nn, "ol")) {
                brk(depth == 0);
                if (!closing && depth < 8) {
                    ol_on[depth] = tag_is(nm, nn, "ol");
                    ol[depth] = 0;
                    depth++;
                } else if (closing && depth > 0) {
                    depth--;
                }
            } else if (tag_is(nm, nn, "li")) {
                brk(0);
                if (!closing) {
                    char m[32];
                    unsigned int k = 0;
                    for (int d = 1; d < depth && k + 3 < sizeof m; d++) k = s_app(m, k, "  ", sizeof m);
                    int lvl = depth > 0 ? depth - 1 : 0;
                    if (depth > 0 && ol_on[lvl]) {
                        k = u_app(m, k, (unsigned int)++ol[lvl], sizeof m);
                        k = s_app(m, k, ".", sizeof m);
                    } else {
                        k = s_app(m, k, "-", sizeof m);
                    }
                    word(m, k, (int)k);
                    space_pending = 1;
                }
            } else if (tag_is(nm, nn, "pre")) {
                brk(1);
                in_pre = !closing;
                style = closing ? WS_TEXT : WS_PRE;
                cur_style = style;
            } else if (tag_is(nm, nn, "hr")) {
                brk(0);
                begin_text();
                cur_style = WS_MUTED;
                int n = cols < 40 ? cols : 40;
                for (int i = 0; i < n; i++) put_bytes("-", 1, 1);
                emit();
            } else if (tag_is(nm, nn, "img") && !closing) {
                const char *v;
                unsigned int vl;
                if (attr(t, close, "alt", &v, &vl) && vl > 0) {
                    char m[64];
                    unsigned int k = s_app(m, 0, "[", sizeof m);
                    for (unsigned int i = 0; i < vl && k + 2 < sizeof m; i++) m[k++] = v[i] == ' ' ? '_' : v[i];
                    k = s_app(m, k, "]", sizeof m);
                    int chars = 0;
                    for (unsigned int i = 0; i < k; i++) chars += !is_cont((unsigned char)m[i]);
                    word(m, k, chars);
                    space_pending = 1;
                }
            } else if (tag_is(nm, nn, "td") || tag_is(nm, nn, "th")) {
                space_pending = 1;
                if (!closing && cur_c > 0) word("|", 1, 1), space_pending = 1;
            } else if (tag_is(nm, nn, "head") && !closing) {
                /* only the title matters in <head> */
            } else {
                /* div, tr, section, dt, dd, header, footer, ...: a line break */
                brk(0);
            }
            p = after;
            continue;
        }
        /* text */
        char out[8];
        unsigned int ob = 1;
        unsigned int used = 1;
        if (c == '&' && src_html) {
            used = 1 + entity(p + 1, out, &ob);
            if (used == 1) out[0] = '&';
        } else {
            out[0] = c;
            unsigned int len = 1;
            unsigned char u = (unsigned char)c;
            if (u >= 0xc0) {
                while (p + len < end && is_cont((unsigned char)p[len]) && len < 4) len++;
            }
            for (unsigned int i = 0; i < len; i++) out[i] = p[i];
            ob = len;
            used = len;
        }
        p += used;
        if (in_title) {
            for (unsigned int i = 0; i < ob && tlen + 1 < sizeof title; i++) {
                char ch = out[i] == '\n' || out[i] == '\t' ? ' ' : out[i];
                if (ch == ' ' && (tlen == 0 || title[tlen - 1] == ' ')) continue;
                title[tlen++] = ch;
            }
            title[tlen] = 0;
            continue;
        }
        if (in_pre) {
            if (out[0] == '\r' && ob == 1) continue;
            if (out[0] == '\n' && ob == 1) {
                begin_text();
                emit();
                continue;
            }
            if (out[0] == '\t' && ob == 1) {
                do pre_char(" ", 1);
                while (cur_c % 4);
                continue;
            }
            if ((unsigned char)out[0] < 32) continue;
            /* one character at a time (an entity may give several) */
            unsigned int i = 0;
            while (i < ob) {
                unsigned int l = 1;
                while (i + l < ob && is_cont((unsigned char)out[i + l])) l++;
                pre_char(out + i, l);
                i += l;
            }
            continue;
        }
        if (ob == 1 && (out[0] == ' ' || out[0] == '\n' || out[0] == '\t' || out[0] == '\r')) {
            FLUSH_WORD();
            if (cur_c > 0) space_pending = 1;
            continue;
        }
        for (unsigned int i = 0; i < ob && wb + 1 < sizeof wbuf; i++) {
            if (out[i] == ' ') {
                FLUSH_WORD();
                if (cur_c > 0) space_pending = 1;
                continue;
            }
            wbuf[wb++] = out[i];
            if (!is_cont((unsigned char)out[i])) wc++;
        }
        if (wc >= cols) FLUSH_WORD();
    }
    FLUSH_WORD();
#undef FLUSH_WORD
    if (cur_c > 0) emit();
    while (nlines > 0 && lines[nlines - 1].text[0] == 0) nlines--;
    if (title[0] == 0) {
        s_cpy(title, cur_url, sizeof title);
    }
}

/* ---------- public ---------- */
void web_layout(int c) {
    cols = c < 20 ? 20 : c > COLS_MAX ? COLS_MAX : c;
    last_cols = cols;
    marks = last_marks;
    render();
}

static void report(void) {
    console_puts("web: loaded ");
    console_puts(cur_url);
    console_puts(" \"");
    console_puts(title);
    console_puts("\" (");
    put_num(nlines);
    console_puts(" lines, ");
    put_num(nlinks);
    console_puts(" links)\n");
}

static void go(const char *url) {
    console_puts("web: open ");
    console_puts(url);
    console_putc('\n');
    s_cpy(cur_url, url, sizeof cur_url);
    load(url);
    web_layout(last_cols);
    report();
}

void web_open(const char *url) {
    char u[WEB_URL_MAX];
    canon(url, u);
    if (cur_url[0] != 0) {
        if (hist_n == HIST_MAX) {
            for (int i = 1; i < HIST_MAX; i++) s_cpy(hist[i - 1], hist[i], WEB_URL_MAX);
            hist_n--;
        }
        s_cpy(hist[hist_n++], cur_url, WEB_URL_MAX);
    }
    go(u);
}

/* First page of a fresh window: no history. */
void web_start(const char *url) {
    char u[WEB_URL_MAX];
    canon(url, u);
    hist_n = 0;
    go(u);
}

int web_back(void) {
    if (hist_n == 0) return 0;
    char u[WEB_URL_MAX];
    s_cpy(u, hist[--hist_n], sizeof u);
    console_puts("web: back ");
    console_puts(u);
    console_putc('\n');
    go(u);
    return 1;
}

void web_reload(void) {
    char u[WEB_URL_MAX];
    s_cpy(u, cur_url[0] ? cur_url : "about:home", sizeof u);
    go(u);
}

void web_follow(int k) {
    if (k < 0 || k >= nlinks) return;
    char u[WEB_URL_MAX];
    resolve(cur_url, href_pool + link_off[k], u);
    web_open(u);
}

int web_line_count(void) { return nlines; }
const char *web_line(int i, int *st) {
    if (i < 0 || i >= nlines) {
        *st = 0;
        return "";
    }
    *st = lines[i].style;
    return lines[i].text;
}
int web_span_count(void) { return nspans; }
const struct web_span *web_span_at(int i) { return &spans[i]; }
int web_link_count(void) { return nlinks; }
const char *web_link(int k) { return k >= 0 && k < nlinks ? href_pool + link_off[k] : ""; }
const char *web_url(void) { return cur_url; }
const char *web_title(void) { return title; }
int web_depth(void) { return hist_n; }

/* ---------- shell ---------- */
static void print_page(void) {
    int save = last_cols;
    last_marks = 1;
    web_layout(78);
    console_puts("== ");
    console_puts(title);
    console_puts(" ==\n");
    for (int i = 0; i < nlines; i++) {
        console_puts(lines[i].style == WS_HEAD ? "# " : "");
        console_puts(lines[i].text);
        console_putc('\n');
    }
    for (int k = 0; k < nlinks; k++) {
        char u[WEB_URL_MAX];
        resolve(cur_url, href_pool + link_off[k], u);
        console_putc('[');
        put_num(k + 1);
        console_puts("] ");
        console_puts(u);
        console_putc('\n');
    }
    last_marks = 0;
    web_layout(save);
}

static const char *skip(const char *s) {
    while (*s == ' ') s++;
    return s;
}

enum { WEB_OPEN = 1, WEB_BACK, WEB_RELOAD, WEB_FOLLOW };

int web_command(const char *line) {
    int browse = pre(line, "browse") && (line[6] == 0 || line[6] == ' ');
    int web = pre(line, "web") && (line[3] == 0 || line[3] == ' ');
    if (!browse && !web) return 0;
    const char *arg = skip(line + (browse ? 6 : 3));
    int gui = desktop_active();
    if (browse || (web && (pre(arg, "open") || arg[0] == 0))) {
        if (web && pre(arg, "open")) arg = skip(arg + 4);
        if (gui) {
            desktop_browse(arg);
        } else {
            web_start(arg);
            print_page();
        }
        return 1;
    }
    if (pre(arg, "back")) {
        if (gui) desktop_web(WEB_BACK, 0);
        else if (web_back()) print_page();
        else console_puts("web: no page behind this one\n");
    } else if (pre(arg, "reload")) {
        if (gui) desktop_web(WEB_RELOAD, 0);
        else {
            web_reload();
            print_page();
        }
    } else if (pre(arg, "follow ")) {
        const char *n = skip(arg + 7);
        int k = 0;
        while (*n >= '0' && *n <= '9') k = k * 10 + (*n++ - '0');
        if (k < 1 || k > nlinks) {
            console_puts("web: no such link\n");
        } else if (gui) {
            desktop_web(WEB_FOLLOW, k - 1);
        } else {
            web_follow(k - 1);
            print_page();
        }
    } else {
        console_puts("web: usage: browse URL | web back | web reload | web follow N\n");
    }
    return 1;
}

/* ---------- pages on the disk: /docs/index.html, /docs/web.html ---------- */
const char web_page_index[] =
    "<!doctype html>\n<html><head><title>Добро пожаловать в genesis</title>\n"
    "<style>body { color: red }</style><script>alert('never runs')</script></head>\n"
    "<body>\n<h1>genesis</h1>\n"
    "<p>Это страница с диска: <b>/docs/index.html</b>. Браузер показывает текст, заголовки, "
    "абзацы, списки, ссылки и блоки кода; скрипты и стили отбрасываются.</p>\n"
    "<h2>Куда дальше</h2>\n<ul>\n"
    "<li><a href=\"web.html\">Сеть и браузер</a> - как открыть страницу из сети</li>\n"
    "<li><a href=\"hello.c\">hello.c</a> - исходник на C (обычный текст)</li>\n"
    "<li><a href=\"/docs/\">Все файлы в /docs</a></li>\n"
    "<li><a href=\"about:help\">Клавиши браузера</a></li>\n</ul>\n"
    "<h2>Пример кода</h2>\n<pre>int main() {\n    printf(\"hello\\n\");\n    return 0;\n}</pre>\n"
    "<ol><li>первый</li><li>второй</li></ol>\n"
    "<p>Конец страницы &amp; ссылок.</p>\n</body></html>\n";
const unsigned int web_page_index_len = sizeof web_page_index - 1;

const char web_page_net[] =
    "<!doctype html>\n<html><head><title>Сеть и браузер</title></head><body>\n"
    "<h1>Сеть и браузер</h1>\n"
    "<p>Адрес вводится в строке внизу экрана; Enter открывает страницу.</p>\n"
    "<h2>QEMU</h2>\n<pre>qemu ... -netdev user,id=n0 -device ne2k_pci,netdev=n0</pre>\n"
    "<p>Компьютер виден как 10.0.2.2: <a href=\"http://10.0.2.2:8000/\">http://10.0.2.2:8000/</a> "
    "(например, python3 -m http.server на нём).</p>\n"
    "<h2>Сайт (v86)</h2>\n<p>Запросы идут через fetch() браузера; работают сайты, которые "
    "разрешают CORS. Пример: <a href=\"http://example.com/\">example.com</a>.</p>\n"
    "<p><a href=\"index.html\">Назад к началу</a></p>\n</body></html>\n";
const unsigned int web_page_net_len = sizeof web_page_net - 1;
