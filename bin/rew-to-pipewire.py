#!/usr/bin/env python3
"""Translate a Room EQ Wizard filter export into a PipeWire filter-chain
`filter.graph { ... }` block, ready to paste into the speaker EQ chain:
board/raspad/rootfs/etc/pipewire/pipewire.conf.d/30-raspad-eq.conf (the
RasPad; --bass keeps its bankstown bass node in front of the EQ) or the
pi-radio rt5645 one.

REW's "Equaliser APO" / "Generic" text export has lines like:

    Filter  1: ON  PK       Fc   200.0 Hz  Gain  -3.00 dB  Q  1.500
    Filter  2: ON  LS       Fc    80.0 Hz  Gain   4.00 dB  Q  0.707
    Filter  3: ON  HS       Fc  8000.0 Hz  Gain   2.00 dB  Q  0.707

This script parses those, mirrors the chain across FL and FR (the
codec sums L+R into the right speaker downstream, so applying the
same correction per channel is mathematically correct), and emits a
filter-chain `nodes` / `links` / `inputs` / `outputs` block.

Usage:
    bin/rew-to-pipewire.py [--bass] rew-filters.txt   > snippet.conf
    bin/rew-to-pipewire.py [--bass] < rew-filters.txt > snippet.conf

    --bass  put the RasPad chain's bankstown node ("bass") first, so the
            output replaces that file's whole filter.graph block

Workflow:
    1. In REW: File -> Export -> Filter Settings as text (or APO format).
    2. Run this script on the export.
    3. Open /etc/pipewire/pipewire.conf.d/30-rt5645-eq.conf, replace the
       existing `filter.graph = { ... }` block with the script's output.
    4. systemctl restart pipewire wireplumber  (re-measure to verify)

Disabled filters (OFF) and unrecognised types are skipped with a
warning to stderr.
"""

import re
import sys

# REW filter type -> PipeWire bq_* label.
TYPE_MAP = {
    "PK":  "bq_peaking",
    "LS":  "bq_lowshelf",
    "LSC": "bq_lowshelf",
    "HS":  "bq_highshelf",
    "HSC": "bq_highshelf",
    "LP":  "bq_lowpass",
    "LP6": "bq_lowpass",
    "LP12": "bq_lowpass",
    "LP24": "bq_lowpass",
    "HP":  "bq_highpass",
    "HP6": "bq_highpass",
    "HP12": "bq_highpass",
    "HP24": "bq_highpass",
    "NO":  "bq_notch",
    "AP":  "bq_allpass",
    "BP":  "bq_bandpass",
}

# Match a REW filter line. Gain and Q are both optional (not all
# filter types take them — bq_lowpass for instance ignores Gain).
LINE_RE = re.compile(
    r"^\s*Filter\s+\d+\s*:\s+(ON|OFF)\s+(\S+)\s+Fc\s+([\d.]+)\s*Hz"
    r"(?:\s+Gain\s+([+-]?[\d.]+)\s*dB)?"
    r"(?:\s+Q\s+([\d.]+))?"
)


def parse(stream):
    bands = []
    for line in stream:
        m = LINE_RE.match(line)
        if not m:
            continue
        enabled, ftype, freq, gain, q = m.groups()
        if enabled != "ON":
            continue
        label = TYPE_MAP.get(ftype.upper())
        if not label:
            print(f"# warning: unknown filter type {ftype!r}, skipping",
                  file=sys.stderr)
            continue
        bands.append({
            "label": label,
            "freq": float(freq),
            "gain": float(gain) if gain is not None else 0.0,
            # Default Q matches REW's "neutral" 0.707 (Butterworth-ish).
            "q": float(q) if q is not None else 0.707,
        })
    return bands


# The RasPad chain's bass node (30-raspad-eq.conf), for --bass.
BASS_NODE = """        { type = lv2 name = bass plugin = "https://chadmed.au/bankstown"
            control = {
                bypass     = 0.0
                amt        = 0.9
                sat_second = 1.65
                sat_third  = 2.20
                blend      = 1.0
                floor      = 20.0
                ceil       = 200.0
                final_hp   = 120.0
            } }"""


def emit(bands, bass=False):
    if not bands:
        sys.exit("# no enabled bands found in the input")

    print("filter.graph = {")
    print("    nodes = [")
    if bass:
        print(BASS_NODE)
    for ch in ("FL", "FR"):
        for i, b in enumerate(bands, 1):
            name = f"eq_{ch}_{i}"
            print(
                f"        {{ type = builtin name = {name} label = {b['label']}\n"
                f"            control = {{ Freq = {b['freq']} "
                f"Q = {b['q']} Gain = {b['gain']} }} }}"
            )
    print("    ]")

    print("    links = [")
    if bass:
        print('        { output = "bass:out_l" input = "eq_FL_1:In" }')
        print('        { output = "bass:out_r" input = "eq_FR_1:In" }')
    for ch in ("FL", "FR"):
        for i in range(2, len(bands) + 1):
            prev = f"eq_{ch}_{i-1}"
            cur = f"eq_{ch}_{i}"
            print(f'        {{ output = "{prev}:Out" input = "{cur}:In" }}')
    print("    ]")

    last = len(bands)
    if bass:
        print('    inputs  = [ "bass:in_l"  "bass:in_r"  ]')
    else:
        print('    inputs  = [ "eq_FL_1:In"  "eq_FR_1:In"  ]')
    print(f'    outputs = [ "eq_FL_{last}:Out" "eq_FR_{last}:Out" ]')
    print("}")


if __name__ == "__main__":
    args = sys.argv[1:]
    bass = "--bass" in args
    args = [a for a in args if a != "--bass"]
    src = open(args[0]) if args else sys.stdin
    bands = parse(src)
    emit(bands, bass)
