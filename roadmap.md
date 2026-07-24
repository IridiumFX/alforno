# Alforno Roadmap

Tracking planned features for the alforno processing library.

---

## Feature 1: `alf_process_to_string()`

**Status:** [x] Complete

Convenience API that combines `alf_process()` + `pasta_write()` into a single call, returning the output as a serialized Pasta/Basta string.

**API:**
```c
char *alf_process_to_string(AlfContext *ctx, int flags, AlfResult *result);
```

**Scope:**
- Calls `alf_process()` internally, then serializes with `pasta_write()`
- Caller owns the returned string (free with `free()`)
- Passes through `flags` (PASTA_PRETTY, PASTA_SECTIONS, etc.)
- Returns NULL on error with details in `AlfResult`

---

## Feature 2: `merge: "deep"`

**Status:** [x] Complete

Recursive map merging strategy for conflate recipes. Unlike `"replace"` (which overwrites at the field level), `"deep"` recursively merges nested maps.

**Recipe usage:**
```
@output {
    consumes: ["a", "b"],
    merge: "deep",
    settings: "nested config"
}
```

**Scope:**
- New merge strategy `"deep"` alongside `"replace"` and `"collect"`
- Nested maps are merged recursively; non-map values use last-write-wins
- Arrays are replaced (not concatenated — that's `"collect"`)
- Implemented as `gather_consumes_deep()` in `alforno_merge.c`
- Update spec and error table

---

## Feature 3: Conditional Sections (`when`)

**Status:** [x] Complete

Tag-based section gating. Sections with a `when` key are only included if the specified tag is active.

**API:**
```c
int alf_set_tags(AlfContext *ctx, const char **tags, size_t count, AlfResult *result);
```

**Input usage:**
```
@logging {
    when: "production",
    level: "warn",
    file: "/var/log/app.log"
}
```

**Scope:**
- New API `alf_set_tags()` to set active tags before processing
- `when` is a reserved key in input sections (string or array of strings)
- Section is included only if at least one `when` tag is in the active set
- Sections without `when` are always included
- `when` is stripped from output
- Evaluated as Pass 2, after Pass 1 (parameterize) and before Pass 3 (merge)
- Add `ALF_MAX_TAGS` constant, `when` to reserved keys in spec

---

## Feature 4: Validation Pass

**Status:** [x] Complete

Optional post-merge validation using recipe field descriptors. Recipe descriptor values gain semantic meaning for type checking and required-field enforcement.

**Recipe usage:**
```
@server {
    consumes: ["server"],
    merge: "replace",
    host: "required string",
    port: "required number",
    debug: "optional bool"
}
```

**Scope:**
- New file `alforno_validate.c` with `alf_pass5_validate()`
- Descriptor format: `"[required|optional] [string|number|bool|array|map]"`
- Missing required field → hard error (`ALF_ERR_VALIDATION`)
- Type mismatch → hard error
- Runs after Pass 4 (link), before returning output
- Optional fields that are absent are silently accepted
- Descriptors without recognized tokens are ignored (backward compatible)

---

## Feature 5: Include Directive (`@include`)

**Status:** [x] Complete

File-based input inclusion. An `@include` section lists files to load as additional inputs, with recursive include resolution.

**Input usage:**
```
@include [
    "base.pasta",
    "overrides.pasta"
]
```

**API:**
```c
int alf_set_base_dir(AlfContext *ctx, const char *dir, AlfResult *result);
int alf_add_input_file(AlfContext *ctx, const char *path, AlfResult *result);
```

**Scope:**
- `@include` is a reserved section name containing an array of file paths
- Paths are relative to the including file's directory (or base dir)
- Includes are resolved recursively before Pass 1
- Circular includes are detected and reported as hard error (`ALF_ERR_INCLUDE`)
- `@include` is consumed and does not appear in output
- New file `alforno_include.c`
- `alf_add_input_file()` reads and parses a file from disk
- `alf_set_base_dir()` sets the root for relative path resolution

---

---

## Feature 6: Scatter Verb

**Status:** [x] Complete

Split a pastlet into per-section files. Each named section is written to its own file in the output directory.

**CLI usage:**
```
alforno scatter input.pasta output_dir/
```

**API:**
```c
AlfContext *ctx = alf_create(ALF_SCATTER, &result);
alf_add_input(ctx, src, len, &result);
int count = alf_scatter_to_dir(ctx, "output_dir", ".pasta", &result);
```

**Scope:**
- New `ALF_SCATTER` operation mode
- `alf_scatter_to_dir()` processes through the full pipeline, then writes each section to `output_dir/section_name.ext`
- Returns file count on success, -1 on error (`ALF_ERR_IO`)
- Output directory must exist

---

## Feature 7: Gather Verb

**Status:** [x] Complete

Combine multiple files into a single pastlet with configurable precedence.

**CLI usage:**
```
alforno gather pastlet1.pasta pastlet2.pasta
alforno gather --first-found pastlet1.pasta pastlet2.pasta
```

**API:**
```c
AlfContext *ctx = alf_create(ALF_GATHER, &result);
alf_set_precedence(ctx, ALF_FIRST_FOUND, &result);
alf_add_input(ctx, src1, len1, &result);
alf_add_input(ctx, src2, len2, &result);
PastaValue *out = alf_process(ctx, &result);
```

**Scope:**
- New `ALF_GATHER` operation mode and `AlfPrecedence` enum
- `ALF_LAST_WINS` (default): later input overrides earlier fields
- `ALF_FIRST_FOUND`: first occurrence of each field is kept
- `alf_set_precedence()` sets the mode before processing
- Field-level precedence within sections (new fields from later inputs are still added)

---

## Feature 8: Prune / Filter (`@prune` / `@filter`)

**Status:** [x] Complete

Post-pipeline trimming of the output tree, producing a task-specific view of an
otherwise complete document. Two dual operations run as the final pass (Pass 6),
after Link and Validate:

- **prune** — drop the selected sections/paths and their subtrees (deny-list).
- **filter** — keep only the selected sections/paths (allow-list).

Unlike `conflate` (which selects during merge via a full recipe contract) or
`when` (declarative tag gating), prune/filter are lightweight, imperative
selectors applied to the finished tree — the missing "keep everything except X"
capability.

**Selectors** anchor on a section and drill down by `/` (a non-labelchar, so a
segment may contain `.`): `@server`, `@server/tls`, `@server/tls/cert`.

**API:**
```c
int alf_set_prune (AlfContext *ctx, const char **selectors, size_t count, AlfResult *result);
int alf_set_filter(AlfContext *ctx, const char **selectors, size_t count, AlfResult *result);
```

**Directive form** (consistent with `@include` / `@vars`, consumed from output):
```
@prune  [ "@secrets", "@server/debug" ]
@filter [ "@server", "@db" ]
```

**Scope:**
- New Pass 6 in `alf_process`, after validate; new file `alforno_select.c`.
- Selector engine generalizing the map-rebuild walk already in `alf_filter_when`.
- filter and prune are duals (`filter(S)` == prune the complement); one engine.
- Runs after Link, so a section linked into another keeps its embedded copy;
  only the top-level entry is removed/kept.
- Malformed selector (no leading `@`) → hard error; a selector matching nothing
  is a no-op.
- `@prune` / `@filter` reserved section names; new error code for bad selectors.
- Update spec (Pass 6, error table, reserved sections, API sketch) and add
  conformance-style tests.

**Primary surface:** the invocation API (`alf_set_prune` / `alf_set_filter`) is
primary — it fits the motivating case, task-specific trimming (the same
document viewed differently per consumer), and mirrors how `alf_set_tags` drives
`when`. The `@prune` / `@filter` directive is the secondary, data-embedded form
for "always trim this document the same way" (mirroring the `when` key living in
the data). Both are supported; their selector lists concatenate.

---

## Implementation Order

1. `alf_process_to_string()` — simplest, no dependencies
2. `merge: "deep"` — extends existing merge infrastructure
3. Conditional sections (`when`) — new API surface, new reserved key
4. Validation pass — new file, new error code, new pass
5. Include directive — new file, new APIs, file I/O
6. Scatter verb — new operation mode, file output
7. Gather verb — new operation mode, precedence switch
8. Prune / Filter — new final pass (Pass 6), selector engine, `@prune`/`@filter`
