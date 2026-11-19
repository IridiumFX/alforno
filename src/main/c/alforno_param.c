#include "alforno_internal.h"
#include <inttypes.h>
#include <float.h>
#include <math.h>

#ifndef DBL_DECIMAL_DIG
  #define DBL_DECIMAL_DIG 17
#endif

/* A non-integral number interpolated into a string, spelled as the Pasta
   writer spells it: the shortest form that reads back as the same double,
   and the format's own Inf / -Inf / NaN.  Plain %g kept six significant
   digits, so {v} with v: 1.2345678 came out "1.23457". */
static void format_double(char *buf, size_t size, double n) {
    if (isnan(n)) { snprintf(buf, size, "NaN"); return; }
    if (isinf(n)) { snprintf(buf, size, n < 0 ? "-Inf" : "Inf"); return; }
    /* As the writer does: "-0" would read back as the integer 0. */
    if (n == 0 && signbit(n)) { snprintf(buf, size, "-0.0"); return; }
    /* Widen until strtod returns the same double; start at 1 so a
       subnormal finds its short form too (see pasta_writer.c). */
    for (int prec = 1; prec <= DBL_DECIMAL_DIG; prec++) {
        snprintf(buf, size, "%.*g", prec, n);
        if (strtod(buf, NULL) == n) break;
    }
}

/* ------------------------------------------------------------------ */
/*  Simple dynamic byte buffer                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} SBuf;

static int sbuf_init(SBuf *b) {
    b->cap  = 128;
    b->len  = 0;
    b->data = (char *)malloc(b->cap);
    if (!b->data) return -1;
    b->data[0] = '\0';   /* an input with nothing to append must still be a string */
    return 0;
}

static int sbuf_grow(SBuf *b, size_t need) {
    if (b->len + need < b->cap) return 0;
    size_t nc = b->cap * 2;
    while (nc < b->len + need) nc *= 2;
    char *t = (char *)realloc(b->data, nc);
    if (!t) return -1;
    b->data = t;
    b->cap  = nc;
    return 0;
}

static int sbuf_append(SBuf *b, const char *s, size_t n) {
    if (sbuf_grow(b, n + 1)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sbuf_putc(SBuf *b, char c) {
    return sbuf_append(b, &c, 1);
}

/* ------------------------------------------------------------------ */
/*  String interpolation                                               */
/* ------------------------------------------------------------------ */

/*
 * Substitute {variable} tokens in s[0..slen) against the vars map.
 * Returns a new malloc'd NUL-terminated string.
 * Sets result and returns NULL on unresolved variable.
 * { without a matching } is treated as a literal character.
 */
static char *substitute_str(const char *s, size_t slen,
                              const PastaValue *vars,
                              AlfResult *result, size_t *out_len) {
    SBuf buf;
    if (sbuf_init(&buf)) return NULL;

    size_t i = 0;
    while (i < slen) {
        if (s[i] != '{') {
            if (sbuf_putc(&buf, s[i++])) { free(buf.data); return NULL; }
            continue;
        }
        /* find closing } */
        size_t j = i + 1;
        while (j < slen && s[j] != '}') j++;
        if (j >= slen) {
            /* no closing brace — treat { as literal */
            if (sbuf_putc(&buf, s[i++])) { free(buf.data); return NULL; }
            continue;
        }

        size_t vlen = j - (i + 1);
        if (vlen == 0 || vlen >= 256) {
            /* empty or oversized name — treat as literal */
            if (sbuf_append(&buf, s + i, j - i + 1)) { free(buf.data); return NULL; }
            i = j + 1;
            continue;
        }

        char varname[256];
        memcpy(varname, s + i + 1, vlen);
        varname[vlen] = '\0';

        /* look up in vars map */
        const PastaValue *val = vars ? pasta_map_get(vars, varname) : NULL;
        if (!val) {
            char msg[320];
            snprintf(msg, sizeof(msg), "unresolved variable '{%s}'", varname);
            alf_set_error(result, ALF_ERR_UNRESOLVED_VAR, ALF_PASS_PARAM, NULL, msg);
            free(buf.data);
            return NULL;
        }

        /* stringify the var value */
        const char *sv = pasta_get_string(val);
        char numbuf[64];
        if (!sv) {
            if (pasta_type(val) == PASTA_NUMBER) {
                double n = pasta_get_number(val);
#ifdef ALF_HAS_EXACT_INTEGERS
                /* Exact integers print exactly, never through double. */
                int64_t  iv;
                uint64_t uv;
                if (pasta_get_number_kind(val) == PASTA_NUMBER_INT64
                    && pasta_get_int64(val, &iv) == 0)
                    snprintf(numbuf, sizeof(numbuf), "%" PRId64, iv);
                else if (pasta_get_number_kind(val) == PASTA_NUMBER_UINT64
                         && pasta_get_uint64(val, &uv) == 0)
                    snprintf(numbuf, sizeof(numbuf), "%" PRIu64, uv);
                else
#endif
                /* Range first: casting an out-of-range double is undefined.
                   Negative zero is no integer; format_double spells it. */
                if (n >= -1e15 && n <= 1e15 && n == (long long)n
                    && !(n == 0 && signbit(n)))
                    snprintf(numbuf, sizeof(numbuf), "%lld", (long long)n);
                else
                    format_double(numbuf, sizeof(numbuf), n);
                sv = numbuf;
            } else if (pasta_type(val) == PASTA_BOOL) {
                sv = pasta_get_bool(val) ? "true" : "false";
            } else {
                sv = "";
            }
        }
        if (sbuf_append(&buf, sv, strlen(sv))) { free(buf.data); return NULL; }
        i = j + 1;
    }
    *out_len = buf.len;
    return buf.data;
}

/* ------------------------------------------------------------------ */
/*  Recursive substitution (builds a new value tree)                  */
/* ------------------------------------------------------------------ */

static PastaValue *subst_value(const PastaValue *v, const PastaValue *vars,
                                AlfResult *result) {
    switch (pasta_type(v)) {
    case PASTA_NULL:   return pasta_new_null();
    case PASTA_BOOL:   return pasta_new_bool(pasta_get_bool(v));
    case PASTA_NUMBER: return alf_number_clone(v);
    case PASTA_STRING: {
        const char *s   = pasta_get_string(v);
        size_t      len = pasta_get_string_len(v);
        size_t nlen;
        char *ns = substitute_str(s, len, vars, result, &nlen);
        if (!ns) return NULL;
        PastaValue *r = pasta_new_string_len(ns, nlen);
        free(ns);
        return r;
    }
    case PASTA_LABEL:
        return pasta_new_label_len(pasta_get_label(v), pasta_get_label_len(v));
    case PASTA_ARRAY: {
        PastaValue *a = pasta_new_array();
        if (!a) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *item = subst_value(pasta_array_get(v, i), vars, result);
            if (!item) { pasta_free(a); return NULL; }
            if (pasta_push(a, item)) { pasta_free(item); pasta_free(a); return NULL; }
        }
        return a;
    }
    case PASTA_MAP: {
        PastaValue *m = pasta_new_map();
        if (!m) return NULL;
        for (size_t i = 0; i < pasta_count(v); i++) {
            PastaValue *val = subst_value(pasta_map_value(v, i), vars, result);
            if (!val) { pasta_free(m); return NULL; }
            if (pasta_set(m, pasta_map_key(v, i), val)) {
                pasta_free(val); pasta_free(m); return NULL;
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
    /* Only a type with no case above reaches here: one this build was
       compiled without (a blob, when ALF_HAS_BLOB is off).  Fail rather
       than substitute null, which would report success with the value
       gone.  Pass 1 visits every input value, so this is where such a
       mismatch surfaces for alf_process. */
    alf_set_error(result, ALF_ERR_UNSUPPORTED_TYPE, ALF_PASS_PARAM, NULL,
                  "value of a type this build does not support "
                  "(blob support compiled out?)");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Collect and merge @vars from all inputs                            */
/* ------------------------------------------------------------------ */

/*
 * Returns a fresh map of all variables (last-write-wins across inputs).
 * The @vars section is removed from each input that contains one.
 */
static PastaValue *collect_and_strip_vars(AlfContext *ctx, AlfResult *result) {
    PastaValue *vars = pasta_new_map();
    if (!vars) {
        alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_PARAM, NULL, "allocation failed");
        return NULL;
    }

    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;

        const PastaValue *v = pasta_map_get(inp, "vars");
        if (!v) continue;
        if (pasta_type(v) != PASTA_MAP) {
            alf_set_error(result, ALF_ERR_PARSE, ALF_PASS_PARAM, "vars",
                          "@vars section must be a map");
            pasta_free(vars);
            return NULL;
        }

        /* Merge into vars (last-write-wins: overlay existing keys) */
        PastaValue *merged = alf_map_merge(vars, v);
        if (!merged) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_PARAM, NULL, "allocation failed");
            pasta_free(vars);
            return NULL;
        }
        pasta_free(vars);
        vars = merged;

        /*
         * Strip @vars from the input: build a new input map without it.
         * We cannot remove an entry in-place via the public API, so rebuild.
         */
        PastaValue *stripped = pasta_new_map();
        if (!stripped) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_PARAM, NULL, "allocation failed");
            pasta_free(vars);
            return NULL;
        }
        for (size_t j = 0; j < pasta_count(inp); j++) {
            const char *key = pasta_map_key(inp, j);
            if (strcmp(key, "vars") == 0) continue;
            PastaValue *c = alf_value_clone(pasta_map_value(inp, j));
            if (!c || pasta_set(stripped, key, c)) {
                pasta_free(c); pasta_free(stripped); pasta_free(vars);
                alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_PARAM, NULL, "allocation failed");
                return NULL;
            }
        }
        pasta_free(ctx->inputs[i]);
        ctx->inputs[i] = stripped;
    }
    return vars;
}

/* ------------------------------------------------------------------ */
/*  Pass 1 entry point                                                 */
/* ------------------------------------------------------------------ */

int alf_pass1_parameterize(AlfContext *ctx, AlfResult *result) {
    PastaValue *vars = collect_and_strip_vars(ctx, result);
    if (!vars) return -1;

    /* Substitute in all inputs (even with empty vars, so that any
       {variable} tokens in the input are caught as unresolved errors) */
    for (size_t i = 0; i < ctx->input_count; i++) {
        PastaValue *subst = subst_value(ctx->inputs[i], vars, result);
        if (!subst) {
            /* Allocation failures inside subst_value return NULL without
               a code; do not let alf_process fail while reporting ALF_OK. */
            if (result && result->code == ALF_OK)
                alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_PARAM, NULL,
                              "allocation failed");
            pasta_free(vars);
            return -1;
        }
        pasta_free(ctx->inputs[i]);
        ctx->inputs[i] = subst;
    }

    pasta_free(vars);
    return 0;
}
