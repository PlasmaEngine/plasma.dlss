# plasma.dlss — NVIDIA DLSS through Streamline

Adds NVIDIA's RTX features to the engine through the [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline). Each one plugs into an engine framework, so it sits next to the other vendors' packages and the game settings choose between them.

| Feature | Engine framework | Setting | GPU |
| --- | --- | --- | --- |
| DLSS Super Resolution and DLAA | Upscaler (`plUpscalerBackend`) | `Rendering.Upscaler = DLSS`, `Rendering.Upscaler.Quality` | RTX |
| DLSS Ray Reconstruction | Ray reconstruction (`plRayReconstructionBackend`) | `Rendering.Upscaler.RayReconstruction` (on by default), with DLSS as the upscaler | RTX |
| DLSS Frame Generation, Multi Frame Generation | Frame generation (`plFrameGenerationBackend`) | `Rendering.FrameGeneration = DLSS`, `Rendering.FrameGeneration.Multiplier` (2, 3, 4, 6, 0 = dynamic) | RTX 40 series and newer; 3x and more on RTX 50 series |
| Reflex, PC Latency stats | Low latency (`plLowLatencyBackend`) | `Rendering.LowLatency` (0 off, 1 on, 2 boost) | GeForce |

- Ray reconstruction replaces DLSS Super Resolution and the path tracer's denoiser while it runs. It needs a render pipeline with the path tracing pass, whose G-buffer it derives its guide images from.
- Frame generation only runs in game windows, never in the editor. It needs Reflex, which the engine turns on with it.
- Platform: D3D12 on Windows. Vulkan is not supported yet.

## How it runs

The plugin initializes Streamline in manual-hooking mode when it loads. The engine routes the creation of its command queues and of the game window's swap chain through Streamline (`plGALNativeHook`). Streamline's proxy swap chain then presents the generated frames, and Reflex measures every present.

For the queue hook, the plugin has to be loaded before the graphics device is created. Load it as a required plugin of the project, or start without the early splash image. A plugin loaded later still provides DLSS, Ray Reconstruction and Reflex, but it logs a warning that Frame Generation may be unavailable.

## NVIDIA's runtime

The plugin contains no NVIDIA code. It compiles against Streamline's headers (MIT, the git submodule `ThirdParty/Streamline`, pinned to a release tag) and loads `sl.interposer.dll` at run time, after checking that NVIDIA signed it.

The signed runtime is part of NVIDIA's Streamline release archive (`streamline-sdk-<version>.zip` on the Streamline releases page) and is never part of this package:

- `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_d.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.nis.dll`
- `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`

A game that uses DLSS ships these next to its executable. It also follows NVIDIA's DLSS branding guidelines and notification terms (`ThirdParty/Streamline/docs`).

## Building

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`). Then:

1. Extract the Streamline release archive of the same version as the submodule.
2. Set `STREAMLINE_SDK_PATH` to the extracted folder, so the build copies the runtime next to the plugin.
3. Set `PLASMA_SDK_ROOT` to the engine root.
4. Run `plasmabuild build DLSSPlugin` in this directory.
