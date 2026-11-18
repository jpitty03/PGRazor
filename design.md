# PGRazor interface design

PGRazor is a utility that runs on top of a game. It is opened for seconds at a time, often while the
player is walking, often one-handed, and always over a live map. The interface should feel like a
good field instrument: legible in sunlight, calm at night, quick to scan, and completely out of the
way when it is not needed.

This document is the reference for that interface. It covers the design intent, the shared token
system, every component, the settings search, performance rules, responsive behavior, and the code
architecture that keeps all surfaces consistent.

---

## 1. Concept: the field instrument

The old interface borrowed the game's visual language: lime-to-mint gradients, glossy cards, a
sharp italic wordmark, spaced uppercase headings. It looked like a game screen, but PGRazor is not a
game screen. It is a tool, and it competed with the map behind it.

The redesign takes the opposite direction.

- **Quiet surfaces.** One page color, one card color, one hairline. No gradients, glow, bevels or
  drop shadows. The map and the game stay the visually interesting layers; PGRazor is the
  instrument panel on top.
- **One accent.** A single mint tone marks state: a switch that is on, the current value of a
  setting, a live status dot, the primary action. Nothing else is colored.
- **Telemetry, not decoration.** Numbers, coordinates, counters and status lines use the platform
  monospace face at a small size. They read like a readout, not like marketing copy.
- **A signature detail.** The status dot — in the settings header, on the floating launcher and on
  every status row — is the one repeated flourish. It is also functional: filled means active,
  faint means idle, amber means waiting, red means a problem.
- **Plain language.** Sentence case everywhere. No uppercase labels, no letter-spaced titles, no
  emoji icons. Short labels, short values.

## 2. Principles

1. **The map is the product.** Every panel is a temporary layer. Nothing may animate for its own
   sake, and opening a panel must not drop frames on the game behind it.
2. **State is visible without opening anything.** The launcher dot, overlay pills, cooldown timer
   and the home telemetry strip answer "is it running?" before the user taps.
3. **One system, no exceptions.** Settings, quick menu, dialogs, overlays, feeds and the map chrome
   draw from the same tokens. A new screen that invents a color is a bug.
4. **Density with air.** Rows are 52 dp tall with 14 dp side padding inside cards. Dense enough for a
   phone, never cramped, never a wall of text.
5. **Reversible, not destructive.** Destructive actions are plainly red and separated from normal
   rows. Backup, account and settings contracts are untouched by visual work.
6. **Cheap by construction.** Static views, guarded setters, no per-tick allocations, no third-party
   UI library. Performance is a design constraint, not a later optimization pass.

## 3. Color

All values live in `SettingsUi` and are selected by `Control.darkTheme()`. There are no other color
sources; `MapTheme` and `FeedTheme` delegate to these.

| Token | Dark | Light | Used for |
| --- | --- | --- | --- |
| `bg` | `#0D131A` | `#F3F6F9` | Page backdrop behind cards |
| `surface` | `#121A23` | `#FFFFFF` | Cards, dialogs, menus, pills |
| `raised` | `#19222D` | `#F7FAFC` | Fields, flat rows, second surface step |
| `border` | `#25303C` | `#DCE4EC` | Card, field and pill hairlines |
| `ink` | `#E9EFF5` | `#16212C` | Primary text |
| `muted` | `#9AABBC` | `#5C6D7C` | Secondary text, captions, section labels |
| `faint` | `#71828F` | `#7E8E9C` | Micro labels, chevrons, disabled text |
| `accent` | `#45E1BA` | `#00745C` | On-state, current values, live dots |
| `accentFill` | `#0B6E58` | `#00745C` | Primary button fill (white text) |
| `accentInk` | `#06231D` | `#FFFFFF` | Text on an accent fill |
| `accentSoft` | accent @ 12% | accent @ 12% | Search jump flash, selected tints |
| `success` | `#4ED8A0` | `#0F7B53` | Passing probe, kept transfers |
| `warn` | `#F2B75E` | `#9A5B00` | Cooldown running, waiting states |
| `danger` | `#F07A72` | `#B3261E` | Errors, destructive actions |

Rules:

- Never place `accent` text on `accentSoft` without checking contrast; values use `accent`, fills use
  `accentFill` with white (dark theme uses a deep accent so white stays readable).
- Status color is always paired with text (`PASS`, `Waiting`, `Mismatch`), never color alone.
- Map tiles keep their own raster colors. Only chrome around them is themed.
- Ripples use `accent` at 20% (`rippleColor`), so touch feedback is visible but quiet.
- The status and navigation bars belong to the game's window, so `SettingsUi.systemBars` flips their
  glyphs with the palette (dark glyphs in light theme) whenever the panel is visible, and hands them
  back to the game's light-on-dark default when it closes.

## 4. Typography

Only two faces ship, both platform-provided: the system sans (regular and `sans-serif-medium`) and
the system monospace. No font asset is loaded, so text rendering has zero startup cost.

| Role | Size | Face | Color |
| --- | --- | --- | --- |
| Screen title | 20 sp | medium | `ink` |
| Section label (page) | 13 sp | medium | `muted` |
| Group label (home) | 12 sp | medium | `faint` |
| Row label, button, body | 14 sp | regular | `ink` |
| Caption, helper, status | 12 sp | regular | `muted` |
| Micro label, telemetry key | 11 sp | mono | `faint` |
| Telemetry value | 20 sp | mono | `ink` |

- Line spacing is 1.15 for labels and 1.35 for paragraphs.
- `RowButton` renders `"Label: value"` with the value in `accent` + medium, so a page reads as a
  list of current states without a second view per row.
- Titles are sentence case, centered only in dialogs. No letter-spacing above 0.01 em anywhere.

## 5. Spacing, shape and elevation

- Spacing scale: 4 / 8 / 12 / 16 / 20 / 24 dp (`SPACE_1`–`SPACE_6`).
- Radii: control 10 dp, field 12 dp, card 14 dp, sheet 20 dp.
- Card padding: 16 dp horizontal, 14–16 dp vertical. Group cards use built-in hairline dividers
  instead of padding between rows.
- Elevation is intentionally flat. Cards are surface steps and hairlines only; floating surfaces
  (the quick menu and dialogs) keep the small platform elevation they need to separate from the
  game behind them.
- One hairline (`SettingsUi.hairline`) is a 1 dp `GradientDrawable` used as a `LinearLayout` divider,
  so a group of ten settings costs no extra views.

## 6. Layout

```
┌──────────────────────────────────────────────┐
│  PGRazor · 0 / 24 h     theme  v0.1.8   ×    │  header
├──────────────────────────────────────────────┤
│  ⌕  Search settings                     ×    │  home search field
├──────────────────────────────────────────────┤
│  ┌────────────────────────────────────────┐  │
│  │ ● Starting                    0.0 km/h │  │  telemetry card
│  │ 37.774900, -122.419400                 │  │
│  └────────────────────────────────────────┘  │
│  Play                                        │  group label
│  ┌────────────────────────────────────────┐  │
│  │ [icon] Location & Map            ›     │  │
│  │        Spoofing · Joystick              │  │  group card
│  │ [icon] Catching                  ›     │  │
│  │        Curveball                        │  │
│  └────────────────────────────────────────┘  │
│  Automation                                  │
│  ┌────────────────────────────────────────┐  │
│  │ [icon] Automation                ›     │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

- The settings body is a single `ScrollView`. The header sits above it and never scrolls away.
  The search field is fixed above the home list; a category page trades it for a back action and
  the category name, so the header stays compact and the page gains vertical room.
- Home groups the ten category pages into three labeled cards: **Play**, **Automation**, **App**.
  The grouping is presentation only; page indices and the persisted contract are unchanged.
- Category pages keep their existing sections and rows, restyled. Collapsible sections remain
  collapsible; the only collapsed-by-default section is **Diagnostics** on Accounts & Backup.
- Dialogs are rounded surface cards on a dimmed backdrop. Full-screen dialogs (accounts, logs,
  backups) reuse the same header, title and card treatment as the settings pages.

## 7. Components

| Component | Behavior |
| --- | --- |
| **Header** | PGRazor title with the encounter counter appended when enabled, theme action, version in mono, close. A category page adds a back arrow and replaces the title with the category name. |
| **Search field** | 48 dp tall, raised fill, magnifier glyph, single-line input, clear button that appears only with text. Fixed at the top of the home screen; category pages show back + title instead. |
| **Telemetry card** | Status dot, one-line state, synthetic coordinate readout in mono. Refreshed on the existing 100 ms tick with change-guarded text setters. |
| **Group card** | `LinearLayout` with a hairline divider between flat `RowButton`s. Whole card is one surface. |
| **Row button** | Flat inside a card, raised + bordered outside. Supports `Label: value` accent formatting, 2-line ellipsis, 52 dp minimum. |
| **Switch row** | Label left, switch right, caption below. Switch has no text of its own; its track and thumb are tinted from `accent`. |
| **Action row** | Same as a row button but performs navigation or an action; trailing chevron drawn at 16 dp. |
| **Pill** | Small floating status readout (cooldown, VGP, berry, counters): surface + border + mono text, 10–12 dp radius. |
| **Quick menu** | Vertical card of icon + label rows, 48 dp each, same surfaces and radius as settings. |
| **Launcher** | 50 dp disc with the PGRazor mark and a status dot. Drag to reposition, tap to open. |
| **Joystick** | Translucent instrument disc with a center dot and crosshair; 78% opacity idle, full opacity while held. |
| **Dialog** | Surface card, 20 sp title, 14 sp body, secondary pill + primary accent fill actions. |
| **Empty / no-results** | Card with a short explanation and a hint about what to try; never a bare blank page. |
| **Toast / inline error** | Plain text in `danger` under the relevant field; no colored banners. |

## 8. Settings search

Search is the fastest path to any setting and is the only new interaction added by the redesign.

**Location.** A permanent field pinned above the scrolling body on the Settings home. It is visible as
soon as the panel opens and stays fixed while the category list scrolls. Category pages trade it for a
back action and the page title, so the panel never spends more vertical space than it must; clearing or
leaving a page returns to the same field.

**Matching.** `SettingsSearch` is a pure-Java catalogue (no Android imports, host-JVM testable):

- Each indexed setting contributes a name, description, category, group and synonyms.
- Text is normalized once: trimmed, lowercased, accents folded (`Pokémon` → `pokemon`),
  punctuation collapsed. Queries normalize the same way.
- Matching is substring-based and requires **every** query token to appear somewhere in the entry's
  haystack. `fake gps`, `mock location`, `dark`, `light mode`, `performance` and `pokemon` all find
  the right rows.
- Results are ranked: exact name, name prefix, name substring, description, then any other indexed
  text (synonyms, group, category). Ties keep the catalogue order, which follows the page order.
- Names are cleaned for display (`Throw rating: Great` → `Throw rating`) so results read as settings,
  not as current values.

**Interaction.**

- Filtering runs on every keystroke. The catalogue is built once by scanning each page tree after
  construction and tagging group cards; there is no view walk per keystroke.
- While a query is active the home/pages are hidden and a results list is shown. Each result shows
  the setting name, its category and its group.
- The clear button appears with text and resets to the previous view.
- An empty result set shows an explicit no-results card: a short explanation and examples such as
  "auto ball", "cooldown" or "IV", never a blank page.
- Tapping a result opens the owning page, expands any collapsed ancestor section, scrolls the row
  to the top of the body and flashes a 900 ms `accentSoft` wash on the row. No animation, no
  delayed layout, no second tap.
- Search state is presentation only. It never writes a setting or changes a page index.

## 9. Performance

The interface is built for low-end devices and for running over a live game.

- **No UI library.** Everything is platform `View`/`LinearLayout`/`ScrollView` plus small custom
  `View`s for glyphs. No RecyclerView, no ConstraintLayout, no animation library, no image loading.
- **Static construction.** Pages are built once, in Java, and shown/hidden. Navigating back does not
  rebuild a page.
- **Guarded refresh.** The panel refreshes on a 100 ms tick while visible. Every text update goes
  through `set(TextView, String)` or `status(...)`, which compare first; alpha, enabled and color
  changes are likewise compared before applying.
- **No per-tick allocation.** Drawables, colors and formatted strings that depend only on theme or
  state are created when the theme changes, not on each tick. Overlay status pills set their
  background only in `applyTheme()`.
- **No animation.** The only time-based visual is the search jump flash (a `setForeground` change
  and a delayed clear), which costs one drawable.
- **Cheap dividers.** Group cards use `LinearLayout` divider drawables rather than per-row spacer
  views.
- **Monospace and system sans** are platform faces; there is no font parsing at startup.
- **Drawings are simple.** Glyphs are 2–5 lines of `Canvas` drawing on a cached `Paint`, sized in dp.

## 10. Responsive rules

- The content column is capped at **620 dp** and centered on tablets and foldables
  (`updateContentWidth()`); on phones it fills the width minus 24 dp side padding.
- Rows wrap to two lines with ellipsis rather than truncating mid-value; switches and chevrons keep
  a fixed trailing position.
- System bar and IME insets are applied to the panel and dialogs, so the search field and buttons
  stay reachable with the keyboard open.
- Landscape keeps the same vertical order; the capped column prevents cards from stretching into
  unreadable lines.
- Dialog bodies scroll; action rows stay pinned at the bottom of the dialog.

## 11. Architecture

```
SettingsUi        tokens, type, shapes, RowButton, glyphs, styleTree, tags
SettingsSearch    pure-Java catalogue, normalization, ranking (host-JVM tested)
SettingsPanel     shell (header, search, telemetry), home groups, category pages,
                  search index/results/jump, guarded refresh
Pad               overlays, launcher, joystick, quick menu hosting
QuickMenu         compact floating menu, themed from SettingsUi
FeedTheme         feed/dialog palette — delegates every color and size to SettingsUi
MapTheme          map chrome and nested dialogs — delegates to SettingsUi
PreviewActivity   standalone preview of the control surface
```

- `SettingsUi` is the single source of truth. A theme switch calls `styleTree` once per open dialog
  and `applyTheme` on persistent components; nothing styles per frame.
- `MapTheme` and `FeedTheme` keep their historical method names so thousands of call sites stay
  untouched, but their bodies are now thin delegations. This is how the whole app changed palette
  without a risky mechanical rewrite.
- `SettingsSearch` is deliberately dependency-free so its matching and ranking are covered by a
  plain JVM test (`pgrazor/test/SettingsSearchTest.java`) with no emulator.
- Visual work never touches `Control` keys, `SettingsBackupRules`, export formats or the public
  runtime API.

## 12. Preserved functionality

- Every setting, page, dialog and overlay that existed before the redesign still exists, with the
  same key, default and persistence behavior.
- `Control` and `SettingsBackupRules` contracts are unchanged; backups and restores remain
  compatible across the redesign.
- The 100 ms refresh cadence, search-free pages' behavior, diagnostics, account sessions, backups,
  feeds, map chrome and the quick menu all keep their original actions.
- The dark/light toggle still applies instantly to every open surface through the same delegation
  path.
- Accessibility: rows keep content descriptions where they had them, switches remain announced as
  switches, touch targets stay at least 48 dp, and status is always expressed in text as well as
  color.
