# Daisy GPU Userland

GPU acceleration and rendering userland stack for the Xiaomi Mi A2 Lite (daisy, MSM8953 / Adreno 506)
on iOS 16.7.16 (20H392).

Imported and adapted from [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) (`/hdd/ios-topaz`),
which originally derived its GPU userland from **HackiOS** (OOSEX GANG, MIT License).

## Architecture

CoreAnimation in iOS 16 (`QuartzCore`) renders via Metal or OpenGL ES (`CA::WindowServer::EAGLServer`).
On hardware without an Apple GPU, stock iOS falls back to a software rasterizer. This stack connects
CoreAnimation's OpenGL ES renderer to Mesa's `freedreno` Gallium driver (supporting Adreno 506 / a5xx)
through an IOKit-backed DRM shim and EAGLContext hooking.

```
+-------------------------------------------------------------+
|                      SpringBoard / Apps                     |
+-------------------------------------------------------------+
                              | (IOSurface sharing)
                              v
+-------------------------------------------------------------+
|                         backboardd                          |
|  +-------------------------------------------------------+  |
|  | bbd_wrap -> real backboardd                           |  |
|  +-------------------------------------------------------+  |
|  | libdaisy-inject.dylib (DYLD_INSERT_LIBRARIES)          |  |
|  |  - GLESContext::function_supported() vtable hook     |  |
|  |  - GLESContext::function_uniform() vtable hook       |  |
|  |  - Crash handlers with backtrace reporting           |  |
|  +-------------------------------------------------------+  |
|  | libdaisy-gl.dylib (eagl.c + gl_osmesa.c)              |  |
|  |  - Replaces EAGLContext methods                       |  |
|  |  - Zero-copy IOSurface binding to GL textures         |  |
|  +-------------------------------------------------------+  |
|  | libOSMesa.8.dylib (Mesa 25.0.7)                       |  |
|  |  - Gallium freedreno (Adreno 5xx/6xx driver)          |  |
|  |  - Gallium softpipe fallback                          |  |
|  +-------------------------------------------------------+  |
|  | redwood-drm (redwood_drm.c / xf86drm.h)               |  |
|  |  - Stand-in for libdrm on Darwin                      |  |
|  |  - IOConnectCallStructMethod to kernel GPU service    |  |
+--+--------------------------+-------------------------------+
                              | IOKit user client
                              v
+-------------------------------------------------------------+
|              Kernel: DaisyGPU / VinceGPU                    |
|        (0181 / TopazAdreno: msm DRM command submission)     |
+-------------------------------------------------------------+
                              | MMIO / Ring Buffer / CP
                              v
+-------------------------------------------------------------+
|              Hardware: Qualcomm Adreno 506                  |
+-------------------------------------------------------------+
```

## Directory Structure

```
userland/
├── ent.plist                   # Codesign entitlements for userland binaries
├── mkheader.py                 # Embeds userland binaries into kernel headers
├── src/                        # Vince / Daisy standalone helper daemons
│   ├── bluetool_stub.c
│   └── p2pd_stub.c
├── gpu/
│   ├── bbd_wrap.c              # backboardd wrapper setting DYLD_INSERT_LIBRARIES
│   ├── daisy_inject.c          # Daisy injector (GLESContext hooks, crash handlers)
│   ├── topaz_inject.c          # Reference Topaz injector
│   ├── inject.c                # Full Redwood injector reference
│   ├── eagl.c                  # EAGLContext hook & CoreAnimation GL bridge
│   ├── gl_osmesa.c             # OSMesa / freedreno context lifecycle
│   ├── redwood_gl.h            # Interface between eagl.c and gl_osmesa.c
│   ├── mkeagl.py               # Generator for eagl_gen.h function jump table
│   ├── eagl_slots.txt          # EAGLContext macro context slot definitions
│   └── Makefile                # Local build rules for GPU helper binaries
└── mesa/
    ├── mk-mesa-mac.sh          # Cross-compiles Mesa 25.0.7 for iOS on macOS
    ├── freedreno-darwin.patch  # Darwin/IOKit modifications for freedreno
    ├── osmesa-es-profile.patch # OSMesa OpenGL ES profile support
    └── redwood-drm/
        ├── redwood_drm.c       # IOKit DRM bridge (matching DaisyGPU/VinceGPU/RedwoodGPU)
        └── xf86drm.h           # DRM declarations for freedreno
```

## Component Details

### 1. `bbd_wrap.c`
Launchd does not propagate `DYLD_*` environment variables to system daemons. `bbd_wrap` takes the place
of `/usr/libexec/backboardd` on the filesystem:
- Sets `DYLD_INSERT_LIBRARIES=/daisy/lib/libdaisy-inject.dylib` (fallback `/topaz/lib/libtopaz-inject.dylib`).
- Checks for `/daisy/backboardd.env` when booted with `bbdenv=1`.
- Executes the real window server binary at `/usr/libexec/real/backboardd` with the original PID and Mach ports intact.

### 2. `daisy_inject.c`
Injected via `DYLD_INSERT_LIBRARIES`:
- **`containermanagerd`**: Overrides `+[MCMUserIdentityCache personasAreSupported]` to return `NO`, eliminating a 20-second startup block that stalls widget/poster registration.
- **`backboardd`**:
  - Polls for kernel GPU services (`DaisyGPU`, `VinceGPU`, `RedwoodGPU`).
  - Sets up signal handlers (`SIGSEGV`, `SIGBUS`, `SIGABRT`, `SIGILL`, `SIGTRAP`) that log program counter, link register and call frames to the console.
  - Dynamically loads `libdaisy-gl.dylib`.
  - Replaces virtual methods in `CA::OGL::GLESContext` (`function_supported` at vtable offset 0x388, `function_uniform` at 0x398) to handle custom shader functions that CoreAnimation's default GL renderer does not natively emit.

### 3. `eagl.c` & `gl_osmesa.c`
Compiles into `libdaisy-gl.dylib`:
- Subclasses and method-swizzles `EAGLContext` (`-initWithAPI:properties:`, `-API`, `-setParameter:to:`, `-getMacroContextPrivate`, `-texImageIOSurface:...`, `-attachImage:toCoreSurface:invertedRender:`).
- Intercepts CoreAnimation draws, shader generation, texture uploads, and surface binds.
- Calls `redwood_gl_surface_texture()` so textures backed by an `IOSurface` ID are imported zero-copy into freedreno GEM buffer objects.
- `gl_osmesa.c` initializes Mesa off-screen rendering via `OSMesaCreateContextAttribs` with `OSMESA_ES_PROFILE` (GLES 2/3).

### 4. `userland/mesa/`
- Builds `libOSMesa.8.dylib` from Mesa 25.0.7 with `-Dgallium-drivers=freedreno,softpipe`.
- `redwood-drm/redwood_drm.c` translates Linux DRM requests (`drmOpenWithType`, `drmIoctl`, `drmCommandWrite`, `drmPrimeFDToHandle`, `redwood_drm_map`, `redwood_drm_surface`) into `IOConnectCallStructMethod` calls against the kernel GPU service (`DaisyGPU` / `VinceGPU` / `RedwoodGPU`).

### 5. `mkeagl.py` & `eagl_slots.txt`
Generates the dispatch table `eagl_gen.h` mapping all OpenGLES macro context function slots (called by QuartzCore as `entry(table[0], args...)`) to the matching Mesa GL entrypoints, handling OES/EXT suffix aliases and fallback stubs.

## Building

### GPU Helpers (`bbd_wrap`, `libdaisy-inject.dylib`)
Built via the `.github/workflows/userland.yml` GitHub Actions workflow or locally on macOS:
```sh
cd userland/gpu
make
```

### Mesa (`libOSMesa.8.dylib`)
On macOS with Xcode and Homebrew:
```sh
brew install bison meson ninja
cd userland/mesa
PREFIX=/daisy ./mk-mesa-mac.sh
```
Outputs in `userland/mesa/build-mesa/out/`:
- `libOSMesa.8.dylib`
- `exported-gl.txt`
- `gl.xml`

### `libdaisy-gl.dylib`
Once Mesa is built:
```sh
cd userland/gpu
make eagl_gen.h MESA_OUT=../mesa/build-mesa/out
make out/libdaisy-gl.dylib MESA_OUT=../mesa/build-mesa/out
```

## Device Deployment

On the device root filesystem (HFS+):
1. Create `/daisy/lib`:
   - Copy `libdaisy-inject.dylib`, `libdaisy-gl.dylib`, and `libOSMesa.8.dylib` to `/daisy/lib/`.
2. Wrap `backboardd`:
   - Move `/usr/libexec/backboardd` to `/usr/libexec/real/backboardd`.
   - Install `bbd_wrap` as `/usr/libexec/backboardd` (mode 0755, root:wheel, signed with `ent.plist`).
3. Boot with `gpu=1` to enable kernel command submission, or `gl=0` to force CPU fallback.

