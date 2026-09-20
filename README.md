Special thanks to [@nesrak1](https://github.com/nesrak1) for the idea of [USCSandbox](https://github.com/nesrak1/USCSandbox) which was a great foundation to start off.

DXBCSandbox is an experiment utilizing Codex, ChatGPT, and OpenAI heavily.

# DXBCSandbox

C11 tools for inspecting Unity shader assets, decoding DXBC, and checking
recompiled output. Currently produces **low-level HLSL**, with ShaderLab
structure reconstructed from supported serialized metadata. High-level Unity
shader reconstruction is in progress.

## Build and test

Requires CMake 3.10+ and a C11 compiler. Initialize the UnityCommon submodule:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The default build does not need Unity. Use `-DBUILD_TESTING=OFF` for tools only.
With CMake older than 3.20, run `ctest` inside `build` instead of `--test-dir`.
An existing checkout can be selected with
`-DDXBCSANDBOX_UNITY_COMMON_DIR=/path/to/UnityCommon`.

## Usage

```sh
build/dxbc-sandbox list /path/to/assets --format table
build/dxbc-sandbox extract /path/to/assets --all --out recovered
build/dxbc-sandbox extract /path/to/assets --name 'Unlit/Color' \
  --materials --out recovered
cmake --install build --prefix /path/to/install
```

Inputs may be UnityFS bundles, SerializedFiles, or player folders. Tree-free
inputs can use `--schema-registry schemas/unity-2021.3-player-shader.registry`.
Each command supports `--help`; reports distinguish unsupported data from
successful extraction. Additional tools inspect schemas, compile profiles,
player capabilities, OraclePacks, and release-shader certificates.

## Optional Unity verification

The compiler protocol backend currently supports macOS and Unity 2021.3.35f1.
A private Editor build is supported; executable fingerprints identify the
selected toolchain and cache entries, without comparison to a stock release.

```sh
cmake -S . -B build-unity -DCMAKE_BUILD_TYPE=Debug \
  -DDXBCSANDBOX_BUILD_UNITY_COMPILER=ON \
  -DDXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS=ON
cmake --build build-unity --parallel
DXBC_UNITY_APP=/path/to/Unity.app \
  ctest --test-dir build-unity --output-on-failure
DXBC_UNITY_APP=/path/to/Unity.app build-unity/dxbc-compiler-session
DXBC_UNITY_APP=/path/to/Unity.app build-unity/unity_golden_verifier
build-unity/dxbc-sandbox extract /path/to/assets --kind graphics --all \
  --high-level --compile-profile captured.profile --out recovered --format json
```

Select Unity with `DXBC_UNITY_CONTENTS_PATH`, `DXBC_UNITY_APP`, or
`UNITY_EDITOR_PATH`. Additional package headers must be supplied through
`--includes` or a local `shader_includes/` folder under the project root.
Unity binaries and copied package headers are not included. Compiler verification
captures bounded literal include dependencies, including inactive branches and
external headers. Macro-expanded include names and implicit Surface Shader
generation currently return `include-authority-unavailable`.

`unity_golden_verifier --high-level --report results.jsonl` tests an opt-in
float4 expression lift for bounded vertex/fragment programs and structured
conditionals, counted loops, and repeated pure multiplication helpers.
Each accepted candidate must reproduce its entire target DXBC container;
unsupported candidates retain verified low-level output. Extraction's
`--high-level` checks every local D3D11 pass/state/tier under the supplied
profile and records request hashes and instruction spans. Failed baselines
produce no Shader. A bounded packed-UV helper can use the selected Unity include
after checking its actual definitions and exact DXBC for every selected variant.
These checks do not certify import, external dependencies,
player/runtime selection, or visual equivalence.

The import, bundle, and finite-visual gate commands accept an explicit Editor
path and use isolated projects. Their installed C# bridges live in
`share/dxbc-sandbox/unity/Editor`. `quick_test.sh --help` describes bundle-wide
verification with explicit compile-profile authority. Cached or captured
OraclePack results are valid only for their recorded inputs and toolchain.

With compiler and bundle support enabled, the `unity_shader_contract` C API
coordinates accepted-source compilation, isolated import, release comparisons,
and captured player metadata under one subject. It requests every D3D11 logical
plane. Optional authenticated native capture enables a conservative closed
selection-congruence check. A logical certificate applies only when all eleven
planes pass under the [defined runtime conditions](VERIFICATION.md); missing
native authority leaves selection unavailable.

The `unity_native_runtime` API retrieves authenticated D3D11Validation
observations through its headless SSH worker. It binds the released bundles and
player package and compares complete bound stages and raw pixels for six paired
fixtures. Those finite observations remain separate from runtime-selection proof.

## Limits and layout

- Parsing targets explicitly supported Unity 2021.3 schemas, not every version.
- HLSL/ShaderLab emission is experimental. Unsupported variants fail closed;
  hull and domain emission remain unavailable, and geometry support is limited.
- Readable output is a presentation mode. Recompilation checks require exact
  mode and matching compiler, platform, keyword, and include authority.
- Matching DXBC or a finite visual test does not prove universal visual equality
  or recovery of original source. Regression fixtures are a finite sample.
- `include/`, `src/`, `resources/`, `schemas/`, and `tests/` hold the public API,
  implementation, Editor bridges, schemas, and fixtures. Build options and
  target groups are in `cmake/`; live probes are in `tests/probes/`.
  Version-specific ABI diagnostics in `tests/abi/` are disabled by default.

## License

GNU General Public License v3.0 only; see [LICENSE](LICENSE).
UnityCommon carries its own license and third-party notices.
