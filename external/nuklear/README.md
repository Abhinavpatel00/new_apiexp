# Nuklear (vendored)

Single-header immediate-mode GUI, public domain.
Upstream: https://github.com/Immediate-Mode-UI/Nuklear (v4.13.4, `nuklear.h` master)

Vendored as one file (`nuklear.h`, 1.19 MB) so demos need no submodule.
The Vulkan backend in `demos/common/nk_vk.{h,c}` is our own, built on
`vk.h`'s bindless SSBO pattern (see `vk.h` §17) — the upstream
`demo/glfw_vulkan/nuklear_glfw_vulkan.h` needs raw `VkPhysicalDevice` /
`VkQueue` / swapchain `VkImageView`s that `vk.h` deliberately hides.

Update: re-download `nuklear.h` from upstream master and replace the file.
