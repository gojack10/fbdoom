#!/bin/bash
# render_all.sh — render all DOOM songs to raw PCM at build time
set -e

WAD="/Users/jack/Downloads/DOOM.WAD"
SF2="/Users/jack/Downloads/SC-55.SF2"
OUTDIR="/Users/jack/projects/fbdoom/music"
TMPDIR=$(mktemp -d)
MUSDIR="/tmp/doom_render/mus"

echo "=== DOOM Music Offline Pre-render ==="
mkdir -p "$OUTDIR" "$MUSDIR" "$TMPDIR/midi"

# Step 1: Extract MUS lumps from WAD (entry size = 16 bytes)
echo "Step 1: Extracting MUS lumps..."
python3 << 'PYEOF'
import struct, os

wad_path = "/Users/jack/Downloads/DOOM.WAD"
out_dir = "$MUSDIR"
os.makedirs(out_dir, exist_ok=True)

with open(wad_path, "rb") as f:
    f.seek(4)
    num_lumps = struct.unpack("<I", f.read(4))[0]
    dir_offset = struct.unpack("<I", f.read(4))[0]

    # Read all directory entries first
    f.seek(dir_offset)
    entries = []
    for i in range(num_lumps):
        entry = f.read(16)
        if len(entry) < 16:
            break
        offset = struct.unpack("<I", entry[0:4])[0]
        size = struct.unpack("<I", entry[4:8])[0]
        name = entry[8:16].decode("ascii", errors="replace").rstrip("\x00")
        entries.append((offset, size, name))

    # Extract music lumps
    count = 0
    for offset, size, name in entries:
        upper = name.upper()
        if upper.startswith("D_") and size > 100:
            f.seek(offset)
            data = f.read(size)
            out_path = f"{out_dir}/{upper}.mus"
            with open(out_path, "wb") as out:
                out.write(data)
            print(f"  {upper}: {size} bytes")
            count += 1

print(f"Extracted {count} MUS files")
PYEOF

# Step 2: Build MUS→MIDI converter (use C++ for the wrapper too)
echo ""
echo "Step 2: Building MUS→MIDI converter..."

cat > "$TMPDIR/convert.cpp" << 'CEOF'
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int convertToMidi(void *musData, void **midiOutput);

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.mus> <output.mid>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("input"); return 1; }
    fseek(fin, 0, SEEK_END);
    int size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    unsigned char *mus_data = (unsigned char *)malloc(size);
    fread(mus_data, 1, size, fin);
    fclose(fin);

    void *midi_out = NULL;
    int result = convertToMidi(mus_data, &midi_out);
    free(mus_data);

    if (!result || !midi_out) {
        fprintf(stderr, "Conversion failed\n");
        return 1;
    }

    // Parse MIDI file to find total size
    unsigned char *ptr = (unsigned char *)midi_out;
    if (memcmp(ptr, "MThd", 4) != 0) {
        fprintf(stderr, "Invalid MIDI\n");
        free(midi_out);
        return 1;
    }

    int header_len = (ptr[4]<<24)|(ptr[5]<<16)|(ptr[6]<<8)|ptr[7];
    int num_tracks = (ptr[10]<<8)|ptr[11];
    int pos = 8 + header_len;

    for (int t = 0; t < num_tracks && pos < 1000000; t++) {
        if (memcmp(ptr + pos, "MTrk", 4) != 0) break;
        int track_len = (ptr[pos+4]<<24)|(ptr[pos+5]<<16)|(ptr[pos+6]<<8)|ptr[pos+7];
        pos += 8 + track_len;
    }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("output"); free(midi_out); return 1; }
    fwrite(ptr, 1, pos, fout);
    fclose(fout);
    free(midi_out);
    return 0;
}
CEOF

cd /Users/jack/projects/fbdoom
clang++ -std=c++03 -fno-exceptions -I. -o "$TMPDIR/convert_mus" \
    "$TMPDIR/convert.cpp" src/mus2midi.cpp src/i_mus_convert.cpp 2>&1 \
    || { echo "Build failed"; exit 1; }

# Step 3: Convert MUS→MIDI, render MIDI→WAV→PCM
echo ""
echo "Step 3: Rendering songs..."

for mus_file in "$MUSDIR"/*.mus; do
    [ -f "$mus_file" ] || continue
    name=$(basename "$mus_file" .mus)
    midi_file="$TMPDIR/midi/${name}.mid"

    # Convert MUS → MIDI
    "$TMPDIR/convert_mus" "$mus_file" "$midi_file" 2>/dev/null || {
        echo "  SKIP $name (conversion failed)"
        continue
    }

    # Render MIDI → WAV with fluidsynth (44100Hz, poly=128)
    wav_file="$TMPDIR/${name}.wav"
    fluidsynth -a null --gain=0.5 \
        --set=synth.polyphony=128 \
        -F "$wav_file" -f 16 -r 44100 \
        "$SF2" "$midi_file" 2>/dev/null || {
        echo "  SKIP $name (fluidsynth failed)"
        continue
    }

    # Strip WAV header → raw PCM (int16 mono 44100Hz)
    python3 -c "
import wave, sys
with wave.open('$wav_file', 'rb') as w:
    data = w.readframes(w.getnframes())
with open('$OUTDIR/${name}.pcm', 'wb') as f:
    f.write(data)
print(f'  {name}: {w.getnframes()} samples ({len(data)} bytes, {len(data)/2/44100:.1f}s)')
"
done

# Step 4: Summary
echo ""
echo "=== Summary ==="
ls -lh "$OUTDIR"/*.pcm 2>/dev/null | wc -l | xargs -I{} echo "Songs: {}"
du -sh "$OUTDIR" 2>/dev/null | awk '{print "Total size: " $1}'

# Cleanup
rm -rf "$TMPDIR"

echo ""
echo "Done! PCM files in: $OUTDIR"