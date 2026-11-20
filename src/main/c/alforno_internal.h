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

/* Inputs, tags and selectors are heap arrays that grow as needed.  They were
   fixed (64 inputs, 32 tags, 64 selectors), and tags and selectors past the
   limit were dropped without an error. */

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
    PastaValue  *recipe;                 /* NULL for aggregate                 */
    PastaValue **inputs;                 /* parsed input section maps          */
    size_t       input_count, input_cap;
    char       **tags;                   /* active tags for conditional when   */
    size_t       tag_count, tag_cap;
    char        *base_dir;               /* base dir for include resolution    */
    AlfPrecedence precedence;            /* gather: LAST_WINS or FIRST_FOUND   */
    char       **prune_sel;              /* Pass 6 prune selectors             */
    size_t       prune_count, prune_cap;
    char       **filter_sel;             /* Pass 6 filter selectors            */
    size_t       filter_count, filter_cap;
    char        *err_section;            /* AlfResult.section text it points at */
    size_t       err_section_cap;
};

/* Append v to ctx->inputs, growing it.  0, or -1 on allocation failure, in
   which case v is still the caller's. */
int alf_push_input(AlfContext *ctx, PastaValue *v);

/* Append a copy of s to a growable string array.  0, or -1 on allocation
   failure (the array is unchanged). */
int alf_push_str(char ***arr, size_t *count, size_t *cap, const char *s);

/* ------------------------------------------------------------------ */
/*  Error helper                                                       */
/* ------------------------------------------------------------------ */

/* `section` must be NULL or a string literal: the result points at it as is.
   For a name taken from the document, use alf_set_error_at. */
static inline void alf_set_error(AlfResult *r, AlfError code, int pass,
                                   const char *section, const char *msg) {
    if (!r) return;
    r->code    = code;
    r->pass    = pass;
    r->section = section ? section : "";
    /* message is fixed-size and truncates by design; the explicit precision
       says so, which is what -Wformat-truncation asks for. */
    snprintf(r->message, sizeof(r->message), "%.*s",
             (int)sizeof(r->message) - 1, msg);
}

/* As alf_set_error, for a section name the document owns (and may free
   before the result is read): the full name is copied into ctx, which owns
   it until the next error naming a section, or alf_free.  If that copy
   cannot be allocated, the error is still reported, with section "". */
void alf_set_error_at(AlfContext *ctx, AlfResult *r, AlfError code, int pass,
                      const char *section, const char *msg);

/* Mark r successful at `pass`; section becomes "" (message is left as is). */
static inline void alf_set_ok(AlfResult *r, int pass) {
    if (!r) return;
    r->code    = ALF_OK;
    r->pass    = pass;
    r->section = "";
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

/* Pass 4: replace label values naming a section with embedded containers
   (strings are never links; an unmatched label is kept).
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

/* Deep-copy a PastaValue tree using the public pasta API only.
   Returns NULL on allocation failure, and for a value of a type this build
   cannot copy (a blob with ALF_HAS_BLOB off) -- never a substitute null. */
PastaValue *alf_value_clone(const PastaValue *v);

/* fopen for every path alforno opens.  On Windows a valid UTF-8 path is
   opened through _wfopen, so names outside the ANSI code page work; any
   other path is opened narrow, exactly as plain fopen did.  Elsewhere it
   is plain fopen. */
FILE *alf_fopen(const char *path, const char *mode);

/* Copy a number keeping its base (0xFF stays 0xFF) and, when the backend
   stores exact integers, its exact value: an integer must not round-trip
   through double, which rounds anything past 2^53. */
PastaValue *alf_number_clone(const PastaValue *v);

/* Merge two map sections: base fields, then overlay fields win on conflict.
   Both src values must be PASTA_MAP. Returns a fresh map, or NULL on
   failure, including a value of a type this build cannot copy. */
PastaValue *alf_map_merge(const PastaValue *base, const PastaValue *overlay);

/* Merge two section values. If both are maps, field-level last-write-wins.
   Otherwise the overlay wins entirely. */
PastaValue *alf_section_merge(const PastaValue *base, const PastaValue *overlay);

#endif /* ALFORNO_INTERNAL_H */
