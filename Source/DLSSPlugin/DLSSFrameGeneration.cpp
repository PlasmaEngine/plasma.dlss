#include <DLSSPlugin/DLSSPluginPCH.h>

#include <DLSSPlugin/Streamline.h>
#include <RendererCore/FrameGeneration/FrameGeneration.h>
#include <RendererCore/FrameGeneration/LowLatency.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

/// NVIDIA DLSS Frame Generation (and Multi Frame Generation) through Streamline. Streamline's proxy swap chain presents
/// the generated frames; it wraps every game window swap chain while the plugin is loaded (plStreamline's native hook),
/// so turning frame generation on or off only changes the DLSS-G mode. Needs an RTX 40 series GPU or newer (3x and more:
/// RTX 50 series) and Reflex.
class plFrameGenerationBackendDLSS : public plFrameGenerationBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plFrameGenerationBackendDLSS, plFrameGenerationBackend);

public:
  virtual plStringView GetName() const override { return "DLSS"; }
  virtual plStringView GetDisplayName() const override { return "NVIDIA DLSS Frame Generation"; }
  virtual plInt32 GetAutoPriority() const override { return 40; }

  virtual plFrameGenerationCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings) override;
  virtual void ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params) override;
  virtual void ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params) override;
  virtual plFrameGenerationStats GetStats() const override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  bool m_bEnabled = false;
};

/// NVIDIA Reflex through Streamline: paces the CPU so frames wait less in the GPU queue, and reports the measured
/// latency (PC Latency stats). Required by DLSS Frame Generation.
class plLowLatencyBackendReflex : public plLowLatencyBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plLowLatencyBackendReflex, plLowLatencyBackend);

public:
  virtual plStringView GetName() const override { return "Reflex"; }
  virtual plStringView GetDisplayName() const override { return "NVIDIA Reflex"; }
  virtual plInt32 GetAutoPriority() const override { return 40; }

  virtual plLowLatencyCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps) override;
  virtual void Sleep(plUInt64 uiFrameId) override;
  virtual void SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId) override;
  virtual bool GetStats(plLowLatencyStats& out_stats) const override;

private:
  plLowLatencyMode::Enum m_Mode = plLowLatencyMode::Off;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFrameGenerationBackendDLSS, 1, plRTTIDefaultAllocator<plFrameGenerationBackendDLSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plLowLatencyBackendReflex, 1, plRTTIDefaultAllocator<plLowLatencyBackendReflex>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plFrameGenerationCapabilities plFrameGenerationBackendDLSS::QueryCapabilities(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  plFrameGenerationCapabilities caps;
  if (!plStreamline::IsFeatureSupported(sl::kFeatureDLSS_G))
    return caps;

  caps.m_bSupported = true;
  caps.m_uiMaxGeneratedFrames = 1;

  sl::DLSSGState state;
  if (slDLSSGGetState(sl::ViewportHandle(plStreamline::s_uiViewport), state, nullptr) == sl::Result::eOk)
  {
    caps.m_uiMaxGeneratedFrames = plMath::Max<plUInt32>(state.numFramesToGenerateMax, 1u);
    caps.m_bSupportsDynamic = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
  }

  // Streamline wraps the swap chain for as long as the plugin is loaded; DLSS-G only switches its mode.
  caps.m_bReplacesSwapChain = false;
  caps.m_bUsesHudless = true;
  caps.m_bNoScRGB = true;
  caps.m_szRequiredLowLatencyBackend = "Reflex";
  return caps;
}

void plFrameGenerationBackendDLSS::Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings)
{
  PL_IGNORE_UNUSED(ref_device);

  if (!plStreamline::IsReady())
    return;

  m_bEnabled = bActive && settings.m_bEnabled;

  sl::DLSSGOptions options;
  if (m_bEnabled)
  {
    options.mode = settings.m_uiGeneratedFrames == 0 ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = plMath::Max<plUInt32>(settings.m_uiGeneratedFrames, 1u);
    options.dynamicTargetFrameRate = settings.m_fDynamicTargetFps;
    // The render size changes with dynamic resolution; the inputs carry their valid region.
    options.flags = sl::DLSSGFlags::eDynamicResolutionEnabled;
  }
  else
  {
    options.mode = sl::DLSSGMode::eOff;
  }

  plStreamline::LogResult(slDLSSGSetOptions(sl::ViewportHandle(plStreamline::s_uiViewport), options), "slDLSSGSetOptions");
}

void plFrameGenerationBackendDLSS::ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params)
{
  if (!m_bEnabled || !plStreamline::IsReady() || params.m_hVelocity.IsInvalidated())
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  sl::Constants constants = plStreamline::MakeDefaultConstants(params.m_vJitterPixels, params.m_bReset, params.m_fCameraNear, params.m_fCameraFar, params.m_fCameraFovVertical, params.m_fAspectRatio);
  plStreamline::FillCameraConstants(constants, params.m_ViewToClip, params.m_ClipToView, params.m_ClipToPrevClip, params.m_PrevClipToClip, params.m_vCameraPosition, params.m_vCameraForward, params.m_vCameraRight, params.m_vCameraUp);
  constants.depthInverted = params.m_bDepthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  plStreamline::SetConstants(params.m_uiFrameId, constants);

  void* pCommandList = pDevice->BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return;

  // The textures are transient render graph textures: Streamline copies them now, so frame generation can read them at
  // present. The upscaler of the same frame reads these copies as well.
  sl::Resource depth = plStreamline::MakeResource(*pDevice, params.m_hDepth, false);
  sl::Resource motion = plStreamline::MakeResource(*pDevice, params.m_hVelocity, false);
  const sl::Extent extent{0, 0, params.m_uiInputWidth, params.m_uiInputHeight};
  const sl::ResourceTag tags[] = {
    sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
    sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent),
  };
  plStreamline::Tag(params.m_uiFrameId, tags, PL_ARRAY_SIZE(tags), pCommandList);
  plStreamline::MarkSceneInputsTagged(params.m_uiFrameId);

  pDevice->EndNativeCommands(ref_encoder);
}

void plFrameGenerationBackendDLSS::ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params)
{
  if (!m_bEnabled || !plStreamline::IsReady())
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  void* pCommandList = pDevice->BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return;

  sl::Resource hudless = plStreamline::MakeResource(*pDevice, params.m_hHudless, false);
  const sl::Extent extent{0, 0, params.m_uiWidth, params.m_uiHeight};
  const sl::ResourceTag tags[] = {
    sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &extent),
  };
  plStreamline::Tag(params.m_uiFrameId, tags, PL_ARRAY_SIZE(tags), pCommandList);

  pDevice->EndNativeCommands(ref_encoder);
}

plFrameGenerationStats plFrameGenerationBackendDLSS::GetStats() const
{
  plFrameGenerationStats stats;
  if (!m_bEnabled || !plStreamline::IsReady())
    return stats;

  sl::DLSSGState state;
  if (slDLSSGGetState(sl::ViewportHandle(plStreamline::s_uiViewport), state, nullptr) == sl::Result::eOk)
  {
    // DLSS-G reports why it does not generate frames in its status flags; log each new combination once.
    static plUInt32 s_uiLoggedStatus = 0;
    if (static_cast<plUInt32>(state.status) != s_uiLoggedStatus)
    {
      s_uiLoggedStatus = static_cast<plUInt32>(state.status);
      plLog::Warning("DLSS: Frame Generation status 0x{} (resolution too low = 1, Reflex missing = 2, HDR format unsupported = 4, constants invalid = 8, GetCurrentBackBufferIndex not called = 16).", plArgU(s_uiLoggedStatus, 1, false, 16));
    }

    stats.m_bActive = state.status == sl::DLSSGStatus::eOk && state.numFramesActuallyPresented > 1;
    stats.m_uiPresentedPerFrame = plMath::Max<plUInt32>(state.numFramesActuallyPresented, 1u);
    stats.m_uiVRAMBytes = state.estimatedVRAMUsageInBytes;
  }
  return stats;
}

void plFrameGenerationBackendDLSS::Shutdown(plGALDevice& ref_device)
{
  Configure(ref_device, false, plFrameGenerationSettings());
  if (plStreamline::IsReady())
    plStreamline::LogResult(slFreeResources(sl::kFeatureDLSS_G, sl::ViewportHandle(plStreamline::s_uiViewport)), "slFreeResources(DLSS-G)");
}

plLowLatencyCapabilities plLowLatencyBackendReflex::QueryCapabilities(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  plLowLatencyCapabilities caps;
  if (!plStreamline::IsFeatureSupported(sl::kFeatureReflex))
    return caps;

  sl::ReflexState state;
  if (slReflexGetState(state) != sl::Result::eOk || !state.lowLatencyAvailable)
    return caps;

  caps.m_bSupported = true;
  caps.m_bSupportsBoost = true;
  caps.m_bReportsLatency = state.latencyReportAvailable;
  return caps;
}

void plLowLatencyBackendReflex::SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps)
{
  PL_IGNORE_UNUSED(ref_device);

  if (!plStreamline::IsReady())
    return;

  m_Mode = mode;

  sl::ReflexOptions options;
  switch (mode)
  {
    case plLowLatencyMode::On:
      options.mode = sl::ReflexMode::eLowLatency;
      break;
    case plLowLatencyMode::Boost:
      options.mode = sl::ReflexMode::eLowLatencyWithBoost;
      break;
    default:
      options.mode = sl::ReflexMode::eOff;
      break;
  }
  options.frameLimitUs = fFrameLimitFps > 0.0f ? static_cast<uint32_t>(1000000.0f / fFrameLimitFps) : 0u;
  options.useMarkersToOptimize = true;
  plStreamline::LogResult(slReflexSetOptions(options), "slReflexSetOptions");
}

void plLowLatencyBackendReflex::Sleep(plUInt64 uiFrameId)
{
  if (m_Mode == plLowLatencyMode::Off)
    return;

  if (const sl::FrameToken* pToken = plStreamline::GetFrameToken(uiFrameId))
    plStreamline::LogResult(slReflexSleep(*pToken), "slReflexSleep");
}

void plLowLatencyBackendReflex::SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId)
{
  sl::PCLMarker pclMarker;
  switch (marker)
  {
    case plLatencyMarker::SimulationStart:
      pclMarker = sl::PCLMarker::eSimulationStart;
      break;
    case plLatencyMarker::SimulationEnd:
      pclMarker = sl::PCLMarker::eSimulationEnd;
      break;
    case plLatencyMarker::RenderSubmitStart:
      pclMarker = sl::PCLMarker::eRenderSubmitStart;
      break;
    case plLatencyMarker::RenderSubmitEnd:
      pclMarker = sl::PCLMarker::eRenderSubmitEnd;
      break;
    case plLatencyMarker::PresentStart:
      pclMarker = sl::PCLMarker::ePresentStart;
      break;
    case plLatencyMarker::PresentEnd:
      pclMarker = sl::PCLMarker::ePresentEnd;
      break;
    case plLatencyMarker::InputSample:
      pclMarker = sl::PCLMarker::eControllerInputSample;
      break;
    default:
      return;
  }

  if (const sl::FrameToken* pToken = plStreamline::GetFrameToken(uiFrameId))
    plStreamline::LogResult(slPCLSetMarker(pclMarker, *pToken), "slPCLSetMarker");
}

bool plLowLatencyBackendReflex::GetStats(plLowLatencyStats& out_stats) const
{
  if (!plStreamline::IsReady())
    return false;

  sl::ReflexState state;
  if (slReflexGetState(state) != sl::Result::eOk || !state.latencyReportAvailable)
    return false;

  // The newest report is the last one with a frame id.
  const sl::ReflexReport* pReport = nullptr;
  for (plInt32 i = sl::kReflexFrameReportCount - 1; i >= 0; --i)
  {
    if (state.frameReport[i].frameID != 0 && state.frameReport[i].presentEndTime > state.frameReport[i].simStartTime)
    {
      pReport = &state.frameReport[i];
      break;
    }
  }
  if (pReport == nullptr)
    return false;

  // Report times are in microseconds.
  out_stats.m_bValid = true;
  out_stats.m_fSimulationToPresentMs = static_cast<float>(pReport->presentEndTime - pReport->simStartTime) / 1000.0f;
  out_stats.m_fRenderToGpuEndMs = pReport->gpuRenderEndTime > pReport->renderSubmitStartTime ? static_cast<float>(pReport->gpuRenderEndTime - pReport->renderSubmitStartTime) / 1000.0f : 0.0f;
  out_stats.m_fGpuFrameMs = static_cast<float>(pReport->gpuFrameTimeUs) / 1000.0f;
  return true;
}

PL_STATICLINK_FILE(DLSSPlugin, DLSSPlugin_DLSSFrameGeneration);
