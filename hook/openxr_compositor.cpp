/**
 * Palworld VR Injector — OpenXR Compositor (C++)
 * 
 * This module initializes an OpenXR instance, creates a head-tracked session,
 * and manages stereo swap chains for the Oculus Rift CV1 / Link / Oasis headset.
 * It uses WiVRn as the runtime backend for WMR/Oasis support on Linux.
 *
 * Build: g++ -std=c++17 -shared -fPIC -o libpalworldvrhook.so dxgi_hook.cpp openxr_compositor.cpp -lopenxr_loader -lvulkan -lpthread
 */

#include "openxr_compositor.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <chrono>
#include <atomic>

/* Include the real OpenXR and Vulkan headers — these provide all type definitions */
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vulkan/vulkan.h>

/* ---------------------------------------------------------------------------
 * Forward declarations of helper functions (needed before use)
 * -------------------------------------------------------------------------- */

static bool create_vulkan_instance(void);
static bool get_vulkan_physical_device(void);
static bool create_xr_instance(void);
static bool create_xr_session(void);
static bool create_swapchains(void);

/* ---------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

struct vrh_compositor_state_t {
    bool active;
    
    /* Vulkan instance for XR surface creation */
    VkInstance g_vulkan_instance = VK_NULL_HANDLE;
    VkPhysicalDevice g_vulkan_device = VK_NULL_HANDLE;
    VkDevice g_vulkan_device_obj = VK_NULL_HANDLE;
    
    /* OpenXR handles — raw pointers for opaque types */
    XrInstance g_xr_instance = nullptr;
    XrSession g_xr_session = nullptr;
    XrSpace g_xr_refspace = nullptr;
    VkSurfaceKHR g_xr_surface = VK_NULL_HANDLE;
    XrSwapchain* g_swapchains = nullptr;
    uint32_t g_num_swapchains = 0;
    
    /* Frame tracking */
    std::chrono::steady_clock::time_point g_last_frame_time;
    uint64_t g_frame_counter;
};

static struct vrh_compositor_state_t g_composer_state = {};
static std::atomic<bool> g_shutting_down{false};

/* ---------------------------------------------------------------------------
 * Library Entry Points (LD_PRELOAD)
 * -------------------------------------------------------------------------- */

__attribute__((constructor(256), visibility("default")))
void __ld_preload_openxr_init(void) {
    printf("[PalworldVRInjector:OpenXR] Compositor library loaded.\n");
    fflush(stdout);
}

__attribute__((destructor(100), visibility("default")))
void __ld_preload_openxr_fini(void) {
    vrh_destroy_compositor(vrh_compositor_handle_t());
}

/* ---------------------------------------------------------------------------
 * API: Compositor Creation
 * -------------------------------------------------------------------------- */

vrh_compositor_handle_t vrh_create_compositor(void) {
    printf("[OpenXRCompositor] Initializing compositor...\n");

    /* Step 1: Create Vulkan instance for XR surface creation */
    if (!create_vulkan_instance()) {
        fprintf(stderr, "[OpenXRCompositor] Failed to create Vulkan instance.\n");
        return vrh_compositor_handle_t();
    }

    /* Step 2: Create OpenXR instance */
    if (!create_xr_instance()) {
        fprintf(stderr, "[OpenXRCompositor] Failed to create XR instance.\n");
        vkDestroyInstance(g_composer_state.g_vulkan_instance, nullptr);
        return vrh_compositor_handle_t();
    }

    /* Step 3: Create session */
    if (!create_xr_session()) {
        fprintf(stderr, "[OpenXRCompositor] Failed to create XR session.\n");
        xrDestroyInstance(g_composer_state.g_xr_instance);
        vkDestroyDevice(g_composer_state.g_vulkan_device_obj, nullptr);
        return vrh_compositor_handle_t();
    }

    /* Step 4: Create swap chains */
    if (!create_swapchains()) {
        fprintf(stderr, "[OpenXRCompositor] Failed to create swap chains.\n");
        xrDestroySession(g_composer_state.g_xr_session);
        xrDestroyInstance(g_composer_state.g_xr_instance);
        vkDestroyDevice(g_composer_state.g_vulkan_device_obj, nullptr);
        return vrh_compositor_handle_t();
    }

    printf("[OpenXRCompositor] Compositor ready. Frame submission enabled.\n");

    vrh_compositor_handle_t handle;
    handle.opaque_ptr = &g_composer_state;
    return handle;
}

/* ---------------------------------------------------------------------------
 * API: Compositor Destruction
 * -------------------------------------------------------------------------- */

void vrh_destroy_compositor(vrh_compositor_handle_t comp) {
    if (!comp.opaque_ptr) return;

    struct vrh_compositor_state_t* state = static_cast<vrh_compositor_state_t*>(comp.opaque_ptr);

    printf("[OpenXRCompositor] Destroying compositor...\n");

    for (uint32_t i = 0; i < VRHOOK_MAX_VR_DISPLAYS && state->g_swapchains[i]; i++) {
        xrDestroySwapchain(state->g_xr_session, state->g_swapchains[i]);
        state->g_swapchains[i] = nullptr;
    }

    if (state->g_xr_refspace != nullptr) {
        xrDestroySpace(state->g_xr_session, state->g_xr_refspace);
    }

    if (state->g_xr_session != nullptr) {
        xrDestroySession(state->g_xr_session);
    }

    if (state->g_xr_instance != nullptr) {
        xrDestroyInstance(state->g_xr_instance);
    }

    if (state->g_vulkan_device_obj != VK_NULL_HANDLE) {
        vkDestroyDevice(state->g_vulkan_device_obj, nullptr);
    }

    if (state->g_xr_surface != VK_NULL_HANDLE) {
        xrDestroySurfaceKHR(state->g_xr_instance, state->g_xr_surface);
    }

    if (state->g_vulkan_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(state->g_vulkan_instance, nullptr);
    }

    delete[] state->g_swapchains;
    g_composer_state.active = false;
    printf("[OpenXRCompositor] Compositor destroyed.\n");
}

/* ---------------------------------------------------------------------------
 * Helper: Create Vulkan Instance (for XR surface)
 * -------------------------------------------------------------------------- */

static bool create_vulkan_instance(void) {
    void* vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vulkan_lib) {
        fprintf(stderr, "[OpenXRCompositor] Warning: Could not load libvulkan.so.1\n");
        return false;
    }

    PFN_vkCreateInstance vk_create_instance = (PFN_vkCreateInstance)dlsym(vulkan_lib, "vkCreateInstance");
    if (!vk_create_instance) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not find vkCreateInstance\n");
        dlclose(vulkan_lib);
        return false;
    }

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "PalworldVRInjector";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "PalworldVRInjector/OpenXR Compositor";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    const char* extensions[] = { "VK_KHR_portability_enumeration",
                                  "VK_KHR_xr_surface" };
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = extensions;

    g_composer_state.g_vulkan_instance = vk_create_instance(&create_info, nullptr);
    if (!g_composer_state.g_vulkan_instance) {
        fprintf(stderr, "[OpenXRCompositor] Error: vkCreateInstance failed\n");
        dlclose(vulkan_lib);
        return false;
    }

    printf("[OpenXRCompositor] Vulkan instance created.\n");
    dlclose(vulkan_lib);
    return true;
}

/* ---------------------------------------------------------------------------
 * Helper: Get Physical Device
 * -------------------------------------------------------------------------- */

static bool get_vulkan_physical_device(void) {
    if (!g_composer_state.g_vulkan_instance) return false;

    void* vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vulkan_lib) {
        fprintf(stderr, "[OpenXRCompositor] Warning: Could not load libvulkan.so.1\n");
        return false;
    }

    PFN_vkEnumeratePhysicalDevices vk_enumerate = (PFN_vkEnumeratePhysicalDevices)dlsym(vulkan_lib, "vkEnumeratePhysicalDevices");
    if (!vk_enumerate) {
        fprintf(stderr, "[OpenXRCompositor] Warning: Could not find vkEnumeratePhysicalDevices\n");
        dlclose(vulkan_lib);
        return false;
    }

    uint32_t device_count = 0;
    vk_enumerate(g_composer_state.g_vulkan_instance, nullptr, &device_count, nullptr);

    if (device_count == 0) {
        fprintf(stderr, "[OpenXRCompositor] Error: No physical devices found\n");
        dlclose(vulkan_lib);
        return false;
    }

    VkPhysicalDevice* device = new VkPhysicalDevice[device_count];
    vk_enumerate(g_composer_state.g_vulkan_instance, &device_count, device);

    g_composer_state.g_vulkan_device = device[0];
    printf("[OpenXRCompositor] Physical device: %d\n", (int)(uint32_t)g_composer_state.g_vulkan_device);

    delete[] device;
    dlclose(vulkan_lib);
    return true;
}

/* ---------------------------------------------------------------------------
 * Helper: Create OpenXR Instance
 * -------------------------------------------------------------------------- */

static bool create_xr_instance(void) {
    void* loader = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!loader) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not load libopenxr_loader.so\n");
        return false;
    }

    PFN_xrGetInstanceProcAddr xr_get_proc_addr = (PFN_xrGetInstanceProcAddr)dlsym(loader, "xrGetInstanceProcAddr");
    if (!xr_get_proc_addr) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not find xrGetInstanceProcAddr\n");
        dlclose(loader);
        return false;
    }

    g_composer_state.g_xr_instance = (XrInstance)xr_get_proc_addr(nullptr, "xrGetInstanceProcAddr");

    PFN_xrCreateInstance xr_create_instance = (PFN_xrCreateInstance)dlsym(loader, "xrCreateInstance");
    if (!xr_create_instance) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not find XRCreateInstance\n");
        dlclose(loader);
        return false;
    }

    XrApplicationInfo app_info = {};
    app_info.applicationName[0] = 'P';
    app_info.applicationName[1] = 'a';
    app_info.applicationName[2] = 'l';
    app_info.applicationName[3] = 'w';
    app_info.applicationName[4] = 'o';
    app_info.applicationName[5] = 'r';
    app_info.applicationName[6] = 'l';
    app_info.applicationName[7] = 'd';
    app_info.applicationName[8] = '\0';
    app_info.applicationVersion = 1;
    app_info.engineName[0] = 'P';
    app_info.engineName[1] = 'a';
    app_info.engineName[2] = 'l';
    app_info.engineName[3] = 'w';
    app_info.engineName[4] = 'o';
    app_info.engineName[5] = 'r';
    app_info.engineName[6] = 'l';
    app_info.engineName[7] = 'd';
    app_info.engineName[8] = '\0';
    app_info.engineVersion = 1;
    app_info.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstanceCreateInfo xr_create_info = {};
    xr_create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    xr_create_info.applicationInfo = app_info;
    xr_create_info.enabledExtensionCount = 2;

    const char* extensions[] = { "VK_KHR_portability_enumeration",
                                  "VK_KHR_xr_surface" };
    xr_create_info.enabledExtensionNames = extensions;

    g_composer_state.g_xr_instance = nullptr;
    XrResult result = xr_create_instance(&xr_create_info, &g_composer_state.g_xr_instance);

    dlclose(loader);

    if (result != XR_SUCCESS) {
        fprintf(stderr, "[OpenXRCompositor] Error: XRCreateInstance failed with %ld\n", 
                (long)result);
        return false;
    }

    printf("[OpenXRCompositor] OpenXR instance created successfully.\n");
    return true;
}

/* ---------------------------------------------------------------------------
 * Helper: Create Session
 * -------------------------------------------------------------------------- */

static bool create_xr_session(void) {
    if (!g_composer_state.g_xr_instance) return false;

    void* loader = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!loader) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not load libopenxr_loader.so\n");
        return false;
    }

    /* Load function pointers */
    PFN_xrCreateSession xr_create_session = (PFN_xrCreateSession)dlsym(loader, "xrCreateSession");
    if (!xr_create_session) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not find xrCreateSession\n");
        dlclose(loader);
        return false;
    }

    PFN_xrCreateReferenceSpace xr_create_refspace = (PFN_xrCreateReferenceSpace)dlsym(loader, "xrCreateReferenceSpace");
    if (!xr_create_refspace) {
        fprintf(stderr, "[OpenXRCompositor] Warning: Could not find xrCreateReferenceSpace\n");
    }

    /* Create session with a dummy system ID — the runtime will handle validation */
    g_composer_state.g_xr_session = nullptr;
    XrSessionCreateInfo xr_session_info = {};
    xr_session_info.type = XR_TYPE_SESSION_CREATE_INFO;
    xr_session_info.systemId = 0;

    result = xr_create_session(g_composer_state.g_xr_instance, 
                               &xr_session_info, &g_composer_state.g_xr_session);

    dlclose(loader);

    if (result != XR_SUCCESS) {
        fprintf(stderr, "[OpenXRCompositor] Error: XRCreateSession failed with %ld\n", 
                (long)result);
        return false;
    }

    printf("[OpenXRCompositor] Session created successfully.\n");

    /* Create reference space */
    g_composer_state.g_xr_refspace = nullptr;
    XrReferenceSpaceCreateInfo xr_refspace_info = {};
    xr_refspace_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    xr_refspace_info.spaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;

    if (xr_create_refspace) {
        result = xr_create_refspace(g_composer_state.g_xr_session, 
                                    &xr_refspace_info, &g_composer_state.g_xr_refspace);
        if (result != XR_SUCCESS) {
            fprintf(stderr, "[OpenXRCompositor] Warning: XRCreateReferenceSpace failed with %ld\n", 
                    (long)result);
        } else {
            printf("[OpenXRCompositor] Reference space created (LOCAL).\n");
        }
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * Helper: Create Swap Chains
 * -------------------------------------------------------------------------- */

static bool create_swapchains(void) {
    if (!g_composer_state.g_xr_session) return false;

    void* loader = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!loader) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not load libopenxr_loader.so\n");
        return false;
    }

    /* Load xrCreateVulkanSurfaceKHR via dlsym */
    PFN_xrCreateVulkanSurfaceKHR xr_create_surface = 
        (PFN_xrCreateVulkanSurfaceKHR)dlsym(loader, "xrCreateVulkanSurfaceKHR");
    if (!xr_create_surface) {
        fprintf(stderr, "[OpenXRCompositor] Error: Could not find xrCreateVulkanSurfaceKHR\n");
        dlclose(loader);
        return false;
    }

    VkXrVkSurfaceCreateInfoKHR surface_info = {};
    surface_info.type = VK_STRUCTURE_TYPE_XR_VK_SURFACE_CREATE_INFO_KHR;
    surface_info.hInstance = (VkInstance)g_composer_state.g_xr_instance;
    surface_info.hDevice = g_composer_state.g_vulkan_device;

    XrResult result = xr_create_surface(g_composer_state.g_xr_instance,
                                         &surface_info,
                                         &g_composer_state.g_xr_surface);

    dlclose(loader);

    if (result != XR_SUCCESS) {
        fprintf(stderr, "[OpenXRCompositor] Error: xrCreateVulkanSurfaceKHR failed with %ld\n", 
                (long)result);
        return false;
    }

    printf("[OpenXRCompositor] VR surface created successfully.\n");

    g_composer_state.g_swapchains = new XrSwapchain[VRHOOK_MAX_VR_DISPLAYS];
    g_composer_state.g_num_swapchains = 0;

    for (uint32_t eye = 0; eye < VRHOOK_MAX_VR_DISPLAYS; eye++) {
        XrSwapchainCreateInfo swapchain_info = {};
        swapchain_info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | 
                                    XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        swapchain_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        swapchain_info.width  = VR_OUTPUT_WIDTH;
        swapchain_info.height = VR_OUTPUT_HEIGHT;
        swapchain_info.faceCount     = 1;
        swapchain_info.arraySize     = 1;
        swapchain_info.mipCount      = 1;
        swapchain_info.imageFormat    = VK_FORMAT_R8G8B8A8_UNORM;
        swapchain_info.imageUsageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | 
                                         XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        swapchain_info.imageAspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
        swapchain_info.imageCount        = VRHOOK_SWAPCHAIN_IMAGE_COUNT;

        g_composer_state.g_swapchains[eye] = nullptr;
        result = xr_create_swapchain(g_composer_state.g_xr_session, 
                                     &swapchain_info, 
                                     &g_composer_state.g_swapchains[eye]);

        if (result != XR_SUCCESS) {
            fprintf(stderr, "[OpenXRCompositor] Error: xrCreateSwapchain(eye=%u) failed with %ld\n", 
                    eye, (long)result);

            for (uint32_t i = 0; i < eye; i++) {
                if (g_composer_state.g_swapchains[i]) {
                    xrDestroySwapchain(g_composer_state.g_xr_session, g_composer_state.g_swapchains[i]);
                    g_composer_state.g_swapchains[i] = nullptr;
                }
            }
            delete[] g_composer_state.g_swapchains;
            return false;
        }

        printf("[OpenXRCompositor] Swap chain %u created: %ux%u @ %.0f Hz\n", 
               eye, VR_OUTPUT_WIDTH, VR_OUTPUT_HEIGHT, VR_REFRESH_HZ);
        g_composer_state.g_num_swapchains++;
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * API: Frame Submission
 * -------------------------------------------------------------------------- */

vrh_compositor_handle_t vrh_begin_frame(vrh_compositor_handle_t comp) {
    if (!comp.opaque_ptr) return vrh_compositor_handle_t();

    struct vrh_compositor_state_t* state = static_cast<vrh_compositor_state_t*>(comp.opaque_ptr);
    if (!state->active || !state->g_xr_session) return vrh_compositor_handle_t();

    state->g_frame_counter++;

    XrFrameState frame_state = {};
    XrResult result = xr_wait_frame(state->g_xr_session, &frame_state);

    if (result != XR_SUCCESS && result != XR_SESSION_NOT_READY) {
        fprintf(stderr, "[OpenXRCompositor] Warning: xrWaitFrame returned %ld\n", 
                (long)result);
    }

    state->g_last_frame_time = std::chrono::steady_clock::now();

    printf("[OpenXRCompositor] Frame %llu started at %.3f ms\n", 
           (unsigned long long)state->g_frame_counter,
           (double)(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - state->g_last_frame_time).count()) / 1000.0);

    /* Acquire images for both eyes */
    for (uint32_t eye = 0; eye < VRHOOK_MAX_VR_DISPLAYS && state->g_swapchains[eye]; eye++) {
        XrSwapchainImageAcquireInfo acquire_info = {};
        acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

        void* image = nullptr;
        result = xr_acquire_swapchain_image(state->g_swapchains[eye], 
                                             0, &image);

        if (result == XR_SUCCESS) {
            printf("[OpenXRCompositor] Acquired swap chain image %u\n", eye);
        } else if (result != XR_OPERATION_NOT_PROPOSED) {
            fprintf(stderr, "[OpenXRCompositor] Warning: xrAcquireSwapchainImage(%u) = %ld\n",
                    eye, (long)result);
        }
    }

    return comp;
}

void vrh_end_frame(vrh_compositor_handle_t comp) {
    if (!comp.opaque_ptr) return;

    struct vrh_compositor_state_t* state = static_cast<vrh_compositor_state_t*>(comp.opaque_ptr);

    /* Release swap chain images */
    for (uint32_t eye = 0; eye < VRHOOK_MAX_VR_DISPLAYS && state->g_swapchains[eye]; eye++) {
        XrSwapchainImageReleaseInfo release_info = {};
        release_info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

        XrResult result = xr_release_swapchain_image(state->g_swapchains[eye], 
                                                      nullptr, 0);

        if (result != XR_SUCCESS) {
            fprintf(stderr, "[OpenXRCompositor] Warning: xrReleaseSwapchainImage(%u) = %ld\n",
                    eye, (long)result);
        }
    }

    /* End the frame */
    state->g_last_frame_time = std::chrono::steady_clock::now();
}

bool vrh_compositor_is_active(vrh_compositor_handle_t comp) {
    if (!comp.opaque_ptr) return false;
    struct vrh_compositor_state_t* state = static_cast<vrh_compositor_state_t*>(comp.opaque_ptr);
    return state->active;
}

uint32_t vrh_get_current_swapchain_index(vrh_compositor_handle_t comp) {
    if (!comp.opaque_ptr) return 0;
    struct vrh_compositor_state_t* state = static_cast<vrh_compositor_state_t*>(comp.opaque_ptr);
    return (state->g_frame_counter & 1u);
}
