#include <DLSSPlugin/DLSSPluginPCH.h>

#include <DLSSPlugin/StreamlineLoader.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/StringConversion.h>

#include <sl.h>

// sl_security.h defines its signature check in the header; this is the one file that includes it. It passes NULL for
// integer handle parameters, which only compiles with the C definition of NULL, not the engine's nullptr.
#pragma push_macro("NULL")
#undef NULL
#define NULL 0
#include <sl_security.h>
#pragma pop_macro("NULL")

// The Streamline core API, forwarded to the sl.interposer.dll this plugin loaded. The SDK declares these functions
// extern "C" without dllimport, so defining them here lets the SDK's inline feature helpers (sl_dlss.h and the others,
// which call slGetFeatureFunction) work without linking sl.interposer.lib.

namespace
{
  HMODULE s_hInterposer = nullptr;

#define PL_SL_FUNCTIONS(MACRO)  \
  MACRO(slInit)                 \
  MACRO(slShutdown)             \
  MACRO(slIsFeatureSupported)   \
  MACRO(slIsFeatureLoaded)      \
  MACRO(slSetFeatureLoaded)     \
  MACRO(slEvaluateFeature)      \
  MACRO(slAllocateResources)    \
  MACRO(slFreeResources)        \
  MACRO(slSetTagForFrame)       \
  MACRO(slGetFeatureRequirements) \
  MACRO(slGetFeatureVersion)    \
  MACRO(slUpgradeInterface)     \
  MACRO(slSetConstants)         \
  MACRO(slGetNativeInterface)   \
  MACRO(slGetFeatureFunction)   \
  MACRO(slGetNewFrameToken)     \
  MACRO(slSetD3DDevice)

#define PL_SL_DECLARE(name) PFun_##name* s_pfn_##name = nullptr;
  PL_SL_FUNCTIONS(PL_SL_DECLARE)
#undef PL_SL_DECLARE
} // namespace

plResult plStreamlineLoader::Load(plStringView sDirectory)
{
  if (s_hInterposer != nullptr)
    return PL_SUCCESS;

  plStringBuilder sPath = sDirectory;
  sPath.AppendPath("sl.interposer.dll");
  sPath.MakePathSeparatorsNative();
  plStringWChar sPathW(sPath);

  if (GetFileAttributesW(sPathW.GetData()) == INVALID_FILE_ATTRIBUTES)
  {
    plLog::Info("DLSS: '{}' does not exist. Place the Streamline runtime (sl.*.dll, nvngx_dlss*.dll) next to the plugin to enable DLSS.", sPath);
    return PL_FAILURE;
  }

  // Only NVIDIA-signed Streamline binaries are loaded.
  if (!sl::security::verifyEmbeddedSignature(sPathW.GetData()))
  {
    plLog::Warning("DLSS: '{}' is not signed by NVIDIA; Streamline is not loaded.", sPath);
    return PL_FAILURE;
  }

  s_hInterposer = LoadLibraryW(sPathW.GetData());
  if (s_hInterposer == nullptr)
  {
    plLog::Warning("DLSS: loading '{}' failed (error {}).", sPath, static_cast<plUInt32>(GetLastError()));
    return PL_FAILURE;
  }

  bool bComplete = true;
#define PL_SL_RESOLVE(name)                                                                        \
  s_pfn_##name = reinterpret_cast<PFun_##name*>(GetProcAddress(s_hInterposer, #name));             \
  if (s_pfn_##name == nullptr)                                                                     \
  {                                                                                                \
    plLog::Warning("DLSS: sl.interposer.dll has no '{}'.", #name);                                 \
    bComplete = false;                                                                             \
  }
  PL_SL_FUNCTIONS(PL_SL_RESOLVE)
#undef PL_SL_RESOLVE

  if (!bComplete)
  {
    Unload();
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plStreamlineLoader::Unload()
{
#define PL_SL_CLEAR(name) s_pfn_##name = nullptr;
  PL_SL_FUNCTIONS(PL_SL_CLEAR)
#undef PL_SL_CLEAR

  if (s_hInterposer != nullptr)
  {
    FreeLibrary(s_hInterposer);
    s_hInterposer = nullptr;
  }
}

bool plStreamlineLoader::IsLoaded()
{
  return s_hInterposer != nullptr;
}

// Every forwarder fails cleanly while the interposer is not loaded.
#define PL_SL_CALL(name, ...) (s_pfn_##name != nullptr ? s_pfn_##name(__VA_ARGS__) : sl::Result::eErrorNotInitialized)

sl::Result slInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
  return PL_SL_CALL(slInit, pref, sdkVersion);
}

sl::Result slShutdown()
{
  return s_pfn_slShutdown != nullptr ? s_pfn_slShutdown() : sl::Result::eErrorNotInitialized;
}

sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
  return PL_SL_CALL(slIsFeatureSupported, feature, adapterInfo);
}

sl::Result slIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
  return PL_SL_CALL(slIsFeatureLoaded, feature, loaded);
}

sl::Result slSetFeatureLoaded(sl::Feature feature, bool loaded)
{
  return PL_SL_CALL(slSetFeatureLoaded, feature, loaded);
}

sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer)
{
  return PL_SL_CALL(slEvaluateFeature, feature, frame, inputs, numInputs, cmdBuffer);
}

sl::Result slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport)
{
  return PL_SL_CALL(slAllocateResources, cmdBuffer, feature, viewport);
}

sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport)
{
  return PL_SL_CALL(slFreeResources, feature, viewport);
}

sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer* cmdBuffer)
{
  return PL_SL_CALL(slSetTagForFrame, frame, viewport, resources, numResources, cmdBuffer);
}

sl::Result slGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements)
{
  return PL_SL_CALL(slGetFeatureRequirements, feature, requirements);
}

sl::Result slGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version)
{
  return PL_SL_CALL(slGetFeatureVersion, feature, version);
}

sl::Result slUpgradeInterface(void** baseInterface)
{
  return PL_SL_CALL(slUpgradeInterface, baseInterface);
}

sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport)
{
  return PL_SL_CALL(slSetConstants, values, frame, viewport);
}

sl::Result slGetNativeInterface(void* proxyInterface, void** baseInterface)
{
  return PL_SL_CALL(slGetNativeInterface, proxyInterface, baseInterface);
}

sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
  return PL_SL_CALL(slGetFeatureFunction, feature, functionName, function);
}

sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex)
{
  return PL_SL_CALL(slGetNewFrameToken, token, frameIndex);
}

sl::Result slSetD3DDevice(void* d3dDevice)
{
  return PL_SL_CALL(slSetD3DDevice, d3dDevice);
}

#undef PL_SL_CALL

PL_STATICLINK_FILE(DLSSPlugin, DLSSPlugin_StreamlineLoader);
