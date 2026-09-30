# Controlled high-level evaluation fixtures

These authored sources are evaluation references and compiler inputs. A
reconstruction run receives the released bundle and admitted metadata; it must
not read these sources, their filenames, or `manifest.json` to choose code.
Shader and binding names identify cases for evaluation only.

`manifest.json` records the frozen sources, stage families, keyword domains, and
negative controls. This is a growing subset of the acceptance matrix. Source
availability or successful compilation does not establish clean reconstruction,
exact recompilation, complete dependency authority, or physical D3D11 acceptance.
The existing golden fixtures and their denominators remain separate.

To capture a controlled released bundle with the selected private Unity
2021.3.35f1 toolchain, create an isolated project, copy the `.shader` and
`.compute` files into `Assets/Fixtures`, and copy `CaptureFixtureBundle.cs` into
`Assets/Editor`. Set `DXBC_FIXTURE_BUNDLE_DIR` to a new absolute output directory.
Launch the selected Editor with `-batchmode -nographics -quit`, the isolated
`-projectPath`, `-executeMethod CaptureFixtureBundle.Build`, and an explicit
`-logFile`. On Wine, supply paths in the selected prefix's Windows syntax.
Keep binaries, raw logs, toolchain fingerprints, and request-specific include
evidence in private local reports. Check the complete Editor log for compute
compilation errors as well as the helper's shader error check.

Use `dxbc-sandbox list BUNDLE --format json` to retain the released inventory.
`golden_target_cli extract --shader NAME --subshader N --pass N --output FILE
BUNDLE` extracts complete graphics targets for explicit pass coordinates.
`dxbc-sandbox extract BUNDLE --kind compute --all --out DIR --format json`
preserves each captured compute kernel binary and its metadata. Those commands
use released artifacts and do not accept an authored reference source.

The renamed reference fixtures change resource layout and declaration order as
well as local spelling. Their reconstructed binding names must follow metadata;
whole-container equality with the unrenamed fixture is not required. The changed
color and length-normalized projection cases are different programs, so their
fragment targets must differ from the corresponding baseline at matching tiers
and keywords. `REVERSE_EMISSION` and `REVERSE_TOPOLOGY` retain real stage-contract
differences. The modified `Reduce` kernel in `resource_kernels_changed.compute`
is a compilation and analysis negative only; do not dispatch it.

`ret_only.compute` is a compiler pruning control. It pairs an empty kernel with
an empty kernel that declares thread and group IDs, and includes an unused
shared-memory declaration. The two keyword states change the declared thread
group size. Inspect the captured DXBC declarations to establish which inputs
and storage survive compilation; do not infer them from the authored reference.
Capture added fixtures in a separate bundle so an earlier frozen denominator
and its original artifact digests remain reproducible.

`geometry_control_flow.shader` forces conditional amplification and a runtime
loop bounded by a material value. Each iteration emits a triangle and restarts
the strip; `REVERSE_WINDING` changes the observable append order. A captured
loop or emission contract remains pending source reconstruction until the
generated stage independently passes its readability and exact compiler gates.

`barrier_forms.compute` separates unused barrier calls from calls anchored by
shared or device memory operations. Dispatch exactly one group and bind at
least eight uint elements for each referenced buffer. Its keyword states change
thread layout and device coherence. `barrier_group_atomic.compute` keeps a group
memory fence between atomic shared-memory accesses without introducing a data
race; intermediate observations may depend on execution order. Inspect retained
SYNC flags after compilation. A statement substitution comparison validates
only that emitted intrinsic and its order, while the surrounding authored memory
operations remain evaluation inputs.

The matrix fixtures compare generic transforms under matrix storage, operand
orientation, precision, array layout, and external-use keyword controls.
`matrix_transforms.shader` uses loose globals; the explicit version uses a named
constant buffer. `matrix_square_projections.shader` retains full square matrix
metadata while selecting a three-component result. A captured matrix name or
three-row layout does not by itself authorize its original source dimensions,
orientation, or coordinate-space purpose. Unsupported metadata shapes remain
negative controls even when their physical arithmetic pattern is recognized.
`matrix_vector4.shader` isolates a full four-component result so the same
metadata and dataflow guards can be evaluated independently of three-component
projection support.
