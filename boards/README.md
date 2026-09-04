# Board profiles

One YAML per **physical board**, keyed by eFuse MAC (colons stripped). Two
boards of the same model get two files — they are different objects with
different factory firmware.

Generate or refresh:

```bash
python3 tools/esp32ident.py --save
```

An existing profile is copied to `.bak` before being overwritten, so hand-added
research is never silently lost — but check the diff.

Schema and provenance rules: `.claude/skills/esp32-workbench/references/board-profile.md`.

The short version: every fact is `{value, provenance, source?, note?}`, and a
value with no provenance is a schema violation. `probed` came off the silicon;
`unverified` is a guess and must never be wired to power.
