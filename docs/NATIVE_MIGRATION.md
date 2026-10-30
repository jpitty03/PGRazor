# Native runtime pilot



This branch introduces an opt-in compiled native library. Normal packages still

use the existing runtime. This is an incremental migration, not a Frida-free

release or a guarantee against reverse engineering or runtime detection.



## First boundary



The spawn-radius getter is the first candidate. Existing IL2CPP metadata and

instruction checks still resolve the target before the native hook is installed.

The C++ callback owns original-call chaining, the radius policy, and monotonic

lease expiration. Settings and account profiles keep their existing Java API.



The native library exposes a versioned C function table and `JNI_OnLoad` only.

The host validates the JNI version and policy check before enabling the pilot.

The guest adapter validates the function table separately. This temporarily

retains Frida for initialization, metadata validation, shared observers, and the

controllers listed below as pending migration.



MuMu's Java host loads x86_64; gameplay loads ARM64 inside the existing translated

runtime. No game pointers cross the JSON settings mailbox. ARM64 phones use the

ARM64 library for both roles. Building both ABIs is not proof of translated-hook

compatibility: actual invocation, lease expiration and original chaining need

device verification.



## Feature migration inventory



The native API keeps v1 available for the original spawn pilot. Version 2 adds

independent feature tables so modules can evolve without changing unrelated

settings interfaces. Native hook installation is serialized across modules.



| Component | Current boundary |

| --- | --- |

| Spawn booster | C++ getter hook and lease; MuMu verified |

| Throw grade and curve | C++ policy and hit hook; successful MuMu catch verified; submit observer still shares Gum with berries |

| Adventure Sync dialogs | C++ hooks and pending-dialog ownership; MuMu installation verified, live dialog regression pending |

| Catch details suppression | C++ identity/ownership gate; successful MuMu map return verified; summary/transfer orchestration retained |

| AR+ metadata | C++ capture hook and rooted proto; both ABIs build, native harness passes, MuMu hook installation verified; live request verification pending |

| Always Hit | C++ collision, throw normalization and launch hooks; native harness passes; MuMu off-target throw caught Zigzagoon with native Excellent/curve grading and returned to the map; Android verification pending |

| Catch animation speed | C++ ownership/controller passes native harness and MuMu Shuppet catch (20x and map return); Gum observers retained for shared trace/outcome callbacks; Android pending |

| Direct catch completion | Feature 24 validates caught identity and invokes the supplied dismissal Action in C++; native/adapter/order checks pass. JS summary orchestration and ordinary-details/canvas fallback remain; user confirmed MuMu screen skipping and map return; Android pending |

| Summary completion | Feature36 validates the caught summary, performs one ordinary dismissal and reads the postcondition in C++. Both ABIs and compiled-library lifetime/lease/reentry tests pass. Shared candidate discovery, presentation bridge, canvas and details orchestration remain JavaScript; batch36 user-confirmed on MuMu for Quick Catch and transfer-only dismissal |

| Post-catch details | Feature37 validates and closes details in C++; batch37 user-confirmed on MuMu. Feature38 recovers the post-catch object snapshot and feature39 owns canvas hiding/restoration; batches38/39 user-confirmed on MuMu. Feature40 now reads registry/scene candidates in C++; compiled-library and host checks pass, gameplay pending |

| Encounter and inventory IV display | C++ display hooks, rooted snapshots and restoration; native/adapter harnesses pass; shared Gum encounter observer avoids a second engine at the VGP entry; MuMu encounter and outlined inventory labels verified |

| Map taps | C++ coordinate hooks and bounded queue; native/adapter checks pass; MuMu tap-to-walk verified, capture timestamps preserved for double taps |

| Camera and game location | Feature41 moves continuous camera reads, world-map mask checks and heading calculation to C++; batch42 moves Android provider delivery and permission policy to compiled Java. MuMu camera readings and startup with denied app location permission verified. Shared observers, thread proof and thin Java hooks remain JavaScript |

| Quick spin camera transition | C++ phase ownership and spline hooks; native harness and MuMu 40 ms in/out transitions verified; bag-full spin attempt did not verify item collection |

| Quick spin request/dismissal | C++ hooks and request/result lifecycle; native and adapter tests pass; MuMu spun a stop, received success, closed after 355 ms and returned to the map |

| Transfer rules and custom keeps | C++ keep-policy plus feature 23 request executor. Native owns release calls, identity recheck, pending objects, duplicate prevention and result/timeout bookkeeping; Feature35 now reads IV/category/size facts in C++; shared JS source/context checks and UI record bridge remain. Native reader, adapter and actual policy-coupling checks pass; batch35 user-confirmed on MuMu in all four manual Quick Catch/transfer combinations |

| Research removal | No active production hook found in this source snapshot |

| Incubation | C++ unlimited-incubator orchestration, rooted inventory enumeration and uncertain-request tracking; both ABIs build, 15 native scenarios and adapter checks pass; MuMu issued one 5 km assignment and confirmed it from live inventory |

| Inventory discard | C++ catalog, recycle/result hooks and uncertain-request quarantine; native/adapter harnesses pass; MuMu cold startup and 27-type catalog verified, live discard pending |

| Buddy feeding | C++ service orchestration and result hooks; 14 native scenarios and adapter checks pass; MuMu fed three Razz Berries and confirmed 180 minutes |

| Encounter berries | Native pilot integrated; MuMu startup and user-tested feeding verified |

| Gifts and stickers | Incoming-open and feature 22 bundled sending/sticker attachment user-verified on MuMu. Native transaction and adapter checks pass; shared Gum observers and legacy gift-list UI remain |

| Rocket minigame | C++ hooks, snapshot/restoration and roots; native harness passes and MuMu installation verified; live battle pending |

| Raid, gym and Dynamax attacks | Native controller integrated; 27 compiled-library scenarios pass; MuMu Dynamax, gym and manual switching verified; raid pending; shared Gum observers retained |

| Rocket attacks and shields | Tested native action policy retained; feature 21 adds native object ownership, candidate evaluation and method invocation. Shared Gum observers and minigame gating remain; 27 compiled-library cases and adapter/integration checks pass; MuMu Rocket battle user-verified |

| VGP | Native controller and auxiliary lookup (33/34) now coordinate native modules25/26/28/29/30/31/32: scanning, pending requests, manual exclusion, results, inventory wait and marker timing. JS transports options/UI/shared-observer events and hands records to shared transfer classification/execution. Batch33 user-confirmed on MuMu: catching, auto-transfer, inventory cleanup and spinning. Latest tested VGP rollback; physical Android pending |
| In-game Routes | Native sequence/lease gate (27), current-state and service actions (43), discovery/geometry and rooted cache (44), service capture/root ownership (45). User confirmed full route completion with batch46; batch47 native capture and planner discovery verified on MuMu. Tracking/reward observers and result-screen handling remain to migrate; Java walking orchestration stays compiled Java |

| Account game-side discovery/sign-out | C++ controller and adapter pass isolated tests, including real Dobby original chaining; stable Java command IDs survive script restarts; MuMu current-account identity verified; live switching pending; encrypted Java storage retained |

| Android authentication and translated bootstrap | Batch43 moves browser sign-in request ownership, polling and result completion to compiled Java. Thin authentication/location hooks and translated bootstrap still use Frida; complete replacement remains pending |



Compiling a module does not mark its gameplay behavior verified. The Java UI,

settings, profile storage, maps and feed do not need to be rewritten in C++ merely

to remove the JavaScript hook runtime.



## Build instructions



Install Android NDK `27.3.13750724`, CMake and Ninja. Set `ANDROID_NDK_HOME` to the

NDK directory. The native helper verifies a pinned Dobby source archive and saves

unstripped binaries privately under the ignored build directory.



Set `PGRAZOR_NATIVE_PILOT=1` before the usual `pgrazor/nonroot.py` packaging

command. First-time dependency acquisition additionally requires

`PGRAZOR_NATIVE_DOWNLOAD=1`. `PGRAZOR_NATIVE_SELFTEST=1` explicitly requests the

own-function Dobby test in a diagnostic pilot build. It is not a release default.



The resulting APK contains both runtimes, required dependency licensing, and

stripped native binaries. Existing Frida notices remain. Never package signing

keys, unstripped private debug files, test-account files, or runtime session data.



## Migration gates



### Validation status (2026-10-26)



Both ABIs compile with the pinned Dobby revision. The x86_64 device harness

passed original-call chaining, enabled/disabled policy, invalid input, lease

expiry, idempotent installation and telemetry checks. MuMu's translated ARM64

runtime passed the separate own-function hook self-test and installed the real

spawn-radius hook. Live callbacks returned 50 m with the setting disabled,

80 m enabled, and 50 m after disabling it again.



The translated adapter uses guest `dlopen`/`dlsym`: the Frida module-wrapper

lookup stalled in this environment. A longer entry sequence in the diagnostic

test function resolved a separate self-test stall. These findings do not imply

that every game function can safely be hooked through the translator.



The final pilot also passed cold startup and background/resume on MuMu. Lease

expiry itself was verified by the device harness; the paused game does not provide

continuous live getter samples to prove that transition independently.

The native lease uses `CLOCK_BOOTTIME` so time spent in device suspend counts

toward expiry. Telemetry reports the last observed getter result; it is not a

fresh getter invocation made by each settings update.



Physical Android verification of this pilot APK and account-switching regression

checks are still pending. Frida remains required.

The native pilot stays opt-in until these gates pass.



### Release gates



The 2026-10-27 batch also passed device harnesses for catch callback ownership,

all transfer IV/category combinations, throw grading and AR+ argument forwarding.

MuMu completed a manual catch with native Excellent/curve grading and returned to

the map through the native details bridge. This does not verify all features or

physical Android behavior. The tested batch is retained as a rollback.



The ball-submit address is shared with the berry interceptor. It retains a Gum

observer calling the compiled throw policy until that entire shared site moves.

Installing Dobby and Gum independently at the same entry is not supported.



1. Verify native policy and original-call chaining, ABI/export contracts, and the

   unchanged JS fallback.

2. Verify a single APK on MuMu and an ARM64 Android phone, including disabling the

   setting, background lease expiry, restart and account switching.

3. Additional feature ports may be developed on the experimental branch, as

   requested on 2026-10-27. Do not enable the native migration in normal packages

   until the same-APK device and account regression checks pass. Port each feature

   behind its own validated configuration and lifecycle boundary.

4. Remove Frida only when the Java hooks, IL2CPP discovery, gameplay callbacks and

   translated bridge all have verified replacements. Do not remove a working

   dependency just because one native feature works.



Native hook installation failures must not trigger a second engine attempting

to patch the same address. Disable the feature and require a clean restart.

Hooks with live callbacks must retain their code/trampoline for process lifetime.



### Shared state during the remaining ports



Inventory is also a dependency of VGP, buddy feeding and catch berries. Its

catalog must remain available when automatic recycling is off. Preserve VGP's

bag access and cleanup scheduling, and the refresh requests made after berry

consumption. A borrowed GC handle cannot outlive its owner; delaying release by

an arbitrary number of seconds is not an ownership protocol.



Treat a recycling request with an unknown outcome as unresolved. A settings

change, a different bag object or a stock change alone does not prove that the

old request can no longer apply. Match results using rooted object identity and

the requested item, and preserve the uncertainty across handler restarts.



Account commands need a process-lifetime monotonic token boundary, independent

of the JS handler's lifetime. Revocation must invalidate an in-flight operation

even if the feature is immediately enabled again. Managed calls and GC cleanup

must execute outside policy locks, on the established Unity thread; nested

callbacks must not free roots still used by an outer call. Account storage and

credentials remain the responsibility of the existing encrypted Java layer.



These are acceptance requirements for the remaining ports, not claims that

inventory or account controllers have already been migrated or device-tested.



## Credential boundary



The source review found game session snapshots encrypted with Android Keystore

AES-GCM and stored in the no-backup directory. LivePokeMap tokens also use

Keystore encryption; its login remains app-global because it is a separate

subscription. Settings exports use an allow-list excluding tokens. OAuth client

IDs and redirect schemes are public identifiers, not shared secrets.



Test credentials remain local and ignored. The source bundle also rejects

`login-info-*` filenames even if accidentally tracked. Signing credentials remain

in local private files or CI secrets. No shared service secret requiring a new

backend was identified; adding a server would not conceal client hook logic.



Source review is not a runtime penetration test. Existing precise-coordinate

diagnostics are a separate privacy consideration and are not credentials.



### Batch 4 observations (2026-10-27)



The catch-animation controller passed native ownership, restoration, lease,

exception, reentrancy, missing-target and thread-change tests. MuMu completed

a Shuppet catch with native speed 20x and returned to the map. The first launch

after installation crashed in linker64/Houdini before guest gameplay hooks

initialized; a fresh launch succeeded. The startup crash remains an unresolved

release gate, with local evidence retained. This is not a verified Android build.



### Batch 5 observations (2026-10-27)



MuMu cold startup reached the map. Native PokéStop camera transitions measured

40 ms in each direction, replacing the observed 600/700 ms transitions. Native

camera and adapter harnesses passed. The bag was full during the spin attempt,

as confirmed by the user; successful item collection and automatic dismissal

remain unverified by that attempt. No rollback is warranted by this result.



### Batches 6â€“7 observations (2026-10-27)



MuMu confirmed one 5 km egg assignment using the unlimited incubator, then

auto-incubation was restored to its original off setting. Native incubation

passed the ownership, interruption, inventory-confirmation and adapter checks.



The subsequent quick-spin check succeeded: the request was observed at

10:21:03.962 and successful dismissal at 10:21:04.216. The stop turned purple

and the game returned to the map; the opening-to-dismissal interval was 355 ms.

Batch 7 is retained as the tested deployment, with batch 6 as a rollback.



### Batch 8 observations (2026-10-27)



Both architecture builds, the native harnesses, adapter tests and repository

checks passed. MuMu cold startup reached the map. Native map-tap capture

produced a coordinate and the existing Java controller applied a short walk.

The bounded queue drops stale policy events and preserves original capture

timestamps so queued delivery does not alter double-tap timing.



Native buddy feeding issued three single-Razz requests, observed each success,

and confirmed 180 minutes of fed time. The buddy preference and map-tap mode

were restored to Off/Do nothing, and the original location was restored.

Batch 8 is retained as the tested MuMu deployment; batch 7 remains a rollback.

Physical Android, account switching and long-duration hunger re-arm remain

live verification gates. The hunger re-arm logic passed the isolated harness.



Claude's initial buddy pass timed out, and its subsequent revision and map-tap

worker hit a session limit. The coordinator corrected the static-call wrapper,

root ownership, reentrant cancellation, queue expiry and test-fixture defects,

then ran the checks and live validation above. These are independently verified

results, not successful completion claims for the failed workers.



An interrupted buddy request with no confirmed result remains blocked across

handler restarts until the process restarts; it is not automatically duplicated.

Frida is still required for the remaining controllers and discovery/bootstrap.



### Inventory, IV and account pilot checks (2026-10-27)



The native inventory controller reported the live catalog on MuMu; no item

recycling was performed. Both architecture builds and the isolated ownership,

uncertain-request and adapter checks passed.



The account command bridge now publishes one atomic command/token pair through

MuMu's Java-to-ARM mailbox. Omitting that new mailbox method initially interrupted

the shared policy heartbeat and left the Accounts screen waiting. A regression

check covers Java-array serialization, exact decimal tokens, cancellation and

mailbox freshness. After the fix, MuMu showed the current authenticated account

and native encounter/inventory IV labels (Delibird, level 17, IV 42.2%;

Drifblim, level 15, IV 33.3%). The inventory preference was restored to Off. Identity events

are intentionally consumed without logging their identifiers.



Native account tests cover original Dobby chaining and command replay guards.

They do not replace a live account-switch round trip, which remains pending,

as does physical Android verification.



### Incoming gifts pilot (2026-10-27)



The incoming gift controller now owns dialog tracking, one-attempt opening and

successful-response presentation skipping in C++. Existing Gum observers still

forward dialog events; sending and stickers keep their existing implementation.

Both architecture builds and repository checks passed. The native harness passed

23 scenarios, including dialog replacement during getters, stop during cleanup,

stale sequence callbacks, manual-open isolation and bounded-queue overflow.



MuMu cold startup reached the map and reported native gift readiness. The user

then confirmed opening a gift and skipping its reward presentation; the live log

recorded the native open and received-gift skip. Physical Android remains

unverified.



### Encounter berries pilot (2026-10-27)



The native controller owns berry selection, request tracking and held-throw

resumption. Existing shared Gum observers remain in place, and JavaScript calls

the original throw method once after claiming a native resume ticket.



Both architecture builds and repository checks passed. The production native

harness passed 25 scenarios, including cancellation during callbacks, stale

results, moving rooted references, timeout and thread ownership. MuMu reached

the populated map with the native berry backend ready. The user subsequently

verified feeding after the enum correction below; physical Android verification

remains pending.



Live berry testing exposed a boxed-type mismatch: the ball getter returns the Item enum, not System.Int32. The pilot now validates that enum separately; the native fixture uses a distinct enum class. The corrected build passes the native scenarios, and the user confirmed berry feeding works on MuMu. Physical Android verification remains pending.



### Raid, gym and Dynamax pilot (2026-10-27)



The C++ controller now owns battle scope, attack priority, dodge timing, manual

switch grace and managed-object lifetimes. Existing Gum observers forward events;

the shared Rocket charge entry does not receive a competing native hook.



Both architectures build. The compiled-library harness passes 27 scenarios,

including real lease expiry, foreign-thread rejection, policy revocation during

callbacks, nested stop and scope replacement during allocation or cleanup.

Adapter and integration checks cover metadata layout, event forwarding and

single-controller selection. These checks do not replace live battles.



Claude's recovery attempt was blocked by its session limit. Agy supplied two

reviews; the coordinator corrected actionable findings and documented rejected

findings separately. MuMu cold startup reached the populated map and reported

native battle readiness. The user verified Dynamax and gym automation, including

manual Pokémon switching during a gym battle. A grunt Rocket battle also passed

as a regression check of the existing Rocket controller. Raids and physical

Android verification remain pending.



### Rocket action policy pilot (2026-10-27)



Feature 20 moves action admission, cooldown timing and the shield-use guard into

C++. Each candidate scan begins with a ticket. Scope changes, prompt changes,

permission revocation or expiry invalidate that ticket; the game method is called

only after a successful, single-use claim. Manual shield use consumes the same

guard. The existing JavaScript observers, managed references, candidate ordering

and method invocations remain in place for this migration stage. The charged

minigame remains on its existing native implementation.



Both architectures build. Seventeen compiled-library cases cover consumed and stale

tickets, revoke/re-enable, scope replacement, shield prompts, real cadence and

lease expiry, wrong-thread calls and stop. Adapter and integration checks verify

that every legacy and vNext action call requires a claim. MuMu startup reached

the game map and reported the native action policy ready. The user subsequently

confirmed a Rocket battle works with this stage enabled. Physical Android

verification remains pending.





### Rocket combat controller (2026-10-27)



Feature 21 reuses feature 20 for authorization and cadence, and moves managed

object ownership, candidate evaluation and all five attack/shield call paths into

C++. Existing Gum observers forward events; no second hook engine patches the

shared vNext charge method. The JavaScript action controller is excluded whenever

feature 21 is selected. The charged minigame retains its own native controller.



The coordinator corrected reentrant replacement and cleanup defects found in the

initial worker draft and an independent Agy review. Incoming objects are rooted

before old references are detached, observations invalidate in-flight scans, and

late UI completion uses a token tied to the rooted pending object. Shutdown

blocks rearming until the outermost callback has released held objects.



Both architectures build and repository checks pass. The production-library

harness passes 27 scenarios, including replacement during allocation and cleanup,

late/replayed UI completion, reentrant shutdown, policy revocation, moving targets

and actual foreign-thread rejection. Every scenario verifies zero roots after

cleanup. Adapter/integration checks verify metadata, single-controller selection

and the shared state/finish guards.



The initial Claude implementation worker and its focused harness follow-up both

timed out after edits. The coordinator reviewed, completed and executed those

changes; independent Claude read-only and Agy reviews completed successfully.

MuMu cold startup reached the populated map and reported native Rocket combat

ready. The user subsequently confirmed a Rocket battle works on this build.

This controller build is retained as a tested rollback alongside the prior policy build;

physical Android and full Frida retirement are not yet verified.



### Bundled outgoing gifts and stickers (2026-10-27)



Feature 22 moves the explicit Send Gift transaction into C++: first-gift

selection, recipient validation, optional sticker reservation, request ownership,

and result/finally matching. Sending still requires the user's Send Gift click.

The shared JavaScript click replacement and callback observers forward into the

native controller; the legacy gift-list UI remains JavaScript.



An issued request with an unknown outcome stays pending rather than retrying.

Disabling the option or closing the profile cancels preparation, while issued

requests retain their arguments and promise for completion bookkeeping. Shutdown

revokes authorization immediately and releases managed objects on their owner

thread. Cleanup releases the local sticker reservation exactly once.



Both ARM64 and x86_64 builds pass. The production-library harness covers 20

numbered scenarios, including synchronous callbacks, cancellation during

preparation/reservation, foreign-thread shutdown and promise retention. Adapter

and shared-observer integration checks pass. Claude reviewed the corrected

controller; Agy's earlier findings were independently assessed and documented.

Research MuMu installation and cold startup reached the populated map, with the

native outgoing-gift controller ready. The user confirmed one-tap sending works

and attaches a sticker when enabled. This build is retained as a tested gift

rollback. Physical Android verification remains pending. Frida is still required

for the shared observers and other unmigrated features.



### Auto-transfer request execution (2026-10-27)



Feature 23 moves `ReleasePokemon` execution and its pending-request lifecycle

into C++. The executor rechecks the caught ID and the bag's current Pokémon,

repeats the native keep-policy decision, and checks the current authorization

after its final managed-object lookup. It retains the bag, Pokémon and promise

until completion or shutdown and prevents a captured ID from being submitted

twice. An uncertain request reports its timeout once and is not retried.



JavaScript still reads the classification data and proves that the record belongs

to a successful manual or VGP catch. Existing result/error observers forward

into native matching; the adapter applies completion events to the catch record

and toast. VGP uses the native timeout result. No separate native hook competes

with these observers, and this increment does not remove Frida.



Both architectures build, and the actual-library harness, adapter/integration

checks and full repository checks pass. Tests include synchronous completion,

stale callback tickets, protected Pokémon, identity mismatch, duplicate requests,

shutdown during the final lookup, and disabling/re-enabling during preparation.

Claude implemented the drafts and reviewed the corrected controller; Codex

verified and corrected the integration, with independent Agy reviews assessed.

Installed on research MuMu; cold startup reached the populated map and reported

the native transfer executor ready. The user reports auto-transfer working on this

build, which is retained as a tested rollback. Individual manual/VGP path coverage

was not specified; physical Android verification remains pending.





## Direct catch completion (feature 24)



The direct Quick Catch path now delegates its identity checks and final

`System.Action.Invoke` to native code. The existing feature 3 capture/suppression

bridge is unchanged. JavaScript still supplies the rooted record, runs the

transfer checks first, and handles ordinary-details recovery. This is not a full

migration of summary and details-screen orchestration.



The native call is synchronous and retains no managed objects. It checks the

view, response, Pokémon and delegate classes, active state, successful catch,

delegate target and exact 64-bit caught ID. Completion uses an owner-thread

lease and a single-use token bound to the revocation generation. It rechecks the

lease after identity validation, before invoking the callback. Callback failures

never retry through the legacy JavaScript completion path; recovery may open the

ordinary details screen if the original encounter identity still matches.



Both ABI builds and the production-library harness pass. The harness covers

identity mismatches, duplicate/older tokens, callback exceptions, reentry,

wrong-thread calls, disable/expiry, same-epoch rearming, expiry during validation,

and validation after STOP. Adapter and host tests cover exact ID bytes,

transfer-before-completion ordering and ordinary-screen fallback. The harness

receipt is `pgrazor/build/native/harness/20261028-045201-040242/receipt.json`.

Batch25 is installed on research MuMu. Cold startup reached the populated map

and logged `native-catch-completion-ready`; the transfer executor also initialized.

User confirmed Quick Catch skips all screens and returns to the map on MuMu. Physical Android verification remains pending. Frida remains required.

## VGP policy, requests and markers (features 25, 26 and 28)

The native policy owns scan cadence, catch/spin alternation, bounded target
quarantine and retry delays. Unknown request outcomes remain blocked until VGP
is toggled; an internal policy failure instead requires restarting the runtime.
The reserved epoch update field is currently diagnostic only. Request execution
has its own independent epoch, owner-thread and short-lease checks.

The native request layer issues encounter, red-ball capture and FortSearch calls.
It preserves the existing request arguments and transfers one rooted promise
back to JavaScript for polling. Temporary native roots are released on every
path. Failed native calls never fall through to a second legacy request. The
host still handles discovery, manual encounter exclusion, inventory checks,
promise results, auto-transfer and item-cleanup orchestration.

Marker projection now reads the target transform and camera in native code.
It checks object liveness, boxed value layouts, dimensions, finite coordinates,
owner thread and lease. It retains no managed references. Marker failures hide
the icon without disabling VGP requests; the existing event cadence is preserved.

## In-game Routes command gate (feature 27)

Native code owns command sequencing, cancellation and authorization expiry.
Reconstructing the controller preserves the sequence high-water mark so old
commands cannot execute again. The host keeps command payloads and all route
discovery, execution, progress and reward handling. This is a partial migration,
not a replacement of the full Routes controller.

Both architecture builds and the full repository checks pass. The combined
production-library harness receipt is
`pgrazor/build/native/harness/20261028-054728-036423/receipt.json`.
Coverage includes stale commands, backwards clocks, wrong-thread calls,
revocation during request preparation, returned-handle ownership, failed roots,
malformed coordinates, marker visibility and cleanup. Claude implemented and
reviewed VGP changes; Sol implemented Routes gating and marker projection; Agy
reviewed the VGP design and request layer. Codex integrated the work, corrected
review findings and ran verification. The user confirmed VGP spins, catches and
auto-transfer when enabled, plus Routes looping and the map-tap stop/continue
dialog. Marker display, item cleanup and physical Android checks were not
explicitly confirmed. Frida and JavaScript orchestration are still required.

Batch26 is installed on research MuMu. Its first launch stayed on the loading
screen; one restart reached the populated map. The cause of that initial stall
is not established. Startup logged all three native VGP components ready and
Routes available. VGP was left off following restart. On October 28, the user
confirmed the gameplay paths listed above. Batch26 is now a tested MuMu rollback;
batch25 is also preserved. The initial loading stall remains unexplained.



## VGP candidate inspection and results (features 29 and 30)

Native candidate inspection evaluates each rooted map object: expected class,
liveness, manual exclusion, visibility, 40 m catch distance, exact encounter ID,
and Pokestop eligibility/fort ID. IDs retain full 64-bit precision; fort IDs use
bounded UTF-16 to UTF-8 conversion. The adapter releases any returned fort root
if the lease expires or settings change before it can be adopted. JavaScript
still enumerates objects, resolves inventory/services and reserves seen keys.

The native result reader validates generic promise fields and reads catch/spin
outcomes. It returns one owned value root for the host to process. Pending
requests still resolve when VGP is off; stopping prevents further processing.
Transfer, item cleanup and the stop's visual cooldown update remain in the host.
The adapter explicitly retains native field-name buffers and frees an unadopted
value handle on decoding/adoption failure.

While preparing batch27, the installed batch26 reported a refused native spin.
Periodic option refreshes were arming from the control thread, where the native
API deliberately revokes rather than grants permission. Ordinary refreshes now
leave the gate alone; actual actions arm on Unity, and restrictive option changes
still revoke immediately. A native return code proving no request was issued
now waits before retrying instead of permanently pausing. Uncertain outcomes
remain blocked to avoid duplicate requests.

Both architecture builds and full repository checks pass. The actual production
library harness passed candidate inspection, results and the existing request
executor: `pgrazor/build/native/harness/20261028-162421-216682/receipt.json`.
Host tests cover off-mask completion, exact IDs, relocating objects, returned-root
cleanup, missing native components and proven-not-issued versus uncertain errors.
Claude wrote both modules and reviewed corrected code; Agy reviewed integration;
Sol added host and native regressions; Codex integrated, corrected and verified.
The user confirmed batch27 catches, transfers and spins on MuMu. Extended
walking/no-pause verification and physical Android were not explicitly confirmed.
Frida remains required.

Batch27 installed on research MuMu and reached the populated map on its first
cold launch. Native candidates/results and existing VGP components reported ready.
VGP was left off after installation. The user subsequently confirmed catches,
transfers and spins; batch27 is now the latest tested MuMu rollback. Physical
Android remains pending.

## VGP scene and inventory discovery (feature 31)

Native discovery resolves the map container and inventory snapshot, enumerates
nearby Pokemon/Pokestops and returns rooted array entries. The adapter adopts
returned roots; the host releases each skipped candidate immediately. Pending
requests retain their own roots. Missing scenes/services defer scanning; invalid
classes, counts and managed exceptions fail closed. Thread ownership and bounded
leases are checked before returning results.

The host still acquires the inventory service and rechecks red-ball availability
immediately before throwing without requiring a fresh map-container lookup.
Pending request completion still works while viewing another screen or after
VGP is switched off. Shared manual-encounter observation, orchestration and the
transfer/item handoff remain in JavaScript; Frida is still required.

Both architecture builds and full repository checks pass. The production-library
harness passed on research MuMu, including relocation, invalid metadata, managed
exceptions, expiry during final rooting and exact output ownership:
`pgrazor/build/native/harness/20261028-170539-891652/receipt.json`. Adapter tests
cover settings changes, invalid leases and tracking failures after root adoption.
Claude implemented the native module and adapter; Agy reviewed host integration;
Sol added regressions; Codex integrated, corrected and verified the changes.
Batch28 gameplay and physical Android verification are pending. Batch27 remains
the user-tested MuMu rollback.

Batch28 installed on research MuMu and reached the populated map on its first
launch. Native discovery, candidates and results reported ready. VGP was left
off after the restart for user testing. Physical Android is unchanged.

The user subsequently reported batch28 pauses with discovery error -14. That
code covers class mismatches. Batch29 adds stage-specific diagnostics; its
first launch reached the map, but the failing stage has not yet been captured.
Batch30 changes scene/container and array validation to IL2CPP assignability,
matching those lookup contracts while retaining exact inventory/count checks.
Config version 2 is 272 bytes; the extra pointer is class_is_assignable_from.
Both ABIs, full checks and production-library derived-type/covariant-array
regressions pass. This candidate is not yet installed or gameplay-verified.
Batch27 remains the tested rollback.

Batch29 reproduced `-14:enumerate:catch` on MuMu, confirming that the inventory
snapshot passed and the exact enumeration-array class check caused the pause.
Batch30 replaces that check with assignability and was installed successfully;
startup reached the populated map with native discovery ready. User gameplay
verification is pending.

The user confirmed batch30 catches, transfers and spins on MuMu. It is now the
latest tested rollback; physical Android verification remains pending.

## VGP pre-throw inventory and spin cooldown (feature 32)

Native maintenance reads the current red-ball count directly from the rooted
item service before a throw. It does not resolve a map scene or container, so
the check remains usable while another screen is open. A zero count prevents
the throw; a closed readiness window defers it without permanently pausing.

After a successful spin, native code checks the response and stop, preserves
the full 64-bit cooldown timestamp, and calls the stop's virtual cooldown setter.
This completion can run after VGP is switched off or its action lease expires.
It uses a separate bounded completion window and requires the proven Unity
thread. Temporary roots are released on every path; no network calls or hooks
are added. Pending orchestration and transfer/item handoffs remain in JavaScript.

Claude wrote the native module and adapter. Codex corrected strict Boolean
validation, unexpected return-code handling and the final pre-setter check.
Sol added host/adapter regressions, and Agy's host review found no new defect.
Both architecture builds and full repository checks pass. The actual production
library passed on MuMu, including empty inventory, object relocation, managed
exceptions, full-width timestamps and expiry before/during the setter:
`pgrazor/build/native/harness/20261028-174046-530012/receipt.json`.
Batch32 gameplay and physical Android verification are pending. Batch30 remains
the user-tested MuMu rollback. Frida is still required.

Batch31 startup caught a metadata binding error: the adapter requested the alive
method from the VGP table, but it is in the shared `p.methods` table. Batch32
corrects the binding and test fixture. Focused adapter tests pass, and an
independent source check verified the remaining bindings against quick-close
metadata. The native library is unchanged and matches the tested harness hash.

Batch32 reached the populated map on its first launch and reported native
maintenance ready. VGP was left off for user testing. Android is unchanged.

The user confirmed batch32 works as expected after the catch/transfer/spin test
request. Batch32 is now the latest tested MuMu rollback. Physical Android
verification remains pending.

## Consolidated VGP controller (features 33 and 34)

The candidate moves VGP scanning, pending encounter/capture/spin state,
manual-target exclusions, result handling, inventory-update waits and marker
scheduling into one native controller. A stateless native helper resolves the
spin RPC method and caught inventory record. Existing native submodules remain
behind their versioned interfaces.

JavaScript still transports settings, shared encounter-observer notifications,
UI events and records into the shared auto-transfer subsystem. Frida remains
required. Batch33 is installed and user-tested on research MuMu: catching,
auto-transfer, inventory cleanup and spinning work. It is the latest tested VGP
rollback; physical Android verification remains pending.

Claude implemented the native modules and independently reviewed the controller;
Codex integrated and corrected the findings, with Sol adapter/regression work
and an Agy adapter review. Both ABIs and repository checks pass. The actual
production library passed auxiliary, controller and targeted regression harnesses
on MuMu, including lease expiry, manual encounters during inventory work, moved
objects, partial root failures, one capture event, off-lease completion and
transfer feedback ownership. Final native receipt:
`pgrazor/build/native/harness/20261028-184150-922230/receipt.json`.

Batch33 first startup reached a populated map with both native modules ready.
VGP was left off for one combined catch/transfer/spin check. The physical phone
was untouched. No commit or release was created. Packaged-library hashes match
the verified native build; deployment receipt is retained locally.

The user confirmed batch33 catches, transfers, cleans inventory and spins
PokeStops. The consolidated VGP gameplay check is complete on MuMu. Future
migration work should move to another subsystem unless a new VGP defect appears.

## Auto-transfer facts inspection (feature 35)

A stateless native reader gathers IVs, species, rarity, shiny status, event and
background flags, size, favorites and egg status. It preserves the existing
feature-4 keep policy and feature-23 release executor. It issues no transfer
requests, installs no hooks and retains no managed objects between calls.

The reader verifies the exact captured ID before and after inspection, checks
boxed and field types, rejects unknown sizes and noncanonical Booleans, and
returns no usable facts after a managed error or expired permission window.
Its JavaScript adapter preserves all eight ID bytes and checks the proven
thread, metadata and returned values. The host rechecks source identity and
settings before submitting to the existing executor. Failed inspection keeps
the Pokemon; a selected but unavailable reader cannot use legacy classification.

Both architecture builds and repository checks pass. Native tests run against
the production library cover inspection errors, relocation, root cleanup and
permissions. A separate coupling harness feeds real reader output to the real
native keep policy, covering category protections, favorites/eggs, IV boundaries
and custom keeps. Receipt:
`pgrazor/build/native/harness/20261028-191921-327200/receipt.json`.
Batch34 exposed a translated-mailbox lifecycle bug: a control snapshot expiring
during the catch heartbeat permanently suspended both catch runtimes. Batch35
classifies that condition explicitly, revokes leases for the stale heartbeat,
and resumes on a fresh snapshot. Regression tests cover expiry in either runtime,
recovery, and continued terminal handling of unrelated errors. Native feature35
is unchanged. Batch35 is installed on research MuMu and its first cold launch
reached the populated map. The user confirmed all four manual Quick Catch/transfer
combinations; logs confirm Quick Catch restored before the first toggle and
transfers resumed after the previously saved Keep All setting was switched off.
Batch35 is the latest tested MuMu rollback; batch33 remains preserved. The
physical phone was untouched.
Frida and the shared JavaScript integration remain required.


## Ordinary summary completion (feature 36)

The summary path now delegates caught-response identity checks, Unity liveness,
the inherited `GuiController.Dismiss` call and its postcondition to C++.
The host still captures candidates, prepares pending details and wraps the call
in the existing presentation bridge before direct completion. Transfer ordering
and the Quick Catch-off/transfer-on path are preserved. This does not complete
the remaining summary/details orchestration migration or remove Frida.

Requests carry borrowed host GC handles. Native code duplicates temporary roots,
refreshes targets across managed calls and releases its own roots on every exit.
Validation and completion reject re-entry; completion consumes one token and
rechecks the owner, epoch, lease and revocation generation before invoking.
Refusals and exceptions never fall through to a second JavaScript dismissal.

Both ABI builds and repository checks pass. The production-library harness covers
identity, thread/lease gates, exactly-once invocation, exceptions, real relocation
during root allocation and managed calls, and balanced cleanup. Final receipt:
`pgrazor/build/native/harness/20261028-200644-179502/receipt.json`.
Batch36 is installed on research MuMu. Its first cold launch reached the map
and logged native-summary-completion-ready. The user confirmed the requested
Quick Catch and transfer-only dismissal checks work as expected. Batch36 is the
latest tested MuMu rollback; batch35 remains preserved. The phone is unchanged.

## Post-catch details completion (feature 37)

C++ now validates the post-catch details identity, invokes its normal close action
once, and reads the resulting state. The host retains candidate discovery, canvas
handling and the transfer-first decision. With Quick Catch off, kept Pokemon
remain open; a completed transfer still allows dismissal. Ordinary inventory
details are excluded by the post-catch identity checks.

Requests use borrowed GC handles and native temporary roots. Validation checks
the callback's target, both caught IDs and the single-Pokemon backing list.
It refreshes targets and checks classes after managed calls, then checks Unity
liveness and the permission lease before clicking. Unknown postconditions never
count as confirmed destruction, and refusal cannot fall through to a legacy click.

Both ABI builds and repository checks pass. The production-library tests cover
identity, opening screens, token/lease/thread gates, relocation, root failures,
callback ownership and invalidation during getters. Receipt:
`pgrazor/build/native/harness/20261028-204602-273930/receipt.json`.
Claude's implementation was independently reviewed and corrected by the
coordinator, with a separate Sol regression fixture and Agy review.
Batch37 is installed on research MuMu. Its first cold launch reached the populated
map and logged native-details-completion-ready.
The user confirmed the requested transfer-only details dismissal works. Batch37
is the latest tested MuMu rollback; batch36 remains preserved.
The physical phone was untouched. Frida and shared JavaScript remain required.

## Post-catch details recovery (feature 38)

The native reader now recovers a post-catch details record from a rooted GUI and
its expected caught response. It verifies the callback target, caught ID, post-catch
flag and one-Pokemon list, then runs feature37's identity validator before returning
six owned handles. The adapter duplicates those into host ownership and releases
all native outputs. The host publishes the record only if the original pending
catch is still current. Scene discovery, canvas handling and transfer order remain
unchanged; ordinary inventory details are rejected.

Both ABI builds and repository checks pass. The production-library tests cover
opening screens, identity mismatches, native-to-host adoption, repeated release,
real object relocation, allocation failures, expiry and reentry. Receipt:
`pgrazor/build/native/harness/20261028-211019-409976/receipt.json`.
Claude implemented the reader and adapter. Coordinator review corrected an early
full-record check before the Pokemon root existed and tightened cleanup; Sol's
independent fixture verifies the full production reader/validator path. Agy's
reviews and their dispositions are retained locally. The user confirmed both requested manual catch paths work on batch38. Its first
cold launch reached the populated map with native-details-recovery-ready. Batch38
is the tested MuMu rollback; batch37 remains preserved. The physical phone is unchanged. Frida remains required.

## Catch canvas and inventory polling (feature 39)

The native canvas module owns the visibility hook and bounded, rooted hide/restore
records. It suppresses visibility only for an identified opening catch-details
screen. Expiry, disabling Quick Catch and shutdown revoke ownership; restoration
runs on the proven Unity thread. The hook itself makes no managed calls. The host
still controls catch orchestration and transfer order. Frida remains required.

Review also confirmed disabled auto-discard still enumerated the inventory in both
the legacy and native item implementations. Both now distinguish idle access,
berry-count requests and full catalog requests. Shared service readiness remains
available for VGP, berries and incubation. Disabled idle ticks do no item-list or
quantity calls; visible berry controls query only five berry types. Full scans
require enabled auto-discard or explicit item-settings demand. Unchanged catalogs
do not advance the publication serial, while pending-request cleanup still runs.

Both architecture builds pass. Production-library canvas, independent lifetime and
item regressions passed, including the slow confirmation-timeout case. Harness:
`pgrazor/build/native/harness/20261028-214235-672781/receipt.json`.
Claude supplied the canvas implementation and adapter; coordinator/Sol review
corrected relocation and retryable-release handling. Agy reviewed item polling;
review dispositions are retained locally. Gameplay validation remains pending.

Batch39 is installed on research MuMu. Cold startup reached a populated map with
`native-catch-canvas-ready`; the user confirmed it works. Batch39 is the latest
tested rollback, with batch38 preserved. Physical Android was not changed.

## Catch candidate discovery (feature 40)

Native code now reads the top GUI and performs the bounded details-component
scene query. Each candidate is rooted, copied into host ownership and checked by
the existing caught-identity recovery before hiding. Scene scans retain the same
50 ms spacing and twenty-scan budget. The reader never dismisses a screen or
submits a gameplay request. Shared observers and host orchestration remain.

Claude implemented the reader and adapter. Coordinator review corrected the
install return convention and protected output cleanup against reentry; Sol's
independent production-library fixture reproduced the cleanup bug before the fix
and passed afterward. Both ABI builds and the full repository checks pass.
The final harness receipt is
`pgrazor/build/native/harness/20261028-221122-519576/receipt.json`.
Physical Android and MuMu gameplay validation for this batch remain pending.

Batch40 is installed on research MuMu. Cold startup reached a populated map and
reported `native-catch-discovery-ready`; the user confirmed it works. Batch40 is
the tested rollback, with batch39 preserved. No physical phone installation or public release.

## Continuous camera sampling (feature 41)

The continuous sampler now reads camera identity, map visibility, forward vector
and heading in C++. It proves the raw rig type before allocating, roots the
returned camera first, refreshes object addresses after managed operations, and
releases every temporary root before publishing scalar output. Expiry is checked
between getters and after cleanup. Shared invocation observers and host queueing
remain, including the 100 ms throttle and map-hidden invalidation.

The camera script is now bundled with its native loader and adapter for both
Android and MuMu. Legacy non-pilot behavior and bounded diagnostics remain.
Claude implemented the sampler; coordinator and Sol review corrected expiry and
status reporting. Both architecture builds, compiled-library lifetime tests,
adapter/host tests and repository checks pass. Agy's final review found no further
actionable issue. Final native harness receipt:
`pgrazor/build/native/harness/20261028-224931-805005/receipt.json`.
Physical Android remains untested for this batch.

Batch41 is installed on research MuMu. Live native readings reported the map
visible and a camera drag changed yaw from 0 to about 331.38 degrees with the same
camera ID. No sampler errors occurred during this check. Hidden-map transitions
are covered by tests but were not exercised live. Batch40 remains the user-tested
rollback; physical Android is unchanged.

## Compiled Android location delivery (batch 42)

The pilot now uses compiled Java for the location timer, provider-listener proxies,
synthetic fix construction, permission decisions and Adventure Sync dismissal
policy. A small JavaScript adapter retains the existing game-facing hooks. This
does not replace Android's native location method or grant OS permissions. Legacy
non-pilot behavior remains available; a selected pilot does not silently fall back.

Two Claude workers implemented the helpers and adapter. Coordinator review fixed
Java enum reflection and lifecycle rollback. Independent JVM regressions reproduced
and then verified fixes for reentrant service replacement, partially installed
listeners, stale callbacks after restarting the same manager, teardown during
context lookup and ambiguous listener overloads. Generation checks guard delivery;
timer revisions prevent duplicate scheduling. The repository checks pass. Both
packaged native libraries are byte-identical to batch41. Physical Android validation
is still pending for this batch.

Batch42 is installed on research MuMu and reached a populated map on its first
cold start. Fine/coarse location permissions remained denied; system location
remained enabled. Compiled delivery and permission events were observed without
location errors. No gameplay requests were exercised. Batch40 remains the
user-tested rollback, with batches39 and41 preserved. Superseded batch36-38 APK
replicas were removed; receipts and diagnostic evidence remain local.

## Compiled Google sign-in handoff (batch 43)

The pilot now keeps pending Google sign-in consumers, the 250 ms result poll and
game callback construction in compiled Java. The existing browser authorization
flow and account storage are unchanged. Result JSON and token values no longer
cross the JavaScript adapter on this path. One thin game sign-in hook remains;
non-pilot packages retain the previous implementation.

Two Claude workers implemented the helper and adapter. Coordinator corrections
cover cancellation during replacement/completion, rejected handler posts and
invalid request IDs. Independent JVM tests using Android's JSON implementation
reproduced the lifecycle failures before correction and pass afterward, including
foreign-thread shutdown. Browser calls and game callbacks stay on the main thread;
logging excludes result payloads. Adapter, host-selection and full repository
checks pass. Live browser login and physical Android validation remain pending;
startup verification does not exercise a new account sign-in.

Batch43 is installed on research MuMu. Its first cold start reached a populated
map and confirmed the compiled authentication factory and hook are ready. The
existing account remained signed in. A new browser login was not attempted.

A subsequent user-driven Google sign-in completed on MuMu: logs confirm a
successful token exchange and the compiled game callback reporting Success. The
game reached a populated map after an account-profile restart. An earlier
cancelled attempt logged a callback failure; its cause remains unresolved, so
cancellation is not yet fully validated on-device. Physical Android is untested.

## Map POI snapshots (feature 42, batch 44)

The native pilot now selects a C++ reader for nearby PokéStops, gyms and raids,
including gym teams, occupancy and raid boss metadata. The world map UI and its
three-second refresh cadence are unchanged. This reader is independent of VGP
and runs only while the map or route planner grants access. It issues no gameplay
requests and installs no hooks.

The adapter keeps configuration and scalar decoding; C++ owns managed enumeration,
temporary object roots, bounded output and optional gym/raid reads. Selected native
initialization failures report unavailable rather than falling back silently.
Two Claude workers implemented the reader and adapter. Coordinator and independent
review corrected expired reads, strict Boolean validation and snapshot corruption
during reentrant cleanup. Both architecture builds, repository checks, and native
tests against the compiled library pass, including moving object addresses and
cleanup-time expiry. Harness receipt:
`pgrazor/build/native/harness/20261029-025111-277086/receipt.json`.

Batch44 is installed on research MuMu. The native reader displayed the same 394
locations as the previous implementation. Gym team and occupancy, raid tier and
egg hatch/end timers were verified live. Hatched boss data is covered by fixtures;
it was not checked live. Temporary map filters were restored. Physical Android
remains untested; no catch, spin or transfer test was needed for this batch.

## Routes state and actions (feature 43, batch 45)

This batch moves the current-route reader and discover/start/complete/pause
service calls into C++. The Java walking controller, route discovery geometry,
service-capture observers and reward UI handling remain in their existing layers.
Native command gate 27 still authorizes each sequence once. The action bridge
checks service identity, route identity, eligibility and the foreground lease
before submitting through the game's existing service methods.

Two Claude workers implemented the core and adapter. Independent review and
coordinator fixes preserve synchronous completion callbacks with one restricted
read-only nested state query, separate adapter buffers, current-route identity
checks and cleanup-time revocation. A submitted mutation remains submitted even
if the lease closes during its call; rejected sequences are never retried.

Both native builds, the repository checks, and focused host checks pass. Actual
compiled-library tests cover moving objects, completion eligibility, expired or
revoked commands, callback identity changes and malformed Boolean fields. Receipt:
`pgrazor/build/native/harness/20261029-032726-938593/receipt.json`.
Installed on research MuMu: native Routes readiness, current-state reads and
discovery submission were verified, and the nearby-route list populated. Returned
to the map with VGP off. The user subsequently confirmed route completion on MuMu. Physical Android
validation remains pending.

## Routes discovery and cache migration (batch 46)

Native feature 44 moves nearby-route enumeration, waypoint and endpoint reads,
and the retained route-definition cache into C++. The adapter copies bounded
route headers and waypoint arrays into the existing bridge shape. Route starts
borrow the cache's managed handle and use the existing native action path.
Java walking, service capture and reward presentation observers remain unchanged.

The cache is tied to the Routes service and a revocation generation. Its roots
are released on the verified owner thread. Only paths with at least two finite,
in-range points are admitted. Expired or revoked reads cannot publish a snapshot
or return a cached route handle. Reopening the planner refreshes before starting.

Both ABI builds, the repository checks, the native harness and 24 independent
native scenarios pass. Adapter checks cover cancellation, reentry and fresh timing;
host checks cover snapshot preservation and refresh before start. Claude's final
read-only review found no remaining blockers after coordinator corrections.

Installed on research MuMu: both native Routes backends became ready and the
nearby-route list populated on first open and after closing/reopening. No route
read errors were recorded. The user subsequently confirmed successful route completion with the new cache.
Physical Android validation remains pending. Batch 45's completion/reward build is
retained as rollback. No public release was created.

## Routes service capture migration (batch 47)

Feature 45 moves the two service-capture hooks and ownership of the Routes and
Route Consumption service handles into C++. The existing native actions and
discovery cache borrow those handles. The Java walking controller and the
tracking, reward and rating observers stay unchanged in this batch.

Capture uses a bounded heartbeat independent of route-planning permission. Roots
are retained before replacement cleanup, active handles remain stable for repeat
captures, and the Routes dependency is authoritative for the consumption service.
Revocation prevents stale captures or handle publication. The host clears refused
borrowed handles without freeing native-owned roots.

Both ABI builds, the repository checks, and actual-library Dobby tests pass.
Nine additional coordinator regressions cover moving receivers, stable handles,
service replacement, detached threads, cancellation and reentry. Three of those
tests reproduce bugs in the initial draft and pass after correction. Claude's
final read-only review found no blockers. A separate test-writing worker timed
out without producing a file; its work is not counted as verification.

Installed on research MuMu: all three Routes backends reported native, both
services were captured, and opening the planner submitted discovery and populated
the nearby-route list. No route faults were recorded. Returned to the map without
starting another walk. Full-route completion on this batch and physical Android
validation remain pending; batch 46 is the user-tested full-route rollback.
No public release was created.

## Overnight continuation after batch47 (in progress; not a completion claim)

The coordinator added native Routes result observers (46), Routes tracking/location/waypoint hooks (50), metadata validation (47), and a dormant Unity scheduler (48). Metadata and scheduler foundations passed actual-library MuMu fixtures; production metadata/scheduling still uses the existing host. Routes result observers passed their primary real-Dobby test and nine independent lifetime/cancellation scenarios. The primary fixture initially failed because direct calls allowed compiler omission of arguments unused by fixture originals; volatile function-pointer calls preserve the hook ABI. Tracking adapter signature/executable boundary tests pass; native tracking build/live integration verification is pending.

Java now owns the translated-host mailbox timer and typed settings/action transport. The mailbox tests include exact integer tokens, malformed batch rejection, consume-before-dispatch acknowledgement, and symlink boundaries. Native guest transport remains unfinished.

An opt-in build-time DEX rewrite (`PGRAZOR_COMPILED_JAVA_BRIDGES=1` with the native pilot) replaces eleven authentication, permission, dialog and service-lifecycle method bodies with compiled Java dispatch. It preserves renamed original bodies and method signatures, including private lifecycle dispatch. Both browser-enabled and original-browser paths are retained. `GameBridgeDispatch` prepares delivery on main before Unity and delegates to the existing Java helpers. The real original APK contains all eleven expected descriptors. A separate rewritten DEX fixture passed on MuMu ART (receipt `build/dex-bridge-fixtures/20261029-153832-280853/receipt.json`), testing callback identity, null Activity fallback, 64-bit flags, original exceptions and lifecycle ordering. This is boundary verification; the new game APK has not yet been built or installed. Agy reviewed the rewriter; duplicated runtime annotations on preserved bodies were removed. Its claimed sign-in fallthrough was not present in the source, and lifecycle exception ordering intentionally matches the existing adapter.

Catch observers (49) and shared-catch orchestration (51) remain under correction. Independent catch review found completion-after-lease, event loss and rooting defects. The first shared-catch draft moved only part of the arming policy while retaining JS selection/pruning; the coordinator rejected its broader ownership claim and requested actual integration. Do not count these drafts as completed migrations.

Frida/readable production scripts are still required. Authentication/location bridges, guest bootstrap, complete shared orchestration, metadata and native scheduling must be verified before retirement. No new production APK or retention pin has replaced batch47 yet; preserve user-tested batch46. Physical Android remains untouched. No commit, push or release.

Overnight continuation: native shared catch queue/expiry/arming (51) and Routes metadata (53) now pass actual-library MuMu fixtures, receipt `20261029-162344-295953`. Root review corrected manual-transfer gating, shutdown cleanup, FIFO order and synchronous transfer correlation. Shared managed observers, GC-root ownership and the Unity fanout still use JavaScript. Routes metadata's default guest module-map path still needs a live APK check.

Compiled binary transport (52) is being integrated and independently reviewed; initial Java seam tests pass, but native replay/bounds corrections and actual JNI verification remain pending. The transitional adapter removes the JSON/file wire for the selected translated runtime, not the gameplay script runtime. The standard ARM JNI/direct-buffer research probe passed on MuMu; same-process host/guest shared transport state remains unverified. No new production APK has been installed yet. Latest detailed evidence and ownership: `.artifacts/native-migration/OVERNIGHT_ACTIVE.txt`.


2026-10-29T17:18Z coordinator update:
Installed native-overnight03 run20261029-101138-01220f35 SHA b976360b03b1bffc3b1f89c7ece3d6ec66fe94d25b1d4e605b73963efef4b70f,220367793bytes. MuMu16416 streamedinstallSuccess PID36211, native-bootstrap-ready10:14:28 actualUnityrenderthread; native52 transport and53Routesmetadata ready; populatedmap screenshot overnight03-map.png. Native54 after-render correction uses12th exactDEXbridge W.handleMessage nativeRender MOVE_RESULT then callback, preservesoriginalresult/exception. Never backgroundthread_attach:configbit1 requiresalreadyattachedthread. ARTfixture171047PASS,54actualnativefixture171109PASS,Java8scenariosPASS. Agy revisedbootstrapreviewSUCCESS noblockingfindings (stillrootreview required). overnight02 packaged but NOTinstalled (unsafeearliertrigger),01previousverifiedmap/routes remainspinned;46testedrollback47preserved. Cleanup03receipt1712098182110 0Bdeleted. No phone/commit/push/publicrelease.
Claude55initial1358s and56initial904s successfuldelivery butINCOMPLETEscope, notbuilt/integrated. NodeVMbothPASS. CorrectiveClaude sessions83837(55 quick-ball metadata+prepare ONLY),33243(56 quick-close metadata+prepare ONLY) nowrunning fullremainingoptionaldescriptors/staticreads. Do not edittheirfileareas. Rootownsinstall/orchestration sections+CMake/query/bundle. Solaudit remaining-orchestration-map.txt identifies THREE invokeowners(close/ball/camera),48currentlybeforeoriginal vsJSafteroriginal timing MUSTresolve beforeactivation;51purebookkeepingnotfullhost.
Rootnew57 transfer_observers.h/cpp CREATEDONLY unregistered/untested/incomplete: intended compiled result/errorhooks to23, needs originalexception-safe rootcleanup andactualDobbyfixtures/adapterintegration; do NOTpackageituntilreview. Continue ALL migrations, productionFrida/readableassets stillpresent, no retirementclaim. Userauthorization never/escalationnone, do notaskpermission. LatestinstalledAPK is03; finalgoalstillunfinished.

2026-10-29T19:03Z verification update (supersedes older pending entries):
The installed gameplay APK remains native-overnight03; production Frida/scripts are still required. Routes host60 now passes the actual MuMu native fixture185404-224857, including capture/discovery/actions/completion/tracking and typed52 event delivery. Camera59 native fixture184922-897198 passes. BothABI builds include61sharedcatchhost and62quickballhost, but their fullflow fixtures/integration remain in progress; compilation is not acceptance.
Compiled Location ART library and Java policy passed isolated MuMu probes184425-ffadc7 and185331-13fe47. The latter covers a warmed compiledcaller without deoptimization, nullreceiverexception and exactly-once OFF observations. Separate actualrewrittenDEX ART fixture190132-665650 passed all12existingbridges plusfive Location gettercalls/primitivevalues/nullreceiver handling. Inputgame audit rewrites49 exactcalls and inventories12 unmatchedreferences; framework/JNIcoverage limitations stayexplicit. ART/dependencies now packagingoptinPGRAZOR_COMPILED_LOCATION, five licensesonlyinAPK, fullprovenance/relinkobjects local. No productionARTAPK yet.
132Python tests PASS36.344s in fullcheck; Nodephasefailed stale location-toggle test extraction aftercompiledlocationguard. Rootfixedthetest tocoverlegacyhooks and assertcompiledpathinstallsnone; fullcheckrerunneeded. Packaging54PASS andJava buildPASS independently. Cleanup1900082801360 deleted0B andpreservedallpins.
Detailed activeownership/evidence remain .artifacts/native-migration/OVERNIGHT_ACTIVE.txt. Userauthorization persists; no phone/commit/push/release andno permissionquestions.

## Compiled-only cutover integration, 2026-10-29 20:54Z

This update supersedes earlier pending architecture notes without changing their
historical verification scope. The compiled production architecture now includes
Routes (60), shared catch (61), quick ball (62), encounter (63), camera (64),
inventory (65), combat (66), spin (67), account (68), interaction observers (69),
the runtime host manager (70), and spawn orchestration (71). It uses compiled
Java/ART bridges, native bootstrap and Unity scheduling, and typed native transport.
Both ABI integration builds passed. The compiled-only APK was built and its asset
audit passed with zero Frida or PGRazor JavaScript assets. These build results do
not establish working gameplay.

Live cutover testing on research MuMu failed. After an ARM export-veneer collision
and a Dobby page-boundary permission defect were corrected, the actual native
page-boundary/manager-flow fixture passed (`20261029-205026-333272`). The subsequent
diagnostic startup reached all hosts and a second pulse, then repeatedly crashed
during map loading near the method adjacent to an eight-byte camera getter.
The suspected small-method hook overwrite remains under repair; no stable populated
map or completed cutover is verified. Evidence is retained in
`.artifacts/native-migration/cutover-page-boundary-startup.log` and
`.artifacts/native-migration/latest-cutover-debug.json`. The latter identifies the
diagnostic APK and hash but still records `installed: false`; the startup log and
coordinator record provide the live-run evidence.

Verified native-overnight06 and user-tested batch46 remain preserved rollback
builds. Overnight06 still requires ARM gameplay Frida. Final live acceptance,
removal of temporary diagnostic traces, final packaging and cleanup remain pending.
Physical Android has not been tested; no commit, push or public release is claimed.

## Production cutover verification, 2026-10-29 21:42Z

This supersedes the failed-startup status above. Standard production factories now
have compiled owners: hosts 60–71, catch-wait 58 and their native leaf/observer
dependencies, driven by native Unity scheduling and transport52 with Java/ART
bridges. The bounded dependency audit found no remaining standard production
factory requiring JavaScript. Research and legacy development scripts remain in
source and are excluded from this production APK. APK audits found no Frida or
PGRazor JavaScript assets. Both ABI libraries are stripped: the core exports only
`JNI_OnLoad` and `pgrazor_native_api`, and the ART library only `JNI_OnLoad`.

The installed `native-cutover-final` candidate (SHA-256
`c8a8b67a829ef1a1d34b2932fdddf0d7868c7ff796e6203e2b476c06c5241726`)
passed populated-map startup, a normal catch, 20x/58 wait shortening and nearby
Routes listing. Subsequent diagnostic builds verified quick-spin request and
dismissal in 230 ms, returning to a purple map stop; catch IV display passed for
Oddish and Pumpkaboo across three checks, including berry overlay enabled.
Earlier missing-IV observations for Wooper/Ekans were not reproduced in those
checks; universal resolution is not established.

Permanent integration fixes cover ARM export veneers, four-byte hook branches via
near ELF-padding islands, both Dobby end-page boundaries, ART/LSPlant backup
publication under SuspendAll, and the Nameplate four-argument weather ABI with
both metadata-pointer slots refreshed. Repeated cold starts passed. Shared host61
now refreshes a coherent owner-thread transport snapshot and invalidates the
DISMISS epoch. Actual fixture `20261029-213354-895344` passed OFF, protection,
keep-policy, dismissal and stale-snapshot cases. Live PID60739 dismissed Oddish
summary/details at 14:36:54 and returned to the map; evidence:
`.artifacts/native-migration/nameplate-diag-live.log` and `shared-fixed-map.png`.
Temporary diagnostic source changes have been removed without skipping hosts.

Full repository checks passed: 135 Python tests plus Java and Node checks
(`.artifacts/native-migration/cutover-final-check.log`). Broad actual native
fixtures passed at `20261029-205946-321689`. The final `native-cutover-verified`
packaging job is running at this status point; its installation and verification
are not yet claimed. Claude worker96795 timed out, and the earlier Agy repair
failed with service503 after 2399 seconds; neither failure counts as verification.

Live coverage on this APK does not include full Routes completion/rewards, a full
VGP cycle, combat/raids, gifts/incubation/buddy, or a new Google login. Native
fixtures cover their tested boundaries; earlier overnight06 Google validation is
not a login test of this APK. Physical Android remains untouched and unverified.
Cleanup receipts record 0.767 GiB plus 181,164,614 bytes reclaimed. Verified
overnight06 and user-tested batch46 remain preserved rollback builds. Final
package installation, retention and handoff remain coordinator work; no commit,
push or public release is claimed here.

### Final installed local artifact - 2026-10-29 21:46 UTC

`native-cutover-verified/PGRazor.apk` (run `20261029-144048-be59e7c8`, SHA256 `cf229832cc078f18e50e3de8c723507d85793f1331bfce780f2e8f7e37f0766b`) is now installed on research MuMu16416, PID61867. Cold start, populated map, Pumpkaboo encounter with IV label, and return to map passed. All12 manager consumers reached ready. Final native fixture214201-352050 passed and matches the packaged x64 core. The APK audit confirms zero Frida/PGRazor-JS assets and stripped core/ART libraries with only the intended factory/JNI exports. Temporary diagnostic code is removed. See `.artifacts/native-migration/FINAL_NATIVE_CUTOVER_HANDOFF.txt` for exact verification provenance, remaining live limits, rollback and tomorrow checklist. Physical Android remains untested; the earlier intermittent missing-IV observation is retained rather than declared resolved.

Repository retirement audit correction (after final APK delivery):
The shipped APK is compiled-only, but repository retirement is NOT complete. No tracked source files have been deleted. Prior cleanup totals describe ignored generated APK outputs only. Legacy production packaging and JS adapters/tests remain; the release workflow still selects the legacy path. Routes/gift native schema generators consume legacy JS literals and must be detached before deleting those scripts. Research runtime dependencies and license/provenance notices must be audited separately. The source archive currently lists only tracked files and omits untracked native migration files, so it is not a complete distributable source package in this working tree. See .artifacts/native-migration/repository-retirement-audit.json. The final installed APK/hash is unchanged. This audit supersedes any implication that the entire repository was buttoned up.

### Repository cleanup and compiled default - 2026-10-29


Cleanup completed 2026-10-29 22:28 UTC (supersedes repository-retirement correction above).
Deleted180 obsolete gameplay JS/adapters/adapter-only tests, including46 tracked files; verified source backup and receipt retained locally. Removed2219771 source bytes; backup729611 bytes, net1490160 bytes. No additional generated APKs eligible in cleanup222659-4059747. Retained research scripts/Frida vendor dependencies and historical native-port comments intentionally.
Routes/gift generators now use versioned neutral JSON with unchanged generated table semantics. Default local/CI production packaging is compiled with no environment opt-ins. Gadget requires explicit --research-runtime and is rejected for retired quick gameplay. Bootstrap likewise gates research mode. Production packaging rejects research payloads. Dormant research loader strings remain in DEX: strict runtime_retired inventory remains false; production_payload_retired is true. This distinction is intentional and not evidence of a bundled interpreter.
Portable pinned native prerequisites setup and CI wiring added; local preflight/tests PASS, fresh remote CI workflow NOT run. Complete working-tree source archive includes native additions, excludes retired sources/keys/local evidence; receipt cleanup-source-archive-receipt.json. No staging/commit/push/release.
Final local APK: pgrazor/build/release/native-cleanup-final/PGRazor.apk
Run20261029-152358-9320f1cc; SHA256384043a6681fdcae2a36df6405e6c1c2f625f5f9dcaaa82279e3f7b9a069e0f4;181168710bytes.
Installed research MuMu16416, cold PID64645, populated map visually confirmed; compiled bootstrap/location/scheduler/hosts ready, Routes service became available. No phone touched. Final cleanup APK did not repeat every gameplay scenario; prior cutover gameplay evidence and limits remain applicable, not relabeled as new verification.
Checks: tools/check.py exit0,127Python tests + Java fixtures +21Node suites; actual native/Dobby host fixtures61/63/70 PASS receipt20261029-222611-354213, packaged x64core matches385ed6d04a4d1fcec8e744efa2bbdd6ba7c2f7cebf98de3dc04583aecb93995c. BothABI core/ART packaging verified. Source archive content checks PASS. Final install/log/screenshots and payload audit use cleanup-* evidence files.
Claude cleanup worker timed out600s after partial helper edits; coordinator/internal workers completed and independently verified the work. Previous Claude/Agy results retained honestly. All prior deployed/tested rollback pins retained; cleanup candidate now pinned too.
No known cleanup blocker remains. Remaining verification: physical Android; extended live Routes reward, VGP/transfer protection, combat, social/eggs and new-login roundtrips; intermittent IV observation from earlier run remains on checklist. Tomorrow: cold launch/login/location; repeated IV/berry/Quick Catch ON/OFF and Quick Spin; VGP/protected transfer; full Route/reward and combat/social features.
