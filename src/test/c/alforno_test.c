#include "alforno.h"
#ifdef ALF_USE_BASTA
#include "alf_backend.h"
#else
#include "pasta.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Minimal test harness (matching pasta_test.c style)                */
/* ------------------------------------------------------------------ */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int suite_run    = 0;
static int suite_passed = 0;

#define ASSERT(cond, msg)                                                     \
    do {                                                                      \
        tests_run++;                                                          \
        if (cond) { tests_passed++; }                                         \
        else {                                                                \
            tests_failed++;                                                   \
            printf("    FAIL: %s (line %d)\n", msg, __LINE__);                \
        }                                                                     \
    } while (0)

#define SUITE(name) \
    printf("\n--- %s ---\n", name); suite_run++

#define SUITE_OK() suite_passed++

/* ------------------------------------------------------------------ */
/*  File reading helper                                                */
/* ------------------------------------------------------------------ */

#ifndef ALF_TEST_RESOURCES
  #define ALF_TEST_RESOURCES "src/test/resources"
#endif

static char *read_file(const char *filename, size_t *out_len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ALF_TEST_RESOURCES, filename);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("    WARNING: could not open %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Build a context, add inline inputs, run alf_process, return output. */
static PastaValue *run_aggregate(const char **srcs, size_t n, AlfResult *result) {
    AlfContext *ctx = alf_create(ALF_AGGREGATE, result);
    if (!ctx) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (alf_add_input(ctx, srcs[i], strlen(srcs[i]), result)) {
            alf_free(ctx);
            return NULL;
        }
    }
    PastaValue *out = alf_process(ctx, result);
    alf_free(ctx);
    return out;
}

static PastaValue *run_conflate(const char *recipe, const char **srcs,
                                 size_t n, AlfResult *result) {
    AlfContext *ctx = alf_create(ALF_CONFLATE, result);
    if (!ctx) return NULL;
    if (alf_set_recipe(ctx, recipe, strlen(recipe), result)) {
        alf_free(ctx);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (alf_add_input(ctx, srcs[i], strlen(srcs[i]), result)) {
            alf_free(ctx);
            return NULL;
        }
    }
    PastaValue *out = alf_process(ctx, result);
    alf_free(ctx);
    return out;
}

/* ================================================================== */
/*  1. API safety                                                      */
/* ================================================================== */

static void test_api_safety(void) {
    SUITE("API safety");

    AlfResult r;
    AlfContext *ctx;

    /* NULL context */
    alf_free(NULL);
    ASSERT(1, "free(NULL) ok");

    ctx = alf_create(ALF_AGGREGATE, NULL);
    ASSERT(ctx != NULL, "create with NULL result ok");
    alf_free(ctx);

    /* alf_process with NULL */
    PastaValue *v = alf_process(NULL, &r);
    ASSERT(v == NULL, "process(NULL) returns NULL");
    ASSERT(r.code != ALF_OK, "process(NULL) sets error");

    /* recipe ignored for aggregate */
    ctx = alf_create(ALF_AGGREGATE, &r);
    int rc = alf_set_recipe(ctx, "@s {a:1}", 8, &r);
    ASSERT(rc == 0, "set_recipe ignored for aggregate");
    alf_free(ctx);

    SUITE_OK();
}

/* ================================================================== */
/*  2. Pass 1: variable substitution                                   */
/* ================================================================== */

static void test_parameterize_basic(void) {
    SUITE("Pass 1: basic substitution");

    AlfResult r;
    const char *srcs[] = {
        "@vars { region: \"eu-west\" }\n"
        "@app  { host: \"db.{region}.internal\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(r.code == ALF_OK, "no error");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app section");
    const char *host = pasta_get_string(pasta_map_get(app, "host"));
    ASSERT(host != NULL, "host exists");
    ASSERT(strcmp(host, "db.eu-west.internal") == 0, "host substituted");

    /* @vars is consumed and absent from output */
    ASSERT(pasta_map_get(out, "vars") == NULL, "vars not in output");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_multi_input(void) {
    SUITE("Pass 1: vars from multiple inputs");

    AlfResult r;
    const char *srcs[] = {
        "@vars { region: \"us-east\", version: \"1.0\" }",
        "@vars { region: \"eu-west\" }",    /* overrides region */
        "@app  { host: \"{region}-db\", ver: \"{version}\" }"
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app exists");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "host")), "eu-west-db") == 0,
           "region overridden by later vars");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "ver")), "1.0") == 0,
           "version from first vars");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_number_var(void) {
    SUITE("Pass 1: number variable");

    AlfResult r;
    const char *srcs[] = {
        "@vars { port: 8080 }\n"
        "@app  { bind: \"0.0.0.0:{port}\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const char *bind = pasta_get_string(
        pasta_map_get(pasta_map_get(out, "app"), "bind"));
    ASSERT(bind && strcmp(bind, "0.0.0.0:8080") == 0, "number var stringified");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_unresolved(void) {
    SUITE("Pass 1: unresolved variable error");

    AlfResult r;
    const char *srcs[] = {
        "@app { host: \"db.{missing}.internal\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out == NULL, "returns NULL");
    ASSERT(r.code == ALF_ERR_UNRESOLVED_VAR, "unresolved var error");
    ASSERT(r.pass == 1, "error in pass 1");
    printf("    error: %s\n", r.message);

    SUITE_OK();
}

static void test_parameterize_no_vars(void) {
    SUITE("Pass 1: no vars section — no substitution");

    AlfResult r;
    const char *srcs[] = {
        "@app { name: \"plain\", port: 8080 }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(r.code == ALF_OK, "no error");
    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "name")), "plain") == 0,
           "unchanged");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_at_literal(void) {
    SUITE("Pass 1: literal { without closing } is kept as-is");

    AlfResult r;
    const char *srcs[] = {
        "@vars { region: \"eu\" }\n"
        "@app  { template: \"prefix {region} suffix\", literal: \"no-brace\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "template")),
                  "prefix eu suffix") == 0, "substituted in middle");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  3. Pass 2: aggregate                                               */
/* ================================================================== */

static void test_aggregate_single(void) {
    SUITE("Pass 2 aggregate: single input");

    AlfResult r;
    const char *srcs[] = {
        "@app { port: 8080, debug: false }\n"
        "@db  { host: \"localhost\", port: 5432 }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(pasta_type(out) == PASTA_MAP, "root is map");
    ASSERT(pasta_count(out) == 2, "2 sections");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app");
    ASSERT(pasta_get_number(pasta_map_get(app, "port")) == 8080.0, "port=8080");
    ASSERT(pasta_get_bool(pasta_map_get(app, "debug")) == 0, "debug=false");

    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT(db != NULL, "db");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "host")), "localhost") == 0,
           "host=localhost");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_two_inputs_merge(void) {
    SUITE("Pass 2 aggregate: two inputs — map field merge");

    AlfResult r;
    const char *srcs[] = {
        "@app { port: 8080, workers: 4, log_level: \"info\" }",
        "@app { workers: 16, log_level: \"warn\" }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app");
    /* second input overrides */
    ASSERT(pasta_get_number(pasta_map_get(app, "workers")) == 16.0, "workers=16");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_level")), "warn") == 0,
           "log_level=warn");
    /* first input survives for non-overridden field */
    ASSERT(pasta_get_number(pasta_map_get(app, "port")) == 8080.0, "port=8080");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_array_section(void) {
    SUITE("Pass 2 aggregate: array section replaced by later input");

    AlfResult r;
    const char *srcs[] = {
        "@services [\"a\", \"b\"]",
        "@services [\"c\", \"d\", \"e\"]"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *svc = pasta_map_get(out, "services");
    ASSERT(svc != NULL && pasta_type(svc) == PASTA_ARRAY, "array");
    /* later array wins entirely */
    ASSERT(pasta_count(svc) == 3, "3 elements (later wins)");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(svc, 0)), "c") == 0, "[0]=c");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_multiple_sections(void) {
    SUITE("Pass 2 aggregate: multiple sections across inputs");

    AlfResult r;
    const char *srcs[] = {
        "@alpha { x: 1 }\n@beta { y: 2 }",
        "@beta  { y: 99, z: 3 }\n@gamma { w: 4 }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(pasta_count(out) == 3, "3 sections");

    ASSERT(pasta_get_number(pasta_map_get(pasta_map_get(out, "alpha"), "x")) == 1.0,
           "alpha.x=1");
    const PastaValue *beta = pasta_map_get(out, "beta");
    ASSERT(pasta_get_number(pasta_map_get(beta, "y")) == 99.0, "beta.y=99 (overridden)");
    ASSERT(pasta_get_number(pasta_map_get(beta, "z")) == 3.0,  "beta.z=3 (added)");
    ASSERT(pasta_get_number(pasta_map_get(pasta_map_get(out, "gamma"), "w")) == 4.0,
           "gamma.w=4");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  4. Pass 2: conflate                                                */
/* ================================================================== */

static void test_conflate_basic(void) {
    SUITE("Pass 2 conflate: basic field allowlist");

    AlfResult r;
    const char *recipe =
        "@app {\n"
        "    consumes: [\"app\"],\n"
        "    port: \"required\",\n"
        "    workers: \"required\"\n"
        "}";
    const char *srcs[] = {
        "@app { port: 8080, workers: 4, secret: \"should-drop\" }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 1, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app");
    ASSERT(pasta_count(app) == 2, "2 fields (secret dropped)");
    ASSERT(pasta_get_number(pasta_map_get(app, "port")) == 8080.0, "port");
    ASSERT(pasta_get_number(pasta_map_get(app, "workers")) == 4.0, "workers");
    ASSERT(pasta_map_get(app, "secret") == NULL, "secret dropped");
    pasta_free(out);

    SUITE_OK();
}

static void test_conflate_last_write_wins(void) {
    SUITE("Pass 2 conflate: last-write-wins across inputs");

    AlfResult r;
    const char *recipe =
        "@cfg {\n"
        "    consumes: [\"cfg\"],\n"
        "    host: \"required\",\n"
        "    port: \"required\",\n"
        "    debug: \"required\"\n"
        "}";
    const char *srcs[] = {
        "@cfg { host: \"localhost\", port: 5432, debug: false }",
        "@cfg { host: \"prod.db\",  debug: true }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *cfg = pasta_map_get(out, "cfg");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "host")), "prod.db") == 0,
           "host overridden");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "port")) == 5432.0, "port from base");
    ASSERT(pasta_get_bool(pasta_map_get(cfg, "debug")) == 1, "debug overridden");
    pasta_free(out);

    SUITE_OK();
}

static void test_conflate_consumes_scoping(void) {
    SUITE("Pass 2 conflate: consumes scopes which sections are merged");

    AlfResult r;
    const char *recipe =
        "@out {\n"
        "    consumes: [\"source_a\"],\n"
        "    x: \"required\"\n"
        "}";
    const char *srcs[] = {
        "@source_a { x: 1 }\n@source_b { x: 99 }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 1, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *sec = pasta_map_get(out, "out");
    ASSERT(sec != NULL, "out section");
    /* source_b is not in consumes, so its x=99 should not appear */
    ASSERT(pasta_get_number(pasta_map_get(sec, "x")) == 1.0, "x=1 (from source_a)");
    pasta_free(out);

    SUITE_OK();
}

static void test_conflate_missing_consumes_error(void) {
    SUITE("Pass 2 conflate: missing consumes is a hard error");

    AlfResult r;
    const char *recipe = "@app { port: \"required\" }"; /* no consumes */
    const char *srcs[] = { "@app { port: 8080 }" };
    PastaValue *out = run_conflate(recipe, srcs, 1, &r);
    ASSERT(out == NULL, "returns NULL");
    ASSERT(r.code == ALF_ERR_MISSING_CONSUMES, "missing consumes error");
    ASSERT(r.pass == 2, "error in pass 2");
    printf("    error: %s\n", r.message);

    SUITE_OK();
}

static void test_conflate_multi_recipe_sections(void) {
    SUITE("Pass 2 conflate: multiple recipe sections");

    AlfResult r;
    const char *recipe =
        "@app { consumes: [\"app\"], port: \"r\", debug: \"r\" }\n"
        "@db  { consumes: [\"db\"],  host: \"r\", port: \"r\" }";
    const char *srcs[] = {
        "@app { port: 8080, debug: true,  ignored: 1 }\n"
        "@db  { host: \"localhost\", port: 5432, ignored: 2 }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(pasta_count(out) == 2, "2 sections");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(pasta_count(app) == 2, "app has 2 fields");
    ASSERT(pasta_map_get(app, "ignored") == NULL, "app ignored dropped");

    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT(pasta_count(db) == 2, "db has 2 fields");
    ASSERT(pasta_map_get(db, "ignored") == NULL, "db ignored dropped");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  5. Pass 3: link resolution                                         */
/* ================================================================== */

static void test_link_basic(void) {
    SUITE("Pass 3: basic link embedding via label-ref");

    AlfResult r;
    const char *srcs[] = {
        "@tls  { cert: \"/etc/ssl/cert.pem\", verify: true }",
        "@net  { port: 443, tls: tls }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *net = pasta_map_get(out, "net");
    ASSERT(net != NULL, "net section");
    const PastaValue *tls_field = pasta_map_get(net, "tls");
    ASSERT(tls_field != NULL, "tls field present");
    ASSERT(pasta_type(tls_field) == PASTA_MAP, "tls field is now a map");
    ASSERT(pasta_get_bool(pasta_map_get(tls_field, "verify")) == 1, "verify=true");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls_field, "cert")),
                  "/etc/ssl/cert.pem") == 0, "cert");

    /* tls section still present in output separately */
    ASSERT(pasta_map_get(out, "tls") != NULL, "tls section also in output");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_from_input(void) {
    SUITE("Pass 3: link resolved from input (not output section)");

    AlfResult r;
    /* @tls only in first input, @net in second with label-ref to tls.
       In conflate mode, @tls won't be in recipe output, but the link
       should still resolve from inputs. */
    const char *recipe =
        "@net { consumes: [\"net\"], port: \"r\", tls: \"r\" }";
    const char *srcs[] = {
        "@tls { cert: \"cert.pem\", verify: true }",
        "@net { port: 443, tls: tls }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *net = pasta_map_get(out, "net");
    ASSERT(net != NULL, "net section");
    const PastaValue *tls_field = pasta_map_get(net, "tls");
    ASSERT(tls_field != NULL && pasta_type(tls_field) == PASTA_MAP, "tls embedded");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls_field, "cert")), "cert.pem") == 0,
           "cert.pem");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_non_matching_label(void) {
    SUITE("Pass 3: label-ref with no matching section kept as PASTA_LABEL");

    AlfResult r;
    /* "unknown" is a label-ref but no section named "unknown" exists */
    const char *srcs[] = {
        "@app { name: \"myapp\", fallback: unknown }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    const PastaValue *fb = pasta_map_get(app, "fallback");
    ASSERT(fb != NULL, "fallback present");
    ASSERT(pasta_type(fb) == PASTA_LABEL, "kept as PASTA_LABEL");
    ASSERT(strcmp(pasta_get_label(fb), "unknown") == 0, "label name preserved");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_chained(void) {
    SUITE("Pass 3: chained label-refs (A links B, B links C)");

    AlfResult r;
    const char *srcs[] = {
        "@creds { user: \"admin\", pass: \"secret\" }",
        "@db    { host: \"db.internal\", auth: creds }",
        "@app   { name: \"myapp\", db: db }"
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app");
    const PastaValue *db_field = pasta_map_get(app, "db");
    ASSERT(db_field != NULL && pasta_type(db_field) == PASTA_MAP, "db embedded in app");

    const PastaValue *auth_field = pasta_map_get(db_field, "auth");
    ASSERT(auth_field != NULL && pasta_type(auth_field) == PASTA_MAP,
           "creds embedded in db");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(auth_field, "user")), "admin") == 0,
           "user=admin");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_cycle_error(void) {
    SUITE("Pass 3: cycle detection");

    AlfResult r;
    /* @a label-refs @b, @b label-refs @a — cycle */
    const char *srcs[] = {
        "@a { ref: b }",
        "@b { ref: a }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out == NULL, "returns NULL");
    ASSERT(r.code == ALF_ERR_CYCLE, "cycle error");
    ASSERT(r.pass == 3, "error in pass 3");
    printf("    error: %s\n", r.message);

    SUITE_OK();
}

/* ================================================================== */
/*  6. Full pipeline: layering case study                              */
/* ================================================================== */

static void test_pipeline_layering_dev(void) {
    SUITE("Pipeline: layering — dev build");

    size_t len;
    char *base   = read_file("base.pasta",       &len);
    char *dev    = read_file("dev.pasta",         &len);
    char *recipe = read_file("recipe_app.pasta",  &len);
    if (!base || !dev || !recipe) {
        if (base) free(base);
        if (dev) free(dev);
        if (recipe) free(recipe);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { base, dev };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    free(base); free(dev); free(recipe);

    ASSERT(out != NULL, "process ok");
    ASSERT(r.code == ALF_OK, "no error");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app section");
    ASSERT(pasta_get_number(pasta_map_get(app, "port"))    == 8080.0, "port=8080 (base)");
    ASSERT(pasta_get_number(pasta_map_get(app, "workers")) == 4.0,    "workers=4 (base)");
    ASSERT(pasta_get_bool(pasta_map_get(app, "debug"))     == 1,      "debug=true (dev)");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_level")), "debug") == 0,
           "log_level=debug (dev)");

    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT(db != NULL, "db section");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "host")), "localhost") == 0,
           "host=localhost (dev)");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "name")), "myapp_dev") == 0,
           "name=myapp_dev (dev)");
    ASSERT(pasta_get_number(pasta_map_get(db, "port")) == 5432.0, "port=5432 (base)");

    pasta_free(out);
    SUITE_OK();
}

static void test_pipeline_layering_prod(void) {
    SUITE("Pipeline: layering — prod build with vars");

    size_t len;
    char *base   = read_file("base.pasta",       &len);
    char *prod   = read_file("prod.pasta",        &len);
    char *vars   = read_file("vars_prod.pasta",   &len);
    char *recipe = read_file("recipe_app.pasta",  &len);
    if (!base || !prod || !vars || !recipe) {
        free(base); free(prod); free(vars); free(recipe);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { base, prod, vars };
    PastaValue *out = run_conflate(recipe, srcs, 3, &r);
    free(base); free(prod); free(vars); free(recipe);

    ASSERT(out != NULL, "process ok");
    ASSERT(r.code == ALF_OK, "no error");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(pasta_get_number(pasta_map_get(app, "workers")) == 16.0,  "workers=16 (prod)");
    ASSERT(pasta_get_bool(pasta_map_get(app, "debug"))     == 0,     "debug=false (base)");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_level")), "warn") == 0,
           "log_level=warn (prod)");
    ASSERT(pasta_get_number(pasta_map_get(app, "port")) == 8080.0, "port=8080 (base)");

    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "host")),
                  "db.eu-west.internal") == 0, "host substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "name")), "myapp_prod") == 0,
           "name=myapp_prod");
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_max")) == 50.0,  "pool_max=50 (prod)");
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_min")) == 2.0,   "pool_min=2 (base)");

    pasta_free(out);
    SUITE_OK();
}

/* ================================================================== */
/*  7. Full pipeline: cascading case study (stage 1: TLS)             */
/* ================================================================== */

static void test_pipeline_cascade_tls(void) {
    SUITE("Pipeline: cascading — stage 1 TLS");

    size_t len;
    char *base   = read_file("tls_base.pasta",  &len);
    char *prod   = read_file("tls_prod.pasta",  &len);
    char *vars   = read_file("vars_prod.pasta", &len);
    char *recipe = read_file("recipe_tls.pasta",&len);
    if (!base || !prod || !vars || !recipe) {
        free(base); free(prod); free(vars); free(recipe);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { base, prod, vars };
    PastaValue *out = run_conflate(recipe, srcs, 3, &r);
    free(base); free(prod); free(vars); free(recipe);

    ASSERT(out != NULL, "process ok");
    ASSERT(r.code == ALF_OK, "no error");

    const PastaValue *tls = pasta_map_get(out, "tls");
    ASSERT(tls != NULL, "tls section");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls, "cert")),
                  "/etc/ssl/prod/cert.pem") == 0, "cert substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls, "key")),
                  "/etc/ssl/prod/key.key") == 0, "key substituted");
    ASSERT(pasta_get_bool(pasta_map_get(tls, "verify")) == 1, "verify=true (base)");

    const PastaValue *protos = pasta_map_get(tls, "protocols");
    ASSERT(protos != NULL && pasta_count(protos) == 2, "2 protocols (base)");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(protos, 0)), "TLSv1.2") == 0,
           "TLSv1.2");

    pasta_free(out);
    SUITE_OK();
}

/* ================================================================== */
/*  8. Full pipeline: cascading — stage 2 DB                          */
/* ================================================================== */

static void test_pipeline_cascade_db(void) {
    SUITE("Pipeline: cascading — stage 2 database");

    size_t len;
    char *base   = read_file("db_base.pasta",   &len);
    char *prod   = read_file("db_prod.pasta",   &len);
    char *vars   = read_file("vars_prod.pasta", &len);
    char *recipe = read_file("recipe_db.pasta", &len);
    if (!base || !prod || !vars || !recipe) {
        free(base); free(prod); free(vars); free(recipe);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { base, prod, vars };
    PastaValue *out = run_conflate(recipe, srcs, 3, &r);
    free(base); free(prod); free(vars); free(recipe);

    ASSERT(out != NULL, "process ok");

    const PastaValue *db = pasta_map_get(out, "database");
    ASSERT(db != NULL, "database section");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "engine")), "postgres") == 0,
           "engine=postgres");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "host")),
                  "db.eu-west.internal") == 0, "host substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "name")), "platform_prod") == 0,
           "name=platform_prod");
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_max")) == 100.0, "pool_max=100");
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_min")) == 2.0,   "pool_min=2 (base)");
    ASSERT(pasta_get_number(pasta_map_get(db, "timeout"))  == 30.0,  "timeout=30 (base)");

    pasta_free(out);
    SUITE_OK();
}

/* ================================================================== */
/*  9. Full pipeline: cascading — stage 3 deploy (with links)         */
/* ================================================================== */

static void test_pipeline_cascade_deploy(void) {
    SUITE("Pipeline: cascading — stage 3 deploy with link embedding");

    const char *tls_resolved =
        "@tls {\n"
        "    cert: \"/etc/ssl/prod/cert.pem\",\n"
        "    key: \"/etc/ssl/prod/key.key\",\n"
        "    protocols: [\"TLSv1.2\", \"TLSv1.3\"],\n"
        "    verify: true\n"
        "}";
    const char *db_resolved =
        "@database {\n"
        "    engine: \"postgres\",\n"
        "    host: \"db.eu-west.internal\",\n"
        "    name: \"platform_prod\",\n"
        "    port: 5432,\n"
        "    pool_min: 2,\n"
        "    pool_max: 100,\n"
        "    timeout: 30\n"
        "}";

    size_t len;
    char *platform = read_file("platform_base.pasta",  &len);
    char *vars     = read_file("vars_prod.pasta",       &len);
    char *recipe   = read_file("recipe_deploy.pasta",   &len);
    if (!platform || !vars || !recipe) {
        free(platform); free(vars); free(recipe);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { platform, vars, tls_resolved, db_resolved };
    PastaValue *out = run_conflate(recipe, srcs, 4, &r);
    free(platform); free(vars); free(recipe);

    ASSERT(out != NULL, "process ok");
    ASSERT(r.code == ALF_OK, "no error");
    ASSERT(pasta_count(out) == 3, "3 sections");

    /* @platform */
    const PastaValue *plat = pasta_map_get(out, "platform");
    ASSERT(plat != NULL, "platform");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(plat, "name")), "my-platform") == 0,
           "name");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(plat, "region")), "eu-west") == 0,
           "region substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(plat, "version")), "3.0.0") == 0,
           "version substituted");

    /* @network — should have tls embedded */
    const PastaValue *net = pasta_map_get(out, "network");
    ASSERT(net != NULL, "network");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(net, "ingress")),
                  "api.eu-west.example.com") == 0, "ingress substituted");
    ASSERT(pasta_get_number(pasta_map_get(net, "port")) == 443.0, "port=443");
    const PastaValue *tls_field = pasta_map_get(net, "tls");
    ASSERT(tls_field != NULL && pasta_type(tls_field) == PASTA_MAP,
           "tls embedded in network");
    ASSERT(pasta_get_bool(pasta_map_get(tls_field, "verify")) == 1, "tls.verify=true");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls_field, "cert")),
                  "/etc/ssl/prod/cert.pem") == 0, "tls.cert");

    /* @services — should have database embedded */
    const PastaValue *svc = pasta_map_get(out, "services");
    ASSERT(svc != NULL, "services");
    ASSERT(pasta_get_number(pasta_map_get(svc, "replicas")) == 4.0, "replicas=4");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(svc, "image")),
                  "platform:3.0.0") == 0, "image substituted");
    const PastaValue *db_field = pasta_map_get(svc, "database");
    ASSERT(db_field != NULL && pasta_type(db_field) == PASTA_MAP,
           "database embedded in services");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db_field, "engine")), "postgres") == 0,
           "db.engine=postgres");
    ASSERT(pasta_get_number(pasta_map_get(db_field, "pool_max")) == 100.0,
           "db.pool_max=100");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db_field, "host")),
                  "db.eu-west.internal") == 0, "db.host");

    pasta_free(out);
    SUITE_OK();
}

/* ================================================================== */
/*  10. Input validation                                               */
/* ================================================================== */

static void test_input_validation(void) {
    SUITE("Input validation");

    AlfResult r;
    AlfContext *ctx;

    /* Anonymous container rejected */
    ctx = alf_create(ALF_AGGREGATE, &r);
    int rc = alf_add_input(ctx, "{ a: 1 }", 8, &r);
    ASSERT(rc != 0, "anonymous container rejected");
    ASSERT(r.code == ALF_ERR_NOT_SECTIONS, "not-sections error");
    alf_free(ctx);

    /* Syntax error rejected */
    ctx = alf_create(ALF_AGGREGATE, &r);
    rc = alf_add_input(ctx, "@s { bad syntax %%% }", 21, &r);
    ASSERT(rc != 0, "syntax error rejected");
    ASSERT(r.code == ALF_ERR_PARSE, "parse error");
    alf_free(ctx);

    /* Anonymous container rejected as recipe */
    ctx = alf_create(ALF_CONFLATE, &r);
    rc = alf_set_recipe(ctx, "{ a: 1 }", 8, &r);
    ASSERT(rc != 0, "anonymous recipe rejected");
    ASSERT(r.code == ALF_ERR_NOT_SECTIONS, "recipe not-sections error");
    alf_free(ctx);

    SUITE_OK();
}

/* ================================================================== */
/*  11. Output is valid Pasta (write and re-parse)                    */
/* ================================================================== */

static void test_output_roundtrip(void) {
    SUITE("Output roundtrips through Pasta writer/parser");

    AlfResult r;
    const char *srcs[] = {
        "@vars { region: \"eu\" }\n"
        "@app  { host: \"api.{region}.com\", port: 8080 }\n"
        "@db   { host: \"db.{region}.com\",  port: 5432 }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "process ok");

    /* Write as sections */
    char *written = pasta_write(out, PASTA_PRETTY | PASTA_SECTIONS);
    ASSERT(written != NULL, "write ok");
    printf("    written:\n%s", written);

    /* Re-parse */
    PastaResult pr;
    PastaValue *reparsed = pasta_parse_cstr(written, &pr);
    ASSERT(reparsed != NULL && pr.code == PASTA_OK, "re-parse ok");

    const PastaValue *app = pasta_map_get(reparsed, "app");
    ASSERT(app != NULL, "app in reparsed");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "host")), "api.eu.com") == 0,
           "host correct after roundtrip");

    free(written);
    pasta_free(out);
    pasta_free(reparsed);
    SUITE_OK();
}

/* ================================================================== */
/*  12. Pass 1: additional substitution cases                         */
/* ================================================================== */

static void test_parameterize_bool_var(void) {
    SUITE("Pass 1: bool variable stringified");

    AlfResult r;
    const char *srcs[] = {
        "@vars { flag: true, mode: false }\n"
        "@app  { status: \"flag={flag} mode={mode}\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const char *status = pasta_get_string(
        pasta_map_get(pasta_map_get(out, "app"), "status"));
    ASSERT(status && strcmp(status, "flag=true mode=false") == 0,
           "bools stringified correctly");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_multiple_vars_in_string(void) {
    SUITE("Pass 1: multiple vars in one string value");

    AlfResult r;
    const char *srcs[] = {
        "@vars { proto: \"https\", host: \"api\", region: \"eu\", port: \"443\" }\n"
        "@app  { endpoint: \"{proto}://{host}.{region}.example.com:{port}/v1\" }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const char *ep = pasta_get_string(
        pasta_map_get(pasta_map_get(out, "app"), "endpoint"));
    ASSERT(ep && strcmp(ep, "https://api.eu.example.com:443/v1") == 0,
           "four vars in one string");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_in_array_elements(void) {
    SUITE("Pass 1: substitution inside array string elements");

    AlfResult r;
    const char *srcs[] = {
        "@vars { region: \"eu-west\", env: \"prod\" }\n"
        "@app  { replicas: [\"{region}-a\", \"{region}-b\", \"{env}-primary\"] }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const PastaValue *reps = pasta_map_get(pasta_map_get(out, "app"), "replicas");
    ASSERT(reps != NULL && pasta_count(reps) == 3, "3 replicas");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(reps, 0)), "eu-west-a") == 0,
           "[0] substituted");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(reps, 1)), "eu-west-b") == 0,
           "[1] substituted");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(reps, 2)), "prod-primary") == 0,
           "[2] substituted");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_in_nested_map(void) {
    SUITE("Pass 1: substitution inside nested map values");

    AlfResult r;
    const char *srcs[] = {
        "@vars { cluster: \"k8s-prod\", ns: \"services\" }\n"
        "@app  { meta: { cluster: \"{cluster}\", namespace: \"{ns}\", "
                        "image: \"registry.{cluster}/{ns}/api\" } }"
    };
    PastaValue *out = run_aggregate(srcs, 1, &r);
    ASSERT(out != NULL, "parsed");
    const PastaValue *meta = pasta_map_get(pasta_map_get(out, "app"), "meta");
    ASSERT(meta != NULL, "meta exists");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(meta, "cluster")), "k8s-prod") == 0,
           "nested cluster");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(meta, "namespace")), "services") == 0,
           "nested namespace");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(meta, "image")),
                  "registry.k8s-prod/services/api") == 0,
           "nested image with two vars");
    pasta_free(out);

    SUITE_OK();
}

static void test_parameterize_vars_only_input(void) {
    SUITE("Pass 1: vars-only input contributes nothing to output sections");

    AlfResult r;
    const char *srcs[] = {
        "@vars { env: \"staging\" }",              /* only vars, no sections */
        "@app  { name: \"svc-{env}\" }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(pasta_count(out) == 1, "exactly 1 section (vars consumed)");
    ASSERT(pasta_map_get(out, "vars") == NULL, "no vars section");
    const char *name = pasta_get_string(pasta_map_get(pasta_map_get(out, "app"), "name"));
    ASSERT(name && strcmp(name, "svc-staging") == 0, "name substituted");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  13. Pass 2 aggregate: additional merge cases                      */
/* ================================================================== */

static void test_aggregate_three_way_precedence(void) {
    SUITE("Pass 2 aggregate: three-way merge — each field traceable to its layer");

    AlfResult r;
    /* Each field is set in exactly one layer so we can trace it */
    const char *srcs[] = {
        /* layer 1: base */
        "@app { port: 8080, workers: 2, log_level: \"info\", timeout: 30 }",
        /* layer 2: platform — overrides workers */
        "@app { workers: 8, registry: \"reg.internal\" }",
        /* layer 3: env — overrides log_level, adds debug flag */
        "@app { log_level: \"warn\", debug: false }"
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app");
    /* layer 1 survivors */
    ASSERT(pasta_get_number(pasta_map_get(app, "port"))    == 8080.0, "port from L1");
    ASSERT(pasta_get_number(pasta_map_get(app, "timeout")) == 30.0,   "timeout from L1");
    /* layer 2 winners */
    ASSERT(pasta_get_number(pasta_map_get(app, "workers")) == 8.0, "workers from L2");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "registry")),
                  "reg.internal") == 0, "registry from L2");
    /* layer 3 winners */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_level")), "warn") == 0,
           "log_level from L3");
    ASSERT(pasta_get_bool(pasta_map_get(app, "debug")) == 0, "debug from L3");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_null_override(void) {
    SUITE("Pass 2 aggregate: later layer explicitly nulls a field");

    AlfResult r;
    const char *srcs[] = {
        "@app { cert_path: \"/etc/ssl/cert.pem\", key_path: \"/etc/ssl/key.pem\" }",
        "@app { cert_path: null }"   /* explicitly nulled in layer 2 */
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(pasta_is_null(pasta_map_get(app, "cert_path")), "cert_path nulled by L2");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "key_path")),
                  "/etc/ssl/key.pem") == 0, "key_path unchanged from L1");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_field_added_in_late_layer(void) {
    SUITE("Pass 2 aggregate: field first introduced in third layer");

    AlfResult r;
    const char *srcs[] = {
        "@svc { name: \"api\", port: 8080 }",
        "@svc { port: 9090 }",
        "@svc { gdpr_mode: true, data_residency: \"EU\" }"   /* new fields in L3 */
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *svc = pasta_map_get(out, "svc");
    ASSERT(pasta_count(svc) == 4, "4 fields total");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(svc, "name")), "api") == 0,
           "name from L1");
    ASSERT(pasta_get_number(pasta_map_get(svc, "port")) == 9090.0, "port from L2");
    ASSERT(pasta_get_bool(pasta_map_get(svc, "gdpr_mode"))      == 1, "gdpr from L3");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(svc, "data_residency")), "EU") == 0,
           "residency from L3");
    pasta_free(out);

    SUITE_OK();
}

static void test_aggregate_section_in_all_three_inputs(void) {
    SUITE("Pass 2 aggregate: same section in all three inputs — correct field resolution");

    AlfResult r;
    const char *srcs[] = {
        "@cfg { a: 1, b: 2, c: 3 }",
        "@cfg { b: 20, d: 4 }",
        "@cfg { c: 300, e: 5 }"
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *cfg = pasta_map_get(out, "cfg");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "a")) == 1.0,   "a: only in L1");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "b")) == 20.0,  "b: L2 wins over L1");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "c")) == 300.0, "c: L3 wins over L1");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "d")) == 4.0,   "d: only in L2");
    ASSERT(pasta_get_number(pasta_map_get(cfg, "e")) == 5.0,   "e: only in L3");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  14. Pass 2 conflate: additional cases                             */
/* ================================================================== */

static void test_conflate_fan_in(void) {
    SUITE("Pass 2 conflate: consumes from two different section names");

    AlfResult r;
    /* The recipe output section fans in fields from two different input sections */
    const char *recipe =
        "@merged {\n"
        "    consumes: [\"source_a\", \"source_b\"],\n"
        "    host: \"required\",\n"
        "    port: \"required\",\n"
        "    name: \"required\"\n"
        "}";
    const char *srcs[] = {
        "@source_a { host: \"db.internal\", port: 5432 }",
        "@source_b { name: \"mydb\", port: 9999 }"  /* port in both: source_b wins */
    };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *merged = pasta_map_get(out, "merged");
    ASSERT(merged != NULL, "merged section");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(merged, "host")), "db.internal") == 0,
           "host from source_a");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(merged, "name")), "mydb") == 0,
           "name from source_b");
    /* source_b appears after source_a so its port wins */
    ASSERT(pasta_get_number(pasta_map_get(merged, "port")) == 9999.0,
           "port: source_b wins (later)");
    pasta_free(out);

    SUITE_OK();
}

static void test_conflate_no_matching_inputs(void) {
    SUITE("Pass 2 conflate: recipe section with no matching input produces empty section");

    AlfResult r;
    const char *recipe =
        "@out { consumes: [\"ghost\"], x: \"required\", y: \"required\" }";
    const char *srcs[] = {
        "@other { x: 1, y: 2 }"   /* "ghost" section never appears */
    };
    PastaValue *out = run_conflate(recipe, srcs, 1, &r);
    ASSERT(out != NULL, "process ok — no error");
    ASSERT(r.code == ALF_OK, "no error");

    const PastaValue *sec = pasta_map_get(out, "out");
    ASSERT(sec != NULL, "out section present");
    ASSERT(pasta_count(sec) == 0, "empty: no fields resolved");
    pasta_free(out);

    SUITE_OK();
}

static void test_conflate_label_ref_passes_through(void) {
    SUITE("Pass 2 conflate: label-ref field declared in recipe passes through to pass 3");

    AlfResult r;
    const char *recipe =
        "@net { consumes: [\"net\"], port: \"required\", security: \"required\" }";
    const char *srcs[] = {
        "@tls { cert: \"cert.pem\", verify: true }",
        "@net { port: 443, security: tls }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    ASSERT(out != NULL, "process ok");

    const PastaValue *net = pasta_map_get(out, "net");
    ASSERT(net != NULL, "net section");
    /* Pass 3 should have resolved the label-ref */
    const PastaValue *sec_field = pasta_map_get(net, "security");
    ASSERT(sec_field != NULL, "security field present");
    ASSERT(pasta_type(sec_field) == PASTA_MAP, "label-ref resolved to map by pass 3");
    ASSERT(pasta_get_bool(pasta_map_get(sec_field, "verify")) == 1, "verify=true");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  15. Pass 3: additional link cases                                 */
/* ================================================================== */

static void test_link_in_array_element(void) {
    SUITE("Pass 3: label-ref as array element");

    AlfResult r;
    const char *srcs[] = {
        "@tls   { cert: \"tls.pem\",   verify: true }",
        "@mtls  { cert: \"mtls.pem\",  verify: true, client_auth: true }",
        "@net   { listeners: [tls, mtls, \"fallback\"] }"
    };
    PastaValue *out = run_aggregate(srcs, 3, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *listeners = pasta_map_get(pasta_map_get(out, "net"), "listeners");
    ASSERT(listeners != NULL && pasta_count(listeners) == 3, "3 listeners");
    /* first two are embedded maps */
    ASSERT(pasta_type(pasta_array_get(listeners, 0)) == PASTA_MAP, "[0] is map");
    ASSERT(pasta_get_bool(pasta_map_get(pasta_array_get(listeners, 0), "verify")) == 1,
           "[0].verify=true");
    ASSERT(pasta_get_bool(pasta_map_get(pasta_array_get(listeners, 1), "client_auth")) == 1,
           "[1].client_auth=true");
    /* third is a plain string — no matching section */
    ASSERT(pasta_type(pasta_array_get(listeners, 2)) == PASTA_STRING, "[2] is string");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(listeners, 2)), "fallback") == 0,
           "[2]=fallback");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_in_nested_map_value(void) {
    SUITE("Pass 3: label-ref inside a nested map value");

    AlfResult r;
    const char *srcs[] = {
        "@creds { user: \"admin\", pass: \"s3cr3t\" }",
        "@app   { name: \"api\", connections: { primary: creds, timeout: 30 } }"
    };
    PastaValue *out = run_aggregate(srcs, 2, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *conns = pasta_map_get(pasta_map_get(out, "app"), "connections");
    ASSERT(conns != NULL, "connections exists");
    const PastaValue *primary = pasta_map_get(conns, "primary");
    ASSERT(primary != NULL && pasta_type(primary) == PASTA_MAP, "primary is embedded map");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(primary, "user")), "admin") == 0,
           "user=admin");
    ASSERT(pasta_get_number(pasta_map_get(conns, "timeout")) == 30.0, "timeout=30");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_diamond(void) {
    SUITE("Pass 3: diamond dependency — two sections both link same third");

    AlfResult r;
    /* @tls is referenced by both @http and @grpc — must be resolved correctly for each */
    const char *srcs[] = {
        "@tls  { cert: \"shared.pem\", verify: true }",
        "@http { port: 443,  tls: tls }",
        "@grpc { port: 8443, tls: tls }",
        "@net  { http: http, grpc: grpc }"
    };
    PastaValue *out = run_aggregate(srcs, 4, &r);
    ASSERT(out != NULL, "parsed");

    /* Both http and grpc must have tls embedded */
    const PastaValue *net  = pasta_map_get(out, "net");
    const PastaValue *http = pasta_map_get(net, "http");
    const PastaValue *grpc = pasta_map_get(net, "grpc");
    ASSERT(http != NULL && pasta_type(http) == PASTA_MAP, "http embedded");
    ASSERT(grpc != NULL && pasta_type(grpc) == PASTA_MAP, "grpc embedded");

    const PastaValue *http_tls = pasta_map_get(http, "tls");
    const PastaValue *grpc_tls = pasta_map_get(grpc, "tls");
    ASSERT(http_tls != NULL && pasta_type(http_tls) == PASTA_MAP, "http.tls embedded");
    ASSERT(grpc_tls != NULL && pasta_type(grpc_tls) == PASTA_MAP, "grpc.tls embedded");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(http_tls, "cert")), "shared.pem") == 0,
           "http.tls.cert correct");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(grpc_tls, "cert")), "shared.pem") == 0,
           "grpc.tls.cert correct");

    /* Ports preserved */
    ASSERT(pasta_get_number(pasta_map_get(http, "port")) == 443.0,  "http.port=443");
    ASSERT(pasta_get_number(pasta_map_get(grpc, "port")) == 8443.0, "grpc.port=8443");
    pasta_free(out);

    SUITE_OK();
}

static void test_link_label_not_in_output_but_in_input(void) {
    SUITE("Pass 3: label-ref resolved from input even when target not in output");

    /* In conflate mode, @tls is not in the recipe so it won't be in the output map.
       But @net references it — pass 3 must look it up from the inputs. */
    AlfResult r;
    const char *recipe =
        "@net { consumes: [\"net\"], port: \"r\", tls: \"r\" }";
    const char *srcs[] = {
        "@tls { cert: \"from-input.pem\" }",   /* not in recipe, not in output */
        "@net { port: 443, tls: tls }"
    };
    PastaValue *out = run_conflate(recipe, srcs, 2, &r);
    ASSERT(out != NULL, "process ok");

    /* Only @net in output — @tls not present as a top-level section */
    ASSERT(pasta_count(out) == 1, "only net in output");
    ASSERT(pasta_map_get(out, "tls") == NULL, "tls not a top-level section");

    const PastaValue *tls = pasta_map_get(pasta_map_get(out, "net"), "tls");
    ASSERT(tls != NULL && pasta_type(tls) == PASTA_MAP, "tls embedded from input");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(tls, "cert")), "from-input.pem") == 0,
           "cert from input tls section");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  16. Deep layering: 5-layer microservice deployment                */
/* ================================================================== */

/*
 * Layer stack (lowest to highest precedence):
 *   L1  layer_defaults.pasta  — org-wide defaults
 *   L2  layer_platform.pasta  — cluster settings + var substitution
 *   L3  layer_env_prod.pasta  — production env overrides
 *   L4  layer_region_eu.pasta — EU region specifics
 *   L5  layer_service_api.pasta — service-specific settings
 *   vars layer_vars.pasta     — deployment variables (consumed by pass 1)
 *
 * Recipe: layer_recipe.pasta  — declares final output shape
 *
 * Expected winners per field:
 *   app.log_format        → L1 "json"        (no override)
 *   app.log_level         → L3 "warn"        (L2 doesn't set it, L3 wins over L1)
 *   app.debug             → L1 false         (L3 re-asserts false — no change)
 *   app.max_connections   → L3 1000          (L3 overrides L1's 100)
 *   app.replicas          → L3 8             (first set in L3)
 *   app.registry          → L2 (var-subst)   (first set in L2)
 *   app.pull_policy       → L2 "IfNotPresent"
 *   app.port              → L5 8080          (first set in L5)
 *   app.health_path       → L5 "/healthz"
 *   app.data_residency    → L4 "EU"
 *   app.gdpr_mode         → L4 true
 *
 *   db.host               → L4 (var-subst)   (L4 overrides L2's host)
 *   db.name               → L5 "api_service_prod" (L5 overrides L2's template)
 *   db.pool_max           → L3 100           (L3 overrides L1's 10)
 *   db.read_replica       → L4 (var-subst)   (L4 overrides L3)
 *   db.backup_region      → L4 "eu-central-1"
 *
 *   cache.host            → L4 (var-subst)   (L4 overrides L2)
 *   cache.ttl_seconds     → L5 60            (L5 overrides L3's 3600 and L1's 300)
 *   cache.key_prefix      → L5 "api:"
 *
 *   observability.tracing_enabled → L2 true  (L1 false, L2 sets true)
 *   observability.tracing_endpoint→ L4 (var-subst, overrides L2)
 *   observability.log_sampling_rate→ L3 0.1  (L3 overrides L1's 1.0)
 *   observability.alert_threshold_ms → L3 500
 *   observability.metrics_endpoint → L4 (var-subst)
 *
 *   security.tls_cert     → L4 (var-subst)   (L4 overrides L2)
 *   security.token_ttl_s  → L3 900           (L3 overrides L1's 3600)
 *   security.rate_limit_rps→ L3 5000         (L3 overrides L1's 100)
 *   security.audit_log    → L3 true
 *   security.data_classification → L4 "EU-restricted"
 *   security.issuer       → L2 (var-subst)
 */

static void test_deep_layering_pipeline(void) {
    SUITE("Deep layering: 5-layer microservice deployment conflate");

    size_t len;
    char *L1   = read_file("layer_defaults.pasta",  &len);
    char *L2   = read_file("layer_platform.pasta",  &len);
    char *L3   = read_file("layer_env_prod.pasta",  &len);
    char *L4   = read_file("layer_region_eu.pasta", &len);
    char *L5   = read_file("layer_service_api.pasta",&len);
    char *vars = read_file("layer_vars.pasta",       &len);
    char *rec  = read_file("layer_recipe.pasta",     &len);

    if (!L1||!L2||!L3||!L4||!L5||!vars||!rec) {
        free(L1);free(L2);free(L3);free(L4);free(L5);free(vars);free(rec);
        ASSERT(0, "file load failed");
        return;
    }

    AlfResult r;
    const char *srcs[] = { L1, L2, L3, L4, L5, vars };
    PastaValue *out = run_conflate(rec, srcs, 6, &r);
    free(L1);free(L2);free(L3);free(L4);free(L5);free(vars);free(rec);

    ASSERT(out != NULL, "process ok");
    ASSERT(r.code == ALF_OK, "no error");
    ASSERT(pasta_count(out) == 5, "5 output sections");

    /* ---- @app ---- */
    const PastaValue *app = pasta_map_get(out, "app");
    ASSERT(app != NULL, "app section");

    /* L1 survivors */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_format")), "json") == 0,
           "log_format: L1");
    ASSERT(pasta_get_bool(pasta_map_get(app, "debug")) == 0,
           "debug: L1 false");

    /* L2 survivors (var-substituted) */
    const char *registry = pasta_get_string(pasta_map_get(app, "registry"));
    ASSERT(registry && strcmp(registry, "registry.prod-k8s.internal") == 0,
           "registry: L2 var-substituted");
    const char *ns = pasta_get_string(pasta_map_get(app, "namespace"));
    ASSERT(ns && strcmp(ns, "prod-k8s-services") == 0,
           "namespace: L2 var-substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "pull_policy")),
                  "IfNotPresent") == 0, "pull_policy: L2");

    /* L2 image uses registry var which itself is var-substituted */
    const char *image = pasta_get_string(pasta_map_get(app, "image"));
    ASSERT(image != NULL, "image exists");
    ASSERT(strstr(image, "api_service") != NULL, "image contains service_name");
    ASSERT(strstr(image, "4.2.1") != NULL, "image contains version");

    /* L3 winners */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "log_level")), "warn") == 0,
           "log_level: L3 wins over L1");
    ASSERT(pasta_get_number(pasta_map_get(app, "max_connections")) == 1000.0,
           "max_connections: L3");
    ASSERT(pasta_get_number(pasta_map_get(app, "replicas")) == 8.0,
           "replicas: L3");

    /* L4 winners */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "data_residency")), "EU") == 0,
           "data_residency: L4");
    ASSERT(pasta_get_bool(pasta_map_get(app, "gdpr_mode")) == 1,
           "gdpr_mode: L4");

    /* L5 winners */
    ASSERT(pasta_get_number(pasta_map_get(app, "port")) == 8080.0,
           "port: L5");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "health_path")), "/healthz") == 0,
           "health_path: L5");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(app, "ready_path")),  "/readyz") == 0,
           "ready_path: L5");
    const PastaValue *routes = pasta_map_get(app, "routes");
    ASSERT(routes != NULL && pasta_count(routes) == 2, "routes: L5, 2 entries");
    ASSERT(strcmp(pasta_get_string(pasta_array_get(routes, 0)), "/api/v1") == 0,
           "routes[0]=/api/v1");

    /* ---- @db ---- */
    const PastaValue *db = pasta_map_get(out, "db");
    ASSERT(db != NULL, "db section");

    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "engine")), "postgres") == 0,
           "engine: L1");
    ASSERT(pasta_get_number(pasta_map_get(db, "port")) == 5432.0,
           "port: L1");
    ASSERT(pasta_get_bool(pasta_map_get(db, "ssl")) == 1,
           "ssl: L1");
    ASSERT(pasta_get_number(pasta_map_get(db, "timeout_ms")) == 5000.0,
           "timeout_ms: L1");

    /* L2 host overridden by L4 */
    const char *db_host = pasta_get_string(pasta_map_get(db, "host"));
    ASSERT(db_host && strstr(db_host, "eu-west-1") != NULL,
           "host: L4 wins (region-specific, var-substituted)");

    /* L3 pool_max */
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_max")) == 100.0,
           "pool_max: L3 wins over L1");
    ASSERT(pasta_get_number(pasta_map_get(db, "pool_min")) == 10.0,
           "pool_min: L3 wins over L1");

    /* L4 read_replica and backup_region */
    const char *rr = pasta_get_string(pasta_map_get(db, "read_replica"));
    ASSERT(rr && strstr(rr, "eu-west-1") != NULL,
           "read_replica: L4 wins over L3, var-substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "backup_region")),
                  "eu-central-1") == 0, "backup_region: L4");

    /* L5 name */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(db, "name")),
                  "api_service_prod") == 0, "name: L5 wins over L2");

    /* ---- @cache ---- */
    const PastaValue *cache = pasta_map_get(out, "cache");
    ASSERT(cache != NULL, "cache section");

    ASSERT(strcmp(pasta_get_string(pasta_map_get(cache, "engine")), "redis") == 0,
           "engine: L1");
    ASSERT(pasta_get_number(pasta_map_get(cache, "port")) == 6379.0,
           "port: L1");

    /* L4 host beats L2 */
    const char *c_host = pasta_get_string(pasta_map_get(cache, "host"));
    ASSERT(c_host && strstr(c_host, "eu-west-1") != NULL,
           "host: L4 wins over L2, var-substituted");

    /* L5 ttl_seconds beats L3 (60 vs 3600) beats L1 (300) */
    ASSERT(pasta_get_number(pasta_map_get(cache, "ttl_seconds")) == 60.0,
           "ttl_seconds: L5 wins over L3 and L1");

    /* L5 key_prefix */
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cache, "key_prefix")), "api:") == 0,
           "key_prefix: L5");

    /* L3 max_entries */
    ASSERT(pasta_get_number(pasta_map_get(cache, "max_entries")) == 500000.0,
           "max_entries: L3 wins over L1");

    /* ---- @observability ---- */
    const PastaValue *obs = pasta_map_get(out, "observability");
    ASSERT(obs != NULL, "observability section");

    ASSERT(pasta_get_bool(pasta_map_get(obs, "metrics_enabled")) == 1,
           "metrics_enabled: L1");
    ASSERT(pasta_get_number(pasta_map_get(obs, "metrics_port")) == 9090.0,
           "metrics_port: L1");
    ASSERT(pasta_get_number(pasta_map_get(obs, "scrape_interval_s")) == 15.0,
           "scrape_interval_s: L1");

    /* L2 sets tracing_enabled true (L1 had false) */
    ASSERT(pasta_get_bool(pasta_map_get(obs, "tracing_enabled")) == 1,
           "tracing_enabled: L2 wins over L1");

    /* L3 overrides */
    ASSERT(pasta_get_number(pasta_map_get(obs, "log_sampling_rate")) < 0.2,
           "log_sampling_rate: L3 (0.1) wins over L1 (1.0)");
    ASSERT(pasta_get_number(pasta_map_get(obs, "alert_threshold_ms")) == 500.0,
           "alert_threshold_ms: L3");

    /* L4 overrides tracing_endpoint set by L2 */
    const char *te = pasta_get_string(pasta_map_get(obs, "tracing_endpoint"));
    ASSERT(te && strstr(te, "eu-west-1") != NULL,
           "tracing_endpoint: L4 wins over L2, var-substituted");

    const char *me = pasta_get_string(pasta_map_get(obs, "metrics_endpoint"));
    ASSERT(me && strstr(me, "eu-west-1") != NULL,
           "metrics_endpoint: L4, var-substituted");

    /* ---- @security ---- */
    const PastaValue *sec = pasta_map_get(out, "security");
    ASSERT(sec != NULL, "security section");

    ASSERT(pasta_get_bool(pasta_map_get(sec, "tls_enabled")) == 1,
           "tls_enabled: L1");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(sec, "tls_min_version")),
                  "TLSv1.2") == 0, "tls_min_version: L1");
    ASSERT(pasta_get_bool(pasta_map_get(sec, "auth_required")) == 1,
           "auth_required: L1");

    /* L2 issuer (var-substituted) */
    const char *issuer = pasta_get_string(pasta_map_get(sec, "issuer"));
    ASSERT(issuer && strstr(issuer, "prod-k8s") != NULL,
           "issuer: L2 var-substituted");

    /* L3 overrides */
    ASSERT(pasta_get_number(pasta_map_get(sec, "token_ttl_s")) == 900.0,
           "token_ttl_s: L3 wins over L1 (3600)");
    ASSERT(pasta_get_number(pasta_map_get(sec, "rate_limit_rps")) == 5000.0,
           "rate_limit_rps: L3 wins over L1 (100)");
    ASSERT(pasta_get_bool(pasta_map_get(sec, "audit_log")) == 1,
           "audit_log: L3");

    /* L4 overrides L2's cert/key paths */
    const char *cert = pasta_get_string(pasta_map_get(sec, "tls_cert"));
    ASSERT(cert && strstr(cert, "eu-west-1") != NULL,
           "tls_cert: L4 wins over L2, var-substituted");
    const char *key = pasta_get_string(pasta_map_get(sec, "tls_key"));
    ASSERT(key && strstr(key, "eu-west-1") != NULL,
           "tls_key: L4 wins over L2, var-substituted");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(sec, "data_classification")),
                  "EU-restricted") == 0, "data_classification: L4");

    pasta_free(out);
    SUITE_OK();
}

static void test_deep_layering_precedence_table(void) {
    SUITE("Deep layering: explicit per-field precedence verification");

    /* Stripped-down version of the 5-layer stack targeting only the fields
       where multiple layers compete, to verify the exact winner precisely. */
    AlfResult r;
    const char *srcs[] = {
        /* L1 */
        "@cfg { a: \"L1\", b: \"L1\", c: \"L1\", d: \"L1\", e: \"L1\" }",
        /* L2 — overrides b, c, d, e */
        "@cfg { b: \"L2\", c: \"L2\", d: \"L2\", e: \"L2\" }",
        /* L3 — overrides c, d, e */
        "@cfg { c: \"L3\", d: \"L3\", e: \"L3\" }",
        /* L4 — overrides d, e */
        "@cfg { d: \"L4\", e: \"L4\" }",
        /* L5 — overrides e */
        "@cfg { e: \"L5\" }"
    };
    PastaValue *out = run_aggregate(srcs, 5, &r);
    ASSERT(out != NULL, "parsed");

    const PastaValue *cfg = pasta_map_get(out, "cfg");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "a")), "L1") == 0,
           "a: L1 wins (only set in L1)");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "b")), "L2") == 0,
           "b: L2 wins over L1");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "c")), "L3") == 0,
           "c: L3 wins over L1 and L2");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "d")), "L4") == 0,
           "d: L4 wins over L1, L2, L3");
    ASSERT(strcmp(pasta_get_string(pasta_map_get(cfg, "e")), "L5") == 0,
           "e: L5 wins over all");
    pasta_free(out);

    SUITE_OK();
}

static void test_deep_layering_vars_resolved_before_merge(void) {
    SUITE("Deep layering: vars consumed before any section merging");

    /* Vars defined across three inputs — later vars win — then all
       sections are resolved against the merged var table. */
    AlfResult r;
    const char *srcs[] = {
        "@vars { env: \"dev\",  region: \"us-east\" }",
        "@vars { env: \"prod\" }",                          /* env overrides */
        "@vars { region: \"eu-west\" }",                    /* region overrides */
        "@app  { tag: \"{env}-{region}\" }",
        "@db   { host: \"db.{region}.{env}.internal\" }"
    };
    PastaValue *out = run_aggregate(srcs, 5, &r);
    ASSERT(out != NULL, "parsed");
    ASSERT(pasta_map_get(out, "vars") == NULL, "vars section absent");

    const char *tag = pasta_get_string(pasta_map_get(pasta_map_get(out, "app"), "tag"));
    ASSERT(tag && strcmp(tag, "prod-eu-west") == 0,
           "tag: both overridden vars used");

    const char *host = pasta_get_string(pasta_map_get(pasta_map_get(out, "db"), "host"));
    ASSERT(host && strcmp(host, "db.eu-west.prod.internal") == 0,
           "host: both vars applied after merge");
    pasta_free(out);

    SUITE_OK();
}

/* ================================================================== */
/*  MAIN                                                               */
/* ================================================================== */

int main(void) {
    printf("========================================\n");
    printf("  Alforno Processor Test Suite\n");
    printf("========================================\n");

    test_api_safety();

    /* Pass 1 */
    test_parameterize_basic();
    test_parameterize_multi_input();
    test_parameterize_number_var();
    test_parameterize_unresolved();
    test_parameterize_no_vars();
    test_parameterize_at_literal();

    /* Pass 2: aggregate */
    test_aggregate_single();
    test_aggregate_two_inputs_merge();
    test_aggregate_array_section();
    test_aggregate_multiple_sections();

    /* Pass 2: conflate */
    test_conflate_basic();
    test_conflate_last_write_wins();
    test_conflate_consumes_scoping();
    test_conflate_missing_consumes_error();
    test_conflate_multi_recipe_sections();

    /* Pass 3: links */
    test_link_basic();
    test_link_from_input();
    test_link_non_matching_label();
    test_link_chained();
    test_link_cycle_error();

    /* Full pipeline */
    test_pipeline_layering_dev();
    test_pipeline_layering_prod();
    test_pipeline_cascade_tls();
    test_pipeline_cascade_db();
    test_pipeline_cascade_deploy();

    /* Pass 1: additional */
    test_parameterize_bool_var();
    test_parameterize_multiple_vars_in_string();
    test_parameterize_in_array_elements();
    test_parameterize_in_nested_map();
    test_parameterize_vars_only_input();

    /* Pass 2 aggregate: additional */
    test_aggregate_three_way_precedence();
    test_aggregate_null_override();
    test_aggregate_field_added_in_late_layer();
    test_aggregate_section_in_all_three_inputs();

    /* Pass 2 conflate: additional */
    test_conflate_fan_in();
    test_conflate_no_matching_inputs();
    test_conflate_label_ref_passes_through();

    /* Pass 3 links: additional */
    test_link_in_array_element();
    test_link_in_nested_map_value();
    test_link_diamond();
    test_link_label_not_in_output_but_in_input();

    /* Deep layering */
    test_deep_layering_pipeline();
    test_deep_layering_precedence_table();
    test_deep_layering_vars_resolved_before_merge();
    test_input_validation();
    test_output_roundtrip();

    printf("\n========================================\n");
    printf("  Suites: %d / %d passed\n", suite_passed, suite_run);
    printf("  Tests:  %d / %d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(" (%d FAILED)", tests_failed);
    printf("\n========================================\n");

    return tests_failed == 0 ? 0 : 1;
}
