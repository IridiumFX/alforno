#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  DAG for topological sort of section link dependencies             */
/* ------------------------------------------------------------------ */

/* Sized from the section count at run time: no ceiling on sections, none on
   name length.  It was a static 64 x 64 matrix of 64-byte names, and every
   section past the 64th, or with a name of 64+ characters, silently left the
   output (reported by Vulpes, 2026-11-20).

   Names point at the output map's own keys, which outlive the pass.  Links
   are adjacency lists (CSR: section i's targets are edges[off[i]..off[i+1]),
   ascending), so memory follows the links present, not N^2. */
typedef struct {
    const char *name;      /* the output map's key                        */
    size_t      idx;       /* its section index                           */
} NameIdx;

typedef struct {
    size_t   n;
    NameIdx *byname;       /* every section, sorted by name, for lookup   */
    size_t  *off;          /* n + 1 offsets into edges                    */
    size_t  *edges;        /* link targets, grouped by source section     */
    size_t   nedges, cap;
    size_t  *mark;         /* mark[j] == i + 1: i already links to j      */
} TopoGraph;

static void topo_free(TopoGraph *g) {
    free(g->byname); free(g->off); free(g->edges); free(g->mark);
}

static int cmp_byname(const void *a, const void *b) {
    const NameIdx *x = (const NameIdx *)a, *y = (const NameIdx *)b;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    return x->idx < y->idx ? -1 : x->idx > y->idx; /* equal: lowest first */
}

/* Index of the section called `name`, or -1.  With duplicate keys the
   first wins, as with pasta_map_get. */
static long topo_index(const TopoGraph *g, const char *name) {
    size_t lo = 0, hi = g->n;
    while (lo < hi) {               /* lower bound */
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(g->byname[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g->n && strcmp(g->byname[lo].name, name) == 0)
        return (long)g->byname[lo].idx;
    return -1;
}

static int cmp_size(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* ------------------------------------------------------------------ */
/*  Collect label-ref references from a value tree                    */
/* ------------------------------------------------------------------ */

/* Append to g->edges each section that v (inside section `from`) links to,
   once.  Returns -1 on allocation failure. */
static int collect_refs(const PastaValue *v, TopoGraph *g, size_t from) {
    if (!v) return 0;
    switch (pasta_type(v)) {
    case PASTA_LABEL: {
        long idx = topo_index(g, pasta_get_label(v));
        if (idx < 0 || g->mark[idx] == from + 1) return 0;
        g->mark[idx] = from + 1;
        if (g->nedges == g->cap) {
            size_t nc = g->cap ? g->cap * 2 : 64;
            size_t *t = (size_t *)realloc(g->edges, nc * sizeof *t);
            if (!t) return -1;
            g->edges = t;
            g->cap   = nc;
        }
        g->edges[g->nedges++] = (size_t)idx;
        return 0;
    }
    case PASTA_ARRAY:
        for (size_t i = 0; i < pasta_count(v); i++)
            if (collect_refs(pasta_array_get(v, i), g, from)) return -1;
        return 0;
    case PASTA_MAP:
        for (size_t i = 0; i < pasta_count(v); i++)
            if (collect_refs(pasta_map_value(v, i), g, from)) return -1;
        return 0;
    default:
        return 0;
    }
}

/* Post-order DFS from every unvisited section, in index order, each
   section's links in ascending order: the order the recursive matrix walk
   produced.  Iterative, so a long link chain cannot exhaust the C stack.
   Fills order[0..n).  Returns 0, 1 on a cycle, -1 on allocation failure. */
static int topo_sort(const TopoGraph *g, size_t *order) {
    size_t n = g->n, norder = 0, depth = 0;
    unsigned char *state = (unsigned char *)calloc(n, 1); /* 0 new, 1 open, 2 done */
    size_t *stack = (size_t *)malloc(n * sizeof *stack);
    size_t *pos   = (size_t *)malloc(n * sizeof *pos);    /* next edge per frame */
    int rc = 0;
    if (!state || !stack || !pos) { rc = -1; goto out; }

    for (size_t s = 0; s < n; s++) {
        if (state[s]) continue;
        state[s] = 1; stack[0] = s; pos[0] = g->off[s]; depth = 1;
        while (depth) {
            size_t node = stack[depth - 1];
            if (pos[depth - 1] < g->off[node + 1]) {
                size_t j = g->edges[pos[depth - 1]++];
                if (state[j] == 1) { rc = 1; goto out; }       /* cycle */
                if (state[j] == 0) {
                    state[j] = 1;
                    stack[depth] = j; pos[depth] = g->off[j]; depth++;
                }
            } else {
                state[node] = 2;
                order[norder++] = node;
                depth--;
            }
        }
    }
out:
    free(state); free(stack); free(pos);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Link lookup: output sections first, then inputs                   */
/* ------------------------------------------------------------------ */

typedef struct {
    PastaValue  *resolved;           /* sections resolved so far (output sections) */
    PastaValue **inputs;
    size_t       input_count;
} LinkCtx;

static const PastaValue *lookup_section(const char *name,
                                         const LinkCtx *lctx) {
    const PastaValue *v = pasta_map_get(lctx->resolved, name);
    if (v) return v;
    for (size_t i = 0; i < lctx->input_count; i++) {
        v = pasta_map_get(lctx->inputs[i], name);
        if (v) return v;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Recursive link resolution (builds a new value tree)               */
/* ------------------------------------------------------------------ */

/*
 * Walk v, replacing any PASTA_LABEL that matches a known section name
 * with a deep clone of that section's container.
 * Non-matching label-refs are kept as-is (no error).
 */
static PastaValue *resolve_links_value(const PastaValue *v,
                                        const LinkCtx *lctx) {
    switch (pasta_type(v)) {
    case PASTA_LABEL: {
        const char *name = pasta_get_label(v);
        const PastaValue *target = lookup_section(name, lctx);
        if (target) return alf_value_clone(target);
        return alf_value_clone(v);
    }
    case PASTA_ARRAY: {
        PastaValue *a = pasta_new_array();
        if (!a) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *item = resolve_links_value(pasta_array_get(v, i), lctx);
            if (!item) { pasta_free(a); return NULL; }
            if (pasta_push(a, item)) { pasta_free(item); pasta_free(a); return NULL; }
        }
        return a;
    }
    case PASTA_MAP: {
        PastaValue *m = pasta_new_map();
        if (!m) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *val = resolve_links_value(pasta_map_value(v, i), lctx);
            if (!val) { pasta_free(m); return NULL; }
            if (pasta_set(m, pasta_map_key(v, i), val)) {
                pasta_free(val); pasta_free(m); return NULL;
            }
        }
        return m;
    }
    default:
        return alf_value_clone(v);
    }
}

/* ------------------------------------------------------------------ */
/*  Pass 4 entry point                                                 */
/* ------------------------------------------------------------------ */

PastaValue *alf_pass4_link(PastaValue *output, AlfContext *ctx,
                            AlfResult *result) {
    if (!output || pasta_type(output) != PASTA_MAP) return output;

    size_t nsec = pasta_count(output);
    if (nsec == 0) return output;

    /* ---- Build topo graph ---- */
    TopoGraph g;
    memset(&g, 0, sizeof(g));
    g.n      = nsec;
    g.byname = (NameIdx *)malloc(nsec * sizeof *g.byname);
    g.off    = (size_t *)malloc((nsec + 1) * sizeof *g.off);
    g.mark   = (size_t *)calloc(nsec, sizeof *g.mark);
    size_t *order = (size_t *)malloc(nsec * sizeof *order);
    int built = g.byname && g.off && g.mark && order;

    if (built) {
        for (size_t i = 0; i < nsec; i++) {
            g.byname[i].name = pasta_map_key(output, i);
            g.byname[i].idx  = i;
        }
        qsort(g.byname, nsec, sizeof *g.byname, cmp_byname);

        for (size_t i = 0; i < nsec && built; i++) {
            g.off[i] = g.nedges;
            if (collect_refs(pasta_map_value(output, i), &g, i)) built = 0;
            else if (g.nedges - g.off[i] > 1)   /* edges is NULL until the first */
                qsort(g.edges + g.off[i], g.nedges - g.off[i],
                      sizeof *g.edges, cmp_size);
        }
        g.off[nsec] = g.nedges;
    }

    /* ---- Topo sort ---- */
    int sorted = built ? topo_sort(&g, order) : -1;
    topo_free(&g);
    if (sorted) {
        if (sorted > 0)
            alf_set_error(result, ALF_ERR_CYCLE, ALF_PASS_LINK, NULL,
                          "cycle detected in section link graph");
        else
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_LINK, NULL,
                          "allocation failed");
        free(order);
        pasta_free(output);
        return NULL;
    }
    /* order is post-order (dependencies before dependants), all nsec */

    /* ---- Resolve links in topo order ---- */
    LinkCtx lctx;
    lctx.inputs      = ctx->inputs;
    lctx.input_count = ctx->input_count;
    lctx.resolved    = pasta_new_map();
    if (!lctx.resolved) {
        alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_LINK, NULL, "allocation failed");
        free(order);
        pasta_free(output);
        return NULL;
    }

    for (size_t oi = 0; oi < nsec; oi++) {
        /* By index, not by a copied name: no lookup can miss. */
        size_t idx = order[oi];
        const char       *secname = pasta_map_key(output, idx);
        const PastaValue *secval  = pasta_map_value(output, idx);

        PastaValue *resolved_sec = resolve_links_value(secval, &lctx);
        if (!resolved_sec) {
            alf_set_error_at(ctx, result, ALF_ERR_ALLOC, ALF_PASS_LINK, secname, "allocation failed");
            free(order);
            pasta_free(lctx.resolved);
            pasta_free(output);
            return NULL;
        }

        /* Add to resolved so subsequent sections can reference it */
        PastaValue *rc = alf_value_clone(resolved_sec);
        if (!rc || pasta_set(lctx.resolved, secname, rc)) {
            pasta_free(rc);
            pasta_free(resolved_sec);
            free(order);
            pasta_free(lctx.resolved);
            pasta_free(output);
            alf_set_error_at(ctx, result, ALF_ERR_ALLOC, ALF_PASS_LINK, secname, "allocation failed");
            return NULL;
        }
        pasta_free(resolved_sec);
    }

    free(order);
    pasta_free(output);
    return lctx.resolved;
}
