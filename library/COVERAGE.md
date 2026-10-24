# Version 0.3.1 coverage

79 distinct types; 402 presets; 6318 referenced files; 5898 distinct audio hashes.

Baseline: 76 types and 399 presets, preserved unchanged. Additions:

| Added type | Presets | Samples / individually tested zones | License |
|---|---:|---:|---|
| Six-string guitar-banjo | 1 | 58 / 58 | CC0-1.0 |
| Steel-string acoustic guitar | 1 | 59 / 59 | GPL-3.0-or-later WITH FreePats composition exception |
| Tabla | 1 | 650 / 650 | CC-BY-4.0 |

All new sources fully decoded; every new zone individually rendered finite/nonzero. Full key/velocity/RR selection and silent library UI checks passed. No listening, physical MIDI or hardware-latency certification.

Named-type catalog counting keeps steel-string acoustic guitar distinct from the baseline nylon-string acoustic guitar: 79 named types if all three additions pass, or 78 if both acoustic guitars are collapsed into one type. Six-string guitar-banjo adds one banjo-type entry; a future five-string banjo is a labelled variant, not automatically a second counted type. Tabla is one type with 13 stroke labels, not 13 types.

Ganjo MIDI 38-72; steel-string guitar MIDI 39-85; Tabla pads 60-72 at original speed, not chromatic pitch claims. Original notices and changes retained.

Remaining gaps include five-string banjo, sitar, koto, shamisen, erhu and djembe. These are not substituted or counted.
