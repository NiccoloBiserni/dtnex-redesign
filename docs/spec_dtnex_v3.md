# DTNEX — Contact exchange redesign

**Date:** 2026-08-03 — §13 added and implemented on 2026-08-08
**Status:** implemented on the `redesign` branch, with the known limitations recorded at the end.
§13 (termination) is implemented and verified (§13.8), but it **does not solve the
symptom that motivated it**: see §13.9.
**Resulting protocol version:** 3 (from 2)

---

## 1. Problems to solve

Three defects of the current implementation, all verified against the code.

### 1.1 The contact that is sent does not match the one that is stored

`exchangeWithNeighbors` (`dtnex.c:734-777`) does not read ION's contact plan. It builds the `ContactInfo` synthetically from the *plans* (convergence-layer adjacencies): `nodeA = local nodeId`, `nodeB = a neighbour`, `duration = contactLifetime / 60` taken from the configuration file.

`getContacts` (`dtnex.c:1009`) does genuinely read ION's contact RBT, but it is a purely diagnostic function: it prints a table, counts the entries, detects an ION restart. Its output feeds nothing.

The two worlds never touch. The source of truth for what gets announced is the plans, not the contacts.

A second source of divergence: on reception (`dtnex.c:3241-3244`) the received `timestamp` is discarded and the start time is rewritten to the local `time(NULL)`. Across a multi-hop flood, the same contact takes on a different absolute window on every node.

### 1.2 The range is made up

`owlt = 1` second, hardcoded (`dtnex.c:3269`). Ranges are never exchanged between nodes. It is not the only fabricated parameter: `xmitRate = 100000` and `confidence = 1.0` are hardcoded at insertion time as well (`dtnex.c:3224-3225`).

### 1.3 Indiscriminate overwriting

`rfx_remove_contact(regionNbr, NULL, ...)` with a `NULL` timestamp is equivalent to ionadmin's `*` scope: it deletes **every** contact between the pair, including those configured by hand by the operator, which dtnex never inserted. The same goes for ranges (`dtnex.c:3230-3233`, `3274-3277`).

This happens on every received bundle, so on re-floods too: continuous churn of the contact plan, which repeatedly invalidates the CGR routes.

---

## 2. Guiding principle

Three notions that are conflated today are cleanly separated:

| concept | meaning | role in the design |
|---|---|---|
| **plan** | "I have an outduct towards X" | transport layer: who I send bundles to |
| **contact** | "there is a transmission window X→Y" | payload: which topology I announce |
| **range** | OWLT between two nodes | payload, carried together with the contact |

In ION, plans and contacts are configured independently. Confusing the two is the root cause of problem 1.

---

## 3. Data model

### 3.1 `plans[]` — the flooding peer set

Role: who I send bundles to. Nothing else.

Structure and population unchanged (`planId`, `timestamp`, from `getplanlist()` over ION's `BpPlan`s, TTL cache). All the code that uses it to build the `destEid` stays valid. Only its conceptual role changes: it is no longer a source of contact data.

### 3.2 `myContacts[]` — the origination set

Role: what I announce. **Read-only from ION**, a snapshot with a TTL cache, no local state.

Populated by walking `ionvdb->contactIndex` with the filter `fromNode == config->nodeId`. For each contact, the matching range is looked up in `ionvdb->rangeIndex` to derive the `owlt`.

| field | origin |
|---|---|
| `regionNbr` | `IonCXref` |
| `fromNode` | `IonCXref` — always `== nodeId` by construction |
| `toNode` | `IonCXref` |
| `fromTime`, `toTime` | `IonCXref` — absolute, never rewritten |
| `xmitRate` | `IonCXref` — bytes/second |
| `confidence` | `IonCXref` |
| `owlt` | `IonRXref`, via a join |

No bookkeeping fields: no `origin` (it is always me), no `owned`, no `lastHeard`. A single validity timestamp for the whole snapshot.

This structure solves problems 1.1 and 1.2 together: what is announced is literally what ION holds, field by field.

### 3.3 No third structure

A registry of "what I inserted myself" is not needed. Targeted removal by exact `fromTime` is sufficient (§6.2). Received contacts simply pass through ION; dtnex keeps no in-memory copy of them.

### 3.4 Contacts with no range

A contact with no local range **is not announced**, with a debug-level log. Reason: CGR discards a contact with no range from consideration as a next hop, so announcing it would occupy SDR and generate churn without ever producing a route.

On reception there is **no** symmetric filter: the v3 format always carries the `owlt` field and `0` is a legitimate value (§6.1). The filter lives entirely in origination.

An intentional side effect: a configuration error (a contact with no range in ionrc) becomes visible instead of propagating silently.

---

## 4. Authority rule: `fromNode == me`

**A node announces only the contacts in which it is itself the `fromNode`. It rejects every received message declaring `fromNode == me`.**

### 4.1 Why not announce both directions

Although a typical ionrc configuration contains both directions, network coverage is already complete without doing so. On the link A↔B: A announces `A→B`, B announces `B→A`. Both directions reach the network, from different sources. Announcing them from both ends is redundancy, not extra coverage.

### 4.2 What is gained

- every direction has exactly one authoritative source → no conflict-resolution rule
- no write dtnex performs can fall into the origination set → **TTL-only cache**, with no invalidation on write
- no `owned` flag in the structure, no state persisted across restarts

### 4.3 What is given up

Coverage of `B→A` when B's dtnex is down while its ION is up, or when B does not run dtnex at all. In that case the network sees the link only half-way.

### 4.4 Escape hatch

Should that coverage be needed one day: contacts with `toNode == me` are announced as well, and a precedence rule is added on reception — *the announcement with `origin == fromNode` wins*. The authoritative source beats the bounced one, so there is no flapping.

**It does not change the CBOR message format.** It relaxes validation check no. 5 (§6.1). It is a switch that can be flipped later, not a choice that must be locked in now.

### 4.5 Received contacts with `toNode == me`

**They are inserted into ION.** Without them the node has no return routes: it would know `me→B` but not `B→me`.

The rule is: write what you learn, announce only what you are authoritative for. The two are independent.

---

## 5. Message format

### 5.1 Envelope: unchanged

It stays a 9-element CBOR array: `[version, type, timestamp, expireTime, origin, from, nonce, payload, hmac]`. Nonce, HMAC, dedup and forwarding are untouched.

`version` goes **from 2 to 3**. v2 messages are **discarded**, with a debug log: a mixed-version network would recreate exactly the inconsistency the redesign removes.

### 5.2 Contact payload: from 3 to 7 fields

| field | type on the wire | notes |
|---|---|---|
| `fromNode` | integer | directional, always `== origin` |
| `toNode` | integer | |
| `fromTime` | integer | absolute epoch, never rewritten |
| `toTime` | integer | absolute epoch |
| `xmitRate` | integer | bytes/second, as in ION |
| `confidence` | integer 0-100 | percentage |
| `owlt` | integer | seconds |

### 5.3 Encoding choices

**Absolute times, not durations.** This is the decision that structurally solves problem 1.1: the receiver never recomputes the window. It requires clock synchronisation between nodes, which ION already imposes for CGR — it is not a new requirement.

**Absolute `toTime`, not a delta from `fromTime`.** A delta would save ~3 bytes while introducing an asymmetry between the two ends. Clarity beats 3 bytes.

**`confidence` as an integer 0-100.** Forced: `include/ion/cbor.h` has no encoding for floats. The 0.01 granularity of ION's `float` is lost; in practice the values used are 1.0 for scheduled contacts and coarse values for predicted ones. Encoding the float as a raw byte string would be fragile across architectures.

**`regionNbr` does not go on the wire.** The receiver inserts into its own default region. Propagating it would risk the receiver trying to insert into a region it does not know, making the insertion fail. **A conscious limitation: dtnex stays single-region**, and must be documented as such.

### 5.4 Size budget and traffic

Envelope ~33-37 bytes, payload ~26-30 bytes → **roughly 60-67 bytes per message**, against the current ~50. `MAX_CBOR_BUFFER` stays at 128, with ample margin.

**Message volume unchanged.** Today: N contacts (`me↔B`) × N neighbours = N². Tomorrow: N contacts (`me→B`) × N neighbours = N². Moving to directional messages doubles nothing, because the opposite direction is announced by the other end. The volume grows only if ION holds more time windows for the same pair — in which case genuinely more information is being propagated.

---

## 6. Reception and writing to ION

### 6.1 Validation pipeline

In order, before touching ION. Every failure: **discard without inserting and without forwarding**.

| # | check | reason |
|---|---|---|
| 1 | `version == 3` | a mixed-version network recreates the inconsistency |
| 2 | valid HMAC | unchanged |
| 3 | nonce not duplicated | unchanged |
| 4 | `origin != me` | unchanged (`dtnex.c:3196`) |
| 5 | `fromNode == origin` | only the source announces its own direction |
| 5b | `fromNode != toNode` | a contact towards oneself is the semantics of *registration contacts*, not topology; origination already filters it, but a peer holding the key could inject one |
| 6 | `toTime != 0` | in ION `toTime = 0` means *discovered contact* → `MAX_POSIX_TIME`, i.e. permanent (`rfx.h:53-56`) |
| 7 | `fromTime < toTime` | integrity |
| 7b | `fromTime <= 0` | in ION `fromTime = 0` means *hypothetical contact* |
| 7c | `fromTime >= MAX_POSIX_TIME` | in ION this is what triggers *registration contacts* |
| 7d | `xmitRate == 0` | ION refuses the contact with user error 5 |
| 7e | `confidence > 100` | outside the 0-100 range: ION refuses with user error 4 |
| 8 | `toTime > now` | already expired: pointless to insert and flood |
| 8b | `fromTime <= now + 30 days` | window too far in the future: suspected clock skew (§7.6) |

**There is no check on the `owlt`.** The first draft included one ("`owlt` present and valid"), but `owlt = 0` is a legitimate value — on a LAN it is the physically correct one, and ION accepts `a range ... 0`. The check contradicted origination, where `findOwlt` returns 0 as a valid value, and on a local testbed it would have made every receiver discard every contact. The v3 format always carries the field and §3.4 guarantees that only contacts for which a range really exists are announced: the receive-side check was redundant.

A message discarded by this pipeline returns **0**, not -1: the discard is a policy decision, not a decoding failure — the message decoded correctly, we simply decided not to apply it. A -1 would travel back up to `decodeCborMessage` and produce a misleading "unknown bundle format" for a perfectly valid message.

Check 5 makes an explicit `fromNode == me` check superfluous: such a message would already fall at check 4. It is the rule that gets relaxed when the escape hatch is enabled (§4.4).

Check 6 is not theoretical: a malformed message triggering that semantics would insert an eternal contact, flooded to the whole network.

### 6.2 Writing the contact

Identity: `K = (local regionNbr, fromNode, toNode, fromTime)`.

| state in ION | action |
|---|---|
| `K` does not exist | `rfx_insert_contact` |
| `K` exists, everything identical | **no-op** |
| `K` exists, only `xmitRate` / `confidence` change | **`rfx_revise_contact`** — in place |
| `K` exists, `toTime` changes | `rfx_remove_contact(&fromTime)` + `rfx_insert_contact` |
| `K` does not exist but the window overlaps another local contact | ION refuses with code 9, the local one is kept, it is logged at debug level: **this is not a failure** |

**The line that solves problem 1.3 is a single one: `&fromTime` instead of `NULL`.** With `NULL`, ION applies the `*` scope and deletes every contact between the pair; with a pointer to the exact `fromTime` it touches only the one actually being updated.

The first three rows solve the churn: today *every* received bundle performs a remove+insert. Under these rules the periodic refresh — the dominant case — does not touch ION at all.

`rfx_revise_contact` (`include/ion/rfx.h:101`) is keyed on `(regionNbr, fromTime, fromNode, toNode)`, exactly the identity tuple.

### 6.3 Writing the range

Same structure, identity `(fromNode, toNode, fromTime)`, but **`rfx_revise_range` does not exist**: absent → insert; identical → no-op; different → `rfx_remove_range(&fromTime)` + insert.

Contact and range are inserted **in the same pass, from the same message**. This is why `owlt` lives inside the contact message instead of having a separate `RangeMessage` type: the pair can never arrive halved, leaving a contact without a range (useless to CGR) or an orphan range.

> Recorded trade-off: in "real" ION, ranges often have longer windows than contacts, because propagation delay is a property of geometry and stays valid while the link is down. For dtnex's current use — announcing reachability between neighbours — coupling them is simpler and more robust. A reversible choice.

### 6.4 Existence check

The headers expose no `rfx_find_*`: the check requires walking the RBT over `ionvdb->contactIndex` and `ionvdb->rangeIndex`, the same pattern as `getContacts` (`dtnex.c:1120-1122`).

This is not a hot path: one message per neighbour per `updateInterval`, plus the floods.

A **reusable walking primitive** is needed, because the same walk is used to build the `myContacts` snapshot. One primitive, two callers — not a single function doing both jobs, otherwise the cache gets re-coupled to the writes.

### 6.5 Forwarding

Unchanged: a message that passes validation is inserted and then forwarded to every neighbour except `origin` and `from`. A discarded message is not forwarded.

Contacts with `toNode == me` follow the general rule: they are inserted **and** forwarded. They are useful topology for the rest of the network too.

### 6.6 Detecting an ION restart

`getContacts` today does double duty: it prints the diagnostic table **and** detects a restart with the rule *"zero contacts → ION restarted → restart dtnex"* (`dtnex.c:1192-1194`).

That rule becomes harmful in the new design: a freshly started node, or an edge node with no contacts configured yet, legitimately has zero contacts and would restart in a loop.

**It must be replaced** with explicit detection: the `ownNodeNbr` field of the IonDB, or the failure of the SDR transaction, which is already handled at `dtnex.c:1100-1106`.

It is a pre-existing, unrelated bug, but the redesign makes it actively harmful: included in scope.

---

## 7. Lifecycle, caching, concurrency

### 7.1 When an announcement happens

Three triggers, instead of the current two:

1. `updateInterval` has elapsed
2. the `plans` list changed (already the case today)
3. **the `myContacts` snapshot changed** with respect to the previous one

The third is new: the current snapshot is compared against the previous one. It is needed because today a contact added to ionrc is discovered by the network only within `updateInterval` (up to 30 minutes).

**Cadence:** the comparison happens at every refresh of the `myContacts` cache, so the responsiveness of trigger 3 is governed by the cache TTL, not by `updateInterval`. It is the TTL that sets the compromise between responsiveness to ionrc changes and the frequency of ION accesses.

### 7.2 Change in the meaning of `contactLifetime`

Today `contactLifetime` determines the lifetime of the announced contact (`dtnex.c:748`). In the new design the lifetime comes from ION: **that parameter no longer governs anything about contacts**.

The envelope's `expireTime` becomes the `toTime` of the contact itself — the message is useful exactly as long as the contact it describes is valid. `contactLifetime` remains in use only for metadata messages.

**This is a behavioural change visible to anyone with a `dtnex.conf` in production**: after the upgrade, the lifetime of announced contacts no longer depends on the configuration but on ionrc. It belongs in the changelog.

### 7.3 Soft state and expiry

Contacts remain soft state, with a new property that follows directly from absolute times: **expiry is consistent across the whole network**. The contact `A→B` expires at the same instant on A, on B and on every node reached by the flooding. Previously each hop recomputed its own window and the same piece of information died at different moments.

The periodic refresh re-announces the same absolute window → by the rules of §6.2 it is a no-op.

**Accepted limitation, to be documented: there is no revocation.** If the operator deletes a contact from the local ION, the remote copies stay until their `toTime`. A withdraw message would open up authentication problems and the flooding of deletions: out of scope. Natural expiry is a sufficient safety net.

### 7.4 Caching

**TTL only, no invalidation on write**, for both `plans` and `myContacts`, using the mechanism already in place.

Correctness rests entirely on the `fromNode == me` rule (§4): no write dtnex performs on ION can fall into the origination set. **This must be written as a comment in the code next to the cache**: if anyone relaxes that filter without noticing, the cache silently becomes wrong.

The TTL value is not a micro-optimisation: it governs the responsiveness of trigger 3 in §7.1. A short TTL discovers ionrc changes sooner at the cost of more ION accesses; a long TTL does the opposite. It must be chosen with that trade-off in mind, not copied from the 20 seconds of the plan cache without thinking it through.

### 7.5 Concurrency

**`myContacts` is single-threaded.** Only the main loop touches it, during origination. The reception thread performs its own RBT walks for the existence check, on a separate primitive that does not write into the array. **No new mutex**; protection for concurrent reads of ION is already provided by the SDR transaction.

**`plans`, on the other hand, has a race that already exists today.** `getplanlist()` writes into the statics `cachedPlans[]` / `cachedPlanCount` and is called both from the main loop and from the reception thread via `forwardCborContactMessage` (`dtnex.c:3358`). It is not introduced by the redesign, **but the redesign fixes it**: it is one mutex on a single function, with the unlock on every exit path.

The previous text of this section said the race stayed open while also being "in scope" — two incompatible statements. The second one holds: it gets fixed.

### 7.6 Errors and diagnostics

**Insertion failures.** The `rfx_*` calls return `-1` on a system error and `> 0` on a user error. The two classes must be kept apart, because they mean opposite things.

The existence check of §6.4 looks up an **exact** `fromTime`: by construction it cannot detect overlaps. A user error is therefore not merely possible but **expected in steady state**: in a conventional bidirectional ionrc both nodes declare both directions with relative times, so the `A→B` contact that B announces overlaps the one B itself has already configured locally, but with a different absolute `fromTime`. `rfx_insert_contact` refuses it with code 9 ("overlapping contact ignored"), and that is the correct behaviour: the operator-configured entry is kept.

Rules:

- **`rc < 0`** is the only anomaly: logged at non-debug level, outcome `IONC_ERROR`.
- **`rc > 0`** is an expected local condition: logged **at debug level** with the *meaning* of the code (9 = overlap with a local entry, 7 = region mismatch, 5 = zero xmitRate, 4 = confidence out of range, 2 on revise = target contact not Scheduled), outcome `IONC_NOOP`, **no early return**: the range write proceeds regardless, because a refusal on the contact must not stop ION from learning the OWLT.
- **`rfx_insert_range` returning 1** is not even a user error: ION's own source comments it as *idempotent* (the range is already there with the same `owlt`). It is a success, outcome `IONC_NOOP`.
- `*cxaddr` / `*rxaddr` serve as a cross-check on the refusal (`rfx.h:62-63`), with the exception of the codes emitted by the conflict scan (contact insert 8 and 9, range insert 1 and 2), where ION deliberately leaves the address of the conflicting entry.

**Clock skew.** Absolute times presuppose synchronised nodes. ION already requires this for CGR, but the failure becomes visible: a node whose clock is an hour behind would see every other node's contacts fall at check 8 and would stay isolated with no explanation.

Mitigation: when a message is discarded because its window lies entirely in the past **or entirely too far in the future**, log it explicitly as suspected clock skew. It costs one line and turns a mute failure into a diagnosable one.

**ION disconnecting mid-operation.** Unchanged: the existing handling on SDR transaction failure (`dtnex.c:1100-1106`) covers the case, and it is the same one that in §6.6 replaces the "zero contacts = restart" rule.

---

## 8. Impact on the code

| item | change |
|---|---|
| `ContactInfo` (`dtnex.h:119`) | replaced by the 7-field structure (§3.2) |
| `DTNEX_PROTOCOL_VERSION` | 2 → 3 |
| `exchangeWithNeighbors` (`dtnex.c:674`) | rewritten: source is `myContacts`, no longer the plans |
| `getplanlist` (`dtnex.c:498`) | role unchanged, mutex added (§7.5) |
| `getContacts` (`dtnex.c:1009`) | split up: the diagnostic printout stays, restart detection moves out (§6.6) |
| new primitives | contact RBT walk, range walk, join for the `owlt` |
| `encodeCborContactMessage` | new payload (§5.2) |
| `decodeCborMessage` | new payload + validation table (§6.1) |
| `processCborContactMessage` (`dtnex.c:3190`) | rewritten: idempotent rules, `&fromTime` instead of `NULL` |
| `forwardCborContactMessage` (`dtnex.c:3358`) | logic unchanged, adapted to the new fields |
| `CLAUDE.md` | corrections applied (§10) |

### 8.1 Extracting `ion_contacts.c`

Everything that talks directly to ION is extracted into a separate file: RBT walks, the contact/range join, and the write rules of §6.2-6.3.

**Why the risk is low:** this is not "move it and then change it". Reading `myContacts` and the join are new code; the write rules are a complete rewrite of `processCborContactMessage`, of which almost nothing survives. The only thing genuinely moved is the RBT walk, some twenty lines from the `getContacts` pattern.

**Concrete payoff:** tests 1, 4 and 5 of the validation plan (§9) exercise this module alone, against a live ION, with no bundles and no network.

**A boundary to respect:** the module exposes three operations —

1. read the set of announceable contacts (filter `fromNode == me`, join with the ranges)
2. apply a received contact+range (idempotent rules)
3. print the diagnostic table

— and it **knows nothing about CBOR, bundles, HMAC, neighbours or flooding**. If encoding slips in because "it's contact stuff", the module stops being testable on its own and the only reason it exists is lost.

---

## 9. Validation

There is no test suite: validation is manual, in `--debug`. Every test is mapped onto a specific problem, so that the verification reads "this problem is closed" and not "it seems to work".

| # | test | verification |
|---|---|---|
| 1 | Single node | The `myContacts` snapshot matches ionadmin's `l contact` / `l range` field by field → **problem 1.1** |
| 2 | Two nodes | The contact inserted on B has the same `fromTime`, `toTime`, `xmitRate`, `owlt` that A read from ION; no rewriting of the start time → **problem 1.1 end-to-end** |
| 3 | Two nodes | The `owlt` on B matches the range configured on A, not 1 second. A contact with no range on A → not announced, and the log says so → **problem 1.2** |
| 4 | Targeted regression | A contact for the pair is configured by hand via ionadmin, then a message arrives for the same pair with a different `fromTime`. **The manual contact must survive** (today it disappears) → **problem 1.3** |
| 5 | Anti-churn | Two consecutive update cycles with no changes: the second produces no `rfx_insert` or `rfx_remove` → **churn** |
| 6 | Three nodes (if the testbed is available) | The same contact propagated over two hops has identical `fromTime`/`toTime` on all three → **property of §7.3** |

---

## 10. Documentation corrections

`CLAUDE.md` contained three verified inaccuracies, all corrected:

- it declared the type 1 payload as `[nodeA, nodeB, duration_min, datarate_bps, reliability]`; it now documents the new design's 7-field payload (`[fromNode, toNode, fromTime, toTime, xmitRate, confidence, owlt]`)
- it declared the data rate in **bits** per second; it now says explicitly `xmitRate` in **bytes** per second, as in ION (`include/ion/ion.h:203`)
- it labelled the two messages as "Type 1" and "Type 2", as if `type` were an integer on the wire; the labels are now "Type \"c\"" and "Type \"m\"", consistent with the field's actual value (a text string, see `README.md`)

---

## 11. Recorded decisions and their reversibility

| decision | reversible? |
|---|---|
| Absolute times on the wire | No — it is the foundation of the design |
| Directional messages (one per direction) | No — it determines the structure and the authority rule |
| Origination filter `fromNode == me` | **Yes** — via the escape hatch of §4.4, without touching the message format |
| Inserting contacts with `toNode == me` | Yes — independent of what is announced |
| `owlt` inside the contact message, no `RangeMessage` | Yes — §6.3 |
| Single-region, `regionNbr` outside the protocol | Yes — requires a version bump |
| `confidence` as an integer 0-100 | Constrained by `cbor.h`, not a choice |
| No `owned` flag, no local registry | A consequence of absolute times |
| No explicit contact revocation | Yes — would require a new message type |

---

## 12. Known limitations that emerged during implementation

Three open points that the final review of the branch found and that are **not**
fixed in the pre-merge wave of fixes. They are recorded here so as not to lose
them, not to be solved right now.

### 12.1 ION restart detection is insufficient

§6.6 prescribes `ownNodeNbr` or the failure of the SDR transaction. But an
`ionstop && ionstart` cycle **preserves** `ownNodeNbr`: the most common restart goes
undetected. An instance marker is needed — for example the identity of the working
memory partition, or the `PsmAddress` of `contactIndex` recorded at startup, which a
fresh vdb reallocates. This requires a design decision.

### 12.2 The symmetric range join weakens §3.4

`findOwlt` accepts the opposite direction too, and `ionc_apply_contact` inserts a range
`(origin → me)` for every accepted contact: that range is a valid candidate for the
local contact `(me → origin)`, so a local contact **with no** range can still be
announced using an OWLT learned from a peer. §3.4 promised that an `ionrc`
configuration error would become visible; here it can be silently covered up.

The review verified that this produces neither churn nor oscillation: the value is
deterministic and in a symmetric configuration the two OWLTs coincide. The cost is the
lost diagnostics, not instability.

Future mitigation: prefer the exact direction and fall back to the opposite one only in
its absence. It should also be noted that the cache invariant written in `dtnex.c` holds
for the contact's identity fields but **not** for `owlt`.

### 12.3 The ION headers in `include/ion/` do not match the installed library

They come from a different release: `IonRegion`, `IonDB`, `IonNode` and `IonContact`
diverge, and `MAX_POSIX_TIME` is 2147397247 in the bundled headers against 2147483647
in the installed ones. The review verified that the layouts this branch depends on —
`IonCXref`, `IonRXref`, `IonVdb`, the offset of `ownNodeNbr` in `IonDB` — **do match**,
so the module is safe today; but commit `87590d4` on this very branch exists precisely
because one of these divergences had silently corrupted the RBT reads.

Future mitigation: resynchronise the headers, or add a couple of `_Static_assert`s in
`ion_contacts.c` on `sizeof(IonCXref)`, `offsetof(IonCXref, fromTime)` and
`sizeof(IonVdb)`, to turn the next mismatch into a compile error rather than mute
corruption.

---

## 13. Termination and SDR transaction integrity

**Added on 2026-08-08**, after running the §9 tests against a live ION. It was not
part of the contact-exchange redesign: it surfaced while validating it.

### 13.1 Why termination is being touched

During the tests the ION node locked up several times: `ionadmin` and `bplist` stuck for
minutes on a shared-memory semaphore, while `sdrwatch` — which does not take the
transaction lock — kept responding. One measurement in particular: `ionadmin`, stuck for
two minutes, unblocked **two seconds after dtnex was stopped**. On another occasion
dtnex did not respond to SIGTERM at all.

The mechanism that explains the observation lives in `signalHandler` (`dtnex.c:995-1069`).
The handler does not merely signal termination: it logs, calls `bp_interrupt`, performs
`pthread_join`, `bp_close`, `bp_detach`, and finishes with `exit(0)`. The signals are
installed with `sigaction` in `main` before any thread is born (`dtnex.c:1679-1685`),
so **the handler can run on any thread whatsoever**. If the signal arrives while a
thread is inside an SDR transaction — the main loop in `ionc_get_own_contacts`, the
reception thread in `ionc_apply_contact`, `getplanlist` — that `exit(0)` terminates the
process with the transaction open. The transaction lock lives in ION's shared memory,
not in the process: it stays held, and every other ION client blocks until an
`ionunlock` or a `killm` arrives.

**This is a hypothesis consistent with the observations, not a proof:** the instant of
the signal was never captured. But it is enough to justify the change, because the
defect is visible statically and independent of the episode: an `exit()` called from
asynchronous context while another thread may be inside a transaction is incorrect
regardless.

For clarity: **v2.52 has the same defect** — same handler, same three signals, same
`exit(0)`. No lost behaviour is being restored; what is being closed is an exposure the
shell version did not have, because it talked to ION only through `ionadmin`, and every
invocation opened and closed its own transaction.

### 13.2 Why the asynchronous handler is removed rather than slimmed down

The minimal solution would be to reduce the handler to `running = 0` and move the
teardown into the main loop. That solves the lock problem, and on its own would suffice.

We go further for an independent reason: **almost everything the handler does today is
not async-signal-safe.** `dtnex_log` is `printf`; `pthread_join` and `bp_close` are not
in the POSIX list of functions callable from a handler. A `printf` interrupted halfway
by another `printf` can deadlock on stdio's internal lock: a deadlock that leaves no
trace, and which would explain the dtnex that stayed deaf to SIGTERM. Slimming the
handler down would leave the whole category open, ready to reappear the next time
somebody adds "just one log line" in there.

The `sigwait` pattern closes it at the root. The three signals are blocked in every
thread and a dedicated thread collects them with `sigwait()`. That thread **is not a
handler**: it is ordinary code in ordinary context, where logging, joining and calling
the ION API are all legitimate. No function is left that must be kept
async-signal-safe, hence no rule that a future contributor could break without
noticing. The cost is a signal mask to set before creating any thread — a constraint
verifiable in a single place, against an invariant spread over every line of the
handler.

### 13.3 Structure of the termination

1. In `main`, **before** creating any thread: `pthread_sigmask(SIG_BLOCK, …)` over
   SIGINT, SIGTERM, SIGTSTP. The mask is inherited by every thread created afterwards.
2. A dedicated thread is born that loops on `sigwait()` over those same three signals.
   The `sigaction` calls at `dtnex.c:1679-1685` disappear, and with them `signalHandler`
   (`dtnex.c:995-1069`) and its declaration in `dtnex.h:168`.
3. **First signal:** the thread logs, clears `running`, `bpechoState.running` and
   `bundleReceptionState.running`, then issues the wake-ups — `bp_interrupt` on the two
   SAPs and `ionPauseAttendant` — to unblock whoever is parked in `bp_receive`. The
   wake-ups are conditional as in the current handler (`ionConnected` and a non-null
   SAP), because dtnex can receive a signal while ION is unreachable. It does not join,
   does not close endpoints, does not detach, does not call `exit`. It returns to
   `sigwait`.
4. **Teardown:** what is already there. `main` contains the complete sequence —
   `stopBundleReception`, joining the two threads, `bp_close`, `bp_detach`, `return 0` —
   at lines `dtnex.c:1761-1788`. Today it is **unreachable code**, because the handler
   calls `exit(0)` before `eventDrivenLoop` returns. The work is not to write the
   teardown: it is to stop bypassing it.

The guarantee is structural, not left to attention: `eventDrivenLoop` checks `running`
only between iterations, so the exit point is by construction outside any transaction.
No new wake-up mechanism is needed — the loop already sleeps in one-second slices while
checking `running` (`dtnex.c:2365-2378`), so the response latency to a signal stays
below one second.

**A defect to fix in the existing teardown.** The two joins are guarded by
`if (bundleReceptionState.running)` and `if (bpechoState.running)` (`dtnex.c:1765`, `1772`).
Those are the very variables the `sigwait` thread clears in order to ask the services to
stop: by the time the guard is evaluated they are already zero, and **the joins get
skipped**. It went unnoticed so far, because that code was never reached. We need to
distinguish "the thread was created" from "the thread should keep running": two separate
flags, or an unconditional join over the threads actually created. Without this,
cooperative termination would close the SAPs while the service threads are still using
them — replacing one defect with another.

The teardown in `main` closes `sap` but not `bpechoState.sap`, which the handler used to
close for safety. Verified: it is not needed, because **the bpecho thread closes and
clears it itself on the way out** (`dtnex.c:1523-1526`). That is another reason the join
must genuinely happen: if it is skipped, the SAPs get closed while that thread is still
running its own cleanup.

### 13.4 Forced exit

The second signal keeps the escape route that exists today, but stops being silent: it
explicitly logs that the exit is forced, that the SDR transaction may stay open and that
the remedy is `ionunlock ion`. Then `_exit(1)`, not `exit(1)`: with other threads still
alive we do not want to run the `atexit` handlers or flush stdio.

Forcing the exit is exactly what can leave the lock held. It stays available because a
stuck operator must be able to get out, but they must know what it cost them.

### 13.5 Invariant to preserve

Every `sdr_begin_xn` has its `sdr_exit_xn` / `sdr_end_xn` on **all** exit paths. That is
true today — verified in `ion_contacts.c` and in `getplanlist` — and must be recorded as
an invariant, not as an observation. It holds for the re-exec on ION restart as well
(`dtnex.c:2422`): we do not re-exec with a transaction open.

### 13.6 `planListMutex`: no action, and why

`planListMutex` (`dtnex.c:511`) is a `pthread_mutex_t` **local to the process**:
`ionadmin` is a different process and cannot see it. It cannot be the cause of what was
observed, and there is no "recovery unlock" to add. With cooperative shutdown, a thread
that happens to be inside `getplanlist` when `running` goes to zero reaches the end of
the function and unlocks by itself.

This section exists to stop the idea from being reintroduced later: the two locks —
a process mutex and the shared SDR transaction — are easy to confuse, and only the
second is the one that blocks other ION clients.

### 13.7 Scope and limits

The same modes of death as v2.52 are covered: **SIGINT, SIGTERM, SIGTSTP** and ordinary
exit. Segfault, abort and SIGKILL stay out: there the process dies without executing
anything, and if a transaction was open the lock stays held. The documented remedy is
`ionunlock ion`.

Covering those would require recovery at startup — dtnex detecting a stale lock and
releasing it — which brings with it the risk of unlocking the transaction of another
legitimate ION process. Deliberately out of scope.

### 13.8 Verification — PERFORMED on 2026-08-08

Manual, like the rest: there is no suite. Implemented in commits `fa12820`,
`b9f0ce3`, `3c2b56c`, `b59b059`.

| # | test | outcome |
|---|---|---|
| 1 | SIGTERM during normal operation | ✅ exit 0.51 s after the signal, full teardown executed (both joins, `bp_close`, `bp_detach`), `DTNEXC terminated normally` — the final line of `main`, which never appeared with the old handler. `ionadmin` immediately afterwards: 7.6 ms |
| 2 | SIGTERM while a received contact is being applied | ✅ repeated 4 times with the signal at 0.2 / 0.6 / 1.0 / 1.5 s after injection. In all of them: contact applied (`contact=inserted range=inserted`), orderly exit, `ionadmin` between 7.9 and 9.9 ms |
| 3 | double signal | ✅ the forced-exit warning is present and names `ionunlock ion` |
| 4 | SIGTERM in service mode | ✅ same outcome as test 1 |

### 13.9 The original symptom was NOT solved

**§13.1 also attributed to this defect the `ionadmin` lock-up observed while dtnex was
running. That attribution is wrong, and the tests disproved it.**

The four tests above all measure the *after-exit* case. An additional test, not foreseen
by the spec, measured the original case — `ionadmin` queried **while dtnex is running**,
with the correct binary:

- `ionadmin` blocked for **2 minutes and 13 seconds**, never completing;
- unblocked **1.0 seconds after** the SIGTERM to dtnex;
- on the same node, in the same minutes, queries with dtnex stopped returned in
  8-10 ms.

During the lock-up dtnex's main thread was in `nanosleep` — sleeping, holding no
transaction — the `sigwait` thread was correctly in `do_sigtimedwait`, and the two
service threads were parked on semaphores inside `bp_receive`.

**But "it is dtnex's fault" is not proven either.** Later measurements, also from
2026-08-08 and on a freshly restarted node, show `ionadmin` blocked for **over ten
minutes with dtnex completely stopped**, only to complete on its own. Repeated several
times. So the node has multi-minute stalls of its own, independent of dtnex, and the
inference "blocked while dtnex runs ⇒ dtnex blocks it" does not hold: unblocking 1.0 s
after the SIGTERM remains suggestive, but with stalls of such variable duration it may
be a coincidence.

**What can be asserted, and what cannot.**

| statement | status |
|---|---|
| §13 fixes a real defect (an asynchronous `exit()` with a potentially open transaction is incorrect) | ✅ established, and verified by the four tests of §13.8 |
| After an orderly exit the node stays usable (`ionadmin` in 8-10 ms, 5 measurements) | ✅ established |
| §13 solves the `ionadmin` lock-up observed with dtnex alive | ❌ **disproved**: the lock-up reproduces with the correct binary |
| The lock-up is caused by dtnex | ❓ **not proven**: it reproduces with dtnex stopped too |
| The lock-up is caused by a thread parked in `bp_receive` | ❓ unverified hypothesis — the shape is the same for both dtnex's service threads and the operator's `bprecvfile`, but the decisive experiment (stopping `bprecvfile` and seeing whether the lock-up ceases) was never run |

**How to resume.** Clean measurements on an undisturbed node are needed, one variable at
a time: (a) freshly restarted node, no clients, N timed queries; (b) the same node with
`bprecvfile` only; (c) the same node with dtnex only; (d) both. Without that baseline,
every single observation is anecdotal — including the one that gave rise to this
section.

**Operational warning.** Killing `ionadmin` while it is waiting genuinely blocks the
node, and skews every subsequent measurement. It must be launched only where no timeout
can interrupt it. Several measurements from that day were invalidated in exactly this
way.
