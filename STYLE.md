# C style

This guide applies to project-owned C sources, headers, probes, and tests.
Use portable C11, as configured by CMake. Formatting choices are project policy;
language rules follow the [WG14 C11 draft, N1570](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf).

## Layout and names

- Use four spaces, LF endings, and a final newline. Aim for 100 columns; keep
  literal text intact when wrapping would obscure its meaning.
- Put opening braces on the same line. Use braces for control-flow bodies in
  new or materially refactored functions. Keep one statement per line.
- Write `const char *name` and `Type *value`. Declare one variable at a time,
  close to its first use, and initialize resource handles before cleanup paths.
- Use `snake_case` for functions and variables, `PascalCase` for named types,
  and `UPPER_SNAKE_CASE` for enum constants and macros. Preserve established
  public names and protocol terminology.
- Prefix public functions with their module name. Keep helpers `static` until
  another caller needs the same documented contract. Avoid reserved identifiers
  and names that shadow parameters or nearby variables.
- Include the matching header first, followed by project and standard headers.
  Keep required platform feature definitions before system headers. Headers must
  provide their own dependencies and use include guards.

Use clang-format **21.1.8** with the checked-in `.clang-format`. It handles layout;
ownership, naming, boundaries, and control-flow changes still require review.
The configuration preserves include order, comments, macros, and string literals.
See the [LLVM configuration reference](https://releases.llvm.org/21.1.0/tools/clang/docs/ClangFormatStyleOptions.html).

```sh
clang-format --style=file -i path/to/reviewed.c path/to/reviewed.h
clang-format --style=file --dry-run --Werror path/to/reviewed.c path/to/reviewed.h
```

## Modules and functions

- Give each module one responsibility. Public headers describe contracts;
  private headers contain implementation details shared within that subsystem.
- Keep generic Unity serialization and I/O in UnityCommon. Keep DXBC, shader
  analysis, translation, and certification in this repository.
- Search for an existing helper before adding one. Share behavior only when
  types, ownership, error handling, limits, and accepted inputs agree.
- Extract named operations when a function mixes parsing, validation, execution,
  and publication. Prefer explicit steps and early validation to deep nesting.
  Choose boundaries by responsibility rather than an arbitrary line count.
- Use typed data structures instead of positional argument lists, repeated
  Boolean switches, or untyped containers. Introduce an abstraction only when
  it removes a real repetition without hiding the contract.
- Use functions for executable logic. Reserve macros for constants, conditional
  compilation, and cases that actually require preprocessing.
- Comments explain invariants, ownership, units, and non-obvious decisions.
  Remove stale explanations and comments that merely repeat the statement.

Focused functions and descriptive cleanup labels are also recommended in the
[Linux kernel guidance on functions and cleanup](https://docs.kernel.org/process/coding-style.html#functions).

## Ownership and errors

- Document borrowed inputs, owned results, lifetimes, initialization, null/empty
  cases, aliasing restrictions, and output state on failure in the API header.
- Give each allocation one owner. Pair allocation and disposal in the same
  abstraction; reset disposed objects to their initialized state. See
  [CERT MEM31-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/memory-management-mem/mem31-c/).
- Build results locally and publish them after validation succeeds. Preserve
  existing transactional output contracts, including rollback after callbacks.
- Use a status enum when callers need distinct failure reasons; use `bool` for
  a genuine yes/no result. Check fallible allocation, formatting, I/O, process,
  and compiler calls. Preserve the original error when cleanup can overwrite it.
  See [CERT ERR33-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/error-handling-err/err33-c/).
- Use an explicit cleanup label when several resources share an error path.
  Release only acquired resources, in reverse order. Simple functions may return
  directly when they own nothing requiring cleanup.
- Validate external input with normal error handling. Assertions express
  internal invariants and must not replace checks needed in Release builds.

## Bounds and representation

- Use `size_t` for object sizes and indices, and fixed-width unsigned types for
  binary fields. Check narrowing conversions and arithmetic before using results
  as lengths, allocation sizes, offsets, or shift counts. See
  [CERT INT30-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/integers-int/int30-c/).
- Include `<stdlib.h>` and use allocation results without casts in C. Size
  allocations from the pointed-to object, using `sizeof(*items)`.
- Check `count > SIZE_MAX / sizeof(*items)` before multiplying allocation sizes;
  reuse the project's checked-size helpers in production code. Impose documented
  input or analysis limits as well as arithmetic limits. See
  [CERT MEM35-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/memory-management-mem/mem35-c/).
- Check `offset <= size` before testing `length <= size - offset`. Form pointers
  only after the range is valid. Decode binary layouts and endianness explicitly;
  avoid casts that assume alignment or compatible object types.
- Compare C structures by meaningful members. Use byte comparisons for actual
  byte sequences, not structure padding. See
  [CERT EXP42-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/expressions-exp/exp42-c/).
- Preserve raw floating-point bits when the format requires them. Never change
  precision, operation order, effects, undefined lanes, or selected stage/variant
  behavior as part of a readability refactor.

## Examples

These examples are original, self-contained C11 illustrations. Production code
should reuse existing buffer and byte-stream APIs rather than add parallel ones.

### Checked allocation and transactional ownership

```c
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { WORD_BUFFER_MAX_WORDS = 16384 };

typedef enum {
    WORD_BUFFER_OK = 0,
    WORD_BUFFER_INVALID_ARGUMENT,
    WORD_BUFFER_LIMIT_EXCEEDED,
    WORD_BUFFER_OUT_OF_MEMORY
} WordBufferStatus;

typedef struct {
    uint32_t *words;
    size_t count;
} WordBuffer;

/* Initialize destination with {0}. Source is borrowed for this call.
 * Success transfers the copy to destination; failure leaves it unchanged. */
WordBufferStatus word_buffer_copy(const uint32_t *source, size_t count, WordBuffer *destination) {
    if (!destination || destination->words || destination->count || (!source && count != 0)) {
        return WORD_BUFFER_INVALID_ARGUMENT;
    }
    if (count > WORD_BUFFER_MAX_WORDS || count > SIZE_MAX / sizeof(*source)) {
        return WORD_BUFFER_LIMIT_EXCEEDED;
    }

    WordBuffer candidate = {0};
    if (count != 0) {
        candidate.words = malloc(count * sizeof(*candidate.words));
        if (!candidate.words) {
            return WORD_BUFFER_OUT_OF_MEMORY;
        }
        memcpy(candidate.words, source, count * sizeof(*candidate.words));
        candidate.count = count;
    }

    *destination = candidate;
    return WORD_BUFFER_OK;
}

/* Accepts NULL or an initialized buffer; repeated disposal is safe. */
void word_buffer_dispose(WordBuffer *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->words);
    *buffer = (WordBuffer){0};
}
```

### Bounded binary decoding

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* bytes is a borrowed size-byte range. Failure leaves value unchanged. */
bool read_u32_le(const uint8_t *bytes, size_t size, size_t offset, uint32_t *value) {
    if (!bytes || !value || offset > size || size - offset < sizeof(*value)) {
        return false;
    }

    const uint8_t *word = bytes + offset;
    *value = (uint32_t)word[0] | ((uint32_t)word[1] << 8) | ((uint32_t)word[2] << 16) |
             ((uint32_t)word[3] << 24);
    return true;
}
```

## Refactoring and review

- Apply this guide to new code and the functions being materially refactored.
  Migrate legacy modules in focused patches; keep mechanical formatting separate
  from behavior changes. Leave third-party and generated inputs under their own
  conventions.
- Preserve API/ABI, ownership, diagnostics, optional builds, and emitted shader
  bytes unless a change explicitly requires otherwise. Formatting C must not
  rewrite generated HLSL/ShaderLab text or binary fixtures.
- Reuse existing parsers, fixtures, analysis passes, and verifiers. Test the
  affected contracts, failure paths, and configurations; use sanitizers for
  memory/ownership changes. Fix warnings rather than suppressing them.
- Keep readability, compilation, complete DXBC equality, semantic proof, import,
  and physical validation as separate claims. A refactor cannot weaken a gate
  or turn an unavailable check into success.
- Review the diff, run `git diff --check`, and check privacy and licenses before
  publishing. Preserve GPL notices, acknowledgements, and unrelated work.
