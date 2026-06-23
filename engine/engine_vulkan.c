/*
 * engine_vulkan.c — Nebula Vulkan backend for FWK.
 *
 * Phase 5 scope: instance, device, swapchain, clear-to-colour, present.
 * OpenGL path in engine.c is NOT touched.
 *
 * Shader strategy (LOCKED for Phase 5):
 *   SPIR-V is pre-compiled from GLSL and embedded as uint32_t[] arrays.
 *   No runtime shader compilation in this file.
 *   shaderc integration deferred to Phase 6+ (real model rendering).
 *
 * Build flags required:
 *   Linux:   -I engine/vendor -lvulkan -lX11
 *   Windows: -I engine/vendor -lvulkan-1 (or link vulkan-1.lib)
 */

#include "engine_vulkan.h"

/* ── Platform Vulkan surface extension ──────────────────────────────────── */
#ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__)
#  define VK_USE_PLATFORM_XLIB_KHR
#endif

/* Use our bundled Vulkan headers (downloaded from Khronos) */
#include "vendor/vulkan/vulkan.h"

/* GLFW forward decls — FWK bundles GLFW3, so these are always available. */
typedef struct GLFWwindow  GLFWwindow;
typedef struct GLFWmonitor GLFWmonitor;
extern int    glfwInit(void);
extern void   glfwTerminate(void);
extern void   glfwPollEvents(void);
extern int    glfwWindowShouldClose(GLFWwindow* window);
extern void   glfwWindowHint(int hint, int value);
extern void   glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height);
extern GLFWwindow* glfwCreateWindow(int w, int h, const char* title,
                                    GLFWmonitor* monitor, GLFWwindow* share);
extern void   glfwDestroyWindow(GLFWwindow* window);
extern int    glfwVulkanSupported(void);
extern const char** glfwGetRequiredInstanceExtensions(uint32_t* count);
extern int    glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window,
                                      const VkAllocationCallbacks* alloc,
                                      VkSurfaceKHR* surface);
/* Additional GLFW decls needed for Nuklear input */
extern int    glfwGetKey(GLFWwindow* window, int key);
extern int    glfwGetMouseButton(GLFWwindow* window, int button);
extern void   glfwGetCursorPos(GLFWwindow* window, double* xpos, double* ypos);
extern int    glfwGetWindowSize(GLFWwindow* window, int* w, int* h);
extern int    glfwGetInputMode(GLFWwindow* window, int mode);
typedef void (*GLFWcharfun)(GLFWwindow*, unsigned int);
extern GLFWcharfun glfwSetCharCallback(GLFWwindow* window, GLFWcharfun cbfun);
typedef void (*GLFWscrollfun)(GLFWwindow*, double, double);
extern GLFWscrollfun glfwSetScrollCallback(GLFWwindow* window, GLFWscrollfun cbfun);
extern void*  glfwGetWindowUserPointer(GLFWwindow* window);
extern void   glfwSetWindowUserPointer(GLFWwindow* window, void* pointer);
/* GLFW window hint constants needed for Vulkan */
#ifndef GLFW_CLIENT_API
#  define GLFW_CLIENT_API 0x00022001
#  define GLFW_NO_API     0
#  define GLFW_RESIZABLE  0x00020003
#endif
/* GLFW key/action constants for Nuklear input */
#ifndef GLFW_PRESS
#  define GLFW_PRESS    1
#  define GLFW_RELEASE  0
#endif
#ifndef GLFW_KEY_ENTER
#  define GLFW_KEY_BACKSPACE  259
#  define GLFW_KEY_DELETE     261
#  define GLFW_KEY_ENTER      257
#  define GLFW_KEY_TAB        258
#  define GLFW_KEY_UP         265
#  define GLFW_KEY_DOWN       264
#  define GLFW_KEY_LEFT       263
#  define GLFW_KEY_RIGHT      262
#  define GLFW_KEY_HOME       268
#  define GLFW_KEY_END        269
#  define GLFW_KEY_PAGE_UP    266
#  define GLFW_KEY_PAGE_DOWN  267
#  define GLFW_KEY_LEFT_SHIFT    340
#  define GLFW_KEY_RIGHT_SHIFT   344
#  define GLFW_KEY_LEFT_CONTROL  341
#  define GLFW_KEY_RIGHT_CONTROL 345
#  define GLFW_KEY_C  67
#  define GLFW_KEY_V  86
#  define GLFW_KEY_X  88
#  define GLFW_KEY_Z  90
#  define GLFW_MOUSE_BUTTON_LEFT   0
#  define GLFW_MOUSE_BUTTON_MIDDLE 2
#  define GLFW_MOUSE_BUTTON_RIGHT  1
#  define GLFW_CURSOR        0x00033001
#  define GLFW_CURSOR_NORMAL 0x00034001
#endif

/* (Nuklear types stay in engine.c via NK_IMPLEMENTATION — not needed here) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Pre-compiled SPIR-V (Phase 5 clear-screen shaders) ─────────────────── */
/*
 * Source GLSL committed alongside this file in engine/shaders_vulkan/.
 * Re-compile with:  glslc clear.vert -o clear.vert.spv && xxd -i ...
 *
 * Vertex: fullscreen triangle trick (no vertex buffer needed).
 * Fragment: reads clear colour from push constant.
 */

static const uint32_t g_clear_vert_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x00000028u,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0007000Fu,0x00000000u,0x00000004u,0x6E69616Du,0x00000000u,0x00000018u,0x0000001Cu,0x00030003u,
    0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,0x4C474F4Fu,0x70635F45u,0x74735F70u,0x5F656C79u,
    0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,0x00080004u,0x475F4C47u,0x4C474F4Fu,0x6E695F45u,
    0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,0x00040005u,0x00000004u,0x6E69616Du,0x00000000u,
    0x00050005u,0x0000000Cu,0x69736F70u,0x6E6F6974u,0x00000073u,0x00060005u,0x00000016u,0x505F6C67u,
    0x65567265u,0x78657472u,0x00000000u,0x00060006u,0x00000016u,0x00000000u,0x505F6C67u,0x7469736Fu,
    0x006E6F69u,0x00070006u,0x00000016u,0x00000001u,0x505F6C67u,0x746E696Fu,0x657A6953u,0x00000000u,
    0x00070006u,0x00000016u,0x00000002u,0x435F6C67u,0x4470696Cu,0x61747369u,0x0065636Eu,0x00070006u,
    0x00000016u,0x00000003u,0x435F6C67u,0x446C6C75u,0x61747369u,0x0065636Eu,0x00030005u,0x00000018u,
    0x00000000u,0x00060005u,0x0000001Cu,0x565F6C67u,0x65747265u,0x646E4978u,0x00007865u,0x00030047u,
    0x00000016u,0x00000002u,0x00050048u,0x00000016u,0x00000000u,0x0000000Bu,0x00000000u,0x00050048u,
    0x00000016u,0x00000001u,0x0000000Bu,0x00000001u,0x00050048u,0x00000016u,0x00000002u,0x0000000Bu,
    0x00000003u,0x00050048u,0x00000016u,0x00000003u,0x0000000Bu,0x00000004u,0x00040047u,0x0000001Cu,
    0x0000000Bu,0x0000002Au,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00030016u,
    0x00000006u,0x00000020u,0x00040017u,0x00000007u,0x00000006u,0x00000002u,0x00040015u,0x00000008u,
    0x00000020u,0x00000000u,0x0004002Bu,0x00000008u,0x00000009u,0x00000003u,0x0004001Cu,0x0000000Au,
    0x00000007u,0x00000009u,0x00040020u,0x0000000Bu,0x00000006u,0x0000000Au,0x0004003Bu,0x0000000Bu,
    0x0000000Cu,0x00000006u,0x0004002Bu,0x00000006u,0x0000000Du,0xBF800000u,0x0005002Cu,0x00000007u,
    0x0000000Eu,0x0000000Du,0x0000000Du,0x0004002Bu,0x00000006u,0x0000000Fu,0x40400000u,0x0005002Cu,
    0x00000007u,0x00000010u,0x0000000Fu,0x0000000Du,0x0005002Cu,0x00000007u,0x00000011u,0x0000000Du,
    0x0000000Fu,0x0006002Cu,0x0000000Au,0x00000012u,0x0000000Eu,0x00000010u,0x00000011u,0x00040017u,
    0x00000013u,0x00000006u,0x00000004u,0x0004002Bu,0x00000008u,0x00000014u,0x00000001u,0x0004001Cu,
    0x00000015u,0x00000006u,0x00000014u,0x0006001Eu,0x00000016u,0x00000013u,0x00000006u,0x00000015u,
    0x00000015u,0x00040020u,0x00000017u,0x00000003u,0x00000016u,0x0004003Bu,0x00000017u,0x00000018u,
    0x00000003u,0x00040015u,0x00000019u,0x00000020u,0x00000001u,0x0004002Bu,0x00000019u,0x0000001Au,
    0x00000000u,0x00040020u,0x0000001Bu,0x00000001u,0x00000019u,0x0004003Bu,0x0000001Bu,0x0000001Cu,
    0x00000001u,0x00040020u,0x0000001Eu,0x00000006u,0x00000007u,0x0004002Bu,0x00000006u,0x00000021u,
    0x00000000u,0x0004002Bu,0x00000006u,0x00000022u,0x3F800000u,0x00040020u,0x00000026u,0x00000003u,
    0x00000013u,0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200F8u,0x00000005u,
    0x0003003Eu,0x0000000Cu,0x00000012u,0x0004003Du,0x00000019u,0x0000001Du,0x0000001Cu,0x00050041u,
    0x0000001Eu,0x0000001Fu,0x0000000Cu,0x0000001Du,0x0004003Du,0x00000007u,0x00000020u,0x0000001Fu,
    0x00050051u,0x00000006u,0x00000023u,0x00000020u,0x00000000u,0x00050051u,0x00000006u,0x00000024u,
    0x00000020u,0x00000001u,0x00070050u,0x00000013u,0x00000025u,0x00000023u,0x00000024u,0x00000021u,
    0x00000022u,0x00050041u,0x00000026u,0x00000027u,0x00000018u,0x0000001Au,0x0003003Eu,0x00000027u,
    0x00000025u,0x000100FDu,0x00010038u,
};
static const size_t g_clear_vert_spv_size = sizeof(g_clear_vert_spv);

static const uint32_t g_clear_frag_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x00000012u,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0006000Fu,0x00000004u,0x00000004u,0x6E69616Du,0x00000000u,0x00000009u,0x00030010u,0x00000004u,
    0x00000007u,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,0x4C474F4Fu,0x70635F45u,
    0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,0x00080004u,0x475F4C47u,
    0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,0x00040005u,0x00000004u,
    0x6E69616Du,0x00000000u,0x00050005u,0x00000009u,0x4374756Fu,0x726F6C6Fu,0x00000000u,0x00050005u,
    0x0000000Au,0x68737550u,0x736E6F43u,0x00000074u,0x00060006u,0x0000000Au,0x00000000u,0x61656C63u,
    0x6C6F4372u,0x0000726Fu,0x00030005u,0x0000000Cu,0x00006370u,0x00040047u,0x00000009u,0x0000001Eu,
    0x00000000u,0x00030047u,0x0000000Au,0x00000002u,0x00050048u,0x0000000Au,0x00000000u,0x00000023u,
    0x00000000u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00030016u,0x00000006u,
    0x00000020u,0x00040017u,0x00000007u,0x00000006u,0x00000004u,0x00040020u,0x00000008u,0x00000003u,
    0x00000007u,0x0004003Bu,0x00000008u,0x00000009u,0x00000003u,0x0003001Eu,0x0000000Au,0x00000007u,
    0x00040020u,0x0000000Bu,0x00000009u,0x0000000Au,0x0004003Bu,0x0000000Bu,0x0000000Cu,0x00000009u,
    0x00040015u,0x0000000Du,0x00000020u,0x00000001u,0x0004002Bu,0x0000000Du,0x0000000Eu,0x00000000u,
    0x00040020u,0x0000000Fu,0x00000009u,0x00000007u,0x00050036u,0x00000002u,0x00000004u,0x00000000u,
    0x00000003u,0x000200F8u,0x00000005u,0x00050041u,0x0000000Fu,0x00000010u,0x0000000Cu,0x0000000Eu,
    0x0004003Du,0x00000007u,0x00000011u,0x00000010u,0x0003003Eu,0x00000009u,0x00000011u,0x000100FDu,
    0x00010038u,
};
static const size_t g_clear_frag_spv_size = sizeof(g_clear_frag_spv);

static const uint32_t g_primitive_vert_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x0000001Fu,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0009000Fu,0x00000000u,0x00000004u,0x6E69616Du,0x00000000u,0x0000000Du,0x00000012u,0x0000001Bu,
    0x0000001Du,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,0x4C474F4Fu,0x70635F45u,
    0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,0x00080004u,0x475F4C47u,
    0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,0x00040005u,0x00000004u,
    0x6E69616Du,0x00000000u,0x00060005u,0x0000000Bu,0x505F6C67u,0x65567265u,0x78657472u,0x00000000u,
    0x00060006u,0x0000000Bu,0x00000000u,0x505F6C67u,0x7469736Fu,0x006E6F69u,0x00070006u,0x0000000Bu,
    0x00000001u,0x505F6C67u,0x746E696Fu,0x657A6953u,0x00000000u,0x00070006u,0x0000000Bu,0x00000002u,
    0x435F6C67u,0x4470696Cu,0x61747369u,0x0065636Eu,0x00070006u,0x0000000Bu,0x00000003u,0x435F6C67u,
    0x446C6C75u,0x61747369u,0x0065636Eu,0x00030005u,0x0000000Du,0x00000000u,0x00040005u,0x00000012u,
    0x6F506E69u,0x00000073u,0x00050005u,0x0000001Bu,0x4374756Fu,0x726F6C6Fu,0x00000000u,0x00040005u,
    0x0000001Du,0x6F436E69u,0x00726F6Cu,0x00030047u,0x0000000Bu,0x00000002u,0x00050048u,0x0000000Bu,
    0x00000000u,0x0000000Bu,0x00000000u,0x00050048u,0x0000000Bu,0x00000001u,0x0000000Bu,0x00000001u,
    0x00050048u,0x0000000Bu,0x00000002u,0x0000000Bu,0x00000003u,0x00050048u,0x0000000Bu,0x00000003u,
    0x0000000Bu,0x00000004u,0x00040047u,0x00000012u,0x0000001Eu,0x00000000u,0x00040047u,0x0000001Bu,
    0x0000001Eu,0x00000000u,0x00040047u,0x0000001Du,0x0000001Eu,0x00000001u,0x00020013u,0x00000002u,
    0x00030021u,0x00000003u,0x00000002u,0x00030016u,0x00000006u,0x00000020u,0x00040017u,0x00000007u,
    0x00000006u,0x00000004u,0x00040015u,0x00000008u,0x00000020u,0x00000000u,0x0004002Bu,0x00000008u,
    0x00000009u,0x00000001u,0x0004001Cu,0x0000000Au,0x00000006u,0x00000009u,0x0006001Eu,0x0000000Bu,
    0x00000007u,0x00000006u,0x0000000Au,0x0000000Au,0x00040020u,0x0000000Cu,0x00000003u,0x0000000Bu,
    0x0004003Bu,0x0000000Cu,0x0000000Du,0x00000003u,0x00040015u,0x0000000Eu,0x00000020u,0x00000001u,
    0x0004002Bu,0x0000000Eu,0x0000000Fu,0x00000000u,0x00040017u,0x00000010u,0x00000006u,0x00000003u,
    0x00040020u,0x00000011u,0x00000001u,0x00000010u,0x0004003Bu,0x00000011u,0x00000012u,0x00000001u,
    0x0004002Bu,0x00000006u,0x00000014u,0x3F800000u,0x00040020u,0x00000019u,0x00000003u,0x00000007u,
    0x0004003Bu,0x00000019u,0x0000001Bu,0x00000003u,0x00040020u,0x0000001Cu,0x00000001u,0x00000007u,
    0x0004003Bu,0x0000001Cu,0x0000001Du,0x00000001u,0x00050036u,0x00000002u,0x00000004u,0x00000000u,
    0x00000003u,0x000200F8u,0x00000005u,0x0004003Du,0x00000010u,0x00000013u,0x00000012u,0x00050051u,
    0x00000006u,0x00000015u,0x00000013u,0x00000000u,0x00050051u,0x00000006u,0x00000016u,0x00000013u,
    0x00000001u,0x00050051u,0x00000006u,0x00000017u,0x00000013u,0x00000002u,0x00070050u,0x00000007u,
    0x00000018u,0x00000015u,0x00000016u,0x00000017u,0x00000014u,0x00050041u,0x00000019u,0x0000001Au,
    0x0000000Du,0x0000000Fu,0x0003003Eu,0x0000001Au,0x00000018u,0x0004003Du,0x00000007u,0x0000001Eu,
    0x0000001Du,0x0003003Eu,0x0000001Bu,0x0000001Eu,0x000100FDu,0x00010038u,
};
static const size_t g_primitive_vert_spv_size = sizeof(g_primitive_vert_spv);

static const uint32_t g_primitive_frag_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x0000000Du,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0007000Fu,0x00000004u,0x00000004u,0x6E69616Du,0x00000000u,0x00000009u,0x0000000Bu,0x00030010u,
    0x00000004u,0x00000007u,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,0x4C474F4Fu,
    0x70635F45u,0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,0x00080004u,
    0x475F4C47u,0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,0x00040005u,
    0x00000004u,0x6E69616Du,0x00000000u,0x00050005u,0x00000009u,0x4374756Fu,0x726F6C6Fu,0x00000000u,
    0x00040005u,0x0000000Bu,0x6F436E69u,0x00726F6Cu,0x00040047u,0x00000009u,0x0000001Eu,0x00000000u,
    0x00040047u,0x0000000Bu,0x0000001Eu,0x00000000u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,
    0x00000002u,0x00030016u,0x00000006u,0x00000020u,0x00040017u,0x00000007u,0x00000006u,0x00000004u,
    0x00040020u,0x00000008u,0x00000003u,0x00000007u,0x0004003Bu,0x00000008u,0x00000009u,0x00000003u,
    0x00040020u,0x0000000Au,0x00000001u,0x00000007u,0x0004003Bu,0x0000000Au,0x0000000Bu,0x00000001u,
    0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200F8u,0x00000005u,0x0004003Du,
    0x00000007u,0x0000000Cu,0x0000000Bu,0x0003003Eu,0x00000009u,0x0000000Cu,0x000100FDu,0x00010038u,
};
static const size_t g_primitive_frag_spv_size = sizeof(g_primitive_frag_spv);

/* Textured vertex shader with GPU-side MVP (mat4 push constant).
 * Input: world-space xyz. Y-flip + z-remap done in shader for Vulkan conventions.
 * Re-compile: glslc textured_mvp.vert -o textured_mvp.vert.spv */
static const uint32_t g_textured_vert_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x00000040u,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x000B000Fu,0x00000000u,0x00000004u,0x6E69616Du,0x00000000u,0x00000015u,0x00000032u,0x00000038u,
    0x0000003Au,0x0000003Cu,0x0000003Eu,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,
    0x4C474F4Fu,0x70635F45u,0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,
    0x00080004u,0x475F4C47u,0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,
    0x00040005u,0x00000004u,0x6E69616Du,0x00000000u,0x00040005u,0x00000009u,0x70696C63u,0x00000000u,
    0x00030005u,0x0000000Bu,0x00004350u,0x00040006u,0x0000000Bu,0x00000000u,0x0070766Du,0x00030005u,
    0x0000000Du,0x00006370u,0x00040005u,0x00000015u,0x705F6E69u,0x0000736Fu,0x00060005u,0x00000030u,
    0x505F6C67u,0x65567265u,0x78657472u,0x00000000u,0x00060006u,0x00000030u,0x00000000u,0x505F6C67u,
    0x7469736Fu,0x006E6F69u,0x00070006u,0x00000030u,0x00000001u,0x505F6C67u,0x746E696Fu,0x657A6953u,
    0x00000000u,0x00070006u,0x00000030u,0x00000002u,0x435F6C67u,0x4470696Cu,0x61747369u,0x0065636Eu,
    0x00070006u,0x00000030u,0x00000003u,0x435F6C67u,0x446C6C75u,0x61747369u,0x0065636Eu,0x00030005u,
    0x00000032u,0x00000000u,0x00040005u,0x00000038u,0x5F74756Fu,0x00007675u,0x00040005u,0x0000003Au,
    0x755F6E69u,0x00000076u,0x00050005u,0x0000003Cu,0x5F74756Fu,0x6F6C6F63u,0x00000072u,0x00050005u,
    0x0000003Eu,0x635F6E69u,0x726F6C6Fu,0x00000000u,0x00030047u,0x0000000Bu,0x00000002u,0x00040048u,
    0x0000000Bu,0x00000000u,0x00000005u,0x00050048u,0x0000000Bu,0x00000000u,0x00000007u,0x00000010u,
    0x00050048u,0x0000000Bu,0x00000000u,0x00000023u,0x00000000u,0x00040047u,0x00000015u,0x0000001Eu,
    0x00000000u,0x00030047u,0x00000030u,0x00000002u,0x00050048u,0x00000030u,0x00000000u,0x0000000Bu,
    0x00000000u,0x00050048u,0x00000030u,0x00000001u,0x0000000Bu,0x00000001u,0x00050048u,0x00000030u,
    0x00000002u,0x0000000Bu,0x00000003u,0x00050048u,0x00000030u,0x00000003u,0x0000000Bu,0x00000004u,
    0x00040047u,0x00000038u,0x0000001Eu,0x00000000u,0x00040047u,0x0000003Au,0x0000001Eu,0x00000001u,
    0x00040047u,0x0000003Cu,0x0000001Eu,0x00000001u,0x00040047u,0x0000003Eu,0x0000001Eu,0x00000002u,
    0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00030016u,0x00000006u,0x00000020u,
    0x00040017u,0x00000007u,0x00000006u,0x00000004u,0x00040020u,0x00000008u,0x00000007u,0x00000007u,
    0x00040018u,0x0000000Au,0x00000007u,0x00000004u,0x0003001Eu,0x0000000Bu,0x0000000Au,0x00040020u,
    0x0000000Cu,0x00000009u,0x0000000Bu,0x0004003Bu,0x0000000Cu,0x0000000Du,0x00000009u,0x00040015u,
    0x0000000Eu,0x00000020u,0x00000001u,0x0004002Bu,0x0000000Eu,0x0000000Fu,0x00000000u,0x00040020u,
    0x00000010u,0x00000009u,0x0000000Au,0x00040017u,0x00000013u,0x00000006u,0x00000003u,0x00040020u,
    0x00000014u,0x00000001u,0x00000013u,0x0004003Bu,0x00000014u,0x00000015u,0x00000001u,0x0004002Bu,
    0x00000006u,0x00000017u,0x3F800000u,0x00040015u,0x0000001Du,0x00000020u,0x00000000u,0x0004002Bu,
    0x0000001Du,0x0000001Eu,0x00000001u,0x00040020u,0x0000001Fu,0x00000007u,0x00000006u,0x0004002Bu,
    0x0000001Du,0x00000024u,0x00000002u,0x0004002Bu,0x00000006u,0x00000027u,0x3F000000u,0x0004002Bu,
    0x0000001Du,0x00000029u,0x00000003u,0x0004001Cu,0x0000002Fu,0x00000006u,0x0000001Eu,0x0006001Eu,
    0x00000030u,0x00000007u,0x00000006u,0x0000002Fu,0x0000002Fu,0x00040020u,0x00000031u,0x00000003u,
    0x00000030u,0x0004003Bu,0x00000031u,0x00000032u,0x00000003u,0x00040020u,0x00000034u,0x00000003u,
    0x00000007u,0x00040017u,0x00000036u,0x00000006u,0x00000002u,0x00040020u,0x00000037u,0x00000003u,
    0x00000036u,0x0004003Bu,0x00000037u,0x00000038u,0x00000003u,0x00040020u,0x00000039u,0x00000001u,
    0x00000036u,0x0004003Bu,0x00000039u,0x0000003Au,0x00000001u,0x0004003Bu,0x00000034u,0x0000003Cu,
    0x00000003u,0x00040020u,0x0000003Du,0x00000001u,0x00000007u,0x0004003Bu,0x0000003Du,0x0000003Eu,
    0x00000001u,0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200F8u,0x00000005u,
    0x0004003Bu,0x00000008u,0x00000009u,0x00000007u,0x00050041u,0x00000010u,0x00000011u,0x0000000Du,
    0x0000000Fu,0x0004003Du,0x0000000Au,0x00000012u,0x00000011u,0x0004003Du,0x00000013u,0x00000016u,
    0x00000015u,0x00050051u,0x00000006u,0x00000018u,0x00000016u,0x00000000u,0x00050051u,0x00000006u,
    0x00000019u,0x00000016u,0x00000001u,0x00050051u,0x00000006u,0x0000001Au,0x00000016u,0x00000002u,
    0x00070050u,0x00000007u,0x0000001Bu,0x00000018u,0x00000019u,0x0000001Au,0x00000017u,0x00050091u,
    0x00000007u,0x0000001Cu,0x00000012u,0x0000001Bu,0x0003003Eu,0x00000009u,0x0000001Cu,0x00050041u,
    0x0000001Fu,0x00000020u,0x00000009u,0x0000001Eu,0x0004003Du,0x00000006u,0x00000021u,0x00000020u,
    0x0004007Fu,0x00000006u,0x00000022u,0x00000021u,0x00050041u,0x0000001Fu,0x00000023u,0x00000009u,
    0x0000001Eu,0x0003003Eu,0x00000023u,0x00000022u,0x00050041u,0x0000001Fu,0x00000025u,0x00000009u,
    0x00000024u,0x0004003Du,0x00000006u,0x00000026u,0x00000025u,0x00050085u,0x00000006u,0x00000028u,
    0x00000026u,0x00000027u,0x00050041u,0x0000001Fu,0x0000002Au,0x00000009u,0x00000029u,0x0004003Du,
    0x00000006u,0x0000002Bu,0x0000002Au,0x00050085u,0x00000006u,0x0000002Cu,0x0000002Bu,0x00000027u,
    0x00050081u,0x00000006u,0x0000002Du,0x00000028u,0x0000002Cu,0x00050041u,0x0000001Fu,0x0000002Eu,
    0x00000009u,0x00000024u,0x0003003Eu,0x0000002Eu,0x0000002Du,0x0004003Du,0x00000007u,0x00000033u,
    0x00000009u,0x00050041u,0x00000034u,0x00000035u,0x00000032u,0x0000000Fu,0x0003003Eu,0x00000035u,
    0x00000033u,0x0004003Du,0x00000036u,0x0000003Bu,0x0000003Au,0x0003003Eu,0x00000038u,0x0000003Bu,
    0x0004003Du,0x00000007u,0x0000003Fu,0x0000003Eu,0x0003003Eu,0x0000003Cu,0x0000003Fu,0x000100FDu,
    0x00010038u,
};
static const size_t g_textured_vert_spv_size = sizeof(g_textured_vert_spv);

static const uint32_t g_textured_frag_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x00000018u,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0008000Fu,0x00000004u,0x00000004u,0x6E69616Du,0x00000000u,0x00000009u,0x00000011u,0x00000015u,
    0x00030010u,0x00000004u,0x00000007u,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,
    0x4C474F4Fu,0x70635F45u,0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,
    0x00080004u,0x475F4C47u,0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,
    0x00040005u,0x00000004u,0x6E69616Du,0x00000000u,0x00050005u,0x00000009u,0x4374756Fu,0x726F6C6Fu,
    0x00000000u,0x00050005u,0x0000000Du,0x78655475u,0x65727574u,0x00000000u,0x00040005u,0x00000011u,
    0x56556E69u,0x00000000u,0x00040005u,0x00000015u,0x6F436E69u,0x00726F6Cu,0x00040047u,0x00000009u,
    0x0000001Eu,0x00000000u,0x00040047u,0x0000000Du,0x00000021u,0x00000000u,0x00040047u,0x0000000Du,
    0x00000022u,0x00000000u,0x00040047u,0x00000011u,0x0000001Eu,0x00000000u,0x00040047u,0x00000015u,
    0x0000001Eu,0x00000001u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00030016u,
    0x00000006u,0x00000020u,0x00040017u,0x00000007u,0x00000006u,0x00000004u,0x00040020u,0x00000008u,
    0x00000003u,0x00000007u,0x0004003Bu,0x00000008u,0x00000009u,0x00000003u,0x00090019u,0x0000000Au,
    0x00000006u,0x00000001u,0x00000000u,0x00000000u,0x00000000u,0x00000001u,0x00000000u,0x0003001Bu,
    0x0000000Bu,0x0000000Au,0x00040020u,0x0000000Cu,0x00000000u,0x0000000Bu,0x0004003Bu,0x0000000Cu,
    0x0000000Du,0x00000000u,0x00040017u,0x0000000Fu,0x00000006u,0x00000002u,0x00040020u,0x00000010u,
    0x00000001u,0x0000000Fu,0x0004003Bu,0x00000010u,0x00000011u,0x00000001u,0x00040020u,0x00000014u,
    0x00000001u,0x00000007u,0x0004003Bu,0x00000014u,0x00000015u,0x00000001u,0x00050036u,0x00000002u,
    0x00000004u,0x00000000u,0x00000003u,0x000200F8u,0x00000005u,0x0004003Du,0x0000000Bu,0x0000000Eu,
    0x0000000Du,0x0004003Du,0x0000000Fu,0x00000012u,0x00000011u,0x00050057u,0x00000007u,0x00000013u,
    0x0000000Eu,0x00000012u,0x0004003Du,0x00000007u,0x00000016u,0x00000015u,0x00050085u,0x00000007u,
    0x00000017u,0x00000013u,0x00000016u,0x0003003Eu,0x00000009u,0x00000017u,0x000100FDu,0x00010038u,
};
static const size_t g_textured_frag_spv_size = sizeof(g_textured_frag_spv);

/* ── Nuklear SPIR-V (2D UI rendering) ───────────────────────────────────────
 * Source GLSL: nk.vert / nk.frag  (position+uv+color → mat4 push constant)
 * Re-compile:  glslc nk.vert -o nk.vert.spv && glslc nk.frag -o nk.frag.spv */
static const uint32_t g_nk_vert_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x0000002Bu,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x000B000Fu,0x00000000u,0x00000004u,0x6E69616Du,0x00000000u,0x00000009u,0x0000000Bu,0x0000000Fu,
    0x00000011u,0x00000018u,0x00000022u,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,
    0x4C474F4Fu,0x70635F45u,0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,
    0x00080004u,0x475F4C47u,0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,
    0x00040005u,0x00000004u,0x6E69616Du,0x00000000u,0x00040005u,0x00000009u,0x5F74756Fu,0x00007675u,
    0x00040005u,0x0000000Bu,0x755F6E69u,0x00000076u,0x00050005u,0x0000000Fu,0x5F74756Fu,0x6F6C6F63u,
    0x00000072u,0x00050005u,0x00000011u,0x635F6E69u,0x726F6C6Fu,0x00000000u,0x00060005u,0x00000016u,
    0x505F6C67u,0x65567265u,0x78657472u,0x00000000u,0x00060006u,0x00000016u,0x00000000u,0x505F6C67u,
    0x7469736Fu,0x006E6F69u,0x00070006u,0x00000016u,0x00000001u,0x505F6C67u,0x746E696Fu,0x657A6953u,
    0x00000000u,0x00070006u,0x00000016u,0x00000002u,0x435F6C67u,0x4470696Cu,0x61747369u,0x0065636Eu,
    0x00070006u,0x00000016u,0x00000003u,0x435F6C67u,0x446C6C75u,0x61747369u,0x0065636Eu,0x00030005u,
    0x00000018u,0x00000000u,0x00030005u,0x0000001Cu,0x00004350u,0x00050006u,0x0000001Cu,0x00000000u,
    0x6A6F7270u,0x00000000u,0x00030005u,0x0000001Eu,0x00006370u,0x00040005u,0x00000022u,0x705F6E69u,
    0x0000736Fu,0x00040047u,0x00000009u,0x0000001Eu,0x00000000u,0x00040047u,0x0000000Bu,0x0000001Eu,
    0x00000001u,0x00040047u,0x0000000Fu,0x0000001Eu,0x00000001u,0x00040047u,0x00000011u,0x0000001Eu,
    0x00000002u,0x00030047u,0x00000016u,0x00000002u,0x00050048u,0x00000016u,0x00000000u,0x0000000Bu,
    0x00000000u,0x00050048u,0x00000016u,0x00000001u,0x0000000Bu,0x00000001u,0x00050048u,0x00000016u,
    0x00000002u,0x0000000Bu,0x00000003u,0x00050048u,0x00000016u,0x00000003u,0x0000000Bu,0x00000004u,
    0x00030047u,0x0000001Cu,0x00000002u,0x00040048u,0x0000001Cu,0x00000000u,0x00000005u,0x00050048u,
    0x0000001Cu,0x00000000u,0x00000007u,0x00000010u,0x00050048u,0x0000001Cu,0x00000000u,0x00000023u,
    0x00000000u,0x00040047u,0x00000022u,0x0000001Eu,0x00000000u,0x00020013u,0x00000002u,0x00030021u,
    0x00000003u,0x00000002u,0x00030016u,0x00000006u,0x00000020u,0x00040017u,0x00000007u,0x00000006u,
    0x00000002u,0x00040020u,0x00000008u,0x00000003u,0x00000007u,0x0004003Bu,0x00000008u,0x00000009u,
    0x00000003u,0x00040020u,0x0000000Au,0x00000001u,0x00000007u,0x0004003Bu,0x0000000Au,0x0000000Bu,
    0x00000001u,0x00040017u,0x0000000Du,0x00000006u,0x00000004u,0x00040020u,0x0000000Eu,0x00000003u,
    0x0000000Du,0x0004003Bu,0x0000000Eu,0x0000000Fu,0x00000003u,0x00040020u,0x00000010u,0x00000001u,
    0x0000000Du,0x0004003Bu,0x00000010u,0x00000011u,0x00000001u,0x00040015u,0x00000013u,0x00000020u,
    0x00000000u,0x0004002Bu,0x00000013u,0x00000014u,0x00000001u,0x0004001Cu,0x00000015u,0x00000006u,
    0x00000014u,0x0006001Eu,0x00000016u,0x0000000Du,0x00000006u,0x00000015u,0x00000015u,0x00040020u,
    0x00000017u,0x00000003u,0x00000016u,0x0004003Bu,0x00000017u,0x00000018u,0x00000003u,0x00040015u,
    0x00000019u,0x00000020u,0x00000001u,0x0004002Bu,0x00000019u,0x0000001Au,0x00000000u,0x00040018u,
    0x0000001Bu,0x0000000Du,0x00000004u,0x0003001Eu,0x0000001Cu,0x0000001Bu,0x00040020u,0x0000001Du,
    0x00000009u,0x0000001Cu,0x0004003Bu,0x0000001Du,0x0000001Eu,0x00000009u,0x00040020u,0x0000001Fu,
    0x00000009u,0x0000001Bu,0x0004003Bu,0x0000000Au,0x00000022u,0x00000001u,0x0004002Bu,0x00000006u,
    0x00000024u,0x00000000u,0x0004002Bu,0x00000006u,0x00000025u,0x3F800000u,0x00050036u,0x00000002u,
    0x00000004u,0x00000000u,0x00000003u,0x000200F8u,0x00000005u,0x0004003Du,0x00000007u,0x0000000Cu,
    0x0000000Bu,0x0003003Eu,0x00000009u,0x0000000Cu,0x0004003Du,0x0000000Du,0x00000012u,0x00000011u,
    0x0003003Eu,0x0000000Fu,0x00000012u,0x00050041u,0x0000001Fu,0x00000020u,0x0000001Eu,0x0000001Au,
    0x0004003Du,0x0000001Bu,0x00000021u,0x00000020u,0x0004003Du,0x00000007u,0x00000023u,0x00000022u,
    0x00050051u,0x00000006u,0x00000026u,0x00000023u,0x00000000u,0x00050051u,0x00000006u,0x00000027u,
    0x00000023u,0x00000001u,0x00070050u,0x0000000Du,0x00000028u,0x00000026u,0x00000027u,0x00000024u,
    0x00000025u,0x00050091u,0x0000000Du,0x00000029u,0x00000021u,0x00000028u,0x00050041u,0x0000000Eu,
    0x0000002Au,0x00000018u,0x0000001Au,0x0003003Eu,0x0000002Au,0x00000029u,0x000100FDu,0x00010038u,
};
static const size_t g_nk_vert_spv_size = sizeof(g_nk_vert_spv);

static const uint32_t g_nk_frag_spv[] = {
    0x07230203u,0x00010000u,0x000D000Bu,0x00000018u,0x00000000u,0x00020011u,0x00000001u,0x0006000Bu,
    0x00000001u,0x4C534C47u,0x6474732Eu,0x3035342Eu,0x00000000u,0x0003000Eu,0x00000000u,0x00000001u,
    0x0008000Fu,0x00000004u,0x00000004u,0x6E69616Du,0x00000000u,0x00000009u,0x0000000Bu,0x00000014u,
    0x00030010u,0x00000004u,0x00000007u,0x00030003u,0x00000002u,0x000001C2u,0x000A0004u,0x475F4C47u,
    0x4C474F4Fu,0x70635F45u,0x74735F70u,0x5F656C79u,0x656E696Cu,0x7269645Fu,0x69746365u,0x00006576u,
    0x00080004u,0x475F4C47u,0x4C474F4Fu,0x6E695F45u,0x64756C63u,0x69645F65u,0x74636572u,0x00657669u,
    0x00040005u,0x00000004u,0x6E69616Du,0x00000000u,0x00050005u,0x00000009u,0x5F74756Fu,0x6F6C6F63u,
    0x00000072u,0x00050005u,0x0000000Bu,0x635F6E69u,0x726F6C6Fu,0x00000000u,0x00030005u,0x00000010u,
    0x00786574u,0x00040005u,0x00000014u,0x755F6E69u,0x00000076u,0x00040047u,0x00000009u,0x0000001Eu,
    0x00000000u,0x00040047u,0x0000000Bu,0x0000001Eu,0x00000001u,0x00040047u,0x00000010u,0x00000021u,
    0x00000000u,0x00040047u,0x00000010u,0x00000022u,0x00000000u,0x00040047u,0x00000014u,0x0000001Eu,
    0x00000000u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00030016u,0x00000006u,
    0x00000020u,0x00040017u,0x00000007u,0x00000006u,0x00000004u,0x00040020u,0x00000008u,0x00000003u,
    0x00000007u,0x0004003Bu,0x00000008u,0x00000009u,0x00000003u,0x00040020u,0x0000000Au,0x00000001u,
    0x00000007u,0x0004003Bu,0x0000000Au,0x0000000Bu,0x00000001u,0x00090019u,0x0000000Du,0x00000006u,
    0x00000001u,0x00000000u,0x00000000u,0x00000000u,0x00000001u,0x00000000u,0x0003001Bu,0x0000000Eu,
    0x0000000Du,0x00040020u,0x0000000Fu,0x00000000u,0x0000000Eu,0x0004003Bu,0x0000000Fu,0x00000010u,
    0x00000000u,0x00040017u,0x00000012u,0x00000006u,0x00000002u,0x00040020u,0x00000013u,0x00000001u,
    0x00000012u,0x0004003Bu,0x00000013u,0x00000014u,0x00000001u,0x00050036u,0x00000002u,0x00000004u,
    0x00000000u,0x00000003u,0x000200F8u,0x00000005u,0x0004003Du,0x00000007u,0x0000000Cu,0x0000000Bu,
    0x0004003Du,0x0000000Eu,0x00000011u,0x00000010u,0x0004003Du,0x00000012u,0x00000015u,0x00000014u,
    0x00050057u,0x00000007u,0x00000016u,0x00000011u,0x00000015u,0x00050085u,0x00000007u,0x00000017u,
    0x0000000Cu,0x00000016u,0x0003003Eu,0x00000009u,0x00000017u,0x000100FDu,0x00010038u,
};
static const size_t g_nk_frag_spv_size = sizeof(g_nk_frag_spv);

#define VK_MAX_FRAMES_IN_FLIGHT       2
#define VK_DESCRIPTOR_POOL_CAPACITY   1024   /* max simultaneously loaded textures */
#define VK_INITIAL_VERTEX_CAPACITY    1024   /* starting CPU-side vertex queue size */
#define VK_INITIAL_BATCH_CAPACITY     128    /* starting textured-batch queue size  */
#define VK_INITIAL_TEXTURE_CAPACITY   64     /* starting texture slot array size    */
#define VK_ENGINE_REQUIRED_API_VERSION VK_API_VERSION_1_2

#define VK_DEFAULT_CLEAR_R 0.392f
#define VK_DEFAULT_CLEAR_G 0.584f
#define VK_DEFAULT_CLEAR_B 0.929f
#define VK_DEFAULT_CLEAR_A 1.0f

/* ── Internal state ──────────────────────────────────────────────────────── */

typedef struct vulkan_texture_t {
    bool alive;
    uint32_t width, height;
    uint32_t mip_levels;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;           /* per-texture, created from TEXTURE_* flags */
    VkDescriptorSet descriptor_set;
} vulkan_texture_t;

typedef struct {
    VkBuffer           gpu_buf[VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory     gpu_mem[VK_MAX_FRAMES_IN_FLIGHT];
    uint32_t           gpu_cap[VK_MAX_FRAMES_IN_FLIGHT];
    fwk_backend_vertex *cpu;
    uint32_t           count;
    uint32_t           capacity;
} vk_vertex_queue_t;

typedef struct vulkan_textured_batch_t {
    fwk_backend_texture_t texture;
    uint32_t first;
    uint32_t count;
    float    mvp[16];   /* GPU-side MVP — pushed as push constant per batch */
} vulkan_textured_batch_t;

/* Current MVP set by engine_vulkan_set_transform() */
static float vk_current_mvp[16] = {
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1  /* identity default */
};

static struct {
    GLFWwindow           *window;
    bool                  owns_window;
    bool                  owns_glfw;
    int                   width, height;

    VkInstance            instance;
    VkPhysicalDevice      physical_device;
    VkDevice              device;
    uint32_t              graphics_family;
    VkQueue               graphics_queue;
    VkQueue               present_queue;
    uint32_t              present_family;

    VkSurfaceKHR          surface;
    VkSwapchainKHR        swapchain;
    VkFormat              swapchain_format;
    VkExtent2D            swapchain_extent;
    uint32_t              image_count;
    VkImage              *images;
    VkImageView          *image_views;
    VkFramebuffer        *framebuffers;

    /* Depth buffer — shared across all framebuffers (one image covers all swapchain images) */
    VkImage               depth_image;
    VkDeviceMemory        depth_memory;
    VkImageView           depth_view;
    VkFormat              depth_format;

    VkRenderPass          render_pass;
    VkPipelineLayout      pipeline_layout;
    VkPipeline            clear_pipeline;
    VkShaderModule        vert_module;
    VkShaderModule        frag_module;
    VkPipelineLayout      primitive_pipeline_layout;
    VkPipeline            primitive_pipeline;
    VkShaderModule        primitive_vert_module;
    VkShaderModule        primitive_frag_module;
    VkPipelineLayout      textured_pipeline_layout;
    VkPipeline            textured_pipeline;
    VkShaderModule        textured_vert_module;
    VkShaderModule        textured_frag_module;
    VkDescriptorSetLayout texture_descriptor_layout;
    VkDescriptorPool      texture_descriptor_pool;
    /* No global sampler — each texture has its own sampler based on TEXTURE_* flags */

    VkCommandPool         cmd_pool;
    VkCommandBuffer      *cmd_buffers;
    VkSemaphore          *image_available;
    VkSemaphore          *render_finished;
    VkFence              *in_flight;
    uint32_t              frame_index;

    float                 pending_clear[4]; /* RGBA — set by engine_vulkan_clear() */
    bool                  has_pending_clear;
    bool                  framebuffer_minimized;
    bool                  frame_started;
    int                   viewport_x, viewport_y;
    int                   viewport_w, viewport_h;
    bool                  blend_enabled;
    bool                  depth_test_enabled;
    bool                  depth_write_enabled;
    vk_vertex_queue_t     prim;
    vk_vertex_queue_t     tex;
    vulkan_textured_batch_t *textured_batches;
    uint32_t              textured_batch_count;
    uint32_t              textured_batch_capacity;
    vulkan_texture_t     *textures;
    uint32_t              texture_capacity;
    fwk_backend_texture_t white_texture;

    /* ── Nuklear Vulkan UI state (no NK types here — they live in 3rd_nuklear_glfw_vk.h) ── */
    struct {
        GLFWwindow                  *win;
        unsigned int                 text_buf[256]; /* char input, fed by vk_nk_char_cb */
        int                          text_len;
        VkBuffer                     vbuf[VK_MAX_FRAMES_IN_FLIGHT];
        VkDeviceMemory               vmem[VK_MAX_FRAMES_IN_FLIGHT];
        VkBuffer                     ibuf[VK_MAX_FRAMES_IN_FLIGHT];
        VkDeviceMemory               imem[VK_MAX_FRAMES_IN_FLIGHT];
        VkPipeline                   pipeline;
        VkPipelineLayout             layout;
        VkShaderModule               vert;
        VkShaderModule               frag;
        bool                         initialized;
    } nk;

    bool                  initialized;
} vk;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

#define VK_CHECK(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "[Vulkan] %s failed: %d (line %d)\n", #expr, _r, __LINE__); \
        return false; \
    } \
} while (0)

#define VK_CHECK_V(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "[Vulkan] %s failed: %d\n", #expr, _r); \
        return; \
    } \
} while (0)

static bool create_shader_module(const uint32_t *code, size_t size, VkShaderModule *out_module) {
    if (!out_module || !code || size == 0)
        return false;
    *out_module = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code,
    };
    VK_CHECK(vkCreateShaderModule(vk.device, &ci, NULL, out_module));
    return true;
}

static uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk.physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

/* ── Generic vertex queue helpers (shared by prim + tex paths) ───────────── */

static bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties,
                          VkBuffer *buffer, VkDeviceMemory *memory);

static void vq_destroy_gpu_buffers(vk_vertex_queue_t *q) {
    if (!vk.device) return;
    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (q->gpu_buf[i]) vkDestroyBuffer(vk.device, q->gpu_buf[i], NULL);
        if (q->gpu_mem[i]) vkFreeMemory(vk.device, q->gpu_mem[i], NULL);
        q->gpu_buf[i] = VK_NULL_HANDLE;
        q->gpu_mem[i] = VK_NULL_HANDLE;
        q->gpu_cap[i] = 0;
    }
}

static bool vq_create_gpu_buffer(vk_vertex_queue_t *q, uint32_t frame, uint32_t capacity) {
    if (frame >= VK_MAX_FRAMES_IN_FLIGHT || capacity == 0)
        return false;
    if (q->gpu_buf[frame]) vkDestroyBuffer(vk.device, q->gpu_buf[frame], NULL);
    if (q->gpu_mem[frame]) vkFreeMemory(vk.device, q->gpu_mem[frame], NULL);
    q->gpu_buf[frame] = VK_NULL_HANDLE;
    q->gpu_mem[frame] = VK_NULL_HANDLE;
    q->gpu_cap[frame] = 0;

    VkDeviceSize size = (VkDeviceSize)capacity * sizeof(fwk_backend_vertex);
    if (!create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &q->gpu_buf[frame], &q->gpu_mem[frame])) {
        fprintf(stderr, "[Vulkan] Failed to create vertex buffer\n");
        return false;
    }
    q->gpu_cap[frame] = capacity;
    return true;
}

static bool vq_ensure_cpu_capacity(vk_vertex_queue_t *q, uint32_t needed, const char *label) {
    if (needed <= q->capacity) return true;
    uint32_t cap = q->capacity ? q->capacity : VK_INITIAL_VERTEX_CAPACITY;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2) { cap = needed; break; }
        cap *= 2;
    }
    fwk_backend_vertex *v = realloc(q->cpu, (size_t)cap * sizeof(fwk_backend_vertex));
    if (!v) {
        fprintf(stderr, "[Vulkan] Out of memory growing %s CPU vertex queue\n", label);
        return false;
    }
    q->cpu = v;
    q->capacity = cap;
    return true;
}

static bool vq_ensure_gpu_buffer(vk_vertex_queue_t *q, uint32_t frame, uint32_t needed) {
    if (frame >= VK_MAX_FRAMES_IN_FLIGHT || needed == 0) return false;
    if (needed <= q->gpu_cap[frame]) return true;
    uint32_t cap = q->gpu_cap[frame] ? q->gpu_cap[frame] : VK_INITIAL_VERTEX_CAPACITY;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2) { cap = needed; break; }
        cap *= 2;
    }
    return vq_create_gpu_buffer(q, frame, cap);
}

static bool vq_upload(vk_vertex_queue_t *q, uint32_t frame, const char *label) {
    if (q->count == 0) return true;
    if (!vq_ensure_gpu_buffer(q, frame, q->count)) return false;
    void *mapped = NULL;
    VkDeviceSize size = (VkDeviceSize)q->count * sizeof(fwk_backend_vertex);
    VkResult r = vkMapMemory(vk.device, q->gpu_mem[frame], 0, size, 0, &mapped);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkMapMemory(%s vertices) failed: %d\n", label, r);
        return false;
    }
    memcpy(mapped, q->cpu, (size_t)size);
    vkUnmapMemory(vk.device, q->gpu_mem[frame]);
    return true;
}

/* ── Per-type enqueue (batch logic differs between prim and tex) ─────────── */

static bool enqueue_primitive_vertices(const fwk_backend_vertex *vertices, uint32_t count) {
    if (!vk.initialized || !vertices || count == 0) return false;
    if (vk.prim.count > UINT32_MAX - count) return false;
    uint32_t needed = vk.prim.count + count;
    if (!vq_ensure_cpu_capacity(&vk.prim, needed, "primitive")) return false;
    memcpy(vk.prim.cpu + vk.prim.count, vertices, (size_t)count * sizeof(fwk_backend_vertex));
    vk.prim.count = needed;
    return true;
}

static bool ensure_textured_batch_capacity(uint32_t needed) {
    if (needed <= vk.textured_batch_capacity)
        return true;

    uint32_t capacity = vk.textured_batch_capacity ? vk.textured_batch_capacity : VK_INITIAL_BATCH_CAPACITY;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    vulkan_textured_batch_t *batches =
        realloc(vk.textured_batches, (size_t)capacity * sizeof(vulkan_textured_batch_t));
    if (!batches) {
        fprintf(stderr, "[Vulkan] Out of memory growing textured batch queue\n");
        return false;
    }
    vk.textured_batches = batches;
    vk.textured_batch_capacity = capacity;
    return true;
}

static bool enqueue_textured_vertices(fwk_backend_texture_t texture,
                                      const fwk_backend_vertex *vertices,
                                      uint32_t count) {
    if (!vk.initialized || !vertices || count == 0) return false;
    if (vk.tex.count > UINT32_MAX - count) return false;

    uint32_t first = vk.tex.count;
    uint32_t needed = vk.tex.count + count;
    if (!vq_ensure_cpu_capacity(&vk.tex, needed, "textured")) return false;

    memcpy(vk.tex.cpu + first, vertices, (size_t)count * sizeof(fwk_backend_vertex));
    vk.tex.count = needed;

    /* Merge into previous batch only if same texture AND same MVP */
    if (vk.textured_batch_count > 0) {
        vulkan_textured_batch_t *last = &vk.textured_batches[vk.textured_batch_count - 1];
        if (last->texture == texture &&
            last->first + last->count == first &&
            memcmp(last->mvp, vk_current_mvp, 64) == 0) {
            last->count += count;
            return true;
        }
    }

    if (!ensure_textured_batch_capacity(vk.textured_batch_count + 1))
        return false;
    vulkan_textured_batch_t *b = &vk.textured_batches[vk.textured_batch_count++];
    b->texture = texture;
    b->first   = first;
    b->count   = count;
    memcpy(b->mvp, vk_current_mvp, 64);
    return true;
}

static bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties,
                          VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo buffer_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(vk.device, &buffer_ci, NULL, buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, *buffer, &req);
    uint32_t mem_type = find_memory_type(req.memoryTypeBits, properties);
    if (mem_type == UINT32_MAX) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk.device, &alloc, NULL, memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(vk.device, *buffer, *memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        vkFreeMemory(vk.device, *memory, NULL);
        *buffer = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool begin_single_time_commands(VkCommandBuffer *cmd) {
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = vk.cmd_pool,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(vk.device, &alloc, cmd) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(*cmd, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, cmd);
        *cmd = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool end_single_time_commands(VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, &cmd);
        return false;
    }

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VkResult r = vkQueueSubmit(vk.graphics_queue, 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS)
        r = vkQueueWaitIdle(vk.graphics_queue);
    vkFreeCommandBuffers(vk.device, vk.cmd_pool, 1, &cmd);
    return r == VK_SUCCESS;
}

static bool transition_image_layout(VkImage image, VkImageLayout old_layout,
                                    VkImageLayout new_layout) {
    VkCommandBuffer cmd;
    if (!begin_single_time_commands(&cmd))
        return false;

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1,
    };

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
    return end_single_time_commands(cmd);
}

static bool copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer cmd;
    if (!begin_single_time_commands(&cmd))
        return false;

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = {0, 0, 0},
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return end_single_time_commands(cmd);
}

static bool convert_pixels_to_rgba8(unsigned w, unsigned h, unsigned n,
                                    const void *pixels, uint8_t **out_pixels) {
    size_t count = (size_t)w * (size_t)h;
    uint8_t *rgba = malloc(count * 4);
    if (!rgba)
        return false;

    const uint8_t *src = (const uint8_t*)pixels;
    for (size_t i = 0; i < count; i++) {
        uint8_t r = 255, g = 255, b = 255, a = 255;
        if (src) {
            if (n == 1) r = g = b = src[i];
            else if (n == 2) r = g = b = src[i * 2 + 0], a = src[i * 2 + 1];
            else if (n == 3) r = src[i * 3 + 0], g = src[i * 3 + 1], b = src[i * 3 + 2];
            else if (n >= 4) r = src[i * 4 + 0], g = src[i * 4 + 1], b = src[i * 4 + 2], a = src[i * 4 + 3];
        }
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }

    *out_pixels = rgba;
    return true;
}

static void destroy_vulkan_texture(vulkan_texture_t *texture) {
    if (!texture || !vk.device)
        return;
    if (texture->descriptor_set && vk.texture_descriptor_pool)
        vkFreeDescriptorSets(vk.device, vk.texture_descriptor_pool, 1, &texture->descriptor_set);
    if (texture->sampler)
        vkDestroySampler(vk.device, texture->sampler, NULL);
    if (texture->view)
        vkDestroyImageView(vk.device, texture->view, NULL);
    if (texture->image)
        vkDestroyImage(vk.device, texture->image, NULL);
    if (texture->memory)
        vkFreeMemory(vk.device, texture->memory, NULL);
    memset(texture, 0, sizeof(*texture));
}

static void destroy_texture_infra(void) {
    if (!vk.device) return;
    if (vk.textures) {
        for (uint32_t i = 1; i <= vk.texture_capacity; i++)
            destroy_vulkan_texture(&vk.textures[i]);
    }
    free(vk.textures);
    vk.textures = NULL;
    vk.texture_capacity = 0;
    vk.white_texture = 0;


    if (vk.texture_descriptor_pool)
        vkDestroyDescriptorPool(vk.device, vk.texture_descriptor_pool, NULL);
    if (vk.texture_descriptor_layout)
        vkDestroyDescriptorSetLayout(vk.device, vk.texture_descriptor_layout, NULL);
    vk.texture_descriptor_pool = VK_NULL_HANDLE;
    vk.texture_descriptor_layout = VK_NULL_HANDLE;
}

static bool create_texture_infra(void) {
    VkDescriptorSetLayoutBinding sampler_binding = {
        .binding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler_binding,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layout_ci, NULL,
                                         &vk.texture_descriptor_layout));

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = VK_DESCRIPTOR_POOL_CAPACITY,
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = VK_DESCRIPTOR_POOL_CAPACITY,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VK_CHECK(vkCreateDescriptorPool(vk.device, &pool_ci, NULL,
                                    &vk.texture_descriptor_pool));

    return true;
}

static bool ensure_texture_capacity(uint32_t needed) {
    if (needed <= vk.texture_capacity)
        return true;

    uint32_t capacity = vk.texture_capacity ? vk.texture_capacity : VK_INITIAL_TEXTURE_CAPACITY;
    while (capacity < needed)
        capacity *= 2;

    vulkan_texture_t *textures =
        realloc(vk.textures, (size_t)(capacity + 1) * sizeof(vulkan_texture_t));
    if (!textures)
        return false;
    memset(textures + vk.texture_capacity + 1, 0,
           (size_t)(capacity - vk.texture_capacity) * sizeof(vulkan_texture_t));
    vk.textures = textures;
    vk.texture_capacity = capacity;
    return true;
}

static vulkan_texture_t *lookup_texture(fwk_backend_texture_t handle) {
    if (handle == 0 || handle > vk.texture_capacity || !vk.textures)
        return NULL;
    vulkan_texture_t *texture = &vk.textures[handle];
    return texture->alive ? texture : NULL;
}

/* Create a sampler respecting TEXTURE_* flags (mirrors GL glTexParameteri behavior) */
static VkSampler vk_create_sampler_for_flags(int flags) {
    bool want_mipmaps = !!(flags & 128);  /* TEXTURE_MIPMAPS */
    /* Use NEAREST for non-mipmap textures: matches OpenGL's effective rendering
     * at native 2:1 scale where GL_LINEAR ≡ GL_NEAREST with integer pixel positions.
     * LINEAR + CLAMP_TO_EDGE on an atlas bleeds adjacent frames; NEAREST never blends. */
    VkFilter filter = (want_mipmaps && (flags & 64)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerMipmapMode mip_mode = (flags & 64) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    /* TEXTURE_REPEAT = 0x200, TEXTURE_BORDER = 0x100, TEXTURE_CLAMP = 0 (default) */
    VkSamplerAddressMode wrap;
    if      (flags & 0x200) wrap = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    else if (flags & 0x100) wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    else                    wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    bool has_mipmaps = !!(flags & 128); /* TEXTURE_MIPMAPS */

    VkSamplerCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .addressModeU = wrap,
        .addressModeV = wrap,
        .addressModeW = wrap,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .mipmapMode = mip_mode,
        .minLod = 0.0f,
        .maxLod = has_mipmaps ? VK_LOD_CLAMP_NONE : 0.0f,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(vk.device, &ci, NULL, &sampler);
    return sampler;
}

/* Generate mipmaps for an image already in TRANSFER_DST_OPTIMAL for level 0.
 * Uses image blits: transitions each level src→TRANSFER_SRC, blits to next, transitions to SHADER_READ. */
static bool vk_generate_mipmaps(VkImage image, uint32_t w, uint32_t h, uint32_t mip_levels) {
    VkCommandBuffer cmd;
    if (!begin_single_time_commands(&cmd)) return false;

    int32_t mip_w = (int32_t)w, mip_h = (int32_t)h;
    for (uint32_t i = 1; i < mip_levels; i++) {
        /* Transition level i-1: TRANSFER_DST → TRANSFER_SRC */
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i-1, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);

        /* Blit level i-1 → level i at half size */
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i-1, 0, 1 },
            .srcOffsets = { {0,0,0}, {mip_w, mip_h, 1} },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
            .dstOffsets = { {0,0,0}, {mip_w>1?mip_w/2:1, mip_h>1?mip_h/2:1, 1} },
        };
        vkCmdBlitImage(cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        /* Transition level i-1: TRANSFER_SRC → SHADER_READ */
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);

        if (mip_w > 1) mip_w /= 2;
        if (mip_h > 1) mip_h /= 2;
    }

    /* Transition last mip level: TRANSFER_DST → SHADER_READ */
    VkImageMemoryBarrier last = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip_levels-1, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &last);

    return end_single_time_commands(cmd);
}

static bool create_vulkan_texture_resource(vulkan_texture_t *texture,
                                           unsigned w, unsigned h, unsigned n,
                                           const void *pixels, int flags) {
    if (!texture || !vk.cmd_pool || !vk.texture_descriptor_layout ||
        w == 0 || h == 0 || n == 0 || n > 4)
        return false;

    uint8_t *rgba = NULL;
    if (!convert_pixels_to_rgba8(w, h, n, pixels, &rgba))
        return false;

    VkDeviceSize image_size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &staging, &staging_memory)) {
        free(rgba);
        return false;
    }

    void *mapped = NULL;
    if (vkMapMemory(vk.device, staging_memory, 0, image_size, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, staging, NULL);
        vkFreeMemory(vk.device, staging_memory, NULL);
        free(rgba);
        return false;
    }
    memcpy(mapped, rgba, (size_t)image_size);
    vkUnmapMemory(vk.device, staging_memory);
    free(rgba);

    /* Compute mip levels (TEXTURE_MIPMAPS = 128) */
    bool want_mipmaps = !!(flags & 128);
    uint32_t mip_levels = 1;
    if (want_mipmaps) {
        uint32_t m = w > h ? w : h;
        while (m > 1) { m >>= 1; mip_levels++; }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (want_mipmaps) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; /* needed for blit-based mip gen */

    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { w, h, 1 },
        .mipLevels = mip_levels,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateImage(vk.device, &image_ci, NULL, &texture->image) != VK_SUCCESS)
        goto fail;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, texture->image, &req);
    uint32_t mem_type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) goto fail;

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk.device, &alloc, NULL, &texture->memory) != VK_SUCCESS) goto fail;
    if (vkBindImageMemory(vk.device, texture->image, texture->memory, 0) != VK_SUCCESS) goto fail;

    /* Upload base mip (level 0) */
    if (!transition_image_layout(texture->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) goto fail;
    if (!copy_buffer_to_image(staging, texture->image, w, h)) goto fail;

    /* Generate remaining mip levels, or just transition to shader read */
    if (want_mipmaps && mip_levels > 1) {
        if (!vk_generate_mipmaps(texture->image, w, h, mip_levels)) goto fail;
    } else {
        if (!transition_image_layout(texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) goto fail;
    }

    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = mip_levels,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    if (vkCreateImageView(vk.device, &view_ci, NULL, &texture->view) != VK_SUCCESS) goto fail;

    /* Per-texture sampler based on TEXTURE_* flags */
    texture->sampler = vk_create_sampler_for_flags(flags);
    if (!texture->sampler) goto fail;

    VkDescriptorSetAllocateInfo desc_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk.texture_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk.texture_descriptor_layout,
    };
    if (vkAllocateDescriptorSets(vk.device, &desc_alloc, &texture->descriptor_set) != VK_SUCCESS)
        goto fail;

    VkDescriptorImageInfo image_info = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = texture->view,
        .sampler     = texture->sampler,   /* per-texture sampler */
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->descriptor_set,
        .dstBinding = 0, .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_info,
    };
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    texture->width      = w;
    texture->height     = h;
    texture->mip_levels = mip_levels;
    texture->alive      = true;
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    return true;

fail:
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    destroy_vulkan_texture(texture);
    return false;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool update_framebuffer_size(void) {
    int width = vk.width;
    int height = vk.height;
    if (vk.window)
        glfwGetFramebufferSize(vk.window, &width, &height);
    if (width <= 0 || height <= 0)
        return false;
    vk.width = width;
    vk.height = height;
    return true;
}

static bool has_vulkan_state(void) {
    return vk.instance || vk.device || vk.surface || vk.swapchain || vk.window;
}

static void cleanup_vulkan_objects(bool wait_idle);
/* NK pipeline helpers — defined later in the NK section */
static bool vk_nk_create_pipeline(void);
static void vk_nk_destroy_pipeline(void);
/* Texture helpers — defined later in the texture section */
static fwk_backend_texture_t engine_vulkan_create_texture(unsigned,unsigned,unsigned,const void*,int);
static fwk_backend_texture_t engine_vulkan_white_texture(void);

/* ── Instance creation ───────────────────────────────────────────────────── */

static bool create_instance(void) {
    /* Gather GLFW's required extensions */
    uint32_t glfw_ext_count = 0;
    const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    if (!glfw_exts || glfw_ext_count == 0) {
        fprintf(stderr, "[Vulkan] GLFW did not provide required instance extensions\n");
        return false;
    }

    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "Nebula Engine",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "Nebula",
        .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion         = VK_ENGINE_REQUIRED_API_VERSION,
    };
    VkInstanceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = glfw_ext_count,
        .ppEnabledExtensionNames = glfw_exts,
        /* Validation layers optional — silently omitted if unavailable */
        .enabledLayerCount       = 0,
        .ppEnabledLayerNames     = NULL,
    };

#ifndef NDEBUG
    /* Attempt to enable validation layers in debug builds */
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    VkLayerProperties *available = malloc(layer_count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, available);
    for (uint32_t i = 0; i < layer_count; i++) {
        if (strcmp(available[i].layerName, layers[0]) == 0) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = layers;
            break;
        }
    }
    free(available);
#endif

    VK_CHECK(vkCreateInstance(&ci, NULL, &vk.instance));
    return true;
}

/* ── Physical device + queue family selection ────────────────────────────── */

static const char *required_device_extensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};
static const uint32_t required_device_extension_count =
    sizeof(required_device_extensions) / sizeof(required_device_extensions[0]);

typedef struct swapchain_support_t {
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR      *formats;
    uint32_t                 format_count;
    VkPresentModeKHR        *present_modes;
    uint32_t                 present_mode_count;
} swapchain_support_t;

static void free_swapchain_support(swapchain_support_t *support) {
    if (!support) return;
    free(support->formats);
    free(support->present_modes);
    memset(support, 0, sizeof(*support));
}

static bool query_swapchain_support(VkPhysicalDevice device, swapchain_support_t *support) {
    if (!support) return false;
    memset(support, 0, sizeof(*support));

    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, vk.surface,
                                                       &support->capabilities));
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, vk.surface,
                                                  &support->format_count, NULL));
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, vk.surface,
                                                       &support->present_mode_count, NULL));
    if (support->format_count == 0 || support->present_mode_count == 0)
        return false;

    support->formats = calloc(support->format_count, sizeof(VkSurfaceFormatKHR));
    support->present_modes = calloc(support->present_mode_count, sizeof(VkPresentModeKHR));
    if (!support->formats || !support->present_modes) {
        free_swapchain_support(support);
        return false;
    }

    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, vk.surface,
                                             &support->format_count,
                                             support->formats) != VK_SUCCESS) {
        free_swapchain_support(support);
        return false;
    }
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, vk.surface,
                                                  &support->present_mode_count,
                                                  support->present_modes) != VK_SUCCESS) {
        free_swapchain_support(support);
        return false;
    }
    return support->format_count > 0 && support->present_mode_count > 0;
}

static bool find_queue_families_for_device(VkPhysicalDevice device,
                                           uint32_t *graphics_family,
                                           uint32_t *present_family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if (count == 0) return false;

    VkQueueFamilyProperties *families = malloc(count * sizeof(VkQueueFamilyProperties));
    if (!families) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

    *graphics_family = UINT32_MAX;
    *present_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (*graphics_family == UINT32_MAX &&
            families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            *graphics_family = i;
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, vk.surface, &present) != VK_SUCCESS) {
            free(families);
            return false;
        }
        if (*present_family == UINT32_MAX && families[i].queueCount > 0 && present)
            *present_family = i;
        if (*graphics_family != UINT32_MAX && *present_family != UINT32_MAX) break;
    }
    free(families);
    return *graphics_family != UINT32_MAX && *present_family != UINT32_MAX;
}

static bool device_supports_required_extensions(VkPhysicalDevice device) {
    uint32_t ext_count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &ext_count, NULL) != VK_SUCCESS)
        return false;
    if (ext_count == 0) return false;

    VkExtensionProperties *extensions = malloc(ext_count * sizeof(VkExtensionProperties));
    if (!extensions) return false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &ext_count, extensions) != VK_SUCCESS) {
        free(extensions);
        return false;
    }

    bool all_found = true;
    for (uint32_t req = 0; req < required_device_extension_count; req++) {
        bool found = false;
        for (uint32_t i = 0; i < ext_count; i++) {
            if (strcmp(required_device_extensions[req], extensions[i].extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            all_found = false;
            break;
        }
    }
    free(extensions);
    return all_found;
}

static int score_physical_device(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 1000;
    score += (int)props.limits.maxImageDimension2D;
    return score;
}

static bool is_physical_device_suitable(VkPhysicalDevice device,
                                        uint32_t *graphics_family,
                                        uint32_t *present_family,
                                        int *score) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    if (!find_queue_families_for_device(device, graphics_family, present_family)) {
        fprintf(stderr, "[Vulkan] Skipping GPU '%s': missing graphics/present queue\n",
                props.deviceName);
        return false;
    }

    if (!device_supports_required_extensions(device)) {
        fprintf(stderr, "[Vulkan] Skipping GPU '%s': missing required device extensions\n",
                props.deviceName);
        return false;
    }

    swapchain_support_t support;
    if (!query_swapchain_support(device, &support)) {
        fprintf(stderr, "[Vulkan] Skipping GPU '%s': incomplete swapchain support\n",
                props.deviceName);
        return false;
    }
    free_swapchain_support(&support);

    if (score)
        *score = score_physical_device(device);
    return true;
}

static bool pick_physical_device(void) {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, NULL));
    if (count == 0) { fprintf(stderr, "[Vulkan] No Vulkan devices found\n"); return false; }

    VkPhysicalDevice *devices = malloc(count * sizeof(VkPhysicalDevice));
    if (!devices) return false;
    if (vkEnumeratePhysicalDevices(vk.instance, &count, devices) != VK_SUCCESS) {
        free(devices);
        return false;
    }

    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t best_graphics = UINT32_MAX;
    uint32_t best_present = UINT32_MAX;
    int best_score = -1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t graphics_family = UINT32_MAX;
        uint32_t present_family = UINT32_MAX;
        int score = -1;

        if (!is_physical_device_suitable(devices[i], &graphics_family, &present_family, &score))
            continue;

        if (score > best_score) {
            best = devices[i];
            best_graphics = graphics_family;
            best_present = present_family;
            best_score = score;
        }
    }
    free(devices);

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "[Vulkan] No suitable GPU found (needs graphics queue, present queue, swapchain extension, surface format and present mode)\n");
        return false;
    }

    vk.physical_device = best;
    vk.graphics_family = best_graphics;
    vk.present_family = best_present;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physical_device, &props);
    fprintf(stderr, "[Vulkan] Using GPU: %s (graphics queue=%u, present queue=%u)\n",
            props.deviceName, vk.graphics_family, vk.present_family);
    return vk.graphics_family != UINT32_MAX && vk.present_family != UINT32_MAX;
}

/* ── Logical device ──────────────────────────────────────────────────────── */

static bool create_device(void) {
    float priority = 1.0f;
    uint32_t unique_families[2] = { vk.graphics_family, vk.present_family };
    uint32_t family_count = (vk.graphics_family == vk.present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_infos[2];
    for (uint32_t i = 0; i < family_count; i++) {
        queue_infos[i] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
    }

    VkPhysicalDeviceFeatures features = {0};

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = family_count,
        .pQueueCreateInfos       = queue_infos,
        .enabledExtensionCount   = required_device_extension_count,
        .ppEnabledExtensionNames = required_device_extensions,
        .pEnabledFeatures        = &features,
    };
    VK_CHECK(vkCreateDevice(vk.physical_device, &ci, NULL, &vk.device));
    vkGetDeviceQueue(vk.device, vk.graphics_family, 0, &vk.graphics_queue);
    vkGetDeviceQueue(vk.device, vk.present_family,  0, &vk.present_queue);
    return true;
}

/* ── Swapchain ───────────────────────────────────────────────────────────── */

static bool create_swapchain(void) {
    if (!update_framebuffer_size()) {
        fprintf(stderr, "[Vulkan] Swapchain creation skipped: framebuffer is 0x0\n");
        return false;
    }

    swapchain_support_t support;
    if (!query_swapchain_support(vk.physical_device, &support)) {
        fprintf(stderr, "[Vulkan] Surface has incomplete swapchain support\n");
        return false;
    }
    VkSurfaceCapabilitiesKHR caps = support.capabilities;

    /* Format: prefer B8G8R8A8_UNORM + SRGB_NONLINEAR to match the GL path,
     * which runs without GL_FRAMEBUFFER_SRGB (linear output, no gamma encode).
     * Using sRGB swapchain would double-encode sRGB textures, making colors lighter. */
    VkSurfaceFormatKHR chosen = support.formats[0];
    for (uint32_t i = 0; i < support.format_count; i++) {
        if (support.formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            support.formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = support.formats[i];
            break;
        }
    }
    vk.swapchain_format = chosen.format;

    /* Present mode: prefer mailbox (low-latency), fallback FIFO */
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < support.present_mode_count; i++) {
        if (support.present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = support.present_modes[i];
            break;
        }
    }

    free_swapchain_support(&support);

    /* Extent */
    if (caps.currentExtent.width != UINT32_MAX) {
        vk.swapchain_extent = caps.currentExtent;
    } else {
        vk.swapchain_extent.width = clamp_u32((uint32_t)vk.width,
                                              caps.minImageExtent.width,
                                              caps.maxImageExtent.width);
        vk.swapchain_extent.height = clamp_u32((uint32_t)vk.height,
                                               caps.minImageExtent.height,
                                               caps.maxImageExtent.height);
    }
    if (vk.swapchain_extent.width == 0 || vk.swapchain_extent.height == 0) {
        fprintf(stderr, "[Vulkan] Swapchain creation skipped: surface extent is 0x0\n");
        return false;
    }

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    uint32_t queue_families[2] = { vk.graphics_family, vk.present_family };
    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = vk.surface,
        .minImageCount    = img_count,
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = vk.swapchain_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = present_mode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE,
    };
    if (vk.graphics_family != vk.present_family) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = queue_families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VK_CHECK(vkCreateSwapchainKHR(vk.device, &ci, NULL, &vk.swapchain));

    VK_CHECK(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, NULL));
    if (vk.image_count == 0) {
        fprintf(stderr, "[Vulkan] Swapchain returned no images\n");
        return false;
    }
    vk.images = calloc(vk.image_count, sizeof(VkImage));
    if (!vk.images) return false;
    VK_CHECK(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images));

    /* Image views */
    vk.image_views = calloc(vk.image_count, sizeof(VkImageView));
    if (!vk.image_views) return false;
    for (uint32_t i = 0; i < vk.image_count; i++) {
        VkImageViewCreateInfo iv = {
            .sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image                           = vk.images[i],
            .viewType                        = VK_IMAGE_VIEW_TYPE_2D,
            .format                          = vk.swapchain_format,
            .components                      = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.baseMipLevel   = 0,
            .subresourceRange.levelCount     = 1,
            .subresourceRange.baseArrayLayer = 0,
            .subresourceRange.layerCount     = 1,
        };
        VK_CHECK(vkCreateImageView(vk.device, &iv, NULL, &vk.image_views[i]));
    }
    return true;
}

/* ── Render pass ─────────────────────────────────────────────────────────── */

/* Find the best supported depth format */
static VkFormat find_depth_format(void) {
    VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (uint32_t i = 0; i < 3; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(vk.physical_device, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return candidates[i];
    }
    return VK_FORMAT_UNDEFINED;
}

/* Create/destroy the shared depth image (single image covers all swapchain framebuffers) */
static bool create_depth_resources(void) {
    vk.depth_format = find_depth_format();
    if (vk.depth_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[Vulkan] No supported depth format found\n");
        return false;
    }

    VkImageCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = vk.depth_format,
        .extent        = { vk.swapchain_extent.width, vk.swapchain_extent.height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(vk.device, &ci, NULL, &vk.depth_image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, vk.depth_image, &req);
    uint32_t mem_type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) { vkDestroyImage(vk.device, vk.depth_image, NULL); return false; }

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk.device, &alloc, NULL, &vk.depth_memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(vk.device, vk.depth_image, vk.depth_memory, 0) != VK_SUCCESS) return false;

    /* Determine aspect mask: pure depth or depth+stencil */
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = vk.depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = vk.depth_format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(vk.device, &view_ci, NULL, &vk.depth_view) != VK_SUCCESS) return false;
    return true;
}

static void destroy_depth_resources(void) {
    if (!vk.device) return;
    if (vk.depth_view)   { vkDestroyImageView(vk.device, vk.depth_view, NULL);  vk.depth_view   = VK_NULL_HANDLE; }
    if (vk.depth_image)  { vkDestroyImage(vk.device, vk.depth_image, NULL);     vk.depth_image  = VK_NULL_HANDLE; }
    if (vk.depth_memory) { vkFreeMemory(vk.device, vk.depth_memory, NULL);      vk.depth_memory = VK_NULL_HANDLE; }
}

static bool create_render_pass(void) {
    VkAttachmentDescription colour = {
        .format         = vk.swapchain_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentDescription depth_att = {
        .format         = vk.depth_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,    /* depth not needed after frame */
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentDescription attachments[2] = { colour, depth_att };

    VkAttachmentReference colour_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref  = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &colour_ref,
        .pDepthStencilAttachment = &depth_ref,
    };

    /* Dependency covers both color AND depth/stencil outputs */
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                       | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                       | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments  = attachments,
        .subpassCount    = 1, .pSubpasses    = &subpass,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    VK_CHECK(vkCreateRenderPass(vk.device, &ci, NULL, &vk.render_pass));
    return true;
}

/* ── Generic pipeline builder ────────────────────────────────────────────── */

typedef struct {
    VkShaderModule                              vert;
    VkShaderModule                              frag;
    const VkPipelineVertexInputStateCreateInfo *vertex_input; /* NULL = empty (no VBO) */
    VkPrimitiveTopology                         topology;
    const VkPipelineColorBlendAttachmentState  *blend_att;    /* NULL = opaque write   */
    bool                                        depth_test;   /* read depth buffer      */
    bool                                        depth_write;  /* write to depth buffer  */
    VkPipelineLayout                            layout;
    VkPipeline                                 *out_pipeline;
} pipeline_build_t;

static bool build_pipeline(const pipeline_build_t *d) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = d->vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = d->frag, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo empty_vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = d->topology };
    VkViewport viewport = { .x=0, .y=0,
        .width=(float)vk.swapchain_extent.width,
        .height=(float)vk.swapchain_extent.height,
        .minDepth=0, .maxDepth=1 };
    VkRect2D scissor = { .offset={0,0}, .extent=vk.swapchain_extent };
    VkPipelineViewportStateCreateInfo vs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1, .pViewports=&viewport, .scissorCount=1, .pScissors=&scissor };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState opaque_att = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                          VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = d->blend_att ? d->blend_att : &opaque_att };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = d->depth_test  ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = d->depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL,
        .minDepthBounds   = 0.0f,
        .maxDepthBounds   = 1.0f,
    };
    VkGraphicsPipelineCreateInfo ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2, .pStages            = stages,
        .pVertexInputState   = d->vertex_input ? d->vertex_input : &empty_vi,
        .pInputAssemblyState = &ia, .pViewportState     = &vs,
        .pRasterizationState = &rs, .pMultisampleState  = &ms,
        .pColorBlendState    = &cbs,
        .pDepthStencilState  = &ds,
        .layout              = d->layout,
        .renderPass          = vk.render_pass, .subpass = 0,
    };
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &ci, NULL, d->out_pipeline));
    return true;
}

/* ── Per-pipeline creation (unique parts only) ───────────────────────────── */

static bool create_clear_pipeline(void) {
    if (!create_shader_module(g_clear_vert_spv, g_clear_vert_spv_size, &vk.vert_module)) return false;
    if (!create_shader_module(g_clear_frag_spv, g_clear_frag_spv_size, &vk.frag_module)) return false;
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 4 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc_range };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &layout_ci, NULL, &vk.pipeline_layout));
    return build_pipeline(&(pipeline_build_t){
        .vert = vk.vert_module, .frag = vk.frag_module,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .depth_test = false, .depth_write = false,   /* fullscreen clear — skip depth */
        .layout = vk.pipeline_layout, .out_pipeline = &vk.clear_pipeline });
}

static bool create_primitive_pipeline(void) {
    if (!create_shader_module(g_primitive_vert_spv, g_primitive_vert_spv_size, &vk.primitive_vert_module)) return false;
    if (!create_shader_module(g_primitive_frag_spv, g_primitive_frag_spv_size, &vk.primitive_frag_module)) return false;
    VkVertexInputBindingDescription binding = {
        .binding=0, .stride=sizeof(fwk_backend_vertex), .inputRate=VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2] = {
        { .location=0, .binding=0, .format=VK_FORMAT_R32G32B32_SFLOAT,    .offset=offsetof(fwk_backend_vertex, x) },
        { .location=1, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(fwk_backend_vertex, r) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,   .pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=2, .pVertexAttributeDescriptions=attrs };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp=VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp=VK_BLEND_OP_ADD,
        .colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT };
    VkPipelineLayoutCreateInfo layout_ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &layout_ci, NULL, &vk.primitive_pipeline_layout));
    return build_pipeline(&(pipeline_build_t){
        .vert=vk.primitive_vert_module, .frag=vk.primitive_frag_module,
        .vertex_input=&vi, .topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST, .blend_att=&blend_att,
        .depth_test=true, .depth_write=false,   /* ddraw reads depth, doesn't write (overlay) */
        .layout=vk.primitive_pipeline_layout, .out_pipeline=&vk.primitive_pipeline });
}

static bool create_textured_pipeline(void) {
    if (!create_shader_module(g_textured_vert_spv, g_textured_vert_spv_size, &vk.textured_vert_module)) return false;
    if (!create_shader_module(g_textured_frag_spv, g_textured_frag_spv_size, &vk.textured_frag_module)) return false;
    VkVertexInputBindingDescription binding = {
        .binding=0, .stride=sizeof(fwk_backend_vertex), .inputRate=VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] = {
        { .location=0, .binding=0, .format=VK_FORMAT_R32G32B32_SFLOAT,    .offset=offsetof(fwk_backend_vertex, x) },
        { .location=1, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(fwk_backend_vertex, u) },
        { .location=2, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(fwk_backend_vertex, r) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,   .pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=3, .pVertexAttributeDescriptions=attrs };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp=VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp=VK_BLEND_OP_ADD,
        .colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT };
    /* 64-byte push constant for the MVP mat4 (applied per sprite batch in vertex shader) */
    VkPushConstantRange textured_pc = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 64 };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1, .pSetLayouts=&vk.texture_descriptor_layout,
        .pushConstantRangeCount=1, .pPushConstantRanges=&textured_pc };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &layout_ci, NULL, &vk.textured_pipeline_layout));
    return build_pipeline(&(pipeline_build_t){
        .vert=vk.textured_vert_module, .frag=vk.textured_frag_module,
        .vertex_input=&vi, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .blend_att=&blend_att,
        .depth_test=true, .depth_write=false,   /* sprites read depth (3D occlusion), no write (alpha blend) */
        .layout=vk.textured_pipeline_layout, .out_pipeline=&vk.textured_pipeline });
}

/* ── Framebuffers ────────────────────────────────────────────────────────── */

static bool create_framebuffers(void) {
    vk.framebuffers = calloc(vk.image_count, sizeof(VkFramebuffer));
    if (!vk.framebuffers) return false;
    for (uint32_t i = 0; i < vk.image_count; i++) {
        /* attachment 0 = color (per swapchain image), attachment 1 = depth (shared) */
        VkImageView fb_attachments[2] = { vk.image_views[i], vk.depth_view };
        VkFramebufferCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = vk.render_pass,
            .attachmentCount = 2, .pAttachments = fb_attachments,
            .width           = vk.swapchain_extent.width,
            .height          = vk.swapchain_extent.height,
            .layers          = 1,
        };
        VK_CHECK(vkCreateFramebuffer(vk.device, &ci, NULL, &vk.framebuffers[i]));
    }
    return true;
}

static void destroy_swapchain_resources(void) {
    if (!vk.device) return;

    destroy_depth_resources();

    if (vk.framebuffers) {
        for (uint32_t i = 0; i < vk.image_count; i++) {
            if (vk.framebuffers[i])
                vkDestroyFramebuffer(vk.device, vk.framebuffers[i], NULL);
        }
    }
    if (vk.clear_pipeline)
        vkDestroyPipeline(vk.device, vk.clear_pipeline, NULL);
    if (vk.pipeline_layout)
        vkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
    if (vk.vert_module)
        vkDestroyShaderModule(vk.device, vk.vert_module, NULL);
    if (vk.frag_module)
        vkDestroyShaderModule(vk.device, vk.frag_module, NULL);
    if (vk.primitive_pipeline)
        vkDestroyPipeline(vk.device, vk.primitive_pipeline, NULL);
    if (vk.primitive_pipeline_layout)
        vkDestroyPipelineLayout(vk.device, vk.primitive_pipeline_layout, NULL);
    if (vk.primitive_vert_module)
        vkDestroyShaderModule(vk.device, vk.primitive_vert_module, NULL);
    if (vk.primitive_frag_module)
        vkDestroyShaderModule(vk.device, vk.primitive_frag_module, NULL);
    if (vk.textured_pipeline)
        vkDestroyPipeline(vk.device, vk.textured_pipeline, NULL);
    if (vk.textured_pipeline_layout)
        vkDestroyPipelineLayout(vk.device, vk.textured_pipeline_layout, NULL);
    if (vk.textured_vert_module)
        vkDestroyShaderModule(vk.device, vk.textured_vert_module, NULL);
    if (vk.textured_frag_module)
        vkDestroyShaderModule(vk.device, vk.textured_frag_module, NULL);
    if (vk.render_pass)
        vkDestroyRenderPass(vk.device, vk.render_pass, NULL);

    if (vk.image_views) {
        for (uint32_t i = 0; i < vk.image_count; i++) {
            if (vk.image_views[i])
                vkDestroyImageView(vk.device, vk.image_views[i], NULL);
        }
    }
    if (vk.swapchain)
        vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);

    free(vk.images);
    free(vk.image_views);
    free(vk.framebuffers);

    vk.swapchain = VK_NULL_HANDLE;
    vk.render_pass = VK_NULL_HANDLE;
    vk.pipeline_layout = VK_NULL_HANDLE;
    vk.clear_pipeline = VK_NULL_HANDLE;
    vk.vert_module = VK_NULL_HANDLE;
    vk.frag_module = VK_NULL_HANDLE;
    vk.primitive_pipeline = VK_NULL_HANDLE;
    vk.primitive_pipeline_layout = VK_NULL_HANDLE;
    vk.primitive_vert_module = VK_NULL_HANDLE;
    vk.primitive_frag_module = VK_NULL_HANDLE;
    vk.textured_pipeline = VK_NULL_HANDLE;
    vk.textured_pipeline_layout = VK_NULL_HANDLE;
    vk.textured_vert_module = VK_NULL_HANDLE;
    vk.textured_frag_module = VK_NULL_HANDLE;
    /* Rebuild NK pipeline on swapchain recreate (viewport/extent changes) */
    if (vk.nk.initialized) vk_nk_destroy_pipeline();
    vk.images = NULL;
    vk.image_views = NULL;
    vk.framebuffers = NULL;
    vk.image_count = 0;
    vk.swapchain_format = VK_FORMAT_UNDEFINED;
    vk.swapchain_extent = (VkExtent2D){0, 0};
}

static bool create_swapchain_resources(void) {
    if (!create_swapchain())          goto fail;
    if (!create_depth_resources())    goto fail;  /* depth image must exist before render pass */
    if (!create_render_pass())        goto fail;
    if (!create_clear_pipeline())     goto fail;
    if (!create_primitive_pipeline()) goto fail;
    if (!create_textured_pipeline())  goto fail;
    if (!create_framebuffers())       goto fail;
    /* Rebuild NK pipeline after swapchain recreate */
    if (vk.nk.initialized && !vk_nk_create_pipeline()) goto fail;
    return true;

fail:
    destroy_swapchain_resources();
    return false;
}

static bool recreate_swapchain(void) {
    if (!vk.initialized)
        return false;
    if (!update_framebuffer_size()) {
        vk.framebuffer_minimized = true;
        return true;
    }
    vk.framebuffer_minimized = false;

    VkResult idle = vkDeviceWaitIdle(vk.device);
    if (idle != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkDeviceWaitIdle failed during swapchain recreate: %d\n", idle);
        cleanup_vulkan_objects(false);
        return false;
    }

    destroy_swapchain_resources();

    if (!create_swapchain_resources()) {
        fprintf(stderr, "[Vulkan] Failed to recreate swapchain\n");
        return false;
    }

    fprintf(stderr, "[Vulkan] Recreated swapchain (%dx%d, %u images)\n",
            vk.width, vk.height, vk.image_count);
    return true;
}

/* ── Command pool + buffers + sync objects ───────────────────────────────── */

static void destroy_command_infra(void) {
    if (vk.device) {
        for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (vk.image_available && vk.image_available[i])
                vkDestroySemaphore(vk.device, vk.image_available[i], NULL);
            if (vk.render_finished && vk.render_finished[i])
                vkDestroySemaphore(vk.device, vk.render_finished[i], NULL);
            if (vk.in_flight && vk.in_flight[i])
                vkDestroyFence(vk.device, vk.in_flight[i], NULL);
        }
        if (vk.cmd_pool)
            vkDestroyCommandPool(vk.device, vk.cmd_pool, NULL);
    }

    free(vk.cmd_buffers);
    free(vk.image_available);
    free(vk.render_finished);
    free(vk.in_flight);

    vk.cmd_pool = VK_NULL_HANDLE;
    vk.cmd_buffers = NULL;
    vk.image_available = NULL;
    vk.render_finished = NULL;
    vk.in_flight = NULL;
    vk.frame_index = 0;
}

static bool recreate_frame_sync(uint32_t frame) {
    if (!vk.device || frame >= VK_MAX_FRAMES_IN_FLIGHT ||
        !vk.image_available || !vk.render_finished || !vk.in_flight)
        return false;

    if (vk.image_available[frame]) {
        vkDestroySemaphore(vk.device, vk.image_available[frame], NULL);
        vk.image_available[frame] = VK_NULL_HANDLE;
    }
    if (vk.render_finished[frame]) {
        vkDestroySemaphore(vk.device, vk.render_finished[frame], NULL);
        vk.render_finished[frame] = VK_NULL_HANDLE;
    }
    if (vk.in_flight[frame]) {
        vkDestroyFence(vk.device, vk.in_flight[frame], NULL);
        vk.in_flight[frame] = VK_NULL_HANDLE;
    }

    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fen_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkResult r = vkCreateSemaphore(vk.device, &sem_ci, NULL, &vk.image_available[frame]);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateSemaphore(image_available) failed while recovering frame sync: %d\n", r);
        return false;
    }
    r = vkCreateSemaphore(vk.device, &sem_ci, NULL, &vk.render_finished[frame]);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateSemaphore(render_finished) failed while recovering frame sync: %d\n", r);
        return false;
    }
    r = vkCreateFence(vk.device, &fen_ci, NULL, &vk.in_flight[frame]);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateFence failed while recovering frame sync: %d\n", r);
        return false;
    }
    return true;
}

static bool create_command_infra(void) {
    VkCommandPoolCreateInfo pool_ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk.graphics_family,
    };
    VK_CHECK(vkCreateCommandPool(vk.device, &pool_ci, NULL, &vk.cmd_pool));

    vk.cmd_buffers     = calloc(VK_MAX_FRAMES_IN_FLIGHT, sizeof(VkCommandBuffer));
    vk.image_available = calloc(VK_MAX_FRAMES_IN_FLIGHT, sizeof(VkSemaphore));
    vk.render_finished = calloc(VK_MAX_FRAMES_IN_FLIGHT, sizeof(VkSemaphore));
    vk.in_flight       = calloc(VK_MAX_FRAMES_IN_FLIGHT, sizeof(VkFence));
    if (!vk.cmd_buffers || !vk.image_available || !vk.render_finished || !vk.in_flight)
        return false;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = vk.cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_MAX_FRAMES_IN_FLIGHT,
    };
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &alloc_info, vk.cmd_buffers));

    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fen_ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(vk.device, &sem_ci, NULL, &vk.image_available[i]));
        VK_CHECK(vkCreateSemaphore(vk.device, &sem_ci, NULL, &vk.render_finished[i]));
        VK_CHECK(vkCreateFence(vk.device, &fen_ci, NULL, &vk.in_flight[i]));
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* ── Nuklear Vulkan UI                                                     ── */
/* ══════════════════════════════════════════════════════════════════════════ */
/*
 * NK code that needs Nuklear types lives in 3rd_nuklear_glfw_vk.h (included
 * by engine.c where NK_IMPLEMENTATION is available).  engine_vulkan.c only
 * provides raw Vulkan resources + a render-callback slot so the NK render
 * function can be called from inside the active render pass.
 */

/* Nuklear vertex layout — 20 bytes, identical to nk_glfw_vertex */
typedef struct { float pos[2]; float uv[2]; unsigned char col[4]; } vk_nk_vertex_t;
#define VK_NK_VERTEX_BYTES  (512u * 1024u)   /* 512 KB */
#define VK_NK_INDEX_BYTES   (128u * 1024u)   /* 128 KB  (uint16 indices) */

/* GLFW char callback — stores codepoints; consumed by 3rd_nuklear_glfw_vk.h */
static void vk_nk_char_cb(GLFWwindow *w, unsigned int cp) {
    (void)w;
    if (vk.nk.text_len < 256) vk.nk.text_buf[vk.nk.text_len++] = cp;
}

/* Render callback — set by 3rd_nuklear_glfw_vk.h, called inside end_frame */
static void (*s_nk_render_fn)(void) = NULL;
/* Current command buffer, valid only while s_nk_render_fn is executing */
static VkCommandBuffer s_nk_cmd = VK_NULL_HANDLE;

void engine_vulkan_set_nk_render_fn(void (*fn)(void)) { s_nk_render_fn = fn; }

/* ── NK per-frame buffer helpers ────────────────────────────────────────── */

static bool vk_nk_create_buffers(void) {
    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (!create_buffer(VK_NK_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &vk.nk.vbuf[i], &vk.nk.vmem[i]))
            return false;
        if (!create_buffer(VK_NK_INDEX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &vk.nk.ibuf[i], &vk.nk.imem[i]))
            return false;
    }
    return true;
}

static void vk_nk_destroy_buffers(void) {
    if (!vk.device) return;
    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vk.nk.vbuf[i]) { vkDestroyBuffer(vk.device, vk.nk.vbuf[i], NULL); vk.nk.vbuf[i] = VK_NULL_HANDLE; }
        if (vk.nk.vmem[i]) { vkFreeMemory(vk.device, vk.nk.vmem[i], NULL);    vk.nk.vmem[i] = VK_NULL_HANDLE; }
        if (vk.nk.ibuf[i]) { vkDestroyBuffer(vk.device, vk.nk.ibuf[i], NULL); vk.nk.ibuf[i] = VK_NULL_HANDLE; }
        if (vk.nk.imem[i]) { vkFreeMemory(vk.device, vk.nk.imem[i], NULL);    vk.nk.imem[i] = VK_NULL_HANDLE; }
    }
}

/* ── NK pipeline (dynamic scissor, alpha blend, mat4 push constant) ─────── */

static bool vk_nk_create_pipeline(void) {
    if (!create_shader_module(g_nk_vert_spv, g_nk_vert_spv_size, &vk.nk.vert)) return false;
    if (!create_shader_module(g_nk_frag_spv, g_nk_frag_spv_size, &vk.nk.frag)) return false;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vk.nk.vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = vk.nk.frag, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = {
        .binding=0, .stride=sizeof(vk_nk_vertex_t), .inputRate=VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] = {
        { .location=0, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,  .offset=0  }, /* pos */
        { .location=1, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,  .offset=8  }, /* uv  */
        { .location=2, .binding=0, .format=VK_FORMAT_R8G8B8A8_UNORM, .offset=16 }, /* col */
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1, .pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=3, .pVertexAttributeDescriptions=attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { .x=0,.y=0,
        .width=(float)vk.swapchain_extent.width,.height=(float)vk.swapchain_extent.height,
        .minDepth=0,.maxDepth=1 };
    VkRect2D sc = { .offset={0,0},.extent=vk.swapchain_extent };
    VkPipelineViewportStateCreateInfo vs = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,
        .frontFace=VK_FRONT_FACE_CLOCKWISE,.lineWidth=1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.colorBlendOp=VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,.alphaBlendOp=VK_BLEND_OP_ADD,
        .colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=1,.pDynamicStates=dyn_states };
    VkPushConstantRange pc_range = {
        .stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=64 }; /* mat4 */
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&vk.texture_descriptor_layout,
        .pushConstantRangeCount=1,.pPushConstantRanges=&pc_range };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &layout_ci, NULL, &vk.nk.layout));

    /* NK UI: depth test OFF, write OFF — UI always renders on top of scene */
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_FALSE, .depthWriteEnable=VK_FALSE,
        .depthCompareOp=VK_COMPARE_OP_ALWAYS };
    VkGraphicsPipelineCreateInfo ci = {
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vs,.pRasterizationState=&rs,
        .pMultisampleState=&ms,.pColorBlendState=&cbs,
        .pDepthStencilState=&ds,
        .pDynamicState=&dyn,
        .layout=vk.nk.layout,.renderPass=vk.render_pass,.subpass=0 };
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &ci, NULL, &vk.nk.pipeline));
    return true;
}

static void vk_nk_destroy_pipeline(void) {
    if (!vk.device) return;
    if (vk.nk.pipeline)     { vkDestroyPipeline(vk.device, vk.nk.pipeline, NULL);       vk.nk.pipeline = VK_NULL_HANDLE; }
    if (vk.nk.layout)       { vkDestroyPipelineLayout(vk.device, vk.nk.layout, NULL);   vk.nk.layout   = VK_NULL_HANDLE; }
    if (vk.nk.vert)         { vkDestroyShaderModule(vk.device, vk.nk.vert, NULL);        vk.nk.vert     = VK_NULL_HANDLE; }
    if (vk.nk.frag)         { vkDestroyShaderModule(vk.device, vk.nk.frag, NULL);        vk.nk.frag     = VK_NULL_HANDLE; }
}

/* ── NK Vulkan resource init (no NK types) ───────────────────────────────── */

bool engine_vulkan_nk_init_resources(void *win) {
    if (!vk.initialized || vk.nk.initialized) return vk.nk.initialized;
    vk.nk.win = (GLFWwindow*)win;
    if (!vk_nk_create_pipeline()) { fprintf(stderr, "[Vulkan NK] Pipeline failed\n"); return false; }
    if (!vk_nk_create_buffers())  { fprintf(stderr, "[Vulkan NK] Buffers failed\n");  return false; }
    glfwSetCharCallback(vk.nk.win, vk_nk_char_cb);
    vk.nk.initialized = true;
    fprintf(stderr, "[Vulkan NK] Resources ready\n");
    return true;
}

/* Upload font atlas pixels as a Vulkan texture (called from 3rd_nuklear_glfw_vk.h) */
fwk_backend_texture_t engine_vulkan_nk_upload_font(const void *pixels, int w, int h) {
    return engine_vulkan_create_texture((unsigned)w, (unsigned)h, 4, pixels, 0);
}

fwk_backend_texture_t engine_vulkan_nk_white_tex(void) {
    return engine_vulkan_white_texture();
}

/* ── Raw Vulkan draw functions (called from 3rd_nuklear_glfw_vk.h callback) ─ */

void *engine_vulkan_nk_map_vbuf(void) {
    void *p = NULL;
    vkMapMemory(vk.device, vk.nk.vmem[vk.frame_index], 0, VK_NK_VERTEX_BYTES, 0, &p);
    return p;
}

void *engine_vulkan_nk_map_ibuf(void) {
    void *p = NULL;
    vkMapMemory(vk.device, vk.nk.imem[vk.frame_index], 0, VK_NK_INDEX_BYTES, 0, &p);
    return p;
}

void engine_vulkan_nk_unmap(void) {
    vkUnmapMemory(vk.device, vk.nk.vmem[vk.frame_index]);
    vkUnmapMemory(vk.device, vk.nk.imem[vk.frame_index]);
}

void engine_vulkan_nk_begin_draw(float W, float H) {
    float proj[4][4] = {
        { 2.0f/W,    0,  0, 0 },
        {      0, 2.0f/H,  0, 0 },
        {      0,    0,  1, 0 },
        {     -1,   -1,  0, 1 },
    };
    VkDeviceSize off = 0;
    vkCmdBindPipeline(s_nk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.nk.pipeline);
    vkCmdBindVertexBuffers(s_nk_cmd, 0, 1, &vk.nk.vbuf[vk.frame_index], &off);
    vkCmdBindIndexBuffer(s_nk_cmd, vk.nk.ibuf[vk.frame_index], 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(s_nk_cmd, vk.nk.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(proj), proj);
}

bool engine_vulkan_nk_draw_cmd(uint64_t tex_id,
                               int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                               uint32_t elem_count, uint32_t idx_offset) {
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cw <= 0 || ch <= 0) return false;
    VkRect2D scissor = { .offset={cx,cy}, .extent={(uint32_t)cw,(uint32_t)ch} };
    vkCmdSetScissor(s_nk_cmd, 0, 1, &scissor);
    vulkan_texture_t *tex = lookup_texture((fwk_backend_texture_t)tex_id);
    if (!tex || !tex->descriptor_set) return false;
    vkCmdBindDescriptorSets(s_nk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk.nk.layout, 0, 1, &tex->descriptor_set, 0, NULL);
    vkCmdDrawIndexed(s_nk_cmd, elem_count, 1, idx_offset, 0, 0);
    return true;
}

uint32_t engine_vulkan_nk_current_frame(void)  { return vk.frame_index; }
float    engine_vulkan_nk_viewport_w(void)     { return (float)vk.swapchain_extent.width;  }
float    engine_vulkan_nk_viewport_h(void)     { return (float)vk.swapchain_extent.height; }
bool     engine_vulkan_nk_is_ready(void)       { return vk.nk.initialized; }
int     *engine_vulkan_nk_text_len(void)       { return &vk.nk.text_len; }
unsigned *engine_vulkan_nk_text_buf(void)      { return vk.nk.text_buf; }

static void cleanup_vulkan_objects(bool wait_idle) {
    GLFWwindow *owned_window = vk.owns_window ? vk.window : NULL;
    bool owned_glfw = vk.owns_glfw;

    if (g_render_api == &fwk_vulkan_render_api)
        g_render_api = NULL;

    if (vk.device) {
        if (wait_idle)
            vkDeviceWaitIdle(vk.device);
        destroy_command_infra();
        destroy_swapchain_resources();
        vq_destroy_gpu_buffers(&vk.prim);
        vq_destroy_gpu_buffers(&vk.tex);
        /* Nuklear Vulkan cleanup (NK context freed by 3rd_nuklear_glfw_vk.h shutdown) */
        if (vk.nk.initialized) {
            vk_nk_destroy_buffers();
            vk_nk_destroy_pipeline();
            vk.nk.initialized = false;
        }
        destroy_texture_infra();
        free(vk.prim.cpu);
        free(vk.tex.cpu);
        free(vk.textured_batches);
        vk.prim = (vk_vertex_queue_t){0};
        vk.tex  = (vk_vertex_queue_t){0};
        vk.textured_batches = NULL;
        vk.textured_batch_count = 0;
        vk.textured_batch_capacity = 0;
        vkDestroyDevice(vk.device, NULL);
    }

    if (vk.surface && vk.instance)
        vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (vk.instance)
        vkDestroyInstance(vk.instance, NULL);

    if (owned_window)
        glfwDestroyWindow(owned_window);
    if (owned_glfw)
        glfwTerminate();

    memset(&vk, 0, sizeof(vk));
}

static bool recover_failed_frame(const char *operation, VkResult result) {
    fprintf(stderr, "[Vulkan] %s failed: %d\n", operation, result);
    if (!recreate_frame_sync(vk.frame_index)) {
        fprintf(stderr, "[Vulkan] Frame sync recovery failed; shutting down backend\n");
        cleanup_vulkan_objects(true);
        return false;
    }
    return recreate_swapchain();
}

/* ── Public lifecycle ────────────────────────────────────────────────────── */

bool engine_vulkan_init(void *glfw_window, int width, int height) {
    if (has_vulkan_state())
        cleanup_vulkan_objects(true);
    else
        memset(&vk, 0, sizeof(vk));

    if (!glfw_window || width <= 0 || height <= 0) {
        fprintf(stderr, "[Vulkan] Invalid init parameters (window=%p, size=%dx%d)\n",
                glfw_window, width, height);
        return false;
    }

    vk.window = (GLFWwindow*)glfw_window;
    vk.width  = width;
    vk.height = height;

    if (!glfwVulkanSupported()) {
        fprintf(stderr, "[Vulkan] Vulkan not supported by GLFW\n");
        goto fail;
    }

    if (!create_instance())       goto fail;
    /* Surface must be created before pick_physical_device (for present support query) */
    if (glfwCreateWindowSurface(vk.instance, vk.window, NULL, &vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Failed to create window surface\n");
        goto fail;
    }
    if (!pick_physical_device())  goto fail;
    if (!create_device())         goto fail;
    if (!create_texture_infra())  goto fail;
    if (!create_swapchain_resources()) goto fail;
    if (!create_command_infra())  goto fail;

    vk.initialized = true;
    vk.viewport_x = 0;
    vk.viewport_y = 0;
    vk.viewport_w = width;
    vk.viewport_h = height;
    g_render_api = &fwk_vulkan_render_api;
    fprintf(stderr, "[Vulkan] Initialised (%dx%d, %u swapchain images)\n",
            width, height, vk.image_count);
    return true;

fail:
    cleanup_vulkan_objects(true);
    return false;
}

void engine_vulkan_shutdown(void) {
    if (!vk.instance && !vk.device) return;
    cleanup_vulkan_objects(true);
}

void engine_vulkan_clear(float r, float g, float b, float a) {
    vk.pending_clear[0] = r; vk.pending_clear[1] = g;
    vk.pending_clear[2] = b; vk.pending_clear[3] = a;
    vk.has_pending_clear = true;
}

bool engine_vulkan_begin_frame(void) {
    if (!vk.initialized) return false;
    vk.frame_started = false;
    if (vk.framebuffer_minimized)
        return true;

    VkResult wait_result = vkWaitForFences(vk.device, 1, &vk.in_flight[vk.frame_index],
                                           VK_TRUE, UINT64_MAX);
    if (wait_result != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkWaitForFences failed: %d\n", wait_result);
        cleanup_vulkan_objects(true);
        return false;
    }
    vk.has_pending_clear = false;
    vk.frame_started = true;
    return true;
}

bool engine_vulkan_end_frame(void) {
    if (!vk.initialized) return false;
    if (vk.framebuffer_minimized || !vk.frame_started) {
        vk.prim.count = 0;
        vk.tex.count = 0;
        vk.textured_batch_count = 0;
        return true;
    }

    if (!vk.swapchain) {
        vk.frame_started = false;
        return recreate_swapchain();
    }

    uint32_t primitive_count = vk.prim.count;
    if (primitive_count && !vq_upload(&vk.prim, vk.frame_index, "primitive")) {
        vk.frame_started = false;
        return false;
    }
    uint32_t textured_count = vk.tex.count;
    uint32_t textured_batch_count = vk.textured_batch_count;
    if (textured_count && !vq_upload(&vk.tex, vk.frame_index, "textured")) {
        vk.frame_started = false;
        return false;
    }

    uint32_t image_index;
    VkResult acq = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                          vk.image_available[vk.frame_index],
                                          VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        vk.frame_started = false;
        return recreate_swapchain();
    }
    bool needs_recreate = (acq == VK_SUBOPTIMAL_KHR);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        vk.frame_started = false;
        return false;
    }

    VkCommandBuffer cmd = vk.cmd_buffers[vk.frame_index];
    VkResult cmd_result = vkResetCommandBuffer(cmd, 0);
    if (cmd_result != VK_SUCCESS) {
        vk.frame_started = false;
        return recover_failed_frame("vkResetCommandBuffer", cmd_result);
    }

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cmd_result = vkBeginCommandBuffer(cmd, &begin_info);
    if (cmd_result != VK_SUCCESS) {
        vk.frame_started = false;
        return recover_failed_frame("vkBeginCommandBuffer", cmd_result);
    }

    /* Clear colour (cornflower blue default) */
    float clear_r = vk.has_pending_clear ? vk.pending_clear[0] : VK_DEFAULT_CLEAR_R;
    float clear_g = vk.has_pending_clear ? vk.pending_clear[1] : VK_DEFAULT_CLEAR_G;
    float clear_b = vk.has_pending_clear ? vk.pending_clear[2] : VK_DEFAULT_CLEAR_B;
    float clear_a = vk.has_pending_clear ? vk.pending_clear[3] : VK_DEFAULT_CLEAR_A;
    VkClearValue clear_vals[2] = {
        { .color = {{ clear_r, clear_g, clear_b, clear_a }} },   /* attachment 0: color */
        { .depthStencil = { 1.0f, 0 } },                          /* attachment 1: depth (far = 1.0) */
    };

    VkRenderPassBeginInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = vk.render_pass,
        .framebuffer     = vk.framebuffers[image_index],
        .renderArea      = { .offset = {0,0}, .extent = vk.swapchain_extent },
        .clearValueCount = 2,
        .pClearValues    = clear_vals,
    };
    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    /* Draw fullscreen triangle via clear pipeline (push constant colour) */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.clear_pipeline);
    float pc[4] = { clear_r, clear_g, clear_b, clear_a };
    vkCmdPushConstants(cmd, vk.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (textured_count && vk.textured_pipeline &&
        vk.tex.gpu_buf[vk.frame_index]) {
        VkDeviceSize offset = 0;
        VkBuffer vertex_buffer = vk.tex.gpu_buf[vk.frame_index];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.textured_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
        float pushed_mvp[16];
        bool mvp_valid = false;
        for (uint32_t i = 0; i < textured_batch_count; i++) {
            vulkan_textured_batch_t *batch = &vk.textured_batches[i];
            vulkan_texture_t *texture = lookup_texture(batch->texture);
            if (!texture || !texture->descriptor_set)
                continue;
            if (!mvp_valid || memcmp(pushed_mvp, batch->mvp, 64) != 0) {
                vkCmdPushConstants(cmd, vk.textured_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, 64, batch->mvp);
                memcpy(pushed_mvp, batch->mvp, 64);
                mvp_valid = true;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk.textured_pipeline_layout, 0, 1,
                                    &texture->descriptor_set, 0, NULL);
            vkCmdDraw(cmd, batch->count, 1, batch->first, 0);
        }
    }
    if (primitive_count && vk.primitive_pipeline &&
        vk.prim.gpu_buf[vk.frame_index]) {
        VkDeviceSize offset = 0;
        VkBuffer vertex_buffer = vk.prim.gpu_buf[vk.frame_index];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.primitive_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
        vkCmdDraw(cmd, primitive_count, 1, 0, 0);
    }
    /* Nuklear UI — callback defined in 3rd_nuklear_glfw_vk.h, must be inside render pass */
    if (s_nk_render_fn && vk.nk.initialized) {
        s_nk_cmd = cmd;
        s_nk_render_fn();
        s_nk_cmd = VK_NULL_HANDLE;
    }
    vkCmdEndRenderPass(cmd);
    cmd_result = vkEndCommandBuffer(cmd);
    if (cmd_result != VK_SUCCESS) {
        vk.frame_started = false;
        return recover_failed_frame("vkEndCommandBuffer", cmd_result);
    }
    vk.prim.count = 0;
    vk.tex.count = 0;
    vk.textured_batch_count = 0;

    VkResult fence_result = vkResetFences(vk.device, 1, &vk.in_flight[vk.frame_index]);
    if (fence_result != VK_SUCCESS) {
        vk.frame_started = false;
        return recover_failed_frame("vkResetFences", fence_result);
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1, .pWaitSemaphores   = &vk.image_available[vk.frame_index],
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1, .pCommandBuffers   = &cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.render_finished[vk.frame_index],
    };
    VkResult submit_result = vkQueueSubmit(vk.graphics_queue, 1, &submit, vk.in_flight[vk.frame_index]);
    if (submit_result != VK_SUCCESS) {
        vk.frame_started = false;
        return recover_failed_frame("vkQueueSubmit", submit_result);
    }

    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.render_finished[vk.frame_index],
        .swapchainCount     = 1, .pSwapchains     = &vk.swapchain,
        .pImageIndices      = &image_index,
    };
    VkResult pres = vkQueuePresentKHR(vk.present_queue, &present);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        needs_recreate = true;
    } else if (pres != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkQueuePresentKHR failed: %d\n", pres);
    }
    vk.frame_index = (vk.frame_index + 1) % VK_MAX_FRAMES_IN_FLIGHT;
    vk.frame_started = false;

    if (needs_recreate)
        return recreate_swapchain();
    return true;
}

bool engine_vulkan_resize(int width, int height) {
    if (!vk.initialized)
        return false;

    vk.width = width;
    vk.height = height;
    vk.viewport_x = 0;
    vk.viewport_y = 0;
    vk.viewport_w = width;
    vk.viewport_h = height;

    if (width <= 0 || height <= 0) {
        vk.framebuffer_minimized = true;
        return true;
    }

    if (!vk.framebuffer_minimized &&
        vk.swapchain_extent.width == (uint32_t)width &&
        vk.swapchain_extent.height == (uint32_t)height)
        return true;

    vk.framebuffer_minimized = false;
    return recreate_swapchain();
}

static fwk_backend_texture_t engine_vulkan_create_texture(unsigned w, unsigned h, unsigned n,
                                                          const void *pixels, int flags) {
    if (!vk.initialized || !vk.device || !vk.cmd_pool)
        return 0;

    uint32_t slot = 0;
    for (uint32_t i = 1; i <= vk.texture_capacity; i++) {
        if (!vk.textures[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot == 0) {
        slot = vk.texture_capacity + 1;
        if (!ensure_texture_capacity(slot))
            return 0;
    }

    vulkan_texture_t texture = {0};
    if (!create_vulkan_texture_resource(&texture, w, h, n, pixels, flags))
        return 0;
    vk.textures[slot] = texture;
    return slot;
}

static bool engine_vulkan_update_texture(fwk_backend_texture_t handle,
                                         unsigned w, unsigned h, unsigned n,
                                         const void *pixels, int flags) {
    vulkan_texture_t *texture = lookup_texture(handle);
    if (!texture)
        return false;

    vulkan_texture_t replacement = {0};
    if (!create_vulkan_texture_resource(&replacement, w, h, n, pixels, flags))
        return false;
    destroy_vulkan_texture(texture);
    *texture = replacement;
    return true;
}

static void engine_vulkan_destroy_texture(fwk_backend_texture_t handle) {
    vulkan_texture_t *texture = lookup_texture(handle);
    if (texture)
        destroy_vulkan_texture(texture);
}

static fwk_backend_texture_t engine_vulkan_white_texture(void) {
    if (!vk.white_texture) {
        const uint32_t pixel = 0xFFFFFFFFu;
        vk.white_texture = engine_vulkan_create_texture(1, 1, 4, &pixel, 0);
    }
    return vk.white_texture;
}

static void engine_vulkan_set_viewport(int x, int y, int w, int h) {
    vk.viewport_x = x;
    vk.viewport_y = y;
    vk.viewport_w = w;
    vk.viewport_h = h;
}

static void engine_vulkan_set_transform(const float mvp[16]) {
    if (mvp) memcpy(vk_current_mvp, mvp, 64);
}

static void engine_vulkan_set_blend(bool enabled) {
    vk.blend_enabled = enabled;
}

static void engine_vulkan_set_depth(bool test_enabled, bool write_enabled) {
    vk.depth_test_enabled = test_enabled;
    vk.depth_write_enabled = write_enabled;
}

static void engine_vulkan_line(const fwk_backend_vertex *a, const fwk_backend_vertex *b) {
    if (!a || !b) return;
    fwk_backend_vertex vertices[2] = { *a, *b };
    enqueue_primitive_vertices(vertices, 2);
}

static void engine_vulkan_triangle(const fwk_backend_vertex vertices[3]) {
    if (!vertices) return;
    fwk_backend_vertex lines[6] = {
        vertices[0], vertices[1],
        vertices[1], vertices[2],
        vertices[2], vertices[0],
    };
    enqueue_primitive_vertices(lines, 6);
}

static void engine_vulkan_quad(const fwk_backend_vertex vertices[4]) {
    if (!vertices) return;
    fwk_backend_vertex lines[8] = {
        vertices[0], vertices[1],
        vertices[1], vertices[2],
        vertices[2], vertices[3],
        vertices[3], vertices[0],
    };
    enqueue_primitive_vertices(lines, 8);
}

static void engine_vulkan_textured_quad(fwk_backend_texture_t texture,
                                        const fwk_backend_vertex vertices[4]) {
    if (!vertices) return;
    if (!lookup_texture(texture))
        texture = engine_vulkan_white_texture();
    if (!lookup_texture(texture)) {
        engine_vulkan_quad(vertices);
        return;
    }

    fwk_backend_vertex tris[6] = {
        vertices[2], vertices[3], vertices[0],
        vertices[2], vertices[0], vertices[1],
    };
    enqueue_textured_vertices(texture, tris, 6);
}

bool engine_vulkan_init_window(const char *title, int width, int height) {
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "[Vulkan] Invalid window size: %dx%d\n", width, height);
        return false;
    }

    if (has_vulkan_state())
        cleanup_vulkan_objects(true);

    if (!glfwInit()) {
        fprintf(stderr, "[Vulkan] glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  1);
    GLFWwindow *win = glfwCreateWindow(width, height,
                                       title ? title : "Nebula (Vulkan)",
                                       NULL, NULL);
    if (!win) {
        fprintf(stderr, "[Vulkan] glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }
    if (!engine_vulkan_init(win, width, height)) {
        glfwDestroyWindow(win);
        glfwTerminate();
        return false;
    }
    vk.owns_window = true;
    vk.owns_glfw = true;
    return true;
}

bool engine_vulkan_poll_events(void) {
    if (!vk.window) return false;
    glfwPollEvents();
    return !glfwWindowShouldClose((GLFWwindow*)vk.window);
}

int    engine_vulkan_width(void)  { return (int)vk.swapchain_extent.width; }
int    engine_vulkan_height(void) { return (int)vk.swapchain_extent.height; }

/* ── Vtable ──────────────────────────────────────────────────────────────── */

fwk_render_api fwk_vulkan_render_api = {
    .name        = "Vulkan",
    .init        = engine_vulkan_init,
    .begin_frame = engine_vulkan_begin_frame,
    .clear       = engine_vulkan_clear,
    .end_frame   = engine_vulkan_end_frame,
    .resize      = engine_vulkan_resize,
    .create_texture = engine_vulkan_create_texture,
    .update_texture = engine_vulkan_update_texture,
    .destroy_texture = engine_vulkan_destroy_texture,
    .set_viewport= engine_vulkan_set_viewport,
    .set_blend     = engine_vulkan_set_blend,
    .set_depth     = engine_vulkan_set_depth,
    .set_transform = engine_vulkan_set_transform,
    .draw_line   = engine_vulkan_line,
    .draw_triangle = engine_vulkan_triangle,
    .draw_quad   = engine_vulkan_quad,
    .draw_textured_quad = engine_vulkan_textured_quad,
    .shutdown    = engine_vulkan_shutdown,
};

/* Active backend — NULL means GL (existing path, engine.c manages it) */
fwk_render_api *g_render_api = NULL;
