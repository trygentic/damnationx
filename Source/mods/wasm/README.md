# Runtime decision (2026-09-27)

The mod runtime is the embedded **wasmi** interpreter on desktop and in the browser. There is
no Wasmtime adapter; references to it below and in the design history are superseded (design
§7.1). Wasmtime would only return as an optional accelerator behind the same `dvl_wasm_*`
boundary if mod authors hit the interpreter's ceiling and ask for it.

# DXM-W01 runtime host integration gate

## DXM-W02 native adapter checkpoint

Native integration is also blocked at plan step 1. This checkout contains no
runtime-neutral implementation in this directory; the DXM-W01 gate below is
still unresolved. Design §7.1's DXM-F03 addendum pins Wasmtime 26.0.0 but leaves
browser linkage and callback evidence pending. Section 7.2's contract boundary
also leaves the shared instrumented gas/logical-depth schedule unresolved.
Do not replace these prerequisites with a private native ABI or runtime fuel.

The dependency configuration now keeps the Wasmtime installation hint out of
fresh Emscripten configurations. It does not download, integrity-verify, link,
or enable Wasmtime. A version string and a user-selected installation directory
are not an integrity-verified dependency.

Resume with the delivered DXM-W01 interface, DXM-F03 linkage evidence, and the
shared validation/gas policy; then select integrity-verified native artifacts
and implement the adapter. Steps 2–7 (lifecycle, containment, trusted cache,
distribution, regressions, and packaged acceptance) have not been executed.
The product-manager-owned Windows/macOS/Linux guest execution, event, save/load,
startup containment, and trusted-cache acceptance checks remain outstanding.

## DXM-W05 validator/instrumenter checkpoint

W05 inspection confirms this directory still contains only this README: no
shared validator, runtime-neutral activation interface, native Wasmtime adapter
or embedded wasmi adapter is available here. The W02 checkpoint above remains
blocked. The design's W03 checkpoint likewise records missing linkage and
adapter evidence. Do not implement a second guessed `dvl_wasm_*` boundary to
make a differential harness appear available.

The W05 contract is now recorded beside design §7.4. F04 wire revision 1
requires rejecting start sections and bulk-memory operations; the older
illustrative design and task's positive start/bulk fixtures cannot override
that allowlist. Its fixed memory/table declarations require explicit equal
minimum/maximum values, but production admission ceilings and decoded-work
limits remain unfrozen. The proposed `dxmod-gas-1` schedule is not an enabled
runtime policy or proof of metering.

Resume step 2 after the prerequisite owners supply the frozen admission policy
and shared activation interfaces. Then implement complete-module validation,
transformation, protected verification, adapter integration and actual paired
fixtures in that order. No permissive partial parser, raw-byte activation path,
native-browser Wasm substitute, or runtime-fuel gameplay schedule was added.
Steps 2–8 remain unimplemented; no validator or cross-runtime test passed in
this work. Product-manager acceptance must supply runnable Wasmtime and
embedded-wasmi boundary checks with artifact identity, gas/depth location,
staged command prefix and zero committed effects on failure, plus browser
linkage evidence. Host codec goldens alone cannot satisfy this gate.

## Host prerequisite details

Status: blocked at the contract checkpoint; no runtime implementation is enabled.

`IModRuntime` and `ModHost` belong here per
`docs/design/webgpu-wasm-modding.md` §3. The existing `../mod_identity.h`
provides versioned manifest capability strings, but does not define a guest ABI
or a simulation phase-boundary interface. Do not treat those strings alone as
authorization to execute guest code.

Before introducing executable contracts, consume the delivered DXM-F04 and
DXM-A03 interfaces rather than inventing a competing ABI. The appended DXM-F04
wire revision 1 in the design supersedes its earlier illustrative ABI:

- The only import is `dxmod_core_1.call(op, length) -> status`; protocol bytes
  occupy the fixed guest scratch range, with a minimum 16-byte header. Empty
  records are header-only, not zero-length buffers. Status values, phases,
  capabilities and generation retirement are defined in §§7.2.1–7.2.2.
- DXM-F04 §7.5.1 explicitly leaves production acceptance blocked on shared
  runtime admission, ownership, capability and failure evidence. Its C/Rust
  golden-byte fixtures are not runtime-host conformance evidence.
- DXM-F03 §7.1 records pinned toolchain inputs but leaves linkage and callback
  measurements pending. The project-owned `dvl_wasm_*` adapter boundary and
  proven embedded-wasmi route must be delivered, not replaced with a guessed
  C ABI or browser-native execution.
- DXM-A03 readiness and the engine phase-boundary interface still require
  verification before integration, including trusted Lua order, whole-phase
  validation/commit, checkpoint recovery, and simulation reentry exclusion.

Inspection at this checkpoint found only `mod_identity.cpp` and
`mod_identity.h` under `Source/mods` before this note was added; a source search
found no `IModRuntime`, `ModHost`, `dvl_wasm_*` or `dxmod-core-1` implementation.
The full design's appended wire revision was read; no runtime linkage or
acceptance test was run. Implementation steps 2–8 remain unexecuted because
step 1 explicitly requires stopping on unresolved prerequisite contracts.

Required ownership and security constraints from design §§3, 7.2–7.4:

- Each runtime exclusively owns one module's instantiate/call/interrupt/destroy
  resources; the host owns instances, quotas, ABI checks and capabilities.
  Partial initialization must release every acquired resource, and repeated
  cleanup must be safe. Initialization cannot mutate gameplay.
- Link only explicitly allowed imports. Guests receive fixed-width serialized
  values and generation-checked identities, never engine pointers, filesystem,
  network, clock, Lua evaluation or GPU access.
- Validate pointer/length ranges without overflow against current linear-memory
  bounds. Copy bounded inputs into host ownership before effects, reacquire and
  validate output destinations after guest calls, and never retain memory views
  across execution. Gameplay modules reject memory/table growth instructions.
- Stage commands, namespaced state and RNG for the entire phase. All guests
  observe pre-commit state. Validate everything before deterministic commit.
  A failure discards the phase and quarantines mutated guests; it does not retry
  their memory or commit earlier guests. Recovery requires a whole committed
  checkpoint. Neither simulation nor another guest may be reentered.
- Apply independent activation/execution/resource limits, including startup and
  serialization. Runtime fuel is containment, not the shared deterministic gas
  schedule; do not invent that schedule or equate Wasmtime and wasmi fuel.

Once these prerequisites are delivered, implement the host and fake-runtime
regressions before wiring engine events. Real Wasmtime and embedded browser
wasmi lifecycle/security checks remain required for acceptance; a fake runtime
or browser-native guest execution cannot substitute for that evidence.
