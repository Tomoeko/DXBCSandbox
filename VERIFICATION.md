# Verification scope

DXBCSandbox distinguishes emitted source, exact released bytecode, conditional
whole-shader equivalence, and finite render observations. Original source identity
and universal visual equality are never inferred from these results.

## Closed D3D11 selection contract

`unity_shader_contract_capture()` requests all eleven D3D11 logical planes.
The runtime-selection producer admits only the resource-free vertex/fragment
scope accepted by `unity_shader_dependency_closure()`. Properties, asset/PPtr
references, fallback, UsePass, GrabPass, instancing, pipeline tags, arbitrary
dynamic state and resource bindings currently prevent this closed certificate.
Other APIs can inspect or emit broader inputs without certifying this scope.

An accepted source must actually compile and pass ordered generated-domain,
complete Release DXBC, diagnostics and binding checks. The same source must
then import in an isolated selected Editor and produce the compared candidate
bundle. Structure, render state, released object, player profile and dependencies
have separate producers. None of these planes is optional for a logical claim.

Selection congruence additionally requires independently captured target and
candidate payloads and schemas to match exactly, including the complete ordered
parsed form and independently inspected dependency inventories. No canonical
sorting, compression normalization or byte substitution is used by this proof.
This conservative equality preserves subshader/pass order, keyword scopes and
masks, stage/tier/requirement rows, aliases, archive indices and program bytes.
It may reject representations that are semantically equivalent.

The runtime contract returned by `unity_shaderlab_lift_runtime_conditions()` is
included verbatim in the subject's versioned scope digest. Its preconditions are:

- The captured D3D11 player and device environment remain the same.
- Defined vertex/fog inputs, keyword values and mappings, tier, capabilities,
  quality, LOD, requested passes and render state are identical.
- Selection starts with corresponding caches and unsupported states. The same
  ordered requests encounter identical blob-load and device-creation outcomes.
- Code and dependencies remain unchanged. Shader replacement, external keyword
  or variant extensions, and callbacks that distinguish object identity do not
  intervene during the requests.

For any request under those conditions, identical ordered inputs to the same
engine preserve its selection decision. In the audited selector, equal scores
retain the first row and unsupported states are skipped. This argument applies
to every corresponding eligibility set, including no supported rows; compiler
acceptance is not a claim that all rows are eligible. Corresponding cache hits,
loads and failure transitions preserve the relation for the next request, so
the argument extends to the defined request history. Equality means corresponding
serialized choices and behavior, not equality of process addresses.

These are conditions of a logical claim. The certificate does not assert that
arbitrary applications satisfy them, that failures never occur, or that extension
inactivity was continuously observed. An application that changes these inputs
or permits identity-dependent extension behavior is outside this scope.

## Native observations

The optional native capture invokes the selected D3D11Validation client in
isolated Python, retrieves authenticated jobs over SSH, and revalidates its held
tool/configuration inputs. It binds the actual released bundles, every player
package member, the protected deployment, device environment and worker epoch.
The selected Python/SSH installation is trusted tooling; this does not establish
complete interpreter or loaded-module closure.

The capture compares full bound vertex/fragment containers and raw pixels for
six paired fixtures. Samples of extension counters apply only at their recorded
times. Identical program bytes cannot distinguish aliases that share those bytes.
Neither observation supplies the logical congruence argument above.

Missing native authority leaves runtime selection `UNAVAILABLE`; changed bindings
cannot construct evidence. Strict shader comparison produces `FAIL` for admitted
unequal data. Unsupported structure or dependencies remain `UNAVAILABLE`.
The coordinator does not promote finite native observations to the separate
finite-input/pixel certificate planes.

Portable tests cover framing, every byte truncation, duplicate observations,
changed stage/pixel/member data, exhaustive small eligibility sets, and missing
authority. Optional live tests exercise actual compile/import/native capture and
reject altered subject/runtime bindings. Sanitizer and native executions remain
separate checks; none of their case counts measures universal shader coverage.

## Expansion gates

The closed whole-shader scope remains vertex/fragment on the selected Unity
2021.3 Windows D3D11 player. A parser or emitter accepting another stage does not
expand this contract. Each extension needs its own supported-input definition,
negative fixtures, compiler/runtime authority and independent release evidence
before changing the admitted scope or required mask.

| Separate milestone | Required additional acceptance |
| --- | --- |
| Geometry | Primitive topology, adjacency, stream/output ordering, limits and linked stage signatures; exact complete containers and physical D3D11 execution |
| Hull/domain | Patch/control-point ABI, tessellation factors, partitioning and topology, phase ordering and all linked stages; complete tier/domain proof and execution |
| Compute | Dispatch/group dimensions, shared memory, barriers, atomics, UAV ordering and memory results; a compute-specific artifact and dependency contract |
| GLCore | Independent precision/overload, compiler, linking, interface and driver authority; preserve the precision-collision negative demonstrating that D3D11 equality is insufficient |
| Other backends | Their own instruction semantics, resource ABI, device/environment capture, release boundary and runtime selection evidence |

Each milestone must retain the original requested corpus denominators, preserve
previously certified cases, report fallback/unsupported outcomes, and pass cold
and warm cache checks, toolchain drift, cancellation, budget and malformed-input
tests. Source readability and byte-match coverage are reported independently.
No stage/backend is admitted by changing a label or dropping an unavailable plane.
