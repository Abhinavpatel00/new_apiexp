# Clean Vulkan library: refactor decisions

## Agreed scope

- **First-pass API:** core device/GPU setup, buffers, textures, samplers, pipelines, uploads/readbacks, frame lifecycle, and minimal draw/compute recording.
- **Compatibility:** breaking redesign is acceptable; do not spend effort on v2 source-compatibility shims.
- **`mu_lib` use:** selective adoption. Use `mu_id_pool` for bindless descriptor slots if its invariants fit; keep fixed resource tables and generation handles unless the actual workload justifies replacing them. Do not import a sparse set or general storage container speculatively.
- **Repository target:** one public `src/vk.h` and one implementation `src/vk.c`; Volk and Vulkan-Headers remain under `external/`. Keep games and legacy renderer out.

## Implementation constraints

- Volk remains the only Vulkan dispatch mechanism. Include it before Vulkan headers in the implementation; do not add local PFN tables or a direct `-lvulkan` link. Keep runtime loading dynamic.
- Keep the v2 code in the first commit as the reference baseline. The submodule, this research/decision record, and the refactor belong in the next, reviewable commit.
- Keep the existing end-to-end Lavapipe test, warning-clean compiler flags, and API/section checks useful through the rename.
- Use IDs/handles for durable references, keep transient allocations off the per-frame heap, and do not make performance claims without measurements.

## `mu_id_pool` fit and lifetime caveat

The resource tables are fixed and modest, so wholesale replacement with `mu_bulk_storage` or `mu_sparse_set` has no demonstrated value. Descriptor slots are different: the texture path currently scans live table entries to find a free slot, and the sampler path increments modulo capacity without checking whether that descriptor slot is still live. A pool is a direct fit for slot allocation/reuse.

Descriptor slots must not become reusable merely because the CPU handle was destroyed: recorded/submitted GPU work may still reference the old descriptor. Return a slot only after the corresponding timeline retirement. On failed resource creation, return an unsubmitted slot immediately. Test exhaustion, uniqueness, release/reuse, and delayed reuse separately from resource-handle generations.

## Review/tool results

- `mu/shaders/tests/validate_math.py` ran successfully; all numerical checks passed.
- `mu/shaders/tests/measure.py` was reviewed. It requires `slangc` (and optionally `spirv-dis`); neither tool is installed in this workspace, so emitted-SPIR-V measurements could not be reproduced here. The shader source remains a submodule, not code copied into the C API.
- The game-engine DOD skill is renderer/frame-lifetime reference only; its simulation/ECS/game architecture is out of scope. SIMD and multithreading remain out of scope absent a profiled CPU bottleneck or an explicit threading goal.
