# Changelog

## [0.2]:
### Added
- [x] Sprites
- [x] _drawsprite_ example
- [x] Sprite animation/controller
- [x] _animsprite_ example
- [x] Sprite debugger
- [x] Verbose logging
- [x] Input system 
- [x] CR update
- [x] sokol update
- [x] Initiate sound plugin
- [x] _playsound_ example
- [x] Gamepad support
- [x] Gamepad demo
- [x] Plugin dependencies
- [x] Plugin APIs by name
- [x] Cleanup and add something for examples cmake
- [x] Add Readme to plugins (imgui, sprite)
- [x] Better graphics debugging (stages)
- [x] Audio backend for sokol_audio
- [x] Android build
- [x] Raspberry build
- [x] iOS build
- [x] EXPERIMENTAL: D3D11 compute-shader support
- [x] Nbody physics example (D3d11 only)

### Changed
- [x] BREAKING - gfx: immediate and staged APIs are now the same. creation/destroy/query functions 
      are moved to `rizz_api_gfx`.

### Fixed
- [x] Deferred destroys. Now objects in use will never be destroyed even if the user calls _destroy_
- [x] immediate mode api conflicts with imgui
- [x] Fixed metal backend shader reload

## [0.1]:
### Added
- Asset manager
- Texture loader
- Graphics command-buffer with multi-threading support
- Shader hot-reload
- Texture hot-reload
- C/C++ hot-reload
- Plugin system
- ImGui integration
- Remotery profiler integration
- Basic Memory debugger
- Basic 2D debug primitives
- Customize ImGui font
- Assets runtime database (json)
- Tween
- Tiny FSM
- Tiny event queue
- Update imgui and incorporate tabs
- Graphics debugger (sokol)
- Demo project

## [WIP]
### Added
- [ ] EXPERIMENTAL: sokol debugging support for compute-shader
- [ ] EXPERIMENTAL: OpenGL compute-shader support

### Changed
- [ ] try to use <sys/sem.h> instead of posix semaphores

### Fixed
- [ ] fix sx_matx_inv 
- [ ] Error handling for missing shaders so they won't crash
