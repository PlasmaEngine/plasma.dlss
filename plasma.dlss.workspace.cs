// #pl-version 1
//
// Makes this package directory buildable on its own, outside the engine checkout.

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaDLSSWorkspace : WorkspaceRules
{
    public PlasmaDLSSWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("DLSSPlugin");
    }
}
