// #pl-version 1
//
// The NVIDIA DLSS SDK this package builds against. ThirdParty/DLSS is a git submodule of NVIDIA's SDK repository, pinned
// to a release; the package repository itself carries none of NVIDIA's files.
//
// nvngx_dlss.dll is copied next to the plugin so a local build runs. It is not part of the published package, whose
// licence terms do not allow redistributing the SDK on its own; a game that uses DLSS ships it beside its executable.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageDLSS
{
    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The DLSS package is Windows only.");
        }

        var sdkRoot = Path.Combine(context.ProjectRoot, "ThirdParty", "DLSS");
        if (!File.Exists(Path.Combine(sdkRoot, "include", "nvsdk_ngx.h")))
        {
            throw new DirectoryNotFoundException("ThirdParty/DLSS is empty. Run 'git submodule update --init' in the package directory.");
        }

        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "include"));

        // The NGX loader library for the dynamic C runtime the engine uses.
        var libName = context.Configuration == BuildConfiguration.Debug ? "nvsdk_ngx_d_dbg.lib" : "nvsdk_ngx_d.lib";
        rules.PrivateLibraries.Add(Path.Combine(sdkRoot, "lib", "Windows_x86_64", "x64", libName));
        // NGX reads its driver paths from the registry and names its log after the owning window.
        rules.PrivateLibraries.Add("Advapi32.lib");
        rules.PrivateLibraries.Add("User32.lib");

        var dll = Path.Combine(sdkRoot, "lib", "Windows_x86_64", "rel", "nvngx_dlss.dll");
        var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Directory.CreateDirectory(binaryDirectory);
        var target = Path.Combine(binaryDirectory, "nvngx_dlss.dll");
        if (!File.Exists(target) || new FileInfo(target).Length != new FileInfo(dll).Length)
        {
            File.Copy(dll, target, true);
        }
    }
}
