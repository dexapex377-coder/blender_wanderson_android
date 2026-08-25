# Blender for Android: Build and Change Guide

A reference for a person or an AI agent picking this fork up cold. It covers what
the port is, how to build it end to end, how to drive a device over ADB, and
where every change lives in the tree.

## Credits

This port did not start here. It started with **Simfeo**, whose Android alpha did
the hard bring up work and is the build this fork was derived from:

> https://github.com/simfeo/blender/releases/tag/android-alpha-1

**Blender** itself is made by the Blender Foundation and its community, and is
free and open source under the GPL. This fork inherits that license: if you
distribute a binary built from this tree, the matching source has to go with it.

The work described below, everything from the on-screen keyboard to the touch and
stylus handling and the dependencies that were cross compiled to turn features
back on, is by **Wanderson M. Pimenta**, continuing Simfeo's port on a Galaxy S24
Ultra, on Blender 5.3 alpha, arm64, Vulkan.

## Status: a study project, open to whoever wants it

This is a study project. It is not maintained, and there is no support: the
author does not have the time an open source project deserves, and would rather
say so plainly than leave anyone waiting on an answer that is not coming.

It is published because it works and because the ground it covers is worth
having. The community is welcome to take it anywhere: fork it, carry the changes
upstream, or use them as the starting point for a better port. Nothing here is
waiting on permission.

The best outcome would be an Android port with the Blender developers behind it,
built by the people who know that codebase best. Everything in this tree is one
person learning C++ against a codebase far larger than him, with the help of AI
assistants for the cross compilation and the debugging. It proves the hardware
can do it. It is not a substitute for the real thing.

## What you get

A single APK, around 240 MB, containing Blender with Cycles, EEVEE, Open Image
Denoise, fluid simulation, motion tracking, USD, OpenVDB, Alembic, MaterialX,
FFmpeg, a full Python with pip, and the essentials asset library. Two feature
sets exist, `full` and `lite`; the numbers above are `full`.

---

# Part 1: Building

## Prerequisites

| Thing | Requirement |
| --- | --- |
| Host | Linux or macOS. On Windows use WSL2 and keep the checkout inside the Linux filesystem. |
| Host compiler | GCC 14 or newer, or Clang 17 or newer. Blender's own code generators need it. `build_files/android/env.sh` picks the first of `gcc-15`, `gcc-14`, `clang-18`, `clang-17` it finds, so it does not have to be the system default. |
| Android SDK | With platform tools. Point `ANDROID_HOME` at it. |
| Android NDK | Version is pinned in [env.sh](build_files/android/env.sh) as `ANDROID_NDK_VERSION`. |
| JDK | 17. Needed for the APK tooling. |
| Disk | Around 60 GB for the dependency prefix, the build trees and the archived APKs. |

Target API levels: minimum 31 (Android 12), compiled against 34.

## Layout

Everything the build produces lives outside the checkout, under one directory
called `BUILD_BASE`:

```
$BUILD_BASE/lib/android_arm64          cross compiled dependencies
$BUILD_BASE/build_host_tools_<cfg>     native code generators (makesdna, makesrna, ...)
$BUILD_BASE/build_android_<cfg>        Blender itself
$BUILD_BASE/android_apk_stage_<cfg>    the APK being assembled
$BUILD_BASE/apk                        archived APKs, newest linked as <cfg>-latest.apk
```

`<cfg>` is `full` or `lite`. `build.py` resolves `BUILD_BASE` from an explicit
`--build-base`, then the environment, then the first candidate that actually
holds `lib/android_arm64`, and stops with a clear message when it finds nothing.

## Dependencies

[build_files/android/deps/build.sh](build_files/android/deps/build.sh) cross
compiles every library into `$BUILD_BASE/lib/android_arm64`. There are 73
recipes, from zlib and Python through OpenVDB, OpenImageIO, OpenSubdiv, Embree,
FFmpeg, GMP, Ceres, FFTW, OpenPGL, Manifold, Draco and Open Image Denoise.

Two of them need patches that live with the build files:

- Open Image Denoise, see [oidn_android.diff](build_files/build_environment/patches/oidn_android.diff): Bionic only exposes the pthread affinity calls from API 36, so they are redirected to the `sched_*` equivalents.
- glog, see [extern/glog/src/config_linux.h](extern/glog/src/config_linux.h): Android is `__linux__` and so claimed `HAVE_EXECINFO_H`, which selects a stack trace path that does not compile below API 33.

The dependency build is a one time cost of hours. Run it before the first
Blender build:

```bash
build_files/android/deps/build.sh
```

## Configurations

Feature toggles live in three files:

- [android_features_common.cmake](build_files/android/android_features_common.cmake)
- [android_features_full.cmake](build_files/android/android_features_full.cmake)
- [android_features_lite.cmake](build_files/android/android_features_lite.cmake)

`full` is everything. `lite` drops TBB, Cycles and the heavy IO, for weaker
devices and a much smaller APK.

The host code generators must be built with the **same** feature set as the
target, or the generated RNA and DNA disagree with the compiled binary, so each
configuration has its own host tools, build and stage directories.

## Building

```bash
python3 build_files/android/build.py full
```

Useful flags:

| Flag | Use |
| --- | --- |
| `--repackage` | Rebuild the runtime payload. Required for any change under `scripts/`, `assets/` or `release/datafiles`. |
| `--reconfigure` | Re-run CMake, keeping every object file. Required after a DNA header or version change, and after changing a feature flag. |
| `--clean` | Throw the build tree away and start over. |
| `--install`, `--run` | Push to a connected device and launch it. |
| `--keep N`, `--no-archive` | How many archived APKs to retain. |
| `--debuggable`, `--validation`, `--turnip` | Debug variants. |
| `-s SERIAL` | Which device, when more than one is attached. |

### Four traps, all of them silent

Each of these ends with a build that reports success while the device runs the
old code. They are the reason `.claude/skills/android-build/SKILL.md` exists.

1. **The fast path only swaps `libblender.so`.** A Python, keymap, datafile or
   manifest change does not reach the device without `--repackage`.
2. **A version or DNA change needs `--reconfigure`.** The fast path does not
   rebuild the host generators, so `makesdna` and `makesrna` keep whatever build
   they had and the generated DNA and RNA disagree with the binary. The result is
   not a compile error: the app installs, the activity starts, and the process
   dies before the first log line.
3. **The device unpacks the runtime once.** `BlenderActivity` writes a marker
   into its data directory and returns early while it exists. The marker now
   carries a hash of the payload, written by
   [package.sh](build_files/android/apk/package.sh), so a changed payload
   invalidates it on its own. If you ever suspect a stale payload,
   `adb shell pm clear <package>` erases it, and the user preferences with it.
4. **Exit codes are lost through `wsl.exe`.** Never trust `$?` for anything run
   that way. Grep the build log for the archive line instead.

### Verifying a change actually made it in

```bash
# The object file was really recompiled
find "$BUILD_BASE/build_android_full" -name 'YOURFILE.cc.o' -newermt '-10 minutes'

# A payload change is really inside the packaged archive
unzip -p "$BUILD_BASE/android_apk_stage_full/assets/blender_runtime.zip" \
  scripts/path/to/file.py | grep 'your change'

# A string you added is really in the shipped library
strings "$BUILD_BASE/android_apk_stage_full/lib/arm64-v8a/libblender.so" | grep 'your marker'
```

That last one matters more than it looks. Instrumentation that logs nothing is
evidence only after you have confirmed it is in the binary.

---

# Part 2: Driving a device with ADB

Nothing here needs a specific machine or path. `adb` is in the SDK platform
tools; put that directory on `PATH`, or call it by its full path in your own
environment.

```bash
adb devices                       # confirm exactly one device is attached
adb install -r path/to/blender.apk
adb shell am start -n org.blender.blender/.BlenderActivity
adb shell pidof org.blender.blender      # still alive a few seconds later?
```

Wireless debugging works and is convenient on a phone. Pair once from the
device developer settings, then `adb connect <host>:<port>`. The connection
drops when the device sleeps or changes network; `adb devices` returning nothing
means reconnect, not a broken build.

## Watching what the app does

```bash
adb logcat -c                     # clear, then reproduce
adb logcat -d -s TAG -v time      # read one tag back
adb logcat -d -b crash | tail -40 # the crash buffer is separate and already holds the tombstone
```

A native crash never has to be reproduced under observation. Read `-b crash`
first.

For temporary native logging, log directly rather than through Blender's CLOG,
which cannot be enabled without a command line:

```cpp
#ifdef __ANDROID__
#  include <android/log.h>
#  define DBG(...) __android_log_print(ANDROID_LOG_INFO, "DBG", __VA_ARGS__)
#endif
```

Remove it before committing.

## Injecting input

Injected events arrive as real touches and exercise the same path a finger does.

```bash
adb shell input tap X Y
adb shell input swipe X1 Y1 X2 Y2 DURATION_MS
adb shell input keyevent 111            # Esc
adb exec-out screencap -p > shot.png
adb shell dumpsys window | grep mCurrentFocus   # what is actually in the foreground
```

Two habits that save hours:

- **Check the foreground first.** The phone is somebody's personal device. If it
  is not Blender, your taps land somewhere they should not.
- **Coordinates from a screenshot are not the coordinates the app sees.** The
  system status bar offsets the y axis, and the port scales input. Log what
  arrives instead of assuming.

## Rotation as a variable

Some behaviour only breaks in one orientation. Force it rather than asking a
human to turn the phone, and put the setting back afterwards:

```bash
adb shell settings put system accelerometer_rotation 0
adb shell settings put system user_rotation 1      # 0 portrait, 1 landscape
# ... test ...
adb shell settings put system user_rotation 0
adb shell settings put system accelerometer_rotation 1
```

---

# Part 3: What was changed, and where

Every entry links the file the change lives in. Function names are given where
they help you find the code quickly.

## Input, touch and stylus

| Change | Where |
| --- | --- |
| Android GHOST backend: touch, stylus, gestures, the soft keyboard bridge, window lifecycle | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc), [GHOST_SystemAndroid.hh](intern/ghost/intern/GHOST_SystemAndroid.hh), [GHOST_AndroidMain.cc](intern/ghost/intern/GHOST_AndroidMain.cc) |
| One finger drag scrolls panels, headers, tool bars and the Properties editor. Widgets in those regions defer activation to `KM_CLICK`, so a tap still presses them and a drag pans. Scope is `but_touch_scroll_region()` | [interface_handlers.cc](source/blender/editors/interface/interface_handlers.cc), [interface_panel.cc](source/blender/editors/interface/interface_panel.cc), [blender_default.py](scripts/presets/keyconfig/keymap_data/blender_default.py) |
| Dragging from plain buttons and tool bar icons too. The press is left unconsumed, the motion is let through `handler_region_menu()`, and the tool group popup waits longer | [interface_handlers.cc](source/blender/editors/interface/interface_handlers.cc) |
| Three finger drag pans the 3D viewport. Shift is mirrored into the GHOST modifier state so the window manager does not cancel it, see `touchSendShift()` | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc), [blender_default.py](scripts/presets/keyconfig/keymap_data/blender_default.py) |
| Two finger gestures commit to pan or zoom once, instead of emitting both every frame | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc) |
| A finger press is reported where the finger landed, not where it has since travelled | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc), `handleMotionEvent()` |
| Window focus is reported to Blender. Without it `wmWindow.active` stays 0 and the window manager overwrites the position of every press with the current cursor position, which is why a finger could never grab anything precise | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc) `handleWindowFocus()`, [GHOST_AndroidMain.cc](intern/ghost/intern/GHOST_AndroidMain.cc) |
| A moving finger no longer becomes a right click at the long press deadline | [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc), `touchLongPressCheck()` |
| Editor borders can be grabbed with a finger. A press without tablet data that lands near a border is moved onto it before it is queued, from the main region of an editor only | [screen_geometry.cc](source/blender/editors/screen/screen_geometry.cc) `ED_screen_edge_snap_for_touch()`, [ED_screen.hh](source/blender/editors/include/ED_screen.hh), [wm_event_system.cc](source/blender/windowmanager/intern/wm_event_system.cc) |
| Stylus pressure reaches 100% with a comfortable press. Android normalises pressure against the range the digitiser declares, and an S Pen tops out around 0.77 | [DNA_userdef_types.h](source/blender/makesdna/DNA_userdef_types.h), [versioning_userdef.cc](source/blender/blenloader/intern/versioning_userdef.cc), [rna_userdef.cc](source/blender/makesrna/intern/rna_userdef.cc) |
| Typing with the platform keyboard. A soft keyboard sends composing text, not key events, so the composition is mirrored into the field | [BlenderActivity.java](build_files/android/apk/app/src/main/java/org/blender/blender/BlenderActivity.java) |
| The interface rotates with the device | [GHOST_AndroidMain.cc](intern/ghost/intern/GHOST_AndroidMain.cc), [GHOST_SystemAndroid.cc](intern/ghost/intern/GHOST_SystemAndroid.cc), [AndroidManifest.xml](build_files/android/apk/app/src/main/AndroidManifest.xml) |

## The on-screen keyboard

A keyboard drawn by Blender itself, opened from a button at the far left of the
status bar. It exists because an add-on cannot do this: a pointer event that
reaches the interface layer ends whatever text field is being edited, and no
overlay can sit in front of that.

| Piece | Where |
| --- | --- |
| State, layout, drawing, hit testing, key injection | [wm_virtual_keyboard.cc](source/blender/windowmanager/intern/wm_virtual_keyboard.cc) |
| The interception, above every handler in Blender | [wm_window.cc](source/blender/windowmanager/intern/wm_window.cc), in `ghost_event_proc()` |
| Holding back the platform keyboard while it is open, and reporting the edited field | [interface_handlers.cc](source/blender/editors/interface/interface_handlers.cc), `textedit_begin()` and `textedit_end()` |
| The status bar button | [space_statusbar.py](scripts/startup/bl_ui/space_statusbar.py) |
| Public API and operator registration | [WM_api.hh](source/blender/windowmanager/WM_api.hh), [wm.hh](source/blender/windowmanager/wm.hh), [wm_operators.cc](source/blender/windowmanager/intern/wm_operators.cc) |
| Why it is native rather than Python, with the anchor points it was built from | [ANDROID_VIRTUAL_KEYBOARD_STUDY.md](ANDROID_VIRTUAL_KEYBOARD_STUDY.md) |

It sends real key events, so it types into any field, holds Ctrl, Shift and Alt,
and fires every shortcut in the keymap including the numpad view keys. It works
in every workspace and lays itself out for portrait or landscape.

## Interface defaults for a phone

| Change | Where |
| --- | --- |
| Resolution scale 1.10, editor borders 4 pixels, temporary editors maximized instead of a new window, which Android cannot open | [DNA_userdef_types.h](source/blender/makesdna/DNA_userdef_types.h), [screen_edit.cc](source/blender/editors/screen/screen_edit.cc) |
| Viewport texture limit defaults to 1024 rather than unlimited | [DNA_userdef_types.h](source/blender/makesdna/DNA_userdef_types.h), [gpu_capabilities.cc](source/blender/gpu/intern/gpu_capabilities.cc) |
| Node editor keymaps stay ahead of the generic View2D one, so box select, link dragging and node moving survive drag to scroll | [space_node.cc](source/blender/editors/space_node/space_node.cc) |
| Splash artwork and launcher icon | [splash.png](release/datafiles/splash.png), [ic_launcher.xml](build_files/android/apk/app/src/main/res/drawable/ic_launcher.xml), [ic_splash_logo.xml](build_files/android/apk/app/src/main/res/drawable/ic_splash_logo.xml) |

## GPU and memory

| Change | Where |
| --- | --- |
| Qualcomm is asked for SPIR-V 1.3 whatever version it reports supporting. Its compiler refuses compute modules emitted as 1.5, which are the ones EEVEE uses for shadows and light culling | [vk_shader_compiler.cc](source/blender/gpu/vulkan/vk_shader_compiler.cc) |
| GPU subdivision falls back to the CPU when the driver refuses the compute pipeline, instead of handing back empty buffers | [subdiv_modifier.cc](source/blender/blenkernel/intern/subdiv_modifier.cc), [GPU_capabilities.hh](source/blender/gpu/GPU_capabilities.hh), [draw_cache_impl_subdivision.cc](source/blender/draw/intern/draw_cache_impl_subdivision.cc) |
| EEVEE shadow pool capped at 64 MB on Android regardless of system RAM, because the constraint is the GPU budget a mobile part shares with the device | [eevee_shadow.cc](source/blender/draw/engines/eevee/eevee_shadow.cc) |
| GPU workarounds are no longer forced at startup. The flag skipped feature detection and left the Qualcomm tile memory extension off, worth 25% of the frame while orbiting | [GHOST_AndroidMain.cc](intern/ghost/intern/GHOST_AndroidMain.cc) |

## Features and dependencies

| Feature | Where it was turned on |
| --- | --- |
| Open Image Denoise 2.5.0 | [android_features_full.cmake](build_files/android/android_features_full.cmake), [deps/build.sh](build_files/android/deps/build.sh), [platform_android.cmake](build_files/cmake/platform/platform_android.cmake) |
| Fluid simulation, Manifold boolean solver, motion tracking, PDF export, Ocean modifier, path guiding | [android_features_full.cmake](build_files/android/android_features_full.cmake), [deps/build.sh](build_files/android/deps/build.sh) |
| Exact boolean solver, through GMP | [deps/build.sh](build_files/android/deps/build.sh), [platform_android.cmake](build_files/cmake/platform/platform_android.cmake) |
| Draco compressed glTF | [deps/build.sh](build_files/android/deps/build.sh), [package.sh](build_files/android/apk/package.sh) |
| Internet access: the permission, a bundled certificate store, and an interpreter the extension system can run | [AndroidManifest.xml](build_files/android/apk/app/src/main/AndroidManifest.xml), [BlenderActivity.java](build_files/android/apk/app/src/main/java/org/blender/blender/BlenderActivity.java), [python/intern/CMakeLists.txt](source/blender/python/intern/CMakeLists.txt), [deps/build.sh](build_files/android/deps/build.sh) |
| pip | [deps/build.sh](build_files/android/deps/build.sh) |
| Essentials assets, 49 translations, USD plugins | [package.sh](build_files/android/apk/package.sh), [GHOST_SystemPathsAndroid.cc](intern/ghost/intern/GHOST_SystemPathsAndroid.cc) |

## Bug fixes

| Fix | Where |
| --- | --- |
| Out of bounds write when a label trims away to nothing, which killed the process the moment the 2D Animation or Storyboarding template opened in portrait. Not Android specific | [interface_widgets.cc](source/blender/editors/interface/interface_widgets.cc), `text_clip_middle_ex()` |
| Widgets in modifier panels needed several taps and the drop-down never opened. The Property Editor keymap bound `object.modifier_set_active` to a press, which consumed the press the click was waiting on | [blender_default.py](scripts/presets/keyconfig/keymap_data/blender_default.py) |
| "Reset to Default Value" disagreed with the value a fresh install starts on, because RNA defaults are baked in by a host tool that never sees `__ANDROID__` | [rna_userdef.cc](source/blender/makesrna/intern/rna_userdef.cc), [DNA_userdef_types.h](source/blender/makesdna/DNA_userdef_types.h) |
| The device kept running a payload it had unpacked weeks earlier | [package.sh](build_files/android/apk/package.sh), [BlenderActivity.java](build_files/android/apk/app/src/main/java/org/blender/blender/BlenderActivity.java) |
| The extension system child interpreter died before running a line, because isolated mode ignored the environment set for it | [BlenderActivity.java](build_files/android/apk/app/src/main/java/org/blender/blender/BlenderActivity.java) |
| glog selecting stack traces that do not compile below API 33 | [config_linux.h](extern/glog/src/config_linux.h) |

## Where else to look

| Document | Contents |
| --- | --- |
| [ANDROID_CHANGELOG.md](ANDROID_CHANGELOG.md) | Every change with the commit that made it, as features, adjustments and fixes |
| [ANDROID_WHATS_NEW.md](ANDROID_WHATS_NEW.md) | The same in plain language, one row per change |
| [ANDROID_BUILD_GUIDE.md](ANDROID_BUILD_GUIDE.md) | The long form build walkthrough, step by step |
| [build_files/android/BUILDING.md](build_files/android/BUILDING.md) | Building from scratch, including the manual steps `build.py` wraps |
| [ANDROID_MISSING_FEATURES.md](ANDROID_MISSING_FEATURES.md) | What is still missing, audited against the real build |
| [ANDROID_VIRTUAL_KEYBOARD_STUDY.md](ANDROID_VIRTUAL_KEYBOARD_STUDY.md) | Why the on-screen keyboard is native, and the event path it relies on |

## Known gaps

Two, as of the last audit:

- **Audio output.** Needs an AAudio device written against audaspace. No library to build, just the code.
- **OSL.** Needs a second host toolchain to generate shader bitcode.

## Working habits that paid off

Written down because each one was learned by losing time to its absence.

- Verify on the device before committing. A build that compiles has proved nothing.
- One hypothesis at a time, and instrument rather than guess after the first miss.
- "Cannot reproduce" is a result. It locates the trigger in a condition not yet set.
- When a change seems to have no effect, check the two payload traps before re-reading any code.
- Measure the real input. A synthetic swipe and a human finger are not the same gesture, and the difference is where the bug usually is.
