#ifndef ALF_BACKEND_H
#define ALF_BACKEND_H

/*
 * Backend abstraction: Alforno can be built against either Pasta (text-only)
 * or Basta (text + binary blobs).  Define ALF_USE_BASTA at compile time to
 * select Basta; the default is Pasta.
 *
 * Blob support (ALF_HAS_BLOB) follows the capability the included header
 * declares (PASTA_HAS_BLOB / BASTA_HAS_BLOB), not ALF_USE_BASTA.  A build
 * may still define ALF_HAS_BLOB itself; that is respected.
 *
 * All source files include this header instead of pasta.h or basta.h
 * directly.  The Pasta-style names are used throughout the codebase;
 * when Basta is selected they are mapped to the corresponding Basta
 * identifiers.
 */

#ifdef ALF_USE_BASTA

#include "basta.h"

/* ---- Types ---- */
#ifndef ALF_BASTA_COMPAT_TYPES
#define ALF_BASTA_COMPAT_TYPES
typedef BastaValue   PastaValue;
typedef BastaResult  PastaResult;
#endif

/* ---- Error codes ---- */
#define PASTA_OK  BASTA_OK

/* ---- Value types ---- */
#define PASTA_NULL    BASTA_NULL
#define PASTA_BOOL    BASTA_BOOL
#define PASTA_NUMBER  BASTA_NUMBER
#define PASTA_STRING  BASTA_STRING
#define PASTA_ARRAY   BASTA_ARRAY
#define PASTA_MAP     BASTA_MAP
#define PASTA_LABEL   BASTA_LABEL

/* ---- Parsing / lifetime ---- */
#define pasta_parse           basta_parse
#define pasta_parse_cstr      basta_parse_cstr
#define pasta_free            basta_free

/* ---- Writing ---- */
/* basta_write takes an extra out_len parameter; wrap to match pasta_write(v, flags) */
#define pasta_write(v, flags) basta_write((v), (flags), NULL)
#define PASTA_PRETTY          BASTA_PRETTY
#define PASTA_COMPACT         BASTA_COMPACT
#define PASTA_SECTIONS        BASTA_SECTIONS
#define PASTA_SORTED          BASTA_SORTED

/* ---- Query ---- */
#define pasta_type            basta_type
#define pasta_is_null         basta_is_null
#define pasta_get_bool        basta_get_bool
#define pasta_get_number      basta_get_number
#define pasta_get_string      basta_get_string
#define pasta_get_string_len  basta_get_string_len
#define pasta_get_label       basta_get_label
#define pasta_get_label_len   basta_get_label_len
#define pasta_count           basta_count
#define pasta_array_get       basta_array_get
#define pasta_map_get         basta_map_get
#define pasta_map_key         basta_map_key
#define pasta_map_value       basta_map_value

/* ---- Building ---- */
#define pasta_new_null        basta_new_null
#define pasta_new_bool        basta_new_bool
#define pasta_new_number      basta_new_number
#define pasta_new_string      basta_new_string
#define pasta_new_string_len  basta_new_string_len
#define pasta_new_label       basta_new_label
#define pasta_new_label_len   basta_new_label_len
#define pasta_new_array       basta_new_array
#define pasta_new_map         basta_new_map
#define pasta_push            basta_push
#define pasta_set             basta_set

/* ---- Number format ---- */
#define PASTA_NUM_DEC           BASTA_NUM_DEC
#define PASTA_NUM_HEX           BASTA_NUM_HEX
#define PASTA_NUM_BIN           BASTA_NUM_BIN
#define pasta_new_number_fmt    basta_new_number_fmt
#define pasta_get_number_fmt    basta_get_number_fmt

/* ---- Blob support ---- */
#define PASTA_BLOB    BASTA_BLOB
#define pasta_new_blob   basta_new_blob
#define pasta_get_blob   basta_get_blob

/* basta.h states its own capability; a basta.h older than BASTA_HAS_BLOB
   is still Basta, and always had blobs. */
#if !defined(BASTA_HAS_BLOB) || BASTA_HAS_BLOB
#  ifndef ALF_HAS_BLOB
#    define ALF_HAS_BLOB  1
#  endif
#endif

#else /* Pasta (default) */

#include "pasta.h"

/* Ask the header that was actually included, not the build flag.  Real
   Pasta says PASTA_HAS_BLOB 0.  A pasta-shaped header backed by Basta (a
   consumer's compatibility shim) says 1 and supplies PASTA_BLOB,
   pasta_new_blob and pasta_get_blob.  Inferring "no ALF_USE_BASTA, so no
   blobs" got that second case wrong: the blob arms compiled out while
   blobs still arrived, and each one was merged into a null.  A pasta.h
   older than PASTA_HAS_BLOB is real Pasta, so absent means 0. */
#if defined(PASTA_HAS_BLOB) && PASTA_HAS_BLOB
#  ifndef ALF_HAS_BLOB
#    define ALF_HAS_BLOB  1
#  endif
#endif

#endif /* ALF_USE_BASTA */

#endif /* ALF_BACKEND_H */
