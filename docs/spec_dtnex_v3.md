# DTNEX v3 — Contact exchange design

**Protocol version:** 3 (from 2)

This document describes what version 3 of DTNEX changes and why. It is organised in
two parts: the defects of the previous version that motivated the redesign, and the
design adopted to solve them.

---

## 1. The problems

Three defects of version 2, all in the mechanism that decides *what* a node announces
and *how* it applies what it receives.

### 1.1 The contact that is sent is not the one that is stored

DTNEX exists to propagate ION's contact plan, but version 2 never read it. What it
announced was built synthetically from the **plans** — the convergence-layer
adjacencies, i.e. "I have an outduct towards X" — using the local node as one endpoint,
a neighbour as the other, and a duration taken from `contactLifetime` in the
configuration file.

The function that genuinely walked ION's contact database was purely diagnostic: it
printed a table and counted entries, and its output fed nothing. The two worlds never
touched. A contact configured in `ionrc` with a specific transmission window was
therefore announced to the network with a completely different window, and a node with
no contacts configured at all still announced one per neighbour.

A second source of divergence sat on the receiving side: the timestamp carried by the
message was discarded and the start time rewritten to the receiver's local clock.
Propagated over multiple hops, the same contact took on a different absolute window on
every node it reached.

### 1.2 The range was fabricated

The OWLT of every inserted range was hardcoded to 1 second. Ranges were never exchanged
between nodes, so a receiver had no way of knowing the real propagation delay of a link
it was learning about. Two other parameters were fabricated in the same way at
insertion time: the transmission rate and the confidence value.

For CGR this is not a cosmetic detail. Delivery time estimates are computed from the
OWLT, and a link with a multi-second delay announced as 1 second produces routes whose
timing is wrong.

Version 3 as first written closed half of this: the OWLT stopped being invented, but it
travelled inside the contact message and a range was written as a side effect of applying
a contact. It is now closed on both sides. The range is an entity of its own — read from
ION's range index, carried in its own message type `"r"`, validated and applied on its
own — and no code path derives a range from a contact any more.

### 1.3 Removal was indiscriminate

Every received contact was applied by removing and reinserting, and the removal was
issued with a null timestamp — the equivalent of ionadmin's `*` scope. That deletes
**every** contact between the pair of nodes, including the ones configured by hand by
the operator, which DTNEX had never inserted and had no business touching. The same
applied to ranges.

Because this happened on every received bundle, re-floods included, the contact plan
was in continuous churn, and every rewrite invalidated the CGR routes that depended on
it.

---

## 2. Guiding principle

Version 3 separates three notions that version 2 conflated:

| concept | meaning | role |
|---|---|---|
| **plan** | "I have an outduct towards X" | transport: who I send bundles to |
| **contact** | "there is a transmission window X→Y" | payload: the topology I announce |
| **range** | the OWLT between two nodes | payload: announced on its own, in its own message |

In ION, plans and contacts are configured independently of each other. Treating the
first as a source of information about the second is the root cause of problem 1.1.

---

## 3. What a node announces

### 3.1 ION is the source of truth

The set of announceable contacts is read from ION and nothing else: it is a read-only
snapshot, refreshed on a TTL, with no local state and no bookkeeping. Every field
travels as ION holds it — the absolute start and end times, the transmission rate in
bytes per second, the confidence, the number of the region the contact belongs to.

Ranges are read the same way, from ION's own range index, as a second snapshot with the
same TTL and its own message type. A contact is announced whether or not a range exists
for the same pair: the two are separate entities in ION, and they are separate on the
wire too.

This is what closes problems 1.1 and 1.2 together: what is announced is literally what
ION holds, field by field.

### 3.2 Authority rule: a node announces only its own direction

**A node announces only the contacts and ranges in which it is itself the transmitting
node, and rejects any received message claiming to describe a direction that originates
at the receiver.**

The rule governs both message types identically: a node announces the contacts and the
ranges whose `fromNode` is itself, and discards any `"c"` or `"r"` whose `fromNode` is
not the origin of the message that carries it.

Coverage of the network is complete without announcing both directions. On the link
A↔B, A announces `A→B` and B announces `B→A`: both directions reach the network, each
from the node that is authoritative for it. Announcing both from both ends would be
redundancy, not extra coverage.

What this buys is the absence of an entire class of problems. Every direction has
exactly one authoritative source, so there is no conflict resolution to define, no
precedence rule between two nodes describing the same link differently, and no need to
track which entries DTNEX inserted itself.

What it gives up is coverage of `B→A` when B's ION is up but its DTNEX is not running.
In that case the network sees the link only half way. Should that coverage ever be
needed, it can be added without changing the message format: contacts towards the local
node would be announced as well, with a precedence rule on reception stating that the
announcement coming from the authoritative source wins.

Received contacts and ranges whose *destination* is the local node are a different
matter, and they **are** inserted: without them the node would know `me→B` but not
`B→me`, and would have no return routes. The rule is: write what you learn, announce
only what you are authoritative for.

### 3.3 Asserted and imputed ranges: only the asserted ones are announced

For ranges the authority rule is necessary but not sufficient, and the way it falls
short damages the receivers rather than the announcer.

**A range in ION has one of two natures**, told apart by `IonRXref.rangeElt`. A range
somebody declared is *asserted*: an assertion object is stored for it and `rangeElt`
points at it. A range ION derived by itself is *imputed*: it exists only as an entry in
the range index, and ION marks it by zeroing that field — `rxref2->rangeElt = 0; /*
Indicates "imputed". */`, literally, in `rfx.c`. ION derives the reverse range on
insertion, and it does so **only** for *canonical* assertions, those with
`fromNode < toNode`, on the assumption that the OWLT between two nodes is symmetric.

**What that does to a range we learned.** Take three nodes — 1, 2 and 3 — with the
asymmetric configuration that is the normal case rather than a pathological one: the
range between 1 and 3 is declared in node 1's `ionrc` and nowhere else. Node 1 asserts
`1→3`, which is canonical, and its own ION imputes `3→1` beside it. Node 1 announces
`1→3`, and nodes 2 and 3 both learn it and insert it; both their IONs impute `3→1` in
turn. On node 3 that imputed record has `fromNode == 3`, the local node, so the authority
rule alone is satisfied by it: without a further filter node 3 would announce `3→1` to
node 2 as though it were the authoritative source of a range its operator never
configured.

**Why the receiver takes such a record seriously.** For ION a range with
`fromNode > toNode` is not a harmless duplicate of the canonical one. It is a
*non-canonical assertion*: the explicit statement that between those two nodes OWLT
symmetry does **not** hold, so the reverse must not be derived from it. Applying such a
record therefore does not add a duplicate: ION deletes the imputed entry it had derived
and replaces it with an asserted one.

The receiver does not always get that far, and the difference matters. If the imputed
entry it already holds carries the same OWLT and the same end time as the announcement —
which is the case when both copies descend from the same canonical assertion — nothing is
written at all and the message is an idempotent no-op (§5.3). But any divergence is
enough for the insertion to happen for real: the reverse arriving before the canonical
assertion it derives from, so that the receiver has nothing to compare against; an OWLT
the receiver's own operator configured for that pair; or the first revision of the
window, when the reverse announcement precedes the updated canonical one. From that
moment the record is asserted, and stays so.

**Why the damage does not heal.** When a canonical range is removed, ION cleans up the
reverse it had derived from it — but it calls `deleteRange(rxaddr, 1)`, that is
`retainIfAsserted = 1`, and `deleteRange` returns without deleting anything when the
record is asserted. A reverse that stayed imputed therefore disappears together with the
canonical range it came from; a reverse that was promoted to asserted **outlives the
thing it was derived from**, on a node whose operator never declared it, and DTNEX has no
revocation (§7) with which to take it back.

**Hence the rule: `ionc_get_own_ranges` filters on `rangeElt != 0`.** A node announces
only what it has itself asserted, never what ION inferred on its behalf. This is the same
principle that makes `ionc_get_own_contacts` keep only `CtScheduled` and `CtPredicted`
contacts and exclude the `Discovered` ones: **what ION deduced by itself is not
propagated.**

The network converges all the same. Every receiver imputes the reverse locally, from the
canonical assertion it received, so the direction that is not announced is reconstructed
where it is needed. And when both endpoints do assert their own side, the second
announcement to arrive matches what the receiver already holds, so DTNEX recognises it
before calling ION at all and treats it as an idempotent no-op. The filter is therefore not a restriction to
be relaxed by a later reader who finds it excessive: removing it corrupts the range index
of every node downstream.

---

## 4. Message format

### 4.1 Envelope

Unchanged from version 2: a nine-element CBOR array carrying version, message type,
timestamps, origin, sender, nonce, payload and HMAC. Nonce handling, authentication,
duplicate suppression and forwarding are untouched. The type is a one-character string:
`"c"` for a contact, `"r"` for a range, `"m"` for node metadata.

The version field goes **from 2 to 3**, and version 2 messages are discarded. A
mixed-version network would recreate precisely the inconsistency the redesign removes.

### 4.2 Contact payload

From three fields to seven:

```
[regionNbr, fromNode, toNode, fromTime, toTime, xmitRate, confidence]
```

| field | type | notes |
|---|---|---|
| `regionNbr` | integer | ION region the contact belongs to; first, because it scopes every field that follows |
| `fromNode` | integer | directional; always equal to the message origin |
| `toNode` | integer | |
| `fromTime` | integer | absolute epoch, never rewritten |
| `toTime` | integer | absolute epoch |
| `xmitRate` | integer | bytes per second, as in ION |
| `confidence` | integer | percentage, 0–100 |

The OWLT is no longer here: it travels in the range message.

### 4.3 Range payload

Five fields:

```
[fromNode, toNode, fromTime, toTime, owlt]
```

| field | type | notes |
|---|---|---|
| `fromNode` | integer | directional; always equal to the message origin |
| `toNode` | integer | |
| `fromTime` | integer | absolute epoch |
| `toTime` | integer | absolute epoch |
| `owlt` | integer | one-way light time, in seconds |

**The range carries no region, and that is not an oversight.** In ION ranges are not
regional entities: `rfx_insert_range()` and `rfx_remove_range()` have no region
parameter, and `IonRXref` has no such field. Adding a region to the `"r"` message would
invent a scope ION does not have and that no call could use. The asymmetry between the
two payloads is ION's, not ours.

The identity key follows from the same fact: a contact is keyed by
*(region, fromNode, toNode, fromTime)*, a range by *(fromNode, toNode, fromTime)*.

### 4.4 Encoding decisions

**Absolute times rather than durations.** This is the decision that structurally solves
problem 1.1: the receiver never recomputes the window, so the same contact denotes the
same interval on every node that learns about it. It presupposes clocks synchronised
between nodes, which ION already requires for CGR — it is not a new constraint.

**The end time is absolute too, not a delta from the start.** A delta would save about
three bytes while introducing an asymmetry between the two ends of the message.

**Confidence as an integer percentage.** ION stores it as a float, but the CBOR
implementation available has no float encoding, and encoding the raw bytes of a float
would be fragile across architectures. The 0.01 granularity is lost; in practice the
values in use are 1.0 for scheduled contacts and coarse values for predicted ones.

**The region number travels, and no one checks it locally.** It is taken from ION on the
announcing side and handed straight to `rfx_insert_contact` on the receiving side. If it
names a region the receiver does not belong to, ION refuses the contact with user error
7, "contact is for a foreign region", logged at debug level like every other user error,
and the message is forwarded regardless: a local refusal must not partition the flooding.
The decision about a region is ION's, not DTNEX's — which is what makes this
pass-through, not multi-region support (§7).

**The range travels in its own message**, not inside the contact. Contact and range are
distinct entities in ION, with distinct keys, distinct API calls and — as above —
different notions of scope, and carrying the OWLT inside the contact forced every range
to borrow the contact's window and to be written as a side effect of applying that
contact.

The price is that the two can now arrive halved: a contact whose range has not arrived
yet, which CGR will not use until it does, or a range with no contact. Both are
transient, and both are repaired by the next announcement. In exchange, one rule from the
first version 3 disappears: a contact for which no range existed locally used to be
skipped and never announced at all. It is now announced regardless, and the range arrives
— or does not — on its own path.

### 4.5 Size

Both message types are far from the 128-byte buffer. On the testbed, a real contact
message came out at 50 bytes, and one built by an independent encoder at 48; those
figures reflect single-digit node numbers, which is what keeps the integers to one byte
each.

The worst case is bounded by the encoder rather than measured: with 64-bit node numbers,
a 32-bit region number and a 32-bit transmission rate, and epochs that still fit in 32
bits, the envelope reaches about 36 bytes and the whole message about 86 bytes for a
contact and about 80 for a range. `MAX_CBOR_BUFFER` stays at 128 with room to spare.

The **number** of messages grows by the ranges. For each neighbour a node sends one
message per announceable contact and one per announceable range. Directionality doubles
nothing, because the opposite direction is announced by the node at the other end;
traffic grows only if ION holds more windows for the same pair — in which case genuinely
more information is being propagated.

---

## 5. Applying a received message

### 5.1 Validating a contact

Every received message is validated before ION is touched at all, and any failure means
the message is discarded without being inserted and without being forwarded. The checks
cover, in order: protocol version, HMAC, nonce, the origin not being the local node,
the announced direction genuinely belonging to the announcing node, a `fromNode`
different from the `toNode` — a contact from a node to itself is a registration, not
topology — and a window whose start precedes its end.

The remaining checks reject windows that ION would interpret as something other than a
scheduled contact — a zero end time means a discovered, effectively permanent contact; a
zero start time means a hypothetical one; a start time at the maximum representable
value has yet another meaning — as well as values ION would refuse outright, such as a
zero transmission rate or a confidence above 100.

Two temporal checks close the set: an already-expired window is pointless to insert and
flood, and a window starting more than thirty days in the future is treated as
suspected clock skew.

### 5.2 Validating a range

A range message goes through the same discipline, in the same order, with the checks that
mean something for a range. There are nine: the origin is not the local node; `fromNode`
equals the origin, which is the authority rule; `fromNode` and `toNode` differ; the end
time is not zero; the window is ordered; the start time is greater than zero; the start
time is below the maximum representable value; the window has not already expired; and
the start time is no more than thirty days in the future. The checks on transmission rate
and confidence have no counterpart here — a range carries neither.

An OWLT of zero is **not** rejected. It is the physically correct value on a LAN and ION
accepts `a range ... 0`; a check rejecting it had already caused every receiver on a
local testbed to discard everything it was sent.

### 5.3 Idempotent writing

A contact is identified by the tuple *(region, fromNode, toNode, fromTime)*. Given a
validated message, the write follows from comparing it against what ION already holds:

| state in ION | action |
|---|---|
| the contact does not exist | insert |
| it exists and is identical | **no-op** |
| it exists, only rate or confidence differ | revise in place |
| it exists, the end time differs | targeted removal, then insert |
| it does not exist but the window overlaps a local contact | ION refuses, the local entry is kept, logged at debug level — this is not a failure |

Ranges are applied by a call of their own, on their own key
*(fromNode, toNode, fromTime)*, and follow the same structure — except that ION exposes
no in-place revision for them, so any change means targeted removal followed by
insertion, with one exception. The exception concerns the *imputed* ranges of §3.3, the
ones ION derived by itself from the canonical assertion in the opposite direction: over
an imputed range there is no assertion object to remove, and the insertion
performs the substitution on its own within a single transaction; removing it first would
split that substitution in two, leaving the pair without a current OWLT in between, and
would widen the exposure to the overlap check — over a key that is already present ION
skips the part of that check which looks at the successor, but not the whole of it.

That residue matters: over an imputed range ION deletes the index entry *before* it
reaches the overlap check, so a refusal there leaves the pair with neither the derived
entry nor the new one. That outcome is reported as a loss and logged unconditionally,
not as a no-op.

An imputed range that a message merely confirms is left untouched: what ION deduced by
itself is not promoted into an assertion of ours.

**The removal is always issued with the exact start time of the entry being replaced.**
This one detail is what closes problem 1.3: with a null timestamp ION applies the `*`
scope and deletes every contact between the pair, operator-configured entries included;
with an exact timestamp it touches only the entry actually being updated.

The first three rows of the table close the churn. Version 2 performed a remove and an
insert on *every* received bundle; under these rules the periodic refresh — by far the
dominant case, since it re-announces the same absolute window — does not touch ION at
all.

The overlap case in the last row is expected in steady state rather than exceptional. A
conventional bidirectional `ionrc` has both nodes declaring both directions, so a
contact announced by a peer will often overlap one the receiver already has configured
locally with a different start time. ION refuses it, the operator's entry survives, and
that is the correct outcome.

### 5.4 Forwarding

Unchanged, and identical for both message types: a message that passes validation is
applied and then forwarded to every neighbour except its origin and the node it was
received from. A discarded message is not forwarded. Forwarding does not depend on the
outcome of the local write — a contact ION refused is still forwarded, since a purely
local problem must not partition the flooding. Contacts and ranges whose destination is
the local node follow the general rule: they are inserted *and* forwarded, being useful
topology for the rest of the network too.

---

## 6. Behaviour over time

### 6.1 When an announcement happens

Four triggers instead of two:

1. the update interval has elapsed;
2. the list of plans has changed (already the case in version 2);
3. **the set of announceable contacts has changed** with respect to the previous
   snapshot;
4. **the set of announceable ranges has changed**, by the same mechanism.

The last two are new, and they are what makes a contact or a range added to `ionrc`
visible to the network promptly instead of within the update interval, which defaults to
thirty minutes. Their responsiveness is governed by the TTL the two snapshots share,
which is therefore a deliberate trade-off between reacting quickly to configuration
changes and accessing ION frequently.

### 6.2 `contactLifetime` no longer governs contacts

In version 2 this configuration parameter determined the lifetime of the announced
contact. It no longer does: the lifetime now comes from ION. The message's own
expiry becomes the end time of the contact it describes, so a message stays useful
exactly as long as the contact is valid. `contactLifetime` remains in use for metadata
messages only.

This is a behavioural change visible to anyone upgrading an existing installation: the
lifetime of announced contacts stops depending on `dtnex.conf` and starts depending on
`ionrc`.

### 6.3 Expiry is consistent network-wide

Contacts remain soft state, with a property that follows directly from absolute times:
the contact `A→B` expires at the same instant on A, on B and on every node the flooding
reached. In version 2 each hop recomputed its own window, so the same piece of
information died at different moments on different nodes.

---

## 7. Known limitations

These are deliberate, and are recorded rather than hidden.

**No multi-region logic.** The region number does travel and is handed to ION as it
stands (§4.4), but DTNEX holds no notion of which regions the local node belongs to, and
no handling of passageways. A contact naming a region foreign to the receiver is refused
by ION with user error 7, logged at debug level, and forwarded regardless. This is
preparation for multi-region operation, not multi-region operation.

**No revocation.** If the operator deletes a contact from the local ION, the copies
already propagated stay on remote nodes until their end time. A withdraw message would
raise its own authentication problems and require flooding deletions; natural expiry is
a sufficient safety net.

**No interoperability with version 2.** Protocol version 3 messages are not understood
by older nodes and version 2 messages are discarded by new ones. A network must be
upgraded as a whole. The same applies within version 3: the payload layouts described
here differ from those of the first version 3 release and the version number was not
raised again (§8), so every node in a network must be upgraded at the same time.

**Clock synchronisation is required.** Absolute times presuppose it. ION already
requires it for CGR, but version 3 makes the failure mode visible: a node whose clock is
badly off will see other nodes' contacts fall at the temporal checks. Those discards are
logged explicitly as suspected clock skew, so that a silent isolation becomes a
diagnosable one.

**Half-link coverage when a peer's DTNEX is not running**, as discussed in §3.2.

**A confirmed imputed range stays imputed.** When a received range matches one ION
derived by itself from the reverse assertion (§3.3), nothing is written, so ION keeps
only the derived entry. That entry lives and dies with the operator's canonical one: if the
operator deletes it, ION drops the derived entry along with it and our knowledge of that
direction disappears without a log, until the next received message restores it — at
most one `updateInterval` later. Promoting it into an assertion of ours would fix the
lifetime at the cost of silently overriding the symmetry the operator configured, which
is the worse trade.

---

## 8. Design decisions and their reversibility

| decision | reversible? |
|---|---|
| Absolute times on the wire | No — it is the foundation of the design |
| Directional messages, one per direction | No — it determines the authority rule |
| Announcing only contacts originating at the local node | **Yes** — §3.2, without touching the message format |
| Inserting contacts whose destination is the local node | Yes — independent of what is announced |
| OWLT inside the contact message, no separate range message | Reversed since: the range has its own message type (§4.3) |
| Single region | Reversed since: the region travels (§4.4). This row called for a version bump; the change shipped without one, because `DTNEX_PROTOCOL_VERSION` stayed at 3 and every node in a network is upgraded together anyway |
| Announcing only the ranges the node itself asserted | No — §3.3: announcing the imputed ones corrupts the range index of the receivers |
| Confidence as an integer 0–100 | Constrained by the CBOR implementation, not a free choice |
| No local registry of self-inserted entries | A consequence of absolute times |
| No explicit revocation | Yes — would require a new message type |

---

## 9. Other changes in version 3

Three changes that are not part of the contact exchange redesign, but that ship with it.

**Direct ION API access.** Contacts and ranges are read and written through ION's
`rfx_*` API rather than by invoking `ionadmin`. All the code that talks to ION lives in
a single module that knows nothing about CBOR, bundles, authentication, neighbours or
flooding: it reads the announceable set, applies a received contact, and prints the
diagnostic table. Keeping that boundary intact is what allows it to be exercised against
a live ION without any network involved.

**Cooperative termination.** Version 2 handled SIGINT, SIGTERM and SIGTSTP in an
asynchronous signal handler that logged, joined threads, closed endpoints and called
`exit()`. Since the handler could run on any thread, it could terminate the process
while another thread was inside an SDR transaction — and the transaction lock lives in
ION's shared memory, not in the process, so it would stay held and block every other ION
client. Most of what the handler did was not async-signal-safe either.

Version 3 blocks those signals in every thread and collects them in a dedicated thread
with `sigwait()`. That thread is not a signal handler but ordinary code, so logging and
calling the ION API from it are legitimate. It only lowers the run flags and wakes the
services; the teardown happens in `main`, at a point that is by construction outside any
transaction. A second signal still forces an immediate exit, but now says explicitly
that a transaction may be left open and that the remedy is `ionunlock ion`.

Termination by segfault, abort or SIGKILL remains uncovered, as it was before: there the
process dies without executing anything.

**HMAC through OpenSSL.** The hand-written HMAC-SHA256 — RFC 2104 padding, `ipad` and
`opad`, the key hashed when longer than the block — was replaced by a single call to
`HMAC(EVP_sha256(), ...)`. The comment that justified writing it by hand claimed minimal
dependencies, which did not hold: the project already links `-lcrypto`, and the
`SHA256_*` calls it used are deprecated since OpenSSL 3.0 and accounted for eight of the
compiler warnings. The output is byte-identical — verified on 2026-08-22 over five cases
against `openssl dgst -sha256 -hmac` — so the substitution needed no coordination with
the protocol change and is invisible on the wire.
