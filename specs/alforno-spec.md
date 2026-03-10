# Alforno Specification
## A Pasta Processor Library

---

## 1. Overview

Alforno (*al forno* — Italian for "baked") is a processing layer built on top of the Pasta format. It takes one or more **input pastlets** and emits a single **output pastlet** by merging named sections according to one of two operations: **aggregate** or **conflate**.

Alforno operates on named sections exclusively. Anonymous container files are not valid inputs.

---

## 2. Definitions

| Term | Meaning |
|---|---|
| **pastlet** | A valid Pasta file using named sections (`@name container`) |
| **recipe pastlet** | A format contract required by `conflate`. First positional argument |
| **input pastlet** | Carries data. One or more files, processed in declaration order |
| **output pastlet** | The result emitted by alforno |
| **section** | A named root container: `@name { ... }` or `@name [ ... ]` |
| **vars section** | The reserved section `@vars` used for parameterization |

---

## 3. Operations

Alforno is invoked with one of two verbs:

```
alforno aggregate pastlet1.pasta pastlet2.pasta ... pastletN.pasta
alforno conflate  recipe.pasta   pastlet1.pasta ... pastletN.pasta
```

For `conflate`, the recipe pastlet is the first positional argument. For `aggregate`, all positional arguments are input pastlets.

---

## 4. Processing Pipeline

Both operations execute the same three passes in strict order.

### Pass 1 — Parameterize

All string values across all input pastlets are scanned for `{variable}` tokens. Each token is resolved against the `@vars` section, which may appear in any input pastlet. If `@vars` appears in multiple files, the maps are merged with last-write-wins semantics before resolution begins.

- An unresolved variable is a **hard error**.
- `@vars` is consumed and does not appear in the output.

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

Links are resolved in topological order. If section `@A` contains a link to `@B`, and `@B` is itself a merged output section, `@B` is fully resolved before `@A`'s link is satisfied. A cycle in the link graph is a **hard error** reported before any processing begins.

---

## 5. Recipe Pastlet Structure

Required only for `conflate`. Each section in the recipe declares one output section. The section name becomes the output section name.

```
@output_section_name {
    consumes: ["section_name", ...],
    field:    "descriptor"
}
```

`consumes` lists which input section names are subject to this rule. It is required. All other keys are field descriptors — their presence in the recipe is the allowlist: any field not listed is dropped from output. Descriptor values are informational and carry no processing semantics. They serve as documentation of intent and as the explicit field allowlist.

`consumes` is the only reserved key in a recipe section.

---

## 6. Error Conditions

| Condition | Severity | Pass |
|---|---|---|
| Unresolved variable `{x}` | Hard error | 1 |
| `@vars` section is not a map | Hard error | 1 |
| `consumes` missing or empty in recipe section | Hard error | 2 |
| Dependency cycle in link graph | Hard error | pre-3 |
| Missing link target `@section_name` | Hard error | 3 |

---

## 7. Reserved Keys and Sections

The following key in a recipe section map is reserved:

- `consumes`

The following section name is reserved across all pastlets:

- `@vars`

---

## 8. Output

The output is a valid Pasta file using named sections. `@vars` is never written to output. For `aggregate`, section order follows declaration order across inputs. For `conflate`, section order follows recipe declaration order.

---

## 9. C API Sketch

```c
typedef struct AlfContext AlfContext;

/* ALF_AGGREGATE or ALF_CONFLATE */
AlfContext *alf_create(AlfOp op, AlfResult *result);

/* Set the recipe (conflate only — must be called before alf_add_input) */
int alf_set_recipe(AlfContext *ctx, const char *src, size_t len, AlfResult *result);

/* Add an input pastlet (call once per file, in declaration order) */
int alf_add_input(AlfContext *ctx, const char *src, size_t len, AlfResult *result);

/* Execute all three passes and return the output as a Pasta value tree */
PastaValue *alf_process(AlfContext *ctx, AlfResult *result);

/* Free the context */
void alf_free(AlfContext *ctx);
```

`AlfResult` mirrors `PastaResult` with pass number and section context added to error messages.

---

*Alforno sits entirely above the Pasta layer. It never modifies the Pasta grammar or parser. All inputs and outputs are valid Pasta files.*
