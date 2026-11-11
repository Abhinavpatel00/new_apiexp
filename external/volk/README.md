# Volk dependency

Vendored Volk from the Vulkan SDK `vulkan-sdk-1.4.357.0` tag:

- Upstream: https://github.com/zeux/volk
- `volk.h` / `volk.c`: MIT licensed; see `LICENSE.md`.
- Built against the repository's vendored Vulkan-Headers (header version 362).

The v2 backend uses Volk's global dispatch: `volkInitialize()` loads the Vulkan
loader at runtime, `volkLoadInstance()` resolves instance commands, and
`volkLoadDevice()` resolves device commands. Backend call sites use the normal
`vkFoo(...)` names; they do not maintain local `PFN_vk*` tables or call
`vkGetDeviceProcAddr` themselves. The private integration header includes
`volk.h` before the API header, so `VK_NO_PROTOTYPES` is set before Vulkan
prototypes can be declared and the `vkFoo` names bind to Volk's dispatch pointers.

On Unix the executable still needs the Vulkan loader installed at runtime.
The build links `-ldl` for Volk's dynamic loader, not `-lvulkan`.
