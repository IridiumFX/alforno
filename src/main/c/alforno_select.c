#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  Pass 6: Prune / Filter                                             */
/*                                                                     */
/*  Selectors are anchored on a section and drill down by '/':         */
/*  "@server", "@server/tls", "@server/tls/cert".  filter keeps only   */
/*  the selected paths (plus ancestors); prune drops them.  Both run   */
/*  after link and validate, on the fully resolved tree.               */
/* ------------------------------------------------------------------ */

#define ALF_MAX_SEG 16

/* Split "@a/b/c" (after the '@') into segment pointers within buf, which is
   overwritten.  Returns the segment count, or -1 if the selector is malformed:
   it must start with '@', be non-empty, contain no empty segments, and fit. */
static int split_selector(const char *sel, char *buf, size_t bufsz,
                           const char **segs) {
    if (!sel || sel[0] != '@') return -1;
    sel++;                                       /* skip '@' */
    size_t n = strlen(sel);
    if (n == 0 || n >= bufsz) return -1;
    memcpy(buf, sel, n + 1);

    int nseg = 0;
    segs[nseg++] = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (nseg >= ALF_MAX_SEG) return -1;
            segs[nseg++] = p + 1;
        }
    }
    for (int i = 0; i < nseg; i++)
        if (segs[i][0] == '\0') return -1;       /* empty segment: @a/, @a//b */
    return nseg;
}

/* Return a clone of `node` with the key-path seg[0..nseg-1] removed.  A path
   that does not exist is a no-op (node cloned unchanged). */
static PastaValue *clone_without(const PastaValue *node,
                                  const char **seg, int nseg) {
    if (pasta_type(node) != PASTA_MAP)
        return alf_value_clone(node);            /* can't navigate: no-op */

    PastaValue *m = pasta_new_map();
    if (!m) return NULL;

    for (size_t i = 0; i < pasta_count(node); i++) {
        const char       *key = pasta_map_key(node, i);
        const PastaValue *val = pasta_map_value(node, i);

        if (strcmp(key, seg[0]) == 0) {
            if (nseg == 1) continue;             /* target: drop it */
            PastaValue *child = clone_without(val, seg + 1, nseg - 1);
            if (!child || pasta_set(m, key, child)) {
                pasta_free(child); pasta_free(m); return NULL;
            }
        } else {
            PastaValue *c = alf_value_clone(val);
            if (!c || pasta_set(m, key, c)) {
                pasta_free(c); pasta_free(m); return NULL;
            }
        }
    }
    return m;
}

/* Copy the subtree at path seg[0..nseg-1] from src into dst at the same path,
   creating intermediate maps as needed.  A path missing in src is a no-op.
   Returns 0 on success, -1 on allocation failure. */
static int graft(PastaValue *dst, const PastaValue *src,
                  const char **seg, int nseg) {
    if (pasta_type(src) != PASTA_MAP) return 0;          /* path into non-map */
    const PastaValue *srcval = pasta_map_get(src, seg[0]);
    if (!srcval) return 0;                               /* missing: no-op */

    if (nseg == 1) {
        PastaValue *c = alf_value_clone(srcval);
        if (!c || pasta_set(dst, seg[0], c)) { pasta_free(c); return -1; }
        return 0;
    }

    PastaValue *dstchild = (PastaValue *)pasta_map_get(dst, seg[0]);
    if (dstchild && pasta_type(dstchild) != PASTA_MAP)
        return 0;                            /* already a leaf here: leave it */
    if (!dstchild) {
        dstchild = pasta_new_map();
        if (!dstchild) return -1;
        if (pasta_set(dst, seg[0], dstchild)) { pasta_free(dstchild); return -1; }
    }
    return graft(dstchild, srcval, seg + 1, nseg - 1);
}

PastaValue *alf_pass6_select(PastaValue *output, AlfContext *ctx,
                              AlfResult *result) {
    if (ctx->filter_count == 0 && ctx->prune_count == 0)
        return output;                          /* nothing to do */
    if (pasta_type(output) != PASTA_MAP)
        return output;                          /* only maps carry sections */

    char        buf[512];
    const char *segs[ALF_MAX_SEG];
    PastaValue *cur = output;

    /* filter first: keep only the selected paths */
    if (ctx->filter_count > 0) {
        PastaValue *kept = pasta_new_map();
        if (!kept) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SELECT, NULL,
                          "allocation failed");
            pasta_free(cur); return NULL;
        }
        for (size_t i = 0; i < ctx->filter_count; i++) {
            int nseg = split_selector(ctx->filter_sel[i], buf, sizeof buf, segs);
            if (nseg < 0) {
                char msg[320];
                snprintf(msg, sizeof msg, "malformed filter selector '%s'",
                         ctx->filter_sel[i]);
                alf_set_error(result, ALF_ERR_BAD_SELECTOR, ALF_PASS_SELECT,
                              NULL, msg);
                pasta_free(kept); pasta_free(cur); return NULL;
            }
            if (graft(kept, cur, segs, nseg)) {
                alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SELECT, NULL,
                              "allocation failed");
                pasta_free(kept); pasta_free(cur); return NULL;
            }
        }
        pasta_free(cur);
        cur = kept;
    }

    /* prune: drop the selected paths */
    for (size_t i = 0; i < ctx->prune_count; i++) {
        int nseg = split_selector(ctx->prune_sel[i], buf, sizeof buf, segs);
        if (nseg < 0) {
            char msg[320];
            snprintf(msg, sizeof msg, "malformed prune selector '%s'",
                     ctx->prune_sel[i]);
            alf_set_error(result, ALF_ERR_BAD_SELECTOR, ALF_PASS_SELECT, NULL, msg);
            pasta_free(cur); return NULL;
        }
        PastaValue *next = clone_without(cur, segs, nseg);
        if (!next) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SELECT, NULL,
                          "allocation failed");
            pasta_free(cur); return NULL;
        }
        pasta_free(cur);
        cur = next;
    }
    return cur;
}

/* ------------------------------------------------------------------ */
/*  Directive collection: @prune / @filter from inputs                 */
/* ------------------------------------------------------------------ */

/* Append the string elements of array `arr` to sel[] (bounded by count). */
static void collect_from_array(const PastaValue *arr, char **sel, size_t *count) {
    if (pasta_type(arr) != PASTA_ARRAY) return;
    for (size_t i = 0; i < pasta_count(arr); i++) {
        const PastaValue *e = pasta_array_get(arr, i);
        if (pasta_type(e) != PASTA_STRING) continue;
        if (*count >= ALF_MAX_SELECTORS) break;
        char *dup = alf_strdup(pasta_get_string(e));
        if (dup) sel[(*count)++] = dup;
    }
}

int alf_collect_select_directives(AlfContext *ctx, AlfResult *result) {
    (void)result;
    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        const PastaValue *pr = pasta_map_get(inp, "prune");
        const PastaValue *fl = pasta_map_get(inp, "filter");
        if (!pr && !fl) continue;

        if (pr) collect_from_array(pr, ctx->prune_sel,  &ctx->prune_count);
        if (fl) collect_from_array(fl, ctx->filter_sel, &ctx->filter_count);

        /* rebuild the input without the @prune / @filter directive sections */
        PastaValue *stripped = pasta_new_map();
        if (!stripped) continue;
        for (size_t j = 0; j < pasta_count(inp); j++) {
            const char *key = pasta_map_key(inp, j);
            if (strcmp(key, "prune") == 0 || strcmp(key, "filter") == 0)
                continue;
            PastaValue *c = alf_value_clone(pasta_map_value(inp, j));
            if (!c || pasta_set(stripped, key, c)) {
                pasta_free(c); pasta_free(stripped); stripped = NULL; break;
            }
        }
        if (stripped) { pasta_free(ctx->inputs[i]); ctx->inputs[i] = stripped; }
    }
    return 0;
}
