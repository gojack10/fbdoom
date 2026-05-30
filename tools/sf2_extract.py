#!/usr/bin/env python3
"""Extract SC-55 SoundFont samples for DOOM GM programs and emit C header."""
import struct, sys

SF2_PATH = '/Users/jack/Downloads/SC-55.SF2'
OUTPUT_H = 'src/device/soundfont_data.h'

DOOM_PROGRAMS = {0, 6, 7, 10, 11, 13, 15, 18, 28, 29, 30, 31, 32, 33, 34, 37, 38, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 55, 62, 63, 70, 72, 75, 79, 80, 81, 82, 87, 92, 94, 97, 101, 102, 108, 117, 118, 119, 120, 123}

# SC-55 SF2 pgen op codes (non-standard):
PGEN_OP_SAMPLE_ID = 48    # 0x30
PGEN_OP_KEY_RANGE = 41    # 0x29

def walk_lists(data):
    """Walk RIFF tree, return dict of LIST type -> subchunks dict."""
    pos = 12
    lists = {}
    while pos < len(data) - 8:
        cid = data[pos:pos+4]
        csize = struct.unpack('<I', data[pos+4:pos+8])[0]
        if cid == b'LIST':
            list_type = data[pos+8:pos+12]
            subchunks = {}
            subpos = pos + 12
            while subpos < pos + 8 + csize:
                if subpos + 8 > len(data):
                    break
                sub_id = data[subpos:subpos+4]
                sub_size = struct.unpack('<I', data[subpos+4:subpos+8])[0]
                sub_data_off = subpos + 8
                subchunks[sub_id] = (sub_size, sub_data_off)
                subpos += 8 + sub_size
                if sub_size % 2:
                    subpos += 1
            lists[list_type] = subchunks
        pos += 8 + csize
        if csize % 2:
            pos += 1
    return lists

def parse(data):
    lists = walk_lists(data)
    pdta = lists.get(b'pdta')
    sdta = lists.get(b'sdta')
    if not pdta or not sdta:
        return None, None, None, None, None

    # phdr: 38-byte records
    phdr_size, phdr_off = pdta[b'phdr']
    n_presets = phdr_size // 38
    presets_by_prog = {}
    for i in range(n_presets):
        off = phdr_off + i * 38
        name_raw = data[off:off+20]
        name = name_raw.decode('ascii','replace').rstrip('\x00').strip().lstrip('\x00').strip()
        if not name:
            continue
        fields = data[off+20:off+38]
        pn = struct.unpack('<H', fields[0:2])[0]
        bank = struct.unpack('<H', fields[2:4])[0]
        zi = struct.unpack('<H', fields[4:6])[0]
        zl = struct.unpack('<H', fields[6:8])[0]
        if bank == 0 and pn not in presets_by_prog:
            presets_by_prog[pn] = (name, zi, zl)

    # pbag: 4-byte records, count inferred from next index
    pbag_size, pbag_off = pdta[b'pbag']
    n_pbag = pbag_size // 4
    pbag_indices = []
    for i in range(n_pbag):
        off = pbag_off + i * 4
        val = struct.unpack('<I', data[off:off+4])[0]
        pbag_indices.append(val & 0xFFFF)

    # pgen: 4-byte records
    pgen_size, pgen_off = pdta[b'pgen']
    n_pgen = pgen_size // 4
    pgen_recs = [struct.unpack('<HH', data[pgen_off+i*4:pgen_off+i*4+4]) for i in range(n_pgen)]

    # inst: 22-byte records
    inst_size, inst_off = pdta[b'inst']
    n_insts = inst_size // 22
    instruments = []
    for i in range(n_insts):
        off = inst_off + i * 22
        name = data[off:off+20].decode('ascii','replace').rstrip('\x00').strip()
        zi = struct.unpack('<H', data[off+20:off+22])[0]
        instruments.append((name, zi))

    # ibag: inferred count
    ibag_size, ibag_off = pdta[b'ibag']
    n_ibag = ibag_size // 4
    ibag_indices = []
    for i in range(n_ibag):
        off = ibag_off + i * 4
        val = struct.unpack('<I', data[off:off+4])[0]
        ibag_indices.append(val & 0xFFFF)

    # igen: 4-byte records
    igen_size, igen_off = pdta[b'igen']
    n_igen = igen_size // 4
    igen_recs = [struct.unpack('<HH', data[igen_off+i*4:igen_off+i*4+4]) for i in range(n_igen)]

    # shdr: 46-byte records (16 name + 30 fields)
    # Field layout: [20]=sample_start, [24]=sample_end, [28]=loop_start, [32]=loop_end, [36]=rate
    shdr_size, shdr_off = pdta[b'shdr']
    n_shdr = shdr_size // 46
    samples = []
    for i in range(n_shdr):
        off = shdr_off + i * 46
        raw = data[off:off+46]
        name = raw[0:16].split(b'\x00',1)[0].decode('ascii','ignore')
        sample_start = struct.unpack('<I', raw[20:24])[0]  # start sample index
        sample_end = struct.unpack('<I', raw[24:28])[0]    # end sample index
        loop_start = struct.unpack('<I', raw[28:32])[0]    # loop start index
        loop_end = struct.unpack('<I', raw[32:36])[0]      # loop end index
        rate = struct.unpack('<I', raw[36:40])[0]          # sample rate
        key = raw[44]                                       # origin key
        samples.append({
            'name': name,
            'start': sample_start, 'end': sample_end,
            'start_loop': loop_start, 'end_loop': loop_end,
            'rate': rate, 'key': key,
        })

    # smpl data offset
    smpl_off = sdta[b'smpl'][1]

    # Helper: get zone ranges from indices
    def get_zones(indices, zone_idx, zone_len):
        if zone_idx >= len(indices) - 1 or zone_len == 0:
            return []
        end = min(zone_idx + zone_len + 1, len(indices))
        zids = indices[zone_idx:end]
        zones = []
        for zi in range(len(zids) - 1):
            gi, ngi = zids[zi], zids[zi + 1]
            if gi == 0 and ngi == 0:
                continue
            zones.append((gi, ngi - gi))
        return zones

    # Trace DOOM programs
    inst_samples = {}
    for gm_prog in DOOM_PROGRAMS:
        sf_prog = gm_prog
        if sf_prog == 0:
            sf_prog = 1
        if sf_prog not in presets_by_prog:
            continue
        pname, pbag_idx, pbag_len = presets_by_prog[sf_prog]
        zones = get_zones(pbag_indices, pbag_idx, pbag_len)

        sample_list = []
        for zi, (gi, gc) in enumerate(zones):
            gens = pgen_recs[gi:gi+gc]
            smpl_id = None
            kl, kh = 0, 127
            for op, val in gens:
                if op == PGEN_OP_SAMPLE_ID:
                    smpl_id = val
                elif op == PGEN_OP_KEY_RANGE:
                    kl = val >> 8
                    kh = val & 0xFF
            if smpl_id is not None and smpl_id < len(samples):
                smpl = samples[smpl_id]
                sample_list.append((smpl_id, kl, kh))
                length = (smpl['end'] - smpl['start']) * 2
                print(f"  prog {gm_prog:3d} ({pname:20s}) -> sample {smpl_id:3d} ({smpl['name']}) "
                      f"keys=[{kl}-{kh}] rate={smpl['rate']}Hz len={length}B "
                      f"loop=[{smpl['start_loop']-smpl['start']}-{smpl['end_loop']-smpl['start']}]",
                      file=sys.stderr)
        inst_samples[gm_prog] = sample_list

    return inst_samples, samples, data, smpl_off, presets_by_prog

def emit_c_header(inst_samples, samples, data, smpl_off, presets_by_prog):
    needed = set()
    for prog, entries in inst_samples.items():
        for si, _, _ in entries:
            needed.add(si)
    needed_sorted = sorted(needed)
    print(f"\nTotal unique samples: {len(needed_sorted)}", file=sys.stderr)

    compact_idx = {}
    sample_table = []
    for idx in needed_sorted:
        smpl = samples[idx]
        off = smpl_off + smpl['start'] * 2
        length = (smpl['end'] - smpl['start']) * 2
        raw = data[off:off + length]
        compact_idx[idx] = len(sample_table)
        sample_table.append({
            'data': raw,
            'rate': smpl['rate'],
            'key': smpl['key'],
            'loop_start': max(0, smpl['start_loop'] - smpl['start']),
            'loop_end': max(0, smpl['end_loop'] - smpl['start']),
        })

    total_size = sum(len(s['data']) for s in sample_table)
    print(f"Total sample data: {total_size} bytes ({total_size/1024:.1f}KB)", file=sys.stderr)

    lines = []
    lines.append("// Auto-generated by tools/sf2_extract.py")
    lines.append("// SC-55 SoundFont samples for DOOM music playback")
    lines.append(f"// {len(sample_table)} samples, {total_size} bytes total")
    lines.append("#ifndef SOUNDFONT_DATA_H")
    lines.append("#define SOUNDFONT_DATA_H")
    lines.append("#include <stdint.h>")
    lines.append("")

    lines.append("static const int16_t sf_sample_data[] = {")
    for si, smpl in enumerate(sample_table):
        raw = smpl['data']
        words = [struct.unpack('<h', raw[i:i+2])[0] for i in range(0, len(raw), 2)]
        lines.append(f'  // Sample {si}: rate={smpl["rate"]}Hz key={smpl["key"]} len={len(words)}')
        for j in range(0, len(words), 16):
            chunk = words[j:j+16]
            lines.append('  ' + ', '.join(f'{w}' for w in chunk) + ',')
    lines.append("};")
    lines.append("")

    lines.append("typedef struct {")
    lines.append("  int32_t offset;")
    lines.append("  int32_t length;")
    lines.append("  int32_t loop_start;")
    lines.append("  int32_t loop_end;")
    lines.append("  int32_t sample_rate;")
    lines.append("  int32_t orig_key;")
    lines.append("} sf_sample_t;")
    lines.append("")

    off = 0
    lines.append("static const sf_sample_t sf_samples[] = {")
    for si, smpl in enumerate(sample_table):
        words = len(smpl['data']) // 2
        lines.append(f'  {{ {off}, {words}, {smpl["loop_start"]}, {smpl["loop_end"]}, '
                     f'{smpl["rate"]}, {smpl["key"]} }},')
        off += words
    lines.append("};")
    lines.append("")

    lines.append("typedef struct {")
    lines.append("  int sample_idx;")
    lines.append("  int key_lo;")
    lines.append("  int key_hi;")
    lines.append("} sf_zone_t;")
    lines.append("")

    for gm_prog in sorted(inst_samples.keys()):
        entries = inst_samples[gm_prog]
        pname = presets_by_prog[gm_prog if gm_prog != 0 else 1][0]
        lines.append(f"// Program {gm_prog}: {pname}")
        lines.append(f"static const sf_zone_t sf_zones_{gm_prog}[] = {{")
        for si, kl, kh in entries:
            ci = compact_idx[si]
            lines.append(f"  {{ {ci}, {kl}, {kh} }},")
        lines.append("};")
        lines.append("")

    lines.append("typedef struct {")
    lines.append("  const sf_zone_t *zones;")
    lines.append("  int num_zones;")
    lines.append("} sf_program_t;")
    lines.append("")
    lines.append("static const sf_program_t sf_programs[128] = {")
    for p in range(128):
        if p in inst_samples:
            lines.append(f"  {{ sf_zones_{p}, {len(inst_samples[p])} }},")
        elif 0 in inst_samples:
            lines.append(f"  {{ sf_zones_0, {len(inst_samples[0])} }},")
        else:
            lines.append("  { NULL, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("#endif // SOUNDFONT_DATA_H")

    with open(OUTPUT_H, 'w') as f:
        f.write('\n'.join(lines))
    print(f"Wrote {OUTPUT_H}", file=sys.stderr)

if __name__ == '__main__':
    with open(SF2_PATH, 'rb') as f:
        data = f.read()
    inst_samples, samples, data, smpl_off, presets_by_prog = parse(data)
    if inst_samples:
        emit_c_header(inst_samples, samples, data, smpl_off, presets_by_prog)
