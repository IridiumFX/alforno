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
    if (pasta_type(v) != PASTA_MAP) {
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
    if (pasta_type(v) != PASTA_MAP) {
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

    /* Pass 1: parameterize */
    if (alf_pass1_parameterize(ctx, &local) != 0) {
        if (result) *result = local;
        return NULL;
    }

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

    if (result) { result->code = ALF_OK; result->pass = 3; }
    return linked;
}

ALF_API void alf_free(AlfContext *ctx) {
    if (!ctx) return;
    pasta_free(ctx->recipe);
    for (size_t i = 0; i < ctx->input_count; i++)
        pasta_free(ctx->inputs[i]);
    free(ctx);
}
