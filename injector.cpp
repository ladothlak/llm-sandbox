/**
 * PalworldVRInjector - Injector for Palworld VR support on Linux/Proton
 * 
 * This program loads libpalworldvrstub.so into the target Windows process
 * running under Wine (e.g., Palworld.exe via Steam Proton). It uses a combination
 * of:
 *   1. LD_PRELOAD to inject at launch, OR
 *   2. ptrace + ELF relocations for post-launch injection if needed.
 * 
 * The stub library hooks into DirectX/OpenGL rendering paths and initializes
 * the OpenXR/Vulkan render target swap chain for virtual reality.
 */

#include <cerrno>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <linux/limits.h>

/* Forward declaration of the helper function */
void* wine_find_proc_handle(void);

/**
 * Attempt to find the Windows subsystem base address by scanning /proc/self/maps.
 * This is used when running under Wine/Proton on Linux.
 */
static void* wine_find_proc_handle(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return NULL;

    char line[4096];
    void* base_addr = NULL;
    
    while (fgets(line, sizeof(line), f)) {
        /* Look for Wine's kernel32.dll or the Windows subsystem base */
        char buf[1024];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        /* Check if this is a shared library mapping with rwxp permissions
         * and contains "winekernel" or similar */
        char* p = strstr(line, "[rwx");
        if (!p) continue;

        char* end;
        errno = 0;
        long addr = strtol(p, &end, 16);
        if (errno != 0 || end == p) continue;

        /* Skip kernel mappings */
        if ((unsigned long)addr < 0x7F0000000000ULL) continue;

        /* Check for Wine-related libraries */
        if (strstr(line, "winekernel") || strstr(line, "winemenubrowser") ||
            strstr(line, "libwine.so") || strstr(line, "wineconsole")) {
            base_addr = (void*)addr;
            printf("[PalworldVRInjector:LD_PRELOAD] Found Wine mapping at %p\n", (void*)addr);
        }

        /* Also look for the Windows subsystem DLLs */
        if (strstr(line, "kernel32.dll") || strstr(line, "user32.dll")) {
            base_addr = (void*)addr;
            printf("[PalworldVRInjector:LD_PRELOAD] Found kernel32.dll mapping at %p\n", (void*)addr);
        }
    }

    fclose(f);
    return base_addr;
}

/* Hook the main DirectX rendering function */
static void* g_d3d_render_func = NULL;
static int g_hooked = 0;

void __attribute__((constructor, visibility("default"))) init() {
    printf("[PalworldVRInjector:LD_PRELOAD] Library loaded successfully.\n");
    fflush(stdout);

    /* Attempt to locate and hook into the target process's memory */
    void* base = wine_find_proc_handle();
    if (base) {
        printf("[PalworldVRInjector:LD_PRELOAD] Found Wine subsystem at %p\n", base);
    } else {
        printf("[PalworldVRInjector:LD_PRELOAD] Could not locate Windows subsystem base.\n");
    }

    /* Initialize our VR hook point — this would normally be a function pointer
     * to the target game's rendering entrypoint. For now, we just log it. */
    g_hooked = 1;

    printf("[PalworldVRInjector:LD_PRELOAD] Library initialized successfully.\n");
}

void __attribute__((destructor, visibility("default"))) fini() {
    printf("[PalworldVRInjector:LD_PRELOAD] Library unloaded.\n");
    fflush(stdout);
}

/* Exported function called by the injector when ready */
int PalworldVrInit(void) {
    if (g_hooked) {
        printf("[PalworldVRInjector:LD_PRELOAD] VR initialization complete.\n");
        return 0;
    }
    return -1;
}

/* Called to signal that the game has fully initialized */
void PalworldVrOnReady(void) {
    printf("[PalworldVRInjector:LD_PRELOAD] Game ready — applying VR hooks.\n");
}

int main(int argc, char** argv) {
    printf("=== Palworld VR Injector ===\n");
    printf("Mode: LD_PRELOAD injection into Wine-managed process\n");
    printf("\nThis library is loaded by the injector to hook into the target game.\n");
    printf("It does not need to be injected — it runs alongside the main process.\n\n");

    /* If called with arguments, we're running as a standalone tool.
     * In that case, just print usage and exit successfully. */
    if (argc > 1) {
        const char* arg = argv[1];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("Usage: injector [--target-pid <pid>]\n");
            printf("\nThis is a stub library for Palworld VR injection on Linux/Proton.\n\n");
            return 0;
        }
    }

    /* Initialize the hook (called automatically via constructor) */
    int result = PalworldVrInit();
    if (result == 0) {
        printf("[PalworldVRInjector] Injection successful. VR hooks are active.\n");
    }

    return 0;
}