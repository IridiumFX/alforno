#ifndef ALFORNO_H
#define ALFORNO_H

#include "pasta.h"
#include <stddef.h>

/* DLL export/import (ALF_STATIC disables for static builds) */
#ifdef ALF_STATIC
  #define ALF_API
#elif defined(_WIN32)
  #ifdef ALF_BUILDING
    #define ALF_API __declspec(dllexport)
  #else
    #define ALF_API __declspec(dllimport)
  #endif
#else
  #define ALF_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Operation mode ---- */

typedef enum {
    ALF_AGGREGATE,  /* open union: all sections and fields pass through */
    ALF_CONFLATE    /* controlled: recipe declares the output contract   */
} AlfOp;

/* ---- Error codes ---- */

typedef enum {
    ALF_OK = 0,
    ALF_ERR_ALLOC,            /* memory allocation failure                */
    ALF_ERR_PARSE,            /* input or recipe failed to parse as pasta  */
    ALF_ERR_NOT_SECTIONS,     /* input or recipe is not a named-section file */
    ALF_ERR_MISSING_CONSUMES, /* conflate recipe section missing consumes  */
    ALF_ERR_BAD_RECIPE,       /* recipe section has wrong structure        */
    ALF_ERR_UNRESOLVED_VAR,   /* {variable} has no definition in @vars     */
    ALF_ERR_CYCLE             /* cycle detected in section link graph       */
} AlfError;

/* ---- Result info ---- */

typedef struct {
    AlfError code;
    int      pass;          /* 0=setup 1=parameterize 2=merge 3=link */
    char     section[64];   /* section name if relevant, else empty   */
    char     message[256];
} AlfResult;

/* ---- Opaque context ---- */

typedef struct AlfContext AlfContext;

/* ---- API ---- */

/* Create a processing context for the given operation. */
ALF_API AlfContext *alf_create(AlfOp op, AlfResult *result);

/* Set the recipe pastlet (conflate only).
   Must be called before alf_add_input. Ignored for aggregate.
   src must be a valid Pasta named-section file. */
ALF_API int alf_set_recipe(AlfContext *ctx, const char *src, size_t len,
                            AlfResult *result);

/* Add an input pastlet. Call once per file, in declaration order.
   src must be a valid Pasta named-section file. */
ALF_API int alf_add_input(AlfContext *ctx, const char *src, size_t len,
                           AlfResult *result);

/* Execute all three passes and return the output as a Pasta named-section
   value tree (PASTA_MAP keyed by section name). Caller must pasta_free().
   Returns NULL on error; result is populated with details. */
ALF_API PastaValue *alf_process(AlfContext *ctx, AlfResult *result);

/* Free the context and all associated resources. */
ALF_API void alf_free(AlfContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALFORNO_H */
