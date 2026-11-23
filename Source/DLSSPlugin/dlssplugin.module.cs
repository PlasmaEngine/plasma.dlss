using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class DLSSPluginModule : ModuleRules
{
    public DLSSPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/DLSSPlugin", "DLSSPluginPCH.h",
            "BUILDSYSTEM_BUILDING_DLSSPLUGIN_LIB");

        // Defines Streamline's signature check (sl_security.h) and the forwarded core API; kept out of the unity files so
        // the Windows and WinTrust headers it pulls in stay in one translation unit.
        UnityBuildExclusions.Add("StreamlineLoader.cpp");

        PlasmaPackageDLSS.ConfigureConsumer(this, context);
    }
}
