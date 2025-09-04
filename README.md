# Iškur Engine

Iškur Engine is a personal rendering engine built with DirectX 12, designed primarily as a platform for prototyping and experimenting with graphics techniques. Its public release is intended more as a portfolio piece than an attempt at widespread use.

> **Note:** This GitHub repository may show relatively few commits because I primarily develop Iškur Engine on a local Git server and only sync to GitHub when meaningful changes are made.

I share brief articles and experiments on computer graphics at [tmarrec.dev](https://tmarrec.dev).

## Features

- **Mesh Shaders**
- **Meshlet Frustum Culling:** engine-generated meshlets are culled on the GPU
- **Ray-Traced Shadows (DXR):** BLAS and TLAS are built at runtime
- **Bindless Resources:** textures, samplers, and buffers
- **PBR Shading**
- **HDR Pipeline:** full FP16 render targets with ACES tone mapping  
- **Auto-Exposure**
- **Runtime Shader Compilation (DXC)**
- **Reverse-Z**
- **SSAO**
- **FidelityFX Super Resolution (FSR)**
- **Scene Packer:** compresses textures and generates meshlets
- **Render pass live profiler**
- **Dear ImGui**

## Screenshots

<p align="center">
  <img src="screenshots/san-miguel.png" alt="San-Miguel scene"><br/>
  <em>San-Miguel scene</em>
</p>
<br/>

<p align="center">
  <img src="screenshots/bistro.png" alt="Bistro scene"><br/>
  <em>Bistro scene</em>
</p>
<br/>

<p align="center">
  <img src="screenshots/meshlets.png" alt="Meshlets debug view"><br/>
  <em>Meshlets debug view</em>
</p>
<br/>

<p align="center">
  <img src="screenshots/culling.png" alt="Meshlets frustum culling"><br/>
  <em>Meshlets frustum culling</em>
</p>

## Dependencies

- [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
- [DirectX 12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-graphics)
- [DirectX-Headers](https://github.com/microsoft/DirectX-Headers)
- [DirectXMesh](https://github.com/microsoft/DirectXMesh)
- [DirectXTex](https://github.com/microsoft/DirectXTex)
- [DirectXTK12](https://github.com/microsoft/DirectXTK12)
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
- [FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [meshoptimizer](https://github.com/zeux/meshoptimizer)
- [MikkTSpace](https://github.com/mmikk/MikkTSpace)
- [TinyGLTF](https://github.com/syoyo/tinygltf)

## Getting Started

### Prerequisites

- Windows 11
- Visual Studio 2022 (ensure the following components are installed)
   - Windows 11 SDK (10.0.26100.X)
- CMake 4.1 or higher

### Setup Instructions

1. **Clone Repository**

  ```bash
  git clone https://github.com/tmarrec/IskurEngine.git
  cd IskurEngine
  ```

2. **Generate Project Files**

   Execute the provided batch script to generate the Visual Studio solution:

  ```bash
   build.bat
   ```

   This creates the solution file (`build/IskurEngine.sln`).

3. **Build Project**

   Open `build/IskurEngine.sln` in Visual Studio and build the solution.

## Command-Line Arguments

You can specify which scene to load at startup using the `--scene` argument. For example:

```bash
IskurEngine.exe --scene San-Miguel
```

## Scene Packer (GLB to .iskurpack)
glTF scenes need to be packed using **IskurScenePacker**; it compresses textures and prepares geometry.

Place your source `.glb` files in `data/scenes_sources/<SceneName>.glb`.
Running the packer will generate `data/scenes/<SceneName>.iskurpack`, which the engine loads at runtime.

Typical usage:
```bash
IskurScenePacker --input San-Miguel --fast
IskurScenePacker --all --fast
```

## License

Iškur Engine is licensed under the MIT License. See [LICENSE](LICENSE) for more information.

© 2025 Tristan Marrec