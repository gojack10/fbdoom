# Fork: Jack ten Bosch  Android fbDOOM Port

Fork of stoffera/fbdoom. I added:

- Android framebuffer and audio device integration
  - Cross-compile and build system changes so fbDOOM runs on Android ARM.
  - Framebuffer display power handling and audioflinger wrapper.
- FluidSynth music pipeline
  - Integrated FluidSynth library, pre-rendered PCM music backend, and offline render tooling.
  - My review: PCM pre-render works on-device and avoids runtime FluidSynth cost.
- Sound and menu fixes
  - SFX volume scaling, gain handling, automap key mapping, Y/N menu confirmations.
  - Tested on physical device.

Generated audio assets are not in this branch. They are build artifacts and too large for git.

---

