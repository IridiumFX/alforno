# Changelog

Notable changes to alforno.

## Why this file calls out source files

Consumers integrate alforno two ways, and they fail differently:

- **Walking the source tree** (`now build`, or a glob in CMake) picks up a new
  file automatically.
- **An explicit source list** does not. A new translation unit then fails at
  *link*, with an undefined reference to something that looks like it should
  exist — and nothing else to go on.

The second is the harder failure to diagnose, because the symptom appears far
from the cause and the build system reports it as a missing symbol rather than
a missing file. So **every entry below states whether the set of source files
changed**, under `Files:`. An entry with no `Files:` line adds no translation
unit, and an explicit source list needs no edit for it.

Requested by the `now` team after `alforno_select.c` landed and their CMake
build — which uses an explicit list — failed at link on
`alf_pass6_select` while their `now build` succeeded.

Current source set (`src/main/c` unless noted):

    alf_backend.h  alforno.c  alforno_include.c  alforno_internal.h
    alforno_link.c  alforno_merge.c  alforno_param.c  alforno_select.c
    alforno_validate.c        src/main/h/alforno.h

---

## Unreleased

### `2d30eb2` — gitignore: ignore now's `target/`

`now` writes its build output to `target/`. The existing `*.dll` / `*.o`
patterns matched the artifacts but not `.now-manifest`, so the directory
showed as untracked on every status and `git add -A` would have committed
build output.

---

## 0.1.0

### `078be82` — `now.pasta`: fix stray comma, migrate to current schema

The descriptor was not valid Pasta at all: `f176e12` removed a `vendored:`
entry and left its separator behind, so the trailing comma had no member
after it. Every commit from `f176e12` to here carried it, and `now` could not
parse the descriptor for three months.

Also applies the May schema migration alforno never received (`link.output`
→ top-level `output`) and adds `defines: ["ALF_BUILDING"]`, without which a
shared Windows build resolves `ALF_API` to `dllimport` and fails to link.

### `347a2ba` — Pass 6: prune / filter

**Files: ADDED `src/main/c/alforno_select.c`.** An explicit source list must
add it, or the build fails at link with `undefined reference to
alf_pass6_select`.

New final pass trimming the output tree with selectors, after link and
validate:

- `filter` (keep-list) then `prune` (drop-list) — duals.
- Selectors anchor on a section and drill down with `/`: `@server`,
  `@server/tls/cert`. A selector matching nothing is a no-op; one without a
  leading `@` is `ALF_ERR_BAD_SELECTOR`.
- New API: `alf_set_prune()`, `alf_set_filter()`.
- New reserved sections `@prune` / `@filter`, collected before merge and
  stripped from output.
- New error code `ALF_ERR_BAD_SELECTOR`.

Also introduces the `AlfPass` enum in `alforno_internal.h` as the single
source of truth for pass numbers.

### `116abc7` — Renumber pipeline passes

`AlfResult.pass` values changed. The pipeline is now sequential with no
half-steps, where conditional filtering used to be "Pass 1.5":

| Pass | Stage |
|---|---|
| 0 | include resolution / setup |
| 1 | parameterize |
| 2 | conditional (`when`) filtering |
| 3 | merge |
| 4 | link |
| 5 | validate |
| 6 | prune / filter |

**Anything asserting on `AlfResult.pass` needs updating**: merge 2 → 3,
link 3 → 4, validate 4 → 5.

### `7163ab0`, `3519601` — CI: co-locate backend DLL on Windows

A FetchContent-built dependency's DLL can land outside `bin/`, so the test
could not load it. Also adds `#include <unistd.h>` for `rmdir`, which gcc 14+
requires (it makes the implicit declaration a hard error).

### `f176e12` — Remove vendored pasta reference

Basta provides the API via the compat shim in `alf_backend.h`.

### `605f5c7` — Hex/bin number format mappings in the backend layer

### `796241e` — Fix SEGV: portable `alf_strdup` for strict C11

`strdup` is POSIX, not C11.

### `0bee5ee` — Seven features

**Files: ADDED `src/main/c/alforno_include.c`, `src/main/c/alforno_validate.c`.**

`alf_process_to_string()`, `merge: "deep"`, conditional sections (`when`) with
`alf_set_tags()`, a validation pass, the `@include` directive with
`alf_set_base_dir()` / `alf_add_input_file()`, and the `scatter` and `gather`
verbs.

### `97cd454` — Basta backend support

**Files: ADDED `src/main/c/alf_backend.h`.**

Build against Pasta (default) or Basta (`-DALF_USE_BASTA`), selected by macro
remapping so the codebase is written once in `pasta_*` names.

### `5e31dc3` — First open source release

**Files: ADDED `src/main/c/alforno.c`, `alforno_internal.h`, `alforno_link.c`,
`alforno_merge.c`, `alforno_param.c`, and `src/main/h/alforno.h`.**

`aggregate` and `conflate` over named sections, with the parameterize → merge
→ link pipeline.
