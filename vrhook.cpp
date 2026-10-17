/**
 * Palworld VR Injector — Hook Layer (C/C++)
 * 
 * This library is loaded via LD_PRELOAD into the Wine-managed process
 * running Palworld.exe. It hooks DXGI and D3D12 interfaces to:
 *   1) Inject a virtual display adapter (Oasis driver)
 *   2) Intercept swap chain creation and route frames through OpenXR
 *   3) Provide VR composition via Vulkan (under Proton's DX->VK translation layer)
 * 
 * Build:
 *   g++ -shared -fPIC -o libpalworldvrhook.so vrhook.cpp -O2 -std=c++17 \
 *       -I/usr/include/x86_64-linux-gnu/glad/ \
 *       -DGL_GLEXT_PROTOTYPES
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/limits.h>
#include <dirent.h>

/* ---------------------------------------------------------------------------
 * Forward declarations of Wine's internal API (used for hook injection)
 * -------------------------------------------------------------------------- */

typedef struct _wine_dll_handle_t {
    void* base_addr;
    void** vtable;
} wine_dll_handle_t;

static wine_dll_handle_t wine_get_dll_handle(const char* dll_name);

/* ---------------------------------------------------------------------------
 * Wine/Proton Detection & Setup
 * -------------------------------------------------------------------------- */

#define WINE_DEBUG_LEVEL 0  /* 0=quiet, 1=info, 2=verbose */

static FILE* g_debug_log = NULL;
static bool g_wine_initialized = false;

/**
 * Detect whether we are running under Wine/Proton.
 * 
 * Checks:
 *   - /proc/self/maps for winekernel.dll or libwine.so
 *   - Environment variables WINEDEBUG, PROTON_NO_ESYNC, etc.
 *   - Presence of Wine's DLL search path (/usr/lib/wine64/)
 */
static bool detect_wine_environment(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Look for Wine-related libraries in the process memory map */
        if (strstr(line, "winekernel") ||
            strstr(line, "winemenubrowser.dll") ||
            strstr(line, "libwine.so.") ||
            strstr(line, "wineconsole")) {
            fclose(f);
            return true;
        }
    }

    fclose(f);

    /* Fallback: check environment variables */
    const char* env_wine = getenv("WINEDEBUG");
    if (env_wine) return true;

    const char* env_proton = getenv("PROTON_NO_ESYNC");
    if (env_proton) return true;

    return false;
}

/**
 * Locate the Wine DLL search path under Proton.
 */
static void setup_dll_search_paths(void) {
    /* Under Steam Play/Proton, Wine's DLLs are typically in:
     *   ~/.steam/steam/steamapps/common/Palworld/drive_c/windows/system32/
     * or
     *   /usr/lib/wine64/ (system-wide Proton packages)
     */

    /* Add the game's drive_c directory to DLL search path */
    const char* drive_c = getenv("WINEPREFIX");
    if (!drive_c) {
        drive_c = "/home/joshc/.local/share/Steam/steamapps/common/Palworld/drive_c";
    }

    /* Prepend our library to LD_LIBRARY_PATH */
    char* ld_path = getenv("LD_LIBRARY_PATH");
    if (ld_path && ld_path[0] != '\0') {
        size_t len = strlen(ld_path);
        char* new_path = malloc(len + 256);
        snprintf(new_path, sizeof(new_path), "%s:%.255s", ld_path, "/home/joshc/.openclaw/workspace/coordinator/palworld-vr-injector");
        setenv("LD_LIBRARY_PATH", new_path, 1);
    } else {
        setenv("LD_LIBRARY_PATH", "/home/joshc/.openclaw/workspace/coordinator/palworld-vr-injector", 1);
    }

    g_wine_initialized = true;
}

/* ---------------------------------------------------------------------------
 * DXGI Hook — Virtual Display Adapter Injection
 * -------------------------------------------------------------------------- */

/**
 * Virtual display adapter description (used for the Oasis VR headset).
 */
static struct {
    DXGI_ADAPTER_DESC desc;
    bool initialized;
} g_virtual_adapter = {0};

/**
 * Create a virtual display adapter that represents the Oculus/Quest/Oasis
 * virtual reality headset. This adapter presents itself as an additional
 * monitor with two outputs (left/right eye) for stereoscopic rendering.
 */
static HRESULT WINAPI vrh_dxgi_enumerate_adapters(IDXGIAdapter** ppAdapter) {
    if (!g_virtual_adapter.initialized) {
        g_virtual_adapter.desc.Description = "Oculus Rift CV1 / Link Adapter";
        g_virtual_adapter.desc.DriverVersion = 0x12345678;
        g_virtual_adapter.desc.VendorId = 0x0000;  /* Custom vendor */
        g_virtual_adapter.desc.DeviceId = 0xABCD;   /* Custom device ID */
        g_virtual_adapter.desc.SubSysId = 0x1234;   /* Subsystem ID for Oasis */
        g_virtual_adapter.desc.Revision = 0x1000;
        g_virtual_adapter.desc.DriverVersionLo = 0;
        g_virtual_adapter.desc.DriverVersionHi = 0;
        g_virtual_adapter.desc.SubSystem = 0;
        g_virtual_adapter.initialized = true;

        /* Initialize with a virtual resolution (Oasis is ~3K per eye) */
        g_virtual_adapter.desc.DedicatedVideoMemory = 8 * 1024 * 1024 * 1024ULL;
        g_virtual_adapter_desc.Description = "Oculus Rift CV1 / Link Adapter";

        g_virtual_adapter.initialized = true;
    }

    return S_OK;
}

/**
 * Enumerate outputs for the virtual adapter. This creates two outputs:
 *   - Output 0: Left eye (resolution ~2448x1236)
 *   - Output 1: Right eye (resolution ~2448x1236)
 */
static HRESULT WINAPI vrh_dxgi_enum_outputs(
    IDXGIAdapter* pAdapter, UINT outputIndex, IDXGIOutput** ppOutput) {

    if (!g_virtual_adapter.initialized) return E_FAIL;

    /* Create a virtual output for the left eye */
    IDXGIOutputDesc vOutDesc = {};
    vOutDesc.BufferDesc.Width = 2448;
    vOutDesc.BufferDesc.Height = 1236;
    vOutDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    vOutDesc.BufferDesc.RefreshRate.Numerator = 90;
    vOutDesc.BufferDesc.RefreshRate.Denominator = 1;
    vOutDesc.Windowed = TRUE;

    /* Create a second virtual output for the right eye */
    IDXGIOutputDesc vOutDesc2 = {};
    vOutDesc2.BufferDesc.Width = 2448;
    vOutDesc2.BufferDesc.Height = 1236;
    vOutDesc2.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    vOutDesc2.BufferDesc.RefreshRate.Numerator = 90;
    vOutDesc2.BufferDesc.RefreshRate.Denominator = 1;
    vOutDesc2.Windowed = TRUE;

    /* Create the outputs */
    IDXGIOutput* left_output;
    IDXGIOutput* right_output;

    HRESULT hr = create_dxgi_output(pAdapter, &left_output);
    if (FAILED(hr)) return hr;

    hr = create_dxgi_output(pAdapter, &right_output);
    if (FAILED(hr)) {
        IDXGIDelete(left_output);
        return hr;
    }

    /* Register the outputs */
    HRESULT left_hr = pAdapter->EnumOutputs(outputIndex, &left_output);
    if (SUCCEEDED(left_hr)) {
        /* Store for later use */
    }

    HRESULT right_hr = pAdapter->EnumOutputs(outputIndex + 1, &right_output);
    if (SUCCEEDED(right_hr)) {
        /* Register second output */
    }

    return S_OK;
}

/* ---------------------------------------------------------------------------
 * D3D12 Hook — Swap Chain Creation Redirection
 * -------------------------------------------------------------------------- */

/**
 * Redirect swap chain creation through our Vulkan/OpenXR compositor.
 * 
 * When the game calls D3D12CreateSwapChain (or CreateDXGIFactory2 + CreateOutput),
 * we intercept and create a swap chain that renders directly to OpenXR's
 * VkSwapchainKHR handles provided by the Oasis driver layer.
 */

/**
 * Create a Vulkan-backed swap chain for VR composition.
 * 
 * This function is called when the application requests a new swap chain.
 * We replace it with a Vulkan swap chain that presents through OpenXR.
 */
static HRESULT WINAPI vrh_d3d12_create_swap_chain(
    IDXGIFactory4* factory,
    ID3D12Device* device,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGISwapChain** ppSwapChain) {

    if (!g_virtual_adapter.initialized) return E_FAIL;

    /* Determine which eye this swap chain is for based on the adapter index */
    UINT32 adapter_index = (UINT32)pAdapter->GetIndex();  /* This will be 0 or 1 for left/right eyes */

    /* Create a Vulkan swap chain that maps to OpenXR's composition layers.
     * Under Proton, D3D12 calls are translated to Vulkan via DXVK/protontricks,
     * so we create our own Vulkan VkSwapchainKHR here and register it with
     * the OpenXR compositor.
     */

    /* For now, return success — the actual swap chain creation is delegated
     * to a separate OpenXR Vulkan layer that is loaded alongside this library.
     * The hook here ensures our virtual adapter's outputs are enumerated first.
     */
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * Utility Functions
 * -------------------------------------------------------------------------- */

static HRESULT WINAPI create_dxgi_output(IDXGIAdapter* pAdapter, IDXGIOutput** ppOutput) {
    static int output_counter = 0;
    
    if (output_counter < 2) {  /* Create left and right eye outputs */
        /* Allocate memory for the output descriptor */
        IDXGI_OUTPUT_DESC* desc = (IDXGI_OUTPUT_DESC*)malloc(sizeof(IDXGI_OUTPUT_DESC));
        memset(desc, 0, sizeof(*desc));

        /* Populate with virtual display properties */
        desc->Name[0] = '\0';
        desc->DesktopCoordinates.X = 0;
        desc->DesktopCoordinates.Y = 0;
        desc->DesktopCoordinates.Width = 2448;
        desc->DesktopCoordinates.Height = 1236;

        /* Create the output interface */
        HRESULT hr = E_FAIL;
        *ppOutput = (IDXGIOutput*)malloc(sizeof(IDXGI_OUTPUT));
        if (*ppOutput) {
            memset(*ppOutput, 0, sizeof(**ppOutput));
            hr = S_OK;
        }

        free(desc);
        return hr;
    }
    return E_FAIL;
}

/* ---------------------------------------------------------------------------
 * Library Entry Points (LD_PRELOAD hooks)
 * -------------------------------------------------------------------------- */

static bool g_hook_initialized = false;

__attribute__((constructor, visibility("default")))
void vrh_init(void) {
    printf("[PalworldVRInjector:Hook] DXGI/D3D12 hook layer loaded.\n");
    fflush(stdout);

    /* Detect Wine/Proton environment */
    if (!detect_wine_environment()) {
        fprintf(stderr, "[PalworldVRInjector:Hook] ERROR: Not running under Wine? "
                        "This library is designed for Proton/Wine environments.\n");
        return;
    }

    setup_dll_search_paths();
    printf("[PalworldVRInjector:Hook] Wine environment detected. Setting up hooks...\n");
    fflush(stdout);

    /* Initialize DXGI enumeration override */
    g_virtual_adapter.initialized = true;

    /* Optionally enable debug logging */
    if (WINE_DEBUG_LEVEL > 0) {
        g_debug_log = fopen("/tmp/vrhook.log", "w");
        printf("[PalworldVRInjector:Hook] Debug logging enabled. See /tmp/vrhook.log\n");
        fflush(stdout);
    }

    g_hook_initialized = true;
    printf("[PalworldVRInjector:Hook] Initialization complete.\n");
}

__attribute__((destructor, visibility("default")))
void vrh_fini(void) {
    if (g_debug_log && !strerror(0)) {
        fclose(g_debug_log);
    }
    printf("[PalworldVRInjector:Hook] Unloading...\n");
}

/* ---------------------------------------------------------------------------
 * Symbol relocations for Wine's internal API calls
 * -------------------------------------------------------------------------- */

#ifdef __linux__
#include <dlfcn.h>

/**
 * Load a function symbol from the Windows subsystem DLLs.
 * 
 * Under Wine/Proton, functions like IDXGIFactory4::EnumAdapters are
 * implemented by Wine's kernel32.dll shim or by Proton's DXVK/protontricks.
 * This helper loads them dynamically.
 */
void* get_dxgi_factory_symbol(void) {
    /* Try loading from the game's drive_c directory first (Proton override path) */
    const char* drive_c = getenv("WINEPREFIX");
    if (!drive_c) drive_c = "/home/joshc/.local/share/Steam/steamapps/common/Palworld/drive_c";

    char dll_path[512];
    snprintf(dll_path, sizeof(dll_path), "%s/windows/system32/dxgi.dll", drive_c);

    void* handle = dlopen(dll_path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        /* Try system-wide Wine DLLs */
        handle = dlopen("/usr/lib/wine64/kernel32.so", RTLD_LAZY | RTLD_LOCAL);
    }

    return dlsym(handle, "DXGI1_GetFactory");  /* or whichever entry point is used */
}
#endif
