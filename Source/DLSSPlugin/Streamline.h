#pragma once

#include <Foundation/Math/Mat4.h>
#include <RendererFoundation/Device/Device.h>

#include <sl.h>
#include <sl_consts.h>

class plGALCommandEncoder;
struct plFrameGenerationSceneParams;

/// \brief The process-wide Streamline runtime of the plugin.
///
/// Loads sl.interposer.dll from the plugin's directory after checking its NVIDIA signature, initializes Streamline in
/// manual-hooking mode when the plugin loads (before the engine creates its device, where possible), hands it the D3D12
/// device, and lets it wrap the command queue creation and the window swap chain through a plGALNativeHook. The plugin
/// links none of NVIDIA's code: every sl* function is forwarded to the loaded interposer (StreamlineLoader.cpp).
namespace plStreamline
{
  /// \brief Loads the interposer and calls slInit. Called once when the plugin loads.
  void Startup();

  /// \brief Calls slShutdown and unloads the interposer.
  void Shutdown();

  /// \brief slInit succeeded and the D3D12 device was handed to Streamline.
  bool IsReady();

  /// \brief The native D3D12 device Streamline was given, or nullptr.
  void* GetD3DDevice();

  /// \brief Whether Streamline reports the feature as supported on the device's adapter.
  bool IsFeatureSupported(sl::Feature feature);

  /// \brief The frame token for an engine frame id (plLowLatencyRegistry frame ids). Tokens of recent frames are cached.
  const sl::FrameToken* GetFrameToken(plUInt64 uiFrameId);

  /// \brief Viewport of the main view. One viewport: frame generation and the upscaler only run for the main camera.
  constexpr plUInt32 s_uiViewport = 0;

  /// \brief Sets the common constants of a frame once. Later calls for the same frame id are ignored, so the first
  /// caller with camera data (frame generation, ray reconstruction) wins over the upscaler's camera-less defaults.
  void SetConstants(plUInt64 uiFrameId, const sl::Constants& constants);

  /// \brief Fills the camera part of the constants from engine data.
  void FillCameraConstants(sl::Constants& out_constants, const plMat4& viewToClip, const plMat4& clipToView, const plMat4& clipToPrevClip, const plMat4& prevClipToClip, const plVec3& vCameraPos, const plVec3& vForward, const plVec3& vRight, const plVec3& vUp);

  /// \brief The constants an upscaler without camera data sets: identity matrices, the jitter and the motion vector convention.
  sl::Constants MakeDefaultConstants(const plVec2& vJitterPixels, bool bReset, float fNear, float fFar, float fFovY, float fAspect);

  sl::float4x4 ToSL(const plMat4& m);

  /// \brief A Streamline resource for an engine texture. D3D12 state: shader resource for inputs, unordered access for outputs.
  sl::Resource MakeResource(plGALDevice& ref_device, plGALTextureHandle hTexture, bool bUnorderedAccess);

  /// \brief Tags resources of a frame on the main viewport.
  void Tag(plUInt64 uiFrameId, const sl::ResourceTag* pTags, plUInt32 uiNumTags, void* pCommandList);

  /// \brief Frame generation tagged copies of this frame's depth and motion vectors (valid until present). The upscaler
  /// must not tag them again: a later tag of the same type replaces the copy frame generation reads at present.
  void MarkSceneInputsTagged(plUInt64 uiFrameId);
  bool AreSceneInputsTagged(plUInt64 uiFrameId);

  /// \brief Logs a failed Streamline call once per call site and result.
  void LogResult(sl::Result res, const char* szWhat);
} // namespace plStreamline
