# Native binding sources

Edit the selected version profile in `game/`, then run from the repository root:

```powershell
python -B tools/game_update.py generate
python -B tools/game_update.py generate --check
python -B tools/game_update.py audit
```

`game/current.json` selects the profile. `game/catch_wait.h.in` contains the stable
Quick Catch declaration/policy template. The remaining feature source references
literal symbols emitted into `../game_bindings.h`; it owns runtime logic.

`routes.json` and `gifts.json` are generated compatibility inputs for their existing
validated schema generators, not independent sources of version values. Keep them
and their generated headers committed so clean checkouts build without extraction
tools or access to original game files.

See [the update guide](../../../docs/GAME_UPDATES.md) for profile entries, validation
limits and the new-version checklist.
