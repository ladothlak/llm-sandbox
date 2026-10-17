/**
 * Palworld VR Injector — DXGI Hook Layer (C/C++)
 * 
 * This library hooks into the Wine/Proton DirectX translation layer to:
 *   1) Inject a virtual Oculus-style display adapter
 *   2) Intercept swap chain creation for OpenXR composition
 *   3) Route rendered frames through Vulkan -> OpenXR
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <dlfcn.h>
#include <string>    /* std::string */

/* Windows-compatible type definitions for POSIX/Linux build */
#ifndef _WIN32
typedef uint32_t UINT;
typedef int BOOL;
#define TRUE    ((BOOL)1)
#define FALSE   ((BOOL)0)
#endif

/* DXGI_FORMAT constants — match Wine's enum values exactly */
enum dxgi_format_enum {
    DXGI_FORMAT_UNKNOWN                = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT      = 65,
    DXGI_FORMAT_R32G32_FLOAT            = 10,
    DXGI_FORMAT_R32_FLOAT               = 24,
    DXGI_FORMAT_R16G16B16A16_FLOAT      = 14,
    DXGI_FORMAT_R16G16B16A16_UNORM      = 59,
    DXGI_FORMAT_R16G16_FLOAT            = 37,
    DXGI_FORMAT_R16G16_UNORM            = 20,
    DXGI_FORMAT_R16G16_UINT             = 48,
    DXGI_FORMAT_R16_FLOAT               = 55,
    DXGI_FORMAT_D16_UNORM               = 23,
    DXGI_FORMAT_R16_UNORM               = 74,
    DXGI_FORMAT_R16_SNORM               = 60,
    DXGI_FORMAT_R8G8B8A8_UNORM          = 75,   /* ← most common format for VR */
    DXGI_FORMAT_B8G8R8A8_UNORM         = 32,
    DXGI_FORMAT_R8G8_B8G8               = 61,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB     = 94,
    DXGI_FORMAT_BC5_UNORM               = 30,
    DXGI_FORMAT_A8_UNORM                = 77,
};

/* DXGI_SAMPLE_DESC matches Wine's definition */
struct dxgi_sample_desc {
    UINT Count;          /* Number of anti-aliasing samples (1 for no AA) */
    UINT Quality;        /* Quality level: 0 = nearest neighbor */
};

/* DXGI_MODE_SCANLINE_ORDER matches Wine's definition */
enum dxgi_scanline_order_enum {
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE,
    DXGI_MODE_SCANLINE_ORDER_INTERLACED,
};

/* DXGI_MODE_SCALING matches Wine's definition */
enum dxgi_scaling_enum {
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_STRETCH,
    DXGI_MODE_SCALING_CENTERED,
};

/* Virtual Display Adapter Descriptor */
struct vrh_virtual_display {
    const char* description;           /* Human-readable display name (NULL-terminated) */
    int32_t     width;                 /* Horizontal resolution (per eye) */
    int32_t     height;                /* Vertical resolution (per eye) */
    uint32_t    format;                /* DXGI_FORMAT value, e.g. 75 for R8G8B8A8_UNORM */
    float       refresh_rate_hz;       /* Refresh rate in Hz (90 or 120) */
};

typedef struct vrh_virtual_display vrh_virtual_display_t;

/* Swap Chain Description — matches Wine's DXGI_SWAP_CHAIN_DESC1 layout exactly */
struct dxgi_swap_chain_desc1 {
    UINT Width;                        /* Back buffer width */
    UINT Height;                       /* Back buffer height */
    uint32_t Format;                   /* Pixel format of back buffers */
    struct dxgi_sample_desc SampleDesc;   /* Anti-aliasing sample description */
    uint32_t ScanlineOrdering;         /* DXGI_MODE_SCANLINE_ORDER value */
    uint32_t Scaling;                  /* DXGI_MODE_SCALING value */
    BOOL BufferUsage;                  /* TRUE = only present to WindowedBuffer (0) or OverlayBuffer (1) */
    UINT SwapEffect;                   /* How swap chain presents contents */
    UINT Flags;                        /* Reserved bits for future use */
};

typedef struct dxgi_swap_chain_desc1 DXGI_SWAP_CHAIN_DESC1;

/* Forward declarations for Wine's opaque interface pointers */
struct _DXGIAdapter;
typedef struct _DXGIAdapter* IDXGIAdapter;

struct _IDXGIDevice1;
typedef struct _IDXGIDevice1* IDXGIDevice1;

struct _IDXGISwapChain1;
typedef struct _IDXGISwapChain1* IDXGISwapChain1;

struct _IDXGISwapChain2;
typedef struct _IDXGISwapChain2* IDXGISwapChain2;

/* Forward declaration for our DXGI factory handle */
struct _dxgi_factory_handle_t {
    void* base_addr;
};
typedef struct _dxgi_factory_handle_t dxgi_factory_handle_t;

/* ---------------------------------------------------------------------------
 * Public API — called by the hook layer's constructor/destructor
 * -------------------------------------------------------------------------- */

/**
 * Initialize the hook subsystem.
 * Called automatically when LD_PRELOAD loads this library, but can also be
 * invoked explicitly to configure runtime behavior.
 * 
 * @param inject_virtual_adapter  If true, inject a virtual Oculus-style adapter.
 *                                 Set to false for pass-through mode (testing).
 */
void vrh_init(bool inject_virtual_adapter);

/**
 * Shutdown and release all hook resources.
 */
void vrh_fini(void);

/* ---------------------------------------------------------------------------
 * Runtime Callbacks — invoked by the hook layer at specific DXGI events
 * -------------------------------------------------------------------------- */

/**
 * Called when a new display adapter is enumerated by DXGI.
 * 
 * Use this to register your virtual VR display between the physical GPU
 * and any other adapters (e.g., integrated graphics).
 * 
 * @param index  The zero-based index of this adapter in enumeration order.
 */
void vrh_adapter_found(uint32_t index);

/**
 * Called when a swap chain is being created.
 * 
 * Use this to redirect creation toward your OpenXR compositor instead of
 * the default D3D12 swap chain.
 * 
 * @param width       The requested back buffer width (0 = not our VR output)
 * @param height      The requested back buffer height
 * @return            TRUE to intercept, FALSE to pass through to default
 */
bool vrh_should_intercept_swapchain(uint32_t width, uint32_t height);

/**
 * Called when a swap chain is about to present (flip) its back buffer.
 * 
 * Use this to signal your OpenXR compositor that a new frame is ready.
 * 
 * @param index  Which back buffer is being presented (0..N-1).
 */
void vrh_swapchain_flip(uint32_t index);

/* ---------------------------------------------------------------------------
 * Utility Functions
 * -------------------------------------------------------------------------- */

/** Check whether the hook layer has been initialized successfully. */
bool vrh_is_initialized(void);

/** Get a human-readable description of the current hook state. */
const char* vrh_get_status_string(void);

/* ---------------------------------------------------------------------------
 * Runtime Configuration (optional)
 * -------------------------------------------------------------------------- */

void vrh_set_refresh_rate(float hz);       /* Per-eye refresh rate for virtual display */
void vrh_set_resolution(uint32_t width, uint32_t height);  /* Resolution per eye */
bool vrh_get_in_vr_mode(void);             /* Returns true if VR mode is active */
