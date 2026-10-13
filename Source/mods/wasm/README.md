# Wasm mod host

This directory will hold the WebAssembly mod host. Mods are meant to run in the
embedded wasmi interpreter, the same way on desktop and in the browser. The host
is not finished and is off by default (`DEVILUTIONX_ENABLE_WASM_MOD_HOST`).

`phase_contract.h` defines the journal a mod phase works on: an authoritative
snapshot, the ordered events of the tick, and the commands mods send back. The
host owns these bytes and guests only ever get copies.

The host follows a few rules:

- Each runtime owns the resources of one module. The host owns instances,
  quotas, ABI checks and capabilities. A failed initialization releases
  everything it acquired, and cleanup is safe to repeat.
- Guests link only the imports they are allowed. They get fixed-width
  serialized values and generation-checked handles, never engine pointers,
  files, network, clock, Lua or GPU access.
- Pointer and length ranges are checked against the current linear memory
  before use. Inputs are copied into host memory before any effect. Gameplay
  modules may not grow memory or tables.
- Commands, mod state and RNG are staged for the whole phase and validated
  before a deterministic commit. A failure discards the phase and quarantines
  the guests that changed state.
- Activation, execution and resource limits apply separately, including
  startup and serialization.

`../mod_identity.h` provides the versioned manifest and its capability strings.
Those strings do not by themselves allow guest code to run.
