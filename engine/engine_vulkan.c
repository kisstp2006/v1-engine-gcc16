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
 *   Linux:   -I engine/vendor/vulkan -lvulkan -lX11
 *   Windows: -I engine/vendor/vulkan -lvulkan-1 (or link vulkan-1.lib)
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
extern void   glfwPollEvents(void);
extern int    glfwWindowShouldClose(GLFWwindow* window);
extern void   glfwWindowHint(int hint, int value);
extern GLFWwindow* glfwCreateWindow(int w, int h, const char* title,
                                    GLFWmonitor* monitor, GLFWwindow* share);
extern int    glfwVulkanSupported(void);
extern const char** glfwGetRequiredInstanceExtensions(uint32_t* count);
extern int    glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window,
                                      const VkAllocationCallbacks* alloc,
                                      VkSurfaceKHR* surface);
/* GLFW window hint constants needed for Vulkan */
#ifndef GLFW_CLIENT_API
#  define GLFW_CLIENT_API 0x00022001
#  define GLFW_NO_API     0
#  define GLFW_RESIZABLE  0x00020003
#endif

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

/* ── Internal state ──────────────────────────────────────────────────────── */

static struct {
    GLFWwindow           *window;
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

    VkRenderPass          render_pass;
    VkPipelineLayout      pipeline_layout;
    VkPipeline            clear_pipeline;
    VkShaderModule        vert_module;
    VkShaderModule        frag_module;

    VkCommandPool         cmd_pool;
    VkCommandBuffer      *cmd_buffers;
    VkSemaphore          *image_available;
    VkSemaphore          *render_finished;
    VkFence              *in_flight;
    uint32_t              frame_index;

    float                 pending_clear[4]; /* RGBA — set by engine_vulkan_clear() */
    bool                  has_pending_clear;
    bool                  initialized;
} vk;

#define VK_MAX_FRAMES_IN_FLIGHT 2

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

static VkShaderModule create_shader_module(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(vk.device, &ci, NULL, &mod);
    return mod;
}

/* ── Instance creation ───────────────────────────────────────────────────── */

static bool create_instance(void) {
    /* Gather GLFW's required extensions */
    uint32_t glfw_ext_count = 0;
    const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "Nebula Engine",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "Nebula",
        .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion         = VK_API_VERSION_1_2,
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

static bool pick_physical_device(void) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, NULL);
    if (count == 0) { fprintf(stderr, "[Vulkan] No Vulkan devices found\n"); return false; }

    VkPhysicalDevice *devices = malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(vk.instance, &count, devices);

    /* Prefer discrete GPU, otherwise use first available */
    vk.physical_device = devices[0];
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            vk.physical_device = devices[i];
            break;
        }
    }
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physical_device, &props);
    fprintf(stderr, "[Vulkan] Using GPU: %s\n", props.deviceName);
    return true;
}

static bool find_queue_families(void) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &count, NULL);
    VkQueueFamilyProperties *families = malloc(count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &count, families);

    vk.graphics_family = UINT32_MAX;
    vk.present_family  = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            vk.graphics_family = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical_device, i, vk.surface, &present);
        if (present) vk.present_family = i;
        if (vk.graphics_family != UINT32_MAX && vk.present_family != UINT32_MAX) break;
    }
    free(families);
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

    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDeviceFeatures features = {0};

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = family_count,
        .pQueueCreateInfos       = queue_infos,
        .enabledExtensionCount   = 1,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures        = &features,
    };
    VK_CHECK(vkCreateDevice(vk.physical_device, &ci, NULL, &vk.device));
    vkGetDeviceQueue(vk.device, vk.graphics_family, 0, &vk.graphics_queue);
    vkGetDeviceQueue(vk.device, vk.present_family,  0, &vk.present_queue);
    return true;
}

/* ── Swapchain ───────────────────────────────────────────────────────────── */

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &caps);

    /* Format: prefer B8G8R8A8_SRGB + SRGB_NONLINEAR */
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(fmt_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &fmt_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i]; break;
        }
    }
    free(formats);
    vk.swapchain_format = chosen.format;

    /* Present mode: prefer mailbox (low-latency), fallback FIFO */
    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, vk.surface, &pm_count, NULL);
    VkPresentModeKHR *modes = malloc(pm_count * sizeof(VkPresentModeKHR));
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, vk.surface, &pm_count, modes);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < pm_count; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) { present_mode = modes[i]; break; }
    free(modes);

    /* Extent */
    if (caps.currentExtent.width != UINT32_MAX) {
        vk.swapchain_extent = caps.currentExtent;
    } else {
        vk.swapchain_extent.width  = (uint32_t)vk.width;
        vk.swapchain_extent.height = (uint32_t)vk.height;
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

    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, NULL);
    vk.images = malloc(vk.image_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images);

    /* Image views */
    vk.image_views = malloc(vk.image_count * sizeof(VkImageView));
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
    VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &colour,
        .subpassCount    = 1, .pSubpasses   = &subpass,
        .dependencyCount = 1, .pDependencies= &dep,
    };
    VK_CHECK(vkCreateRenderPass(vk.device, &ci, NULL, &vk.render_pass));
    return true;
}

/* ── Clear pipeline (fullscreen triangle + push constant colour) ─────────── */

static bool create_clear_pipeline(void) {
    vk.vert_module = create_shader_module(g_clear_vert_spv, g_clear_vert_spv_size);
    vk.frag_module = create_shader_module(g_clear_frag_spv, g_clear_frag_spv_size);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vk.vert_module, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = vk.frag_module, .pName = "main" },
    };

    VkPipelineVertexInputStateCreateInfo   vi  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia  = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = { .x=0,.y=0,
        .width=(float)vk.swapchain_extent.width,
        .height=(float)vk.swapchain_extent.height,
        .minDepth=0,.maxDepth=1 };
    VkRect2D scissor = { .offset={0,0}, .extent=vk.swapchain_extent };
    VkPipelineViewportStateCreateInfo vs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend_att = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                          VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_att };

    /* Push constant: 4 floats for clear colour */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = 4 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc_range };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &layout_ci, NULL, &vk.pipeline_layout));

    VkGraphicsPipelineCreateInfo ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2, .pStages      = stages,
        .pVertexInputState   = &vi, .pInputAssemblyState = &ia,
        .pViewportState      = &vs, .pRasterizationState = &rs,
        .pMultisampleState   = &ms, .pColorBlendState    = &cbs,
        .layout              = vk.pipeline_layout,
        .renderPass          = vk.render_pass,
        .subpass             = 0,
    };
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &ci, NULL, &vk.clear_pipeline));
    return true;
}

/* ── Framebuffers ────────────────────────────────────────────────────────── */

static bool create_framebuffers(void) {
    vk.framebuffers = malloc(vk.image_count * sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < vk.image_count; i++) {
        VkFramebufferCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = vk.render_pass,
            .attachmentCount = 1, .pAttachments = &vk.image_views[i],
            .width           = vk.swapchain_extent.width,
            .height          = vk.swapchain_extent.height,
            .layers          = 1,
        };
        VK_CHECK(vkCreateFramebuffer(vk.device, &ci, NULL, &vk.framebuffers[i]));
    }
    return true;
}

/* ── Command pool + buffers + sync objects ───────────────────────────────── */

static bool create_command_infra(void) {
    VkCommandPoolCreateInfo pool_ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk.graphics_family,
    };
    VK_CHECK(vkCreateCommandPool(vk.device, &pool_ci, NULL, &vk.cmd_pool));

    vk.cmd_buffers     = malloc(VK_MAX_FRAMES_IN_FLIGHT * sizeof(VkCommandBuffer));
    vk.image_available = malloc(VK_MAX_FRAMES_IN_FLIGHT * sizeof(VkSemaphore));
    vk.render_finished = malloc(VK_MAX_FRAMES_IN_FLIGHT * sizeof(VkSemaphore));
    vk.in_flight       = malloc(VK_MAX_FRAMES_IN_FLIGHT * sizeof(VkFence));

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

/* ── Public lifecycle ────────────────────────────────────────────────────── */

bool engine_vulkan_init(void *glfw_window, int width, int height) {
    memset(&vk, 0, sizeof(vk));
    vk.window = (GLFWwindow*)glfw_window;
    vk.width  = width;
    vk.height = height;

    if (!glfwVulkanSupported()) {
        fprintf(stderr, "[Vulkan] Vulkan not supported by GLFW\n");
        return false;
    }

    if (!create_instance())       return false;
    /* Surface must be created before pick_physical_device (for present support query) */
    if (glfwCreateWindowSurface(vk.instance, vk.window, NULL, &vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Failed to create window surface\n");
        return false;
    }
    if (!pick_physical_device())  return false;
    if (!find_queue_families())   return false;
    if (!create_device())         return false;
    if (!create_swapchain())      return false;
    if (!create_render_pass())    return false;
    if (!create_clear_pipeline()) return false;
    if (!create_framebuffers())   return false;
    if (!create_command_infra())  return false;

    vk.initialized = true;
    fprintf(stderr, "[Vulkan] Initialised (%dx%d, %u swapchain images)\n",
            width, height, vk.image_count);
    return true;
}

void engine_vulkan_shutdown(void) {
    if (!vk.initialized) return;
    vkDeviceWaitIdle(vk.device);

    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(vk.device, vk.image_available[i], NULL);
        vkDestroySemaphore(vk.device, vk.render_finished[i],  NULL);
        vkDestroyFence(vk.device, vk.in_flight[i], NULL);
    }
    vkDestroyCommandPool(vk.device, vk.cmd_pool, NULL);
    for (uint32_t i = 0; i < vk.image_count; i++)
        vkDestroyFramebuffer(vk.device, vk.framebuffers[i], NULL);
    vkDestroyPipeline(vk.device, vk.clear_pipeline, NULL);
    vkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
    vkDestroyShaderModule(vk.device, vk.vert_module, NULL);
    vkDestroyShaderModule(vk.device, vk.frag_module, NULL);
    vkDestroyRenderPass(vk.device, vk.render_pass, NULL);
    for (uint32_t i = 0; i < vk.image_count; i++)
        vkDestroyImageView(vk.device, vk.image_views[i], NULL);
    vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vkDestroyDevice(vk.device, NULL);
    vkDestroyInstance(vk.instance, NULL);

    free(vk.images); free(vk.image_views); free(vk.framebuffers);
    free(vk.cmd_buffers); free(vk.image_available);
    free(vk.render_finished); free(vk.in_flight);

    memset(&vk, 0, sizeof(vk));
}

void engine_vulkan_clear(float r, float g, float b, float a) {
    vk.pending_clear[0] = r; vk.pending_clear[1] = g;
    vk.pending_clear[2] = b; vk.pending_clear[3] = a;
    vk.has_pending_clear = true;
}

void engine_vulkan_begin_frame(void) {
    if (!vk.initialized) return;
    vkWaitForFences(vk.device, 1, &vk.in_flight[vk.frame_index], VK_TRUE, UINT64_MAX);
    vkResetFences(vk.device, 1, &vk.in_flight[vk.frame_index]);
    vk.has_pending_clear = false;
}

void engine_vulkan_end_frame(void) {
    if (!vk.initialized) return;

    uint32_t image_index;
    VkResult acq = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                          vk.image_available[vk.frame_index],
                                          VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) return;
    if (acq != VK_SUCCESS) return;

    VkCommandBuffer cmd = vk.cmd_buffers[vk.frame_index];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &begin_info);

    /* Clear colour (cornflower blue default) */
    float clear_r = vk.pending_clear[0] > 0 ? vk.pending_clear[0] : 0.392f;
    float clear_g = vk.pending_clear[1] > 0 ? vk.pending_clear[1] : 0.584f;
    float clear_b = vk.pending_clear[2] > 0 ? vk.pending_clear[2] : 0.929f;
    VkClearValue clear_val = { .color = {{ clear_r, clear_g, clear_b, 1.0f }} };

    VkRenderPassBeginInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = vk.render_pass,
        .framebuffer     = vk.framebuffers[image_index],
        .renderArea      = { .offset = {0,0}, .extent = vk.swapchain_extent },
        .clearValueCount = 1,
        .pClearValues    = &clear_val,
    };
    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    /* Draw fullscreen triangle via clear pipeline (push constant colour) */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.clear_pipeline);
    float pc[4] = { clear_r, clear_g, clear_b, 1.0f };
    vkCmdPushConstants(cmd, vk.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1, .pWaitSemaphores   = &vk.image_available[vk.frame_index],
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1, .pCommandBuffers   = &cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.render_finished[vk.frame_index],
    };
    vkQueueSubmit(vk.graphics_queue, 1, &submit, vk.in_flight[vk.frame_index]);

    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.render_finished[vk.frame_index],
        .swapchainCount     = 1, .pSwapchains     = &vk.swapchain,
        .pImageIndices      = &image_index,
    };
    vkQueuePresentKHR(vk.present_queue, &present);
    vk.frame_index = (vk.frame_index + 1) % VK_MAX_FRAMES_IN_FLIGHT;
}

bool engine_vulkan_init_window(const char *title, int width, int height) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  1);
    GLFWwindow *win = glfwCreateWindow(width, height,
                                       title ? title : "Nebula (Vulkan)",
                                       NULL, NULL);
    if (!win) { fprintf(stderr, "[Vulkan] glfwCreateWindow failed\n"); return false; }
    return engine_vulkan_init(win, width, height);
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
    .shutdown    = engine_vulkan_shutdown,
    .begin_frame = engine_vulkan_begin_frame,
    .end_frame   = engine_vulkan_end_frame,
    .clear       = engine_vulkan_clear,
};

/* Active backend — NULL means GL (existing path, engine.c manages it) */
fwk_render_api *g_render_api = NULL;
