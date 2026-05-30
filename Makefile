# Paths
SDL2_PREFIX = /opt/homebrew/Cellar/sdl2/2.32.10
SDL2_TTF_PREFIX = /opt/homebrew/Cellar/sdl2_ttf/2.24.0
NDK_CC = /opt/homebrew/Caskroom/android-ndk/29/AndroidNDK14206865.app/Contents/NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang
NDK_CCXX = /opt/homebrew/Caskroom/android-ndk/29/AndroidNDK14206865.app/Contents/NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang++
NDK_STLPORT = /opt/homebrew/Caskroom/android-ndk/29/AndroidNDK14206865.app/Contents/NDK/sources/cxx-stl/system

# Source files
SOURCES = $(patsubst src/%, %, $(wildcard src/*.c))
SOURCES += device/main.c device/i_fb_video.c device/i_no_sound.c device/i_no_music.c
OBJECTS = $(patsubst %.c, %.o, $(SOURCES))
TARGET_OBJS = $(patsubst %, build/%, $(OBJECTS))
CCFLAGS = -DNORMALUNIX -std=gnu99
CPPFLAGS = -DNORMALUNIX -std=c++03 

# FluidSynth sources (minimal: synth core only, no drivers/sequencer/shell)
FLUID_C = \
	fluidsynth/src/bindings/fluid_cmd.c \
	fluidsynth/src/bindings/fluid_filerenderer.c \
	fluidsynth/src/drivers/fluid_adriver.c \
	fluidsynth/src/drivers/fluid_mdriver.c \
	fluidsynth/src/midi/fluid_midi.c \
	fluidsynth/src/midi/fluid_midi_router.c \
	fluidsynth/src/midi/fluid_seq.c \
	fluidsynth/src/midi/fluid_seqbind.c \
	fluidsynth/src/rvoice/fluid_adsr_env.c \
	fluidsynth/src/rvoice/fluid_chorus.c \
	fluidsynth/src/rvoice/fluid_iir_filter.c \
	fluidsynth/src/rvoice/fluid_lfo.c \
	fluidsynth/src/rvoice/fluid_rev.c \
	fluidsynth/src/rvoice/fluid_rvoice.c \
	fluidsynth/src/rvoice/fluid_rvoice_event.c \
	fluidsynth/src/rvoice/fluid_rvoice_mixer.c \
	fluidsynth/src/sfloader/fluid_defsfont.c \
	fluidsynth/src/sfloader/fluid_samplecache.c \
	fluidsynth/src/sfloader/fluid_sffile.c \
	fluidsynth/src/sfloader/fluid_sfont.c \
	fluidsynth/src/synth/fluid_chan.c \
	fluidsynth/src/synth/fluid_event.c \
	fluidsynth/src/synth/fluid_gen.c \
	fluidsynth/src/synth/fluid_mod.c \
	fluidsynth/src/synth/fluid_synth.c \
	fluidsynth/src/synth/fluid_synth_monopoly.c \
	fluidsynth/src/synth/fluid_tuning.c \
	fluidsynth/src/synth/fluid_voice.c \
	fluidsynth/src/utils/fluid_conv.c \
	fluidsynth/src/utils/fluid_hash.c \
	fluidsynth/src/utils/fluid_list.c \
	fluidsynth/src/utils/fluid_ringbuffer.c \
	fluidsynth/src/utils/fluid_settings.c \
	fluidsynth/src/utils/fluid_sys.c

FLUID_CPP = \
	fluidsynth/src/drivers/fluid_audio_convert.cpp \
	fluidsynth/src/gentables/fluid_cb2amp.cpp \
	fluidsynth/src/gentables/fluid_concave.cpp \
	fluidsynth/src/gentables/fluid_convex.cpp \
	fluidsynth/src/gentables/fluid_ct2hz.cpp \
	fluidsynth/src/gentables/fluid_interp_coeff.cpp \
	fluidsynth/src/gentables/fluid_interp_coeff_linear.cpp \
	fluidsynth/src/gentables/fluid_interp_coeff_sinc7.cpp \
	fluidsynth/src/gentables/fluid_pan.cpp \
	fluidsynth/src/midi/fluid_seq_queue.cpp \
	fluidsynth/src/midi/fluid_seqbind_notes.cpp \
	fluidsynth/src/rvoice/fluid_iir_filter_impl.cpp \
	fluidsynth/src/rvoice/fluid_rvoice_dsp.cpp \
	fluidsynth/src/synth/fluid_synth_write_int.cpp \
	fluidsynth/src/utils/fluid_file.cpp \
	fluidsynth/src/utils/fluid_sys_cpp11.cpp
FLUID_INCLUDES = -I fluidsynth/include -I fluidsynth/src -I fluidsynth/src/utils -I fluidsynth/src/sfloader -I fluidsynth/src/rvoice -I fluidsynth/src/synth -I fluidsynth/src/midi -I fluidsynth/src/drivers -I fluidsynth/src/bindings -I fluidsynth/src/gentables
FLUID_CCFLAGS = $(FLUID_INCLUDES) -DWITH_FLOAT=1 -DOSAL_cpp11=1
FLUID_CPPFLAGS = $(FLUID_INCLUDES) -DWITH_FLOAT=1 -DOSAL_cpp11=1 -std=c++17 
FLUID_OBJS = $(shell find build/fluidsynth -name "*.o" 2>/dev/null)

.PHONY: all
all: doom

# Compile FluidSynth sources (mac)
define compile_fluid_mac
	@mkdir -p build/fluidsynth/src/bindings build/fluidsynth/src/drivers build/fluidsynth/src/midi build/fluidsynth/src/rvoice build/fluidsynth/src/sfloader build/fluidsynth/src/synth build/fluidsynth/src/utils build/fluidsynth/src/gentables
	@for f in $(FLUID_C); do \
		d=$$(dirname $$f | sed 's|fluidsynth/||'); \
		n=$$(basename $$f .c); \
		echo "FluidSynth $$n..."; \
		$(CC) -c $$f $(FLUID_CCFLAGS) -g -o build/fluidsynth/$$d/$$n.o || exit 1; \
	done
	@for f in $(FLUID_CPP); do \
		d=$$(dirname $$f | sed 's|fluidsynth/||'); \
		n=$$(basename $$f .cpp); \
		echo "FluidSynth $$n..."; \
		$(CC) -c $$f $(FLUID_CPPFLAGS) -g -o build/fluidsynth/$$d/$$n.o || exit 1; \
	done
endef

# Compile FluidSynth sources (android)
define compile_fluid_android
	@mkdir -p build/fluidsynth/src/bindings build/fluidsynth/src/drivers build/fluidsynth/src/midi build/fluidsynth/src/rvoice build/fluidsynth/src/sfloader build/fluidsynth/src/synth build/fluidsynth/src/utils build/fluidsynth/src/gentables
	@for f in $(FLUID_C); do \
		d=$$(dirname $$f | sed 's|fluidsynth/||'); \
		n=$$(basename $$f .c); \
		echo "FluidSynth $$n..."; \
		$(NDK_CC) -c $$f $(FLUID_CCFLAGS) -g -o build/fluidsynth/$$d/$$n.o || exit 1; \
	done
	@for f in $(FLUID_CPP); do \
		d=$$(dirname $$f | sed 's|fluidsynth/||'); \
		n=$$(basename $$f .cpp); \
		echo "FluidSynth $$n..."; \
		$(NDK_CCXX) -c $$f $(FLUID_CPPFLAGS) -g -o build/fluidsynth/$$d/$$n.o || exit 1; \
	done
endef

.PHONY: mac
mac:
	@rm -f build/*.o build/device/*.o $$(find build/fluidsynth -name "*.o")
	@mkdir -p build/device
	@$(CC) -c src/device/main.c -I src $(CCFLAGS) -g -o build/device/main.o
	@$(CC) -c src/device/i_sdl_video.c -I src $(CCFLAGS) \
		-I$(SDL2_PREFIX)/include -I$(SDL2_PREFIX)/include/SDL2 \
		-I$(SDL2_TTF_PREFIX)/include -I$(SDL2_TTF_PREFIX)/include/SDL2 \
		-g -o build/device/i_sdl_video.o
	@$(CC) -c src/device/i_no_sound.c -I src $(CCFLAGS) -g -o build/device/i_no_sound.o
	@$(CC) -c src/device/i_no_music.c -I src $(CCFLAGS) -g -o build/device/i_no_music.o
	@for f in src/*.c; do \
		n=$$(basename $$f .c); \
		echo "Compiling $$n..."; \
		$(CC) -c $$f -I src $(CCFLAGS) -g -o build/$$n.o; \
	done
	$(compile_fluid_mac)
	@echo "Linking..."
	@$(CC) $(CCFLAGS) \
		-L$(SDL2_PREFIX)/lib -L$(SDL2_TTF_PREFIX)/lib \
		-lSDL2 -lSDL2_ttf -framework Cocoa -lm \
		-o doom_mac build/*.o build/device/*.o $$(find build/fluidsynth -name "*.o") -lc++

.PHONY: android
android:
	@rm -f build/*.o build/device/*.o $$(find build/fluidsynth -name "*.o")
	@mkdir -p build/device
	@$(NDK_CC) -c src/device/main.c -I src $(CCFLAGS) -g -o build/device/main.o
	@$(NDK_CC) -c src/device/i_fb_video.c -I src $(CCFLAGS) -g -o build/device/i_fb_video.o
	@$(NDK_CC) -c src/device/i_android_sound.c -I src $(CCFLAGS) -g -o build/device/i_android_sound.o
	@$(NDK_CC) -c src/device/i_android_music.c -I src $(CCFLAGS) $(FLUID_INCLUDES) -g -o build/device/i_android_music.o
	@$(NDK_CC) -c src/i_mus_convert.cpp -I src $(CPPFLAGS) -g -o build/i_mus_convert.o
	@$(NDK_CC) -c src/mus2midi.cpp -I src $(CPPFLAGS) -g -o build/mus2midi.o
	@$(NDK_CC) -c lib/c_compat.c -I src $(CCFLAGS) -g -o build/c_compat.o
	@for f in src/*.c; do \
		n=$$(basename $$f .c); \
		echo "Compiling $$n..."; \
		$(NDK_CC) -c $$f -I src $(CCFLAGS) -g -o build/$$n.o; \
	done
	$(compile_fluid_android)
	@echo "Linking..."
	@$(NDK_CCXX) --target=armv7a-linux-androideabi21 -nostdlib++ $(CCFLAGS) -o doom build/c_compat.o build/*.o build/device/*.o $$(find build/fluidsynth -name "*.o") /opt/homebrew/Caskroom/android-ndk/29/AndroidNDK14206865.app/Contents/NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/arm-linux-androideabi/libc++_static.a /opt/homebrew/Caskroom/android-ndk/29/AndroidNDK14206865.app/Contents/NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/arm-linux-androideabi/libc++abi.a -ldl -lm -llog
	@ls -lh doom 2>&1 | tail -1

.PHONY: sources
sources:
	@echo $(SOURCES)

.PHONY: objects
objects:
	@echo $(TARGET_OBJS)

build:
	mkdir -p build/device

build/%.o: src/%.c
	@mkdir -p build/device
	@echo "Compiling $<..."
	@$(CC) -c $< -I src $(CCFLAGS) -g -o $@

build/device/%.o: src/device/%.c
	@echo "Compiling $<..."
	@$(CC) -c $< -I src $(CCFLAGS) -g -o $@

.PHONY:
link: $(TARGET_OBJS)
	@echo "Linking..."

.PHONY: wrapper
wrapper:
	@$(NDK_CCXX) -shared -fPIC -std=c++03 -fno-exceptions \
		--target=armv7a-linux-androideabi21 \
		-I$(NDK_STLPORT)/include \
		-Wl,--allow-shlib-undefined,-rpath,/system/lib \
		-L/tmp -lmedia -lutils -lbinder \
		-o libaudiowrapper.so audioflinger_wrapper.cpp
	@ls -lh libaudiowrapper.so 2>&1 | tail -1

doom: link