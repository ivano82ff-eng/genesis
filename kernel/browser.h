#pragma once
/* Browser: fetches a page (http://, file path, about:), turns simple HTML
 * into wrapped lines with link spans, keeps a history for Back. */
enum { WEB_LINE_CAP = 200, WEB_LINES_MAX = 500, WEB_LINKS_MAX = 160, WEB_SPANS_MAX = 600, WEB_URL_MAX = 256 };
enum { WS_TEXT = 0, WS_HEAD = 1, WS_PRE = 2, WS_MUTED = 3, WS_ERROR = 4 };
struct web_span { short line, col, chars, link; };

void web_open(const char *url);      /* new page: the current one goes on the history */
int web_back(void);                  /* one page back; 0 when there is none */
void web_reload(void);
void web_layout(int cols);           /* wrap width in characters */
int web_line_count(void);
const char *web_line(int i, int *style);
int web_span_count(void);
const struct web_span *web_span_at(int i);
int web_link_count(void);
const char *web_link(int k);
const char *web_url(void);
const char *web_title(void);
int web_depth(void);                 /* pages behind this one */
void web_follow(int k);
int web_looks_like_url(const char *s);
int web_command(const char *line);   /* shell: browse / web ... */
