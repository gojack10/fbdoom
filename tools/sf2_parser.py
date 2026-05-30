#!/usr/bin/env python3
"""
Proper RIFF-walking SF2 parser.
Finds all subchunks by walking the RIFF tree.
"""
import struct, sys

SF2_PATH = '/Users/jack/Downloads/SC-55.SF2'

def parse_sf2(path):
    with open(path, 'rb') as f:
        data = f.read()
    
    pos = 0
    riff_id = data[0:4]
    if riff_id != b'RIFF':
        print("Not a RIFF file", file=sys.stderr)
        return None
    
    file_size = struct.unpack('<I', data[4:8])[0]
    form_type = data[8:12]
    print(f"RIFF: form={form_type} size={file_size}", file=sys.stderr)
    
    # Walk top-level chunks starting at 12
    pos = 12
    chunks = {}  # chunk_id -> (offset, size)
    lists = {}   # list_type -> {chunk_id -> (offset, size)}
    
    while pos < len(data) - 8:
        chunk_id = data[pos:pos+4]
        chunk_size = struct.unpack('<I', data[pos+4:pos+8])[0]
        
        if chunk_id == b'LIST':
            list_type = data[pos+8:pos+12]
            list_type_str = list_type.decode('ascii', errors='replace')
            print(f"  LIST {list_type_str}: size={chunk_size}", file=sys.stderr)
            
            subchunks = {}
            subpos = pos + 12
            while subpos < pos + 8 + chunk_size:
                if subpos + 8 > len(data):
                    break
                sub_id = data[subpos:subpos+4]
                sub_size = struct.unpack('<I', data[subpos+4:subpos+8])[0]
                sub_data_off = subpos + 8
                sub_id_str = sub_id.decode('ascii', errors='replace')
                print(f"    {sub_id_str}: size={sub_size} data_off={sub_data_off}", file=sys.stderr)
                subchunks[sub_id] = (sub_size, sub_data_off)
                subpos += 8 + sub_size
                if sub_size % 2:
                    subpos += 1
            
            lists[list_type] = subchunks
        else:
            chunks[chunk_id] = (chunk_size, pos + 8)
        
        pos += 8 + chunk_size
        if chunk_size % 2:
            pos += 1
    
    return data, lists

def parse_shdr(data, shdr_info):
    """Parse sample headers (76 bytes each)."""
    shdr_size, shdr_off = shdr_info
    n = shdr_size // 76
    samples = []
    for i in range(n):
        off = shdr_off + i * 76
        name = data[off:off+16].decode('ascii', errors='replace').rstrip('\x00').strip()
        start = struct.unpack('<I', data[off+16:off+20])[0]
        end = struct.unpack('<I', data[off+20:off+24])[0]
        start_loop = struct.unpack('<I', data[off+24:off+28])[0]
        end_loop = struct.unpack('<I', data[off+28:off+32])[0]
        rate = struct.unpack('<I', data[off+32:off+36])[0]
        key = data[off+44]
        samples.append({
            'name': name, 'start': start, 'end': end,
            'start_loop': start_loop, 'end_loop': end_loop,
            'rate': rate, 'key': key,
        })
    return samples

def parse_phdr(data, phdr_info):
    """Parse preset headers (38 bytes each)."""
    phdr_size, phdr_off = phdr_info
    n = phdr_size // 38
    presets = {}
    for i in range(n):
        off = phdr_off + i * 38
        name_raw = data[off:off+20]
        name = name_raw.decode('ascii', errors='replace').rstrip('\x00').strip().lstrip('\x00').strip()
        if not name:
            continue
        pn = struct.unpack('<H', data[off+20:off+22])[0]
        bank = struct.unpack('<H', data[off+22:off+24])[0]
        zi = struct.unpack('<H', data[off+24:off+26])[0]
        zl = struct.unpack('<H', data[off+26:off+28])[0]
        if bank == 0 and pn not in presets:
            presets[pn] = (name, zi, zl)
    return presets

def parse_pbag(data, pbag_info):
    """Parse preset bag indices (4 bytes each, count inferred from next)."""
    pbag_size, pbag_off = pbag_info
    n = pbag_size // 4
    indices = []
    for i in range(n):
        off = pbag_off + i * 4
        val = struct.unpack('<I', data[off:off+4])[0]
        indices.append(val & 0xFFFF)
    return indices

def parse_pgen(data, pgen_info):
    """Parse preset generators (4 bytes each)."""
    pgen_size, pgen_off = pgen_info
    n = pgen_size // 4
    recs = []
    for i in range(n):
        off = pgen_off + i * 4
        op = struct.unpack('<H', data[off:off+2])[0]
        val = struct.unpack('<H', data[off+2:off+4])[0]
        recs.append((op, val))
    return recs

def parse_inst(data, inst_info):
    """Parse instrument headers (22 bytes each)."""
    inst_size, inst_off = inst_info
    n = inst_size // 22
    insts = []
    for i in range(n):
        off = inst_off + i * 22
        name = data[off:off+20].decode('ascii', errors='replace').rstrip('\x00').strip()
        zi = struct.unpack('<H', data[off+20:off+22])[0]
        insts.append((name, zi))
    return insts

def parse_ibag(data, ibag_info):
    """Parse instrument bag indices."""
    ibag_size, ibag_off = ibag_info
    n = ibag_size // 4
    indices = []
    for i in range(n):
        off = ibag_off + i * 4
        val = struct.unpack('<I', data[off:off+4])[0]
        indices.append(val & 0xFFFF)
    return indices

def parse_igen(data, igen_info):
    """Parse instrument generators (4 bytes each)."""
    igen_size, igen_off = igen_info
    n = igen_size // 4
    recs = []
    for i in range(n):
        off = igen_off + i * 4
        op = struct.unpack('<H', data[off:off+2])[0]
        val = struct.unpack('<H', data[off+2:off+4])[0]
        recs.append((op, val))
    return recs

def get_zones(indices, zone_idx, zone_len):
    """Get zone pgen/igen ranges from indices list.
    zone_len is the number of zones. Each zone's pgen count = next index - current index.
    """
    if zone_idx >= len(indices) - 1 or zone_len == 0:
        return []
    
    # Get the indices for this zone range, plus one extra for count
    start = zone_idx
    end = min(zone_idx + zone_len + 1, len(indices))
    zone_indices = indices[start:end]
    
    zones = []
    for zi in range(len(zone_indices) - 1):
        gi = zone_indices[zi]
        next_gi = zone_indices[zi + 1]
        if gi == 0 and next_gi == 0:
            continue
        count = next_gi - gi
        zones.append((gi, count))
    
    return zones

if __name__ == '__main__':
    data, lists = parse_sf2(SF2_PATH)
    
    # Get pdta subchunks
    pdta = lists.get(b'pdta')
    if not pdta:
        print("No pdta LIST found", file=sys.stderr)
        sys.exit(1)
    
    # Parse all structures
    presets = parse_phdr(data, pdta[b'phdr'])
    pbag_indices = parse_pbag(data, pdta[b'pbag'])
    pgen_recs = parse_pgen(data, pdta[b'pgen'])
    instruments = parse_inst(data, pdta[b'inst'])
    ibag_indices = parse_ibag(data, pdta[b'ibag'])
    igen_recs = parse_igen(data, pdta[b'igen'])
    samples = parse_shdr(data, pdta[b'shdr'])
    
    # Get sdta sample data offset
    sdta = lists.get(b'sdta')
    smpl_info = sdta[b'smpl']
    smpl_off = smpl_info[1]  # data offset for smpl chunk
    
    print(f"\nPresets: {len(presets)}", file=sys.stderr)
    print(f"Instruments: {len(instruments)}", file=sys.stderr)
    print(f"Samples: {len(samples)}", file=sys.stderr)
    print(f"Sample data offset: {smpl_off}", file=sys.stderr)
    
    # DOOM programs
    DOOM_PROGRAMS = {0, 6, 7, 10, 11, 13, 15, 18, 28, 29, 30, 31, 32, 33, 34, 37, 38, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 55, 62, 63, 70, 72, 75, 79, 80, 81, 82, 87, 92, 94, 97, 101, 102, 108, 117, 118, 119, 120, 123}
    
    # Trace each DOOM program
    for gm_prog in sorted(DOOM_PROGRAMS):
        sf_prog = gm_prog
        if sf_prog == 0:
            sf_prog = 1  # Program 0 is terminal, use Piano 2
        
        if sf_prog not in presets:
            continue
        
        pname, pbag_idx, pbag_len = presets[sf_prog]
        zones = get_zones(pbag_indices, pbag_idx, pbag_len)
        
        print(f"\nprog {gm_prog:3d} ({pname}) zones={len(zones)}")
        for zi, (gi, gc) in enumerate(zones):
            gens = pgen_recs[gi:gi+gc]
            smpl_id = None
            key_lo, key_hi = 0, 127
            for op, val in gens:
                if op == 0x1F:  # sampleID
                    smpl_id = val
                elif op == 0x28:  # keyRange
                    key_lo = val >> 8
                    key_hi = val & 0xFF
                elif op == 0x30:  # sampleRate
                    pass  # not needed
            
            if smpl_id is not None:
                smpl = samples[smpl_id]
                print(f"  Zone {zi}: sample={smpl_id} \"{smpl['name']}\" keys=[{key_lo}-{key_hi}] rate={smpl['rate']}Hz len={smpl['end']-smpl['start']}")
PYEOF