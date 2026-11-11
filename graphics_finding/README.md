# Graphics findings and research

This folder preserves useful design, optimization, and implementation research
from the previous working tree for the clean Vulkan library.

- `docs/`: retained API/design notes, rewrite plan, pipeline analysis, and
  implementation/skills audit snapshots. Historical v2-specific findings are
  reference material, not a mandate to preserve the old API.
- `skills/`: local Vulkan and shader optimization references; source folder
  names are retained.
- `mu_lib-review.md`: review of the pinned upstream submodule's guidance,
  structures, skills, and Slang shader library.
- `docs/refactor-decisions.md`: the agreed first-pass API scope and the
  structure-adoption/lifetime decisions for this refactor.
- `tools/layer_check_v2_legacy.py`: intentionally retained historical checker;
  it assumes the omitted legacy `shaders/mu/dispatch.slang` tree and is not a
  validator for the clean library.

Use the documents as evidence and context; verify every path and hardware claim
against the current checkout and target workload before carrying it forward.
