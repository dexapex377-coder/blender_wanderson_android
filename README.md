<!-- Kronos3D: the Kronos3D header replaces the upstream read-me that follows. -->

# Kronos3D

**An unofficial port of Blender to Android. arm64, Vulkan, GPL-3.0.**

Kronos3D is a fork of the Android Blender port lineage: the original bring-up,
the cross-compilation of the dependency chain, and the Vulkan/GHOST work that
made Blender usable on a phone. It is maintained here as a study and hobby
project.

Unofficial. Not affiliated with, endorsed by, or supported by the Blender
Foundation or the Blender developers. "Blender" is a trademark of its owners and
is used here only to say what this software is derived from.

## Status

Alpha, and honestly labelled as such. It runs, it opens a file, and the
viewport draws — but treat it as a sketchbook, not a reference
implementation. See **About the code** below before you rely on it for
anything.

Developed and tested on an arm64 Android device (Moto G56 5G) over ADB.

## Building

Two workflows, split on purpose:

| Workflow | What it does |
| --- | --- |
| **Kronos3D Native** | Cross-compiles the dependency chain and `libblender.so`, then publishes a `kronos3d-native` artifact. |
| **Kronos3D APK** | Downloads a successful native artifact and packages the APK. It never compiles anything. |

The point of the split is iteration speed. A change to Java, the manifest,
layouts, resources or the launcher repackages in minutes, reusing the last
`.so` that built cleanly; you only pay for a native run when you touch C, C++
or the build itself. `Kronos3D APK` takes the most recent successful native
run by default, and accepts an explicit run id when you want to reproduce a
specific binary.

The build itself is inherited from the fork below, whose
[ANDROID_AI_GUIDE.md](https://github.com/Wanderson-Magalhaes/blender_for_android/blob/main/ANDROID_AI_GUIDE.md)
is still the most complete map of this tree: prerequisites, the dependency
build, the feature profiles, the flags that matter, driving a device over ADB,
and a file-by-file list of the changes with links. A Kronos3D-specific guide
replacing it does not exist yet.

## About the code

**This is AI-assisted code, and it is not reviewed to production standards.
Expect bugs.**

The cross-compilation of one dependency after another, the debugging of driver
and threading faults, and much of the packaging work in this tree were carried
out with heavy AI assistance. That is what made the port possible at all, and
it is also exactly why you should be careful with the result:

- Code may be **incorrect, inconsistent, or plainly wrong**, in ways that
  compile cleanly and only show up at runtime.
- **Rendering, GPU and threading code is the risky part.** A mistake there can
  produce a black viewport, a corrupted frame or a crash rather than an error
  message.
- **Performance and memory behaviour are unmeasured.** Nothing here has been
  profiled or tuned for general use.
- There is **no test suite, no code review, and no guarantee of correctness**.
- Changes are made quickly and often land in the same commit that broke
  something else. The history is a record of experiments, not a curated patch
  series.

The lineage this tree inherits carries the same warning and says it more
directly: it is a sketchbook, and some of the experiments left marks. If you
want a reliable Blender on Android, use a supported build or the official
desktop release.

**Contributing:** welcome, but please read the code before you rely on it, and
open issues with a `adb logcat` excerpt and the device model. Reports of real
device crashes are the most useful thing you can send.

## Credits

This port did not start here, and none of it exists without the work of the
people below.

### Blender

Blender is made and maintained by the **Blender Foundation** and a large
community of contributors. Everything in `source/`, `intern/`, `extern/` and
the rest of the tree is upstream Blender, licensed GPL-3.0-or-later, and none
of it would exist without them.

- Main site — <https://www.blender.org>
- About the foundation — <https://www.blender.org/about/foundation/>
- Source and issue tracker — <https://projects.blender.org>
- Code hosting / contributions — <https://projects.blender.org/blender/blender>
- Developer documentation — <https://developer.blender.org/docs/>
- User manual — <https://docs.blender.org/manual/en/latest/index.html>
- Community and forums — <https://www.blender.org/community/> · <https://devtalk.blender.org>
- License (GPL-3.0) — <https://www.blender.org/about/license/>

### The original Android port

**@idimus** (a.k.a. **simfeo**) did the initial bring-up: the first working
arm64/Vulkan build, the dependency bring-up, and the release that the later
forks derive from. This tree would not exist without it.

- Repository — <https://github.com/simfeo/blender>
- First public Android build (`android-alpha-1`) — <https://github.com/simfeo/blender/releases/tag/android-alpha-1>
- Releases — <https://github.com/simfeo/blender/releases>
- Reddit profile — <https://www.reddit.com/user/idimus/>

### The fork this tree is based on

**Wanderson-Magalhaes** carried the adjustments that `blender_for_android` is:
the long tail of CMake, toolchain and packaging fixes, the device bring-up
notes, and the guide this project still points at. Most of the structure of the
Android build in this tree comes from there.

- Repository — <https://github.com/Wanderson-Magalhaes/blender_for_android>
- Demonstration video — <https://www.youtube.com/watch?v=qzdrdLbK7Kw>
- Build guide — <https://github.com/Wanderson-Magalhaes/blender_for_android/blob/main/ANDROID_AI_GUIDE.md>

### Kronos3D

Rebranding, the launcher, the packaging workflows, and the thread-ownership
and GPU-context fixes are the work of the Kronos3D contributors.

- Repository — <https://github.com/dexapex377-coder/kronos3D>

## License

Kronos3D is licensed under the **GNU General Public License, Version 3 or
later**, inherited from Blender. If you distribute a binary built from this
tree, you must also provide the corresponding source code.

Individual files may carry a different but compatible license; see
<https://www.blender.org/about/license/>.

## Disclaimer

Kronos3D is provided as-is, with no warranty of any kind. The authors are not
liable for damage arising from its use, including data loss. Test it on
something you can afford to lose.

***

Everything below is the upstream Blender read-me.

Blender is the free and open source 3D creation suite.
It supports the entirety of the 3D pipeline, modeling, rigging, animation, simulation, rendering, compositing,
motion tracking and video editing.

## Project Pages

- [Main Website](https://www.blender.org)
- [Reference Manual](https://docs.blender.org/manual/en/latest/index.html)
- [User Community](https://www.blender.org/community/)

## Development

- [Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Code Review & Bug Tracker](https://projects.blender.org)
- [Developer Forum](https://devtalk.blender.org)
- [Developer Documentation](https://developer.blender.org/docs/)

## License

Blender as a whole is licensed under the GNU General Public License, Version 3.
Individual files may have a different but compatible license.

See [blender.org/about/license](https://www.blender.org/about/license) for details.
