#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  DAG for topological sort of section link dependencies             */
/* ------------------------------------------------------------------ */

#define TOPO_MAX 64

typedef struct {
    char   names[TOPO_MAX][64];  /* section names               */
    size_t count;
    int    deps[TOPO_MAX][TOPO_MAX]; /* deps[i][j]: section i links to section j */
    int    state[TOPO_MAX];          /* 0=unvisited 1=in-progress 2=done         */
    int    order[TOPO_MAX];          /* result: indices in topo order             */
    size_t order_count;
} TopoGraph;

static int topo_index(const TopoGraph *g, const char *name) {
    for (size_t i = 0; i < g->count; i++)
        if (strcmp(g->names[i], name) == 0) return (int)i;
    return -1;
}

/* DFS topo sort. Returns -1 on cycle. */
static int topo_dfs(TopoGraph *g, int node) {
    if (g->state[node] == 1) return -1; /* cycle */
    if (g->state[node] == 2) return 0;  /* already processed */
    g->state[node] = 1;
    for (size_t j = 0; j < g->count; j++) {
        if (g->deps[node][j]) {
            if (topo_dfs(g, (int)j)) return -1;
        }
    }
    g->state[node] = 2;
    g->order[g->order_count++] = node;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Collect label-ref references from a value tree                    */
/* ------------------------------------------------------------------ */

static void collect_refs(const PastaValue *v, const TopoGraph *g,
                          int refs[TOPO_MAX]) {
    if (!v) return;
    switch (pasta_type(v)) {
    case PASTA_LABEL: {
        int idx = topo_index(g, pasta_get_label(v));
        if (idx >= 0) refs[idx] = 1;
        return;
    }
    case PASTA_ARRAY:
        for (size_t i = 0; i < pasta_count(v); i++)
            collect_refs(pasta_array_get(v, i), g, refs);
        return;
    case PASTA_MAP:
        for (size_t i = 0; i < pasta_count(v); i++)
            collect_refs(pasta_map_value(v, i), g, refs);
        return;
    default:
        return;
    }
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
/*  Pass 3 entry point                                                 */
/* ------------------------------------------------------------------ */

PastaValue *alf_pass3_link(PastaValue *output, AlfContext *ctx,
                            AlfResult *result) {
    if (!output || pasta_type(output) != PASTA_MAP) return output;

    size_t nsec = pasta_count(output);
    if (nsec == 0) return output;
    if (nsec > TOPO_MAX) {
        /* Too many sections for the static graph — just do a single pass
           without topo ordering (handles the common case). */
        nsec = TOPO_MAX;
    }

    /* ---- Build topo graph ---- */
    TopoGraph g;
    memset(&g, 0, sizeof(g));
    g.count = nsec;

    for (size_t i = 0; i < nsec; i++) {
        const char *name = pasta_map_key(output, i);
        size_t nlen = strlen(name);
        if (nlen >= sizeof(g.names[0])) nlen = sizeof(g.names[0]) - 1;
        memcpy(g.names[i], name, nlen);
        g.names[i][nlen] = '\0';
    }

    for (size_t i = 0; i < nsec; i++) {
        int refs[TOPO_MAX] = {0};
        collect_refs(pasta_map_value(output, i), &g, refs);
        for (size_t j = 0; j < nsec; j++)
            g.deps[i][j] = refs[j];
    }

    /* ---- Topo sort ---- */
    for (size_t i = 0; i < nsec; i++) {
        if (g.state[i] == 0) {
            if (topo_dfs(&g, (int)i)) {
                alf_set_error(result, ALF_ERR_CYCLE, 3, NULL,
                              "cycle detected in section link graph");
                pasta_free(output);
                return NULL;
            }
        }
    }
    /* g.order is in post-order (dependencies before dependants) */

    /* ---- Resolve links in topo order ---- */
    LinkCtx lctx;
    lctx.inputs      = ctx->inputs;
    lctx.input_count = ctx->input_count;
    lctx.resolved    = pasta_new_map();
    if (!lctx.resolved) {
        alf_set_error(result, ALF_ERR_ALLOC, 3, NULL, "allocation failed");
        pasta_free(output);
        return NULL;
    }

    for (size_t oi = 0; oi < g.order_count; oi++) {
        int idx = g.order[oi];
        const char    *secname = g.names[idx];
        const PastaValue *secval = pasta_map_get(output, secname);
        if (!secval) continue; /* shouldn't happen */

        PastaValue *resolved_sec = resolve_links_value(secval, &lctx);
        if (!resolved_sec) {
            alf_set_error(result, ALF_ERR_ALLOC, 3, secname, "allocation failed");
            pasta_free(lctx.resolved);
            pasta_free(output);
            return NULL;
        }

        /* Add to resolved so subsequent sections can reference it */
        PastaValue *rc = alf_value_clone(resolved_sec);
        if (!rc || pasta_set(lctx.resolved, secname, rc)) {
            pasta_free(rc);
            pasta_free(resolved_sec);
            pasta_free(lctx.resolved);
            pasta_free(output);
            alf_set_error(result, ALF_ERR_ALLOC, 3, secname, "allocation failed");
            return NULL;
        }
        pasta_free(resolved_sec);
    }

    pasta_free(output);
    return lctx.resolved;
}
