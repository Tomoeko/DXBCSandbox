Special thanks to [@nesrak1](https://github.com/nesrak1) for the idea of [USCSandbox](https://github.com/nesrak1/USCSandbox) which was a great foundation to start off.

DXBCSandbox is an experiment using Codex and ChatGPT.

# DXBCSandbox

C11 tools for inspecting Unity shader assets, decoding DXBC, and reconstructing
ShaderLab and HLSL. Targets supported Unity 2021.3 formats.

Default extraction emits low-level HLSL. High-level shader and compute
reconstruction is experimental; unsupported forms are reported explicitly.

## Build

Requires CMake 3.10+ and a C11 compiler.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

With CMake older than 3.20, run `ctest` from the build directory. Unity is optional.

## Usage

```sh
build/dxbc-sandbox list /path/to/assets --format table
build/dxbc-sandbox extract /path/to/assets --all --out recovered
build/dxbc-sandbox extract /path/to/assets --name 'Unlit/Color' --out recovered
```

Accepts UnityFS bundles, SerializedFiles, and player folders. For inputs without
type trees, use `--schema-registry schemas/unity-2021.3-player-shader.registry`.
Run each command with `--help` for options.

### Arguments

Selectors are repeatable and combined. `extract` requires `--all` or a selector.

| Argument | Purpose |
| --- | --- |
| `--all` | Extract all selected assets. |
| `--name NAME` | Exact, case-sensitive shader name. |
| `--match GLOB` | Case-sensitive shader-name pattern. |
| `--index N` | One-based row in the selected kind. |
| `--id ID` | Select one source occurrence from `list`. |
| `--content-id ID` | Select all identical content aliases from `list`. |
| `-o, --out DIR` | Extraction output directory. |
| `--kind all\|graphics\|compute` | Asset kind; default: `all`. |
| `--flat-shaders` | Put graphics `.shader` files directly in the output directory. |
| `--high-level` | Reconstruct and verify supported high-level Unity ShaderLab; retain verified low-level fallback. |
| `--compile-profile FILE` | Captured Unity profile required by `--high-level`. |
| `--project-root DIR` | Compiler project directory; default: `.`. |
| `--includes DIR` | Additional compiler headers. |
| `--lift-max-compiles N` | Compile limit per shader; default: `4096`. |
| `--lift-timeout-ms N` | Verification deadline per shader; default: `120000`. |
| `--compute-source-candidate` | Emit unverified `.compute` candidates; requires `--kind compute`. |
| `--materials` | Export linked materials and supported texture dependencies. |
| `--schema-registry FILE` | TypeTree schema override. |
| `--format table\|json` | Report format. |
| `--sources` | Include the source/TypeTree ledger in table output. |
| `--report FILE` | Save the report without overwriting an existing file. |
| `--recursive`, `--no-recursive` | Control directory recursion; default: recursive. |
| `-h, --help` | Show command help. |

## Unity verification

The optional compiler backend supports macOS and Unity 2021.3.35f1:

```sh
cmake -S . -B build -DDXBCSANDBOX_BUILD_UNITY_COMPILER=ON
cmake --build build --config Release --parallel
DXBC_UNITY_APP=/path/to/Unity.app build/dxbc-compiler-session
```

Extract high-level Unity ShaderLab with a captured profile:

```sh
DXBC_UNITY_APP=/path/to/Unity.app build/dxbc-sandbox extract /path/to/assets \
  --kind graphics --all --high-level --compile-profile captured.profile \
  --project-root /path/to/project --includes /path/to/headers \
  --out recovered --format json
```

Omit `--includes` when no additional headers are needed. Compiler checks compare
complete DXBC containers. See [VERIFICATION.md](VERIFICATION.md) for their scope.

## License

[GPL-3.0-only](LICENSE). UnityCommon has its own license and third-party notices.
