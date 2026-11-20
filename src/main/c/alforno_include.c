#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  File reading                                                       */
/* ------------------------------------------------------------------ */

static char *read_file_contents(const char *path, size_t *out_len) {
    FILE *fp = alf_fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Path helpers                                                       */
/* ------------------------------------------------------------------ */

static char *resolve_path(const char *base_dir, const char *rel) {
    if (!base_dir || !rel) return NULL;

    /* If rel is absolute, use as-is */
    if (rel[0] == '/' || rel[0] == '\\' ||
        (rel[0] != '\0' && rel[1] == ':'))
        return alf_strdup(rel);

    size_t blen = strlen(base_dir);
    size_t rlen = strlen(rel);
    char *path = (char *)malloc(blen + 1 + rlen + 1);
    if (!path) return NULL;

    memcpy(path, base_dir, blen);
    /* ensure separator */
    if (blen > 0 && base_dir[blen - 1] != '/' && base_dir[blen - 1] != '\\')
        path[blen++] = '/';
    memcpy(path + blen, rel, rlen);
    path[blen + rlen] = '\0';
    return path;
}

/* ------------------------------------------------------------------ */
/*  Include resolution                                                 */
/* ------------------------------------------------------------------ */

/* The files being included, innermost first: a chain through the recursion's
   own frames, so it needs no fixed array (it was 16 slots). */
typedef struct SeenPath {
    const char             *path;
    const struct SeenPath  *parent;
    size_t                  depth;   /* files in the chain, this one included */
} SeenPath;

/* Not a storage limit but a guard.  Paths are compared as spelled, so a
   cycle spelled two ways (x.pasta including ./x.pasta) is not seen as one;
   this stops it with an error rather than unbounded recursion.  Legitimate
   nesting is nowhere near it. */
#define ALF_MAX_INCLUDE_DEPTH 256

/* A copy of section map `inp` without its @include section, or NULL on
   allocation failure. */
static PastaValue *strip_include(const PastaValue *inp) {
    PastaValue *stripped = pasta_new_map();
    if (!stripped) return NULL;
    for (size_t j = 0; j < pasta_count(inp); j++) {
        const char *key = pasta_map_key(inp, j);
        if (strcmp(key, "include") == 0) continue;
        PastaValue *c = alf_value_clone(pasta_map_value(inp, j));
        if (!c || pasta_set(stripped, key, c)) {
            pasta_free(c); pasta_free(stripped);
            return NULL;
        }
    }
    return stripped;
}

/*
 * Recursively resolve @include sections in a parsed input.
 * `seen` is the chain of files currently being included, for cycle detection.
 */
static int resolve_includes_recursive(AlfContext *ctx,
                                        PastaValue *inp,
                                        const char *base_dir,
                                        const SeenPath *seen,
                                        AlfResult *result) {
    size_t seen_count = seen ? seen->depth : 0;
    if (!inp || pasta_type(inp) != PASTA_MAP) return 0;

    const PastaValue *inc = pasta_map_get(inp, "include");
    if (!inc) return 0;

    if (pasta_type(inc) != PASTA_ARRAY) {
        alf_set_error(result, ALF_ERR_INCLUDE, ALF_PASS_SETUP, "include",
                      "@include must be an array of file paths");
        return -1;
    }

    if (seen_count >= ALF_MAX_INCLUDE_DEPTH) {
        alf_set_error(result, ALF_ERR_INCLUDE, ALF_PASS_SETUP, "include",
                      "includes nested more than 256 deep "
                      "(a cycle through differently spelled paths?)");
        return -1;
    }

    /* Process each included file */
    for (size_t i = 0; i < pasta_count(inc); i++) {
        const PastaValue *item = pasta_array_get(inc, i);
        if (pasta_type(item) != PASTA_STRING) continue;
        const char *rel = pasta_get_string(item);

        char *path = resolve_path(base_dir, rel);
        if (!path) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SETUP, NULL, "allocation failed");
            return -1;
        }

        /* Cycle detection */
        for (const SeenPath *s = seen; s; s = s->parent) {
            if (strcmp(s->path, path) == 0) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "circular include detected: '%s'", rel);
                alf_set_error(result, ALF_ERR_INCLUDE, ALF_PASS_SETUP, "include", msg);
                free(path);
                return -1;
            }
        }

        /* Read and parse the file */
        size_t flen;
        char *src = read_file_contents(path, &flen);
        if (!src) {
            char msg[320];
            snprintf(msg, sizeof(msg), "cannot read include file '%s'", rel);
            alf_set_error(result, ALF_ERR_INCLUDE, ALF_PASS_SETUP, "include", msg);
            free(path);
            return -1;
        }

        PastaResult pr;
        PastaValue *parsed = pasta_parse(src, flen, &pr);
        free(src);
        if (!parsed || pr.code != PASTA_OK) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "parse error in include '%s' at %d:%d: %s",
                     rel, pr.line, pr.col, pr.message);
            alf_set_error(result, ALF_ERR_INCLUDE, ALF_PASS_SETUP, "include", msg);
            pasta_free(parsed);
            free(path);
            return -1;
        }
        if (pasta_type(parsed) != PASTA_MAP || !pr.sections) {
            alf_set_error(result, ALF_ERR_NOT_SECTIONS, ALF_PASS_SETUP, "include",
                          "included file must be a named-section file");
            pasta_free(parsed);
            free(path);
            return -1;
        }

        /* Recursively resolve includes in the parsed file */
        SeenPath here = { path, seen, seen_count + 1 };

        /* Derive base dir for nested includes */
        char *nested_base = alf_strdup(path);
        if (nested_base) {
            /* Find last separator */
            char *last_sep = NULL;
            for (char *p2 = nested_base; *p2; p2++) {
                if (*p2 == '/' || *p2 == '\\') last_sep = p2;
            }
            if (last_sep) *last_sep = '\0';
            else { free(nested_base); nested_base = alf_strdup("."); }
        }

        if (resolve_includes_recursive(ctx, parsed, nested_base ? nested_base : ".",
                                         &here, result) != 0) {
            free(nested_base);
            pasta_free(parsed);
            free(path);
            return -1;
        }
        free(nested_base);

        /* @include is consumed in an included file too.  Only top-level
           inputs used to lose it, so a nested file's @include reached the
           output as a section. */
        if (pasta_map_get(parsed, "include")) {
            PastaValue *stripped = strip_include(parsed);
            if (!stripped) {
                alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SETUP, NULL, "allocation failed");
                pasta_free(parsed);
                free(path);
                return -1;
            }
            pasta_free(parsed);
            parsed = stripped;
        }

        /* Add as input (before current inputs that come after includes) */
        if (alf_push_input(ctx, parsed)) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SETUP, NULL, "allocation failed");
            pasta_free(parsed);
            free(path);
            return -1;
        }
        free(path);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Entry points                                                       */
/* ------------------------------------------------------------------ */

int alf_resolve_includes(AlfContext *ctx, AlfResult *result) {
    const char *base = ctx->base_dir ? ctx->base_dir : ".";

    /*
     * Process includes from existing inputs. Since includes add new inputs
     * at the end, we only iterate over the original count to avoid
     * re-processing newly added files (they've already been recursively
     * resolved).
     */
    size_t orig_count = ctx->input_count;
    for (size_t i = 0; i < orig_count; i++) {
        PastaValue *inp = ctx->inputs[i];
        if (!inp || pasta_type(inp) != PASTA_MAP) continue;
        if (!pasta_map_get(inp, "include")) continue;

        if (resolve_includes_recursive(ctx, inp, base, NULL, result) != 0)
            return -1;

        PastaValue *stripped = strip_include(inp);
        if (!stripped) {
            alf_set_error(result, ALF_ERR_ALLOC, ALF_PASS_SETUP, NULL, "allocation failed");
            return -1;
        }
        pasta_free(ctx->inputs[i]);
        ctx->inputs[i] = stripped;
    }
    return 0;
}
