/* cc: a small C compiler that lives inside genesis.
 *
 * `cc file.c` compiles a C subset into genesis bytecode and runs it at once
 * in a sandboxed virtual machine (every load and store is bounds-checked, so
 * a buggy program cannot touch the kernel). The design follows the classic
 * one-pass c4 approach: precedence climbing straight into stack code.
 *
 * Language: int, char, void, pointers, arrays (global and local), functions
 * with recursion and any order of definition, if/else, while, do/while, for,
 * break, continue, return, sizeof, casts, ?:, all the usual operators
 * including compound assignment and ++/--, string and char literals, global
 * initializers with constants.
 * Library: printf (%d %i %u %x %c %s %%, width and 0-padding), putchar,
 * puts, malloc, free, strlen, strcmp, strcpy, memset, exit,
 * tcp_connect, tcp_send, tcp_recv, tcp_close. */

extern void console_putc(char ch);
extern void console_puts(const char *text);
/* kernel/net.c: TCP for programs */
extern int net_tcp_connect(const char *host, unsigned int port);
extern int tcp_send(int s, const unsigned char *data, unsigned int len);
extern int tcp_recv(int s, unsigned char *buf, unsigned int max, unsigned int ms);
extern void tcp_close(int s);

enum { CODE_MAX = 32768, MEM_SIZE = 512 * 1024, STACK_SIZE = 64 * 1024, SYM_MAX = 1024, LOCAL_MAX = 256,
       STEP_LIMIT = 100000000 };

/* tokens; Assign..Brak are ordered by precedence */
enum { Num = 128, Str, Id, KwInt, KwChar, KwVoid, KwIf, KwElse, KwWhile, KwFor, KwDo, KwReturn, KwBreak,
       KwContinue, KwSizeof,
       Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec,
       Brak };

/* opcodes */
enum { EXIT, LEA, IMM, JMP, JSR, BZ, BNZ, ENT, ADJ, LEV, LI, LC, SI, SC, PSH,
       OR, XOR, AND, EQ, NE, LT, GT, LE, GE, SHL, SHR, ADD, SUB, MUL, DIV, MOD, SYS };

/* types: CHAR, INT, then +PTR per level of pointer */
enum { CHAR, INT, PTR };

/* symbol classes */
enum { CNone, CFun, CSys, CGlo, CLoc };

/* builtins */
enum { S_PRINTF, S_PUTCHAR, S_PUTS, S_MALLOC, S_FREE, S_STRLEN, S_STRCMP, S_STRCPY, S_MEMSET, S_EXIT,
       S_TCP_CONNECT, S_TCP_SEND, S_TCP_RECV, S_TCP_CLOSE };

struct sym {
    const char *name;
    int len;
    int tk;
    int cls, type, val, arr;
    int hcls, htype, hval, harr; /* the outer meaning a local hides */
    int defined, fix;            /* functions: body seen; chain of calls waiting for it */
};

static int code[CODE_MAX];
static int ncode, lastop;
static unsigned char mem[MEM_SIZE];
static int data_top, heap_top;
static struct sym syms[SYM_MAX];
static int nsyms;
static int locals[LOCAL_MAX];
static int nlocals;

static const char *p;
static int line;
static int tk, ival, aop, cur;
static int ty;
static int loc, maxloc;
static int brk_chain, cont_chain, in_loop;
static const char *file_name;

static void *jb[5];
static int failed;
static int exit_code;
static int exiting;

static void put_num(int v) {
    char buf[12];
    int n = 0;
    unsigned int u = v < 0 ? 0u - (unsigned int)v : (unsigned int)v;
    if (v < 0) {
        console_putc('-');
    }
    do {
        buf[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u != 0);
    while (n > 0) {
        console_putc(buf[--n]);
    }
}

__attribute__((noreturn)) static void fail(const char *msg) {
    console_puts("cc: ");
    if (line > 0) {
        console_puts(file_name);
        console_putc(':');
        put_num(line);
        console_puts(": ");
    } else {
        console_puts("runtime error: ");
    }
    console_puts(msg);
    console_putc('\n');
    failed = 1;
    __builtin_longjmp(jb, 1);
}

/* ---------- lexer ---------- */

static int is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

static int sym_find(const char *name, int len) {
    for (int i = 0; i < nsyms; i++) {
        if (syms[i].len == len) {
            int k = 0;
            while (k < len && syms[i].name[k] == name[k]) {
                k++;
            }
            if (k == len) {
                return i;
            }
        }
    }
    if (nsyms >= SYM_MAX) {
        fail("too many names");
    }
    struct sym *s = &syms[nsyms];
    struct sym zero = {0};
    *s = zero;
    s->name = name;
    s->len = len;
    s->tk = Id;
    return nsyms++;
}

static int escape(void) {
    int c = (unsigned char)*p++;
    if (c != '\\') {
        return c;
    }
    c = (unsigned char)*p++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return 8;
    case 'e': return 27;
    case 'x': {
        int v = 0;
        for (int i = 0; i < 2; i++) {
            int h = *p;
            if (is_digit(h)) v = v * 16 + h - '0';
            else if (h >= 'a' && h <= 'f') v = v * 16 + h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v = v * 16 + h - 'A' + 10;
            else break;
            p++;
        }
        return v;
    }
    case 0: p--; fail("unfinished escape");
    default: return c;
    }
}

static void next(void) {
    for (;;) {
        int c = (unsigned char)*p;
        if (c == 0) {
            tk = 0;
            return;
        }
        p++;
        if (c == '\n') {
            line++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            continue;
        }
        if (c == '#') { /* #include and friends: nothing to do */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (is_alpha(c)) {
            const char *start = p - 1;
            while (is_alpha(*p) || is_digit(*p)) p++;
            cur = sym_find(start, (int)(p - start));
            tk = syms[cur].tk;
            return;
        }
        if (is_digit(c)) {
            unsigned int v = (unsigned int)(c - '0');
            if (c == '0' && (*p == 'x' || *p == 'X')) {
                p++;
                v = 0;
                for (;;) {
                    int h = *p;
                    if (is_digit(h)) v = v * 16u + (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f') v = v * 16u + (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v = v * 16u + (unsigned int)(h - 'A' + 10);
                    else break;
                    p++;
                }
            } else {
                while (is_digit(*p)) v = v * 10u + (unsigned int)(*p++ - '0');
            }
            ival = (int)v;
            tk = Num;
            return;
        }
        if (c == '/' && *p == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '/' && *p == '*') {
            p++;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p += 2;
            continue;
        }
        if (c == '\'') {
            ival = escape();
            if (*p != '\'') fail("bad character literal");
            p++;
            tk = Num;
            return;
        }
        if (c == '"') {
            ival = data_top;
            while (*p && *p != '"') {
                if (*p == '\n') fail("newline in string");
                int ch = escape();
                if (data_top >= MEM_SIZE - STACK_SIZE - 16) fail("out of data space");
                mem[data_top++] = (unsigned char)ch;
            }
            if (*p != '"') fail("unterminated string");
            p++;
            tk = Str;
            return;
        }
        aop = 0;
        switch (c) {
        case '=': if (*p == '=') { p++; tk = Eq; } else tk = Assign; return;
        case '+':
            if (*p == '+') { p++; tk = Inc; }
            else if (*p == '=') { p++; tk = Assign; aop = ADD; }
            else tk = Add;
            return;
        case '-':
            if (*p == '-') { p++; tk = Dec; }
            else if (*p == '=') { p++; tk = Assign; aop = SUB; }
            else tk = Sub;
            return;
        case '*': if (*p == '=') { p++; tk = Assign; aop = MUL; } else tk = Mul; return;
        case '/': if (*p == '=') { p++; tk = Assign; aop = DIV; } else tk = Div; return;
        case '%': if (*p == '=') { p++; tk = Assign; aop = MOD; } else tk = Mod; return;
        case '!': if (*p == '=') { p++; tk = Ne; } else tk = '!'; return;
        case '<':
            if (*p == '=') { p++; tk = Le; }
            else if (*p == '<') { p++; if (*p == '=') { p++; tk = Assign; aop = SHL; } else tk = Shl; }
            else tk = Lt;
            return;
        case '>':
            if (*p == '=') { p++; tk = Ge; }
            else if (*p == '>') { p++; if (*p == '=') { p++; tk = Assign; aop = SHR; } else tk = Shr; }
            else tk = Gt;
            return;
        case '|':
            if (*p == '|') { p++; tk = Lor; }
            else if (*p == '=') { p++; tk = Assign; aop = OR; }
            else tk = Or;
            return;
        case '&':
            if (*p == '&') { p++; tk = Lan; }
            else if (*p == '=') { p++; tk = Assign; aop = AND; }
            else tk = And;
            return;
        case '^': if (*p == '=') { p++; tk = Assign; aop = XOR; } else tk = Xor; return;
        case '?': tk = Cond; return;
        case '[': tk = Brak; return;
        case '~': case ';': case '{': case '}': case '(': case ')': case ']': case ',': case ':':
            tk = c;
            return;
        default:
            fail("unexpected character");
        }
    }
}

static void expect(int t, const char *msg) {
    if (tk != t) fail(msg);
    next();
}

/* ---------- code ---------- */

static void emit(int v) {
    if (ncode >= CODE_MAX) fail("program too big");
    code[ncode++] = v;
}

static void op(int o) {
    lastop = ncode;
    emit(o);
}

static void op1(int o, int v) {
    op(o);
    emit(v);
}

static int last_is(int o) {
    return lastop == ncode - 1 && code[lastop] == o;
}

static int load_op(int t) {
    return t == CHAR ? LC : LI;
}

static int step(int t) {
    return t > PTR ? 4 : 1; /* ++ and pointer arithmetic: int* and deeper move 4 bytes */
}

/* ---------- expressions ---------- */

static int parse_type(void) {
    int t;
    if (tk == KwInt) t = INT;
    else if (tk == KwChar || tk == KwVoid) t = CHAR;
    else return -1;
    next();
    while (tk == Mul) {
        next();
        t += PTR;
    }
    return t;
}

static void lvalue_to_address(void) {
    if (last_is(LC) || last_is(LI)) {
        code[lastop] = PSH;
    } else {
        fail("not assignable");
    }
}

static void expr(int lev) {
    int t;
    if (tk == 0) {
        fail("unexpected end of file");
    } else if (tk == Num) {
        op1(IMM, ival);
        next();
        ty = INT;
    } else if (tk == Str) {
        op1(IMM, ival);
        next();
        while (tk == Str) next();
        mem[data_top++] = 0;
        data_top = (data_top + 3) & ~3;
        ty = CHAR + PTR;
    } else if (tk == KwSizeof) {
        next();
        expect('(', "( expected after sizeof");
        t = parse_type();
        if (t < 0) fail("type expected in sizeof");
        expect(')', ") expected");
        op1(IMM, t == CHAR ? 1 : 4);
        ty = INT;
    } else if (tk == Id) {
        struct sym *s = &syms[cur];
        next();
        if (tk == '(') {
            next();
            t = 0;
            while (tk != ')') {
                expr(Assign);
                op(PSH);
                t++;
                if (tk == ',') next();
                else if (tk != ')') fail(", or ) expected in call");
            }
            next();
            if (s->cls == CSys) {
                op(SYS);
                emit(s->val);
                emit(t);
            } else if (s->cls == CFun || s->cls == CNone) {
                if (s->cls == CNone) {
                    s->cls = CFun;
                    s->type = INT;
                }
                op(JSR);
                if (s->defined) {
                    emit(s->val);
                } else {
                    emit(s->fix);
                    s->fix = ncode; /* index + 1 of the hole */
                }
            } else {
                fail("not a function");
            }
            if (t) op1(ADJ, t);
            ty = s->type;
        } else {
            if (s->cls == CLoc) op1(LEA, s->val);
            else if (s->cls == CGlo) op1(IMM, s->val);
            else fail("undeclared name");
            if (s->arr) {
                ty = s->type + PTR;
            } else {
                ty = s->type;
                op(load_op(ty));
            }
        }
    } else if (tk == '(') {
        next();
        if (tk == KwInt || tk == KwChar || tk == KwVoid) {
            t = parse_type();
            expect(')', ") expected after cast");
            expr(Inc);
            ty = t;
        } else {
            expr(Assign);
            expect(')', ") expected");
        }
    } else if (tk == Mul) {
        next();
        expr(Inc);
        if (ty < PTR) fail("dereferencing a non-pointer");
        ty -= PTR;
        op(load_op(ty));
    } else if (tk == And) {
        next();
        expr(Inc);
        if (last_is(LC) || last_is(LI)) {
            ncode--;
        } else {
            fail("cannot take the address");
        }
        ty += PTR;
    } else if (tk == '!') {
        next();
        expr(Inc);
        op(PSH);
        op1(IMM, 0);
        op(EQ);
        ty = INT;
    } else if (tk == '~') {
        next();
        expr(Inc);
        op(PSH);
        op1(IMM, -1);
        op(XOR);
        ty = INT;
    } else if (tk == Add) {
        next();
        expr(Inc);
        ty = INT;
    } else if (tk == Sub) {
        next();
        if (tk == Num) {
            op1(IMM, (int)(0u - (unsigned int)ival));
            next();
        } else {
            op1(IMM, -1);
            op(PSH);
            expr(Inc);
            op(MUL);
        }
        ty = INT;
    } else if (tk == Inc || tk == Dec) {
        t = tk;
        next();
        expr(Inc);
        if (last_is(LC)) { code[lastop] = PSH; op(LC); }
        else if (last_is(LI)) { code[lastop] = PSH; op(LI); }
        else fail("++/-- needs a variable");
        op(PSH);
        op1(IMM, step(ty));
        op(t == Inc ? ADD : SUB);
        op(ty == CHAR ? SC : SI);
    } else {
        fail("bad expression");
    }

    while (tk >= lev) {
        t = ty;
        if (tk == Assign) {
            int o = aop;
            next();
            lvalue_to_address();
            if (o) {
                op(load_op(t));
                op(PSH);
                expr(Assign);
                if ((o == ADD || o == SUB) && t > PTR) {
                    op(PSH);
                    op1(IMM, 4);
                    op(MUL);
                }
                op(o);
            } else {
                expr(Assign);
            }
            op((ty = t) == CHAR ? SC : SI);
        } else if (tk == Cond) {
            next();
            op1(BZ, 0);
            int d = ncode - 1;
            expr(Assign);
            expect(':', ": expected in ?:");
            op1(JMP, 0);
            code[d] = ncode;
            d = ncode - 1;
            expr(Cond);
            code[d] = ncode;
        } else if (tk == Lor || tk == Lan) {
            int which = tk;
            next();
            op1(which == Lor ? BNZ : BZ, 0);
            int d = ncode - 1;
            expr(which == Lor ? Lan : Or);
            code[d] = ncode;
            op(PSH);
            op1(IMM, 0);
            op(NE);
            ty = INT;
        } else if (tk >= Or && tk <= Shr) {
            static const int ops[] = {OR, XOR, AND, EQ, NE, LT, GT, LE, GE, SHL, SHR};
            static const int nextlev[] = {Xor, And, Eq, Lt, Lt, Shl, Shl, Shl, Shl, Add, Add};
            int k = tk - Or;
            next();
            op(PSH);
            expr(nextlev[k]);
            op(ops[k]);
            ty = INT;
        } else if (tk == Add) {
            next();
            op(PSH);
            expr(Mul);
            if ((ty = t) > PTR) {
                op(PSH);
                op1(IMM, 4);
                op(MUL);
            }
            op(ADD);
        } else if (tk == Sub) {
            next();
            op(PSH);
            expr(Mul);
            if (t >= PTR && t == ty) {
                op(SUB);
                if (t > PTR) {
                    op(PSH);
                    op1(IMM, 4);
                    op(DIV);
                }
                ty = INT;
            } else if (t > PTR) {
                op(PSH);
                op1(IMM, 4);
                op(MUL);
                op(SUB);
                ty = t;
            } else {
                op(SUB);
                ty = t;
            }
        } else if (tk == Mul || tk == Div || tk == Mod) {
            int o = tk == Mul ? MUL : tk == Div ? DIV : MOD;
            next();
            op(PSH);
            expr(Inc);
            op(o);
            ty = INT;
        } else if (tk == Inc || tk == Dec) {
            int inc = tk == Inc;
            if (last_is(LC)) { code[lastop] = PSH; op(LC); }
            else if (last_is(LI)) { code[lastop] = PSH; op(LI); }
            else fail("++/-- needs a variable");
            op(PSH);
            op1(IMM, step(ty));
            op(inc ? ADD : SUB);
            op(ty == CHAR ? SC : SI);
            op(PSH);
            op1(IMM, step(ty));
            op(inc ? SUB : ADD);
            next();
        } else if (tk == Brak) {
            next();
            op(PSH);
            expr(Assign);
            expect(']', "] expected");
            if (t > PTR) {
                op(PSH);
                op1(IMM, 4);
                op(MUL);
            } else if (t < PTR) {
                fail("indexing a non-pointer");
            }
            op(ADD);
            ty = t - PTR;
            op(load_op(ty));
        } else {
            fail("bad expression");
        }
    }
}

/* ---------- statements ---------- */

static void patch_chain(int chain, int target) {
    while (chain) {
        int nxt = code[chain - 1];
        code[chain - 1] = target;
        chain = nxt;
    }
}

static void hide(int idx) {
    struct sym *s = &syms[idx];
    for (int i = 0; i < nlocals; i++) {
        if (locals[i] == idx) return; /* already a local of this function */
    }
    if (nlocals >= LOCAL_MAX) fail("too many locals");
    locals[nlocals++] = idx;
    s->hcls = s->cls;
    s->htype = s->type;
    s->hval = s->val;
    s->harr = s->arr;
}

static void unhide_all(void) {
    for (int i = 0; i < nlocals; i++) {
        struct sym *s = &syms[locals[i]];
        s->cls = s->hcls;
        s->type = s->htype;
        s->val = s->hval;
        s->arr = s->harr;
    }
    nlocals = 0;
}

static int const_int(void) {
    int neg = 0;
    if (tk == Sub) {
        neg = 1;
        next();
    }
    if (tk != Num) fail("constant expected");
    int v = ival;
    next();
    return neg ? (int)(0u - (unsigned int)v) : v;
}

static void stmt(void);

static void loop_body(int *brk, int *cont) {
    int ob = brk_chain, oc = cont_chain, ol = in_loop;
    brk_chain = cont_chain = 0;
    in_loop = 1;
    stmt();
    *brk = brk_chain;
    *cont = cont_chain;
    brk_chain = ob;
    cont_chain = oc;
    in_loop = ol;
}

static void stmt(void) {
    int brk, cont;
    if (tk == KwIf) {
        next();
        expect('(', "( expected after if");
        expr(Assign);
        expect(')', ") expected");
        op1(BZ, 0);
        int b = ncode - 1;
        stmt();
        if (tk == KwElse) {
            next();
            op1(JMP, 0);
            code[b] = ncode;
            b = ncode - 1;
            stmt();
        }
        code[b] = ncode;
    } else if (tk == KwWhile) {
        next();
        int top = ncode;
        expect('(', "( expected after while");
        expr(Assign);
        expect(')', ") expected");
        op1(BZ, 0);
        int b = ncode - 1;
        loop_body(&brk, &cont);
        op1(JMP, top);
        code[b] = ncode;
        patch_chain(brk, ncode);
        patch_chain(cont, top);
    } else if (tk == KwDo) {
        next();
        int top = ncode;
        loop_body(&brk, &cont);
        if (tk != KwWhile) fail("while expected after do");
        next();
        int cond = ncode;
        expect('(', "( expected");
        expr(Assign);
        expect(')', ") expected");
        expect(';', "; expected");
        op1(BNZ, top);
        patch_chain(brk, ncode);
        patch_chain(cont, cond);
    } else if (tk == KwFor) {
        next();
        expect('(', "( expected after for");
        if (tk != ';') expr(Assign);
        expect(';', "; expected in for");
        int cond = ncode;
        int b = 0;
        if (tk != ';') {
            expr(Assign);
            op1(BZ, 0);
            b = ncode - 1;
        }
        expect(';', "; expected in for");
        op1(JMP, 0);
        int to_body = ncode - 1;
        int incr = ncode;
        if (tk != ')') expr(Assign);
        expect(')', ") expected");
        op1(JMP, cond);
        code[to_body] = ncode;
        loop_body(&brk, &cont);
        op1(JMP, incr);
        if (b) code[b] = ncode;
        patch_chain(brk, ncode);
        patch_chain(cont, incr);
    } else if (tk == KwReturn) {
        next();
        if (tk != ';') expr(Assign);
        op(LEV);
        expect(';', "; expected");
    } else if (tk == KwBreak || tk == KwContinue) {
        int is_break = tk == KwBreak;
        if (!in_loop) fail(is_break ? "break outside a loop" : "continue outside a loop");
        next();
        expect(';', "; expected");
        op(JMP);
        if (is_break) {
            emit(brk_chain);
            brk_chain = ncode;
        } else {
            emit(cont_chain);
            cont_chain = ncode;
        }
    } else if (tk == '{') {
        next();
        while (tk != '}') {
            if (tk == 0) fail("} expected");
            stmt();
        }
        next();
    } else if (tk == ';') {
        next();
    } else if (tk == KwInt || tk == KwChar || tk == KwVoid) {
        int bt = tk == KwInt ? INT : CHAR;
        next();
        for (;;) {
            int t = bt;
            while (tk == Mul) {
                next();
                t += PTR;
            }
            if (tk != Id) fail("name expected");
            int idx = cur;
            struct sym *s = &syms[idx];
            next();
            hide(idx);
            s->cls = CLoc;
            s->type = t;
            if (tk == Brak) {
                next();
                int n = const_int();
                expect(']', "] expected");
                if (n <= 0) fail("bad array size");
                loc += t == CHAR ? (n + 3) / 4 : n;
                s->arr = 1;
            } else {
                loc++;
                s->arr = 0;
            }
            s->val = -loc * 4;
            if (loc > maxloc) maxloc = loc;
            if (tk == Assign && aop == 0 && !s->arr) {
                next();
                op1(LEA, s->val);
                op(PSH);
                expr(Assign);
                op(t == CHAR ? SC : SI);
            }
            if (tk == ',') {
                next();
                continue;
            }
            expect(';', "; expected");
            break;
        }
    } else {
        expr(Assign);
        expect(';', "; expected");
    }
}

/* ---------- program ---------- */

static void program(void) {
    next();
    while (tk) {
        int bt;
        if (tk == KwInt) bt = INT;
        else if (tk == KwChar || tk == KwVoid) bt = CHAR;
        else fail("declaration expected");
        next();
        while (tk != ';' && tk != '}') {
            int t = bt;
            while (tk == Mul) {
                next();
                t += PTR;
            }
            if (tk != Id) fail("name expected");
            int fidx = cur;
            next();
            if (tk == '(') {
                struct sym *f = &syms[fidx];
                if (f->cls != CNone && f->cls != CFun) fail("name already used");
                if (f->defined) fail("function defined twice");
                f->cls = CFun;
                f->type = t;
                next();
                int params[32];
                int np = 0;
                if (tk == KwVoid && *p != '*') {
                    const char *save = p;
                    int sline = line;
                    next();
                    if (tk != ')') { /* "void x" is not valid; let the loop complain */
                        p = save;
                        line = sline;
                        tk = KwVoid;
                    }
                }
                while (tk != ')') {
                    int pt = parse_type();
                    if (pt < 0) fail("parameter type expected");
                    if (tk != Id) fail("parameter name expected");
                    if (np >= 32) fail("too many parameters");
                    hide(cur);
                    syms[cur].cls = CLoc;
                    syms[cur].type = pt;
                    syms[cur].arr = 0;
                    params[np++] = cur;
                    next();
                    if (tk == ',') next();
                    else if (tk != ')') fail(", or ) expected");
                }
                next();
                if (tk == ';') { /* prototype */
                    unhide_all();
                    break;
                }
                if (tk != '{') fail("{ expected");
                for (int k = 0; k < np; k++) {
                    syms[params[k]].val = 4 * (np - k + 1);
                }
                f->defined = 1;
                f->val = ncode;
                patch_chain(f->fix, ncode);
                f->fix = 0;
                loc = maxloc = 0;
                op1(ENT, 0);
                int ent = ncode - 1;
                next();
                while (tk != '}') {
                    if (tk == 0) fail("} expected at end of function");
                    stmt();
                }
                op1(IMM, 0);
                op(LEV);
                code[ent] = maxloc * 4;
                unhide_all();
                break; /* tk is the closing }, eaten below */
            } else {
                struct sym *g = &syms[fidx];
                if (g->cls != CNone) fail("name already used");
                g->cls = CGlo;
                g->type = t;
                data_top = (data_top + 3) & ~3;
                g->val = data_top;
                int bytes = 4;
                g->arr = 0;
                if (tk == Brak) {
                    next();
                    int n = const_int();
                    expect(']', "] expected");
                    if (n <= 0) fail("bad array size");
                    bytes = t == CHAR ? n : n * 4;
                    g->arr = 1;
                }
                if (data_top + bytes >= MEM_SIZE - STACK_SIZE - 16) fail("out of data space");
                data_top += bytes;
                if (tk == Assign && aop == 0 && !g->arr) {
                    next();
                    int v = const_int();
                    if (t == CHAR) {
                        mem[g->val] = (unsigned char)v;
                    } else {
                        mem[g->val] = (unsigned char)v;
                        mem[g->val + 1] = (unsigned char)(v >> 8);
                        mem[g->val + 2] = (unsigned char)(v >> 16);
                        mem[g->val + 3] = (unsigned char)(v >> 24);
                    }
                }
            }
            if (tk == ',') next();
            else if (tk != ';') fail("; expected");
        }
        next();
    }
}

/* ---------- virtual machine ---------- */

static int sp, bp;

static void check(int addr, int n) {
    if (addr < 16 || addr > MEM_SIZE - n) fail(addr >= 0 && addr < 16 ? "null pointer" : "address out of range");
}

static int ld32(int a) {
    check(a, 4);
    return (int)((unsigned int)mem[a] | (unsigned int)mem[a + 1] << 8 | (unsigned int)mem[a + 2] << 16 |
                 (unsigned int)mem[a + 3] << 24);
}

static void st32(int a, int v) {
    check(a, 4);
    mem[a] = (unsigned char)v;
    mem[a + 1] = (unsigned char)(v >> 8);
    mem[a + 2] = (unsigned char)(v >> 16);
    mem[a + 3] = (unsigned char)(v >> 24);
}

static int ld8(int a) {
    check(a, 1);
    return mem[a];
}

static void push(int v) {
    sp -= 4;
    if (sp < MEM_SIZE - STACK_SIZE) fail("stack overflow (recursion too deep?)");
    st32(sp, v);
}

static int pop(void) {
    int v = ld32(sp);
    sp += 4;
    return v;
}

static void put_padded(const char *s, int len, int width, int left, char pad) {
    if (!left) for (int i = len; i < width; i++) console_putc(pad);
    for (int i = 0; i < len; i++) console_putc(s[i]);
    if (left) for (int i = len; i < width; i++) console_putc(' ');
}

static int vm_printf(int fmt, int *args, int nargs) {
    int ai = 0;
    int count = 0;
    for (;;) {
        int c = ld8(fmt++);
        if (c == 0) break;
        if (c != '%') {
            console_putc((char)c);
            count++;
            continue;
        }
        int left = 0, width = 0;
        char pad = ' ';
        c = ld8(fmt++);
        if (c == '-') { left = 1; c = ld8(fmt++); }
        if (c == '0') { pad = '0'; c = ld8(fmt++); }
        while (is_digit(c)) { width = width * 10 + c - '0'; c = ld8(fmt++); }
        if (c == 'l') c = ld8(fmt++);
        if (c == '%') { console_putc('%'); count++; continue; }
        if (c == 0) break;
        int v = ai < nargs ? args[ai++] : 0;
        char buf[16];
        int n = 0;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            unsigned int u = (unsigned int)v;
            int neg = 0;
            if ((c == 'd' || c == 'i') && v < 0) { neg = 1; u = 0u - u; }
            unsigned int base = (c == 'x' || c == 'X') ? 16u : 10u;
            char tmp[12];
            int k = 0;
            do {
                unsigned int d = u % base;
                tmp[k++] = (char)(d < 10 ? '0' + d : (c == 'X' ? 'A' : 'a') + d - 10);
                u /= base;
            } while (u);
            if (neg && pad == '0') { console_putc('-'); count++; if (width) width--; }
            else if (neg) buf[n++] = '-';
            while (k) buf[n++] = tmp[--k];
            put_padded(buf, n, width, left, left ? ' ' : pad);
            count += n > width ? n : width;
        } else if (c == 'c') {
            buf[0] = (char)v;
            put_padded(buf, 1, width, left, ' ');
            count += 1 > width ? 1 : width;
        } else if (c == 's') {
            int len = 0;
            while (ld8(v + len)) len++;
            if (!left) for (int i = len; i < width; i++) console_putc(' ');
            for (int i = 0; i < len; i++) console_putc((char)mem[v + i]);
            if (left) for (int i = len; i < width; i++) console_putc(' ');
            count += len > width ? len : width;
        } else {
            console_putc('%');
            console_putc((char)c);
            count += 2;
        }
    }
    return count;
}

static int sys_call(int id, int n) {
    int args[16];
    if (n > 16) fail("too many arguments");
    for (int i = 0; i < n; i++) args[i] = ld32(sp + 4 * (n - 1 - i));
    int a0 = n > 0 ? args[0] : 0;
    switch (id) {
    case S_PRINTF:
        if (n < 1) fail("printf needs a format");
        return vm_printf(a0, args + 1, n - 1);
    case S_PUTCHAR:
        console_putc((char)a0);
        return a0 & 0xff;
    case S_PUTS: {
        int i = 0;
        while (ld8(a0 + i)) console_putc((char)mem[a0 + i++]);
        console_putc('\n');
        return 0;
    }
    case S_MALLOC: {
        if (a0 < 0) return 0;
        int start = (heap_top + 7) & ~7;
        if (start + 8 + a0 > MEM_SIZE - STACK_SIZE) return 0;
        st32(start, a0);
        heap_top = start + 8 + a0;
        for (int i = 0; i < a0; i++) mem[start + 8 + i] = 0;
        return start + 8;
    }
    case S_FREE:
        return 0; /* the arena is freed whole when the program ends */
    case S_STRLEN: {
        int i = 0;
        while (ld8(a0 + i)) i++;
        return i;
    }
    case S_STRCMP: {
        int b0 = n > 1 ? args[1] : 0;
        for (int i = 0;; i++) {
            int x = ld8(a0 + i), y = ld8(b0 + i);
            if (x != y) return x - y;
            if (x == 0) return 0;
        }
    }
    case S_STRCPY: {
        int b0 = n > 1 ? args[1] : 0;
        int i = 0;
        for (;; i++) {
            int x = ld8(b0 + i);
            check(a0 + i, 1);
            mem[a0 + i] = (unsigned char)x;
            if (x == 0) break;
        }
        return a0;
    }
    case S_MEMSET: {
        int v = n > 1 ? args[1] : 0, len = n > 2 ? args[2] : 0;
        if (len > 0) {
            check(a0, 1);
            check(a0 + len - 1, 1);
            for (int i = 0; i < len; i++) mem[a0 + i] = (unsigned char)v;
        }
        return a0;
    }
    case S_EXIT:
        exit_code = a0;
        exiting = 1;
        __builtin_longjmp(jb, 1);
    case S_TCP_CONNECT: { /* tcp_connect("host", port) -> socket or -1 */
        char host[64];
        int i = 0;
        for (; i < 63 && ld8(a0 + i); i++) host[i] = (char)mem[a0 + i];
        host[i] = 0;
        return net_tcp_connect(host, n > 1 ? (unsigned int)args[1] : 80u);
    }
    case S_TCP_SEND: { /* tcp_send(s, buf, len) */
        int b0 = n > 1 ? args[1] : 0, len = n > 2 ? args[2] : 0;
        if (len <= 0) return 0;
        check(b0, 1);
        check(b0 + len - 1, 1);
        return tcp_send(a0, mem + b0, (unsigned int)len);
    }
    case S_TCP_RECV: { /* tcp_recv(s, buf, max): bytes, 0 = closed, waits up to 3 s */
        int b0 = n > 1 ? args[1] : 0, max = n > 2 ? args[2] : 0;
        if (max <= 0) return 0;
        check(b0, 1);
        check(b0 + max - 1, 1);
        return tcp_recv(a0, mem + b0, (unsigned int)max, 3000u);
    }
    case S_TCP_CLOSE:
        tcp_close(a0);
        return 0;
    default:
        fail("unknown builtin");
    }
}

static int run(int entry) {
    int pc = entry, a = 0;
    unsigned int steps = 0;
    sp = MEM_SIZE;
    bp = sp;
    push(0); /* main returns to code[0] = EXIT */
    for (;;) {
        if (++steps > STEP_LIMIT) fail("program ran too long (stopped)");
        if (pc < 0 || pc >= ncode) fail("jump out of the program");
        int o = code[pc++];
        unsigned int x, y;
        switch (o) {
        case EXIT: return a;
        case LEA: a = bp + code[pc++]; break;
        case IMM: a = code[pc++]; break;
        case JMP: pc = code[pc]; break;
        case JSR: push(pc + 1); pc = code[pc]; break;
        case BZ: pc = a ? pc + 1 : code[pc]; break;
        case BNZ: pc = a ? code[pc] : pc + 1; break;
        case ENT:
            push(bp);
            bp = sp;
            sp -= code[pc++];
            if (sp < MEM_SIZE - STACK_SIZE) fail("stack overflow (recursion too deep?)");
            break;
        case ADJ: sp += code[pc++] * 4; break;
        case LEV: sp = bp; bp = pop(); pc = pop(); break;
        case LI: a = ld32(a); break;
        case LC: a = ld8(a); break;
        case SI: st32(pop(), a); break;
        case SC: { int addr = pop(); check(addr, 1); mem[addr] = (unsigned char)a; a &= 0xff; break; }
        case PSH: push(a); break;
        case SYS: {
            int id = code[pc++];
            int n = code[pc++];
            a = sys_call(id, n);
            break;
        }
        default: {
            int b = pop();
            x = (unsigned int)b;
            y = (unsigned int)a;
            switch (o) {
            case OR: a = b | a; break;
            case XOR: a = b ^ a; break;
            case AND: a = b & a; break;
            case EQ: a = b == a; break;
            case NE: a = b != a; break;
            case LT: a = b < a; break;
            case GT: a = b > a; break;
            case LE: a = b <= a; break;
            case GE: a = b >= a; break;
            case SHL: a = (int)(x << (y & 31u)); break;
            case SHR: a = b >> (a & 31); break;
            case ADD: a = (int)(x + y); break;
            case SUB: a = (int)(x - y); break;
            case MUL: a = (int)(x * y); break;
            case DIV:
                if (a == 0) fail("division by zero");
                a = (b == (int)0x80000000u && a == -1) ? b : b / a;
                break;
            case MOD:
                if (a == 0) fail("division by zero");
                a = (b == (int)0x80000000u && a == -1) ? 0 : b % a;
                break;
            default: fail("bad instruction");
            }
        }
        }
    }
}

static void add_name(const char *name, int tk_kind, int cls, int val) {
    int len = 0;
    while (name[len]) len++;
    int i = sym_find(name, len);
    syms[i].tk = tk_kind;
    syms[i].cls = cls;
    syms[i].val = val;
    syms[i].type = INT;
}

/* Compile SOURCE (NUL-terminated) and run its main(). Returns the exit code,
 * or -1 when compiling or running failed (the reason is already printed). */
int cc_run(const char *source, const char *name) {
    static const char *const kws[] = {"int", "char", "void", "if", "else", "while", "for", "do", "return",
                                      "break", "continue", "sizeof"};
    static const char *const libs[] = {"printf", "putchar", "puts", "malloc", "free", "strlen", "strcmp",
                                       "strcpy", "memset", "exit", "tcp_connect", "tcp_send",
                                       "tcp_recv", "tcp_close"};
    nsyms = 0;
    nlocals = 0;
    ncode = 0;
    lastop = -1;
    failed = 0;
    exit_code = 0;
    line = 0;
    brk_chain = cont_chain = in_loop = 0;
    file_name = name;
    for (int i = 0; i < MEM_SIZE; i++) mem[i] = 0;
    data_top = 16;
    exiting = 0;
    if (__builtin_setjmp(jb)) {
        return exiting ? exit_code : -1;
    }
    for (unsigned int i = 0; i < sizeof kws / sizeof kws[0]; i++) add_name(kws[i], KwInt + (int)i, CNone, 0);
    for (unsigned int i = 0; i < sizeof libs / sizeof libs[0]; i++) add_name(libs[i], Id, CSys, (int)i);
    emit(EXIT);
    p = source;
    line = 1;
    program();
    for (int i = 0; i < nsyms; i++) {
        if (syms[i].cls == CFun && !syms[i].defined && syms[i].fix) {
            line = 0;
            console_puts("cc: undefined function ");
            for (int k = 0; k < syms[i].len; k++) console_putc(syms[i].name[k]);
            console_putc('\n');
            return -1;
        }
    }
    int m = sym_find("main", 4);
    if (syms[m].cls != CFun || !syms[m].defined) {
        console_puts("cc: no main()\n");
        return -1;
    }
    heap_top = (data_top + 15) & ~15;
    line = 0; /* errors from here on are runtime errors */
    return run(syms[m].val);
}

/* Sample programs seeded into /docs. */
const char cc_sample_hello[] =
    "// hello.c: cc /docs/hello.c\n"
    "int main() {\n"
    "    printf(\"hello from C, %d + %d = %d\\n\", 2, 3, 2 + 3);\n"
    "    return 0;\n"
    "}\n";

const char cc_sample_fib[] =
    "// fib.c: recursion, loops, arrays, pointers\n"
    "int memo[30];\n"
    "\n"
    "int fib(int n) {\n"
    "    if (n < 2) return n;\n"
    "    if (memo[n]) return memo[n];\n"
    "    return memo[n] = fib(n - 1) + fib(n - 2);\n"
    "}\n"
    "\n"
    "void reverse(char *s) {\n"
    "    char *e = s + strlen(s) - 1;\n"
    "    while (s < e) { char t = *s; *s++ = *e; *e-- = t; }\n"
    "}\n"
    "\n"
    "int main() {\n"
    "    int i;\n"
    "    for (i = 0; i < 15; i++) printf(\"%d \", fib(i));\n"
    "    printf(\"\\n\");\n"
    "    char word[16];\n"
    "    strcpy(word, \"genesis\");\n"
    "    reverse(word);\n"
    "    printf(\"%s\\n\", word);\n"
    "    return 0;\n"
    "}\n";

const unsigned int cc_sample_hello_len = sizeof cc_sample_hello - 1;
const unsigned int cc_sample_fib_len = sizeof cc_sample_fib - 1;
