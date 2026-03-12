#ifndef ALFORNO_H
#define ALFORNO_H

#ifdef ALF_USE_BASTA
#include "basta.h"
#ifndef ALF_BASTA_COMPAT_TYPES
#define ALF_BASTA_COMPAT_TYPES
typedef BastaValue   PastaValue;
typedef BastaResult  PastaResult;
#endif
#else
#include "pasta.h"
#endif
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
    ALF_CONFLATE,   /* controlled: recipe declares the output contract   */
    ALF_SCATTER,    /* split: each named section → its own file          */
    ALF_GATHER      /* combine: multiple files → one pastlet             */
} AlfOp;

/* ---- Gather precedence ---- */

typedef enum {
    ALF_LAST_WINS,   /* later input overrides earlier (default)  */
    ALF_FIRST_FOUND  /* first occurrence of a field is kept      */
} AlfPrecedence;

/* ---- Error codes ---- */

typedef enum {
    ALF_OK = 0,
    ALF_ERR_ALLOC,            /* memory allocation failure                */
    ALF_ERR_PARSE,            /* input or recipe failed to parse as pasta  */
    ALF_ERR_NOT_SECTIONS,     /* input or recipe is not a named-section file */
    ALF_ERR_MISSING_CONSUMES, /* conflate recipe section missing consumes  */
    ALF_ERR_BAD_RECIPE,       /* recipe section has wrong structure        */
    ALF_ERR_UNRESOLVED_VAR,   /* {variable} has no definition in @vars     */
    ALF_ERR_CYCLE,            /* cycle detected in section link graph       */
    ALF_ERR_VALIDATION,       /* recipe validation failed (required/type)   */
    ALF_ERR_INCLUDE,          /* include directive error (file/cycle)       */
    ALF_ERR_IO                /* file I/O error (scatter write failure)     */
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

/* Set gather precedence mode (gather only, default ALF_LAST_WINS).
   ALF_LAST_WINS: later file overrides. ALF_FIRST_FOUND: first value kept. */
ALF_API int alf_set_precedence(AlfContext *ctx, AlfPrecedence prec,
                                 AlfResult *result);

/* Set active tags for conditional section filtering.
   Sections with a "when" key are included only if at least one of their
   tags matches the active set.  Must be called before alf_process. */
ALF_API int alf_set_tags(AlfContext *ctx, const char **tags, size_t count,
                           AlfResult *result);

/* Set the base directory for resolving relative include paths.
   If not set, includes use the current working directory. */
ALF_API int alf_set_base_dir(AlfContext *ctx, const char *dir,
                               AlfResult *result);

/* Add an input pastlet by reading a file from disk.
   The file is parsed and added as if alf_add_input were called. */
ALF_API int alf_add_input_file(AlfContext *ctx, const char *path,
                                 AlfResult *result);

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

/* Execute all three passes and return the serialized output string.
   flags is passed to pasta_write (e.g. PASTA_PRETTY | PASTA_SECTIONS).
   Caller must free() the returned string. Returns NULL on error. */
ALF_API char *alf_process_to_string(AlfContext *ctx, int flags,
                                      AlfResult *result);

/* Scatter: process inputs, then write each output section to its own file
   in output_dir. ext is the file extension (e.g. ".pasta" or ".basta").
   Returns the number of files written, or -1 on error. */
ALF_API int alf_scatter_to_dir(AlfContext *ctx, const char *output_dir,
                                 const char *ext, AlfResult *result);

/* Free the context and all associated resources. */
ALF_API void alf_free(AlfContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALFORNO_H */
