#include <DLSSPlugin/DLSSPluginPCH.h>

#include <DLSSPlugin/Streamline.h>
#include <DLSSPlugin/StreamlineLoader.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <RendererCore/FrameGeneration/FrameGeneration.h>
#include <RendererFoundation/Device/NativeHooks.h>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace
{
  // Identifies the engine to Streamline. NVIDIA issues ids to shipping titles; this one stands for the engine itself.
  constexpr const char* s_szProjectId = "69a72dfc-5690-492c-9e00-3077d5e947b2";

  // Every feature the plugin uses. Streamline loads their plugins (sl.<feature>.dll) from the plugin's directory.
  const sl::Feature s_Features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureDLSS_RR, sl::kFeatureReflex, sl::kFeaturePCL};

  bool s_bInitialized = false;
  void* s_pD3DDevice = nullptr;
  plUInt8 s_AdapterLuid[sizeof(LUID)] = {};
  bool s_bHasAdapterLuid = false;

  plMutex s_Mutex;

  struct TokenEntry
  {
    plUInt64 m_uiFrameId = plUInt64(-1);
    sl::FrameToken* m_pToken = nullptr;
  };
  TokenEntry s_Tokens[8];
  plUInt64 s_uiConstantsFrameId = plUInt64(-1);
  plUInt64 s_uiSceneInputsFrameId = plUInt64(-1);

  plStringBuilder GetPluginDirectory()
  {
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetPluginDirectory), &hSelf);

    wchar_t szPath[MAX_PATH] = {};
    GetModuleFileNameW(hSelf, szPath, MAX_PATH);

    plStringBuilder sPath = plStringUtf8(szPath).GetData();
    sPath.PathParentDirectory();
    return sPath;
  }

  void LogCallback(sl::LogType type, const char* szMessage)
  {
    plStringBuilder sMessage = szMessage;
    sMessage.Trim("\r\n ");
    switch (type)
    {
      case sl::LogType::eError:
        plLog::Error("Streamline: {}", sMessage);
        break;
      case sl::LogType::eWarn:
        plLog::Warning("Streamline: {}", sMessage);
        break;
      default:
        plLog::Debug("Streamline: {}", sMessage);
        break;
    }
  }

  /// Hands the D3D12 device to Streamline, once. Streamline needs it before any of its proxies is used, so this runs from
  /// the device upgrade hook when the plugin was loaded before the device, and after device creation otherwise. Only
  /// NVIDIA adapters: elsewhere Streamline stays unused and wraps nothing.
  bool SetDevice(ID3D12Device* pDevice)
  {
    if (s_pD3DDevice != nullptr)
      return s_pD3DDevice == pDevice;
    if (pDevice == nullptr || !s_bInitialized)
      return false;

    const LUID luid = pDevice->GetAdapterLuid();

    UINT uiVendorId = 0;
    IDXGIFactory4* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory))))
    {
      IDXGIAdapter1* pAdapter = nullptr;
      if (SUCCEEDED(pFactory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&pAdapter))))
      {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(pAdapter->GetDesc1(&desc)))
          uiVendorId = desc.VendorId;
        pAdapter->Release();
      }
      pFactory->Release();
    }

    if (uiVendorId != 0x10DE)
    {
      plLog::Info("DLSS: the GPU is not an NVIDIA GPU; DLSS, DLSS Frame Generation, DLSS Ray Reconstruction and Reflex are unavailable.");
      return false;
    }

    const sl::Result res = slSetD3DDevice(pDevice);
    if (res != sl::Result::eOk)
    {
      plStreamline::LogResult(res, "slSetD3DDevice");
      return false;
    }

    s_pD3DDevice = pDevice;
    plMemoryUtils::Copy(s_AdapterLuid, reinterpret_cast<const plUInt8*>(&luid), sizeof(LUID));
    s_bHasAdapterLuid = true;
    return true;
  }

  void SetDevice(plGALDevice& ref_device)
  {
    const plGALNativeDevice native = ref_device.GetNativeDevice();
    if (native.m_Api == plGALNativeApi::D3D12)
      SetDevice(static_cast<ID3D12Device*>(native.m_pDevice));
  }

  void ShutdownRuntime()
  {
    if (!s_bInitialized)
      return;

    {
      PL_LOCK(s_Mutex);
      for (TokenEntry& entry : s_Tokens)
        entry = TokenEntry();
      s_uiConstantsFrameId = plUInt64(-1);
    }

    plStreamline::LogResult(slShutdown(), "slShutdown");
    s_bInitialized = false;
    s_pD3DDevice = nullptr;
    s_bHasAdapterLuid = false;
  }

  void DeviceEventHandler(const plGALDeviceEvent& e)
  {
    if (e.m_pDevice != plGALDevice::GetDefaultDevice() || !s_bInitialized)
      return;

    if (e.m_Type == plGALDeviceEvent::AfterInit)
    {
      SetDevice(*e.m_pDevice);
    }
    else if (e.m_Type == plGALDeviceEvent::BeforeShutdown)
    {
      // The engine's registries destroyed the backends (and with them every Streamline resource) on this event
      // already; Streamline itself has to be gone before the device.
      ShutdownRuntime();
    }
  }

  /// Lets Streamline see the command queues being created and wrap the swap chain of game windows. With Streamline's
  /// proxy swap chain, DLSS Frame Generation presents its frames and Reflex measures the present.
  class plStreamlineNativeHook : public plGALNativeHook
  {
  public:
    virtual plStringView GetName() const override { return "Streamline"; }

    virtual void* UpgradeDevice(plGALNativeApi::Enum api, void* pNativeDevice) override
    {
      // Streamline must know the device before its proxies are used, and only wraps NVIDIA devices.
      if (api != plGALNativeApi::D3D12 || !SetDevice(static_cast<ID3D12Device*>(pNativeDevice)))
        return nullptr;

      // PL_STREAMLINE_NO_DEVICE_PROXY=1 creates the queues on the native device, for integration problems.
      if (GetEnvironmentVariableW(L"PL_STREAMLINE_NO_DEVICE_PROXY", nullptr, 0) > 0)
        return nullptr;

      // The proxy takes its own reference to the device; the engine releases the proxy with the device.
      void* pProxy = pNativeDevice;
      if (slUpgradeInterface(&pProxy) != sl::Result::eOk || pProxy == pNativeDevice)
        return nullptr;
      return pProxy;
    }

    virtual void* UpgradeFactory(plGALNativeApi::Enum api, void* pFactory, const plGALNativeSwapChainCreateInfo& info) override
    {
      PL_IGNORE_UNUSED(info);

      // Only game windows: the editor's swap chains never present generated frames.
      if (api != plGALNativeApi::D3D12 || !plStreamline::IsReady() || !plFrameGenerationRegistry::IsAllowedInApplication())
        return nullptr;

      // Another vendor's frame generation replaces the swap chain with its own; Streamline's proxy must not sit under it.
      // The registry re-creates the swap chain when that backend is turned off, and Streamline wraps the new one.
      if (plFrameGenerationBackend* pActive = plFrameGenerationRegistry::GetActiveBackend())
      {
        if (!pActive->GetName().IsEqual_NoCase("DLSS") && plFrameGenerationRegistry::GetCapabilities(pActive).m_bReplacesSwapChain)
          return nullptr;
      }

      void* pProxy = pFactory;
      if (slUpgradeInterface(&pProxy) != sl::Result::eOk || pProxy == pFactory)
        return nullptr;
      return pProxy;
    }
  };

  plStreamlineNativeHook* s_pHook = nullptr;
} // namespace

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(DLSSPlugin, Streamline)
  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plStreamline::Startup();
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plStreamline::Shutdown();
  }
PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

void plStreamline::Startup()
{
  if (s_pHook != nullptr)
    return;

  // The hook exists even when Streamline cannot load: it then upgrades nothing.
  s_pHook = PL_DEFAULT_NEW(plStreamlineNativeHook);
  plGALDevice::s_Events.AddEventHandler(DeviceEventHandler);

  const plStringBuilder sPluginDir = GetPluginDirectory();
  if (plStreamlineLoader::Load(sPluginDir).Failed())
    return;

  plStringWChar sPluginDirW(sPluginDir);
  const wchar_t* pluginPaths[] = {sPluginDirW.GetData()};

  sl::Preferences prefs;
  prefs.showConsole = false;
  prefs.logLevel = sl::LogLevel::eDefault;
  prefs.logMessageCallback = &LogCallback;

  // PL_STREAMLINE_LOG=<folder> writes Streamline's verbose log there, for integration problems.
  plStringWChar sLogDirW;
  {
    wchar_t szLogDir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"PL_STREAMLINE_LOG", szLogDir, MAX_PATH) > 0)
    {
      sLogDirW = szLogDir;
      prefs.pathToLogsAndData = sLogDirW.GetData();
      prefs.logLevel = sl::LogLevel::eVerbose;
    }
  }
  prefs.pathsToPlugins = pluginPaths;
  prefs.numPathsToPlugins = 1;
  // Manual hooking: the engine routes queue and swap chain creation through the hook instead of Streamline
  // intercepting every D3D call. No over-the-air updates: the runtime next to the plugin is the one that runs.
  prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  prefs.featuresToLoad = s_Features;
  prefs.numFeaturesToLoad = PL_ARRAY_SIZE(s_Features);
  prefs.engine = sl::EngineType::eCustom;
  prefs.engineVersion = "0.12";
  prefs.projectId = s_szProjectId;
  prefs.renderAPI = sl::RenderAPI::eD3D12;

  const sl::Result res = slInit(prefs, sl::kSDKVersion);
  if (res != sl::Result::eOk)
  {
    LogResult(res, "slInit");
    plStreamlineLoader::Unload();
    return;
  }

  s_bInitialized = true;
  plLog::Info("DLSS: Streamline initialized.");

  // Plugins loaded after the device was created still get it; only the queue hook is missed then.
  if (plGALDevice* pDevice = plGALDevice::GetDefaultDevice())
  {
    plLog::Warning("DLSS: the graphics device already exists, so Streamline could not see its command queues being created. DLSS Frame Generation may be unavailable; load the plugin before the device (a required plugin of the project).");
    SetDevice(*pDevice);
  }
}

void plStreamline::Shutdown()
{
  ShutdownRuntime();

  if (s_pHook != nullptr)
  {
    plGALDevice::s_Events.RemoveEventHandler(DeviceEventHandler);
    PL_DEFAULT_DELETE(s_pHook);
  }

  plStreamlineLoader::Unload();
}

bool plStreamline::IsReady()
{
  return s_bInitialized && s_pD3DDevice != nullptr;
}

void* plStreamline::GetD3DDevice()
{
  return s_pD3DDevice;
}

bool plStreamline::IsFeatureSupported(sl::Feature feature)
{
  if (!IsReady())
    return false;

  sl::AdapterInfo adapter;
  if (s_bHasAdapterLuid)
  {
    adapter.deviceLUID = s_AdapterLuid;
    adapter.deviceLUIDSizeInBytes = sizeof(LUID);
  }

  const sl::Result res = slIsFeatureSupported(feature, adapter);
  if (res != sl::Result::eOk)
  {
    plLog::Info("DLSS: Streamline feature {} is not supported ({}).", feature, static_cast<plUInt32>(res));
    return false;
  }
  return true;
}

const sl::FrameToken* plStreamline::GetFrameToken(plUInt64 uiFrameId)
{
  if (!IsReady())
    return nullptr;

  PL_LOCK(s_Mutex);

  TokenEntry& entry = s_Tokens[uiFrameId % PL_ARRAY_SIZE(s_Tokens)];
  if (entry.m_uiFrameId != uiFrameId || entry.m_pToken == nullptr)
  {
    const uint32_t uiIndex = static_cast<uint32_t>(uiFrameId);
    sl::FrameToken* pToken = nullptr;
    if (slGetNewFrameToken(pToken, &uiIndex) != sl::Result::eOk)
      return nullptr;

    entry.m_uiFrameId = uiFrameId;
    entry.m_pToken = pToken;
  }
  return entry.m_pToken;
}

void plStreamline::SetConstants(plUInt64 uiFrameId, const sl::Constants& constants)
{
  const sl::FrameToken* pToken = GetFrameToken(uiFrameId);
  if (pToken == nullptr)
    return;

  {
    PL_LOCK(s_Mutex);
    if (s_uiConstantsFrameId == uiFrameId)
      return;
    s_uiConstantsFrameId = uiFrameId;
  }

  LogResult(slSetConstants(constants, *pToken, sl::ViewportHandle(s_uiViewport)), "slSetConstants");
}

void plStreamline::Tag(plUInt64 uiFrameId, const sl::ResourceTag* pTags, plUInt32 uiNumTags, void* pCommandList)
{
  const sl::FrameToken* pToken = GetFrameToken(uiFrameId);
  if (pToken == nullptr)
    return;

  LogResult(slSetTagForFrame(*pToken, sl::ViewportHandle(s_uiViewport), pTags, uiNumTags, pCommandList), "slSetTagForFrame");
}

void plStreamline::MarkSceneInputsTagged(plUInt64 uiFrameId)
{
  PL_LOCK(s_Mutex);
  s_uiSceneInputsFrameId = uiFrameId;
}

bool plStreamline::AreSceneInputsTagged(plUInt64 uiFrameId)
{
  PL_LOCK(s_Mutex);
  return s_uiSceneInputsFrameId == uiFrameId;
}

sl::float4x4 plStreamline::ToSL(const plMat4& m)
{
  // Streamline's matrices are row major for row vectors (v * M); the engine's are column vectors (M * v) stored column
  // major. Both are the same 16 floats in memory.
  sl::float4x4 result;
  plMemoryUtils::Copy(reinterpret_cast<float*>(&result), m.m_fElementsCM, 16);
  return result;
}

void plStreamline::FillCameraConstants(sl::Constants& out_constants, const plMat4& viewToClip, const plMat4& clipToView, const plMat4& clipToPrevClip, const plMat4& prevClipToClip, const plVec3& vCameraPos, const plVec3& vForward, const plVec3& vRight, const plVec3& vUp)
{
  out_constants.cameraViewToClip = ToSL(viewToClip);
  out_constants.clipToCameraView = ToSL(clipToView);
  out_constants.clipToPrevClip = ToSL(clipToPrevClip);
  out_constants.prevClipToClip = ToSL(prevClipToClip);
  out_constants.cameraPos = sl::float3(vCameraPos.x, vCameraPos.y, vCameraPos.z);
  out_constants.cameraFwd = sl::float3(vForward.x, vForward.y, vForward.z);
  out_constants.cameraRight = sl::float3(vRight.x, vRight.y, vRight.z);
  out_constants.cameraUp = sl::float3(vUp.x, vUp.y, vUp.z);
}

sl::Constants plStreamline::MakeDefaultConstants(const plVec2& vJitterPixels, bool bReset, float fNear, float fFar, float fFovY, float fAspect)
{
  sl::Constants constants;
  const plMat4 identity = plMat4::MakeIdentity();
  constants.cameraViewToClip = ToSL(identity);
  constants.clipToCameraView = ToSL(identity);
  constants.clipToLensClip = ToSL(identity);
  constants.clipToPrevClip = ToSL(identity);
  constants.prevClipToClip = ToSL(identity);
  constants.cameraPos = sl::float3(0, 0, 0);
  constants.cameraFwd = sl::float3(1, 0, 0);
  constants.cameraRight = sl::float3(0, 1, 0);
  constants.cameraUp = sl::float3(0, 0, 1);
  constants.cameraPinholeOffset = sl::float2(0, 0);

  constants.jitterOffset = sl::float2(vJitterPixels.x, vJitterPixels.y);
  // The engine's velocity is current minus previous in UV units; Streamline wants previous minus current, normalized.
  constants.mvecScale = sl::float2(-1.0f, -1.0f);
  constants.cameraNear = fNear;
  constants.cameraFar = fFar;
  constants.cameraFOV = fFovY;
  constants.cameraAspectRatio = fAspect;
  constants.depthInverted = sl::Boolean::eFalse;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = bReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.orthographicProjection = sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  constants.motionVectorsJittered = sl::Boolean::eFalse;
  return constants;
}

sl::Resource plStreamline::MakeResource(plGALDevice& ref_device, plGALTextureHandle hTexture, bool bUnorderedAccess)
{
  plGALNativeTexture texture;
  if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
    return sl::Resource(sl::ResourceType::eTex2d, nullptr, 0);

  // The render graph left inputs in the compute shader read state and outputs in unordered access.
  const uint32_t uiState = bUnorderedAccess ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return sl::Resource(sl::ResourceType::eTex2d, texture.m_pResource, uiState);
}

void plStreamline::LogResult(sl::Result res, const char* szWhat)
{
  if (res == sl::Result::eOk)
    return;

  // Per-frame calls fail the same way every frame; one line per call site and result is enough.
  static plHybridArray<plUInt64, 16> s_Logged;
  const plUInt64 uiKey = (reinterpret_cast<plUInt64>(szWhat) << 8) ^ static_cast<plUInt64>(res);
  {
    PL_LOCK(s_Mutex);
    if (s_Logged.Contains(uiKey))
      return;
    s_Logged.PushBack(uiKey);
  }

  plLog::Warning("DLSS: {} failed (Streamline result {}).", szWhat, static_cast<plUInt32>(res));
}

PL_STATICLINK_FILE(DLSSPlugin, DLSSPlugin_Streamline);
