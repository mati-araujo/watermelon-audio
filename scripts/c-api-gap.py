#!/usr/bin/env python3
"""WA-0.1 — coverage of the C API (`wma_*`) against the JNI surface.

The JNI bridge is the de-facto complete API: Android has been shipping on it.
The C API is what Kotlin/Native will bind to via cinterop (D1), so every
non-USB JNI entry point needs a `wma_*` counterpart before iOS can reach
parity (WA-2.5).

Run:  python3 scripts/c-api-gap.py [--markdown]

Matching is token-set based because the two surfaces use different naming
conventions (`nativeStartEngine` vs `wma_engine_start`) over the same
vocabulary. Names are lowercased, split, the `wma` prefix dropped, and a few
plural/singular variants folded together. `get`/`set`/`is`/`has` are kept:
they distinguish a getter from a setter.

Exact token-set equality is reported as covered. Everything else is a gap,
except that near-misses (Jaccard >= 0.6) are flagged separately — those are
usually the same function under a different name and need a human to confirm.
"""
import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
JNI_SRC = REPO / "audio/src/main/cpp/jni/jni_audio_bridge.cpp"
CAPI_SRC = REPO / "audio/src/main/cpp/api/watermelon_audio.h"

JNI_PREFIX = "Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_native"
# Appears only in a doc comment as a placeholder, not a real entry point.
JNI_PLACEHOLDERS = {"Xxx"}

NOISE = {"wma"}
SYNONYM = {
    "params": "param", "parameters": "param", "parameter": "param",
    "effects": "effect", "voices": "voice", "engines": "engine",
    "regions": "region", "tracks": "track", "layers": "layer",
}

CATEGORY_RULES = [
    ("USB (Android-only)", ("usb", "uac", "libusb")),
    ("Looper", ("looper", "loop", "overdub", "punch")),
    ("Mixer / Regions", ("mixer", "region", "clip")),
    ("Mode transitions", ("mode", "transition")),
    ("Analysis", ("spectrum", "waveform", "fft", "rms", "peak", "meter", "analy")),
    ("Metronome", ("metronome", "click", "tempo", "bpm", "beat")),
    ("Input / monitor", ("input", "monitor", "record", "mic", "gate")),
    ("Benchmark / diagnostics", ("benchmark", "latency", "stat", "diag", "profil", "debug", "log")),
    ("Effects", ("effect", "bypass", "wet", "dry", "preset")),
    ("Voice / polyphony", ("voice", "poly", "steal", "note", "midi", "chord", "touch")),
    ("Engine / lifecycle", ("engine", "start", "stop", "pause", "resume", "init", "shutdown", "fade")),
    ("Oscillator / synth", ("osc", "wave", "freq", "amplitude", "detune", "octave", "scale", "arp", "sequencer")),
    ("Modulation", ("lfo", "envelope", "adsr", "modulat", "automation")),
]


def normalize(tokens):
    out = set()
    for t in tokens:
        if not t or t in NOISE:
            continue
        out.add(SYNONYM.get(t, t))
    return frozenset(out)


def camel_tokens(name):
    return normalize(p.lower() for p in re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+", name))


def snake_tokens(name):
    return normalize(name.lower().split("_"))


def categorize(name):
    n = name.lower()
    for label, keys in CATEGORY_RULES:
        if any(k in n for k in keys):
            return label
    return "Otros"


def jaccard(a, b):
    return len(a & b) / len(a | b) if (a | b) else 0.0


def collect():
    jni_src = JNI_SRC.read_text()
    jni = sorted({
        m for m in re.findall(re.escape(JNI_PREFIX) + r"([A-Za-z0-9_]+)", jni_src)
    } - JNI_PLACEHOLDERS)
    capi = sorted(set(re.findall(r"\b(wma_[a-z0-9_]+)\s*\(", CAPI_SRC.read_text())))
    return jni, capi


def analyze():
    jni, capi = collect()
    capi_keys = {c: snake_tokens(c) for c in capi}

    covered, gap = {}, []
    for j in jni:
        key = camel_tokens(j)
        match = [c for c in capi if capi_keys[c] == key]
        if match:
            covered[j] = match[0]
        else:
            gap.append(j)

    unused = [c for c in capi if c not in set(covered.values())]

    near = {}
    for j in gap:
        key = camel_tokens(j)
        best = max(((jaccard(key, capi_keys[c]), c) for c in unused), default=(0, None))
        if best[0] >= 0.6:
            near[j] = best

    usb = [j for j in gap if categorize(j).startswith("USB")]
    portable = [j for j in gap if j not in usb]
    return dict(jni=jni, capi=capi, covered=covered, gap=gap, unused=unused,
                near=near, usb=usb, portable=portable)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--markdown", action="store_true", help="emit the coverage doc on stdout")
    args = ap.parse_args()
    r = analyze()

    net = len(r["portable"]) - len([j for j in r["near"] if j in r["portable"]])
    if not args.markdown:
        print(f"JNI entry points:            {len(r['jni'])}")
        print(f"C API functions:             {len(r['capi'])}")
        print(f"Covered (exact match):       {len(r['covered'])}")
        print(f"Gap:                         {len(r['gap'])}")
        print(f"  USB (Android-only, D4):    {len(r['usb'])}")
        print(f"  Portable gap:              {len(r['portable'])}")
        print(f"    with a near-match:       {len([j for j in r['near'] if j in r['portable']])}")
        print(f"    net (needs new C API):   ~{net}")
        print(f"C API not reached from JNI:  {len(r['unused'])}")
        return 0

    by_cat = defaultdict(list)
    for j in r["portable"]:
        by_cat[categorize(j)].append(j)

    out = []
    out.append("<!--SUMMARY-->")
    out.append("| Métrica | Valor |\n|---|---|")
    out.append(f"| JNIEXPORT (entry points) | {len(r['jni'])} |")
    out.append(f"| Funciones `wma_*` | {len(r['capi'])} |")
    out.append(f"| Cubiertas (match exacto) | {len(r['covered'])} |")
    out.append(f"| **Gap total** | **{len(r['gap'])}** |")
    out.append(f"| — USB, no se porta (D4) | {len(r['usb'])} |")
    out.append(f"| — **Gap portable** | **{len(r['portable'])}** |")
    out.append(f"| — con near-match (revisar) | {len([j for j in r['near'] if j in r['portable']])} |")
    out.append(f"| — **neto a implementar** | **~{net}** |")
    out.append("")
    out.append("<!--CATEGORIES-->")
    out.append("| Categoría | Funciones |\n|---|---|")
    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        out.append(f"| {cat} | {len(by_cat[cat])} |")
    out.append("")
    out.append("<!--DETAIL-->")
    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        out.append(f"### {cat} ({len(by_cat[cat])})\n")
        for j in sorted(by_cat[cat]):
            hint = f" — near-match: `{r['near'][j][1]}` ({r['near'][j][0]:.2f})" if j in r["near"] else ""
            out.append(f"- `native{j}`{hint}")
        out.append("")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
