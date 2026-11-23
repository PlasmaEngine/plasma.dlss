// #pl-version 2
//
// The NVIDIA Streamline SDK this package builds against. ThirdParty/Streamline is a git submodule of NVIDIA's
// Streamline repository (MIT), pinned to a release tag; the plugin only uses its headers and loads sl.interposer.dll at
// run time, so the plugin binary contains none of NVIDIA's code.
//
// The signed runtime (sl.*.dll, nvngx_dlss*.dll) is not in the repository: it ships in the matching release archive
// (streamline-sdk-<version>.zip from https://github.com/NVIDIA-RTX/Streamline/releases). Point STREAMLINE_SDK_PATH at
// the extracted archive and a local build copies the runtime next to the plugin. The published package never contains
// it; a game that uses DLSS ships those DLLs beside its executable.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageDLSS
{
    /// The Streamline runtime DLLs the plugin loads: the interposer, the common plugin and one plugin per feature, plus
    /// the NGX models they drive.
    private static readonly string[] s_RuntimeDlls =
    {
        "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.dlss_d.dll", "sl.reflex.dll",
        "sl.pcl.dll", "sl.nis.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll",
    };

    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The DLSS package is Windows only.");
        }

        var sdkRoot = Path.Combine(context.ProjectRoot, "ThirdParty", "Streamline");
        if (!File.Exists(Path.Combine(sdkRoot, "include", "sl.h")))
        {
            throw new DirectoryNotFoundException("ThirdParty/Streamline is empty. Run 'git submodule update --init' in the package directory.");
        }

        // Headers only: the signature check in sl_security.h loads WinTrust and Crypt32 itself, and the plugin loads
        // sl.interposer.dll at run time.
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "include"));
        // DXGI finds the vendor of the device's adapter before Streamline is handed the device.
        rules.PrivateLibraries.Add("dxgi.lib");

        CopyRuntime(context);
    }

    private static void CopyRuntime(BuildContext context)
    {
        var runtimeRoot = Environment.GetEnvironmentVariable("STREAMLINE_SDK_PATH");
        if (string.IsNullOrWhiteSpace(runtimeRoot))
        {
            Console.WriteLine("plasma.dlss: STREAMLINE_SDK_PATH is not set, the Streamline runtime is not copied next to the plugin. DLSS stays unavailable until sl.interposer.dll and the feature DLLs are next to the plugin or the executable.");
            return;
        }

        var binDirectory = Path.Combine(runtimeRoot, "bin", "x64");
        if (!File.Exists(Path.Combine(binDirectory, "sl.interposer.dll")))
        {
            Console.WriteLine($"plasma.dlss: no sl.interposer.dll in '{binDirectory}'. STREAMLINE_SDK_PATH must point at an extracted Streamline release archive.");
            return;
        }

        var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Directory.CreateDirectory(binaryDirectory);

        foreach (var name in s_RuntimeDlls)
        {
            var source = Path.Combine(binDirectory, name);
            if (!File.Exists(source))
            {
                continue;
            }

            var target = Path.Combine(binaryDirectory, name);
            if (!File.Exists(target) || new FileInfo(target).Length != new FileInfo(source).Length || File.GetLastWriteTimeUtc(target) < File.GetLastWriteTimeUtc(source))
            {
                File.Copy(source, target, true);
            }
        }
    }
}
