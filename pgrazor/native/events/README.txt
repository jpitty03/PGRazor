Halloween box collector
=======================
Quick Menu > Events > Halloween 2026 exposes one default-off setting:
Open boxes and skip encounters.

The compiled collector opens visible TAPPABLE_TYPE_BOX objects through the
game's existing processing path. It collects rewards and suppresses only its
own resulting Pokemon encounter screens; it never catches those Pokemon.
Manual interactions remain forwarded. This is not candy-only filtering.

Integration uses the Unity owner thread, bounded rooted ownership, one pending
action, deduplication/backoff, fresh settings/location leases, and asynchronous
encounter attribution that survives disabling while a request completes.
Transport frame version4 carries the collector gate at boolean39. Retired
scanner/candy preferences cannot enable this feature.

The experimental normal-encounter scanner and diagnostic sources/build flags
were removed. A normal encounter response containing Pokemon data did not
predict the box's actual reward: a matched LivePokeMap rare-candy sample returned
species931 from the normal encounter request and later awarded1RareCandy.
No pre-opening reward classifier or map-service dependency is shipped.

Verification: actual collector native fixtures cover callback ownership,
manual forwarding, delayed/OFF results, exceptions, bounds and deduplication.
Transport and runtime-flow fixtures execute on MuMu; ARM64 fixture compiles.
Full repository checks cover Java preferences, transport and existing features.
Earlier live open-all verification observed candy, rare candy and skipped
encounters. Physical Android has not been retested for this final cleanup.

Final cleanup APK MuMu smoke test: one box completed, one encounter skipped,
zero failures; map remained visible. No new candy reward in this smoke test.
