#include "alforno_internal.h"

/* ------------------------------------------------------------------ */
/*  Pass 4: Recipe descriptor validation                               */
/* ------------------------------------------------------------------ */

/*
 * Parse a descriptor string like "required string", "optional number",
 * "required bool", "required array", "required map", etc.
 * Returns 1 if the descriptor is recognized, 0 if not.
 */
static int parse_descriptor(const char *desc,
                              int *out_required,
                              int *out_type_check,
                              int *out_expected_type) {
    *out_required   = 0;
    *out_type_check = 0;
    *out_expected_type = -1;

    if (!desc) return 0;

    const char *p = desc;
    /* skip leading whitespace */
    while (*p == ' ') p++;

    /* check for required/optional */
    int found_req = 0;
    if (strncmp(p, "required", 8) == 0 && (p[8] == ' ' || p[8] == '\0')) {
        *out_required = 1;
        found_req = 1;
        p += 8;
    } else if (strncmp(p, "optional", 8) == 0 && (p[8] == ' ' || p[8] == '\0')) {
        *out_required = 0;
        found_req = 1;
        p += 8;
    }

    if (!found_req) return 0; /* no recognized keyword */

    /* skip whitespace */
    while (*p == ' ') p++;

    /* check for type */
    if (*p == '\0') {
        /* just "required" or "optional" with no type — only check presence */
        return 1;
    }

    *out_type_check = 1;
    if (strcmp(p, "string") == 0)
        *out_expected_type = PASTA_STRING;
    else if (strcmp(p, "number") == 0)
        *out_expected_type = PASTA_NUMBER;
    else if (strcmp(p, "bool") == 0)
        *out_expected_type = PASTA_BOOL;
    else if (strcmp(p, "array") == 0)
        *out_expected_type = PASTA_ARRAY;
    else if (strcmp(p, "map") == 0)
        *out_expected_type = PASTA_MAP;
    else {
        /* unknown type word — treat as unrecognized descriptor */
        *out_type_check = 0;
        return 0;
    }

    return 1;
}

static const char *type_name(int t) {
    switch (t) {
    case PASTA_NULL:   return "null";
    case PASTA_BOOL:   return "bool";
    case PASTA_NUMBER: return "number";
    case PASTA_STRING: return "string";
    case PASTA_ARRAY:  return "array";
    case PASTA_MAP:    return "map";
    case PASTA_LABEL:  return "label";
    default:           return "unknown";
    }
}

int alf_pass4_validate(PastaValue *output, AlfContext *ctx,
                         AlfResult *result) {
    /* Validation only applies to conflate with a recipe */
    if (ctx->op != ALF_CONFLATE || !ctx->recipe)
        return 0;

    for (size_t ri = 0; ri < pasta_count(ctx->recipe); ri++) {
        const char       *secname = pasta_map_key(ctx->recipe, ri);
        const PastaValue *rule    = pasta_map_value(ctx->recipe, ri);

        if (pasta_type(rule) != PASTA_MAP) continue;

        const PastaValue *out_sec = pasta_map_get(output, secname);

        for (size_t fi = 0; fi < pasta_count(rule); fi++) {
            const char       *field = pasta_map_key(rule, fi);
            const PastaValue *desc  = pasta_map_value(rule, fi);

            /* Skip reserved keys */
            if (strcmp(field, "consumes") == 0 || strcmp(field, "merge") == 0)
                continue;

            /* Descriptor must be a string */
            if (pasta_type(desc) != PASTA_STRING) continue;

            int is_required, has_type, expected_type;
            if (!parse_descriptor(pasta_get_string(desc),
                                   &is_required, &has_type, &expected_type))
                continue; /* unrecognized descriptor — skip (backward compat) */

            const PastaValue *fval = out_sec ? pasta_map_get(out_sec, field) : NULL;

            if (!fval || pasta_type(fval) == PASTA_NULL) {
                if (is_required) {
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                             "required field '%s' missing in section '%s'",
                             field, secname);
                    alf_set_error(result, ALF_ERR_VALIDATION, 4, secname, msg);
                    return -1;
                }
                continue;
            }

            if (has_type && (int)pasta_type(fval) != expected_type) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "field '%s' in section '%s': expected %s, got %s",
                         field, secname,
                         type_name(expected_type),
                         type_name((int)pasta_type(fval)));
                alf_set_error(result, ALF_ERR_VALIDATION, 4, secname, msg);
                return -1;
            }
        }
    }
    return 0;
}
