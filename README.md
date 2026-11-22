# plasma.dlss — NVIDIA DLSS upscaler

Adds **NVIDIA DLSS** as a backend of the engine's temporal upscaler framework (`plUpscalerBackend`, RendererCore/Upscaling).

- Select it with `Rendering.Upscaler = DLSS` or in the game settings (Graphics → Upscaler). `Auto` picks the best supported backend.
- Mode: `Rendering.Upscaler.Quality` (0 native AA … 4 ultra performance, 5 custom). Dynamic resolution: `Rendering.DynamicResolution.Enable`.
- Several upscaler packages can be installed at the same time; the setting chooses between them at run time. Without any, the engine's built-in TSR is available.

Platform: D3D12 on NVIDIA RTX GPUs (Windows). The SDK comes from NVIDIA's own repository as the git submodule `ThirdParty/DLSS`; this repository carries none of NVIDIA's files. NVIDIA's licence does not allow redistributing the SDK on its own, so the published package ships no NVIDIA runtime: ship `nvngx_dlss.dll` next to your game executable. A local build copies it next to the plugin.

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`). Build against an engine SDK: set `PLASMA_SDK_ROOT` to the engine root, then `plasmabuild build DLSSPlugin` in this directory.
