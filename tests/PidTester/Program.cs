using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace PidTester
{
    /// <summary>
    /// Test harness to run the stub injector logic against a live Palworld process.
    /// Pass the target PID as an argument: dotnet run -- -p <pid>
    /// </summary>
    class Program
    {
        static void Main(string[] args)
        {
            Console.WriteLine("=== Palworld VR Injector — Live Process Test ===\n");

            // Parse command-line argument for target PID
            int targetPid = 0;
            if (args.Length > 1 && args[0] == "-p" && int.TryParse(args[1], out var pid))
                targetPid = pid;

            if (targetPid == 0)
            {
                Console.Error.WriteLine("[ERROR] Usage: dotnet run -- -p <PID>");
                Environment.Exit(1);
            }

            // Try attaching via kernel32.dll OpenProcess first
            Console.WriteLine($"--- Attempting to attach via kernel32.dll (OpenProcess) ---");
            var handle = OpenProcess(
                ProcessAccessFlags.PROCESS_VM_READ | ProcessAccessFlags.PROCESS_QUERY_INFORMATION,
                false, targetPid);

            if (handle != IntPtr.Zero)
            {
                Console.WriteLine($"[OK] Successfully attached to PID {targetPid} via P/Invoke");
                AttachAndDump(handle, targetPid);
                CloseHandle(handle);
                return;
            }

            // Fallback: read /proc/[pid]/maps directly (Linux-native approach)
            Console.WriteLine("[INFO] OpenProcess failed. Falling back to Linux-native /proc reading...");
            ReadMapsDirectly(targetPid);
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
                return;
            }

            try
            {
                var lines = File.ReadAllLines(mapsPath);
                var modules = new List<string>();

                foreach (var line in lines)
                {
                    var parts = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length >= 6 && (parts[5].EndsWith(".so") || parts[5].EndsWith(".dll")))
                    {
                        modules.Add(parts[parts.Length - 1]);
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
                }
            }
            catch (UnauthorizedAccessException ex)
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

        [DllImport("kernel32.dll")]
        private static extern IntPtr OpenProcess(
            ProcessAccessFlags processDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr hObject);

        enum ProcessAccessFlags : uint
        {
            PROCESS_ALL_ACCESS = 0x1F0FFF,
            PROCESS_VM_READ = 0x0010,
            PROCESS_CREATE_THREAD = 0x0002,
            PROCESS_QUERY_INFORMATION = 0x0400,
            PROCESS_SET_INFORMATION = 0x0200,
        }
    }
}
