#!/usr/bin/env python3
"""
SF2-to-C-header converter for DOOM SoundFont integration.

Parses SC-55.SF2, extracts samples needed for DOOM's GM programs,
emits a C header with sample data and mapping tables.
"""

import struct
import sys
import os

# GM programs used by DOOM (from MUS lump analysis)
DOOM_PROGRAMS = {0, 28, 29, 30, 33, 34, 38, 45, 48, 49, 50, 51, 52, 53, 54, 62, 75, 79, 81, 94, 118}

def parse_sf2(path):
    """Parse SC-55.SF2 and return sample data + mapping."""
    with open(path, 'rb') as f:
        data = f.read()

    # Known offsets from RIFF tree analysis
    # LIST pdta at offset 10254068
    # Subchunks within pdta:
    pdta_base = 10254068 + 12  # LIST header (8) + list type (4)

    # Subchunk offsets within pdta:
    # phdr: pdta_base + 0, size 5244
    # pbag: pdta_base + 8 + 5244, size 1292
    # pgen: pdta_base + 8 + 5244 + 8 + 1292, size 1268
    # inst: pdta_base + 8 + 5244 + 8 + 1292 + 8 + 1268, size 4092
    # ... etc
    
    # Actually, let me use the known data offsets from earlier analysis:
    phdr_off = 10254088
    phdr_size = 5244
    pbag_off = 10259340
    pbag_size = 1292
    pgen_off = 10260658
    pgen_size = 1268
    inst_off = 10261934
    inst_size = 4092
    ibag_off = 10266034
    ibag_size = 7556
    igen_off = 10285356
    igen_size = 87072
    shdr_off = 10372436
    shdr_size = 31418
    smpl_off = 200  # sample data starts at LIST sdta -> smpl -> data

    # --- Parse phdr: Preset headers (36 bytes each) ---
    n_presets = phdr_size // 36
    presets = []
    for i in range(n_presets):
        off = phdr_off + i * 36
        name = data[off:off+20].decode('ascii','replace').rstrip('\x00').strip()
        if not name:
            continue
        preset_num = struct.unpack('<H', data[off+20:off+22])[0]
        bank = struct.unpack('<H', data[off+22:off+24])[0]
        zone_idx = struct.unpack('<H', data[off+24:off+26])[0]
        zone_len = struct.unpack('<H', data[off+26:off+28])[0]
        presets.append((name, preset_num, bank, zone_idx, zone_len))

    # --- Parse pbag: Preset bag zones (4 bytes each) ---
    n_pbag = pbag_size // 4
    pbag_records = []
    for i in range(n_pbag):
        off = pbag_off + i * 4
        zi = struct.unpack('<H', data[off:off+2])[0]
        zl = struct.unpack('<H', data[off+2:off+4])[0]
        pbag_records.append((zi, zl))

    # --- Parse pgen: Preset generators (4 bytes each) ---
    n_pgen = pgen_size // 4
    pgen_records = []
    for i in range(n_pgen):
        off = pgen_off + i * 4
        op = struct.unpack('<H', data[off:off+2])[0]
        val = struct.unpack('<H', data[off+2:off+4])[0]
        pgen_records.append((op, val))

    # --- Parse inst: Instrument headers (22 bytes each) ---
    n_insts = inst_size // 22
    instruments = []
    for i in range(n_insts):
        off = inst_off + i * 22
        name = data[off:off+20].decode('ascii','replace').rstrip('\x00').strip()
        zone_idx = struct.unpack('<H', data[off+20:off+22])[0]
        instruments.append((name, zone_idx))

    # --- Parse ibag: Instrument bag zones (4 bytes each) ---
    n_ibag = ibag_size // 4
    ibag_records = []
    for i in range(n_ibag):
        off = ibag_off + i * 4
        zi = struct.unpack('<H', data[off:off+2])[0]
        zl = struct.unpack('<H', data[off+2:off+4])[0]
        ibag_records.append((zi, zl))

    # --- Parse igen: Instrument generators (4 bytes each) ---
    n_igen = igen_size // 4
    igen_records = []
    for i in range(n_igen):
        off = igen_off + i * 4
        op = struct.unpack('<H', data[off:off+2])[0]
        val = struct.unpack('<H', data[off+2:off+4])[0]
        igen_records.append((op, val))

    # --- Parse shdr: Sample headers (76 bytes each) ---
    n_samples = shdr_size // 76
    samples = []
    for i in range(n_samples):
        off = shdr_off + i * 76
        name = data[off:off+16].decode('ascii','replace').rstrip('\x00').strip()
        start = struct.unpack('<I', data[off+16:off+20])[0]
        end = struct.unpack('<I', data[off+20:off+24])[0]
        start_loop = struct.unpack('<I', data[off+24:off+28])[0]
        end_loop = struct.unpack('<I', data[off+28:off+32])[0]
        sample_rate = struct.unpack('<I', data[off+32:off+36])[0]
        orig_key = data[off+44]
        orig_cents = struct.unpack('<h', data[off+46:off+48])[0]
        # Sample data offset (in samples, convert to bytes)
        # Sample data is 16-bit signed mono at the stored sample rate
        # smpl data starts at offset 200
        samples.append({
            'name': name,
            'start': start,
            'end': end,
            'start_loop': start_loop,
            'end_loop': end_loop,
            'rate': sample_rate,
            'key': orig_key,
            'cents': orig_cents,
            'data_offset': start * 2,  # 16-bit samples = 2 bytes each
            'data_length': (end - start) * 2,
            'loop_length': (end_loop - start_loop) * 2,
        })

    # --- Build GM program -> sample mapping ---
    # Follow the chain: preset -> pbag -> pgen (sample_instrument_id) -> inst -> ibag -> igen (sample_id) -> sample
    
    print(f"Parsed: {len(presets)} presets, {len(instruments)} instruments, {len(samples)} samples", file=sys.stderr)
    
    # Build a mapping of bank 0 presets (GM) by program number
    gm_presets = {}
    for pname, pprog, pbank, pzone_idx, pzone_len in presets:
        if pbank != 0 and pbank != 1:  # bank 0 or 1 (GM/drum)
            continue
        if pprog not in gm_presets:
            gm_presets[pprog] = (pname, pbank, pzone_idx, pzone_len)

    # For each DOOM program, find its samples
    program_samples = {}  # program -> [(sample_idx, key_range, vel_range), ...]
    
    for prog in DOOM_PROGRAMS:
        if prog not in gm_presets:
            print(f"  Program {prog}: no preset found", file=sys.stderr)
            continue
        
        pname, pbank, pzone_idx, pzone_len = gm_presets[prog]
        
        # Get pbag zones for this preset
        if pzone_idx + pzone_len > len(pbag_records):
            print(f"  Program {prog}: pbag zone out of range", file=sys.stderr)
            continue
        
        pbag_zones = pbag_records[pzone_idx:pzone_idx + pzone_len]
        
        for zi_idx, (z_pgen_idx, z_pgen_len) in enumerate(pbag_zones):
            if z_pgen_len == 0:
                continue
            
            if z_pgen_idx + z_pgen_len > len(pgen_records):
                continue
            
            zone_gens = pgen_records[z_pgen_idx:z_pgen_idx + z_pgen_len]
            
            sample_inst_id = None
            key_range = None
            vel_range = None
            
            for op, val in zone_gens:
                if op == 0x1F:  # sample instrument ID
                    sample_inst_id = val
                elif op == 0x28:  # key range
                    key_range = (val >> 8, val & 0xFF)
                elif op == 0x29:  # velocity range
                    vel_range = (val >> 8, val & 0xFF)
            
            if sample_inst_id is not None and sample_inst_id < len(instruments):
                inst_name, inst_zone_idx = instruments[sample_inst_id]
                
                # Find ibag zones for this instrument
                # The instrument's zone_idx points to the ibag list
                # We need to determine zone count from the next instrument's zone_idx
                next_inst_zone_idx = inst_zone_idx
                for j in range(sample_inst_id + 1, len(instruments)):
                    if instruments[j][0] != '':
                        next_inst_zone_idx = instruments[j][1]
                        break
                
                if inst_zone_idx >= len(ibag_records):
                    continue
                    
                # Get ibag zones for this instrument
                ibag_zones = ibag_records[inst_zone_idx:min(next_inst_zone_idx, len(ibag_records))]
                
                for ib_zi_idx, (ib_pgen_idx, ib_pgen_len) in enumerate(ibag_zones):
                    if ib_pgen_len == 0:
                        continue
                    
                    if ib_pgen_idx + ib_pgen_len > len(igen_records):
                        continue
                    
                    inst_gens = igen_records[ib_pgen_idx:ib_pgen_idx + ib_pgen_len]
                    
                    smpl_idx = None
                    inst_key_range = None
                    for op, val in inst_gens:
                        if op == 0x1F:  # sample ID
                            smpl_idx = val
                        elif op == 0x28:  # key range
                            inst_key_range = (val >> 8, val & 0xFF)
                    
                    if smpl_idx is not None and smpl_idx < len(samples):
                        smpl = samples[smpl_idx]
                        if prog not in program_samples:
                            program_samples[prog] = []
                        program_samples[prog].append({
                            'sample_idx': smpl_idx,
                            'sample_name': smpl['name'],
                            'sample_rate': smpl['rate'],
                            'orig_key': smpl['key'],
                            'key_range': inst_key_range or key_range,
                            'data_offset': smpl['data_offset'],
                            'data_length': smpl['data_length'],
                            'loop_start': smpl['start_loop'],
                            'loop_end': smpl['end_loop'],
                            'loop_length': smpl['loop_length'],
                        })
    
    return program_samples, samples, data

def emit_header(program_samples, samples, data, output_path):
    """Emit C header file with sample data and mapping tables."""
    
    # Collect unique samples needed
    needed_sample_indices = set()
    for prog, entries in program_samples.items():
        for e in entries:
            needed_sample_indices.add(e['sample_idx'])
    
    # Build sample data blob
    # Each sample: 16-bit signed PCM, stored as raw bytes
    # We need to extract from the smpl chunk
    sample_blobs = {}
    total_size = 0
    for idx in sorted(needed_sample_indices):
        smpl = samples[idx]
        offset = smpl['data_offset'] + smpl_off  # offset into file
        length = smpl['data_length']
        sample_blobs[idx] = data[offset:offset + length]
        total_size += length
    
    print(f"Total sample data size: {total_size} bytes ({total_size/1024:.1f}KB)", file=sys.stderr)
    
    # Write header
    lines = []
    lines.append("// Auto-generated by tools/sf2_convert.py")
    lines.append("// SoundFont sample data for DOOM GM programs")
    lines.append(f"// Total samples: {len(needed_sample_indices)}, data: {total_size} bytes")
    lines.append("")
    lines.append("#ifndef SOUNDFONT_DATA_H")
    lines.append("#define SOUNDFONT_DATA_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    
    # Sample data array
    lines.append("// Raw 16-bit signed PCM sample data")
    lines.append(f"static const int16_t soundfont_data[{total_size // 2}] = {{")
    
    # Write sample data as hex words
    for idx in sorted(needed_sample_indices):
        blob = sample_blobs[idx]
        smpl = samples[idx]
        lines.append(f"    // Sample {idx}: {smpl['name']} ({smpl['rate']}Hz, key={smpl['key']}, len={len(blob)//2})")
        # Write as 16-bit values
        words = []
        for i in range(0, len(blob), 2):
            if i + 2 <= len(blob):
                val = struct.unpack('<h', blob[i:i+2])[0]
                words.append(val)
        
        # Emit in batches of 16
        for j in range(0, len(words), 16):
            chunk = words[j:j+16]
            hex_str = ', '.join(f'{w:5d}' if w >= 0 else f'{w:5d}' for w in chunk)
            if j == 0:
                lines.append(f'    {hex_str},')
            else:
                lines.append(f'    {hex_str},')
    
    lines.append("};")
    lines.append("")
    
    # Sample info table
    lines.append("// Sample info: offset in soundfont_data, length, sample rate, original key, loop start/end")
    lines.append("typedef struct {")
    lines.append("    int offset;     // start offset in soundfont_data (samples)")
    lines.append("    int length;     // total length (samples)")
    lines.append("    int loop_start; // loop start offset (samples)")
    lines.append("    int loop_end;   // loop end offset (samples)")
    lines.append("    int sample_rate; // original sample rate (Hz)")
    lines.append("    int orig_key;    // original MIDI key")
    lines.append("} soundfont_sample_t;")
    lines.append("")
    
    # Build sample table
    offset = 0
    lines.append("static const soundfont_sample_t soundfont_samples[] = {")
    for idx in sorted(needed_sample_indices):
        smpl = samples[idx]
        blob = sample_blobs[idx]
        length = len(blob) // 2
        loop_start = smpl['start_loop'] - smpl['start']
        loop_end = smpl['end_loop'] - smpl['start']
        lines.append(f'    {{.offset = {offset}, .length = {length}, '
                     f'.loop_start = {loop_start}, .loop_end = {loop_end}, '
                     f'.sample_rate = {smpl["rate"]}, .orig_key = {smpl["key"]}}},')
        offset += length
    
    lines.append("};")
    lines.append("")
    
    # Program -> sample mapping table
    # Each program has one or more sample zones (key range + sample)
    lines.append("// Program -> sample zone mapping")
    lines.append("typedef struct {")
    lines.append("    int sample_idx;  // index into soundfont_samples")
    lines.append("    int key_lo;      // low key for this zone (0-127)")
    lines.append("    int key_hi;      // high key for this zone (0-127)")
    lines.append("} sf_zone_t;")
    lines.append("")
    
    # Build per-program zone tables
    lines.append("// Per-program zone tables")
    for prog in sorted(program_samples.keys()):
        entries = program_samples[prog]
        pname, _, _, _ = gm_presets[prog]
        lines.append(f"// Program {prog}: {pname}")
        lines.append(f"static const sf_zone_t sf_zones_prog_{prog}[] = {{")
        for e in entries:
            key_range = e['key_range'] or (0, 127)
            sample_idx = e['sample_idx']
            # Map sample index to our compact index
            compact_idx = sorted(needed_sample_indices).index(sample_idx)
            lines.append(f'    {{{compact_idx}, {key_range[0]}, {key_range[1]}}},')
        lines.append("};")
        lines.append("")
    
    # Master mapping table: program -> zone table pointer + count
    lines.append("// Master mapping: GM program -> zone table")
    lines.append("typedef struct {")
    lines.append("    const sf_zone_t *zones;")
    lines.append("    int num_zones;")
    lines.append("} sf_program_t;")
    lines.append("")
    lines.append("static const sf_program_t soundfont_programs[128] = {")
    
    for prog in range(128):
        if prog in program_samples:
            lines.append(f'    {{sf_zones_prog_{prog}, {len(program_samples[prog])}}},')
        else:
            # Use program 0 (Piano) as default for undefined programs
            if 0 in program_samples:
                lines.append(f'    {{sf_zones_prog_0, {len(program_samples[0])}}},')
            else:
                lines.append(f'    {{NULL, 0}},')
    
    lines.append("};")
    lines.append("")
    lines.append("#endif // SOUNDFONT_DATA_H")
    
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))
    
    print(f"Wrote {output_path}", file=sys.stderr)

if __name__ == '__main__':
    sf2_path = '/Users/jack/Downloads/SC-55.SF2'
    output_path = 'src/device/soundfont_data.h'
    
    program_samples, samples, data = parse_sf2(sf2_path)
    emit_header(program_samples, samples, data, output_path)
