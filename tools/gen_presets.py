#!/usr/bin/env python3
"""Generate the unified Megalo / MegaloHN factory presets.

Single source of truth for the ~10 "factory" presets shipped across every
format:

  * megalo.lv2/<Name>.ttl        — LV2 preset, stock granular ports only
  * megaloHN.lv2/<Name>.ttl      — LV2 preset, granular ports + H+N timbre ports
  * megalo.lv2/manifest.ttl      — re-listing every preset for the host
  * megaloHN.lv2/manifest.ttl
  * juce/megalo_presets.h        — C++ table consumed by the JUCE plugins
                                   (VST3 / AU / Standalone) for both builds

Run from anywhere:  python3 tools/gen_presets.py
The same name/value pair therefore drives the LV2 and the native formats, so a
preset sounds identical in MOD, a DAW, and the standalone app.
"""

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MEGALO_URI = "https://github.com/pilali/megalo"
HN_URI     = "https://github.com/pilali/megalo/hn"

# ── Preset definitions ─────────────────────────────────────────────────────
# Each preset carries:
#   base : control values shared by both plugins (these are LV2 ports too)
#   pitch_mode : JUCE-only runtime pitch-engine choice (0=Granular, 1=PhaseVoc);
#                in LV2 the engine is selected at build time, so it is omitted
#                from the .ttl but kept in the native table.
#   hn   : MegaloHN-only additive timbre values, tuned so each HN preset has a
#          character of its own rather than mirroring the granular Megalo sound.
#          (dry_level is declared here for readability but is a shared control —
#          see preset(), which moves it into the base set for both plugins.)
#
# base symbols (ranges mirror the .ttl):
#   onset_threshold 0..1   sample_ms 50..500   attack_skip_ms 0..500
#   blend 0..1             grain_size_ms 5..200  grain_xfade_ms 5..100
#   base_pitch -12..12     pitch1_semi/-2_semi -24..24   *_level 0..1
#   detune_cents 0..50     chorus_rate 0.1..8   detune_blend 0..1
#   filter_type 0=LP 1=HP 2=BP   filter_cutoff 20..20000   filter_q 0.1..10
#   env_attack 0..5000     env_decay 0..5000    env_sustain 0..1
#   env_release 0..10000   detune_enable/pitch1_enable/pitch2_enable 0|1
# hn symbols:
#   dry_level 0..2   hn_brightness -1..1   hn_damping 0..1
#   hn_even_odd -1..1   hn_noise 0..1   hn_width 0..1

def preset(name, base, pitch_mode, hn):
    # dry_level is a shared control (both Megalo and MegaloHN), so it lives with
    # the base params even though it is written alongside the timbre values for
    # readability. The onset dry→wet crossfade time is not a control — it
    # follows env_attack — so there is nothing else to move here.
    base = dict(base)
    hn = dict(hn)
    base["dry_level"] = hn.pop("dry_level")
    return {"name": name, "base": base, "pitch_mode": pitch_mode, "hn": hn}

PRESETS = [
    preset("Clean Sustain",
        dict(onset_threshold=0.12, sample_ms=160, attack_skip_ms=40, blend=0.85,
             grain_size_ms=120, grain_xfade_ms=50, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.0, pitch2_semi=12, pitch2_level=0.0,
             detune_cents=8, chorus_rate=0.4, detune_blend=0.15,
             filter_type=0, filter_cutoff=6000, filter_q=0.6,
             env_attack=60, env_decay=200, env_sustain=1.0, env_release=1200,
             detune_enable=0, pitch1_enable=0, pitch2_enable=0),
        pitch_mode=0,
        hn=dict(dry_level=1.0, hn_brightness=0.0, hn_damping=0.2,
                hn_even_odd=0.0, hn_noise=0.25, hn_width=0.2)),

    preset("Octave Pad",
        dict(onset_threshold=0.13, sample_ms=200, attack_skip_ms=60, blend=0.90,
             grain_size_ms=140, grain_xfade_ms=60, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.5, pitch2_semi=0, pitch2_level=0.0,
             detune_cents=12, chorus_rate=0.45, detune_blend=0.25,
             filter_type=0, filter_cutoff=4500, filter_q=0.6,
             env_attack=200, env_decay=300, env_sustain=1.0, env_release=2000,
             detune_enable=1, pitch1_enable=1, pitch2_enable=0),
        pitch_mode=0,
        hn=dict(dry_level=1.0, hn_brightness=-0.2, hn_damping=0.35,
                hn_even_odd=-0.2, hn_noise=0.2, hn_width=0.35)),

    preset("Shimmer",
        dict(onset_threshold=0.12, sample_ms=180, attack_skip_ms=50, blend=0.92,
             grain_size_ms=100, grain_xfade_ms=50, base_pitch=0,
             pitch1_semi=12, pitch1_level=0.45, pitch2_semi=19, pitch2_level=0.30,
             detune_cents=10, chorus_rate=0.5, detune_blend=0.2,
             filter_type=0, filter_cutoff=9000, filter_q=0.5,
             env_attack=400, env_decay=300, env_sustain=1.0, env_release=4000,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=1,
        hn=dict(dry_level=0.9, hn_brightness=0.5, hn_damping=0.1,
                hn_even_odd=0.3, hn_noise=0.15, hn_width=0.5)),

    preset("Choir",
        dict(onset_threshold=0.10, sample_ms=220, attack_skip_ms=70, blend=0.88,
             grain_size_ms=110, grain_xfade_ms=60, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.4, pitch2_semi=12, pitch2_level=0.4,
             detune_cents=30, chorus_rate=1.2, detune_blend=0.5,
             filter_type=0, filter_cutoff=3500, filter_q=0.7,
             env_attack=300, env_decay=250, env_sustain=1.0, env_release=2500,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=0,
        hn=dict(dry_level=0.8, hn_brightness=0.1, hn_damping=0.3,
                hn_even_odd=0.0, hn_noise=0.55, hn_width=0.6)),

    preset("Cathedral",
        dict(onset_threshold=0.10, sample_ms=300, attack_skip_ms=80, blend=0.95,
             grain_size_ms=160, grain_xfade_ms=80, base_pitch=0,
             pitch1_semi=12, pitch1_level=0.35, pitch2_semi=7, pitch2_level=0.25,
             detune_cents=35, chorus_rate=0.8, detune_blend=0.6,
             filter_type=0, filter_cutoff=7000, filter_q=0.5,
             env_attack=800, env_decay=400, env_sustain=1.0, env_release=8000,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=1,
        hn=dict(dry_level=0.7, hn_brightness=0.3, hn_damping=0.15,
                hn_even_odd=0.1, hn_noise=0.3, hn_width=0.8)),

    preset("Drone",
        dict(onset_threshold=0.15, sample_ms=360, attack_skip_ms=100, blend=0.95,
             grain_size_ms=180, grain_xfade_ms=70, base_pitch=-12,
             pitch1_semi=-12, pitch1_level=0.5, pitch2_semi=-24, pitch2_level=0.3,
             detune_cents=15, chorus_rate=0.3, detune_blend=0.3,
             filter_type=0, filter_cutoff=1200, filter_q=0.8,
             env_attack=600, env_decay=400, env_sustain=1.0, env_release=6000,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=0,
        hn=dict(dry_level=0.8, hn_brightness=-0.5, hn_damping=0.6,
                hn_even_odd=-0.3, hn_noise=0.2, hn_width=0.4)),

    preset("Glass",
        dict(onset_threshold=0.18, sample_ms=120, attack_skip_ms=30, blend=0.85,
             grain_size_ms=40, grain_xfade_ms=20, base_pitch=0,
             pitch1_semi=12, pitch1_level=0.4, pitch2_semi=24, pitch2_level=0.25,
             detune_cents=6, chorus_rate=2.0, detune_blend=0.2,
             filter_type=2, filter_cutoff=4000, filter_q=2.5,
             env_attack=50, env_decay=200, env_sustain=0.9, env_release=1500,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=1,
        hn=dict(dry_level=0.9, hn_brightness=0.8, hn_damping=0.05,
                hn_even_odd=0.6, hn_noise=0.1, hn_width=0.6)),

    preset("Sub Bass",
        dict(onset_threshold=0.20, sample_ms=150, attack_skip_ms=30, blend=0.90,
             grain_size_ms=130, grain_xfade_ms=50, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.6, pitch2_semi=-24, pitch2_level=0.0,
             detune_cents=4, chorus_rate=0.3, detune_blend=0.1,
             filter_type=0, filter_cutoff=800, filter_q=0.9,
             env_attack=30, env_decay=150, env_sustain=1.0, env_release=800,
             detune_enable=0, pitch1_enable=1, pitch2_enable=0),
        pitch_mode=0,
        hn=dict(dry_level=1.0, hn_brightness=-0.6, hn_damping=0.5,
                hn_even_odd=-0.4, hn_noise=0.1, hn_width=0.1)),

    preset("Octaver",
        dict(onset_threshold=0.22, sample_ms=100, attack_skip_ms=20, blend=0.70,
             grain_size_ms=90, grain_xfade_ms=40, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.5, pitch2_semi=12, pitch2_level=0.3,
             detune_cents=5, chorus_rate=0.5, detune_blend=0.0,
             filter_type=0, filter_cutoff=8000, filter_q=0.5,
             env_attack=5, env_decay=100, env_sustain=1.0, env_release=400,
             detune_enable=0, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=0,
        hn=dict(dry_level=1.2, hn_brightness=0.0, hn_damping=0.2,
                hn_even_odd=0.0, hn_noise=0.15, hn_width=0.2)),

    preset("Frozen Strings",
        dict(onset_threshold=0.12, sample_ms=240, attack_skip_ms=90, blend=0.90,
             grain_size_ms=150, grain_xfade_ms=70, base_pitch=0,
             pitch1_semi=-12, pitch1_level=0.3, pitch2_semi=12, pitch2_level=0.3,
             detune_cents=25, chorus_rate=0.7, detune_blend=0.45,
             filter_type=0, filter_cutoff=5000, filter_q=0.6,
             env_attack=1200, env_decay=400, env_sustain=1.0, env_release=3500,
             detune_enable=1, pitch1_enable=1, pitch2_enable=1),
        pitch_mode=0,
        hn=dict(dry_level=0.85, hn_brightness=0.2, hn_damping=0.25,
                hn_even_odd=-0.1, hn_noise=0.35, hn_width=0.7)),
]

# Order base symbols are emitted in the .ttl (alphabetical, matching the
# existing hand-written presets) and the extra HN symbols (appended).
BASE_ORDER = sorted([
    "attack_skip_ms", "base_pitch", "blend", "chorus_rate", "detune_blend",
    "detune_cents", "detune_enable", "dry_level", "env_attack", "env_decay",
    "env_release", "env_sustain", "filter_cutoff", "filter_q", "filter_type",
    "grain_size_ms", "grain_xfade_ms", "onset_threshold", "pitch1_enable",
    "pitch1_level", "pitch1_semi", "pitch2_enable", "pitch2_level",
    "pitch2_semi", "sample_ms",
])
HN_ORDER = ["hn_brightness", "hn_damping", "hn_even_odd", "hn_noise", "hn_width"]

TTL_PREFIX = """\
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""


def filename(name):
    return name.replace(" ", "_") + ".ttl"


def ttl_value(v):
    # LV2 pset:value as a float literal with an explicit decimal point.
    return f"{float(v):.1f}" if float(v) == int(v) else repr(float(v))


def cpp_value(v):
    s = f"{float(v):.1f}" if float(v) == int(v) else repr(float(v))
    return s + "f"


def write_preset_ttl(path, applies_to, name, symbols, values):
    rows = []
    for sym in symbols:
        rows.append(f'\t\tlv2:symbol "{sym}" ;\n\t\tpset:value {ttl_value(values[sym])}')
    body = "\n\t] , [\n".join(rows)
    text = (
        TTL_PREFIX
        + f'\n<{filename(name)}>\n'
        + "\ta pset:Preset ;\n"
        + f"\tlv2:appliesTo <{applies_to}> ;\n"
        + f'\trdfs:label "{name}" ;\n'
        + "\tlv2:port [\n"
        + body
        + "\n\t] .\n"
    )
    with open(path, "w") as f:
        f.write(text)


def write_manifest(path, plugin_uri, binary, main_ttl):
    out = [
        "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .",
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .",
        "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
        f"<{plugin_uri}>",
        "    a lv2:Plugin ;",
        f"    lv2:binary <{binary}> ;",
        f"    rdfs:seeAlso <{main_ttl}> .",
        "",
    ]
    for p in PRESETS:
        fn = filename(p["name"])
        out += [
            f"<{fn}>",
            f"    lv2:appliesTo <{plugin_uri}> ;",
            "    a pset:Preset ;",
            f"    rdfs:seeAlso <{fn}> .",
            "",
        ]
    out += [f"<{plugin_uri}> rdfs:seeAlso <modgui.ttl> .", ""]
    with open(path, "w") as f:
        f.write("\n".join(out))


def write_cpp_header(path):
    L = []
    L.append("// Generated by tools/gen_presets.py — do not edit by hand.")
    L.append("//")
    L.append("// Unified Megalo / MegaloHN factory presets. The same name/value")
    L.append("// pairs drive the LV2 .ttl presets, so a program sounds identical")
    L.append("// across MOD (LV2), a DAW (VST3/AU) and the standalone app.")
    L.append("#pragma once")
    L.append("")
    L.append("namespace megalo {")
    L.append("")
    L.append("struct PresetParam { const char* symbol; float value; };")
    L.append("struct Preset { const char* name; const PresetParam* params; int numParams; };")
    L.append("")
    for i, p in enumerate(PRESETS):
        L.append(f"// {p['name']}")
        L.append(f"static const PresetParam kPreset{i}[] = {{")
        for sym in BASE_ORDER:
            L.append(f'    {{ "{sym}", {cpp_value(p["base"][sym])} }},')
        L.append(f'    {{ "pitch_mode", {cpp_value(p["pitch_mode"])} }},')
        L.append("#ifdef MEGALO_HN_SYNTH")
        for sym in HN_ORDER:
            L.append(f'    {{ "{sym}", {cpp_value(p["hn"][sym])} }},')
        L.append("#endif")
        L.append("};")
        L.append("")
    L.append("static const Preset kPresets[] = {")
    for i, p in enumerate(PRESETS):
        L.append(f'    {{ "{p["name"]}", kPreset{i}, (int) (sizeof(kPreset{i}) / sizeof(kPreset{i}[0])) }},')
    L.append("};")
    L.append("")
    L.append(f"static constexpr int kNumPresets = {len(PRESETS)};")
    L.append("")
    L.append("} // namespace megalo")
    L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))


def remove_stale_presets(directory):
    """Delete any .ttl preset no longer in PRESETS (keeps the dir tidy)."""
    keep = {filename(p["name"]) for p in PRESETS}
    keep |= {"manifest.ttl", "megalo.ttl", "megaloHN.ttl", "modgui.ttl"}
    for fn in os.listdir(directory):
        if fn.endswith(".ttl") and fn not in keep:
            os.remove(os.path.join(directory, fn))


def main():
    megalo_dir = os.path.join(REPO, "megalo.lv2")
    hn_dir = os.path.join(REPO, "megaloHN.lv2")

    remove_stale_presets(megalo_dir)
    remove_stale_presets(hn_dir)

    for p in PRESETS:
        write_preset_ttl(os.path.join(megalo_dir, filename(p["name"])),
                         MEGALO_URI, p["name"], BASE_ORDER, p["base"])
        hn_vals = dict(p["base"]); hn_vals.update(p["hn"])
        write_preset_ttl(os.path.join(hn_dir, filename(p["name"])),
                         HN_URI, p["name"], BASE_ORDER + HN_ORDER, hn_vals)

    write_manifest(os.path.join(megalo_dir, "manifest.ttl"),
                   MEGALO_URI, "megalo.so", "megalo.ttl")
    write_manifest(os.path.join(hn_dir, "manifest.ttl"),
                   HN_URI, "megaloHN.so", "megaloHN.ttl")

    write_cpp_header(os.path.join(REPO, "juce", "megalo_presets.h"))

    print(f"Generated {len(PRESETS)} presets:")
    for p in PRESETS:
        print("  -", p["name"])


if __name__ == "__main__":
    main()
