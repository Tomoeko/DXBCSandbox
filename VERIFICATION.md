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

Parsed immediate constant buffers retain their raw declaration identity and a
separate copy of every payload DWORD. `usil_icb_declaration_is_valid()` checks
that mutable projection against its retained owner, including parsed absence.
It does not authenticate an original target or admit high-level indexed arrays.
Generic caller-built constant-buffer presentation remains a separate path.

A high-level candidate may establish independent generated-domain compiler
evidence when ordinary source emission is unsupported. It still requires a
complete instruction source map, pinned compiler/include/profile authority,
valid preprocessing identity, complete pass/state/tier coverage, exact full
containers and the existing reflected-binding checks. The raw emission failure
remains in the report and never becomes a fallback. Compiler, byte, provenance,
authority, cancellation and budget failures in the ordinary baseline stop this
independent path. Acceptance does not enlarge the logical or native scope.

The high-level source-quality ledger is independent of these certificates. Its
stage-entry observations retain AST/provenance residuals, missing source units,
and complete emission coverage. The extraction lift report renders these facts
beside exact compiler results; it does not classify unobserved enclosing
ShaderLab state or dependencies as clean. The bounded compute source candidate
API likewise returns unverified source and owned per-kernel evidence. Controlled
compiler equality does not supply the missing generic compute, import or native
producers or enlarge the closed whole-shader scope above.

Private HULL coverage records configuration, patch helper and entry-point units
separately, including owned expression trees, source spans and typed phase or
signature owners for structural values. It preserves the normal emitted source
and classification. Body and declaration obligations remain open until a separate
factory replays immutable target bytes and current metadata through the complete
ShaderLab producer. Local capture integrity does not supply that receipt.

Native compute preprocessing and kernel responses remain compiler observations.
Their canonical identities retain every ordered request field and current
compiler/include authority. Malformed or over-budget responses discard partial
results and recycle the process; an uncaptured returned include clears identity
and invalidates the compiler's cached content. The preprocessing protocol has no
native success Boolean, and a cache-only miss provides no compiler result.
Decoded native `ComputeShaderBinary` resource/group records and a selected
complete DXBC comparison do not authenticate missing player ClassID 72 fields,
enumerate the whole keyword domain, prove memory semantics or validate dispatch
and readback. These producers do not change any required logical plane or the
closed certificate scope.

Portable compute domain planning preserves ordered native family choices while
retaining their global/local scope. Its enumeration order is independent of
serialized ClassID 72 order; selected-domain evaluation must establish a unique
kernel/keyword mapping and retain every requested target. Comparison reparses
raw native payloads, checks full containers and declared groups, and compares
all common resource fields. Unsupported buffer selection cannot become a
metadata match. The manual evaluator retains the selected SerializedFile
metadata and anchored input through its final request and source lease checks.
These observations leave ClassID 72 production, original compiler controls,
import, memory semantics and physical execution open.

Scalar HULL source admission uses the existing cbuffer layout and declaration
inventory. A complete named 16-byte buffer at b0 must authorize the float field
at byte zero; a common partial field requires its current complete shell.
Unmodified fork-ID copies retain their actual singleton SSA lane. Natural
`min`/`max` expressions remain in instruction operand order. The optional manual
scalar fixture probe supplies an explicit controlled API layout and checks it
against native reflection; this supplies no player metadata authority. A clean
source result and successful warm replay do not turn a failed cold complete
container comparison into an exact match. The bounded final-factor rule retains
the parsed maximum-declaration coordinate, exact literal bits, original MIN
operand order, phase and sole output consumer. Its assignment and attribute
spans replay against the current decoded owners; callback mutation guards cover
the admitted stage and current/common metadata. These provenance guards do not
replace immutable complete-target capture or the compiler comparator.

The manual `catalog_dependencies_probe` reads one released input of at most
1 MiB in place. It observes empty dependency tables, then independently replays
the ordinary owned source inventory. Source, receipt, historical-quality and
root mutations must fail, and restored observations must replay. It exports
compact summaries only. Nonempty graphs remain unavailable; this factory and
probe do not close any existing source-quality gap or invoke a compiler or
runtime gate.

The optional `--compute-source-candidate` extraction publishes candidate source and
evidence as distinct members of the existing compute package transaction. It never
changes the original binary manifest or its source authority. Generation,
publication, source quality and requested verification remain separate report
fields. A missing candidate returns command failure even when the binary package
was successfully exported; unavailable checks cannot become successful ones.
