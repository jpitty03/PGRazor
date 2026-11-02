# Unreleased catching, inventory and automation improvements

These features are optional controls on top of the existing PGRazor runtime.
This branch follows the responsive-overlay change in PR #1.

## Feeds and interface

PoGoCoordinates uses its own authenticated provider session. Choose the feed mode,
country and filters in Feeds; presets and refresh controls reuse those selections.
Provider authorization and subscription requirements still apply.

The interface adds themed VGP/Gym controls and rounded status cards for VGP, Gym,
Emotion Engine and automation. Map overlay opacity applies across overlays.
Hide VGP status text and Hide Gym status text hide their labels while retaining the
icons. Inventory IV/level is under Catch & summary.

## Location and catching

Background location and walking preserve the existing synthetic-location controls.
This does not bypass foreground requirements of gameplay actions or automatically
resume an in-game Route. Existing user stop/cancellation controls remain authoritative.

Silver Pinap is available through berry selection. Random throws can be enabled
independently of the saved fixed throw grade. Buddy Emotion Engine supports optional
Poffin use alongside berry reserves and priority selection.

PvP ranks are calculated natively for encounters and inventory. IV and best PvP
information under inventory names also updates the first visible rows.

## Automation

- Full Auto Rocket opens an eligible invaded stop, advances the supported battle
  flow, uses the selected team and attempts a rescue throw. Unsupported/uncertain
  contracts pause the relevant flow. The game still decides action results.
- Skip hatch handles normal and Daily Adventure Eggs and confirms the final summary
  closes with bounded retries. Keep shiny & 100% retains matching summaries.
- Revive/heal finishes healing revived Pokemon to full. The optional half-HP mode
  triggers strictly below half HP, including odd maximum HP; unknown state prevents
  action. Item selection follows the configured availability rules.
- Optional manual batch transfer allows selected special Pokemon through the normal
  confirmation flow. Favorites, eggs and Gym defenders remain protected.
- Inventory/PvP work uses bounded scans and caches; leases and cancellation revoke
  outstanding work.

## Runtime and validation

The Java and native transport schema is version 17 with 15 strings and 62 actions.
The appended Rocket swipe, hatch update and manual-transfer status actions are
59, 60 and 61 respectively. Java and native components must be built together.
Action authorization, expiry and account/foreground state are validated at the
boundary and when queued gestures execute.

Automated checks include native policy fixtures, Java persistence/gating tests,
typed transport dispatch and viewport geometry. Report those checks separately
from phone observations. The contributor reports Android testing on HONOR WIN
with Android 17.
