#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  Shared utilities                                                   */
/* ------------------------------------------------------------------ */

PastaValue *alf_value_clone(const PastaValue *v) {
    if (!v) return pasta_new_null();
    switch (pasta_type(v)) {
    case PASTA_NULL:   return pasta_new_null();
    case PASTA_BOOL:   return pasta_new_bool(pasta_get_bool(v));
    case PASTA_NUMBER: return pasta_new_number(pasta_get_number(v));
    case PASTA_STRING:
        return pasta_new_string_len(pasta_get_string(v), pasta_get_string_len(v));
    case PASTA_LABEL:
        return pasta_new_label_len(pasta_get_label(v), pasta_get_label_len(v));
    case PASTA_ARRAY: {
        PastaValue *a = pasta_new_array();
        if (!a) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *c = alf_value_clone(pasta_array_get(v, i));
            if (!c) { pasta_free(a); return NULL; }
            if (pasta_push(a, c)) { pasta_free(c); pasta_free(a); return NULL; }
        }
        return a;
    }
    case PASTA_MAP: {
        PastaValue *m = pasta_new_map();
        if (!m) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *c = alf_value_clone(pasta_map_value(v, i));
            if (!c) { pasta_free(m); return NULL; }
            if (pasta_set(m, pasta_map_key(v, i), c)) {
                pasta_free(c); pasta_free(m); return NULL;
            }
        }
        return m;
    }
#ifdef ALF_HAS_BLOB
    case PASTA_BLOB: {
        size_t blen;
        const uint8_t *data = pasta_get_blob(v, &blen);
        return pasta_new_blob(data, blen);
    }
#endif
    }
    return pasta_new_null();
}

PastaValue *alf_map_merge(const PastaValue *base, const PastaValue *overlay) {
    PastaValue *result = pasta_new_map();
    if (!result) return NULL;

    /* Add base fields not overridden by overlay */
    for (size_t i = 0; i < pasta_count(base); i++) {
        const char *key = pasta_map_key(base, i);
        /* Check if overlay has this key */
        int overridden = 0;
        for (size_t j = 0; j < pasta_count(overlay); j++) {
            if (strcmp(pasta_map_key(overlay, j), key) == 0) { overridden = 1; break; }
        }
        if (!overridden) {
            PastaValue *c = alf_value_clone(pasta_map_value(base, i));
            if (!c || pasta_set(result, key, c)) {
                pasta_free(c); pasta_free(result); return NULL;
            }
        }
    }

    /* Add all overlay fields (they win) */
    for (size_t i = 0; i < pasta_count(overlay); i++) {
        PastaValue *c = alf_value_clone(pasta_map_value(overlay, i));
        if (!c || pasta_set(result, pasta_map_key(overlay, i), c)) {
            pasta_free(c); pasta_free(result); return NULL;
        }
    }

    return result;
}

PastaValue *alf_section_merge(const PastaValue *base, const PastaValue *overlay) {
    if (pasta_type(base) == PASTA_MAP && pasta_type(overlay) == PASTA_MAP)
        return alf_map_merge(base, overlay);
    return alf_value_clone(overlay);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Conditional section filtering ("when")                             */
/* ------------------------------------------------------------------ */

static int tag_active(AlfContext *ctx, const char *tag) {
    for (size_t i = 0; i < ctx->tag_count; i++)
        if (strcmp(ctx->tags[i], tag) == 0) return 1;
    return 0;
}

/*
 * Check if a section's "when" clause matches the active tags.
 * Returns 1 if section should be included, 0 if it should be dropped.
 * A section without "when" is always included.
 */
static int section_passes_when(AlfContext *ctx, const PastaValue *sec) {
    if (pasta_type(sec) != PASTA_MAP) return 1;
    const PastaValue *when = pasta_map_get(sec, "when");
    if (!when) return 1;
    if (ctx->tag_count == 0) return 0;

    if (pasta_type(when) == PASTA_STRING) {
        return tag_active(ctx, pasta_get_string(when));
    }
    if (pasta_type(when) == PASTA_ARRAY) {
        for (size_t i = 0; i < pasta_count(when); i++) {
            const PastaValue *item = pasta_array_get(when, i);
            if (pasta_type(item) == PASTA_STRING && tag_active(ctx, pasta_get_string(item)))
                return 1;
        }
        return 0;
    }
    return 1; /* non-string/array when: include (ignore malformed) */
}

void alf_filter_when(AlfContext *ctx) {
    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        int need_filter = 0;
        for (size_t j = 0; j < pasta_count(inp); j++) {
            const PastaValue *sec = pasta_map_value(inp, j);
            if (pasta_type(sec) == PASTA_MAP && pasta_map_get(sec, "when")) {
                need_filter = 1;
                break;
            }
        }
        if (!need_filter) continue;

        PastaValue *filtered = pasta_new_map();
        if (!filtered) continue;

        for (size_t j = 0; j < pasta_count(inp); j++) {
            const char *key = pasta_map_key(inp, j);
            const PastaValue *sec = pasta_map_value(inp, j);

            if (!section_passes_when(ctx, sec)) continue;

            /* Clone the section, stripping the "when" key */
            PastaValue *clone;
            if (pasta_type(sec) == PASTA_MAP && pasta_map_get(sec, "when")) {
                clone = pasta_new_map();
                if (!clone) { pasta_free(filtered); continue; }
                for (size_t k = 0; k < pasta_count(sec); k++) {
                    const char *fkey = pasta_map_key(sec, k);
                    if (strcmp(fkey, "when") == 0) continue;
                    PastaValue *fval = alf_value_clone(pasta_map_value(sec, k));
                    if (!fval || pasta_set(clone, fkey, fval)) {
                        pasta_free(fval); pasta_free(clone); clone = NULL; break;
                    }
                }
                if (!clone) { pasta_free(filtered); continue; }
            } else {
                clone = alf_value_clone(sec);
                if (!clone) { pasta_free(filtered); continue; }
            }

            if (pasta_set(filtered, key, clone)) {
                pasta_free(clone); pasta_free(filtered); filtered = NULL; break;
            }
        }

        if (filtered) {
            pasta_free(ctx->inputs[i]);
            ctx->inputs[i] = filtered;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ALF_API int alf_set_precedence(AlfContext *ctx, AlfPrecedence prec,
                                 AlfResult *result) {
    if (!ctx) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "null context");
        return -1;
    }
    ctx->precedence = prec;
    if (result) { result->code = ALF_OK; result->pass = 0; }
    return 0;
}

ALF_API int alf_set_base_dir(AlfContext *ctx, const char *dir,
                               AlfResult *result) {
    if (!ctx) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "null context");
        return -1;
    }
    free(ctx->base_dir);
    ctx->base_dir = dir ? strdup(dir) : NULL;
    if (dir && !ctx->base_dir) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "allocation failed");
        return -1;
    }
    if (result) { result->code = ALF_OK; result->pass = 0; }
    return 0;
}

ALF_API int alf_add_input_file(AlfContext *ctx, const char *path,
                                 AlfResult *result) {
    if (!ctx || !path) {
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, "null argument");
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char msg[320];
        snprintf(msg, sizeof(msg), "cannot open file '%s'", path);
        alf_set_error(result, ALF_ERR_INCLUDE, 0, NULL, msg);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "allocation failed"); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    int rc = alf_add_input(ctx, buf, rd, result);
    free(buf);
    return rc;
}

ALF_API int alf_set_tags(AlfContext *ctx, const char **tags, size_t count,
                           AlfResult *result) {
    if (!ctx) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "null context");
        return -1;
    }
    /* Free any previously set tags */
    for (size_t i = 0; i < ctx->tag_count; i++)
        free(ctx->tags[i]);
    ctx->tag_count = 0;

    size_t n = count < ALF_MAX_TAGS ? count : ALF_MAX_TAGS;
    for (size_t i = 0; i < n; i++) {
        ctx->tags[i] = strdup(tags[i]);
        if (!ctx->tags[i]) {
            alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "allocation failed");
            return -1;
        }
        ctx->tag_count++;
    }
    if (result) { result->code = ALF_OK; result->pass = 0; }
    return 0;
}

ALF_API AlfContext *alf_create(AlfOp op, AlfResult *result) {
    AlfContext *ctx = (AlfContext *)calloc(1, sizeof(AlfContext));
    if (!ctx) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "allocation failed");
        return NULL;
    }
    ctx->op          = op;
    ctx->recipe      = NULL;
    ctx->input_count = 0;
    if (result) {
        result->code       = ALF_OK;
        result->pass       = 0;
        result->section[0] = '\0';
        result->message[0] = '\0';
    }
    return ctx;
}

ALF_API int alf_set_recipe(AlfContext *ctx, const char *src, size_t len,
                             AlfResult *result) {
    if (!ctx || !src) {
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, "null argument");
        return -1;
    }
    if (ctx->op != ALF_CONFLATE) return 0; /* recipe ignored for aggregate */

    PastaResult pr;
    PastaValue *v = pasta_parse(src, len, &pr);
    if (!v || pr.code != PASTA_OK) {
        char msg[320];
        snprintf(msg, sizeof(msg), "recipe parse error at %d:%d: %s",
                 pr.line, pr.col, pr.message);
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, msg);
        pasta_free(v);
        return -1;
    }
    if (pasta_type(v) != PASTA_MAP || !pr.sections) {
        alf_set_error(result, ALF_ERR_NOT_SECTIONS, 0, NULL,
                      "recipe must be a named-section file");
        pasta_free(v);
        return -1;
    }
    pasta_free(ctx->recipe);
    ctx->recipe = v;
    if (result) { result->code = ALF_OK; result->pass = 0; }
    return 0;
}

ALF_API int alf_add_input(AlfContext *ctx, const char *src, size_t len,
                            AlfResult *result) {
    if (!ctx || !src) {
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, "null argument");
        return -1;
    }
    if (ctx->input_count >= ALF_MAX_INPUTS) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "too many inputs");
        return -1;
    }

    PastaResult pr;
    PastaValue *v = pasta_parse(src, len, &pr);
    if (!v || pr.code != PASTA_OK) {
        char msg[320];
        snprintf(msg, sizeof(msg), "input parse error at %d:%d: %s",
                 pr.line, pr.col, pr.message);
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, msg);
        pasta_free(v);
        return -1;
    }
    if (pasta_type(v) != PASTA_MAP || !pr.sections) {
        alf_set_error(result, ALF_ERR_NOT_SECTIONS, 0, NULL,
                      "input must be a named-section file");
        pasta_free(v);
        return -1;
    }
    ctx->inputs[ctx->input_count++] = v;
    if (result) { result->code = ALF_OK; result->pass = 0; }
    return 0;
}

ALF_API PastaValue *alf_process(AlfContext *ctx, AlfResult *result) {
    if (!ctx) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "null context");
        return NULL;
    }

    /* Clear result */
    if (result) {
        result->code       = ALF_OK;
        result->pass       = 0;
        result->section[0] = '\0';
        result->message[0] = '\0';
    }

    AlfResult local = {0};

    /* Pass 0: resolve @include directives */
    if (alf_resolve_includes(ctx, &local) != 0) {
        if (result) *result = local;
        return NULL;
    }

    /* Pass 1: parameterize */
    if (alf_pass1_parameterize(ctx, &local) != 0) {
        if (result) *result = local;
        return NULL;
    }

    /* Pass 1.5: conditional section filtering */
    alf_filter_when(ctx);

    /* Pass 2: merge */
    PastaValue *output = alf_pass2_merge(ctx, &local);
    if (!output) {
        if (result) *result = local;
        return NULL;
    }

    /* Pass 3: link */
    PastaValue *linked = alf_pass3_link(output, ctx, &local);
    if (!linked) {
        if (result) *result = local;
        return NULL;
    }

    /* Pass 4: validate (conflate only, if recipe has descriptors) */
    if (alf_pass4_validate(linked, ctx, &local) != 0) {
        if (result) *result = local;
        pasta_free(linked);
        return NULL;
    }

    if (result) { result->code = ALF_OK; result->pass = 4; }
    return linked;
}

ALF_API char *alf_process_to_string(AlfContext *ctx, int flags,
                                      AlfResult *result) {
    PastaValue *output = alf_process(ctx, result);
    if (!output) return NULL;

    char *str = pasta_write(output, flags);
    pasta_free(output);
    if (!str) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "serialization failed");
        return NULL;
    }
    return str;
}

ALF_API int alf_scatter_to_dir(AlfContext *ctx, const char *output_dir,
                                 const char *ext, AlfResult *result) {
    if (!ctx || !output_dir) {
        alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "null argument");
        return -1;
    }
    if (!ext) ext = ".pasta";

    PastaValue *output = alf_process(ctx, result);
    if (!output) return -1;

    if (pasta_type(output) != PASTA_MAP) {
        alf_set_error(result, ALF_ERR_PARSE, 0, NULL, "output is not a map");
        pasta_free(output);
        return -1;
    }

    int written = 0;
    for (size_t i = 0; i < pasta_count(output); i++) {
        const char *secname = pasta_map_key(output, i);
        const PastaValue *secval = pasta_map_value(output, i);

        /* Build a single-section named-section file */
        PastaValue *file_map = pasta_new_map();
        if (!file_map) {
            alf_set_error(result, ALF_ERR_ALLOC, 0, secname, "allocation failed");
            pasta_free(output);
            return -1;
        }
        PastaValue *clone = alf_value_clone(secval);
        if (!clone || pasta_set(file_map, secname, clone)) {
            pasta_free(clone); pasta_free(file_map); pasta_free(output);
            alf_set_error(result, ALF_ERR_ALLOC, 0, secname, "allocation failed");
            return -1;
        }

        char *serialized = pasta_write(file_map, PASTA_PRETTY | PASTA_SECTIONS);
        pasta_free(file_map);
        if (!serialized) {
            alf_set_error(result, ALF_ERR_ALLOC, 0, secname, "serialization failed");
            pasta_free(output);
            return -1;
        }

        /* Build file path: output_dir/secname.ext */
        size_t dlen = strlen(output_dir);
        size_t nlen = strlen(secname);
        size_t elen = strlen(ext);
        char *path = (char *)malloc(dlen + 1 + nlen + elen + 1);
        if (!path) {
            free(serialized); pasta_free(output);
            alf_set_error(result, ALF_ERR_ALLOC, 0, NULL, "allocation failed");
            return -1;
        }
        memcpy(path, output_dir, dlen);
        if (dlen > 0 && output_dir[dlen - 1] != '/' && output_dir[dlen - 1] != '\\')
            path[dlen++] = '/';
        memcpy(path + dlen, secname, nlen);
        memcpy(path + dlen + nlen, ext, elen);
        path[dlen + nlen + elen] = '\0';

        FILE *fp = fopen(path, "wb");
        if (!fp) {
            char msg[320];
            snprintf(msg, sizeof(msg), "cannot write file '%s'", path);
            alf_set_error(result, ALF_ERR_IO, 0, secname, msg);
            free(path); free(serialized); pasta_free(output);
            return -1;
        }
        fwrite(serialized, 1, strlen(serialized), fp);
        fclose(fp);
        free(path);
        free(serialized);
        written++;
    }

    pasta_free(output);
    if (result) { result->code = ALF_OK; }
    return written;
}

ALF_API void alf_free(AlfContext *ctx) {
    if (!ctx) return;
    pasta_free(ctx->recipe);
    for (size_t i = 0; i < ctx->input_count; i++)
        pasta_free(ctx->inputs[i]);
    for (size_t i = 0; i < ctx->tag_count; i++)
        free(ctx->tags[i]);
    free(ctx->base_dir);
    free(ctx);
}
