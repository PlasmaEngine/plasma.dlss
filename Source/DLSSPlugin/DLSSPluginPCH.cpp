#include <DLSSPlugin/DLSSPluginPCH.h>

#include <Foundation/Configuration/Plugin.h>

PL_STATICLINK_LIBRARY(DLSSPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(DLSSPlugin_DLSSFrameGeneration);
  PL_STATICLINK_REFERENCE(DLSSPlugin_DLSSUpscaler);
  PL_STATICLINK_REFERENCE(DLSSPlugin_Streamline);
  PL_STATICLINK_REFERENCE(DLSSPlugin_StreamlineLoader);
}
