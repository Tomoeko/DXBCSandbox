#!/bin/sh

# Some launch environments inject C.UTF-8 even on macOS installations that do
# not provide that locale. Enter Bash through a POSIX shell with the universally
# available C locale so normal shebang execution is warning-free and sorting,
# token classification, and diagnostics remain deterministic.
if [ "${DXBC_QUICK_TEST_BASH_REEXEC:-0}" != "1" ]; then
    exec env DXBC_QUICK_TEST_BASH_REEXEC=1 LC_ALL=C LANG=C LC_CTYPE=C \
        bash "$0" "$@"
fi
unset DXBC_QUICK_TEST_BASH_REEXEC

set -eo pipefail

# Quick iteration script for DXBC mismatch fixing.
#
# Usage:
#   DXBC_BUNDLE_PATH=/path/to/shaders.bundle \
#     DXBC_ORIGINAL_SHADERS_DIR=/path/to/sources \
#     ./quick_test.sh --profile FILE
#   DXBC_BUNDLE_PATH=/path/to/shaders.bundle \
#     ./quick_test.sh --direct-only --profile FILE
#
# DXBC_WORKERS controls CPU parsing/comparison threads. All threads share one
# persistent compiler broker and at most one live UnityShaderCompiler process.
# DXBC_USC_CACHE_DIR selects the persistent cache; DXBC_OUT_ROOT selects the
# parent of fresh per-run output directories. Unity itself can be relocated
# with DXBC_UNITY_CONTENTS_PATH, DXBC_UNITY_APP, or UNITY_EDITOR_PATH; see
# README.md for supported workflows and limitations.
# Every build, decompile, and compiler-verification transcript is retained in
# the fresh verification-artifacts directory.  Console summaries never replace
# those logs.

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BUILD_DIR="${DXBC_BUILD_DIR:-$SCRIPT_DIR/build}"
BUNDLE="${DXBC_BUNDLE_PATH:-}"
ORIG="${DXBC_ORIGINAL_SHADERS_DIR:-}"
FILTER_PATH="${DXBC_FILTER_PATH:-/dev/null}"
REQUESTED_OUT="${DXBC_GENERATED_SHADERLAB_DIR:-}"
OUT_ROOT="${DXBC_OUT_ROOT:-/tmp}"
OUT=""
ARTIFACTS_DIR=""
BUILD_CONFIG="${DXBC_BUILD_CONFIG:-Debug}"
BUILD_JOBS="${DXBC_BUILD_JOBS:-4}"

SHOW_DIFF=0
DIFF_LIMIT=5
ARTIFACTS_FLAG="--no-artifacts"
VERIFY_ARGS=()
CACHE_DIR="${DXBC_USC_CACHE_DIR:-/tmp/dxbc_usc_cache}"
WORKER_COUNT="${DXBC_WORKERS:-2}"
USE_CACHE=1
CACHE_ONLY=0
ORACLE_PACK_VALUE=""
ORACLE_CAPTURE_VALUE=""
ORACLE_STRICT=0
PROFILE_PATH="${DXBC_COMPILE_PROFILE:-}"
DEFAULT_SCHEMA_REGISTRY="$SCRIPT_DIR/schemas/unity-2021.3-player-shader.registry"
SCHEMA_REGISTRY_PATH="${DXBC_SCHEMA_REGISTRY:-$DEFAULT_SCHEMA_REGISTRY}"
SCHEMA_REGISTRY_ARGUMENT_SET=0
EXPECT_PROFILE_PATH=0
EXPECT_SCALAR_VALUE=""
EXPECT_PASSTHROUGH_VALUE=""
PROFILE_ARGUMENT_SET=0
HAS_BUILD_PLATFORM=0
HAS_VALID_APIS=0
HAS_D3D11_CAPS=0
HAS_GLCORE_CAPS=0
DXBC_ONLY=0
DOMAIN_ONLY=0
DIRECT_ONLY=0
BUILD_PLATFORM_VALUE="${DXBC_BUILD_PLATFORM:-}"
VALID_APIS_VALUE="${DXBC_VALID_APIS:-}"
D3D11_CAPS_VALUE="${DXBC_D3D11_PLATFORM_CAPS:-}"
GLCORE_CAPS_VALUE="${DXBC_GLCORE_PLATFORM_CAPS:-}"

usage() {
    cat <<EOF
Usage: $0 [verification options]

Exact verification requires --profile FILE, DXBC_COMPILE_PROFILE, or complete
explicit build/API/capability values. With --dxbc-only, GLCore capabilities
are not required. No profile is searched for or inferred implicitly.

DXBC_BUNDLE_PATH must explicitly name the readable input bundle.
DXBC_ORIGINAL_SHADERS_DIR is also required for full verification and for
artifact-enabled DXBC verification, except in --direct-only mode. Original
source is not read by --direct-only or --dxbc-only without artifact flags.

Options:
  --profile FILE                 use captured compile authority
  --build-platform N             override the Unity build-platform value
  --valid-apis MASK              override the valid-API mask
  --d3d11-platform-caps BITS     override the D3D11 capability snapshot
  --glcore-platform-caps BITS    override the GLCore capability snapshot
  --dxbc-only                    skip GLCore verification
  --domain-only                  certify the complete generated D3D11 pass
                                  domain without the redundant direct census;
                                  requires --dxbc-only
  --direct-only                  preprocess each selected generated candidate
                                  once for exact snippet authority, then
                                  compile only exact filter-selected rows;
                                  skips generated-domain and original-source
                                  work; mutually exclusive with --domain-only
  --workers N                    CPU parsing/comparison workers (1..64)
  --compiler-source-budget-mib N recycle USC above N MiB of unique source
                                  (one oversized source stays resident)
                                  (default 128; 0 disables)
  --oracle-pack FILE             replay exact captured compiler authority
  --oracle-strict                require pack hits; start no compiler process
  --oracle-capture FILE          write a new exact authority pack
  --schema-registry FILE         exact TypeTree schema authority
  --artifacts                    retain mismatch artifacts
  --diff                         show the first five DXBC mismatch diffs
  --diff-all                     show every DXBC mismatch diff
  --cache-dir=PATH               select the persistent compiler cache
  --cache-only                   require exact persistent-cache hits and
                                  never start UnityShaderCompiler
  --no-cache                     disable the persistent compiler cache
  -h, --help                     show this help without configuring/building

Optional path settings: DXBC_FILTER_PATH, DXBC_GENERATED_SHADERLAB_DIR,
DXBC_OUT_ROOT, and DXBC_BUILD_DIR. A requested ShaderLab output directory must
be absent or empty.
EOF
}

resolve_built_tool() {
    local tool_name="$1"
    local candidate
    for candidate in \
        "$BUILD_DIR/$tool_name" \
        "$BUILD_DIR/$BUILD_CONFIG/$tool_name" \
        "$BUILD_DIR/Debug/$tool_name" \
        "$BUILD_DIR/Release/$tool_name" \
        "$BUILD_DIR/RelWithDebInfo/$tool_name" \
        "$BUILD_DIR/MinSizeRel/$tool_name"; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    echo "error: built tool was not found: $tool_name" >&2
    return 1
}

prepare_requested_output() {
    local requested="$1"
    if [ -e "$requested" ] && [ ! -d "$requested" ]; then
        echo "error: ShaderLab output path is not a directory: $requested" >&2
        return 1
    fi
    if [ -d "$requested" ] &&
       [ -n "$(find "$requested" -mindepth 1 -print -quit)" ]; then
        echo "error: ShaderLab output directory must be empty: $requested" >&2
        return 1
    fi
    mkdir -p "$requested"
}

[ -n "${DXBC_BUILD_PLATFORM:-}" ] && HAS_BUILD_PLATFORM=1
[ -n "${DXBC_VALID_APIS:-}" ] && HAS_VALID_APIS=1
[ -n "${DXBC_D3D11_PLATFORM_CAPS:-}" ] && HAS_D3D11_CAPS=1
[ -n "${DXBC_GLCORE_PLATFORM_CAPS:-}" ] && HAS_GLCORE_CAPS=1

for arg in "$@"; do
    if [ "$EXPECT_PROFILE_PATH" = "1" ]; then
        if [ -z "$arg" ] || [[ "$arg" == --* ]]; then
            echo "error: --profile requires a path" >&2
            exit 2
        fi
        PROFILE_PATH="$arg"
        EXPECT_PROFILE_PATH=0
        VERIFY_ARGS+=("$arg")
        continue
    fi
    if [ -n "$EXPECT_PASSTHROUGH_VALUE" ]; then
        if [ -z "$arg" ] || [[ "$arg" == --* ]]; then
            echo "error: $EXPECT_PASSTHROUGH_VALUE requires a value" >&2
            exit 2
        fi
        case "$EXPECT_PASSTHROUGH_VALUE" in
            --oracle-pack) ORACLE_PACK_VALUE="$arg" ;;
            --oracle-capture) ORACLE_CAPTURE_VALUE="$arg" ;;
            --schema-registry) SCHEMA_REGISTRY_PATH="$arg" ;;
        esac
        VERIFY_ARGS+=("$arg")
        EXPECT_PASSTHROUGH_VALUE=""
        continue
    fi
    if [ -n "$EXPECT_SCALAR_VALUE" ]; then
        if [ -z "$arg" ] || [[ "$arg" == --* ]]; then
            echo "error: $EXPECT_SCALAR_VALUE requires a value" >&2
            exit 2
        fi
        case "$EXPECT_SCALAR_VALUE" in
            --build-platform)
                HAS_BUILD_PLATFORM=1
                BUILD_PLATFORM_VALUE="$arg"
                ;;
            --valid-apis)
                HAS_VALID_APIS=1
                VALID_APIS_VALUE="$arg"
                ;;
            --d3d11-platform-caps)
                HAS_D3D11_CAPS=1
                D3D11_CAPS_VALUE="$arg"
                ;;
            --glcore-platform-caps)
                HAS_GLCORE_CAPS=1
                GLCORE_CAPS_VALUE="$arg"
                ;;
        esac
        VERIFY_ARGS+=("$arg")
        EXPECT_SCALAR_VALUE=""
        continue
    fi
    case "$arg" in
        -h|--help)
            usage
            exit 0
            ;;
        --artifacts) ARTIFACTS_FLAG="--artifacts" ;;
        --diff)      ARTIFACTS_FLAG="--artifacts"; SHOW_DIFF=1 ;;
        --diff-all)  ARTIFACTS_FLAG="--artifacts"; SHOW_DIFF=1; DIFF_LIMIT=999 ;;
        --no-cache)  USE_CACHE=0 ;;
        --cache-only) CACHE_ONLY=1 ;;
        --cache-dir=*)
            CACHE_DIR="${arg#--cache-dir=}"
            if [ -z "$CACHE_DIR" ]; then
                echo "error: --cache-dir requires a path" >&2
                exit 2
            fi
            ;;
        --profile)
            if [ "$PROFILE_ARGUMENT_SET" = "1" ]; then
                echo "error: --profile may be specified only once" >&2
                exit 2
            fi
            PROFILE_ARGUMENT_SET=1
            EXPECT_PROFILE_PATH=1
            VERIFY_ARGS+=("$arg")
            ;;
        --profile=*)
            if [ "$PROFILE_ARGUMENT_SET" = "1" ]; then
                echo "error: --profile may be specified only once" >&2
                exit 2
            fi
            PROFILE_ARGUMENT_SET=1
            PROFILE_PATH="${arg#--profile=}"
            if [ -z "$PROFILE_PATH" ]; then
                echo "error: --profile requires a path" >&2
                exit 2
            fi
            VERIFY_ARGS+=("$arg")
            ;;
        --build-platform)
            EXPECT_SCALAR_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --valid-apis)
            EXPECT_SCALAR_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --d3d11-platform-caps)
            EXPECT_SCALAR_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --glcore-platform-caps)
            EXPECT_SCALAR_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --dxbc-only)
            DXBC_ONLY=1
            VERIFY_ARGS+=("$arg")
            ;;
        --domain-only)
            if [ "$DOMAIN_ONLY" = "1" ]; then
                echo "error: --domain-only may be specified only once" >&2
                exit 2
            fi
            DOMAIN_ONLY=1
            VERIFY_ARGS+=("$arg")
            ;;
        --direct-only)
            if [ "$DIRECT_ONLY" = "1" ]; then
                echo "error: --direct-only may be specified only once" >&2
                exit 2
            fi
            DIRECT_ONLY=1
            VERIFY_ARGS+=("$arg")
            ;;
        --workers)
            EXPECT_PASSTHROUGH_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --workers=*) VERIFY_ARGS+=("$arg") ;;
        --compiler-source-budget-mib)
            EXPECT_PASSTHROUGH_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --compiler-source-budget-mib=*) VERIFY_ARGS+=("$arg") ;;
        --oracle-pack)
            EXPECT_PASSTHROUGH_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --oracle-pack=*)
            ORACLE_PACK_VALUE="${arg#--oracle-pack=}"
            if [ -z "$ORACLE_PACK_VALUE" ]; then
                echo "error: --oracle-pack requires a path" >&2
                exit 2
            fi
            VERIFY_ARGS+=("$arg")
            ;;
        --oracle-capture)
            EXPECT_PASSTHROUGH_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --oracle-capture=*)
            ORACLE_CAPTURE_VALUE="${arg#--oracle-capture=}"
            if [ -z "$ORACLE_CAPTURE_VALUE" ]; then
                echo "error: --oracle-capture requires a path" >&2
                exit 2
            fi
            VERIFY_ARGS+=("$arg")
            ;;
        --schema-registry)
            if [ "$SCHEMA_REGISTRY_ARGUMENT_SET" = "1" ]; then
                echo "error: --schema-registry may be specified only once" >&2
                exit 2
            fi
            SCHEMA_REGISTRY_ARGUMENT_SET=1
            EXPECT_PASSTHROUGH_VALUE="$arg"
            VERIFY_ARGS+=("$arg")
            ;;
        --schema-registry=*)
            if [ "$SCHEMA_REGISTRY_ARGUMENT_SET" = "1" ]; then
                echo "error: --schema-registry may be specified only once" >&2
                exit 2
            fi
            SCHEMA_REGISTRY_ARGUMENT_SET=1
            SCHEMA_REGISTRY_PATH="${arg#--schema-registry=}"
            if [ -z "$SCHEMA_REGISTRY_PATH" ]; then
                echo "error: --schema-registry requires a path" >&2
                exit 2
            fi
            VERIFY_ARGS+=("$arg")
            ;;
        --oracle-strict)
            ORACLE_STRICT=1
            VERIFY_ARGS+=("$arg")
            ;;
        --*)
            echo "error: unknown quick-test option: $arg" >&2
            exit 2
            ;;
        *)
            echo "error: unexpected quick-test argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [ "$EXPECT_PROFILE_PATH" = "1" ]; then
    echo "error: --profile requires a path" >&2
    exit 2
fi
if [ -n "$EXPECT_SCALAR_VALUE" ]; then
    echo "error: $EXPECT_SCALAR_VALUE requires a value" >&2
    exit 2
fi
if [ -n "$EXPECT_PASSTHROUGH_VALUE" ]; then
    echo "error: $EXPECT_PASSTHROUGH_VALUE requires a value" >&2
    exit 2
fi
if [ "$DOMAIN_ONLY" = "1" ] && [ "$DIRECT_ONLY" = "1" ]; then
    echo "error: --domain-only and --direct-only are mutually exclusive" >&2
    exit 2
fi
if [ "$DOMAIN_ONLY" = "1" ] && [ "$DXBC_ONLY" != "1" ]; then
    echo "error: --domain-only requires --dxbc-only" >&2
    exit 2
fi
if [ "$ORACLE_STRICT" = "1" ] && [ -z "$ORACLE_PACK_VALUE" ]; then
    echo "error: --oracle-strict requires --oracle-pack FILE" >&2
    exit 2
fi
if [ "$CACHE_ONLY" = "1" ] && [ "$USE_CACHE" != "1" ]; then
    echo "error: --cache-only cannot be combined with --no-cache" >&2
    exit 2
fi
if [ -z "$BUNDLE" ]; then
    echo "error: DXBC_BUNDLE_PATH is required; no input bundle is discovered implicitly" >&2
    exit 2
fi
if [ ! -r "$BUNDLE" ]; then
    echo "error: shader bundle is not readable: $BUNDLE" >&2
    exit 2
fi

USES_ORIGINAL_SHADERS=0
if [ "$DIRECT_ONLY" != "1" ] &&
   { [ "$DXBC_ONLY" != "1" ] || [ "$ARTIFACTS_FLAG" = "--artifacts" ]; }; then
    USES_ORIGINAL_SHADERS=1
fi
if [ "$USES_ORIGINAL_SHADERS" = "1" ]; then
    if [ -z "$ORIG" ]; then
        echo "error: DXBC_ORIGINAL_SHADERS_DIR is required by this verification mode" >&2
        exit 2
    fi
    if [ ! -d "$ORIG" ] || [ ! -r "$ORIG" ]; then
        echo "error: original shader directory is not readable: $ORIG" >&2
        exit 2
    fi
else
    # The verifier's '-' sentinel records that original source is unavailable
    # and prevents an optional environment value from changing a source-free
    # verification mode.
    ORIG="-"
fi
if [ -n "$ORACLE_PACK_VALUE" ] && [ ! -r "$ORACLE_PACK_VALUE" ]; then
    echo "error: oracle pack is not readable: $ORACLE_PACK_VALUE" >&2
    exit 2
fi
if [ -n "$ORACLE_CAPTURE_VALUE" ] && [ -e "$ORACLE_CAPTURE_VALUE" ]; then
    echo "error: oracle capture path already exists: $ORACLE_CAPTURE_VALUE" >&2
    exit 2
fi

if [ "$SCHEMA_REGISTRY_ARGUMENT_SET" != "1" ]; then
    VERIFY_ARGS+=("--schema-registry" "$SCHEMA_REGISTRY_PATH")
fi
if [ ! -r "$SCHEMA_REGISTRY_PATH" ]; then
    echo "error: TypeTree schema registry is not readable: $SCHEMA_REGISTRY_PATH" >&2
    exit 2
fi

if [ -n "$PROFILE_PATH" ]; then
    if [ ! -r "$PROFILE_PATH" ]; then
        echo "error: compile profile is not readable: $PROFILE_PATH" >&2
        exit 2
    fi
elif [ "$HAS_BUILD_PLATFORM" != "1" ] ||
     [ "$HAS_VALID_APIS" != "1" ] ||
     [ "$HAS_D3D11_CAPS" != "1" ] ||
     { [ "$DXBC_ONLY" != "1" ] && [ "$HAS_GLCORE_CAPS" != "1" ]; }; then
    if [ "$DXBC_ONLY" = "1" ]; then
        echo "error: exact DXBC verification needs --profile FILE (or explicit DXBC_BUILD_PLATFORM, DXBC_VALID_APIS, and DXBC_D3D11_PLATFORM_CAPS values)" >&2
    else
        echo "error: exact verification needs --profile FILE (or complete explicit DXBC_BUILD_PLATFORM, DXBC_VALID_APIS, DXBC_D3D11_PLATFORM_CAPS, and DXBC_GLCORE_PLATFORM_CAPS values)" >&2
    fi
    echo "error: no compile profile is selected; implicit or inferred compatibility defaults are intentionally not accepted as authority" >&2
    exit 2
fi

# The quick verifier owns its native build configuration. Reconfiguring is
# cheap and makes a fresh checkout usable while repairing stale portable-only
# caches that do not contain the live verifier target.
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
    -DDXBCSANDBOX_BUILD_UNITY_COMPILER=ON \
    -DDXBCSANDBOX_BUILD_UNITY_ORACLE=ON

# Validate canonical syntax, fingerprint, and every scalar before allocating
# corpus output or invoking the decompiler.
cmake --build "$BUILD_DIR" --config "$BUILD_CONFIG" \
    --target compile_profile_cli --parallel "$BUILD_JOBS"
PROFILE_TOOL="$(resolve_built_tool compile_profile_cli)"
if [ -n "$PROFILE_PATH" ]; then
    "$PROFILE_TOOL" validate "$PROFILE_PATH" >/dev/null
fi
if [ "$HAS_BUILD_PLATFORM" = "1" ]; then
    "$PROFILE_TOOL" check-value build-platform \
        "$BUILD_PLATFORM_VALUE" >/dev/null
fi
if [ "$HAS_VALID_APIS" = "1" ]; then
    "$PROFILE_TOOL" check-value valid-apis "$VALID_APIS_VALUE" >/dev/null
fi
if [ "$HAS_D3D11_CAPS" = "1" ]; then
    "$PROFILE_TOOL" check-value capabilities "$D3D11_CAPS_VALUE" >/dev/null
fi
if [ "$HAS_GLCORE_CAPS" = "1" ]; then
    "$PROFILE_TOOL" check-value capabilities "$GLCORE_CAPS_VALUE" >/dev/null
fi

if [ "$FILTER_PATH" != "/dev/null" ] && [ ! -r "$FILTER_PATH" ]; then
    echo "error: filter file is not readable: $FILTER_PATH" >&2
    exit 2
fi

mkdir -p "$OUT_ROOT"
if [ -n "$REQUESTED_OUT" ]; then
    prepare_requested_output "$REQUESTED_OUT"
    OUT="$REQUESTED_OUT"
else
    OUT="$(mktemp -d "$OUT_ROOT/dxbc_quick_test.XXXXXX")"
fi
ARTIFACTS_DIR="$(mktemp -d "$OUT_ROOT/dxbc_quick_artifacts.XXXXXX")"
BUILD_LOG="$ARTIFACTS_DIR/build.log"
DECOMPILE_LOG="$ARTIFACTS_DIR/decompile.log"
VERIFY_LOG="$ARTIFACTS_DIR/verification.log"
VERIFICATION_REPORT="$ARTIFACTS_DIR/verification-report.json"

export DXBC_WORKERS="$WORKER_COUNT"
if [ "$USE_CACHE" = "1" ]; then
    export DXBC_USC_CACHE_DIR="$CACHE_DIR"
else
    unset DXBC_USC_CACHE_DIR
fi
if [ "$CACHE_ONLY" = "1" ]; then
    export DXBC_USC_CACHE_ONLY=1
else
    unset DXBC_USC_CACHE_ONLY
fi

echo "=== Building ==="
set +e
cmake --build "$BUILD_DIR" --config "$BUILD_CONFIG" \
    --target asset_client_cli test_shaderlab_roundtrip \
    --parallel "$BUILD_JOBS" 2>&1 | tee "$BUILD_LOG" >/dev/null
BUILD_STATUS="${PIPESTATUS[0]}"
set -e
tail -3 "$BUILD_LOG"
if [ "$BUILD_STATUS" -ne 0 ]; then
    echo "error: native build failed; full transcript: $BUILD_LOG" >&2
    tail -40 "$BUILD_LOG" >&2
    exit 1
fi
ASSET_CLIENT="$(resolve_built_tool asset_client_cli)"
ROUNDTRIP_VERIFIER="$(resolve_built_tool test_shaderlab_roundtrip)"

echo ""
echo "=== Decompiling ==="
echo "Generated ShaderLab: $OUT"
echo "Verification artifacts: $ARTIFACTS_DIR"
set +e
"$ASSET_CLIENT" "$BUNDLE" --out "$OUT" 2>&1 | \
    tee "$DECOMPILE_LOG" >/dev/null
DECOMPILE_STATUS="${PIPESTATUS[0]}"
set -e
tail -8 "$DECOMPILE_LOG"
if [ "$DECOMPILE_STATUS" -ne 0 ]; then
    echo "[WARN] Decompiler reported unavailable Shader objects; continuing so the verifier can classify emitted and unavailable results."
    echo "[WARN] Full decompiler transcript: $DECOMPILE_LOG"
fi
if [ -z "$(find "$OUT" -type f -name '*.shader' -print -quit)" ]; then
    echo "error: decompiler produced no ShaderLab files" >&2
    exit 1
fi

echo ""
echo "=== Verifying ==="
set +e
"$ROUNDTRIP_VERIFIER" "$BUNDLE" "$ORIG" "$OUT" "$FILTER_PATH" \
    "$ARTIFACTS_DIR" "$ARTIFACTS_FLAG" \
    --report "$VERIFICATION_REPORT" "${VERIFY_ARGS[@]}" 2>&1 | \
    tee "$VERIFY_LOG" >/dev/null
VERIFY_STATUS="${PIPESTATUS[0]}"
set -e
tail -36 "$VERIFY_LOG"

echo ""
echo "Full transcripts:"
echo "  build:        $BUILD_LOG"
echo "  decompile:    $DECOMPILE_LOG"
echo "  verification: $VERIFY_LOG"
if [ -f "$VERIFICATION_REPORT" ]; then
    echo "  JSON report:  $VERIFICATION_REPORT"
fi

if [ "$VERIFY_STATUS" -ne 0 ]; then
    echo ""
    echo "=== Compiler/verification errors (full context in verification log) ==="
    grep -nE "(\[FAIL\]|[Ee]rror:|cannot convert|implicit truncation|DXBCSandbox_)" \
        "$VERIFY_LOG" | tail -40 || true
fi

if [ "$SHOW_DIFF" = "1" ]; then
    echo ""
    echo "=== DXBC Mismatch Diffs (first $DIFF_LIMIT) ==="
    count=0
    for orig in "$ARTIFACTS_DIR"/mismatch_*_dxbc_original.asm; do
        [ -f "$orig" ] || continue
        gen="${orig/_original/_generated}"
        [ -f "$gen" ] || continue
        echo ""
        echo "--- $(basename "$orig" _dxbc_original.asm) ---"
        diff "$orig" "$gen" 2>/dev/null | head -30 || true
        count=$((count + 1))
        [ "$count" -ge "$DIFF_LIMIT" ] && break
    done
fi

if [ "$DECOMPILE_STATUS" -ne 0 ] || [ "$VERIFY_STATUS" -ne 0 ]; then
    exit 1
fi
