# Alforno Guide
## Worked Examples and Case Studies

---

## Primer: How Alforno Thinks

You give alforno one or more **input pastlets** and it merges them into a single output. Two verbs, one rule:

```
alforno aggregate pastlet1.pasta pastlet2.pasta ... pastletN.pasta
alforno conflate  recipe.pasta   pastlet1.pasta ... pastletN.pasta
```

**Aggregate** is the open form: everything from every input is merged, last file wins on any conflict. No recipe needed.

**Conflate** is the controlled form: a recipe declares exactly which sections and fields appear in the output. Everything else is dropped. Last file still wins on field conflicts.

Both run the same three passes:

1. **Parameterize** — resolve `{variable}` tokens in all inputs against `@vars`
2. **Merge** — aggregate or conflate per the operation
3. **Link** — any string value `"@name"` matching a section name is replaced by embedding that section

---

## Case Study 1: Multi-File Layering (Dev / UAT / Prod)

The classic environment-override pattern. A base file defines stable defaults. Each environment file overrides only what differs. A vars file parameterises hostnames.

### Files

**base.pasta**
```
@app {
    port: 8080,
    workers: 4,
    log_level: "info",
    debug: false
}

@db {
    engine: "postgres",
    port: 5432,
    pool_min: 2,
    pool_max: 10
}
```

**dev.pasta**
```
@app {
    debug: true,
    log_level: "debug"
}

@db {
    host: "localhost",
    name: "myapp_dev"
}
```

**prod.pasta**
```
@app {
    workers: 16,
    log_level: "warn"
}

@db {
    host: "db.{region}.internal",
    name: "myapp_prod",
    pool_max: 50
}
```

**vars-prod.pasta**
```
@vars {
    region: "eu-west"
}
```

**recipe.pasta**
```
@app {
    consumes: ["app"],
    port: "required",
    workers: "required",
    log_level: "required",
    debug: "required"
}

@db {
    consumes: ["db"],
    engine: "required",
    host: "required",
    name: "required",
    port: "required",
    pool_min: "required",
    pool_max: "required"
}
```

### Invocations

Development:
```
alforno conflate recipe.pasta base.pasta dev.pasta
```

Production:
```
alforno conflate recipe.pasta base.pasta prod.pasta vars-prod.pasta
```

### How It Resolves (Production)

**Pass 1 — Parameterize**

`prod.pasta @db.host` → `"db.eu-west.internal"`.

**Pass 2 — Conflate**

For `@app`, inputs are merged in order: base.pasta then prod.pasta. Later file wins.

| Field | base | prod | Result |
|---|---|---|---|
| port | 8080 | — | 8080 |
| workers | 4 | 16 | 16 |
| log_level | "info" | "warn" | "warn" |
| debug | false | — | false |

For `@db`:

| Field | base | prod | Result |
|---|---|---|---|
| engine | "postgres" | — | "postgres" |
| host | — | "db.eu-west.internal" | "db.eu-west.internal" |
| name | — | "myapp_prod" | "myapp_prod" |
| port | 5432 | — | 5432 |
| pool_min | 2 | — | 2 |
| pool_max | 10 | 50 | 50 |

**Pass 3 — Link**

No `"@ref"` values in the output tree. Nothing to do.

**Output**
```
@app {
    port: 8080,
    workers: 16,
    log_level: "warn",
    debug: false
}

@db {
    engine: "postgres",
    host: "db.eu-west.internal",
    name: "myapp_prod",
    port: 5432,
    pool_min: 2,
    pool_max: 50
}
```

The recipe is the contract. Swapping `dev.pasta` for `prod.pasta` is the only change between environments. The recipe never changes.

---

## Case Study 2: Multi-File Cascading (Pipeline Composition)

A more complex scenario: sub-system configs are each assembled independently, then linked into a final deployment pastlet. Alforno runs multiple times; each run's output feeds the next.

### Goal

Produce a single deployment pastlet with:
- `@platform` — top-level settings
- `@network` — network config with an embedded `@tls` block
- `@services` — service registry with an embedded `@database` block

### Stage 1: Build TLS config

**tls-base.pasta**
```
@tls {
    cert: "/etc/ssl/certs/base.pem",
    key: "/etc/ssl/private/base.key",
    protocols: ["TLSv1.2", "TLSv1.3"],
    verify: true
}
```

**tls-prod.pasta**
```
@tls {
    cert: "/etc/ssl/{env}/cert.pem",
    key: "/etc/ssl/{env}/key.key"
}
```

**vars-prod.pasta**
```
@vars {
    region: "eu-west",
    env: "prod",
    version: "3.0.0"
}
```

```
alforno conflate tls-recipe.pasta tls-base.pasta tls-prod.pasta vars-prod.pasta \
  --out tls-resolved.pasta
```

**tls-resolved.pasta**
```
@tls {
    cert: "/etc/ssl/prod/cert.pem",
    key: "/etc/ssl/prod/key.key",
    protocols: ["TLSv1.2", "TLSv1.3"],
    verify: true
}
```

### Stage 2: Build database config

**db-base.pasta**
```
@database {
    engine: "postgres",
    port: 5432,
    pool_min: 2,
    pool_max: 10,
    timeout: 30
}
```

**db-prod.pasta**
```
@database {
    host: "db.{region}.internal",
    name: "platform_prod",
    pool_max: 100
}
```

```
alforno conflate db-recipe.pasta db-base.pasta db-prod.pasta vars-prod.pasta \
  --out db-resolved.pasta
```

**db-resolved.pasta**
```
@database {
    engine: "postgres",
    host: "db.eu-west.internal",
    name: "platform_prod",
    port: 5432,
    pool_min: 2,
    pool_max: 100,
    timeout: 30
}
```

### Stage 3: Assemble final deployment config

**platform-base.pasta**
```
@platform {
    name: "my-platform",
    version: "{version}",
    region: "{region}"
}

@network {
    ingress: "api.{region}.example.com",
    port: 443,
    tls: "@tls"
}

@services {
    replicas: 4,
    image: "platform:{version}",
    database: "@database"
}
```

**deploy-recipe.pasta**
```
@platform {
    consumes: ["platform"],
    name: "required",
    version: "required",
    region: "required"
}

@network {
    consumes: ["network"],
    ingress: "required",
    port: "required",
    tls: "required"
}

@services {
    consumes: ["services"],
    replicas: "required",
    image: "required",
    database: "required"
}
```

```
alforno conflate deploy-recipe.pasta \
  platform-base.pasta vars-prod.pasta \
  tls-resolved.pasta db-resolved.pasta \
  --out deploy.pasta
```

**Pass 1 — Parameterize**

`vars-prod.pasta @vars` is resolved across all inputs:
- `@platform.version` → `"3.0.0"`
- `@platform.region` → `"eu-west"`
- `@network.ingress` → `"api.eu-west.example.com"`
- `@services.image` → `"platform:3.0.0"`

**Pass 2 — Conflate**

`@platform`, `@network`, `@services` merged from their respective `consumes` sections. Field values are now concrete.

**Pass 3 — Link**

Alforno scans the output tree and finds two `"@ref"` values:
- `@network.tls` = `"@tls"` → section `@tls` found in `tls-resolved.pasta`
- `@services.database` = `"@database"` → section `@database` found in `db-resolved.pasta`

Dependency check: neither link target is itself a format section, so both resolve immediately in parallel. No cycles.

**deploy.pasta**
```
@platform {
    name: "my-platform",
    version: "3.0.0",
    region: "eu-west"
}

@network {
    ingress: "api.eu-west.example.com",
    port: 443,
    tls: {
        cert: "/etc/ssl/prod/cert.pem",
        key: "/etc/ssl/prod/key.key",
        protocols: ["TLSv1.2", "TLSv1.3"],
        verify: true
    }
}

@services {
    replicas: 4,
    image: "platform:3.0.0",
    database: {
        engine: "postgres",
        host: "db.eu-west.internal",
        name: "platform_prod",
        port: 5432,
        pool_min: 2,
        pool_max: 100,
        timeout: 30
    }
}
```

### What the cascade gives you

Each stage produces a valid, inspectable Pasta file. `tls-resolved.pasta` and `db-resolved.pasta` can be version-controlled, diffed, or consumed by other tools independently. The final recipe is environment-agnostic — to target dev, replace the resolved inputs with dev equivalents. The recipe never changes.

---

## Aggregate vs. Conflate: When to Use Each

| Situation | Use |
|---|---|
| Output shape is fully known, unknown fields should be dropped | `conflate` |
| Open union — all fields from all inputs should pass through | `aggregate` |
| Inputs are independently authored nodes in a build graph | `aggregate` |
| Final assembly step requiring a declared output contract | `conflate` |
| Merging plugin configs or extensible registries | `aggregate` |

A common pattern in a build graph: upstream nodes produce outputs via `aggregate` (lightweight, streamable, no contract), and a single downstream `conflate` step consumes them all to produce the final guaranteed-shape artifact.

---

## Processing Rules Summary

```
Pass 1 — Parameterize:
  Collect @vars from all inputs, merge last-write-wins
  Resolve {variable} in all string values
  Hard error on unresolved variable

Pass 2 — Merge:
  Process inputs in declaration order, last-write-wins per field
  aggregate: emit all sections and fields
  conflate:  emit only sections and fields declared in recipe

Pass 3 — Link:
  Build dependency DAG from "@ref" string values
  Hard error on cycle
  Resolve in topological order:
    look up @ref in Pass 2 output, then in inputs
    embed container at field position
    hard error on missing target
```

---

*Both case studies use only valid Pasta syntax at every stage. Alforno introduces no new file format — only a processing convention layered on top.*
