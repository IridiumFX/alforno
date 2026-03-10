# Alforno

A C library for merging and composing [Pasta](https://github.com/IridiumFX/Pasta) configuration files.

Alforno (*al forno* — Italian for "baked") takes one or more input pastlets and produces a single output pastlet through a three-pass pipeline: **parameterize**, **merge**, **link**.

## Operations

- **Aggregate** — open union: all sections and fields from all inputs pass through.
- **Conflate** — controlled merge: a recipe declares the output contract; unlisted sections and fields are dropped.

## Building

Requires CMake 3.20+ and a C11 compiler. The [Pasta](https://github.com/IridiumFX/Pasta) library is fetched automatically.

```bash
cmake --preset linux        # or: macos, windows, freebsd
cmake --build build/linux
./build/linux/bin/alforno_test
```

See `CMakePresets.json` for all available presets (Linux, macOS, Windows, FreeBSD, iOS, Android).

## API

```c
#include "alforno.h"

AlfResult result;
AlfContext *ctx = alf_create(ALF_AGGREGATE, &result);

alf_add_input(ctx, src1, len1, &result);
alf_add_input(ctx, src2, len2, &result);

PastaValue *output = alf_process(ctx, &result);
/* use output... */
pasta_free(output);
alf_free(ctx);
```

For conflate, call `alf_set_recipe()` before adding inputs.

## Documentation

- [Specification](specs/alforno-spec.md) — formal definition of the processing pipeline
- [Guide](specs/alforno-guide.md) — worked examples and case studies

## License

MIT — see [LICENSE](LICENSE).
