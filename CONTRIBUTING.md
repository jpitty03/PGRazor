# Contributing

For the React showcase and its independent GitHub Pages workflow, see [website development](website/README.md). Website pushes publish the site, not an APK release.

## Pull-request expectations

Keep each PR focused on **one feature, one bug fix, or one coherent maintenance task**. Include the tests and documentation needed for that change. Split unrelated features, cleanup, dependency updates, and broad formatting into separate PRs.

Before requesting review:

- Describe the problem and resulting behavior. For a fix, include reproduction steps and the expected result; link a relevant issue when one exists.
- Explain meaningful implementation choices, compatibility changes, and known limitations. Include screenshots or a short recording for visible UI changes, with private information removed.
- Add a concise entry under **Unreleased** in [CHANGELOG.md](CHANGELOG.md) for user-facing app changes. Follow the [release-note conventions](docs/releases/README.md); omit website-only changes and test/build details. Documentation-only or internal tooling changes do not need a changelog entry unless they affect users.
- Update [README.md](README.md) and relevant usage guides in the same PR when adding, changing, or removing a feature. Update website feature descriptions/guides when affected, and distinguish unreleased behavior from the published app. If documentation does not need changing, say why in the PR.
- Add or update meaningful regression tests for behavior changes, including relevant error and cancellation paths. Documentation-only and minor cosmetic changes do not require artificial tests. Explain any test coverage that is unavailable.
- Run the relevant checks below and report exact commands and results. Separate automated results, compilation, and actual device observations; list anything not tested and why.
- Review the final diff for unrelated edits, generated files, private data, and obsolete code introduced by the change.

Use the PR template to provide this information. Passing CI does not replace review or prove in-game functionality. Maintainers handle version bumps and releases; do not bundle publishing or release-tag changes into a feature PR unless requested.

## Validation

Run `python -B tools/check.py` for code changes; Android UI changes also require `python -B pgrazor/build.py`. Native behavior changes need relevant native fixtures and, when available, verification on Android and MuMu. Report the device, game version, and scenarios tested. If a device or live scenario is unavailable, record that limitation explicitly rather than claiming it works.

For website changes, follow [website development](website/README.md), including its build and applicable tests. For documentation-only changes, check links, commands, and consistency with the implementation.

## Repository and runtime rules

Do not submit game APKs/assets, signing keys, private logs, account details, or coordinates. The source artifact builder fails if generated/private files are Git-visible. Keep third-party provenance and license notices intact.

Native changes must validate metadata and preserve foreground/map leases, GC ownership, cancellation, and error handling. Destructive inventory automation must keep uncertain data and honor selected protection rules. Never use an automated test to spend balls, transfer live Pokémon, or change a user's inventory.

Use PGRazor for display names and `dev.pgrazor` for Java packages. Preserve the public `Control` bridge API and existing saved-setting keys when moving code into smaller classes. Changes to the supported game version need updated contracts and explicit device testing.

APK publishing is a maintainer operation, not a pull-request workflow. See [BUILDING.md](docs/BUILDING.md).

## Adding or separating a feature

Keep `README.md` up to date with every feature addition, change, or removal in the same change. Update the feature table and relevant usage instructions, and distinguish unreleased behavior from published releases.

- Start with the [architecture map](docs/ARCHITECTURE.md) and [maintainability guide](docs/MAINTAINABILITY.md). Keep policy, UI, and native contracts separate where existing boundaries allow it.
- Give each runtime module explicit dependencies and ownership of its timers, hooks, and retained objects. Construction must not initialize game classes; installation belongs on the required thread.
- Production gameplay runs in compiled native/Java code. Retained research scripts use `pgrazor/runtime/bundles.json`; do not reintroduce them as production runtime assets.
- Name Python tests `test_*.py` and Node tests `*-test.cjs` so `tools/check.py` discovers them. Java tests need a Python/build runner; document that mapping in the architecture page. Do not add standalone Java tests that CI never runs.
- Prefer tests through a module API. Before extracting code covered by source-slicing tests, replace those slices with direct module fixtures. Avoid mass reformatting alongside behavioral changes.
- Report compilation, fixture checks, and device observations separately. Passing mocked tests does not validate game offsets or Android UI behavior.

For game-version compatibility work, follow [GAME_UPDATES.md](docs/GAME_UPDATES.md). Update the selected version profile and regenerate covered headers instead of editing generated headers directly. Review remaining hand-maintained bindings too; a signature match alone does not establish compatibility.

CI sets `PGRAZOR_BUILD_TOOLS=36.0.0` and `PGRAZOR_ANDROID_PLATFORM=android-36`. Set those locally when reproducing CI; local defaults currently select the installed 36.1 SDK. Keep generated outputs ignored, and use `pgrazor/cleanup.ps1` after packaging with deployed/rollback retention pins preserved.
