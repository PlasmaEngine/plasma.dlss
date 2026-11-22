#include <DLSSPlugin/DLSSPluginPCH.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/StringConversion.h>
#include <RendererCore/Upscaling/Upscaler.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

/// NVIDIA DLSS Super Resolution through NGX on D3D12. Needs an NVIDIA RTX GPU, a current driver and nvngx_dlss.dll,
/// which NGX looks for next to the executable and next to this plugin.
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
  virtual void Shutdown(plGALDevice& ref_device) override;

  NVSDK_NGX_Parameter* GetParameters() const { return m_pParameters; }

private:
  bool m_bInitialized = false;
  ID3D12Device* m_pDevice = nullptr;
  NVSDK_NGX_Parameter* m_pParameters = nullptr;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUpscalerBackendDLSS, 1, plRTTIDefaultAllocator<plUpscalerBackendDLSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  // Identifies the engine to NGX. NVIDIA issues ids to shipping titles; this one stands for the engine itself.
  constexpr const char* s_szProjectId = "69a72dfc-5690-492c-9e00-3077d5e947b2";

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

  NVSDK_NGX_PerfQuality_Value ToPerfQuality(plUInt32 uiQuality)
  {
    switch (uiQuality)
    {
      case plUpscalerQuality::NativeAA:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
      case plUpscalerQuality::Quality:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
      case plUpscalerQuality::Balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
      case plUpscalerQuality::Performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
      case plUpscalerQuality::UltraPerformance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
      default:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
  }

  ID3D12Resource* GetResource(plGALDevice& ref_device, plGALTextureHandle hTexture)
  {
    plGALNativeTexture texture;
    if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
      return nullptr;
    return static_cast<ID3D12Resource*>(texture.m_pResource);
  }

  class plDLSSUpscalerContext : public plUpscalerContext
  {
  public:
    plDLSSUpscalerContext(plGALDevice& ref_device, NVSDK_NGX_Parameter* pParameters, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
      , m_pParameters(pParameters)
    {
      m_Desc = desc;
    }

    ~plDLSSUpscalerContext()
    {
      if (m_pFeature != nullptr)
        NVSDK_NGX_D3D12_ReleaseFeature(m_pFeature);
    }

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params) override
    {
      ID3D12GraphicsCommandList* pCommandList = static_cast<ID3D12GraphicsCommandList*>(m_Device.BeginNativeCommands(ref_encoder));
      if (pCommandList == nullptr)
        return PL_FAILURE;

      // The feature is created on the command list, so the first frame creates it.
      if (m_pFeature == nullptr && !m_bCreationFailed)
      {
        NVSDK_NGX_DLSS_Create_Params create = {};
        create.Feature.InWidth = m_Desc.m_uiMaxRenderWidth;
        create.Feature.InHeight = m_Desc.m_uiMaxRenderHeight;
        create.Feature.InTargetWidth = m_Desc.m_uiDisplayWidth;
        create.Feature.InTargetHeight = m_Desc.m_uiDisplayHeight;
        create.Feature.InPerfQualityValue = ToPerfQuality(m_Desc.m_uiQuality);
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        if (m_Desc.m_bHDR)
          create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        if (m_Desc.m_bDepthInverted)
          create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

        const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(pCommandList, 1, 1, &m_pFeature, m_pParameters, &create);
        if (NVSDK_NGX_FAILED(result))
        {
          plLog::Error("DLSS: creating the feature failed (0x{}).", plArgU(static_cast<plUInt32>(result), 8, true, 16));
          m_pFeature = nullptr;
          m_bCreationFailed = true;
        }
      }

      plResult res = PL_FAILURE;
      if (m_pFeature != nullptr)
      {
        NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
        eval.Feature.pInColor = GetResource(m_Device, params.m_hColor);
        eval.Feature.pInOutput = GetResource(m_Device, params.m_hOutput);
        eval.pInDepth = GetResource(m_Device, params.m_hDepth);
        eval.pInMotionVectors = GetResource(m_Device, params.m_hVelocity);
        eval.pInBiasCurrentColorMask = GetResource(m_Device, params.m_hReactiveMask);
        eval.InJitterOffsetX = params.m_vJitterPixels.x;
        eval.InJitterOffsetY = params.m_vJitterPixels.y;
        eval.InRenderSubrectDimensions = {params.m_uiRenderWidth, params.m_uiRenderHeight};
        eval.InReset = params.m_bReset ? 1 : 0;
        // The engine's velocity is current minus previous in UV units; DLSS wants the pixel offset to the previous position.
        eval.InMVScaleX = -static_cast<float>(params.m_uiRenderWidth);
        eval.InMVScaleY = -static_cast<float>(params.m_uiRenderHeight);
        eval.InPreExposure = plMath::Max(params.m_fPreExposure, 0.0001f);
        eval.InExposureScale = 1.0f;
        eval.InFrameTimeDeltaInMsec = params.m_fDeltaTimeMs;

        if (eval.Feature.pInColor != nullptr && eval.Feature.pInOutput != nullptr && eval.pInDepth != nullptr && eval.pInMotionVectors != nullptr)
        {
          const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(pCommandList, m_pFeature, m_pParameters, &eval);
          res = NVSDK_NGX_SUCCEED(result) ? PL_SUCCESS : PL_FAILURE;
        }
      }

      m_Device.EndNativeCommands(ref_encoder);
      return res;
    }

  private:
    plGALDevice& m_Device;
    NVSDK_NGX_Parameter* m_pParameters = nullptr;
    NVSDK_NGX_Handle* m_pFeature = nullptr;
    bool m_bCreationFailed = false;
  };
} // namespace

plUpscalerCapabilities plUpscalerBackendDLSS::QueryCapabilities(plGALDevice& ref_device)
{
  plUpscalerCapabilities caps;

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  if (native.m_Api != plGALNativeApi::D3D12 || native.m_uiVendorId != 0x10DE)
    return caps;

  if (!m_bInitialized)
  {
    m_pDevice = static_cast<ID3D12Device*>(native.m_pDevice);

    // NGX searches the plugin's directory for nvngx_dlss.dll after the executable's directory.
    plStringBuilder sPluginDir = GetPluginDirectory();
    sPluginDir.MakePathSeparatorsNative();
    plStringWChar sPluginDirW(sPluginDir);

    const wchar_t* paths[1] = {sPluginDirW.GetData()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1u;

    wchar_t szTemp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, szTemp);

    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(s_szProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.12", szTemp, m_pDevice, &info);
    if (NVSDK_NGX_FAILED(result))
    {
      plLog::Info("DLSS: NGX is not available on this system (0x{}).", plArgU(static_cast<plUInt32>(result), 8, true, 16));
      return caps;
    }
    m_bInitialized = true;

    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_pParameters);
    if (NVSDK_NGX_FAILED(result))
    {
      m_pParameters = nullptr;
      return caps;
    }
  }

  if (m_pParameters == nullptr)
    return caps;

  int iAvailable = 0;
  int iNeedsDriver = 0;
  m_pParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &iAvailable);
  m_pParameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &iNeedsDriver);
  if (iNeedsDriver != 0)
    plLog::Warning("DLSS: the NVIDIA driver is too old for DLSS.");

  caps.m_bSupported = iAvailable != 0;
  caps.m_bSupportsTextureArrays = false;
  caps.m_bUsesReactiveMask = true;
  caps.m_bSupportsDynamicResolution = true;
  caps.m_fMinRenderScale = 1.0f / 3.0f;
  return caps;
}

plUniquePtr<plUpscalerContext> plUpscalerBackendDLSS::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (m_pParameters == nullptr)
    return nullptr;

  return PL_DEFAULT_NEW(plDLSSUpscalerContext, ref_device, m_pParameters, desc);
}

void plUpscalerBackendDLSS::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  if (m_pParameters != nullptr)
    NVSDK_NGX_D3D12_DestroyParameters(m_pParameters);
  m_pParameters = nullptr;

  if (m_bInitialized)
    NVSDK_NGX_D3D12_Shutdown1(m_pDevice);
  m_bInitialized = false;
  m_pDevice = nullptr;
}

PL_STATICLINK_FILE(DLSSPlugin, DLSSPlugin_DLSSUpscaler);
