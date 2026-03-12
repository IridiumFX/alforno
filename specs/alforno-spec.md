# Alforno Specification
## A Pasta/Basta Processor Library

---

## 1. Overview

Alforno (*al forno* — Italian for "baked") is a processing layer built on top of the Pasta format. It takes one or more **input pastlets** and emits a single **output pastlet** by merging named sections according to one of two operations: **aggregate** or **conflate**.

Alforno can be built against either **Pasta** (text-only) or **Basta** (text + binary blobs). When built with Basta, both `.pasta` and `.basta` inputs can be mixed in the same processing context — Basta's parser handles pure-text Pasta files natively.

Alforno operates on named sections exclusively. Anonymous container files are not valid inputs.

---

## 2. Definitions

| Term | Meaning |
|---|---|
| **pastlet** | A valid Pasta (or Basta) file using named sections (`@name container`) |
| **recipe pastlet** | A format contract required by `conflate`. First positional argument |
| **input pastlet** | Carries data. One or more files, processed in declaration order |
| **output pastlet** | The result emitted by alforno |
| **section** | A named root container: `@name { ... }` or `@name [ ... ]` |
| **vars section** | The reserved section `@vars` used for parameterization |

---

## 3. Operations

Alforno is invoked with one of four verbs:

```
alforno aggregate pastlet1.pasta pastlet2.pasta ... pastletN.pasta
alforno conflate  recipe.pasta   pastlet1.pasta ... pastletN.pasta
alforno scatter   pastlet.pasta  output_dir/
alforno gather    [--first-found] pastlet1.pasta ... pastletN.pasta
```

- **aggregate** — open union: all sections and fields from all inputs pass through (last-write-wins).
- **conflate** — controlled merge: a recipe declares the output contract; unlisted sections and fields are dropped.
- **scatter** — split: each named section in the input is written to its own file in the output directory (e.g. `@server { ... }` → `output_dir/server.pasta`).
- **gather** — combine: multiple files are merged into a single pastlet. Supports two precedence modes:
  - **last-wins** (default) — later input overrides earlier fields.
  - **first-found** — first occurrence of each field is kept; later values are ignored.

For `conflate`, the recipe pastlet is the first positional argument. For `aggregate` and `gather`, all positional arguments are input pastlets. For `scatter`, the first argument is the input pastlet and the second is the output directory.

---

## 4. Processing Pipeline

Both operations execute the same pipeline in strict order.

### Pass 0 — Include Resolution

If any input pastlet contains an `@include` section (an array of file paths), alforno reads and parses each referenced file and adds it as an additional input. Includes are resolved recursively. Circular includes are a **hard error**. File paths are relative to the including file's directory (or the configured base directory). `@include` is consumed and does not appear in the output.

### Pass 1 — Parameterize

All string values across all input pastlets are scanned for `{variable}` tokens. Each token is resolved against the `@vars` section, which may appear in any input pastlet. If `@vars` appears in multiple files, the maps are merged with last-write-wins semantics before resolution begins.

- An unresolved variable is a **hard error**.
- `@vars` is consumed and does not appear in the output.

### Pass 1.5 — Conditional Filtering (`when`)

After parameterization, sections containing a `when` key are evaluated against the active tag set. A section is included only if at least one of its `when` tags matches an active tag. Sections without `when` are always included. The `when` key is stripped from output. If no tags are set, all `when`-guarded sections are excluded.

`when` accepts a string or an array of strings:

```
@logging {
    when: "production",
    level: "warn"
}

@monitoring {
    when: ["staging", "production"],
    enabled: true
}
```

### Pass 2 — Merge

Input pastlets are processed in declaration order. For each named section, all definitions across all inputs are merged with **last-write-wins** semantics. The two operations differ in what they do with the merged result:

- **aggregate** — output contains all sections and all fields from all inputs. No recipe required.
- **conflate** — output contains only the sections and fields declared in the recipe. Unknown sections and fields are silently dropped.

In both cases, input order is the sole precedence rule: a later file overrides an earlier one for any given field.

### Pass 3 — Link

After the merge pass, alforno scans all string values in the output tree. Any value matching `"@name"` where `@name` is a section present in the resolved output or in any input pastlet is replaced by embedding that section's container at the field position.

Link lookup order:
1. Resolved output sections (produced by Pass 2)
2. Any section present in any input pastlet

A missing link target is a **hard error**.

### Pass 4 — Validate (conflate only)

If the recipe contains field descriptors with `"required"` or `"optional"` keywords, alforno validates the output against these constraints. A missing required field or a type mismatch is a **hard error**. This pass runs only for `conflate` operations with a recipe.

Links are resolved in topological order. If section `@A` contains a link to `@B`, and `@B` is itself a merged output section, `@B` is fully resolved before `@A`'s link is satisfied. A cycle in the link graph is a **hard error** reported before any processing begins.

---

## 5. Blob Handling (Basta Mode)

When built against Basta (`ALF_USE_BASTA`), alforno accepts inputs containing binary blob values. Blobs are **opaque leaf values** — they are never interpreted, traversed, or modified by any pass:

- **Pass 1 (Parameterize)** — blobs are cloned as-is. No `{variable}` scanning occurs inside a blob.
- **Pass 2 (Merge)** — blobs participate in merge like any other scalar value. With `"replace"`, the last blob wins. With `"collect"`, blobs are collected into arrays alongside other values.
- **Pass 3 (Link)** — blobs are cloned as-is. No label-ref resolution occurs inside a blob.

A blob is structurally equivalent to a string or number for merging purposes: it is an atomic, indivisible value. Alforno never reads, writes, or allocates blob content beyond cloning.

When built against Pasta (the default), blob values cannot appear in the value tree and this section does not apply.

---

## 6. Recipe Pastlet Structure

Required only for `conflate`. Each section in the recipe declares one output section. The section name becomes the output section name.

```
@output_section_name {
    consumes: ["section_name", ...],
    merge:    "replace",
    field:    "descriptor"
}
```

`consumes` lists which input section names are subject to this rule. It is required. All other keys are field descriptors — their presence in the recipe is the allowlist: any field not listed is dropped from output. Descriptor values are informational and carry no processing semantics. They serve as documentation of intent and as the explicit field allowlist.

`merge` selects the merge strategy for same-key collisions within consumed sections. It is optional and defaults to `"replace"`.

| Strategy    | Behavior on same-key collision                                     |
|-------------|--------------------------------------------------------------------|
| `"replace"` | Last-write-wins (default, unchanged from prior behavior)           |
| `"collect"` | Collect all values into an array, in input order                   |
| `"deep"`    | Recursive map merge; non-map values use last-write-wins            |

With `"collect"`:

- A key that appears in multiple inputs produces an array of all seen values.
- A key that appears in only one input stays as-is (no wrapping).
- If colliding values are both arrays, they are concatenated rather than nested.

With `"deep"`:

- If both the base and overlay values for a key are maps, they are merged recursively.
- Non-map values (strings, numbers, arrays, etc.) use last-write-wins.
- Arrays are replaced entirely (use `"collect"` for concatenation).

An unknown merge strategy is a hard error.

### Field Descriptors (Validation)

Field descriptor values in the recipe can optionally carry validation semantics. A descriptor matching the format `"required <type>"` or `"optional <type>"` enables post-processing validation (Pass 4):

- `"required string"` — field must be present and be a string.
- `"optional number"` — field may be absent; if present, must be a number.
- Recognized types: `string`, `number`, `bool`, `array`, `map`.
- `"required"` or `"optional"` without a type checks only presence.
- Descriptors that do not match this format are ignored (backward compatible).

`consumes` and `merge` are the reserved keys in a recipe section.

---

## 7. Error Conditions

| Condition | Severity | Pass |
|---|---|---|
| Circular `@include` | Hard error | 0 |
| Missing include file | Hard error | 0 |
| Include depth exceeded | Hard error | 0 |
| Unresolved variable `{x}` | Hard error | 1 |
| `@vars` section is not a map | Hard error | 1 |
| `consumes` missing or empty in recipe section | Hard error | 2 |
| Unknown `merge` strategy | Hard error | 2 |
| Dependency cycle in link graph | Hard error | pre-3 |
| Missing link target `@section_name` | Hard error | 3 |
| Required field missing | Hard error | 4 |
| Field type mismatch | Hard error | 4 |
| Scatter file write failure | Hard error | output |

---

## 8. Reserved Keys and Sections

The following keys in a recipe section map are reserved:

- `consumes`
- `merge`

The following key in an input section map is reserved:

- `when`

The following section names are reserved across all pastlets:

- `@vars`
- `@include`

---

## 9. Output

The output is a valid Pasta file using named sections. `@vars` is never written to output.

- **aggregate** / **gather** — section order follows declaration order across inputs.
- **conflate** — section order follows recipe declaration order.
- **scatter** — each section is written to its own file named `<section_name>.<ext>` in the output directory. The output directory must exist.

---

## 10. C API Sketch

```c
typedef struct AlfContext AlfContext;

/* ALF_AGGREGATE, ALF_CONFLATE, ALF_SCATTER, or ALF_GATHER */
AlfContext *alf_create(AlfOp op, AlfResult *result);

/* Set gather precedence: ALF_LAST_WINS (default) or ALF_FIRST_FOUND */
int alf_set_precedence(AlfContext *ctx, AlfPrecedence prec, AlfResult *result);

/* Set active tags for conditional section filtering (when) */
int alf_set_tags(AlfContext *ctx, const char **tags, size_t count, AlfResult *result);

/* Set the base directory for resolving @include paths */
int alf_set_base_dir(AlfContext *ctx, const char *dir, AlfResult *result);

/* Add an input pastlet by reading a file from disk */
int alf_add_input_file(AlfContext *ctx, const char *path, AlfResult *result);

/* Set the recipe (conflate only — must be called before alf_add_input) */
int alf_set_recipe(AlfContext *ctx, const char *src, size_t len, AlfResult *result);

/* Add an input pastlet (call once per file, in declaration order) */
int alf_add_input(AlfContext *ctx, const char *src, size_t len, AlfResult *result);

/* Execute all passes and return the output as a Pasta value tree */
PastaValue *alf_process(AlfContext *ctx, AlfResult *result);

/* Execute all passes and return the output as a serialized string */
char *alf_process_to_string(AlfContext *ctx, int flags, AlfResult *result);

/* Scatter: process, then write each section to output_dir/name.ext */
int alf_scatter_to_dir(AlfContext *ctx, const char *output_dir,
                         const char *ext, AlfResult *result);

/* Free the context */
void alf_free(AlfContext *ctx);
```

`AlfResult` mirrors `PastaResult` with pass number and section context added to error messages.

---

*Alforno sits entirely above the Pasta/Basta layer. It never modifies the grammar or parser. All inputs and outputs are valid Pasta (or Basta) files.*
