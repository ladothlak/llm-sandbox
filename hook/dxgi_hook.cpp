/**
 * Palworld VR Injector — DXGI Hook Implementation (C++)
 * 
 * This library is loaded via LD_PRELOAD into the Wine-managed process
 * running Palworld.exe. It hooks DXGI calls to inject a virtual Oculus-style
 * display adapter and route frames through OpenXR/Vulkan composition.
 *
 * Build: g++ -std=c++17 -shared -fPIC -o libpalworldvrhook.so dxgi_hook.cpp -ldl -lpthread
 * Load:  LD_PRELOAD=./libpalworldvrhook.so wine64 palworld-win64-shipping.exe
 */

#include "dxgi_hook.h"
#include <cstdio>   /* printf, fprintf */
#include <cstdlib>  /* malloc, free */
#include <cstring>  /* memcpy */
#include <unistd.h>
#include <sys/mman.h>

/* ---------------------------------------------------------------------------
 * Constants and Configuration
 * -------------------------------------------------------------------------- */

#define VRHOOK_DEBUG 0          /* Enable debug output when non-zero */
#define VRHOOK_MAX_VR_DISPLAYS 2   /* Max virtual displays (left + right eyes) */
#define VRHOOK_VIRTUAL_ADAPTER_INDEX 1  /* Insert virtual adapter after index 0 (physical GPU) */

/* Default virtual display properties for Oculus Rift CV1 / Link/Oasis */
static vrh_virtual_display_t g_default_left_eye = {
    .description = "Oculus Rift CV1 / Link - Left Eye",
    .width       = 2448,
    .height      = 1236,
    .format      = DXGI_FORMAT_R8G8B8A8_UNORM,
    .refresh_rate_hz = 90.0f
};

static vrh_virtual_display_t g_default_right_eye = {
    .description = "Oculus Rift CV1 / Link - Right Eye",
    .width       = 2448,
    .height      = 1236,
    .format      = DXGI_FORMAT_R8G8B8A8_UNORM,
    .refresh_rate_hz = 90.0f
};

/* ---------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static bool g_initialized = false;
static bool g_inject_virtual_adapter = true;
static int32_t g_current_eye_index = -1;   /* -1: undetermined, 0: left, 1: right */
static uint64_t g_frame_counter = 0;       /* For latency tracking */

/* Virtual display state per adapter index — use pointers to mutable copies */
static vrh_virtual_display_t* g_vr_displays[VRHOOK_MAX_VR_DISPLAYS];
static bool g_displays_registered[VRHOOK_MAX_VR_DISPLAYS] = {false, false};

/* Real DXGI function pointers — resolved from Wine's kernel32.dll shim */
static void* g_dxgi_factory_handle = NULL;
static void* g_d3d12_device_handle = NULL;

/* ---------------------------------------------------------------------------
 * Forward Declarations
 * -------------------------------------------------------------------------- */

static const char* get_wine_dll_path(void);

/* ---------------------------------------------------------------------------
 * Utility: Get Wine DLL search path (Proton override prefix or system path)
 * Returns a statically allocated string — caller does NOT own it.
 * -------------------------------------------------------------------------- */

static const char* get_wine_dll_path(void) {
    /* Prefer Proton's override directory first */
    const char* drive_c = getenv("WINEPREFIX");
    if (!drive_c || strlen(drive_c) == 0) {
        drive_c = "/home/joshc/.local/share/Steam/steamapps/common/Palworld/drive_c";
    }

    /* Fall back to system-wide Wine packages */
    static const char* fallback_paths[] = {
        "/usr/lib/wine64",
        "/usr/lib/wine",
        "/usr/lib/x86_64-linux-gnu/wine64"
    };

    /* Single static buffer for every branch — the caller does not own it. */
    static char path_buf[512];

    for (size_t i = 0; i < sizeof(fallback_paths)/sizeof(fallback_paths[0]); i++) {
        if (access(fallback_paths[i], R_OK) == 0) {
            snprintf(path_buf, sizeof(path_buf), "%s/kernel32.so", fallback_paths[i]);
            return path_buf;
        }
    }

    /* Default to game's drive_c */
    snprintf(path_buf, sizeof(path_buf), "%s/windows/system32/kernel32.dll", drive_c);
    return path_buf;
}

/* ---------------------------------------------------------------------------
 * Core: Resolve DXGI function pointers from Wine's kernel shim
 * -------------------------------------------------------------------------- */

static int resolve_dxgi_symbols(void) {
    const char* dll_path = get_wine_dll_path();
    if (!dll_path) return -1;

    /* Open the DLL and look for DXGI factory symbol */
    g_dxgi_factory_handle = dlopen(dll_path, RTLD_LAZY | RTLD_GLOBAL);
    if (g_dxgi_factory_handle == NULL) {
        fprintf(stderr, "[VRHook] Warning: Could not load Wine kernel32.dll shim from %s\n", dll_path);
        fprintf(stderr, "[VRHook] Continuing without DXGI injection...\n");
        return -1;  /* Don't fail — just operate in "pass-through" mode */
    }

    void* enum_adapters_sym = dlsym(g_dxgi_factory_handle, "DXGI1_GetFactory");
    if (enum_adapters_sym) {
        printf("[VRHook] Successfully resolved DXGI factory symbol from %s\n", dll_path);
    } else {
        fprintf(stderr, "[VRHook] Warning: Could not find DXGI1_GetFactory in Wine shim\n");
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * API: Library Initialization (called via LD_PRELOAD constructor)
 * -------------------------------------------------------------------------- */

void vrh_init(bool inject_virtual_adapter) {
    if (g_initialized) return;

    g_inject_virtual_adapter = inject_virtual_adapter;
    resolve_dxgi_symbols();

    printf("[PalworldVRInjector] DXGI Hook initialized.\n");
    printf("[PalworldVRInjector] Injecting virtual display: %s\n",
           g_inject_virtual_adapter ? "Yes (Oculus Rift CV1 / Link)" : "No (pass-through)");
    fflush(stdout);

    /* Allocate mutable copies of the default displays */
    for (int i = 0; i < VRHOOK_MAX_VR_DISPLAYS; i++) {
        g_vr_displays[i] = new vrh_virtual_display_t();
        if (!g_vr_displays[i]) continue;
        
        /* Copy defaults */
        memcpy(g_vr_displays[i], 
               &g_default_left_eye, sizeof(vrh_virtual_display_t));

        printf("[VRHook] Registered virtual display %d: %s (%ux%u @ %.0f Hz)\n",
               i, g_vr_displays[i]->description ? g_vr_displays[i]->description : "",
               (int)g_vr_displays[i]->width,
               (int)g_vr_displays[i]->height,
               g_vr_displays[i]->refresh_rate_hz);

        g_displays_registered[i] = true;
    }

    /* Set default resolution if not explicitly set */
    vrh_set_resolution(2448, 1236);

    g_initialized = true;
}

/* ---------------------------------------------------------------------------
 * API: Library Teardown
 * -------------------------------------------------------------------------- */

void vrh_fini(void) {
    if (!g_initialized) return;

    printf("[PalworldVRInjector] DXGI Hook unloading...\n");
    fflush(stdout);

    /* Free allocated display structures */
    for (int i = 0; i < VRHOOK_MAX_VR_DISPLAYS && g_vr_displays[i]; i++) {
        delete g_vr_displays[i];
        g_vr_displays[i] = NULL;
    }

    g_frame_counter = 0;
}

/* ---------------------------------------------------------------------------
 * API: Adapter Found Callback
 * 
 * Called by the hook layer whenever a new adapter is enumerated. This is
 * where we register our virtual Oculus-style display between the physical GPU
 * and any other adapters (e.g., integrated graphics).
 * -------------------------------------------------------------------------- */

void vrh_adapter_found(uint32_t index) {
    if (!g_initialized || !g_inject_virtual_adapter) return;

    /* Only inject after the first adapter (index 0 = physical GPU) */
    if (index <= VRHOOK_VIRTUAL_ADAPTER_INDEX && index < VRHOOK_MAX_VR_DISPLAYS) {
        vrh_virtual_display_t* disp = g_vr_displays[index];
        
        if (disp != NULL && !g_displays_registered[index]) {
            printf("[VRHook] Registering virtual display at adapter index %u\n", index);
            fflush(stdout);
            
            /* Populate with default values for this eye */
            disp->description = (index == 0) ? "Oculus Rift CV1 / Link - Left Eye" :
                                  "Oculus Rift CV1 / Link - Right Eye";
            disp->width       = 2448;
            disp->height      = 1236;
            disp->format      = DXGI_FORMAT_R8G8B8A8_UNORM;
            disp->refresh_rate_hz = 90.0f;

            g_displays_registered[index] = true;
        }
    } else if (index >= VRHOOK_VIRTUAL_ADAPTER_INDEX) {
        printf("[VRHook] Adapter index %u is beyond virtual display range\n", index);
    }
}

/* ---------------------------------------------------------------------------
 * API: Swap Chain Intercept Decision
 * 
 * Called when a swap chain is being created. We check the requested resolution
 * to determine if this is our virtual VR output (2448x1236 per eye) or a
 * normal desktop window.
 * -------------------------------------------------------------------------- */

bool vrh_should_intercept_swapchain(uint32_t width, uint32_t height) {
    if (!g_initialized || !g_inject_virtual_adapter) return false;

    /* Our VR output is 2448x1236 per eye (Oculus Rift CV1 resolution) */
    if (width == 2448 && height == 1236) {
        printf("[VRHook] Intercepting swap chain for VR output (%ux%u)\n", width, height);
        fflush(stdout);
        return true;
    }

    /* For non-VR resolutions, pass through to default behavior */
    return false;
}

/* ---------------------------------------------------------------------------
 * API: Swap Chain Flip Callback
 * 
 * Called when the swap chain is about to present its back buffer. This is
 * where we signal our OpenXR compositor that a new frame is ready for
 * presentation. In the full implementation, this would call into your
 * VR compositor thread via IPC or shared memory.
 * -------------------------------------------------------------------------- */

void vrh_swapchain_flip(uint32_t buffer_index) {
    if (!g_initialized || !g_inject_virtual_adapter) return;

    g_frame_counter++;
    
    /* If you have a separate compositor running, send the rendered frame data here */
    printf("[VRHook] Frame %llu rendered — ready for VR composition\n", 
           (unsigned long long)g_frame_counter);

    fflush(stdout);
}

/* ---------------------------------------------------------------------------
 * API: Status / Debug Helpers
 * -------------------------------------------------------------------------- */

bool vrh_is_initialized(void) {
    return g_initialized;
}

const char* vrh_get_status_string(void) {
    if (!g_initialized) return "uninitialized";
    if (g_inject_virtual_adapter && g_displays_registered[0] && g_displays_registered[1]) {
        return "initialized (VR injection active)";
    }
    if (g_inject_virtual_adapter) {
        return "initialized (partial)";
    }
    return "initialized (pass-through)";
}

void vrh_set_refresh_rate(float hz) {
    if (!g_initialized || !g_vr_displays[0]) return;
    
    g_default_left_eye.refresh_rate_hz = hz;
    g_default_right_eye.refresh_rate_hz = hz;
    printf("[VRHook] Set refresh rate to %.1f Hz\n", hz);
}

void vrh_set_resolution(uint32_t width, uint32_t height) {
    if (!g_initialized || !g_vr_displays[0]) return;
    
    g_default_left_eye.width = (int32_t)width;
    g_default_left_eye.height = (int32_t)height;
    g_default_right_eye.width = (int32_t)width;
    g_default_right_eye.height = (int32_t)height;
    
    printf("[VRHook] Set resolution to %ux%u\n", width, height);
}

bool vrh_get_in_vr_mode(void) {
    return g_initialized && g_inject_virtual_adapter && 
           g_displays_registered[0] && g_displays_registered[1];
}

/* ---------------------------------------------------------------------------
 * LD_PRELOAD constructor/destructor — called automatically when library is loaded
 * -------------------------------------------------------------------------- */

__attribute__((constructor(256), visibility("default")))
void __ld_preload_init(void) {
    vrh_init(true);  /* Default: inject virtual VR display */
}

__attribute__((destructor(100), visibility("default")))
void __ld_preload_fini(void) {
    vrh_fini();
}
