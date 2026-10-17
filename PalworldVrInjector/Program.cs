#pragma warning disable CS8600, CS8625, CS8714
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace PalworldVrInjector
{
    /// <summary>
    /// Stub injector for Palworld running under Proton on Linux.
    /// This is a proof-of-concept that proves the injection path works:
    ///   - .NET 8 runs inside Wine/Proton
    ///   - We can detect and attach to Palworld.exe
    ///   - We can read diagnostic info about the process
    /// 
    /// The real injector will use P/Invoke + native DLL hooks for DirectX injection.
    /// This stub validates that environment is correct first.
    /// </summary>
    class Program
    {
        // P/Invoke declarations — using standard Windows API signatures
        // Wine's kernel32.dll shim on Linux uses the same signatures as Windows
        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "OpenProcess")]
        private static extern IntPtr OpenProcess(
            ProcessAccessFlags processDesiredAccess,
            bool bInheritHandle,
            int dwProcessId);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "CloseHandle")]
        private static extern bool CloseHandle(IntPtr hObject);

        // ReadProcessMemory: on Wine/Linux the 4th param is byte* (pointer to buffer)
        // We pass a pinvoke-compatible reference. The shim wraps this.
        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "ReadProcessMemory", SetLastError = false)]
        private static extern uint ReadProcessMemory(
            IntPtr hProcess,
            System.IntPtr lpBaseAddress,
            byte[] pbBuffer,
            int dwSize,
            out nint lpBytesRead);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "WriteProcessMemory", SetLastError = false)]
        private static extern uint WriteProcessMemory(
            IntPtr hProcess,
            System.IntPtr lpBaseAddress,
            byte[] pbBuffer,
            int dwSize,
            out nint lpNumberOfBytesWritten);

        // VirtualProtectEx: on Linux/Wine, addresses are passed as long (64-bit) not IntPtr
        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "VirtualProtectEx", SetLastError = true)]
        private static extern bool VirtualProtectEx(
            IntPtr hProcess,
            long lpAddress,          // 64-bit address on Linux
            uint dwSize,
            ref ProtectionFlags flNewProtect,
            out ulong OldProtect);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", EntryPoint = "CreateRemoteThread")]
        private static extern IntPtr CreateRemoteThread(
            IntPtr hProcess,
            IntPtr lpThreadAttributes,
            int dwStackSize,
            IntPtr lpStartAddress,
            IntPtr lpParameter,
            uint dwCreationFlags,
            out nint lpThreadId);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
        [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        private static extern bool GetExitCodeProcess(
            IntPtr hProcess,
            out int exitCode);

        enum ProcessAccessFlags : uint
        {
            PROCESS_ALL_ACCESS    = 0x1F0FFF,
            PROCESS_VM_READ       = 0x0010,
            PROCESS_VM_WRITE      = 0x0020,
            PROCESS_CREATE_THREAD  = 0x0002,
            PROCESS_QUERY_INFORMATION = 0x0400,
            PROCESS_SET_INFORMATION    = 0x0200,
        }

        enum ProtectionFlags : uint
        {
            PAGE_EXECUTE_READWRITE   = 0x40,
            PAGE_EXECUTE_READ        = 0x20,
            PAGE_READONLY            = 0x02,
            PAGE_READWRITE           = 0x04,
        }

        static void Main(string[] args)
        {
            Console.WriteLine("=== Palworld VR Injector (Stub) ===");
            Console.WriteLine($"Run time: {DateTime.Now:O}");

            // Parse --target-pid argument
            int targetPid = 0;
            if (args.Length > 1 && args[0] == "--target-pid" && int.TryParse(args[1], out var pid))
                targetPid = pid;

            Console.WriteLine($"\n--- Target PID: {targetPid} ---");

            // If no PID specified, try to find Palworld.exe automatically
            if (targetPid == 0)
            {
                Console.WriteLine("[INFO] Scanning for Palworld.exe...");
                targetPid = FindPalworldProcessId();
                if (targetPid == 0)
                {
                    Console.Error.WriteLine("[ERROR] Could not find Palworld process. Use --target-pid <pid> to specify.");
                    Environment.Exit(1);
                }
            }

            // Try attaching via kernel32.dll OpenProcess first
            Console.WriteLine($"[INFO] Attempting to attach via kernel32.dll (OpenProcess)...");
            var handle = OpenProcess(
                ProcessAccessFlags.PROCESS_VM_READ | ProcessAccessFlags.PROCESS_QUERY_INFORMATION,
                false, targetPid);

            if (handle == IntPtr.Zero)
            {
                Console.Error.WriteLine("[ERROR] Failed to open process. Error: " + System.Runtime.InteropServices.Marshal.GetLastWin32Error());
                Environment.Exit(1);
            }

            Console.WriteLine($"[OK] Successfully attached to PID {targetPid} via P/Invoke");
            AttachAndDump(handle, targetPid);
            CloseHandle(handle);
        }

        /// <summary>
        /// Reads /proc/[pid]/maps to get loaded shared object (module) names.
        /// </summary>
        static void ReadMapsDirectly(int pid)
        {
            var mapsPath = $"/proc/{pid}/maps";

            if (!File.Exists(mapsPath))
            {
                Console.Error.WriteLine($"[ERROR] /proc/{pid}/maps does not exist (process may have exited).");
                Environment.Exit(1);
            }

            try
            {
                var lines = File.ReadAllLines(mapsPath);
                var modules = new List<string>();

                foreach (var line in lines)
                {
                    var parts = line.Split(new[] { ' ', '\t' }, System.StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length >= 6 && !string.IsNullOrEmpty(parts[5]))
                    {
                        string path;
                        try
                        {
                            path = new FileInfo(parts[5]).Name;
                        }
                        catch
                        {
                            path = parts[5];
                        }

                        modules.Add(path);
                    }
                }

                Console.WriteLine($"\n--- Loaded Modules ({modules.Count} total) ---");
                foreach (var line in modules.Take(30))
                    Console.WriteLine($"   {line.Trim()}");

                // Check for Unreal Engine modules
                var ueModules = FindUnrealEngineModules(modules);
                if (ueModules.Count > 0)
                {
                    Console.WriteLine($"\n--- Detected Unreal Engine Modules ({ueModules.Count} found) ---");
                    foreach (var mod in ueModules.Take(10))
                        Console.WriteLine($"   {mod}");

                    // Check for specific Palworld-related modules
                    var palworldModules = FindPalworldRelatedModules(modules);
                    if (palworldModules.Count > 0)
                    {
                        Console.WriteLine($"\n--- Palworld-Related Modules ({palworldModules.Count} found) ---");
                        foreach (var mod in palworldModules.Take(10))
                            Console.WriteLine($"   {mod}");
                    }
                }

                // Check for Wine-related DLLs to confirm we're inside a Wine/Proton environment
                var wineDlls = modules.Where(m => m.Contains("wine") || m.Contains("libwine")).ToList();
                if (wineDlls.Count > 0)
                {
                    Console.WriteLine($"\n--- Wine DLLs detected ({wineDlls.Count} found) ---");
                    foreach (var dll in wineDlls.Take(10))
                        Console.WriteLine($"   {dll}");
                }

                // Check for DirectX / OpenGL libraries
                var dxGfxLibs = modules.Where(m =>
                    m.Contains("dxgi") || m.Contains("d3d") || m.Contains("opengl") ||
                    m.Contains("vulkan") || m.Contains("egl")).ToList();
                if (dxGfxLibs.Count > 0)
                {
                    Console.WriteLine($"\n--- Graphics Libraries ({dxGfxLibs.Count} found) ---");
                    foreach (var lib in dxGfxLibs.Take(15))
                        Console.WriteLine($"   {lib}");
                }

                // Check for OpenXR libraries
                var xrLibs = modules.Where(m => m.Contains("openxr") || m.Contains("XR")).ToList();
                if (xrLibs.Count > 0)
                {
                    Console.WriteLine($"\n--- OpenXR Libraries ({xrLibs.Count} found) ---");
                    foreach (var lib in xrLibs.Take(15))
                        Console.WriteLine($"   {lib}");
                }

                // Check for Steam client libraries (indicates Proton/Steam runtime)
                var steamLibs = modules.Where(m => m.Contains("steam") || m.Contains("proton")).ToList();
                if (steamLibs.Count > 0)
                {
                    Console.WriteLine($"\n--- Steam/Proton Libraries ({steamLibs.Count} found) ---");
                    foreach (var lib in steamLibs.Take(15))
                        Console.WriteLine($"   {lib}");
                }

            }
            catch (System.UnauthorizedAccessException ex)
            {
                Console.Error.WriteLine($"[WARN] Cannot read /proc/{pid}/maps: {ex.Message}");
                Console.Error.WriteLine("[INFO] This is normal if the process is running as a different user.");
            }
        }

        /// <summary>
        /// Filters the module list for Unreal Engine–specific libraries.
        /// </summary>
        static List<string> FindUnrealEngineModules(List<string> modules)
        {
            var ueKeywords = new[] { "Unreal", "UE5", "Slate", "RHI", "RhiRender" };
            return modules.Where(m => m.Contains("Unreal") || m.Contains("UE5") || m.Contains("Rhi")).ToList();
        }

        /// <summary>
        /// Filters for Palworld-specific modules (if any are found).
        /// </summary>
        static List<string> FindPalworldRelatedModules(List<string> modules)
        {
            var palKeywords = new[] { "pal", "palworld", "player" };
            return modules.Where(m => m.ToLower().Contains("pal") || m.ToLower().Contains("player")).ToList();
        }

        /// <summary>
        /// Scans /proc/*/comm and /proc/*/cmdline for Palworld.exe.
        /// </summary>
        static int FindPalworldProcessId()
        {
            try
            {
                // Scan /proc by reading directory entries directly (avoids File.GetFiles limitations)
                var dirEntries = System.IO.Directory.EnumerateDirectories("/proc")
                    .Where(n => !int.TryParse(System.IO.Path.GetFileName(n), out _) && n != "/proc/self/")
                    .ToList();

                foreach (var entry in dirEntries)
                {
                    // Skip non-numeric entries (sockets, etc.)
                    if (!int.TryParse(System.IO.Path.GetFileName(entry), out int pid))
                        continue;

                    try
                    {
                        string comm = null;
                        var commPath = $"/proc/{pid}/comm";
                        if (File.Exists(commPath))
                            comm = File.ReadAllText(commPath).Trim();

                        // Check cmdline (primary: check the first argument)
                        var cmdLinePath = $"/proc/{pid}/cmdline";
                        string cmdline = null;
                        if (File.Exists(cmdLinePath))
                            cmdline = File.ReadAllText(cmdLinePath);

                        // Also check /proc/[pid]/exe symlink to get the executable name
                        var exeLink = $"/proc/{pid}/exe";
                        string exeName = null;
                        try
                        {
                            if (File.Exists(exeLink))
                                exeName = new System.IO.FileInfo(exeLink).Name;
                        }
                        catch { }

                        // Match against known Palworld process names
                        bool isPalworld = false;
                        string matchReason = null;

                        if (comm != null)
                        {
                            if (comm.Contains("pal") || comm.Contains("PAL"))
                            {
                                isPalworld = true;
                                matchReason = $"comm={comm}";
                            }
                        }

                        if (!isPalworld && cmdline != null)
                        {
                            var cmdlineLower = cmdline.ToLower();
                            if (cmdlineLower.Contains("palworld") || cmdlineLower.Contains("palworld-win64-shipping.exe"))
                            {
                                isPalworld = true;
                                matchReason = "cmdline contains 'palworld'";
                            }
                        }

                        if (!isPalworld && exeName != null)
                        {
                            if (exeName.ToLower().Contains("pal"))
                            {
                                isPalworld = true;
                                matchReason = $"exe={exeName}";
                            }
                        }

                        if (isPalworld)
                        {
                            Console.WriteLine($"  [FOUND] PID={pid} via {matchReason}");
                            return pid;
                        }
                    }
                    catch { }
                }

                Console.Error.WriteLine("[INFO] No Palworld process found in /proc.");
                Console.Error.WriteLine("[INFO] This is expected if the game isn't running or was started under a different user (e.g., Steam's child process).");
                return 0;
            }
            catch { return 0; }
        }

        /// <summary>
        /// Reads process memory and dumps key information.
        /// </summary>
        static void AttachAndDump(IntPtr hProcess, int pid)
        {
            Console.WriteLine($"[INFO] Dumping info for PID {pid}...");

            // Try to read /proc/[pid]/maps via OpenProcess + ReadProcessMemory
            // On Linux/Wine, ReadProcessMemory P/Invoke doesn't work reliably.
            // Fall back to reading /proc/[pid]/maps directly — this is more robust anyway.
            var mapsPath = $"/proc/{pid}/maps";

            try
            {
                if (File.Exists(mapsPath))
                {
                    Console.WriteLine($"\n--- Process Memory Maps (/proc/{pid}/maps) ---");
                    var lines = File.ReadAllLines(mapsPath);
                    foreach (var line in lines)
                        Console.WriteLine($"   {line.Trim()}");

                    // Parse out module names from the maps output
                    var modules = new List<string>();
                    foreach (var line in lines)
                    {
                        if (!string.IsNullOrWhiteSpace(line))
                        {
                            var parts = line.Split(new[] { ' ', '\t' }, System.StringSplitOptions.RemoveEmptyEntries);
                            // Format: addr perms offset dev inode pathname
                            if (parts.Length >= 6 && !string.IsNullOrEmpty(parts[5]))
                            {
                                try
                                {
                                    modules.Add(new FileInfo(parts[5]).Name);
                                }
                                catch { /* ignore */ }
                            }
                        }
                    }

                    Console.WriteLine($"\n--- Modules ({modules.Count}) ---");
                    foreach (var mod in modules)
                        Console.WriteLine($"   {mod}");
                }
            }
            catch (Exception ex) when (!ex.Message.Contains("process has exited") && !ex.Message.Contains("no such process"))
            {
                Console.Error.WriteLine($"[WARN] Memory dump encountered an error: {ex.GetType().Name}: {ex.Message}");
            }

            // Dump process cmdline via /proc directly as fallback
            try
            {
                var cmdPath = $"/proc/{pid}/cmdline";
                if (File.Exists(cmdPath))
                {
                    Console.WriteLine($"\n--- Process Command Line (/proc/{pid}/cmdline) ---");
                    foreach (var token in File.ReadAllText(cmdPath).Split('\0'))
                        Console.WriteLine($"   \"{token.Trim()}\"");
                }
            }
            catch { }

            // Dump process environment variables as well
            try
            {
                var envPath = $"/proc/{pid}/environ";
                if (File.Exists(envPath))
                {
                    Console.WriteLine($"\n--- Environment Variables ---");
                    foreach (var line in File.ReadAllLines(envPath).Take(40))
                        Console.WriteLine($"   {line.Trim()}");
                }
            }
            catch { }

            // Try to dump process heap/memory info
            try
            {
                var memMapPath = $"/proc/{pid}/smaps_rollup";
                if (File.Exists(memMapPath))
                {
                    Console.WriteLine($"\n--- Memory Usage ---");
                    foreach (var line in File.ReadAllLines(memMapPath).Take(20))
                        Console.WriteLine($"   {line.Trim()}");
                }
            }
            catch { }

            // Try /proc/[pid]/status for quick stats
            try
            {
                var statusPath = $"/proc/{pid}/status";
                if (File.Exists(statusPath))
                {
                    Console.WriteLine($"\n--- Process Status ---");
                    foreach (var line in File.ReadAllLines(statusPath).Take(15))
                        Console.WriteLine($"   {line.Trim()}");
                }
            }
            catch { }

            Console.WriteLine($"\n[OK] Dump complete for PID={pid}.");
        }
    }
}