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
        if (strcmp(field, "consumes") == 0) continue;

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

        PastaValue *merged = gather_consumes(ctx, consumes);
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
/*  Pass 2 entry point                                                 */
/* ------------------------------------------------------------------ */

PastaValue *alf_pass2_merge(AlfContext *ctx, AlfResult *result) {
    return (ctx->op == ALF_AGGREGATE)
        ? do_aggregate(ctx, result)
        : do_conflate(ctx, result);
}
