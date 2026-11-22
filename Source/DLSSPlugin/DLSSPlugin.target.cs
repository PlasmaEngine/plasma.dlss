using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class DLSSPluginTarget : TargetRules
{
    public DLSSPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plDLSSPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("DLSSPluginModule");
    }
}
