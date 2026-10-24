# World Heritage Engine — Sound Library (backup v1)

Real recorded acoustic instruments for the World Heritage Engine, rescued on 2026-10-24 from the
"World Heritage Instrument Creator 0.3.3" install on Steven's PC
(`%LOCALAPPDATA%\Programs\World Heritage Instrument Creator`). Nothing in the samples was changed.

## What is in this zip

| Folder | What it is | License | Where the audio is |
|---|---|---|---|
| `library/FreePats` | Nylon (Spanish classical) guitar, ukulele, button accordion, bagpipe, jaw harp, hang drum | CC0 1.0 | **in this zip** |
| `library/SteelString` | Steel-string acoustic guitar (FreePats FSS) | GPL-3.0-or-later with the FreePats composition exception (`source/.../gpl.txt`, `readme.txt`) | **in this zip** |
| `library/Tabla` | Tabla strokes dataset, 13 strokes x 50 takes | **CC BY 4.0 — credit required** (see below) | **in this zip** |
| `library/Ganjo` | Six-string guitar-banjo | CC0 1.0 | **in this zip** |
| `library/VSCO-2-CE` | Orchestral: brass, woodwinds, strings, piano, organ, percussion | CC0 1.0 | NOT in this zip — see "Big packs" |
| `library/VCSL` | Recorders, ocarinas, harmonicas, didgeridoo, sax, harpsichords, pianos, mallets, world percussion | CC0 1.0 | NOT in this zip — see "Big packs" |

Every folder keeps its original license, readme and attribution files untouched.
`library/*/presets/*.whic` are the key/velocity/round-robin mappings the Instrument Creator made
(plain text; format `WHIC_PRESET 4`). The `*MANIFEST*.json`, `COVERAGE*.json`, `plan.json` and
`provenance.json` files list every sample with its SHA-256.

`original-app-docs/` holds the Instrument Creator's own README, user guide and notices, for reference.

## Big packs (VSCO 2 CE and VCSL)

Too big to back up here (about 2.6 GB and 2.2 GB of selected WAVs). They are pinned to exact
upstream versions, and every file was checked against the manifests on 2026-10-24:

- VSCO 2 CE — https://github.com/sgossner/VSCO-2-CE at commit `440300901dfe9275fd84e0b7763af1f8443ae62e`
  — 3,177 of 3,177 sample files listed in `library/VSCO-2-CE/provenance.json` matched SHA-256.
- VCSL — https://github.com/sgossner/VCSL at commit `c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e`
  — 2,930 of 2,930 files in `library/VCSL/COLLECTION-MANIFEST.json` matched SHA-256.

To restore: clone those repos at those commits; the presets reference the samples by their upstream paths
(`library/<pack>/source/<upstream path>`).

## Checks done on this backup (2026-10-24)

- Tabla: 650 / 650 WAVs match `Tabla/source/SOURCE-HASHES.json`.
- Steel-string guitar: 59 / 59 match `SteelString/EXPANSION-MANIFEST.json`.
- Ganjo: 58 / 58 match `Ganjo/EXPANSION-MANIFEST.json` (audio taken from github.com/sfzinstruments/ganjo).
- FreePats: 222 / 222 files present on the PC match `FreePats/COVERAGE.json`. 20 manifest entries
  (pack pictures and the accordion's `rel/` release samples) were never in the PC install, so they are not here either.
- `SHA256SUMS.txt` covers every file in this zip.
- Not checked: how any of it sounds. Nobody has listened to these through the World Heritage Engine yet.

## Credits

- **Tabla:** Subodh Deolekar (2020), *Tabla strokes dataset*, Zenodo, DOI 10.5281/zenodo.4327350,
  https://zenodo.org/records/4327350 — CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/).
  Sample bytes unchanged; key/round-robin mapping added. Any product using these must show this credit.
- **VSCO 2 CE:** Versilian Studios / Sam Gossner, Ivy Audio / Simon Dalzell. Please credit and link
  https://versilian-studios.com/vsco-community/. Don't sell the raw samples on their own.
- **VCSL:** Versilian Studios LLC and contributors — https://github.com/sgossner/VCSL
- **FreePats:** https://freepats.zenvoid.org/ — original authors in each pack's README.
- **Steel-string guitar:** Gary Campion / FlameStudios, modified by Roberto for FreePats.
- **Ganjo:** itsclipping — https://github.com/sfzinstruments/ganjo

Credit is not endorsement. None of these people or projects are connected with HEM.
