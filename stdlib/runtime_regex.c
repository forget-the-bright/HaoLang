/*
 * HaoLang 运行时 —— 正则（v0.28）
 * ------------------------------------------------------------
 *  自包含回溯树匹配，零外部库。Pattern 句柄存于 Long?（8 字节 GC 块）。
 *  支持：字面量 . * + ? | () 捕获 [] \d\w\s 及常用转义 ^ $
 *  下标按 UTF-8 字节（与 HaoString.len 一致）。
 */
#include "runtime_internal.h"

#define HAO_RE_MAX_NODES  512
#define HAO_RE_MAX_CAPS   32
#define HAO_RE_MAX_STEPS  1000000  /* 单次 search/fullmatch 总步数上限，防灾难性回溯 */

typedef enum {
    N_CHAR, N_ANY, N_CLASS, N_NCLASS, N_BOL, N_EOL,
    N_SEQ, N_ALT, N_REP, N_CAP
} Nt;

typedef struct ReAst ReAst;
struct ReAst {
    Nt      kind;
    int32_t ch;
    int32_t cls;
    int32_t cap;
    int32_t minr, maxr; /* maxr=-1 无限 */
    ReAst*  a;
    ReAst*  b;
};

typedef struct {
    uint8_t classes[HAO_RE_MAX_NODES][32];
    int32_t nclasses;
    int32_t ncaps;
    char    err;
    ReAst   pool[HAO_RE_MAX_NODES];
    int32_t npool;
    ReAst*  root;
    int32_t steps; /* 整次 search/fullmatch 累计步数（入口清零） */
} HaoReProg;

typedef struct {
    HaoReProg* prog;
    const char* pat;
    const char* pend;
} ReComp;

static void re_cls_set(uint8_t* bm, int c) {
    if (c >= 0 && c < 256) bm[c >> 3] |= (uint8_t)(1u << (c & 7));
}
static int re_cls_has(const uint8_t* bm, int c) {
    if (c < 0 || c >= 256) return 0;
    return (bm[c >> 3] >> (c & 7)) & 1;
}
static void re_cls_digit(uint8_t* bm) {
    int c; for (c = '0'; c <= '9'; c++) re_cls_set(bm, c);
}
static void re_cls_word(uint8_t* bm) {
    int c;
    for (c = '0'; c <= '9'; c++) re_cls_set(bm, c);
    for (c = 'A'; c <= 'Z'; c++) re_cls_set(bm, c);
    for (c = 'a'; c <= 'z'; c++) re_cls_set(bm, c);
    re_cls_set(bm, '_');
}
static void re_cls_space(uint8_t* bm) {
    re_cls_set(bm, ' '); re_cls_set(bm, '\t'); re_cls_set(bm, '\n');
    re_cls_set(bm, '\r'); re_cls_set(bm, '\f'); re_cls_set(bm, '\v');
}

static ReAst* re_new(ReComp* c, Nt k) {
    if (c->prog->npool >= HAO_RE_MAX_NODES) { c->prog->err = 1; return NULL; }
    ReAst* n = &c->prog->pool[c->prog->npool++];
    memset(n, 0, sizeof(*n));
    n->kind = k;
    n->maxr = -1;
    return n;
}

static ReAst* re_p_alt(ReComp* c);
static ReAst* re_p_seq(ReComp* c);
static ReAst* re_p_piece(ReComp* c);
static ReAst* re_p_atom(ReComp* c);

static int re_esc(ReComp* c, uint8_t* bm, int* isc) {
    if (c->pat >= c->pend) { c->prog->err = 1; return -1; }
    char e = *c->pat++;
    *isc = 0;
    switch (e) {
    case 'd': memset(bm, 0, 32); re_cls_digit(bm); *isc = 1; return 0;
    case 'D': {
        uint8_t t[32]; memset(t, 0, 32); re_cls_digit(t);
        memset(bm, 0xff, 32);
        { int i; for (i = 0; i < 32; i++) bm[i] ^= t[i]; }
        *isc = 1; return 0;
    }
    case 'w': memset(bm, 0, 32); re_cls_word(bm); *isc = 1; return 0;
    case 'W': {
        uint8_t t[32]; memset(t, 0, 32); re_cls_word(t);
        memset(bm, 0xff, 32);
        { int i; for (i = 0; i < 32; i++) bm[i] ^= t[i]; }
        *isc = 1; return 0;
    }
    case 's': memset(bm, 0, 32); re_cls_space(bm); *isc = 1; return 0;
    case 'S': {
        uint8_t t[32]; memset(t, 0, 32); re_cls_space(t);
        memset(bm, 0xff, 32);
        { int i; for (i = 0; i < 32; i++) bm[i] ^= t[i]; }
        *isc = 1; return 0;
    }
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    default:  return (unsigned char)e;
    }
}

static ReAst* re_p_class(ReComp* c) {
    int neg = 0;
    if (c->pat < c->pend && *c->pat == '^') { neg = 1; c->pat++; }
    if (c->prog->nclasses >= HAO_RE_MAX_NODES) { c->prog->err = 1; return NULL; }
    int ci = c->prog->nclasses++;
    memset(c->prog->classes[ci], 0, 32);
    int first = 1;
    while (c->pat < c->pend && (*c->pat != ']' || first)) {
        first = 0;
        int a;
        if (*c->pat == '\\') {
            c->pat++;
            uint8_t tmp[32]; int isc = 0;
            a = re_esc(c, tmp, &isc);
            if (c->prog->err) return NULL;
            if (isc) {
                int b;
                for (b = 0; b < 256; b++)
                    if (re_cls_has(tmp, b)) re_cls_set(c->prog->classes[ci], b);
                continue;
            }
        } else {
            a = (unsigned char)*c->pat++;
        }
        if (c->pat + 1 < c->pend && *c->pat == '-' && c->pat[1] != ']') {
            c->pat++;
            int b;
            if (*c->pat == '\\') {
                c->pat++;
                uint8_t tmp[32]; int isc = 0;
                b = re_esc(c, tmp, &isc);
                if (c->prog->err || isc) { c->prog->err = 1; return NULL; }
            } else {
                b = (unsigned char)*c->pat++;
            }
            if (a > b) { int t = a; a = b; b = t; }
            for (; a <= b; a++) re_cls_set(c->prog->classes[ci], a);
        } else {
            re_cls_set(c->prog->classes[ci], a);
        }
    }
    if (c->pat >= c->pend || *c->pat != ']') { c->prog->err = 1; return NULL; }
    c->pat++;
    ReAst* n = re_new(c, neg ? N_NCLASS : N_CLASS);
    if (!n) return NULL;
    n->cls = ci;
    return n;
}

static ReAst* re_p_atom(ReComp* c) {
    if (c->pat >= c->pend) { c->prog->err = 1; return NULL; }
    char ch = *c->pat++;
    if (ch == '(') {
        if (c->prog->ncaps >= HAO_RE_MAX_CAPS) { c->prog->err = 1; return NULL; }
        int cap = c->prog->ncaps++;
        ReAst* body = re_p_alt(c);
        if (!body || c->pat >= c->pend || *c->pat != ')') {
            c->prog->err = 1; return NULL;
        }
        c->pat++;
        ReAst* n = re_new(c, N_CAP);
        if (!n) return NULL;
        n->cap = cap;
        n->a = body;
        return n;
    }
    if (ch == '.') return re_new(c, N_ANY);
    if (ch == '^') return re_new(c, N_BOL);
    if (ch == '$') return re_new(c, N_EOL);
    if (ch == '[') return re_p_class(c);
    if (ch == '\\') {
        uint8_t bm[32]; int isc = 0;
        int v = re_esc(c, bm, &isc);
        if (c->prog->err) return NULL;
        if (isc) {
            if (c->prog->nclasses >= HAO_RE_MAX_NODES) {
                c->prog->err = 1; return NULL;
            }
            int ci = c->prog->nclasses++;
            memcpy(c->prog->classes[ci], bm, 32);
            ReAst* n = re_new(c, N_CLASS);
            if (!n) return NULL;
            n->cls = ci;
            return n;
        }
        ReAst* n = re_new(c, N_CHAR);
        if (!n) return NULL;
        n->ch = v;
        return n;
    }
    if (ch == '*' || ch == '+' || ch == '?' || ch == ')' || ch == '|') {
        c->prog->err = 1; return NULL;
    }
    ReAst* n = re_new(c, N_CHAR);
    if (!n) return NULL;
    n->ch = (unsigned char)ch;
    return n;
}

static ReAst* re_p_piece(ReComp* c) {
    ReAst* atom = re_p_atom(c);
    if (!atom) return NULL;
    if (c->pat >= c->pend) return atom;
    char q = *c->pat;
    if (q != '*' && q != '+' && q != '?') return atom;
    c->pat++;
    ReAst* n = re_new(c, N_REP);
    if (!n) return NULL;
    n->a = atom;
    if (q == '*') { n->minr = 0; n->maxr = -1; }
    else if (q == '+') { n->minr = 1; n->maxr = -1; }
    else { n->minr = 0; n->maxr = 1; }
    return n;
}

static ReAst* re_empty(ReComp* c) {
    /* min=max=0 的 REP：匹配空串 */
    ReAst* body = re_new(c, N_CHAR);
    if (!body) return NULL;
    body->ch = 0;
    ReAst* n = re_new(c, N_REP);
    if (!n) return NULL;
    n->a = body;
    n->minr = 0;
    n->maxr = 0;
    return n;
}

static ReAst* re_p_seq(ReComp* c) {
    ReAst* left = NULL;
    while (c->pat < c->pend && *c->pat != ')' && *c->pat != '|') {
        ReAst* p = re_p_piece(c);
        if (!p) return NULL;
        if (!left) left = p;
        else {
            ReAst* s = re_new(c, N_SEQ);
            if (!s) return NULL;
            s->a = left;
            s->b = p;
            left = s;
        }
    }
    return left ? left : re_empty(c);
}

static ReAst* re_p_alt(ReComp* c) {
    ReAst* left = re_p_seq(c);
    if (!left) return NULL;
    while (c->pat < c->pend && *c->pat == '|') {
        c->pat++;
        ReAst* right = re_p_seq(c);
        if (!right) return NULL;
        ReAst* n = re_new(c, N_ALT);
        if (!n) return NULL;
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static int re_starts_with_bol(ReAst* n) {
    if (!n) return 0;
    if (n->kind == N_BOL) return 1;
    if (n->kind == N_CAP) return re_starts_with_bol(n->a);
    if (n->kind == N_SEQ) return re_starts_with_bol(n->a);
    return 0;
}

static int re_exec(HaoReProg* prog, ReAst* node, const char* text, int32_t tlen,
                   int32_t pos, int32_t* caps);

static int re_exec_rep(HaoReProg* prog, ReAst* node, const char* text, int32_t tlen,
                       int32_t pos, int32_t* caps, int32_t count) {
    int32_t save[HAO_RE_MAX_CAPS * 2];
    size_t capbytes = sizeof(int32_t) * (size_t)prog->ncaps * 2;

    if (node->maxr >= 0 && count > node->maxr) return -1;

    /* 贪婪：先尽量多吃 */
    if (node->maxr < 0 || count < node->maxr) {
        memcpy(save, caps, capbytes);
        int np = re_exec(prog, node->a, text, tlen, pos, caps);
        if (np >= 0 && np > pos) {
            int r = re_exec_rep(prog, node, text, tlen, np, caps, count + 1);
            if (r >= 0) return r;
            memcpy(caps, save, capbytes);
        } else {
            memcpy(caps, save, capbytes);
        }
    }
    if (count >= node->minr) return pos;
    return -1;
}

static int re_exec(HaoReProg* prog, ReAst* node, const char* text, int32_t tlen,
                   int32_t pos, int32_t* caps) {
    if (!node) return pos;
    if (++prog->steps > HAO_RE_MAX_STEPS) return -1;
    switch (node->kind) {
    case N_CHAR:
        if (pos >= tlen || (unsigned char)text[pos] != (unsigned char)node->ch)
            return -1;
        return pos + 1;
    case N_ANY:
        if (pos >= tlen) return -1;
        return pos + 1;
    case N_CLASS:
        if (pos >= tlen) return -1;
        if (!re_cls_has(prog->classes[node->cls], (unsigned char)text[pos]))
            return -1;
        return pos + 1;
    case N_NCLASS:
        if (pos >= tlen) return -1;
        if (re_cls_has(prog->classes[node->cls], (unsigned char)text[pos]))
            return -1;
        return pos + 1;
    case N_BOL:
        return pos == 0 ? pos : -1;
    case N_EOL:
        return pos == tlen ? pos : -1;
    case N_SEQ: {
        int p1 = re_exec(prog, node->a, text, tlen, pos, caps);
        if (p1 < 0) return -1;
        return re_exec(prog, node->b, text, tlen, p1, caps);
    }
    case N_ALT: {
        int32_t save[HAO_RE_MAX_CAPS * 2];
        size_t capbytes = sizeof(int32_t) * (size_t)prog->ncaps * 2;
        memcpy(save, caps, capbytes);
        int r = re_exec(prog, node->a, text, tlen, pos, caps);
        if (r >= 0) return r;
        memcpy(caps, save, capbytes);
        return re_exec(prog, node->b, text, tlen, pos, caps);
    }
    case N_REP:
        return re_exec_rep(prog, node, text, tlen, pos, caps, 0);
    case N_CAP: {
        int32_t old_s = caps[node->cap * 2];
        int32_t old_e = caps[node->cap * 2 + 1];
        caps[node->cap * 2] = pos;
        int r = re_exec(prog, node->a, text, tlen, pos, caps);
        if (r < 0) {
            caps[node->cap * 2] = old_s;
            caps[node->cap * 2 + 1] = old_e;
            return -1;
        }
        caps[node->cap * 2 + 1] = r;
        return r;
    }
    default:
        return -1;
    }
}

static HaoReProg* re_compile_str(const char* pat, int32_t plen) {
    HaoReProg* prog = (HaoReProg*)malloc(sizeof(HaoReProg));
    if (!prog) return NULL;
    memset(prog, 0, sizeof(*prog));
    prog->ncaps = 1; /* 组 0 预留 */

    ReComp c;
    c.prog = prog;
    c.pat = pat;
    c.pend = pat + plen;

    ReAst* body = re_p_alt(&c);
    if (!body || c.pat != c.pend || prog->err) {
        free(prog);
        return NULL;
    }

    ReAst* cap0 = re_new(&c, N_CAP);
    if (!cap0) { free(prog); return NULL; }
    cap0->cap = 0;
    cap0->a = body;
    prog->root = cap0;
    return prog;
}

static void re_store(int64_t* unit, HaoReProg* p) {
    *unit = (int64_t)(uintptr_t)p;
}
static HaoReProg* re_load(const int64_t* unit) {
    return (HaoReProg*)(uintptr_t)(*unit);
}

/* 将交错 caps 写入 Matcher 侧 [Int] 元素区；out_* 为 NULL 则跳过 */
static void re_write_caps(HaoReProg* p, const int32_t* caps, int32_t from, int32_t end,
                          int32_t* out_s, int32_t* out_e) {
    int i;
    int32_t n;
    int64_t ns, ne;
    if (!out_s || !out_e) return;
    ns = hao_array_len(out_s);
    ne = hao_array_len(out_e);
    n = p->ncaps;
    if (n > (int32_t)ns) n = (int32_t)ns;
    if (n > (int32_t)ne) n = (int32_t)ne;
    for (i = 0; i < n; i++) {
        out_s[i] = caps[i * 2];
        out_e[i] = caps[i * 2 + 1];
    }
    if (n > 0) {
        out_s[0] = from;
        out_e[0] = end;
    }
}

/* 不重置 steps：整次 search 共用预算 */
static int re_try_at(HaoReProg* p, const char* text, int32_t tlen, int32_t from,
                     int32_t* out_end, int32_t* out_s, int32_t* out_e) {
    int32_t caps[HAO_RE_MAX_CAPS * 2];
    int i;
    for (i = 0; i < HAO_RE_MAX_CAPS * 2; i++) caps[i] = -1;
    int end = re_exec(p, p->root, text, tlen, from, caps);
    if (end < 0) return 0;
    re_write_caps(p, caps, from, end, out_s, out_e);
    if (out_end) *out_end = end;
    return 1;
}

/* 清扫回调：块已摘链，勿再 clear_finalizer；只释放原生 NFA */
static void hao_regex_unit_finalize(void* user) {
    int64_t* unit = (int64_t*)user;
    if (!unit) return;
    HaoReProg* p = re_load(unit);
    if (p) free(p);
    *unit = 0;
}

int8_t hao_regex_compile(int64_t* unit, HaoString* pattern) {
    if (!unit || !pattern) return 0;
    /* 对齐 channel：同一句柄盒禁止 remake（须先 free/close） */
    if (re_load(unit)) return 0;
    hao_gc_add_root(pattern); /* set_finalizer 持锁窗口；禁 is_heap_ptr 前置 */
    HaoReProg* prog = re_compile_str(pattern->data, pattern->len);
    if (!prog) {
        hao_gc_remove_root(pattern);
        return 0;
    }
    re_store(unit, prog);
    hao_gc_set_finalizer(unit, hao_regex_unit_finalize);
    hao_gc_remove_root(pattern);
    return 1;
}

int32_t hao_regex_group_count(int64_t* unit) {
    HaoReProg* p = re_load(unit);
    if (!p) return 0;
    return p->ncaps;
}

/* 捕获写入调用方 [Int] 元素区；prog 不共享状态。cap_* 可为 NULL。 */
int8_t hao_regex_search_into(int64_t* unit, HaoString* text, int32_t from,
                             int32_t* cap_s, int32_t* cap_e) {
    HaoReProg* p = re_load(unit);
    int32_t tlen;
    int anchored;
    int32_t pos;
    if (!p || !text || from < 0) return 0;
    tlen = text->len;
    if (from > tlen) return 0;

    p->steps = 0; /* 整次 search 共用步数预算 */
    anchored = re_starts_with_bol(p->root);
    for (pos = from; pos <= tlen; pos++) {
        int32_t end = -1;
        if (re_try_at(p, text->data, tlen, pos, &end, cap_s, cap_e))
            return 1;
        if (anchored) break;
        if (p->steps > HAO_RE_MAX_STEPS) break;
    }
    return 0;
}

int8_t hao_regex_search(int64_t* unit, HaoString* text, int32_t from,
                        int64_t* mstart, int64_t* mend) {
    HaoReProg* p = re_load(unit);
    int32_t tlen;
    int anchored;
    int32_t pos;
    if (!p || !text || from < 0) return 0;
    tlen = text->len;
    if (from > tlen) return 0;
    p->steps = 0;
    anchored = re_starts_with_bol(p->root);
    for (pos = from; pos <= tlen; pos++) {
        int32_t end = -1;
        if (re_try_at(p, text->data, tlen, pos, &end, NULL, NULL)) {
            if (mstart) *mstart = (int64_t)pos;
            if (mend) *mend = (int64_t)end;
            return 1;
        }
        if (anchored) break;
        if (p->steps > HAO_RE_MAX_STEPS) break;
    }
    return 0;
}

/* Matcher：写入 cap_s/cap_e */
int8_t hao_regex_search_simple(int64_t* unit, HaoString* text, int32_t from,
                               int32_t* cap_s, int32_t* cap_e) {
    return hao_regex_search_into(unit, text, from, cap_s, cap_e);
}

int8_t hao_regex_fullmatch_into(int64_t* unit, HaoString* text,
                                int32_t* cap_s, int32_t* cap_e) {
    HaoReProg* p = re_load(unit);
    int32_t end = -1;
    if (!p || !text) return 0;
    p->steps = 0;
    if (!re_try_at(p, text->data, text->len, 0, &end, cap_s, cap_e)) return 0;
    return end == text->len ? 1 : 0;
}

int8_t hao_regex_fullmatch(int64_t* unit, HaoString* text) {
    return hao_regex_fullmatch_into(unit, text, NULL, NULL);
}

void hao_regex_free(int64_t* unit) {
    if (!unit) return;
    hao_gc_clear_finalizer(unit);
    HaoReProg* p = re_load(unit);
    if (p) free(p);
    *unit = 0;
}
