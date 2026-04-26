#!/usr/bin/env python3
"""
Decode captured OOK raw pulses from an ESPHome remote_receiver log.

Protocol (see protocol.txt):
    - Carrier: 304.30 MHz OOK
    - Chip duration: 333 us
    - Each logical bit -> 3 chips: '1', '0', b
    - Message = preamble(4) + FAN_ID(14) + cmdid(7) = 25 bits = 75 chips
    - All cmdids end in '0', so the receiver-captured pulse list ends at
      the last HIGH chip (chip 72) and the trailing '00' chips of bit 24
      merge with the inter-burst pause. We see 24 bits + 1 leftover '1'
      chip; we infer bit 24 = '0' to reconstruct the full 25-bit message.

After every button press the remote also emits a few "trailer" bursts
whose 7-bit cmdid is `0000000`. We label those as such instead of as
unknown buttons.

Usage:
    python3 decode_fan_remote.py capture.txt
    cat capture.txt | python3 decode_fan_remote.py
"""

import re
import sys
from collections import Counter

CHIP_US           = 333
PREAMBLE          = "1111"
GAP_THRESHOLD_US  = 5000
PREAMBLE_LEN      = 4
FANID_LEN         = 14
CMDID_LEN         = 7
MESSAGE_BITS      = PREAMBLE_LEN + FANID_LEN + CMDID_LEN          # 25

CMDS = {
    'reverse': '0001000',
    'light':   '0000010',
    'stop':    '0000100',
    'speed1':  '0010000',
    'speed2':  '0010100',
    'speed3':  '0100000',
    'speed4':  '0110000',
    'speed5':  '1000100',
    'speed6':  '1000000',
    'pair':    '0100100',
}

CMD_LOOKUP = {v: k for k, v in CMDS.items()}

# Known FAN_IDs for nicer decoder output
FAN_IDS = {
    '00011001111000': 'living_room',
    '01011111101000': 'parents_room',
    '10001010010000': 'baby_room',
    '01111011000010': 'upstairs',
}

# Trailer cmdids — emitted after every button press as "release" / "idle"
# signaling. Always all-zeros in the 7-bit cmdid space. The trailer's
# FAN_ID may differ slightly from the regular FAN_ID for some fans (an
# artifact of where the trailer's bits land relative to the FAN_ID
# boundary); both forms are flagged here.
TRAILER_CMDID = '0000000'


def name_of_cmdid(cmdid):
    if cmdid == TRAILER_CMDID:
        return 'trailer'
    return CMD_LOOKUP.get(cmdid, '<unknown>')


def name_of_fanid(fan_id):
    return FAN_IDS.get(fan_id, '<unknown>')


def parse_raw_events(raw_str):
    """Parse an ESPHome log into a list of bursts (one per Received Raw: event)."""
    bursts = []
    current = None
    for line in raw_str.splitlines():
        m_new = re.search(r'Received Raw:\s*', line)
        m_cont = re.search(r']:\s{2,}', line)
        if m_new:
            if current is not None:
                bursts.append(current)
            current = []
            data_portion = line[m_new.end():]
        elif m_cont and current is not None:
            data_portion = line[m_cont.end():]
        else:
            continue
        for tok in re.findall(r'-?\d+', data_portion):
            try:
                current.append(int(tok))
            except ValueError:
                pass
    if current is not None:
        bursts.append(current)
    return bursts


def parse_raw(raw_str):
    """Backward-compatible: flatten everything into one pulse list."""
    bursts = parse_raw_events(raw_str)
    return [p for b in bursts for p in b]


def split_bursts(pulses, gap_us=GAP_THRESHOLD_US):
    """Split pulses on long LOW gaps (fallback when no log markers exist)."""
    bursts, current = [], []
    for p in pulses:
        if p < 0 and abs(p) >= gap_us:
            if current:
                bursts.append(current); current = []
        else:
            current.append(p)
    if current:
        bursts.append(current)
    return bursts


def pulses_to_chips(pulses, chip_us=CHIP_US):
    chips = []
    for p in pulses:
        level = '1' if p > 0 else '0'
        n = max(1, round(abs(p) / chip_us))
        chips.append(level * n)
    return ''.join(chips)


def decode_chips(chips):
    """Parse chips as '10b' groups. Returns (bits, leftover_chips, bad_groups)."""
    bits, bad, i = [], 0, 0
    while i + 3 <= len(chips):
        g = chips[i:i+3]
        if g[:2] == '10':
            bits.append(g[2])
        else:
            bad += 1
        i += 3
    return ''.join(bits), chips[i:], bad


def reconstruct_25bit(bits, leftover):
    """
    The captured pulse list ends at the last HIGH chip of the message.
    For cmdids ending in '0' (all of ours), the last bit's '00' chip tail
    merges with the pause and is invisible — we see 24 decoded bits plus
    a leftover '1' chip (= chip 72, the first chip of bit 24's '10b'
    encoding). Bit 24 is the third chip of that group, which lives in the
    pause = LOW = '0'.
    """
    if len(bits) >= MESSAGE_BITS:
        return bits[:MESSAGE_BITS], 'fully decoded'
    if len(bits) == MESSAGE_BITS - 1:
        return bits + '0', 'inferred bit24=0'
    return bits, f'truncated ({len(bits)}/{MESSAGE_BITS} bits)'


def decode_burst(idx, pulses):
    chips = pulses_to_chips(pulses)
    bits, leftover, bad = decode_chips(chips)
    msg, source = reconstruct_25bit(bits, leftover)

    if len(msg) >= MESSAGE_BITS:
        pre = msg[:PREAMBLE_LEN]
        fid = msg[PREAMBLE_LEN:PREAMBLE_LEN + FANID_LEN]
        cmd = msg[PREAMBLE_LEN + FANID_LEN:MESSAGE_BITS]
        if pre == PREAMBLE:
            fan_name = name_of_fanid(fid)
            cmd_name = name_of_cmdid(cmd)
            print(f"Burst {idx:3d}: pulses={len(pulses):3d}  "
                  f"FAN_ID={fid} ({fan_name})  cmdid={cmd}  → {cmd_name}")
            return {'fan_id': fid, 'cmdid': cmd, 'cmd_name': cmd_name, 'fan_name': fan_name}
        else:
            print(f"Burst {idx:3d}: pulses={len(pulses):3d}  bits={msg}  BAD PREAMBLE ({pre})")
    else:
        print(f"Burst {idx:3d}: pulses={len(pulses):3d}  truncated  bits={bits}  ({source})")
    return None


def decode_capture(raw):
    bursts = parse_raw_events(raw)
    if not bursts:
        pulses = parse_raw(raw)
        bursts = split_bursts(pulses)
        method = f"split on LOW >= {GAP_THRESHOLD_US} us (raw mode)"
    else:
        method = "split per 'Received Raw:' event"

    total = sum(len(b) for b in bursts)
    print(f"Input: {total} pulses, {len(bursts)} bursts ({method})")
    print("=" * 78)

    results = []
    for i, b in enumerate(bursts, 1):
        r = decode_burst(i, b)
        if r:
            results.append(r)

    if not results:
        print("\nNo fully-decoded bursts.")
        return

    print("=" * 78)
    print(f"Decoded {len(results)}/{len(bursts)} bursts.")
    fid_ct = Counter((r['fan_id'], r['fan_name']) for r in results)
    print(f"FAN_IDs seen:")
    for (fid, fname), n in fid_ct.most_common():
        print(f"  {fid} ({fname}): {n} bursts")
    print()
    print(f"cmdid -> button mapping (sorted by frequency):")
    for cmd, n in Counter(r['cmdid'] for r in results).most_common():
        print(f"  cmdid={cmd}  count={n:3d}  -> {name_of_cmdid(cmd)}")


if __name__ == '__main__':
    data = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    decode_capture(data)
