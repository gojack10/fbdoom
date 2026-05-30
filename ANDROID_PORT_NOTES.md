# Android fbDOOM Port Snapshot

Public snapshot of Jack ten Bosch's Android fbDOOM port branch.

Generated audio assets were omitted from this branch because they are large build/runtime artifacts (`music/*.pcm`, `music.tar.gz`) and do not belong in GitHub history. The source code and build integration are included.

Highlights:

- Android framebuffer/audio device integration work.
- Cross-compile/build-system changes for running fbDOOM on Android ARM hardware.
- FluidSynth integration and offline/pre-rendered music pipeline code.
- Sound and menu fixes tested on device, including volume slider behavior, SFX gain handling, automap key mapping, and framebuffer display power handling.

Local full branch includes generated PCM assets used during device testing; this public branch keeps the code reviewable without shipping large binary artifacts.
