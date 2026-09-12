# Checking DLSS 5 Neural Rendering from a mod

Neural rendering (NGX feature 18) is wired all the way through Dx8to12, but
whether it actually runs is decided by the NVIDIA driver, not by this shim.
This is how a mod asks what the state is and shows the right thing in its UI.

## The short version

Call `Dx8to12_GetUpscalerStatusEx` and read three fields:

- `neural_rendering_available` — a runtime file (`nvngx_dlssnr.dll` or similar)
  was found next to the game or the helper. Known from startup.
- `neural_rendering_support` — the **driver's own** verdict on whether it will
  run feature 18 on this GPU. Populated only after neural rendering has been
  requested at least once (see the note below).
- `neural_rendering_active` — it is genuinely running this frame.

There is no bypass switch, and none is needed on this side: the shim already
attempts to create the feature whenever it is requested and does not gate it
behind any check of its own. What refuses it is the driver's signed NGX core,
which this code does not and will not fake. If a driver (or a core) that
reports the feature as available is installed, `active` becomes 1 with no
change here.

## Reading the status

`Dx8to12_UpscalerStatusEx` carries its own size. **Set `struct_size` before
calling**, or the call returns `false` and you get nothing -- this is the
single most common reason a panel shows "no runtime detected" while the log
says one was found.

```cpp
#include <windows.h>

// Matches src/device.h. Fields are appended over time; struct_size is what
// lets an older mod call a newer shim safely, so always set it.
struct Dx8to12_UpscalerStatusEx {
  int struct_size;
  int compiled_in, helper_running, ready, healthy, mode, preset, helper_status;
  unsigned int failed_frames, render_width, render_height, output_width, output_height;
  int neural_rendering_active;
  int neural_rendering_available;
  char neural_rendering_runtime[64];
  int neural_rendering_support;            // 0 none, 1 available, 2 needs driver, 3 unsupported
  unsigned int neural_rendering_min_driver_major;
  unsigned int neural_rendering_min_driver_minor;
  // ... may grow further; struct_size covers it.
};

using GetExFn = bool(__cdecl *)(Dx8to12_UpscalerStatusEx *);

void DescribeNeuralRendering() {
  HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
  if (!d3d8) return;
  auto get_ex = reinterpret_cast<GetExFn>(
      GetProcAddress(d3d8, "Dx8to12_GetUpscalerStatusEx"));
  if (!get_ex) return;  // Older shim without the Ex API.

  Dx8to12_UpscalerStatusEx s = {};
  s.struct_size = sizeof(s);           // <-- REQUIRED.
  if (!get_ex(&s)) return;

  if (s.neural_rendering_active) {
    // Running. Show the look controls (NRStyle, NRIntensity, ...).
  } else if (!s.neural_rendering_available) {
    // No runtime file installed. The user can fix this by dropping one in.
    // Grey the toggle; label it "no runtime installed".
  } else {
    switch (s.neural_rendering_support) {
      case 1:  // Driver says available but it is not running yet.
        // Enable the toggle; setting NeuralRendering=1 should bring it up.
        break;
      case 2:  // Driver says it needs a newer version.
        // Grey the toggle; label it e.g.
        //   "requires driver %u.%u", min_driver_major, min_driver_minor.
        break;
      case 3:  // Driver says this GPU does not support it.
        // Grey the toggle; label it "not supported on this GPU".
        break;
      default: // 0: not queried yet -- request it once to find out (below).
        break;
    }
  }
}
```

## Why `support` may read 0 until you ask once

The support verdict comes from NGX's capability query, and that query needs
NGX initialised. NGX is only initialised on the helper side the first time
neural rendering is actually requested -- bringing it up unprompted would mean
touching the shared NGX core that super resolution also uses, which is exactly
the kind of thing that has destabilised this path before.

So the honest sequence for a panel that wants the detailed reason up front is:

1. Set `NeuralRendering=1` once (via `Dx8to12_SetSettingBool` or the INI).
2. Give the helper a few frames to come up.
3. Read `neural_rendering_support`. Now it holds the driver's real answer, and
   you can grey the control with the correct label and turn the setting back
   off if it was `2`/`3`.

`neural_rendering_available` (the file exists) is known immediately and needs
none of this, so a simpler panel can gate on that alone.

## Turning it on

`NeuralRendering` is an ordinary boolean setting:

```cpp
using SetBoolFn = bool(__cdecl *)(const char *, bool);
auto set_bool = reinterpret_cast<SetBoolFn>(
    GetProcAddress(d3d8, "Dx8to12_SetSettingBool"));
if (set_bool) set_bool("NeuralRendering", true);
```

The look knobs -- `NRStyle`, `NRPreset`, `NRIntensity`, `NRGlobalTone`,
`NRLocalTone`, `NRLocalStructure`, `NRSkinStructure`, `NRAutoMask`,
`NRUICorrection` -- are read every frame, so a slider moves the image live.
They only do anything while `neural_rendering_active` is 1.

## What "bypass" would and would not mean

A mod cannot make the driver run feature 18 on hardware whose NGX core refuses
it, and neither can this shim. The refusal happens inside NVIDIA's signed
`_nvngx.dll` before the runtime file is ever opened; the only thing that
changes the answer is a driver (or a replaced core) that returns a different
one. If you install such a core yourself, nothing here needs changing -- the
shim already tries every time, so `active` simply becomes 1. Faking the core's
answer is out of scope for this project and no setting here does it.
