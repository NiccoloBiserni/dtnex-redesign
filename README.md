# DTNEX - DTN Network Information Exchange

DTNEX is a high-performance system for distributing network topology information across Delay Tolerant Networks (DTN). It enables DTN nodes to build and maintain local contact graphs by exchanging information about network connectivity and node metadata.

## Overview

DTNEX operates by having DTN nodes periodically exchange information about their configured contacts (communication opportunities) with other nodes in the network. This allows all nodes to build a comprehensive view of the network topology, which is essential for effective DTN routing.

## Key Features

- **Automated Contact Distribution**: Shares ION-DTN contact information across the network
- **Node Metadata Exchange**: Distributes node descriptions, GPS coordinates, and contact information
- **CBOR Message Protocol**: Efficient binary message format with HMAC authentication
- **Network Topology Visualization**: Optional GraphViz integration for network diagrams
- **Multi-threaded Operation**: Event-driven architecture with background services
- **Security**: HMAC-based message authentication and replay protection
- **High Performance**: Contacts and ranges are read and written through the ION API directly (`rfx_*`), with no `ionadmin` round-trip; the optional GraphViz output is the one remaining path that still shells out

## Architecture

### Core Components

1. **Contact Exchange Engine**: Distributes ION contact plan information to neighbor nodes
2. **Metadata Management**: Shares node descriptions, locations, and contact details
3. **Message Authentication**: HMAC-SHA256 with configurable pre-shared keys
4. **Network Visualization**: GraphViz integration for topology diagrams
5. **Bundle Protocol Integration**: Native ION-DTN Bundle Protocol v7 support

### Network Topology Discovery

DTNEX builds network topology through a distributed information exchange process:

1. **Contact Discovery**: Each node periodically broadcasts its configured ION contacts
2. **Information Propagation**: Nodes forward received contact information to their neighbors  
3. **Local Graph Building**: Each node builds a complete network topology view
4. **Routing Integration**: ION uses the distributed contact graph for routing decisions

### Message Protocol

DTNEX uses CBOR (Compact Binary Object Representation) for efficient message encoding:

- **Contact Messages**: Inform nodes about connectivity between node pairs
- **Metadata Messages**: Share node descriptions, GPS coordinates, and operator information
- **Authentication**: HMAC-SHA256 with configurable pre-shared network keys
- **Replay Protection**: Nonce-based duplicate detection and caching

## Installation

### Prerequisites

- **ION-DTN 4.1.0+**: System libraries must be installed (`libbp`, `libici`)
- **OpenSSL Development Libraries**: For HMAC authentication
- **Build Tools**: GCC compiler and development headers
- **Optional**: GraphViz for network visualization

### System Requirements

#### Linux/Ubuntu/Debian
```bash
sudo apt update
sudo apt install build-essential libssl-dev
# Optional: For network visualization
sudo apt install graphviz
```

#### Raspberry Pi (Raspbian/Debian)
```bash
sudo apt update  
sudo apt install build-essential libssl-dev
# Optional: For network visualization
sudo apt install graphviz
```

### ION-DTN System Libraries

DTNEX requires ION-DTN system libraries to be installed. Install ION-DTN either:

#### Option 1: From Package Manager (if available)
```bash
# Ubuntu/Debian (if ION packages are available)
sudo apt install ion-dtn-dev
```

#### Option 2: Build and Install ION from Source
```bash
# Download and build ION-DTN
git clone https://github.com/nasa-jpl/ION
cd ION
make install
# This installs system libraries to /usr/local/lib/
```

### Building DTNEX (Self-contained)

DTNEX now includes all necessary ION headers and builds without requiring ION source code:

```bash
# Clone the repository
git clone https://github.com/NiccoloBiserni/dtnex-redesign
cd dtnex-redesign

# Build using self-contained build script
./build_standalone.sh

# Optional: Install system-wide
sudo make install
```

The self-contained build only requires ION system libraries (`libbp`, `libici`) to be installed - no ION source code needed!

> **Upstream.** This repository is a fork of
> [samograsic/ion-dtn-dtnex](https://github.com/samograsic/ion-dtn-dtnex) by
> Samo Grasic, which remains the original project. This fork carries the v3
> contact-exchange redesign described in [the design spec](docs/spec_dtnex_v3.md):
> contacts are read from and written to ION's own contact plan, times travel as
> absolute epochs, and the protocol version is 3, so it does **not** interoperate
> with v2 nodes.

## Configuration

### Basic Configuration File (dtnex.conf)

The repository ships a `dtnex.conf` with these example values, so a fresh
checkout runs unattended. Edit it for your own node before joining a real
network — in particular `presSharedNetworkKey` and `nodemetadata`.

```bash
# DTNEX Configuration File
# DTN Network Information Exchange
#
# These are example values, meant to let a freshly cloned checkout run
# unattended. Edit them for your own node before joining a real network -
# in particular presSharedNetworkKey and nodemetadata.

# Message exchange interval (seconds) - how often to send contact/metadata updates
updateInterval=1800

# Bundle time-to-live should be longer than update interval for reliability
bundleTTL=1800      # 30 minutes

# Contact lifetime - since v3.00 this applies ONLY to metadata messages.
# The lifetime of the announced contacts is read from ION's contact plan
# (ionrc), no longer from this parameter.
contactLifetime=1800  # 30 minutes

# Pre-shared network key for message authentication.
# "open" is the default and provides no protection: every node that knows it
# can inject contacts. Use a unique key per network.
presSharedNetworkKey=open

# Node metadata shared with other nodes (max 128 characters)
# Format: "NodeName,ContactInfo,LocationDescription"
# CBOR will create: [nodeId, name, contact] or [nodeId, name, contact, lat, lon] with GPS
nodemetadata="DTNEX-Node,admin@example.com,Test-Location"

# GPS coordinates for enhanced metadata (optional)
# When enabled, CBOR metadata will include GPS coordinates as integers (multiplied by 1000000)
#gpsLatitude=59.334591
#gpsLongitude=18.063240

# Graph visualization settings
createGraph=true
graphFile=contactGraph.gv

# Service mode operation
serviceMode=false    # Set to true for background daemon mode
debugMode=false      # Enable verbose debug output

# Disable metadata exchange if needed
noMetadataExchange=false

```

### Configuration Parameters

| Parameter | Description | Default | Example |
|-----------|-------------|---------|---------|
| `updateInterval` | Message exchange frequency (seconds) | 600 | 1800 |
| `bundleTTL` | Bundle time-to-live (seconds) | 1800 | 3600 |
| `contactLifetime` | Metadata message validity (seconds); since v3.00 no longer governs announced contact duration, which is read from ION's contact plan | 3600 | 1800 |
| `contactTimeTolerance` | Removed in this version. If present in an existing configuration file, the key is silently ignored | - | - |
| `presSharedNetworkKey` | HMAC authentication key | "open" | "mynetwork123" |
| `nodemetadata` | Node description string | "" | "Node1,admin@site.com,Location" |
| `gpsLatitude` | GPS latitude (decimal degrees) | - | 59.334591 |
| `gpsLongitude` | GPS longitude (decimal degrees) | - | 18.063240 |
| `createGraph` | Write the GraphViz topology file on each update | false | true |
| `graphFile` | Output path for the graph. The file written is **GraphViz source** (`digraph { … }`), not an image, so a `.gv` name is the sensible one; render it with `dot -Tpng contactGraph.gv -o contactGraph.png`. The built-in default is the misleading `contactGraph.png`, which the shipped configuration overrides | contactGraph.png | contactGraph.gv |
| `serviceMode` | Background daemon mode. The `--service` flag overrides this | false | true |
| `debugMode` | Verbose debug output. The `--debug` flag overrides this | false | true |
| `noMetadataExchange` | Disable metadata sharing. Defaults to `true` when no configuration file is found, so a node without a `dtnex.conf` never advertises itself | true | false |

## Usage

### Basic Operation

```bash
# Start DTNEX (ensure ION is running first)
./dtnex

# Start with debug output
./dtnex --debug

# Background service mode
./dtnex --service
```

Both flags override the corresponding key in `dtnex.conf`, so `--debug` turns the
verbose output on even when the file says `debugMode=false`. Any other argument is
reported and ignored.

### Service Integration

#### Systemd Service (Linux)
```bash
# Create service file
sudo tee /etc/systemd/system/dtnex.service << EOF
[Unit]
Description=DTNEX - DTN Network Information Exchange
After=ion.service
Requires=ion.service

[Service]
Type=simple
User=dtn
WorkingDirectory=/opt/dtnex
ExecStart=/usr/local/bin/dtnex --service
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

# Enable and start the service
sudo systemctl enable dtnex
sudo systemctl start dtnex
```

## CBOR Message Format Specification

DTNEX uses CBOR (Compact Binary Object Representation) for efficient, authenticated message exchange between DTN nodes. The protocol supports two primary message types and can be extended for custom applications.

### Protocol Overview

- **Protocol Version**: 3
- **Message Format**: CBOR arrays with HMAC authentication
- **Authentication**: HMAC-SHA256 (truncated to 64 bits for efficiency)
- **Replay Protection**: 3-byte nonce with origin node tracking
- **Transport**: ION Bundle Protocol v7 (service 12160)

### Message Structure

All DTNEX messages follow this general CBOR array format:

```
[version, type, timestamp, expireTime, origin, from, nonce, messageData, hmac]
```

#### Common Header Fields

| Field | Type | Description | Size |
|-------|------|-------------|------|
| `version` | Integer | Protocol version (currently 3) | 1 byte |
| `type` | Text String | Message type ("c"=contact, "m"=metadata) | 2 bytes (1 header + 1 content) |
| `timestamp` | Integer | Unix timestamp when message was created | 4 bytes |
| `expireTime` | Integer | Unix timestamp when message expires | 4 bytes |
| `origin` | Integer | Node ID that originally created the message | 4-8 bytes |
| `from` | Integer | Node ID that sent this message (may differ from origin for forwarded messages) | 4-8 bytes |
| `nonce` | Byte String | 3-byte random nonce for replay protection | 3 bytes |
| `messageData` | Array/Map | Type-specific message payload | Variable |
| `hmac` | Byte String | 8-byte HMAC-SHA256 authentication tag | 8 bytes |

### Contact Messages (type `"c"`)

Contact messages distribute network connectivity information between DTN nodes.

Messages are directional: a node only announces contacts where it is the `fromNode` (see §4 of [the design spec](docs/spec_dtnex_v3.md)).

#### CBOR Structure
```
[3, "c", timestamp, expireTime, origin, from, nonce, [fromNode, toNode, fromTime, toTime, xmitRate, confidence, owlt], hmac]
```

#### Message Data Array
```cbor
[fromNode, toNode, fromTime, toTime, xmitRate, confidence, owlt]
```

| Field | Type | Description | Example |
|-------|------|-------------|---------|
| `fromNode` | Integer | Node ID that owns/announces this contact; must equal the message `origin` | 268484800 |
| `toNode` | Integer | Node ID at the other end of the contact | 268484801 |
| `fromTime` | Integer | Absolute Unix epoch when the contact window opens | 1694885400 |
| `toTime` | Integer | Absolute Unix epoch when the contact window closes | 1694887200 |
| `xmitRate` | Integer | Data rate in **bytes** per second (ION's own unit) | 100000 |
| `confidence` | Integer | Reliability, percentage 0-100 | 100 |
| `owlt` | Integer | One-way light time in seconds, from the paired ION range | 1 |

#### Example Contact Message
```json
[
  3,                    // Protocol version
  "c",                  // Contact message type
  1694885400,          // Timestamp (Unix epoch)
  1694887200,          // Expire time (equals the contact's toTime)
  268484800,           // Origin node ID
  268484800,           // From node ID (same as origin if not forwarded)
  h'A1B2C3',           // 3-byte nonce
  [                    // Contact data
    268484800,         // fromNode (must equal origin)
    268484801,         // toNode
    1694885400,        // fromTime (absolute epoch)
    1694887200,        // toTime (absolute epoch)
    100000,            // xmitRate (100000 bytes/s)
    100,               // confidence (100%)
    1                  // owlt (1 second)
  ],
  h'1234567890ABCDEF'  // 8-byte HMAC
]
```

### Metadata Messages (type `"m"`)

Metadata messages share node descriptions, GPS coordinates, and operator contact information.

#### CBOR Structure
```
[3, "m", timestamp, expireTime, origin, from, nonce, metadataArray, hmac]
```

#### Metadata Array Structure

The payload is a CBOR **array**, not a map. Its length is variable: the first
three elements are always present, and the optional ones are appended in a fixed
order — `location` first, then the GPS pair. A receiver must therefore dispatch
on the array length, which is what the decoder does.

```cbor
[nodeId, name, contact]                        // 3 elements
[nodeId, name, contact, location]              // 4 elements
[nodeId, name, contact, latitude, longitude]   // 5 elements
[nodeId, name, contact, location, latitude, longitude]  // 6 elements
```

| Position | Field | Type | Description | Example |
|---------|-------|------|-------------|---------|
| 0 | `nodeId` | Integer | Node identifier | 268484800 |
| 1 | `name` | Text String | Node name (up to `MAX_NODE_NAME_LENGTH`, 64 bytes) | "DTNEX-Gateway" |
| 2 | `contact` | Text String | Contact info (up to `MAX_CONTACT_INFO_LENGTH`, 128 bytes) | "ops@example.com" |
| 3 | `location` | Text String | Optional free-text location, sent when no GPS pair is configured or alongside it (up to `MAX_LOCATION_LENGTH`, 64 bytes) | "Stockholm" |
| 3 or 4 | `latitude` | Integer | Optional: GPS latitude × 1,000,000 | 59334591 |
| 4 or 5 | `longitude` | Integer | Optional: GPS longitude × 1,000,000 | 18063240 |

The whole message must still fit within `MAX_CBOR_BUFFER` (128 bytes), which in
practice is the binding limit on the text fields, not the per-field maxima above.

#### Example Metadata Message
```json
[
  3,                    // Protocol version
  "m",                  // Metadata message type
  1694885400,          // Timestamp
  1694887200,          // Expire time
  268484800,           // Origin node ID
  268484800,           // From node ID
  h'D4E5F6',           // 3-byte nonce
  [                    // Metadata array, 5 elements: no location, GPS present
    268484800,         // nodeId
    "Gateway-Node",    // name
    "admin@net.com",   // contact
    59334591,          // latitude (Stockholm)
    18063240           // longitude (Stockholm)
  ],
  h'FEDCBA0987654321'  // 8-byte HMAC
]
```

### Message Authentication

All messages include HMAC-SHA256 authentication using a pre-shared network key.

#### HMAC Calculation
1. **Key Preparation**: Pre-shared network key (configured in `dtnex.conf`)
2. **Message Data**: Everything except the HMAC field
3. **HMAC Computation**: HMAC-SHA256 of message data using the network key
4. **Truncation**: First 8 bytes of the HMAC for space efficiency

#### Replay Protection
- **Nonce**: 3-byte random value included in each message
- **Cache**: Each node maintains a cache of seen (nonce, origin) pairs
- **Validation**: Messages with duplicate (nonce, origin) pairs are rejected

### Message Forwarding

DTNEX implements epidemic-style message forwarding:

1. **Reception**: Node receives and validates message
2. **Processing**: Extracts and stores contact/metadata information
3. **Forwarding**: Forwards message to all neighbors except origin and sender
4. **Flood Control**: Nonce-based duplicate detection prevents loops

### Custom Message Types

The CBOR protocol can be extended for custom applications:

#### Adding New Message Types
1. **Define Type Tag**: Choose an unused one-character text string ("s", "t", ...)
2. **Design Message Data**: Create CBOR array or map structure
3. **Implement Handlers**: Add encoding/decoding functions
4. **Authentication**: Use same HMAC scheme for security

#### Example Custom Message (type `"s"`)
```json
[
  3,                    // Protocol version
  "s",                  // Custom message type
  1694885400,          // Timestamp
  1694887200,          // Expire time
  268484800,           // Origin node ID
  268484800,           // From node ID
  h'789ABC',           // 3-byte nonce
  {                    // Custom data map
    "sensor": "temperature",
    "value": 2350,     // scaled integer: 23.50 °C × 100
    "unit": "celsius",
    "location": "Building-A"
  },
  h'1122334455667788'  // 8-byte HMAC
]
```

> Note: the bundled `include/ion/cbor.h` has no float encoding, which is why
> `confidence` travels as an integer percentage and why the example above scales
> its reading instead of sending `23.5`. Any custom type built on this encoder
> has the same constraint.

### Performance Characteristics

- **Contact Message Size**: ~60-67 bytes (envelope ~33-37 + 7-field payload ~26-30)
- **Metadata Message Size**: ~55-85 bytes (without GPS), ~65-95 bytes (with GPS)
- **Maximum Message Size**: 128 bytes (configurable via `MAX_CBOR_BUFFER`)
- **Authentication Overhead**: 8 bytes HMAC + 3 bytes nonce = 11 bytes
- **Encoding Efficiency**: CBOR provides ~20-40% size reduction vs JSON

### Security Considerations

- **Pre-shared Keys**: Use strong, randomly generated network keys
- **Key Distribution**: Secure key distribution required for network access
- **Message Expiry**: Contact messages expire at the contact's own `toTime`; metadata messages expire after `contactLifetime` — both bound how long a captured message stays replayable
- **Nonce Entropy**: Ensure good randomness for nonce generation
- **Replay Window**: Balance cache size with replay protection needs

## Credits

DTNEX was written by **Samo Grasic** (samo@grasic.net), who remains the author of
the original project at
[samograsic/ion-dtn-dtnex](https://github.com/samograsic/ion-dtn-dtnex) — the
architecture, the CBOR protocol with HMAC authentication, the bpecho service and
the epidemic forwarding are his.

The **v3 contact-exchange redesign** in this fork is by **Niccolo Biserni**
(niccolo.biserni@studio.unibo.it): contacts and ranges are now read from and written to ION's
contact plan through the `rfx_*` API, times travel as absolute epochs, writes are
idempotent and targeted, and termination is cooperative. It is described in full in
[the design spec](docs/spec_dtnex_v3.md).

## License

No license has been declared for this project, upstream or here. Until one is,
default copyright applies and no redistribution or modification rights are
granted. If you intend to use this code, please contact the original author.
