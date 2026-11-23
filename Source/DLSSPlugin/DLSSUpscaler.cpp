#include <DLSSPlugin/DLSSPluginPCH.h>

#include <DLSSPlugin/Streamline.h>
#include <RendererCore/FrameGeneration/LowLatency.h>
#include <RendererCore/Upscaling/RayReconstruction.h>
#include <RendererCore/Upscaling/Upscaler.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <sl_dlss.h>
#include <sl_dlss_d.h>

/// NVIDIA DLSS Super Resolution (and DLAA) through Streamline on D3D12. Needs an NVIDIA RTX GPU and the Streamline
/// runtime (sl.interposer.dll, sl.dlss.dll, nvngx_dlss.dll) next to the plugin or the executable.
class plUpscalerBackendDLSS : public plUpscalerBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plUpscalerBackendDLSS, plUpscalerBackend);

public:
  virtual plStringView GetName() const override { return "DLSS"; }
  virtual plStringView GetDisplayName() const override { return "NVIDIA DLSS"; }

  // Only supported on NVIDIA RTX GPUs, where it is the best choice.
  virtual plInt32 GetAutoPriority() const override { return 40; }

  virtual plUpscalerCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual plUniquePtr<plUpscalerContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
};

/// NVIDIA DLSS Ray Reconstruction through Streamline: denoises the ray traced lighting and upscales in one step,
/// replacing DLSS Super Resolution and the path tracer's denoiser while it runs.
class plRayReconstructionBackendDLSS : public plRayReconstructionBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plRayReconstructionBackendDLSS, plRayReconstructionBackend);

public:
  virtual plStringView GetName() const override { return "DLSS-RR"; }
  virtual plStringView GetUpscalerName() const override { return "DLSS"; }
  virtual bool IsSupported(plGALDevice& ref_device) override;
  virtual plUniquePtr<plRayReconstructionContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUpscalerBackendDLSS, 1, plRTTIDefaultAllocator<plUpscalerBackendDLSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plRayReconstructionBackendDLSS, 1, plRTTIDefaultAllocator<plRayReconstructionBackendDLSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  sl::DLSSMode ToDLSSMode(plUInt32 uiQuality)
  {
    switch (uiQuality)
    {
      case plUpscalerQuality::NativeAA:
        return sl::DLSSMode::eDLAA;
      case plUpscalerQuality::Quality:
        return sl::DLSSMode::eMaxQuality;
      case plUpscalerQuality::Balanced:
        return sl::DLSSMode::eBalanced;
      case plUpscalerQuality::Performance:
        return sl::DLSSMode::eMaxPerformance;
      case plUpscalerQuality::UltraPerformance:
        return sl::DLSSMode::eUltraPerformance;
      default:
        return sl::DLSSMode::eMaxQuality;
    }
  }

  /// Tags the inputs and the output both DLSS variants read, then sets the frame's constants unless frame generation
  /// already set them with the camera. Returns the frame id.
  plUInt64 TagCommonInputs(plGALDevice& ref_device, void* pCommandList, const plUpscalerExecuteParams& params, sl::Resource& ref_color, sl::Resource& ref_output, sl::Resource& ref_depth, sl::Resource& ref_motion, sl::Resource& ref_reactive)
  {
    const plUInt64 uiFrameId = plLowLatencyRegistry::GetRenderFrameId();

    ref_color = plStreamline::MakeResource(ref_device, params.m_hColor, false);
    ref_output = plStreamline::MakeResource(ref_device, params.m_hOutput, true);
    ref_depth = plStreamline::MakeResource(ref_device, params.m_hDepth, false);
    ref_motion = plStreamline::MakeResource(ref_device, params.m_hVelocity, false);
    ref_reactive = plStreamline::MakeResource(ref_device, params.m_hReactiveMask, false);

    const sl::Extent renderExtent{0, 0, params.m_uiRenderWidth, params.m_uiRenderHeight};
    const sl::Extent displayExtent{0, 0, params.m_uiDisplayWidth, params.m_uiDisplayHeight};

    // The upscale reads everything inside the evaluate call recorded right after, so nothing has to be copied.
    plHybridArray<sl::ResourceTag, 6> tags;
    tags.PushBack(sl::ResourceTag(&ref_color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
    tags.PushBack(sl::ResourceTag(&ref_output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &displayExtent));
    if (!plStreamline::AreSceneInputsTagged(uiFrameId))
    {
      tags.PushBack(sl::ResourceTag(&ref_depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
      tags.PushBack(sl::ResourceTag(&ref_motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
    }
    if (ref_reactive.native != nullptr)
      tags.PushBack(sl::ResourceTag(&ref_reactive, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
    plStreamline::Tag(uiFrameId, tags.GetData(), tags.GetCount(), pCommandList);

    const float fAspect = params.m_uiDisplayHeight > 0 ? static_cast<float>(params.m_uiDisplayWidth) / static_cast<float>(params.m_uiDisplayHeight) : 1.0f;
    plStreamline::SetConstants(uiFrameId, plStreamline::MakeDefaultConstants(params.m_vJitterPixels, params.m_bReset, params.m_fCameraNear, params.m_fCameraFar, params.m_fCameraFovVertical, fAspect));
    return uiFrameId;
  }

  class plDLSSUpscalerContext : public plUpscalerContext
  {
  public:
    plDLSSUpscalerContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
    {
      m_Desc = desc;
    }

    ~plDLSSUpscalerContext()
    {
      plStreamline::LogResult(slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(plStreamline::s_uiViewport)), "slFreeResources(DLSS)");
    }

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params) override
    {
      if (!plStreamline::IsReady() || params.m_hVelocity.IsInvalidated())
        return PL_FAILURE;

      sl::DLSSOptions options;
      options.mode = ToDLSSMode(m_Desc.m_uiQuality);
      options.outputWidth = params.m_uiDisplayWidth;
      options.outputHeight = params.m_uiDisplayHeight;
      options.preExposure = plMath::Max(params.m_fPreExposure, 0.0001f);
      options.colorBuffersHDR = m_Desc.m_bHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      plStreamline::LogResult(slDLSSSetOptions(sl::ViewportHandle(plStreamline::s_uiViewport), options), "slDLSSSetOptions");

      void* pCommandList = m_Device.BeginNativeCommands(ref_encoder);
      if (pCommandList == nullptr)
        return PL_FAILURE;

      sl::Resource color, output, depth, motion, reactive;
      const plUInt64 uiFrameId = TagCommonInputs(m_Device, pCommandList, params, color, output, depth, motion, reactive);

      plResult res = PL_FAILURE;
      if (const sl::FrameToken* pToken = plStreamline::GetFrameToken(uiFrameId))
      {
        const sl::ViewportHandle viewport(plStreamline::s_uiViewport);
        const sl::BaseStructure* inputs[] = {&viewport};
        const sl::Result result = slEvaluateFeature(sl::kFeatureDLSS, *pToken, inputs, PL_ARRAY_SIZE(inputs), pCommandList);
        plStreamline::LogResult(result, "slEvaluateFeature(DLSS)");
        res = result == sl::Result::eOk ? PL_SUCCESS : PL_FAILURE;
      }

      m_Device.EndNativeCommands(ref_encoder);
      return res;
    }

  private:
    plGALDevice& m_Device;
  };

  class plDLSSRayReconstructionContext : public plRayReconstructionContext
  {
  public:
    plDLSSRayReconstructionContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
    {
      m_Desc = desc;
    }

    ~plDLSSRayReconstructionContext()
    {
      plStreamline::LogResult(slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(plStreamline::s_uiViewport)), "slFreeResources(DLSS-RR)");
    }

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params, const plRayReconstructionParams& rr) override
    {
      if (!plStreamline::IsReady() || params.m_hVelocity.IsInvalidated())
        return PL_FAILURE;

      sl::DLSSDOptions options;
      options.mode = ToDLSSMode(m_Desc.m_uiQuality);
      options.outputWidth = params.m_uiDisplayWidth;
      options.outputHeight = params.m_uiDisplayHeight;
      options.preExposure = plMath::Max(params.m_fPreExposure, 0.0001f);
      options.colorBuffersHDR = m_Desc.m_bHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      // The engine's guide stores roughness in the normal's w.
      options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
      options.worldToCameraView = plStreamline::ToSL(rr.m_WorldToView);
      options.cameraViewToWorld = plStreamline::ToSL(rr.m_ViewToWorld);
      plStreamline::LogResult(slDLSSDSetOptions(sl::ViewportHandle(plStreamline::s_uiViewport), options), "slDLSSDSetOptions");

      void* pCommandList = m_Device.BeginNativeCommands(ref_encoder);
      if (pCommandList == nullptr)
        return PL_FAILURE;

      // Ray reconstruction needs the camera: set it before the upscaler's camera-less defaults would be.
      {
        const plUInt64 uiFrameId = plLowLatencyRegistry::GetRenderFrameId();
        const float fAspect = params.m_uiDisplayHeight > 0 ? static_cast<float>(params.m_uiDisplayWidth) / static_cast<float>(params.m_uiDisplayHeight) : 1.0f;
        sl::Constants constants = plStreamline::MakeDefaultConstants(params.m_vJitterPixels, params.m_bReset, params.m_fCameraNear, params.m_fCameraFar, params.m_fCameraFovVertical, fAspect);
        plStreamline::FillCameraConstants(constants, rr.m_ViewToClip, rr.m_ClipToView, rr.m_ClipToPrevClip, rr.m_PrevClipToClip, rr.m_vCameraPosition, rr.m_vCameraForward, rr.m_vCameraRight, rr.m_vCameraUp);
        plStreamline::SetConstants(uiFrameId, constants);
      }

      sl::Resource color, output, depth, motion, reactive;
      const plUInt64 uiFrameId = TagCommonInputs(m_Device, pCommandList, params, color, output, depth, motion, reactive);

      sl::Resource diffuseAlbedo = plStreamline::MakeResource(m_Device, rr.m_hDiffuseAlbedo, false);
      sl::Resource specularAlbedo = plStreamline::MakeResource(m_Device, rr.m_hSpecularAlbedo, false);
      sl::Resource normalRoughness = plStreamline::MakeResource(m_Device, rr.m_hNormalRoughness, false);
      sl::Resource hitDistance = plStreamline::MakeResource(m_Device, rr.m_hSpecularHitDistance, false);

      const sl::Extent renderExtent{0, 0, params.m_uiRenderWidth, params.m_uiRenderHeight};
      plHybridArray<sl::ResourceTag, 4> guideTags;
      guideTags.PushBack(sl::ResourceTag(&diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
      guideTags.PushBack(sl::ResourceTag(&specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
      // Packed mode (normal in xyz, roughness in w) reads the combined normal-roughness tag.
      guideTags.PushBack(sl::ResourceTag(&normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
      if (hitDistance.native != nullptr)
        guideTags.PushBack(sl::ResourceTag(&hitDistance, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent));
      plStreamline::Tag(uiFrameId, guideTags.GetData(), guideTags.GetCount(), pCommandList);

      plResult res = PL_FAILURE;
      if (const sl::FrameToken* pToken = plStreamline::GetFrameToken(uiFrameId))
      {
        const sl::ViewportHandle viewport(plStreamline::s_uiViewport);
        const sl::BaseStructure* inputs[] = {&viewport};
        const sl::Result result = slEvaluateFeature(sl::kFeatureDLSS_RR, *pToken, inputs, PL_ARRAY_SIZE(inputs), pCommandList);
        plStreamline::LogResult(result, "slEvaluateFeature(DLSS-RR)");
        res = result == sl::Result::eOk ? PL_SUCCESS : PL_FAILURE;
      }

      m_Device.EndNativeCommands(ref_encoder);
      return res;
    }

  private:
    plGALDevice& m_Device;
  };
} // namespace

plUpscalerCapabilities plUpscalerBackendDLSS::QueryCapabilities(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  plUpscalerCapabilities caps;
  caps.m_bSupported = plStreamline::IsFeatureSupported(sl::kFeatureDLSS);
  caps.m_bSupportsTextureArrays = false;
  caps.m_bUsesReactiveMask = true;
  caps.m_bSupportsDynamicResolution = true;
  caps.m_fMinRenderScale = 1.0f / 3.0f;
  return caps;
}

plUniquePtr<plUpscalerContext> plUpscalerBackendDLSS::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (!plStreamline::IsReady())
    return nullptr;

  return PL_DEFAULT_NEW(plDLSSUpscalerContext, ref_device, desc);
}

bool plRayReconstructionBackendDLSS::IsSupported(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);
  return plStreamline::IsFeatureSupported(sl::kFeatureDLSS_RR);
}

plUniquePtr<plRayReconstructionContext> plRayReconstructionBackendDLSS::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (!plStreamline::IsReady())
    return nullptr;

  return PL_DEFAULT_NEW(plDLSSRayReconstructionContext, ref_device, desc);
}

PL_STATICLINK_FILE(DLSSPlugin, DLSSPlugin_DLSSUpscaler);
