#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  Aggregate                                                          */
/* ------------------------------------------------------------------ */

/*
 * Merge all sections from all inputs with last-write-wins semantics.
 * Each section is merged at the field level if both occurrences are maps;
 * otherwise the later value replaces the earlier one entirely.
 */
static PastaValue *do_aggregate(AlfContext *ctx, AlfResult *result) {
    PastaValue *out = pasta_new_map();
    if (!out) {
        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
        return NULL;
    }

    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        for (size_t j = 0; j < pasta_count(inp); j++) {
            const char    *key  = pasta_map_key(inp, j);
            const PastaValue *val = pasta_map_value(inp, j);

            /* Check if this section already exists in the output */
            const PastaValue *existing = pasta_map_get(out, key);
            if (existing) {
                /* Merge or replace */
                PastaValue *merged = alf_section_merge(existing, val);
                if (!merged) {
                    alf_set_error(result, ALF_ERR_ALLOC, 2, key, "allocation failed");
                    pasta_free(out);
                    return NULL;
                }
                /* Rebuild the output map with the merged section.
                   Since the public API has no in-place replace, we rebuild. */
                PastaValue *rebuilt = pasta_new_map();
                if (!rebuilt) {
                    pasta_free(merged); pasta_free(out);
                    alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                    return NULL;
                }
                for (size_t k = 0; k < pasta_count(out); k++) {
                    const char *okey = pasta_map_key(out, k);
                    PastaValue *oval;
                    if (strcmp(okey, key) == 0) {
                        oval = merged;
                        merged = NULL;
                    } else {
                        oval = alf_value_clone(pasta_map_value(out, k));
                    }
                    if (!oval || pasta_set(rebuilt, okey, oval)) {
                        pasta_free(oval); pasta_free(merged);
                        pasta_free(rebuilt); pasta_free(out);
                        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                        return NULL;
                    }
                }
                pasta_free(out);
                out = rebuilt;
            } else {
                PastaValue *c = alf_value_clone(val);
                if (!c || pasta_set(out, key, c)) {
                    pasta_free(c); pasta_free(out);
                    alf_set_error(result, ALF_ERR_ALLOC, 2, key, "allocation failed");
                    return NULL;
                }
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Deep merge helper                                                  */
/* ------------------------------------------------------------------ */

/*
 * Recursively merge two maps: if a key exists in both and both values
 * are maps, merge them recursively. Otherwise the overlay value wins.
 */
static PastaValue *alf_deep_merge(const PastaValue *base,
                                    const PastaValue *overlay) {
    if (pasta_type(base) != PASTA_MAP || pasta_type(overlay) != PASTA_MAP)
        return alf_value_clone(overlay);

    PastaValue *result = pasta_new_map();
    if (!result) return NULL;

    /* Copy base fields, recursively merging where overlay also has the key */
    for (size_t i = 0; i < pasta_count(base); i++) {
        const char *key = pasta_map_key(base, i);
        const PastaValue *bval = pasta_map_value(base, i);
        const PastaValue *oval = pasta_map_get(overlay, key);

        PastaValue *merged;
        if (oval && pasta_type(bval) == PASTA_MAP && pasta_type(oval) == PASTA_MAP)
            merged = alf_deep_merge(bval, oval);
        else if (oval)
            merged = alf_value_clone(oval);
        else
            merged = alf_value_clone(bval);

        if (!merged || pasta_set(result, key, merged)) {
            pasta_free(merged); pasta_free(result); return NULL;
        }
    }

    /* Add overlay-only keys */
    for (size_t i = 0; i < pasta_count(overlay); i++) {
        const char *key = pasta_map_key(overlay, i);
        if (pasta_map_get(base, key)) continue;
        PastaValue *c = alf_value_clone(pasta_map_value(overlay, i));
        if (!c || pasta_set(result, key, c)) {
            pasta_free(c); pasta_free(result); return NULL;
        }
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  Conflate                                                           */
/* ------------------------------------------------------------------ */

/*
 * Gather and merge all sections named in the consumes array, in input order.
 * Returns NULL (without error) if no matching sections are found.
 */
static PastaValue *gather_consumes(AlfContext *ctx,
                                    const PastaValue *consumes) {
    PastaValue *acc = NULL;

    for (size_t inp_i = 0; inp_i < ctx->input_count; inp_i++) {
        PastaValue *inp = ctx->inputs[inp_i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        for (size_t ci = 0; ci < pasta_count(consumes); ci++) {
            const char *cname = pasta_get_string(pasta_array_get(consumes, ci));
            if (!cname) continue;

            const PastaValue *sec = pasta_map_get(inp, cname);
            if (!sec) continue;

            if (acc) {
                PastaValue *merged = alf_section_merge(acc, sec);
                pasta_free(acc);
                if (!merged) return NULL;
                acc = merged;
            } else {
                acc = alf_value_clone(sec);
                if (!acc) return NULL;
            }
        }
    }
    return acc;
}

/*
 * Gather and merge all sections named in the consumes array using "deep"
 * semantics: nested maps are merged recursively, non-map values use
 * last-write-wins.
 * Returns NULL (without error) if no matching sections are found.
 */
static PastaValue *gather_consumes_deep(AlfContext *ctx,
                                          const PastaValue *consumes) {
    PastaValue *acc = NULL;

    for (size_t inp_i = 0; inp_i < ctx->input_count; inp_i++) {
        PastaValue *inp = ctx->inputs[inp_i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        for (size_t ci = 0; ci < pasta_count(consumes); ci++) {
            const char *cname = pasta_get_string(pasta_array_get(consumes, ci));
            if (!cname) continue;

            const PastaValue *sec = pasta_map_get(inp, cname);
            if (!sec) continue;

            if (acc) {
                PastaValue *merged = alf_deep_merge(acc, sec);
                pasta_free(acc);
                if (!merged) return NULL;
                acc = merged;
            } else {
                acc = alf_value_clone(sec);
                if (!acc) return NULL;
            }
        }
    }
    return acc;
}

/*
 * Gather all sections named in the consumes array using "collect" semantics.
 * For map sections: keys that appear in multiple inputs are collected into
 * arrays (in input order).  Keys that appear only once stay as-is.
 * Array values from multiple inputs are concatenated.
 * Returns NULL (without error) if no matching sections are found.
 */
static PastaValue *gather_consumes_collect(AlfContext *ctx,
                                            const PastaValue *consumes) {
    PastaValue *acc = NULL;

    for (size_t inp_i = 0; inp_i < ctx->input_count; inp_i++) {
        PastaValue *inp = ctx->inputs[inp_i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        for (size_t ci = 0; ci < pasta_count(consumes); ci++) {
            const char *cname = pasta_get_string(pasta_array_get(consumes, ci));
            if (!cname) continue;

            const PastaValue *sec = pasta_map_get(inp, cname);
            if (!sec) continue;

            if (!acc) {
                acc = alf_value_clone(sec);
                if (!acc) return NULL;
                continue;
            }

            if (pasta_type(acc) != PASTA_MAP || pasta_type(sec) != PASTA_MAP) {
                /* Non-map section: replace (collect only applies to maps) */
                PastaValue *c = alf_value_clone(sec);
                pasta_free(acc);
                if (!c) return NULL;
                acc = c;
                continue;
            }

            /* Map-to-map collect merge */
            PastaValue *result = pasta_new_map();
            if (!result) { pasta_free(acc); return NULL; }

            /* Copy existing acc fields */
            for (size_t k = 0; k < pasta_count(acc); k++) {
                const char *key = pasta_map_key(acc, k);
                const PastaValue *aval = pasta_map_value(acc, k);
                const PastaValue *sval = pasta_map_get(sec, key);

                PastaValue *merged_val;
                if (!sval) {
                    /* Key only in acc — keep as-is */
                    merged_val = alf_value_clone(aval);
                } else if (pasta_type(aval) == PASTA_ARRAY &&
                           pasta_type(sval) == PASTA_ARRAY) {
                    /* Both arrays — concatenate */
                    merged_val = pasta_new_array();
                    if (merged_val) {
                        for (size_t i = 0; i < pasta_count(aval); i++) {
                            PastaValue *item = alf_value_clone(pasta_array_get(aval, i));
                            if (!item || pasta_push(merged_val, item)) {
                                pasta_free(item); pasta_free(merged_val);
                                merged_val = NULL; break;
                            }
                        }
                        if (merged_val) {
                            for (size_t i = 0; i < pasta_count(sval); i++) {
                                PastaValue *item = alf_value_clone(pasta_array_get(sval, i));
                                if (!item || pasta_push(merged_val, item)) {
                                    pasta_free(item); pasta_free(merged_val);
                                    merged_val = NULL; break;
                                }
                            }
                        }
                    }
                } else if (pasta_type(aval) == PASTA_ARRAY) {
                    /* acc is already an array (from prior collect), append new value */
                    merged_val = alf_value_clone(aval);
                    if (merged_val) {
                        PastaValue *item = alf_value_clone(sval);
                        if (!item || pasta_push(merged_val, item)) {
                            pasta_free(item); pasta_free(merged_val);
                            merged_val = NULL;
                        }
                    }
                } else {
                    /* Collision: wrap both into array */
                    merged_val = pasta_new_array();
                    if (merged_val) {
                        PastaValue *a = alf_value_clone(aval);
                        PastaValue *b = alf_value_clone(sval);
                        if (!a || !b ||
                            pasta_push(merged_val, a) || pasta_push(merged_val, b)) {
                            pasta_free(a); pasta_free(b);
                            pasta_free(merged_val);
                            merged_val = NULL;
                        }
                    }
                }

                if (!merged_val || pasta_set(result, key, merged_val)) {
                    pasta_free(merged_val); pasta_free(result); pasta_free(acc);
                    return NULL;
                }
            }

            /* Add keys only in sec (not in acc) */
            for (size_t k = 0; k < pasta_count(sec); k++) {
                const char *key = pasta_map_key(sec, k);
                if (pasta_map_get(acc, key)) continue; /* already handled */
                PastaValue *c = alf_value_clone(pasta_map_value(sec, k));
                if (!c || pasta_set(result, key, c)) {
                    pasta_free(c); pasta_free(result); pasta_free(acc);
                    return NULL;
                }
            }

            pasta_free(acc);
            acc = result;
        }
    }
    return acc;
}

/*
 * Build the output section for a conflate rule.
 * merged: the result of gathering and merging the consumed input sections.
 * rule:   the recipe section map (consumes + field descriptors).
 * Returns a fresh map containing only the recipe-declared fields.
 */
static PastaValue *build_conflated_section(const PastaValue *merged,
                                            const PastaValue *rule,
                                            const char *secname,
                                            AlfResult *result) {
    PastaValue *out = pasta_new_map();
    if (!out) {
        alf_set_error(result, ALF_ERR_ALLOC, 2, secname, "allocation failed");
        return NULL;
    }

    for (size_t fi = 0; fi < pasta_count(rule); fi++) {
        const char *field = pasta_map_key(rule, fi);
        if (strcmp(field, "consumes") == 0 || strcmp(field, "merge") == 0)
            continue;

        const PastaValue *fval = merged ? pasta_map_get(merged, field) : NULL;
        if (!fval) {
            /* Field not present in any consumed section: skip silently.
               Link strings ("@name") in inputs naturally pass through
               when present; absent fields are simply omitted. */
            continue;
        }
        PastaValue *c = alf_value_clone(fval);
        if (!c || pasta_set(out, field, c)) {
            pasta_free(c); pasta_free(out);
            alf_set_error(result, ALF_ERR_ALLOC, 2, secname, "allocation failed");
            return NULL;
        }
    }
    return out;
}

static PastaValue *do_conflate(AlfContext *ctx, AlfResult *result) {
    if (!ctx->recipe || pasta_type(ctx->recipe) != PASTA_MAP) {
        alf_set_error(result, ALF_ERR_BAD_RECIPE, 2, NULL,
                      "conflate requires a valid recipe");
        return NULL;
    }

    PastaValue *out = pasta_new_map();
    if (!out) {
        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
        return NULL;
    }

    for (size_t ri = 0; ri < pasta_count(ctx->recipe); ri++) {
        const char    *secname = pasta_map_key(ctx->recipe, ri);
        const PastaValue *rule = pasta_map_value(ctx->recipe, ri);

        if (pasta_type(rule) != PASTA_MAP) {
            alf_set_error(result, ALF_ERR_BAD_RECIPE, 2, secname,
                          "recipe section must be a map");
            pasta_free(out);
            return NULL;
        }

        const PastaValue *consumes = pasta_map_get(rule, "consumes");
        if (!consumes || pasta_type(consumes) != PASTA_ARRAY
                      || pasta_count(consumes) == 0) {
            alf_set_error(result, ALF_ERR_MISSING_CONSUMES, 2, secname,
                          "recipe section missing 'consumes' array");
            pasta_free(out);
            return NULL;
        }

        /* Determine merge strategy: "replace" (default), "collect", or "deep" */
        enum { MS_REPLACE, MS_COLLECT, MS_DEEP } strategy = MS_REPLACE;
        const PastaValue *merge_val = pasta_map_get(rule, "merge");
        if (merge_val) {
            const char *ms = pasta_get_string(merge_val);
            if (ms && strcmp(ms, "collect") == 0)
                strategy = MS_COLLECT;
            else if (ms && strcmp(ms, "deep") == 0)
                strategy = MS_DEEP;
            else if (ms && strcmp(ms, "replace") != 0) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "unknown merge strategy '%s' (expected 'replace', 'collect', or 'deep')", ms);
                alf_set_error(result, ALF_ERR_BAD_RECIPE, 2, secname, msg);
                pasta_free(out);
                return NULL;
            }
        }

        PastaValue *merged;
        switch (strategy) {
        case MS_COLLECT: merged = gather_consumes_collect(ctx, consumes); break;
        case MS_DEEP:    merged = gather_consumes_deep(ctx, consumes);    break;
        default:         merged = gather_consumes(ctx, consumes);         break;
        }
        /* merged may be NULL if no inputs matched — that's OK, fields will be absent */

        PastaValue *sec = build_conflated_section(merged, rule, secname, result);
        pasta_free(merged);
        if (!sec) { pasta_free(out); return NULL; }

        if (pasta_set(out, secname, sec)) {
            pasta_free(sec); pasta_free(out);
            alf_set_error(result, ALF_ERR_ALLOC, 2, secname, "allocation failed");
            return NULL;
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Gather (first-found merge)                                         */
/* ------------------------------------------------------------------ */

/*
 * Merge base and overlay maps with first-found semantics:
 * keys already in base are kept; only new keys from overlay are added.
 */
static PastaValue *alf_first_found_merge(const PastaValue *base,
                                           const PastaValue *overlay) {
    if (pasta_type(base) != PASTA_MAP || pasta_type(overlay) != PASTA_MAP)
        return alf_value_clone(base); /* first found wins */

    PastaValue *result = pasta_new_map();
    if (!result) return NULL;

    /* Copy all base fields (they win) */
    for (size_t i = 0; i < pasta_count(base); i++) {
        PastaValue *c = alf_value_clone(pasta_map_value(base, i));
        if (!c || pasta_set(result, pasta_map_key(base, i), c)) {
            pasta_free(c); pasta_free(result); return NULL;
        }
    }

    /* Add overlay fields only if not already in base */
    for (size_t i = 0; i < pasta_count(overlay); i++) {
        const char *key = pasta_map_key(overlay, i);
        if (pasta_map_get(base, key)) continue; /* first found wins */
        PastaValue *c = alf_value_clone(pasta_map_value(overlay, i));
        if (!c || pasta_set(result, key, c)) {
            pasta_free(c); pasta_free(result); return NULL;
        }
    }
    return result;
}

/*
 * Gather: merge all inputs like aggregate, but with first-found semantics.
 * The first occurrence of each field is kept; later occurrences are ignored.
 */
static PastaValue *do_gather(AlfContext *ctx, AlfResult *result) {
    PastaValue *out = pasta_new_map();
    if (!out) {
        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
        return NULL;
    }

    int first_found = (ctx->precedence == ALF_FIRST_FOUND);

    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        for (size_t j = 0; j < pasta_count(inp); j++) {
            const char       *key = pasta_map_key(inp, j);
            const PastaValue *val = pasta_map_value(inp, j);

            const PastaValue *existing = pasta_map_get(out, key);
            if (existing) {
                if (first_found) {
                    /* First-found: merge at field level, keeping existing fields */
                    PastaValue *merged = alf_first_found_merge(existing, val);
                    if (!merged) {
                        alf_set_error(result, ALF_ERR_ALLOC, 2, key, "allocation failed");
                        pasta_free(out); return NULL;
                    }
                    /* Rebuild output with merged section */
                    PastaValue *rebuilt = pasta_new_map();
                    if (!rebuilt) {
                        pasta_free(merged); pasta_free(out);
                        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                        return NULL;
                    }
                    for (size_t k = 0; k < pasta_count(out); k++) {
                        const char *okey = pasta_map_key(out, k);
                        PastaValue *oval;
                        if (strcmp(okey, key) == 0) {
                            oval = merged; merged = NULL;
                        } else {
                            oval = alf_value_clone(pasta_map_value(out, k));
                        }
                        if (!oval || pasta_set(rebuilt, okey, oval)) {
                            pasta_free(oval); pasta_free(merged);
                            pasta_free(rebuilt); pasta_free(out);
                            alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                            return NULL;
                        }
                    }
                    pasta_free(out);
                    out = rebuilt;
                } else {
                    /* Last-wins: same as aggregate */
                    PastaValue *merged = alf_section_merge(existing, val);
                    if (!merged) {
                        alf_set_error(result, ALF_ERR_ALLOC, 2, key, "allocation failed");
                        pasta_free(out); return NULL;
                    }
                    PastaValue *rebuilt = pasta_new_map();
                    if (!rebuilt) {
                        pasta_free(merged); pasta_free(out);
                        alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                        return NULL;
                    }
                    for (size_t k = 0; k < pasta_count(out); k++) {
                        const char *okey = pasta_map_key(out, k);
                        PastaValue *oval;
                        if (strcmp(okey, key) == 0) {
                            oval = merged; merged = NULL;
                        } else {
                            oval = alf_value_clone(pasta_map_value(out, k));
                        }
                        if (!oval || pasta_set(rebuilt, okey, oval)) {
                            pasta_free(oval); pasta_free(merged);
                            pasta_free(rebuilt); pasta_free(out);
                            alf_set_error(result, ALF_ERR_ALLOC, 2, NULL, "allocation failed");
                            return NULL;
                        }
                    }
                    pasta_free(out);
                    out = rebuilt;
                }
            } else {
                PastaValue *c = alf_value_clone(val);
                if (!c || pasta_set(out, key, c)) {
                    pasta_free(c); pasta_free(out);
                    alf_set_error(result, ALF_ERR_ALLOC, 2, key, "allocation failed");
                    return NULL;
                }
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Pass 2 entry point                                                 */
/* ------------------------------------------------------------------ */

PastaValue *alf_pass2_merge(AlfContext *ctx, AlfResult *result) {
    switch (ctx->op) {
    case ALF_AGGREGATE:
    case ALF_SCATTER:
        return do_aggregate(ctx, result);
    case ALF_GATHER:
        return do_gather(ctx, result);
    case ALF_CONFLATE:
        return do_conflate(ctx, result);
    }
    return do_aggregate(ctx, result);
}
