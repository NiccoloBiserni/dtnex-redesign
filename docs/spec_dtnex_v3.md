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
| **range** | the OWLT between two nodes | payload, carried together with the contact |

In ION, plans and contacts are configured independently of each other. Treating the
first as a source of information about the second is the root cause of problem 1.1.

---

## 3. What a node announces

### 3.1 ION is the source of truth

The set of announceable contacts is read from ION and nothing else: it is a read-only
snapshot, refreshed on a TTL, with no local state and no bookkeeping. Every field
travels as ION holds it — the absolute start and end times, the transmission rate in
bytes per second, the confidence — and the OWLT is obtained by joining each contact
with its corresponding range.

This is what closes problems 1.1 and 1.2 together: what is announced is literally what
ION holds, field by field.

### 3.2 Contacts with no range are not announced

A contact for which no range exists locally is skipped, with a debug-level log. CGR
discards a contact with no range from consideration as a next hop, so announcing it
would occupy memory and generate traffic without ever producing a route.

The side effect is intentional: a configuration error in `ionrc` becomes visible
instead of propagating silently.

### 3.3 Authority rule: a node announces only its own direction

**A node announces only the contacts in which it is itself the transmitting node, and
rejects any received message claiming to describe a direction that originates at the
receiver.**

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

Received contacts whose *destination* is the local node are a different matter, and
they **are** inserted: without them the node would know `me→B` but not `B→me`, and
would have no return routes. The rule is: write what you learn, announce only what you
are authoritative for.

---

## 4. Message format

### 4.1 Envelope

Unchanged from version 2: a nine-element CBOR array carrying version, message type,
timestamps, origin, sender, nonce, payload and HMAC. Nonce handling, authentication,
duplicate suppression and forwarding are untouched.

The version field goes **from 2 to 3**, and version 2 messages are discarded. A
mixed-version network would recreate precisely the inconsistency the redesign removes.

### 4.2 Contact payload

From three fields to seven:

| field | type | notes |
|---|---|---|
| `fromNode` | integer | directional; always equal to the message origin |
| `toNode` | integer | |
| `fromTime` | integer | absolute epoch, never rewritten |
| `toTime` | integer | absolute epoch |
| `xmitRate` | integer | bytes per second, as in ION |
| `confidence` | integer | percentage, 0–100 |
| `owlt` | integer | seconds |

### 4.3 Encoding decisions

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

**The region number does not travel.** The receiver inserts into its own default
region. Propagating it would risk a receiver attempting to insert into a region it does
not know. This makes DTNEX **single-region** — a conscious limitation, recorded in §7.

**The OWLT travels inside the contact message**, rather than in a separate range
message. Contact and range are therefore applied in the same pass, from the same
message, and the pair can never arrive halved — leaving either a contact with no range,
useless to CGR, or an orphan range.

### 4.4 Size

The envelope occupies roughly 33–37 bytes and the payload 26–30, for about 60–67 bytes
per message against the 50 or so of version 2, well within the 128-byte buffer.

The **number** of messages is unchanged. Version 2 sent one contact per neighbour for
each neighbour; version 3 sends one *direction* per neighbour for each neighbour.
Moving to directional messages doubles nothing, because the opposite direction is
announced by the node at the other end. Traffic grows only if ION holds more time
windows for the same pair — in which case genuinely more information is being
propagated.

---

## 5. Applying a received contact

### 5.1 Validation

Every received message is validated before ION is touched at all, and any failure means
the message is discarded without being inserted and without being forwarded. The checks
cover, in order: protocol version, HMAC, nonce, the origin not being the local node,
and the announced direction genuinely belonging to the announcing node.

The remaining checks reject windows that ION would interpret as something other than a
scheduled contact — a zero end time means a discovered, effectively permanent contact; a
zero start time means a hypothetical one; a start time at the maximum representable
value has yet another meaning — as well as values ION would refuse outright, such as a
zero transmission rate or a confidence above 100.

Two temporal checks close the set: an already-expired window is pointless to insert and
flood, and a window starting more than thirty days in the future is treated as
suspected clock skew.

An OWLT of zero is **not** rejected. It is the physically correct value on a LAN, ION
accepts it, and §3.2 already guarantees that only contacts backed by a real range are
ever announced.

### 5.2 Idempotent writing

A contact is identified by the tuple *(region, fromNode, toNode, fromTime)*. Given a
validated message, the write follows from comparing it against what ION already holds:

| state in ION | action |
|---|---|
| the contact does not exist | insert |
| it exists and is identical | **no-op** |
| it exists, only rate or confidence differ | revise in place |
| it exists, the end time differs | targeted removal, then insert |
| it does not exist but the window overlaps a local contact | ION refuses, the local entry is kept, logged at debug level — this is not a failure |

Ranges follow the same structure, except that ION exposes no in-place revision for
them, so a change means targeted removal followed by insertion — with one exception.
ION holds two kinds of range: *asserted* ones, which somebody declared and which are
backed by a stored assertion object, and *imputed* ones, which ION derived by itself
from the canonical assertion in the opposite direction and which exist only as an index
entry. Over an imputed range there is nothing to remove, and the insertion performs the
substitution on its own within a single transaction; removing it first would split that
substitution in two, leaving the pair without a current OWLT in between, and would
expose the insertion to an overlap check that ION skips when the key is already present.
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

### 5.3 Forwarding

Unchanged: a message that passes validation is applied and then forwarded to every
neighbour except its origin and the node it was received from. A discarded message is
not forwarded. Contacts whose destination is the local node follow the general rule —
they are inserted *and* forwarded, being useful topology for the rest of the network
too.

---

## 6. Behaviour over time

### 6.1 When an announcement happens

Three triggers instead of two:

1. the update interval has elapsed;
2. the list of plans has changed (already the case in version 2);
3. **the set of announceable contacts has changed** with respect to the previous
   snapshot.

The third is new, and it is what makes a contact added to `ionrc` visible to the network
promptly instead of within the update interval, which defaults to thirty minutes. Its
responsiveness is governed by the TTL of the contact snapshot, which is therefore a
deliberate trade-off between reacting quickly to configuration changes and accessing
ION frequently.

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

**Single region.** The region number does not travel on the wire (§4.3); every receiver
inserts into its own default region.

**No revocation.** If the operator deletes a contact from the local ION, the copies
already propagated stay on remote nodes until their end time. A withdraw message would
raise its own authentication problems and require flooding deletions; natural expiry is
a sufficient safety net.

**No interoperability with version 2.** Protocol version 3 messages are not understood
by older nodes and version 2 messages are discarded by new ones. A network must be
upgraded as a whole.

**Clock synchronisation is required.** Absolute times presuppose it. ION already
requires it for CGR, but version 3 makes the failure mode visible: a node whose clock is
badly off will see other nodes' contacts fall at the temporal checks. Those discards are
logged explicitly as suspected clock skew, so that a silent isolation becomes a
diagnosable one.

**Half-link coverage when a peer's DTNEX is not running**, as discussed in §3.3.

**A confirmed imputed range stays imputed.** When a received range matches one ION
derived by itself from the reverse assertion, nothing is written, so ION keeps only the
derived entry. That entry lives and dies with the operator's canonical one: if the
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
| Announcing only contacts originating at the local node | **Yes** — §3.3, without touching the message format |
| Inserting contacts whose destination is the local node | Yes — independent of what is announced |
| OWLT inside the contact message, no separate range message | Yes |
| Single region | Yes — requires a version bump |
| Confidence as an integer 0–100 | Constrained by the CBOR implementation, not a free choice |
| No local registry of self-inserted entries | A consequence of absolute times |
| No explicit revocation | Yes — would require a new message type |

---

## 9. Other changes in version 3

Two changes that are not part of the contact exchange redesign, but that ship with it.

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
