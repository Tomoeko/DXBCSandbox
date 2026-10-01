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

## Unity verification

The optional compiler backend supports macOS and Unity 2021.3.35f1:

```sh
cmake -S . -B build -DDXBCSANDBOX_BUILD_UNITY_COMPILER=ON
cmake --build build --config Release --parallel
DXBC_UNITY_APP=/path/to/Unity.app build/dxbc-compiler-session
```

High-level extraction requires a captured compile profile. Compiler checks compare
complete DXBC containers; matching bytecode does not prove original source recovery
or universal visual equivalence. See [VERIFICATION.md](VERIFICATION.md) for supported
checks and their scope.

## License and credits

[GPL-3.0-only](LICENSE). UnityCommon has its own license and third-party notices.

Thanks to [nesrak1](https://github.com/nesrak1) for
[USCSandbox](https://github.com/nesrak1/USCSandbox), the project's starting point.
This experiment uses Codex, ChatGPT, and OpenAI.
