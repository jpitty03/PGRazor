# Updating the supported game version

The version-specific native binding values live in
`pgrazor/native/schema/game/0.435.1.json`; `current.json` selects the profile.
Update that profile, then regenerate. Do not hand-edit generated headers or the
derived `routes.json` and `gifts.json` files.

The profile contains input identity, method RVAs, field offsets, obfuscated names,
and checked code fingerprints. Entries are grouped by their consuming module and
include a purpose. Shared fingerprints and GC implementation offsets have aliases
or derived views so their consumers can share one definition. Aliases mean the
same binding, not merely two methods that happen to have identical bytes today.

Runtime behavior, stable metadata lookup signatures, ABI declarations, instruction
decoders and feature policies stay in C++/Java. A changed signature or behavior can
still require implementation changes. Updating addresses alone does **not** prove
compatibility. These tools do not publish, update secrets, install or run gameplay.

## Repeatable commands

Run from the repository root with Python 3.12:

```powershell
python -B tools/game_update.py verify-input path/to/game.apkm
python -B tools/game_update.py generate
python -B tools/game_update.py generate --check
python -B tools/game_update.py audit
python -B tools/check.py
```

Input verification compares the complete file hash with the approved profile.
It does not infer a version from the filename or certify gameplay behavior. Keep
the original input unpublished in the existing draft-release asset workflow.

Generation validates the full profile before writing six outputs: the shared
`game_bindings.h`, the Quick Catch wait header, and the Routes/gifts JSON and
headers. Native builds and device-free CI reject stale generated files. The
address audit catches inline numeric candidates in the current ARM64 address
range; it is a regression guard, not a complete C++ parser or proof that every
future binding is covered.

## Editing bindings

An entry uses a stable key, a kind, a value and a purpose. For example:

```json
"player_id_rva": {
  "kind": "rva",
  "value": "0x92cb930ULL",
  "purpose": "PlayerService.get_PlayerId method address"
}
```

Numeric strings preserve C/C++ literal widths and permit readable hexadecimal
offsets. Fingerprints use byte/word arrays or hex strings. `alias` entries refer
to `module.key`; `view` entries derive a bounded byte prefix or little-endian word
array from a shared byte fingerprint. Cycles, missing references, invalid values
and conflicting generated names are rejected.

Keep keys stable when the game's obfuscated names change. Change the `value`, not
the key used by feature code. Search for the generated `PGR_GAME_...` symbol to
find its consumers. When adding a binding, add a descriptive entry and reference
its generated symbol in source; keep timers, distances and user settings out of
the profile. Do not mechanically alias unrelated methods just because their
current addresses or prefixes match.

Compare old/new extracted signature snapshots:

```powershell
python -B tools/game_update.py compare --old old-signatures.json --new new-signatures.json --output .artifacts/game-update/report.json
```

The existing extraction format has a `methods` array. Each method needs `module`,
`owner`, `name`, `return`, `params` (ordered type names), `argc`, `flags`, and `rva`.
Metadata extraction is still a separate research step; this command does not
extract signatures directly from an APK. Reports include snapshot hashes.

Only exact module/owner/name plus complete signature matches become candidates.
Duplicates, unknown signatures, missing names and signature changes require
review. A matching signature is not proof that a method's behavior or machine code
is unchanged. Reports never authorize replacement RVAs automatically, and their
exit status indicates report generation, not compatibility approval.

## New-version checklist

1. Preserve the original input and tested rollback. Inspect the new package's
   manifest, signing certificate, native libraries and metadata.
2. Extract old/new signatures and produce the comparison report. Review renamed
   types, ABI changes, field layouts, exports, interior patch sites and fingerprints.
3. Copy the previous profile to a new version file. Update verified bindings,
   input/library/metadata hashes and version identity; select it in `current.json`.
   Review changed method signatures and behavior in the consuming source. Stable
   metadata names are resolved and validated at runtime; this does not eliminate
   the need to review changes to those contracts.
4. Generate headers and run checks. Update golden parity tests only after reviewing
   the intended binding changes. Build both native ABIs and run relevant fixtures.
5. Package and verify on MuMu, then test affected gameplay. Record unavailable
   scenarios rather than treating metadata matches as successful gameplay tests.
6. Update the private input asset/secret using `BUILDING.md`, retain deployed and
   rollback builds, and use the normal release workflow when authorized.

Next work: replace the one-off metadata extractor and add a preparation command that ties input
inspection, extraction and targeted fixture selection together.
