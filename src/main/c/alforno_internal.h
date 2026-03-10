#ifndef ALFORNO_INTERNAL_H
#define ALFORNO_INTERNAL_H

#include "alforno.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Context                                                            */
/* ------------------------------------------------------------------ */

#define ALF_MAX_INPUTS 64

struct AlfContext {
    AlfOp        op;
    PastaValue  *recipe;                    /* NULL for aggregate                 */
    PastaValue  *inputs[ALF_MAX_INPUTS];    /* parsed input section maps          */
    size_t       input_count;
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

/* Pass 1: resolve {variable} tokens against @vars */
int         alf_pass1_parameterize(AlfContext *ctx, AlfResult *result);

/* Pass 2: aggregate or conflate input sections */
PastaValue *alf_pass2_merge(AlfContext *ctx, AlfResult *result);

/* Pass 3: replace "@section" link strings with embedded containers.
   Takes ownership of output; returns a new (or the same) value. */
PastaValue *alf_pass3_link(PastaValue *output, AlfContext *ctx,
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
