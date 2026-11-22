using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class DLSSPluginModule : ModuleRules
{
    public DLSSPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/DLSSPlugin", "DLSSPluginPCH.h",
            "BUILDSYSTEM_BUILDING_DLSSPLUGIN_LIB");

        PlasmaPackageDLSS.ConfigureConsumer(this, context);
    }
}
