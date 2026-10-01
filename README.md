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

List shader names:

```sh
build/dxbc-sandbox list /path/to/assets --kind graphics
```

Extract one shader by its exact name:

```sh
# Low-level Unity ShaderLab
build/dxbc-sandbox extract /path/to/assets --kind graphics --name "Unlit/SingleColor" --out recovered

# High-level Unity ShaderLab
build/dxbc-sandbox extract /path/to/assets --kind graphics --name "Unlit/SingleColor" --high-level --out recovered
```

Extract all shaders:

```sh
# Low-level Unity ShaderLab
build/dxbc-sandbox extract /path/to/assets --kind graphics --all --out recovered

# High-level Unity ShaderLab
build/dxbc-sandbox extract /path/to/assets --kind graphics --all --high-level --out recovered
```

Extraction reads game assets directly. Accepts UnityFS bundles, SerializedFiles,
and player folders. Names are case-sensitive; use a name from the listing.

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
| `--high-level` | Reconstruct supported high-level Unity ShaderLab from game assets. |
| `--compile-profile FILE` | Optional compiler verification profile. |
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

## Optional verification

Source extraction does not require Unity, a project, headers, or a compile profile.
Compiler verification is optional and currently supports macOS with Unity
2021.3.35f1. See [VERIFICATION.md](VERIFICATION.md) for setup and scope.

## License

[GPL-3.0-only](LICENSE). UnityCommon has its own license and third-party notices.
