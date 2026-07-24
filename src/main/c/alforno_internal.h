#ifndef ALFORNO_INTERNAL_H
#define ALFORNO_INTERNAL_H

#include "alf_backend.h"
#include "alforno.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Portable strdup (strdup is not in C11, only POSIX)                 */
/* ------------------------------------------------------------------ */

static inline char *alf_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* ------------------------------------------------------------------ */
/*  Context                                                            */
/* ------------------------------------------------------------------ */

#define ALF_MAX_INPUTS    64
#define ALF_MAX_TAGS      32
#define ALF_MAX_SELECTORS 64

/* Pipeline pass numbers — single source of truth, mirroring the spec's
   Processing Pipeline.  Reported in AlfResult.pass; changing the pipeline is
   a change here, not scattered literals. */
typedef enum {
    ALF_PASS_SETUP    = 0,  /* include resolution / API setup */
    ALF_PASS_PARAM    = 1,  /* {variable} parameterization    */
    ALF_PASS_WHEN     = 2,  /* conditional (when) filtering    */
    ALF_PASS_MERGE    = 3,  /* aggregate / conflate            */
    ALF_PASS_LINK     = 4,  /* @section link resolution        */
    ALF_PASS_VALIDATE = 5,  /* recipe descriptor validation    */
    ALF_PASS_SELECT   = 6   /* prune / filter                  */
} AlfPass;

struct AlfContext {
    AlfOp        op;
    PastaValue  *recipe;                    /* NULL for aggregate                 */
    PastaValue  *inputs[ALF_MAX_INPUTS];    /* parsed input section maps          */
    size_t       input_count;
    char        *tags[ALF_MAX_TAGS];        /* active tags for conditional when   */
    size_t       tag_count;
    char        *base_dir;                /* base dir for include resolution    */
    AlfPrecedence precedence;            /* gather: LAST_WINS or FIRST_FOUND  */
    char        *prune_sel[ALF_MAX_SELECTORS];  /* Pass 6 prune selectors        */
    size_t       prune_count;
    char        *filter_sel[ALF_MAX_SELECTORS]; /* Pass 6 filter selectors       */
    size_t       filter_count;
};

/* ------------------------------------------------------------------ */
/*  Error helper                                                       */
/* ------------------------------------------------------------------ */

static inline void alf_set_error(AlfResult *r, AlfError code, int pass,
                                   const char *section, const char *msg) {
    if (!r) return;
    r->code = code;
    r->pass = pass;
    if (section)
        snprintf(r->section, sizeof(r->section), "%s", section);
    else
        r->section[0] = '\0';
    snprintf(r->message, sizeof(r->message), "%s", msg);
}

/* ------------------------------------------------------------------ */
/*  Internal pass prototypes                                           */
/* ------------------------------------------------------------------ */

/* Pass 0: resolve @include directives (reads files, adds inputs) */
int         alf_resolve_includes(AlfContext *ctx, AlfResult *result);

/* Pass 1: resolve {variable} tokens against @vars */
int         alf_pass1_parameterize(AlfContext *ctx, AlfResult *result);

/* Pass 2: strip sections whose "when" tag doesn't match active tags */
void        alf_filter_when(AlfContext *ctx);

/* Pass 3: aggregate or conflate input sections */
PastaValue *alf_pass3_merge(AlfContext *ctx, AlfResult *result);

/* Pass 4: replace "@section" link strings with embedded containers.
   Takes ownership of output; returns a new (or the same) value. */
PastaValue *alf_pass4_link(PastaValue *output, AlfContext *ctx,
                            AlfResult *result);

/* Pass 5: validate output against recipe field descriptors.
   Returns 0 on success, -1 on validation failure. */
int         alf_pass5_validate(PastaValue *output, AlfContext *ctx,
                                AlfResult *result);

/* Collect @prune / @filter directive selectors from the inputs into ctx and
   strip those sections (runs before merge, so they never reach output). */
int         alf_collect_select_directives(AlfContext *ctx, AlfResult *result);

/* Pass 6: apply filter (keep-list) then prune (drop-list) selectors.
   Takes ownership of output; returns a new (or the same) value, NULL on error. */
PastaValue *alf_pass6_select(PastaValue *output, AlfContext *ctx,
                              AlfResult *result);

/* ------------------------------------------------------------------ */
/*  Shared utility                                                     */
/* ------------------------------------------------------------------ */

/* Deep-copy a PastaValue tree using the public pasta API only. */
PastaValue *alf_value_clone(const PastaValue *v);

/* Merge two map sections: base fields, then overlay fields win on conflict.
   Both src values must be PASTA_MAP. Returns a fresh map. */
PastaValue *alf_map_merge(const PastaValue *base, const PastaValue *overlay);

/* Merge two section values. If both are maps, field-level last-write-wins.
   Otherwise the overlay wins entirely. */
PastaValue *alf_section_merge(const PastaValue *base, const PastaValue *overlay);

#endif /* ALFORNO_INTERNAL_H */
