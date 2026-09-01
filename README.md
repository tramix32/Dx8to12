# Dx8to12 - A DirectX 8 implementation on DirectX12.

Dx8to12 implements the DirectX8 API on top of DirectX 12. The main functionality is complete. However, I haven't implemented parts of the API that weren't used in the games I was testing; for example, many of the getter functions like `GetTexture`.

---

## This fork

A fork of [RamiHg/Dx8to12](https://github.com/RamiHg/Dx8to12) that keeps the
original goal — a drop-in `d3d8.dll` — and adds modern rendering features on
top of it, driven by an INI file and a C API for mods.

### NVIDIA DLAA on a 2002 game

`TemporalAA=1` in `dx8to12.ini` runs the game's frame through NVIDIA DLAA.
Verified on Grand Theft Auto: Vice City.

Getting there needed the whole temporal pipeline, because an upscaler cannot
be bolted onto a fixed-function renderer that was never designed to feed one:

* **Sub-pixel jitter** on the projection, so each frame samples a different
  point inside every pixel. The HUD is untouched: pre-transformed (`XYZRHW`)
  geometry takes a different shader path that never sees the jittered matrix.
* **An offscreen scene target**, so there is an image of the world that does
  not yet contain the HUD.
* **Motion vectors reconstructed from the depth buffer** — D3D8 has no notion
  of them. Each pixel is unprojected with this frame's inverse view-projection
  and re-projected with the previous frame's. Depth is emitted as a second
  render target of that same pass, which converts it to a format another
  process can read for free.
* **An x64 helper process.** NVIDIA Streamline ships x64-only and a D3D8 game
  is a 32-bit process, so the two talk over shared textures and shared fences.
  The game's GPU queue never waits on the helper: a queue wait on a fence a
  crashed helper will never signal is a hung GPU, so the wait is on the CPU,
  bounded, and falls back to presenting the frame unchanged.
* **Two frame slots**, so the game hands over frame N and presents the
  finished frame N-1 rather than stalling on the round trip.

Measured at 2560x1440 on an RTX 4080, same binary, only the INI changed:
the whole transport costs less than the measurement noise (245 vs 249 fps);
DLAA itself costs about 1.9 ms, which is the price of the quality, not a bug.

**Known limit:** the motion vectors describe camera movement only, so things
that move independently of the camera — cars, pedestrians — ghost slightly.
Static geometry does not.

**Not yet:** rendering *below* output resolution (i.e. DLSS as a performance
win rather than DLAA as a quality one). `TemporalAA=2` is accepted but
currently behaves as DLAA.

### Requirements for DLAA

An NVIDIA RTX GPU, and `dx8to12_dlaa_helper.exe` plus `sl.interposer.dll`,
`sl.common.dll`, `sl.dlss.dll` and `nvngx_dlss.dll` next to `d3d8.dll`. With
any of them missing the game runs exactly as if the feature were off, and says
so in `log.txt`.

The NVIDIA Streamline SDK is **not** included here — it has its own licence
and redistribution terms. Drop it in `third_party/streamline/` to build the
helper with DLSS support; without it everything still builds and runs.

### Other additions

* **`dx8to12.ini` + a C API for mods** (`MODDING.md`) over one shared state:
  a setting a mod changes at runtime is written back to the INI, in place,
  leaving your own comments and your own keys untouched.
* **Native D3D12 access for mods** — device, queue and command list, plus the
  depth buffer, the view-projection matrix and the active lights, so a
  companion mod can render without a D3D9 shim underneath it.
* **Fixed-function pixel shader injection**, for mods that want to change how
  the game is lit rather than draw on top of it.
* A **disappearing-geometry fix** for GTA: Vice City, which turned out to write
  past the end of its own static index buffers and then draw those indices.

### Building

See `CLAUDE.md` for build profiles and the full list of CMake options. The
short version: it must be built as **x86**, and `RelWithDebInfo` is the
default rather than `Release` so crashes stay symbolicatable.

## Supported Features

* The full fixed-function vertex and pixel pipeline. Vs_1_1 programmable shaders, and a subset of ps_1_3 programmable shaders (enough to get the games I was testing working).

* Cubemaps.

* Dynamic and managed buffers/textures.

## Tested Games

Battlefield 1942™, Age of Mythology™ and Grand Theft Auto: Vice City™.

Vice City is this fork's main test subject and the game DLAA was brought up
against. Getting it running needed one non-obvious fix: it writes more index
data than fits in its own static index buffers and then draws those indices,
which the shim was clamping away — taking the trailing triangles of a material
with it, so roads and buildings vanished in patches. `DX8TO12_PAD_BUFFERS`
(on by default) is the fix.

## Why?

Why not?

## No, seriously. Why?

I wanted to learn DirectX 12!
