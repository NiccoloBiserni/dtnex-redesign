/**
 * dtnex.c
 * DTNEX - DTN Network Information Exchange
 * High-performance C implementation for exchanging DTN contact and metadata information
 *
 * Author: Samo Grasic (samo@grasic.net)
 * v3 contact-exchange redesign: Niccolo Biserni (niccolo.biserni@studio.unibo.it)
 */

#include "dtnex.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>  // for execv

// Global variables
volatile int running = 1;
/*
    Flag that keeps the main loop alive. It is volatile because the thread that
    modifies it, signalWaitThread, is not the one running the loop: it is set to
    0 when a signal arrives (SIGINT/SIGTERM/SIGTSTP), so that the main loop
    terminates in an orderly fashion.
*/
volatile int ionConnected = 0;  // Global ION connection status; volatile because it can change asynchronously (e.g. if ION restarts while DTNEX is running)
volatile int ionRestartDetected = 0;  // Flag to trigger complete restart
/*
    If set to 1, dtnex does not simply leave the loop: it calls restartDtnex(),
    which execv()s the process onto itself. No child process is created, the
    process image is simply replaced.
*/
static char **original_argv = NULL;  // Store original argv for restart
static int original_argc = 0;        // Store original argc for restart
/* Arguments used to perform the execv; they are saved at the top of main(). */
BpSAP sap;
/*
    Bundle Protocol Service Access Point (SAP): the main handle onto ION's BP
    layer. It behaves like a socket, but for DTN bundles; it is obtained from
    bp_open() and used to send and receive bundles. Used throughout the code
    for every BP operation.
*/
Sdr sdr;
/*
    Handle used to access ION's shared persistent memory, which does not go
    through the standard malloc.
*/

// Message deduplication cache
HashCache hashCache[MAX_HASH_CACHE];
int hashCacheCount = 0;
/*
    Used to deduplicate processed messages: every received message is hashed
    and compared against the hashes already stored. A matching hash means the
    message is a duplicate, and it is discarded.
 */
NonceCache nonceCache[MAX_HASH_CACHE]; // For CBOR replay protection
int nonceCacheCount = 0;
/*
    The same, but for nonces: this guards against message replay. The cache
    tracks (nonce, origin) pairs, so that a message intercepted and re-sent by
    an attacker after it has already been processed is discarded.
*/

NodeMetadata nodeMetadataList[MAX_PLANS];
int nodeMetadataCount = 0;
/*
    A local "address book" of nodeId + metadata. It is updated by
    updateNodeMetadata() when a metadata message arrives, and then consulted
    in getContacts().
*/

BpechoState bpechoState;   // Bpecho service state
pthread_t bpechoThread;    // Thread for bpecho service
/*
    Echo service management. BpechoState is a struct holding the state, a sap
    and an attendant.
*/

BundleReceptionState bundleReceptionState;  // Bundle reception service state
/*
    Bundle reception thread management. BundleReceptionState is a struct
    holding the state, the config and the thread identifier.
*/

/* "The thread has been created" is a different fact from "the thread should
 * keep running": the latter is cleared in order to REQUEST shutdown, so it
 * cannot guard the join, or the join would be skipped exactly when it is
 * needed. Each thread can be born at two points: at startup in main(), if ION
 * is already reachable, or inside eventDrivenLoop() after a later reconnection
 * to ION. The flag must be raised at both points, otherwise a thread born only
 * from a reconnection would never be joined. */
int bpechoThreadStarted = 0;
int bundleReceptionThreadStarted = 0;

pthread_t signalThread;    /* Thread dedicated to collecting signals via sigwait */

/**
 * Logging helper function with color support
 * 
 * Color codes:
 * - Red (ERROR): \033[31m
 * - Green (SUCCESS/RECEIVED): \033[32m
 * - Yellow (SENT/WARNING): \033[33m
 * - Blue (FORWARDING): \033[34m
 * - Magenta (NEIGHBOR INFO): \033[35m
 * - Cyan (INFO/SYSTEM): \033[36m
 * - Bold: \033[1m
 * - Reset: \033[0m
 */
void dtnex_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

/**
 * Debug logging - only outputs if debug mode is enabled
 */
void debug_log(DtnexConfig *config, const char *format, ...) {
    if (!config || !config->debugMode) {
        return;
    }

    va_list args;
    va_start(args, format);
    printf("\033[90m[DEBUG] ");  // Dark gray color
    vprintf(format, args);
    printf("\033[0m\n");  // Reset color
    va_end(args);
    fflush(stdout);
}


/**
 * Optimized message logging functions
 * Format: [TYPE] Origin→From→To: Contact(A↔B) | Metadata(Node:Name)
 */

// Yellow for sending messages
void log_message_sent(DtnexConfig *config, unsigned long origin, unsigned long to, const char *type, 
                     unsigned long nodeA, unsigned long nodeB, const char *metadata) {
    if (!config->debugMode) return;
    
    if (strcmp(type, "contact") == 0) {
        printf("\033[33m[SENT] Origin:%lu, Source:%lu, Dest:%lu: Contact(%lu↔%lu)\033[0m\n", 
               origin, config->nodeId, to, nodeA, nodeB);
    } else if (strcmp(type, "metadata") == 0) {
        printf("\033[33m[SENT] Origin:%lu, Source:%lu, Dest:%lu: Metadata(%lu:%s)\033[0m\n",
               origin, config->nodeId, to, nodeA, metadata ? metadata : "?");
    }
    fflush(stdout);
}

// Green for receiving messages  
void log_message_received(DtnexConfig *config, unsigned long origin, unsigned long from, const char *type,
                         unsigned long nodeA, unsigned long nodeB, const char *metadata) {
    if (!config->debugMode) return;
    
    if (strcmp(type, "contact") == 0) {
        printf("\033[32m[RECV] Origin:%lu, Source:%lu, Dest:%lu: Contact(%lu↔%lu)\033[0m\n",
               origin, from, config->nodeId, nodeA, nodeB);
    } else if (strcmp(type, "metadata") == 0) {
        printf("\033[32m[RECV] Origin:%lu, Source:%lu, Dest:%lu: Metadata(%lu:%s)\033[0m\n",
               origin, from, config->nodeId, nodeA, metadata ? metadata : "?");
    }
    fflush(stdout);
}

// Purple for forwarding messages
void log_message_forwarded(DtnexConfig *config, unsigned long origin, unsigned long from, unsigned long to,
                          const char *type, unsigned long nodeA, unsigned long nodeB, const char *metadata) {
    if (!config->debugMode) return;
    
    if (strcmp(type, "contact") == 0) {
        printf("\033[35m[FRWD] Origin:%lu, Source:%lu, Dest:%lu: Contact(%lu↔%lu)\033[0m\n",
               origin, from, to, nodeA, nodeB);
    } else if (strcmp(type, "metadata") == 0) {
        printf("\033[35m[FRWD] Origin:%lu, Source:%lu, Dest:%lu: Metadata(%lu:%s)\033[0m\n",
               origin, from, to, nodeA, metadata ? metadata : "?");
    }
    fflush(stdout);
}

// Gray for error/unknown messages (always shown even in normal mode)
void log_message_error(DtnexConfig *config, const char *error_msg) {
    printf("\033[90m[ERROR] %s\033[0m\n", error_msg);
    fflush(stdout);
}

// New contact graph updates (shown in both debug and normal mode)
void log_contact_update(DtnexConfig *config, int contactCount) {
    printf("\033[36m[UPDATE] Contact graph refreshed: %d active contacts\033[0m\n", contactCount);
    fflush(stdout);
}

/**
 * Loads configuration from dtnex.conf file
 * If file doesn't exist, use defaults (but will not exchange own metadata)
 */
void loadConfig(DtnexConfig *config) {
    // Set defaults
    config->updateInterval = DEFAULT_UPDATE_INTERVAL; // 600, i.e. 10 minutes between updates
    config->contactLifetime = DEFAULT_CONTACT_LIFETIME; // 3600, i.e. 1 hour of contact validity
    config->bundleTTL = DEFAULT_BUNDLE_TTL; // 1800, i.e. 30 minutes of bundle TTL (3x update interval)
    strcpy(config->presSharedNetworkKey, DEFAULT_PRESHARED_KEY);
    sprintf(config->serviceNr, "%d", DEFAULT_SERVICE_NR);
    sprintf(config->bpechoServiceNr, "%d", DEFAULT_BPECHO_SERVICE_NR);
    config->nodeId = 0; // set to 0 because the node does not know its own ID until it connects to ION; it is then updated in tryConnectToIon()
    memset(config->nodemetadata, 0, MAX_METADATA_LENGTH);
    config->createGraph = 0;
    strcpy(config->graphFile, "contactGraph.gv");  // GraphViz source, not an image
    config->noMetadataExchange = 1; // Default to not exchanging own metadata if no config file
    config->debugMode = 0; // Default to no debug output
    config->serviceMode = 0; // Default to interactive mode
    config->gpsLatitude = 0.0;
    config->gpsLongitude = 0.0;
    config->hasGpsCoordinates = 0;

    // Try to read from config file
    FILE *configFile = fopen("dtnex.conf", "r");
    if (configFile) {
        
        // Config file exists, disable the no-metadata-exchange flag
        config->noMetadataExchange = 0;
        
        char line[MAX_LINE_LENGTH];
        while (fgets(line, MAX_LINE_LENGTH, configFile)) {
            char *ptr = line;
            // Skip whitespace
            while(*ptr && isspace(*ptr)) ptr++;
            // Skip comments and empty lines
            if (*ptr == '#' || *ptr == '\0') continue;

            // Parse key=value pairs
            char *key = strtok(ptr, "=");
            char *value = strtok(NULL, "\n");
            if (key && value) {
                // Remove whitespace
                char *end = key + strlen(key) - 1;
                while(end > key && isspace(*end)) *end-- = '\0';
                
                while(*value && isspace(*value)) value++;
                
                // Remove comments from value (anything after #)
                char *comment = strchr(value, '#');
                if (comment) {
                    *comment = '\0';  // Terminate the string at the comment
                }
                
                // Remove trailing whitespace
                end = value + strlen(value) - 1;
                while(end > value && isspace(*end)) *end-- = '\0';

                // Remove quotes if present
                if (*value == '"' && value[strlen(value)-1] == '"') {
                    value[strlen(value)-1] = '\0';
                    value++;
                }

                // Assign values based on key
                if (strcmp(key, "updateInterval") == 0) {
                    config->updateInterval = atoi(value);
                } else if (strcmp(key, "contactLifetime") == 0) {
                    config->contactLifetime = atoi(value);
                } else if (strcmp(key, "bundleTTL") == 0) {
                    config->bundleTTL = atoi(value);
                } else if (strcmp(key, "presSharedNetworkKey") == 0) {
                    strncpy(config->presSharedNetworkKey, value, sizeof(config->presSharedNetworkKey) - 1);
                } else if (strcmp(key, "serviceNr") == 0) {
                    strncpy(config->serviceNr, value, sizeof(config->serviceNr) - 1);
                } else if (strcmp(key, "bpechoServiceNr") == 0) {
                    strncpy(config->bpechoServiceNr, value, sizeof(config->bpechoServiceNr) - 1);
                } else if (strcmp(key, "nodemetadata") == 0) {
                    strncpy(config->nodemetadata, value, sizeof(config->nodemetadata) - 1);
                } else if (strcmp(key, "createGraph") == 0) {
                    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                        config->createGraph = 1;
                    }
                } else if (strcmp(key, "graphFile") == 0) {
                    strncpy(config->graphFile, value, sizeof(config->graphFile) - 1);
                } else if (strcmp(key, "noMetadataExchange") == 0) {
                    if (strcmp(value, "true") == 0) {
                        config->noMetadataExchange = 1;
                    }
                } else if (strcmp(key, "debugMode") == 0) {
                    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                        config->debugMode = 1;
                    } else {
                        config->debugMode = 0;
                    }
                } else if (strcmp(key, "serviceMode") == 0) {
                    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                        config->serviceMode = 1;
                    } else {
                        config->serviceMode = 0;
                    }
                } else if (strcmp(key, "gpsLatitude") == 0) {
                    config->gpsLatitude = atof(value);
                    config->hasGpsCoordinates = 1;
                } else if (strcmp(key, "gpsLongitude") == 0) {
                    config->gpsLongitude = atof(value);
                    config->hasGpsCoordinates = 1;
                }
            }
        }
        fclose(configFile);
        dtnex_log("Configuration loaded from dtnex.conf");
    } else {
        dtnex_log("No dtnex.conf found, using f settings (no metadata exchange)");
    }
}


// No global endpoint status needed in the single-threaded version

/**
 * Try to connect to ION - returns 0 on success, -1 on failure
 * This function handles all ION connection logic cleanly
 */

/**
 * This function is the bridge between DTNEX and ION. It is made of 5 sequential
 * phases; any failure performs a clean rollback and returns -1. On success it
 * returns 0 and leaves DTNEX ready to use ION.
 */
int tryConnectToIon(DtnexConfig *config) {
    char endpointId[MAX_EID_LENGTH];
    
    // Try to attach to ION BP system
    /**
     * Attaches the process to the already running ION instance.
     */
    if (bp_attach() < 0) {
        return -1;
    }

    /**
     * Reading the Node ID from ION.
     */
    
    // Get the node ID from ION configuration
    Sdr ionsdr = getIonsdr();
    if (ionsdr == NULL) {
        bp_detach();
        return -1;
    }
    
    // Start transaction to safely access ION configuration
    if (sdr_begin_xn(ionsdr) < 0) {
        bp_detach();
        return -1;
    }
    
    // Get the node number from ION configuration
    Object iondbObject = getIonDbObject(); // pointer to the iondb object inside ION's SDR
    if (iondbObject == 0) {
        sdr_exit_xn(ionsdr);
        bp_detach();
        return -1;
    }

    /* Only the ownNodeNbr field is read, not the whole IonDB: the
     * sizeof(IonDB) of the bundled headers does not match that of the
     * installed ION library, so reading the entire struct would over-read
     * past the end of the object in SDR. ownNodeNbr is the first field of
     * the struct, so its offset is the same in both layouts. */
    uvast ownNodeNbr;
    sdr_read(ionsdr, (char *) &ownNodeNbr, iondbObject + offsetof(IonDB, ownNodeNbr), sizeof(ownNodeNbr));
    config->nodeId = ownNodeNbr;
    sdr_exit_xn(ionsdr);

    /* Only ownNodeNbr is ever taken from the IonDB: the struct offsets in the
     * bundled headers (include/ion/ion.h) do not match those of the installed
     * ION library, so a value read at any other offset would not be the field
     * it is named after (risk §12.3 of the design spec). */

    if (config->nodeId == 0) {
        bp_detach();
        return -1;
    }
    
    dtnex_log("Using node ID: %lu detected from ION configuration", config->nodeId);
    
    // Construct endpoint ID
    snprintf(endpointId, MAX_EID_LENGTH, "ipn:%lu.%s", config->nodeId, config->serviceNr);
    dtnex_log("Using endpoint: %s", endpointId);
    
    // Get the SDR
    sdr = bp_get_sdr(); // obtained via bp_get_sdr() rather than getIonsdr(): this is the correct call for the SDR to be used with BP
    if (sdr == NULL) {
        bp_detach();
        return -1;
    }
    
    // First try to add/register the endpoint in ION's routing database

    /**
     * addEndpoint() is an ION API that adds the EID to the routing database. It
     * tells ION that a bundle arriving for this EID must be enqueued here
     * rather than discarded. A failure is not fatal: it means the endpoint was
     * already registered by a previous run.
     */

    if (addEndpoint(endpointId, EnqueueBundle, NULL) < 0) {
        debug_log(config, "Warning: Could not register endpoint %s in routing database", endpointId);
    }

    /**
     * With the endpoint now registered, we obtain the sap for that EID, which is
     * what we will receive and send messages through; it is stored in the global
     * variable sap.
     */
    
    // Try to open the endpoint for receiving messages
    if (bp_open(endpointId, &sap) < 0) {
        bp_detach();
        return -1;
    }
    
    dtnex_log("✅ Endpoint opened successfully: %s", endpointId);
    
    // If metadata exchange is enabled, add our own metadata first
    if (!config->noMetadataExchange && strlen(config->nodemetadata) > 0) {
        StructuredMetadata metadata;
        char ownMetadata[MAX_METADATA_LENGTH];
        
        // Parse our own metadata
        parseNodeMetadata(config->nodemetadata, &metadata);
        
        // Create metadata string with GPS if available, otherwise use location field

        /**
         * Adds our own metadata, taken from the configuration file, to the local
         * metadata list, from which it will later be pulled to fill the metadata
         * messages sent to neighbours.
         */

        if (config->hasGpsCoordinates) {
            snprintf(ownMetadata, sizeof(ownMetadata), "%s,%s,%.6f,%.6f", 
                    metadata.name, metadata.contact, 
                    config->gpsLatitude, config->gpsLongitude);
        } else if (strlen(metadata.location) > 0) {
            snprintf(ownMetadata, sizeof(ownMetadata), "%s,%s,%s", 
                    metadata.name, metadata.contact, metadata.location);
        } else {
            snprintf(ownMetadata, sizeof(ownMetadata), "%s,%s", 
                    metadata.name, metadata.contact);
        }
        
        // Add as first entry
        updateNodeMetadata(config, config->nodeId, ownMetadata);
        dtnex_log("✅ Added own node metadata: %s", ownMetadata);
    }
    
    return 0;
}

/**
 * Initialize the DTNEX application - Modified to work without requiring ION connection
 */
int init(DtnexConfig *config) {
    dtnex_log("Starting DTNEXC v%s (built %s %s)", 
              DTNEXC_VERSION, DTNEXC_BUILD_DATE, DTNEXC_BUILD_TIME);
    dtnex_log("Original author: Samo Grasic (samo@grasic.net) — "
              "v3 contact-exchange redesign: Niccolo Biserni (niccolo.biserni@studio.unibo.it)");
    
    // Try to connect to ION, but don't fail if unavailable
    if (tryConnectToIon(config) == 0) {
        ionConnected = 1;
        dtnex_log("✅ Successfully connected to ION");
    } else {
        ionConnected = 0;
        dtnex_log("⚠️ ION not available - will retry every minute");
        // Set default node ID for service mode
        config->nodeId = 0;
    }
    
    // Initialize service configuration
    strcpy(config->serviceNr, "12160");
    strcpy(config->bpechoServiceNr, "12161");
    
    dtnex_log("DTNEXC initialized successfully");
    return 0;
}

/**
 * Get the list of plans (neighbor nodes) directly from ION using ION API
 * Based on ipnadmin's listPlans function
 */

/**
 * What is this function for? It tells us which neighbour nodes ION knows about
 * right now. It queries ION's INTERNAL STRUCTURES directly to obtain the list
 * of neighbour nodes (plans).
 */

/* getplanlist is called both from the main loop and from the reception thread
 * (via forwardCborContactMessage) and writes into its own static cache:
 * access must be serialised (§7.5). */
static pthread_mutex_t planListMutex = PTHREAD_MUTEX_INITIALIZER;

void getplanlist(DtnexConfig *config, Plan *plans, int *planCount) {
    time_t currentTime;

    // Static variables caching the plans; they persist across calls to avoid
    // querying ION too often. If the plans were refreshed recently (less than
    // 20 seconds ago), the cached ones are used.
    // Being static, they are initialised only once.
    static Plan cachedPlans[MAX_PLANS];
    static int cachedPlanCount = 0;
    static time_t lastPlanUpdate = 0;
    
    // ION API related variables
    Sdr sdr;
    BpDB *bpConstants;
    
    // Initialize the plan count to 0
    *planCount = 0;

    // Get current time
    time(&currentTime);

    pthread_mutex_lock(&planListMutex);

    // Check if we've updated plans recently - if so, use cached results to avoid 
    // constant calls to ION API which can be expensive
    if (lastPlanUpdate > 0 && (currentTime - lastPlanUpdate) < 20) {
        // Copy the cached plans to the output
        for (int i = 0; i < cachedPlanCount; i++) {
            plans[i] = cachedPlans[i];
        }
        *planCount = cachedPlanCount;

        dtnex_log("Using cached plan list (age: %ld seconds)", currentTime - lastPlanUpdate);
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    dtnex_log("Getting a fresh list of neighbors from ION...");
    
    // Get the SDR database
    sdr = getIonsdr();

    /**
     * The cache TTL has expired but the SDR still cannot be obtained from ION;
     * if anything is left in the cache, it is used anyway.
     */

    if (sdr == NULL) {
        dtnex_log("Error: can't get ION SDR");

        // Fallback: use previously cached plans if available
        if (cachedPlanCount > 0) {
            for (int i = 0; i < cachedPlanCount; i++) {
                plans[i] = cachedPlans[i];
            }
            *planCount = cachedPlanCount;
            dtnex_log("Using %d plans from cache (fallback)", *planCount);
        }
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    // ION-specific part: querying ION to obtain the list of plans

    // Start a transaction
    if (sdr_begin_xn(sdr) < 0) {
        dtnex_log("Error: can't begin SDR transaction");
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    // Get the BP constants
    bpConstants = getBpConstants(); // pointer to the database object living in the SDR
    if (bpConstants == NULL) {
        dtnex_log("Error: can't get BP constants");
        sdr_exit_xn(sdr);
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    // Get the list of plans and iterate through it with safe access

    /**
     * bpConstants->plans is an SDR-managed list of plans, not a C list, so it
     * must be iterated through the appropriate API.
     */

    Object planElt = 0;
    for (planElt = sdr_list_first(sdr, bpConstants->plans); 
         planElt && planElt != 0; 
         planElt = sdr_list_next(sdr, planElt)) {
        
        // Get the plan data with careful error checking
        Object planData = sdr_list_data(sdr, planElt); // an offset relative to the base of shared memory, not an actual pointer
        if (planData == 0) {
            dtnex_log("Warning: Null plan data, skipping");
            continue;
        }
        
        // Get the plan object - cast to BpPlan* with proper error checking
        BpPlan *plan = (BpPlan*) sdr_pointer(sdr, planData); // turns the Object into a readable, dereferenceable pointer into shared memory - not a copy
        if (plan == NULL) {
            dtnex_log("Warning: Null plan pointer, skipping");
            continue;
        }

        // Only include plans with a valid neighbor node number (CBHE-compliant)
        if (plan->neighborNodeNbr == 0) {
            continue;
        }

        // Skip our own node
        if (plan->neighborNodeNbr == config->nodeId) { // prevents the node from sending plans to itself
            continue;
        }

        // No verbose output for each plan found

        // Add this plan to our lists (both the output and the cache)
        if (*planCount < MAX_PLANS) {
            plans[*planCount].planId = plan->neighborNodeNbr; // neighborNodeNbr is stored as planId: it is the key piece of information identifying the neighbour, and is used later to build the EID -> ipn:neighborNodeNbr.serviceNr
            time(&plans[*planCount].timestamp);
            
            // Also update the cache
            cachedPlans[*planCount].planId = plan->neighborNodeNbr;
            cachedPlans[*planCount].timestamp = plans[*planCount].timestamp;
            
            (*planCount)++;
        } else {
            dtnex_log("Warning: Plan list is full (%d entries), skipping additional plans", MAX_PLANS);
            break; // Stop processing if we hit the limit
        }
    }
    
    // End the transaction
    sdr_exit_xn(sdr);

    // Update the cached plan count
    cachedPlanCount = *planCount;
    
    // Update timestamp of last plan update
    lastPlanUpdate = currentTime;
    
    dtnex_log("\033[35mList of configured plans:\033[0m");  // Magenta color like tput setaf 5
    for (int i = 0; i < *planCount; i++) {
        dtnex_log(">%lu", plans[i].planId);
    }

    dtnex_log("%d neighbors found in ION configuration", *planCount);
    pthread_mutex_unlock(&planListMutex);
}

/**
 * Send a bundle using ION BP functions - following bpsource.c pattern
 */

/**
 * Exchange contact information with neighbors
 * Only perform exchange every 30 minutes (1800 seconds) or if plan list changes
 */
/**
 * Exchange CBOR-encoded contact and metadata messages with neighbor nodes
 * Pure CBOR implementation - no string format support
 */

/**
 * Decides whether, what and to whom information is sent, then delegates the
 * construction and transmission of the bundle to the CBOR functions. It has
 * three phases: the time gate, the contact send loop and the metadata send loop.
 */

/**
 * Snapshot of the announceable contacts (§3.2).
 *
 * CACHE CORRECTNESS INVARIANT (§7.4): this cache is TTL-only, with no
 * invalidation on write, and that is correct ONLY because the snapshot holds
 * exclusively contacts whose fromNode is the local node (authority rule, §4).
 * No write dtnex performs on ION can therefore fall inside this set. If anyone
 * relaxes that filter, this cache silently becomes wrong.
 *
 * The TTL governs the responsiveness of trigger 3 in §7.1: it is the maximum
 * delay between a change to ionrc and its discovery by the network. 60 seconds
 * is the chosen compromise: it matches the longest wake-up period of the main
 * loop, so it adds no ION accesses beyond the pace of the loop itself.
 */
#define MY_CONTACTS_CACHE_TTL 60

static ContactRecord myContacts[IONC_MAX_CONTACTS];
static int myContactCount = 0;
static time_t myContactsUpdated = 0;

static int sameContactRecord(const ContactRecord *a, const ContactRecord *b) {
    return a->fromNode == b->fromNode
        && a->toNode == b->toNode
        && a->fromTime == b->fromTime
        && a->toTime == b->toTime
        && a->xmitRate == b->xmitRate
        && a->confidence == b->confidence
        && a->owlt == b->owlt;
}

/**
 * Refreshes the snapshot if the TTL has expired. Writes 1 into *changed if the
 * new snapshot differs from the previous one, 0 otherwise (or if it was not
 * refreshed). Returns the number of contacts in the snapshot, or -1 on an ION
 * access error (in which case the previous snapshot stays valid).
 *
 * Single-threaded (§7.5): only the main loop calls this function.
 */
static int refreshMyContacts(DtnexConfig *config, int *changed) {
    ContactRecord fresh[IONC_MAX_CONTACTS];
    time_t now = time(NULL);
    int freshCount;
    int i;

    *changed = 0;

    if (myContactsUpdated > 0 && (now - myContactsUpdated) < MY_CONTACTS_CACHE_TTL) {
        return myContactCount;
    }

    freshCount = ionc_get_own_contacts(config->nodeId, fresh, IONC_MAX_CONTACTS,
            config->debugMode);
    if (freshCount < 0) {
        debug_log(config, "⚠️ Could not re-read the announceable contacts from ION");
        return -1;
    }

    if (freshCount != myContactCount) {
        *changed = 1;
    } else {
        for (i = 0; i < freshCount; i++) {
            if (!sameContactRecord(&fresh[i], &myContacts[i])) {
                *changed = 1;
                break;
            }
        }
    }

    memcpy(myContacts, fresh, sizeof(ContactRecord) * freshCount);
    myContactCount = freshCount;
    myContactsUpdated = now;

    if (*changed) {
        dtnex_log("🔄 The snapshot of announceable contacts changed (%d contacts)",
                myContactCount);
    }

    return myContactCount;
}

void exchangeWithNeighbors(DtnexConfig *config, Plan *plans, int planCount) {
    int i, j;
    time_t currentTime;
    char destEid[MAX_EID_LENGTH];
    unsigned char cborBuffer[MAX_CBOR_BUFFER];
    int messageSize;

    static time_t lastExchangeTime = 0;
    static int lastPlanCount = 0;
    static unsigned long lastPlanList[MAX_PLANS];
    int planListChanged = 0;
    int contactsChanged = 0;

    time(&currentTime);

    // Trigger 3 (§7.1): refresh the snapshot and see whether it changed.
    refreshMyContacts(config, &contactsChanged);

    // Trigger 2: the neighbour list changed
    if (planCount != lastPlanCount) {
        planListChanged = 1;
    } else {
        for (i = 0; i < planCount; i++) {
            int found = 0;
            for (j = 0; j < lastPlanCount; j++) {
                if (plans[i].planId == lastPlanList[j]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                planListChanged = 1;
                break;
            }
        }
    }

    // Trigger 1: updateInterval has elapsed
    if (!(lastExchangeTime == 0
            || (currentTime - lastExchangeTime) >= config->updateInterval
            || planListChanged
            || contactsChanged)) {
        int remainingTime = config->updateInterval - (int) (currentTime - lastExchangeTime);
        debug_log(config, "Skipping neighbor exchange (next in %d seconds)", remainingTime);
        return;
    }

    dtnex_log("📤 Announcing %d contacts to %d neighbours...", myContactCount, planCount);

    lastExchangeTime = currentTime;
    lastPlanCount = planCount;
    for (i = 0; i < planCount && i < MAX_PLANS; i++) {
        lastPlanList[i] = plans[i].planId;
    }

    // One message per announceable contact, to every neighbour (§5.4).
    for (i = 0; i < planCount; i++) {
        unsigned long neighborId = plans[i].planId;

        if (neighborId == config->nodeId) {
            continue;  // local loopback plan
        }

        for (j = 0; j < myContactCount; j++) {
            ContactRecord *contact = &myContacts[j];

            messageSize = encodeCborContactMessage(config, contact, cborBuffer,
                    MAX_CBOR_BUFFER);
            if (messageSize <= 0) {
                dtnex_log("❌ Failed to encode CBOR contact message for %lu→%lu",
                        contact->fromNode, contact->toNode);
                continue;
            }

            snprintf(destEid, sizeof(destEid), "ipn:%lu.%s", neighborId, config->serviceNr);
            debug_log(config, "[exchange] %lu→%lu from=%ld to=%ld xmitRate=%lu conf=%u owlt=%u → %s (%d byte)",
                    contact->fromNode, contact->toNode,
                    (long) contact->fromTime, (long) contact->toTime,
                    contact->xmitRate, contact->confidence, contact->owlt,
                    destEid, messageSize);

            sendCborBundle(destEid, cborBuffer, messageSize, config->bundleTTL);
            log_message_sent(config, config->nodeId, neighborId, "contact",
                    contact->fromNode, contact->toNode, NULL);
        }
    }

    // Send CBOR metadata to neighbors (if enabled)
    /**
     * If metadata exchange is enabled, the node exchanges ITS OWN metadata,
     * taken from the configuration file, and sends it to every neighbour.
     */
    if (!config->noMetadataExchange && strlen(config->nodemetadata) > 0) {
        dtnex_log("📤 Exchanging CBOR metadata with neighbors...");

        for (i = 0; i < planCount; i++) {
            unsigned long neighborId = plans[i].planId;

            // Skip local loopback plan
            if (neighborId == config->nodeId) {
                continue;
            }

            // Create metadata structure
            StructuredMetadata metadata;
            metadata.nodeId = config->nodeId;
            parseNodeMetadata(config->nodemetadata, &metadata);

            // Add GPS coordinates if available
            if (config->hasGpsCoordinates) {
                metadata.latitude = (int)(config->gpsLatitude * GPS_PRECISION_FACTOR);
                metadata.longitude = (int)(config->gpsLongitude * GPS_PRECISION_FACTOR);
            } else {
                metadata.latitude = 0;
                metadata.longitude = 0;
            }

            // Encode metadata message to CBOR
            // Writes the metadata into cborBuffer and returns the message size
            messageSize = encodeCborMetadataMessage(config, &metadata, cborBuffer, MAX_CBOR_BUFFER);
            if (messageSize > 0) {
                sprintf(destEid, "ipn:%lu.%s", neighborId, config->serviceNr);

                // Send CBOR bundle
                sendCborBundle(destEid, cborBuffer, messageSize, config->bundleTTL);

                // Log optimized metadata sending message
                log_message_sent(config, config->nodeId, neighborId, "metadata",
                               metadata.nodeId, 0, metadata.name);
            } else {
                dtnex_log("❌ Failed to encode CBOR metadata message for node %lu", config->nodeId);
            }
        }
    } else if (config->noMetadataExchange) {
        if (config->debugMode) {
            dtnex_log("📤 Metadata exchange disabled in configuration");
        }
    }
}

/**
 * Forward a received contact message to other neighbors
 */


/**
 * Process a contact message type "c"
 */


/**
 * Update node metadata in memory and optionally in file
 */
void updateNodeMetadata(DtnexConfig *config, unsigned long nodeId, const char *metadata) {
    int i;
    int nodeFound = 0;
    
    // First check if we already have metadata for this node
    for (i = 0; i < nodeMetadataCount; i++) {
        if (nodeMetadataList[i].nodeId == nodeId) {
            // Update existing entry
            strncpy(nodeMetadataList[i].metadata, metadata, sizeof(nodeMetadataList[i].metadata) - 1);
            nodeMetadataList[i].metadata[sizeof(nodeMetadataList[i].metadata) - 1] = '\0';
            nodeFound = 1;
            debug_log(config, "[INFO] Updated metadata for node %lu: \"%s\"", 
                      nodeId, nodeMetadataList[i].metadata);
            break;
        }
    }
    
    // If not found, add new entry
    if (!nodeFound && nodeMetadataCount < MAX_PLANS) {
        nodeMetadataList[nodeMetadataCount].nodeId = nodeId;
        strncpy(nodeMetadataList[nodeMetadataCount].metadata, metadata, 
                sizeof(nodeMetadataList[nodeMetadataCount].metadata) - 1);
        nodeMetadataList[nodeMetadataCount].metadata[sizeof(nodeMetadataList[nodeMetadataCount].metadata) - 1] = '\0';
        debug_log(config, "[INFO] Added new metadata for node %lu: \"%s\"", 
                  nodeId, nodeMetadataList[nodeMetadataCount].metadata);
        nodeMetadataCount++;
    }
    
    // Update the metadata file if graph generation is enabled
    if (config->createGraph) {
        FILE *metadataFile = fopen("nodesmetadata.txt", "w");
        if (metadataFile) {
            for (i = 0; i < nodeMetadataCount; i++) {
                fprintf(metadataFile, "%lu:%s\n", nodeMetadataList[i].nodeId, nodeMetadataList[i].metadata);
            }
            fclose(metadataFile);
            debug_log(config, "[INFO] Updated nodesmetadata.txt for graph generation");
        }
    }
    
    // Print a complete list of all metadata after receiving and processing (debug mode only)
    debug_log(config, "======== COLLECTED NODE METADATA (%d nodes) ========", nodeMetadataCount);
    debug_log(config, "NODE ID    | METADATA");
    debug_log(config, "----------------------------------------");
    for (i = 0; i < nodeMetadataCount; i++) {
        debug_log(config, "%-10lu | %s", 
                nodeMetadataList[i].nodeId, nodeMetadataList[i].metadata);
    }
    debug_log(config, "========================================");
}

/**
 * Process one received message by extracting the basic information,
 * then calling the appropriate specialized message handler.
 */

/**
 * Check for incoming bundles - Non-blocking implementation
 * Based on bpsink.c but modified to be non-blocking and integrated with main loop
 */

/**
 * Collection of the termination signals.
 *
 * This is NOT a signal handler: the three signals are blocked in every thread
 * (pthread_sigmask in main) and this thread picks them up with sigwait(). It
 * therefore runs in ordinary context, where logging and calling the ION API
 * are legitimate — which they were not in an asynchronous handler: dtnex_log
 * is printf, and neither pthread_join nor bp_close is async-signal-safe.
 *
 * On the first signal it lowers the running flags and wakes up whoever is
 * parked in bp_receive. It does NOT join, does not close endpoints, does not
 * detach and does not call exit: cleanup is performed by main when
 * eventDrivenLoop returns, which is by construction a point outside any SDR
 * transaction. Exiting from here while another thread is inside a transaction
 * would leave the lock held in ION's shared memory, blocking every other
 * client until ionunlock is run.
 */
static void *signalWaitThread(void *arg)
{
    sigset_t waitSet;
    int      sig;
    int      signalCount = 0;

    (void) arg;

    sigemptyset(&waitSet);
    sigaddset(&waitSet, SIGINT);
    sigaddset(&waitSet, SIGTERM);
    sigaddset(&waitSet, SIGTSTP);

    while (1) {
        if (sigwait(&waitSet, &sig) != 0) {
            continue;
        }

        signalCount++;

        if (signalCount > 1) {
            dtnex_log("⚠️  Forced exit requested: if a thread is inside an SDR "
                    "transaction, ION's lock will stay held and will block the "
                    "other clients. In that case release it with: ionunlock ion");
            _exit(1);
        }

        if (sig == SIGINT) {
            dtnex_log("Received SIGINT (Ctrl+C), shutting down...");
        } else if (sig == SIGTERM) {
            dtnex_log("Received SIGTERM, shutting down...");
        } else {
            dtnex_log("Received SIGTSTP (Ctrl+Z), shutting down instead of "
                    "suspending...");
        }

        /* Request shutdown: the main loop checks running between iterations
         * and sleeps in one-second slices, so it reacts within a second
         * (dtnex.c, eventDrivenLoop). */
        running = 0;
        bpechoState.running = 0;
        bundleReceptionState.running = 0;

        /* Wake-ups: without these the service threads would stay parked in the
         * blocking bp_receive and the join would never return. They are
         * conditional, because a signal can arrive with ION unreachable. */
        if (ionConnected) {
            if (sap != NULL) {
                bp_interrupt(sap);
            }

            if (bpechoState.sap != NULL) {
                bp_interrupt(bpechoState.sap);
                ionPauseAttendant(&bpechoState.attendant);
            }
        }
    }

    return NULL;
}

/**
 * A thin wrapper around the ion_contacts module: prints the diagnostic table,
 * explicitly checks that ION is alive and consistent (§6.6), then produces the
 * snapshot of announceable contacts and the graph.
 */
void getContacts(DtnexConfig *config) {
    int contactCount;
    int alive;

    contactCount = ionc_print_contact_table(config->debugMode);

    if (contactCount < 0) {
        // ION unreachable: this may be a restart or a disconnection.
        dtnex_log("⚠️  Cannot access ION contact database - ION may have been restarted");

        if (sap != NULL) {
            bp_close(sap);
            sap = NULL;
        }
        ionConnected = 0;
        restartDtnex(config);
        return;
    }

    // Explicit restart detection (§6.6): "zero contacts" is NOT a hint of a
    // restart, a freshly started node legitimately has zero.
    alive = ionc_check_alive(config->nodeId);
    if (alive != 1) {
        dtnex_log("⚠️  ION restart detected (ownNodeNbr is no longer %lu)", config->nodeId);

        if (sap != NULL) {
            bp_close(sap);
            sap = NULL;
        }
        ionConnected = 0;
        restartDtnex(config);
        return;
    }

    if (!config->debugMode) {
        log_contact_update(config, contactCount);
    }

    // Snapshot of the announceable contacts: this is exactly what goes on the
    // wire, so it is what should be compared against ionadmin's 'l contact'.
    if (config->debugMode) {
        ContactRecord snapshot[IONC_MAX_CONTACTS];
        int snapshotCount = ionc_get_own_contacts(config->nodeId, snapshot,
                IONC_MAX_CONTACTS, config->debugMode);

        if (snapshotCount < 0) {
            dtnex_log("⚠️  Could not read the snapshot of announceable contacts");
        } else {
            dtnex_log("\033[36mAnnounceable contacts (fromNode == %lu): %d\033[0m",
                    config->nodeId, snapshotCount);
            for (int s = 0; s < snapshotCount; s++) {
                dtnex_log("  %lu→%lu  from=%ld to=%ld  xmitRate=%lu B/s  conf=%u%%  owlt=%us",
                        snapshot[s].fromNode, snapshot[s].toNode,
                        (long) snapshot[s].fromTime, (long) snapshot[s].toTime,
                        snapshot[s].xmitRate, snapshot[s].confidence,
                        snapshot[s].owlt);
            }
        }
    }

    if (config->createGraph) {
        createGraph(config);
    }
}

/**
 * Generate a graph visualization file (Graphviz .gv format)
 * Based on creategraph function in dtnex.sh
 */
void createGraph(DtnexConfig *config) {
    int i;
    time_t currentTime;
    FILE *graphFile;
    
    if (!config->createGraph) {
        // This check is redundant as the main loop already checks, but kept for safety
        return;
    }
    
    // Use the exact file path from config - don't automatically append .gv
    char graphvizFile[256];
    strncpy(graphvizFile, config->graphFile, sizeof(graphvizFile) - 1);
    graphvizFile[sizeof(graphvizFile) - 1] = '\0';

    /* The rendered image must not overwrite the GraphViz source: the hint
     * below used to name the same file as both input and output, so anyone
     * pasting it destroyed the very file dot was reading. Derive a distinct
     * name by dropping a trailing .gv, when present, and appending .png. */
    char imageFile[300];
    size_t graphvizFileLen = strlen(graphvizFile);
    if (graphvizFileLen > 3
            && strcmp(graphvizFile + graphvizFileLen - 3, ".gv") == 0) {
        snprintf(imageFile, sizeof(imageFile), "%.*s.png",
                (int) (graphvizFileLen - 3), graphvizFile);
    } else {
        snprintf(imageFile, sizeof(imageFile), "%s.png", graphvizFile);
    }

    graphFile = fopen(graphvizFile, "w");
    if (!graphFile) {
        dtnex_log("Failed to open graph file for writing: %s", graphvizFile);
        return;
    }
    
    // Write the comment header with instructions
    fprintf(graphFile, "// DTN Contact Graph generated by DTNEXC\n");
    fprintf(graphFile, "// To generate an image from this file, run:\n");
    fprintf(graphFile, "// dot -Tpng %s -o %s\n", graphvizFile, imageFile);
    fprintf(graphFile, "// You can also use other formats like: -Tsvg, -Tpdf, -Tjpg\n\n");
    
    // Write the graph header (bash-compatible format)
    fprintf(graphFile, "digraph G { layout=neato; overlap=false;\n");
    
    // Add nodes to the graph from in-memory metadata list
    // Note: The nodeMetadataList is kept in memory and only written to nodesmetadata.txt
    // when createGraph is true, so we rely on the in-memory list here
    for (i = 0; i < nodeMetadataCount; i++) {
        // Prepare metadata string (escape special chars)
        char escapedMetadata[MAX_METADATA_LENGTH * 2] = {0};
        char *metadata = nodeMetadataList[i].metadata;
        char *dst = escapedMetadata;
        
        // Escape special characters
        while (*metadata) {
            if (*metadata == '@') {
                *dst++ = '&'; *dst++ = '#'; *dst++ = '6'; *dst++ = '4'; *dst++ = ';';
            }
            else if (*metadata == '.') {
                *dst++ = '&'; *dst++ = '#'; *dst++ = '4'; *dst++ = '6'; *dst++ = ';';
            }
            else if (*metadata == ',') {
                *dst++ = '<'; *dst++ = 'b'; *dst++ = 'r'; *dst++ = '/'; *dst++ = '>';
            }
            else {
                *dst++ = *metadata;
            }
            metadata++;
        }
        *dst = '\0';
        
        // Add the node with bash-compatible styling
        fprintf(graphFile, "\"ipn:%lu\" [label=< <FONT POINT-SIZE=\"14\" FACE=\"Arial\" COLOR=\"darkred\"><B>ipn:%lu</B></FONT><BR/><FONT POINT-SIZE=\"10\" FACE=\"Arial\" COLOR=\"blue\">%s</FONT>>];\n", 
                nodeMetadataList[i].nodeId, nodeMetadataList[i].nodeId, escapedMetadata);
    }
    
    // Add self node
    char escapedMetadata[MAX_METADATA_LENGTH * 2] = {0};
    char *metadata = config->nodemetadata;
    char *dst = escapedMetadata;
    
    // Escape special characters
    while (*metadata) {
        if (*metadata == '@') {
            *dst++ = '&'; *dst++ = '#'; *dst++ = '6'; *dst++ = '4'; *dst++ = ';';
        }
        else if (*metadata == '.') {
            *dst++ = '&'; *dst++ = '#'; *dst++ = '4'; *dst++ = '6'; *dst++ = ';';
        }
        else if (*metadata == ',') {
            *dst++ = '<'; *dst++ = 'b'; *dst++ = 'r'; *dst++ = '/'; *dst++ = '>';
        }
        else {
            *dst++ = *metadata;
        }
        metadata++;
    }
    *dst = '\0';
    
    // Add local node with bash-compatible formatting
    fprintf(graphFile, "\"ipn:%lu\" [label=< <FONT POINT-SIZE=\"14\" FACE=\"Arial\" COLOR=\"darkred\"><B>ipn:%lu</B></FONT><BR/><FONT POINT-SIZE=\"10\" FACE=\"Arial\" COLOR=\"blue\">%s</FONT>>];\n", 
            config->nodeId, config->nodeId, escapedMetadata);
    
    // Get contacts using ionadmin command (same as bash version)
    int contactCount = 0;
    debug_log(config, "Extracting contacts using ionadmin command...");
    
    FILE *ionadmin_pipe = popen("echo 'l contact' | ionadmin 2>/dev/null | grep -o -P '(?<=From).*?(?=is)'", "r");
    /** How the contacts used to draw the edges between nodes are obtained:
     * send 'l contact' to ionadmin's stdin,
     * capture the output,
     * filter it with grep to keep only the lines holding the contacts.
     */
    if (ionadmin_pipe) {
        char line[1024];
        while (fgets(line, sizeof(line), ionadmin_pipe) != NULL) {
            // Remove trailing newline
            line[strcspn(line, "\n")] = 0;
            
            if (strlen(line) > 0) {
                debug_log(config, "Processing contact line: '%s'", line);
                
                // Split line into words (same logic as bash version)
                char *words[20];  // Enough space for all words
                int wordCount = 0;
                char *token = strtok(line, " ");
                while (token != NULL && wordCount < 20) {
                    words[wordCount++] = token;
                    token = strtok(NULL, " ");
                }
                
                // In bash version: nodearray[8] and nodearray[11] are the node numbers
                if (wordCount > 11) {
                    unsigned long fromNode = atol(words[8]);
                    unsigned long toNode = atol(words[11]);
                    
                    if (fromNode > 0 && toNode > 0) {
                        fprintf(graphFile, "\"ipn:%lu\" -> \"ipn:%lu\"\n", fromNode, toNode);
                        contactCount++;
                        debug_log(config, "Added contact: %lu -> %lu", fromNode, toNode);
                    }
                } else {
                    debug_log(config, "Not enough words in contact line (%d words)", wordCount);
                }
            }
        }
        pclose(ionadmin_pipe);
    } else {
        debug_log(config, "Failed to execute ionadmin command");
    }
    
    // Close the graph (bash-compatible format)
    time(&currentTime);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d_%H-%M-%S", localtime(&currentTime));
    fprintf(graphFile, "labelloc=\"t\"; label=\"IPNSIG's DTN Network Graph, Updated:%s\"}\n", timeStr);
    
    // Ensure all data is written and properly close the file
    fflush(graphFile);
    fclose(graphFile);
    
    dtnex_log("[INFO] Graph file updated with %d contacts: %s", contactCount, graphvizFile);
    
    // Write the metadata list to a file in the same directory as the .gv file
    // Extract the directory path from the graphvizFile
    char metadataFilePath[512];
    char *lastSlash = strrchr(graphvizFile, '/');
    
    if (lastSlash) {
        // If graphvizFile includes a path, use the same path
        size_t dirLen = lastSlash - graphvizFile + 1;
        strncpy(metadataFilePath, graphvizFile, dirLen);
        metadataFilePath[dirLen] = '\0';
        strcat(metadataFilePath, "metadata_list.txt");
    } else {
        // If graphvizFile is just a filename, use the current directory
        strcpy(metadataFilePath, "metadata_list.txt");
    }
    
    // Write metadata list to file
    FILE *metadataFile = fopen(metadataFilePath, "w");
    if (metadataFile) {
        fprintf(metadataFile, "# DTN Metadata List - Generated by DTNEXC on %s\n\n", timeStr);
        fprintf(metadataFile, "NODE ID    | METADATA\n");
        fprintf(metadataFile, "------------------------------------------------------------\n");
        
        // First write our own node's metadata
        fprintf(metadataFile, "%-10lu | %s (LOCAL NODE)\n", config->nodeId, config->nodemetadata);
        
        // Then write all other nodes' metadata
        for (i = 0; i < nodeMetadataCount; i++) {
            if (nodeMetadataList[i].nodeId != config->nodeId) {
                fprintf(metadataFile, "%-10lu | %s\n", 
                        nodeMetadataList[i].nodeId, nodeMetadataList[i].metadata);
            }
        }
        
        fprintf(metadataFile, "\n# Total nodes: %d\n", nodeMetadataCount + 1); // +1 for our own node
        
        fflush(metadataFile);
        fclose(metadataFile);
        debug_log(config, "[INFO] Metadata list written to %s", metadataFilePath);
    } else {
        dtnex_log("[ERROR] Failed to write metadata list to %s", metadataFilePath);
    }
    
    // Print a complete list of all metadata after graph generation (debug mode only)
    debug_log(config, "======== METADATA USED FOR GRAPH GENERATION (%d nodes) ========", nodeMetadataCount);
    debug_log(config, "NODE ID    | METADATA");
    debug_log(config, "----------------------------------------");
    
    // Print our own node's metadata
    debug_log(config, "%-10lu | %s (LOCAL NODE)", config->nodeId, config->nodemetadata);
    
    // Print all other nodes' metadata
    for (i = 0; i < nodeMetadataCount; i++) {
        // Skip our own node as we already printed it
        if (nodeMetadataList[i].nodeId != config->nodeId) {
            debug_log(config, "%-10lu | %s", 
                    nodeMetadataList[i].nodeId, nodeMetadataList[i].metadata);
        }
    }
    debug_log(config, "========================================");
}


/**
 * Initialize the bpecho service
 */
int initBpechoService(DtnexConfig *config, BpechoState *state) {
    char bpechoEid[MAX_EID_LENGTH];
    
    dtnex_log("Initializing bpecho service...");
    
    // Format the endpoint ID as ipn:node.service
    snprintf(bpechoEid, MAX_EID_LENGTH, "ipn:%lu.%s", config->nodeId, config->bpechoServiceNr);
    dtnex_log("Using bpecho endpoint: %s", bpechoEid);
    
    // First try to add/register the bpecho endpoint in ION's routing database
    if (addEndpoint(bpechoEid, EnqueueBundle, NULL) < 0) {
        dtnex_log("⚠️ Warning: Could not register bpecho endpoint %s in routing database", bpechoEid);
    }
    
    // Open the bpecho endpoint
    if (bp_open(bpechoEid, &state->sap) < 0) {
        dtnex_log("❌ Failed to open bpecho endpoint");
        return -1;
    }
    
    // Initialize state
    state->running = 1;
    
    // Initialize attendant for blocking transmission
    if (ionStartAttendant(&state->attendant) < 0) {
        dtnex_log("❌ Failed to initialize blocking transmission for bpecho");
        bp_close(state->sap);
        return -1;
    }
    
    dtnex_log("✅ Bpecho service initialized successfully");
    return 0;
}

/**
 * Run the bpecho service (based on bpecho.c)
 */
void *runBpechoService(void *arg) {
    DtnexConfig *config = (DtnexConfig *)arg;
    static char dlvmarks[] = "?.*!X";  // Delivery status markers
    Sdr sdr;
    char dataToSend[BPECHO_ADU_LEN];
    Object bundleZco;
    Object newBundle;
    Object extent;
    BpDelivery dlv;
    ZcoReader reader;
    char sourceEid[MAX_EID_LENGTH];
    int bytesToEcho = 0;
    int result;
    
    // The bpecho thread does not touch signals: it inherited them blocked from
    // the mask set in main. It stops through bpechoState.running plus the
    // wake-up from bp_interrupt, both driven by signalWaitThread.

    sdr = bp_get_sdr();
    dtnex_log("Starting bpecho service thread on service %s", config->bpechoServiceNr);
    
    while (bpechoState.running) {
        // Wait for a bundle from a sender
        if (bp_receive(bpechoState.sap, &dlv, BP_BLOCKING) < 0) {
            dtnex_log("❌ Bpecho bundle reception failed");
            bpechoState.running = 0;
            break;
        }
        
        // Debug indicators for different status codes
        putchar(dlvmarks[dlv.result]);
        fflush(stdout);
        
        // Handle different reception results
        if (dlv.result == BpReceptionInterrupted) {
            continue;
        }
        
        if (dlv.result == BpEndpointStopped) {
            bpechoState.running = 0;
            continue;
        }
        
        if (dlv.result == BpPayloadPresent) {
            // Got a bundle with payload, echo it back
            
            // Save the source EID
            istrcpy(sourceEid, dlv.bundleSourceEid, sizeof(sourceEid));
            
            // Calculate how many bytes to echo (limit to our buffer size)
            bytesToEcho = MIN(zco_source_data_length(sdr, dlv.adu), BPECHO_ADU_LEN);
            
            // Set up for reading the payload
            zco_start_receiving(dlv.adu, &reader);
            
            // Read the payload data
            CHKZERO(sdr_begin_xn(sdr));
            result = zco_receive_source(sdr, &reader, bytesToEcho, dataToSend);
            
            if (sdr_end_xn(sdr) < 0 || result < 0) {
                dtnex_log("❌ Can't receive payload for echo");
                bp_release_delivery(&dlv, 1);
                continue;
            }
            
            // Parse source node number if available
            unsigned long sourceNode = 0;
            if (strncmp(sourceEid, "ipn:", 4) == 0) {
                sscanf(sourceEid, "ipn:%lu", &sourceNode);
            }
            
            // Echo message received with enhanced information
            dtnex_log("\033[32m[BPECHO] Received %d bytes from %s\033[0m", 
                bytesToEcho, sourceEid);
            
            // Release the delivery
            bp_release_delivery(&dlv, 1);
            
            // Skip anonymous senders
            if (strcmp(sourceEid, "dtn:none") == 0) {
                dtnex_log("\033[33m[WARN] Anonymous sender - echo reply skipped\033[0m");
                continue;
            }
            
            // Prepare to send echo reply
            CHKZERO(sdr_begin_xn(sdr));
            extent = sdr_malloc(sdr, bytesToEcho);
            if (extent) {
                sdr_write(sdr, extent, dataToSend, bytesToEcho);
            }
            
            if (sdr_end_xn(sdr) < 0) {
                dtnex_log("❌ No space for ZCO extent for echo reply");
                continue;
            }
            
            // Create ZCO for the echo data
            bundleZco = ionCreateZco(ZcoSdrSource, extent, 0, bytesToEcho,
                                   BP_STD_PRIORITY, 0, ZcoOutbound, &bpechoState.attendant);
            
            if (bundleZco == 0 || bundleZco == (Object) ERROR) {
                dtnex_log("❌ Can't create ZCO for echo reply");
                continue;
            }
            
            // Send the echo reply
            if (bp_send(bpechoState.sap, sourceEid, NULL, 300, BP_STD_PRIORITY,
                      NoCustodyRequested, 0, 0, NULL, bundleZco, &newBundle) < 1) {
                dtnex_log("\033[31m[ERROR] Failed to send bpecho reply\033[0m");
                continue;
            }
            
            // Enhanced output with node number
            unsigned long replyNode = 0;
            if (strncmp(sourceEid, "ipn:", 4) == 0) {
                sscanf(sourceEid, "ipn:%lu", &replyNode);
            }
            
            dtnex_log("\033[33m[BPECHO] Reply sent to %s - %d bytes\033[0m", 
                     sourceEid, bytesToEcho);
        } else {
            // Other status (timeout, etc)
            bp_release_delivery(&dlv, 1);
        }
    }
    
    // Clean up
    dtnex_log("🧹 Shutting down bpecho service...");
    if (bpechoState.sap != NULL) {
        bp_close(bpechoState.sap);
        bpechoState.sap = NULL;
    }
    ionStopAttendant(&bpechoState.attendant);
    
    dtnex_log("✅ Bpecho service terminated normally");
    return NULL;
}

/**
 * Initialize bundle reception service
 */
int initBundleReception(DtnexConfig *config, BundleReceptionState *state) {
    state->config = config;
    state->running = 1;
    return 0;
}

/**
 *  Reception pipeline
 * runBundleReception()          ← dedicated thread, reads bundles from ION
    └── processCborMessage()  ← entry point, minimal validation
            └── decodeCborMessage()   ← CBOR parsing + authentication
                    ├── processCborContactMessage()   ← type "c"
                    │       └── forwardCborContactMessage()
                    └── processCborMetadataMessage()  ← type "m"
                            └── forwardCborMetadataMessage()
 */

/**
 * Bundle reception thread - handles incoming DTNEX CBOR messages
 * Uses blocking reception pattern like bpsink
 */
void *runBundleReception(void *arg) {
    BundleReceptionState *state = (BundleReceptionState *)arg;
    DtnexConfig *config = state->config;
    BpDelivery dlv;
    Sdr sdr;
    char buffer[MAX_LINE_LENGTH];
    ZcoReader reader;
    
    dtnex_log("📥 Starting bundle reception thread");
    
    sdr = bp_get_sdr();
    
    while (state->running && running) {
        // Clear the delivery structure
        memset(&dlv, 0, sizeof(BpDelivery));
        
        // Wait for a bundle using blocking reception (like bpsink)
        if (bp_receive(sap, &dlv, BP_BLOCKING) < 0) {
            if (!running) break; // Normal shutdown
            dtnex_log("❌ Bundle reception failed, thread terminating");
            state->running = 0;
            break;
        }
        
        // Handle different reception results
        if (dlv.result == BpReceptionInterrupted) {
            continue;
        }
        
        if (dlv.result == BpEndpointStopped) {
            dtnex_log("❌ Endpoint stopped, bundle reception thread terminating");
            state->running = 0;
            running = 0;
            break;
        }
        
        if (dlv.result == BpPayloadPresent) {
            // Get source node information
            unsigned long sourceNode = 0;
            if (dlv.bundleSourceEid && strncmp(dlv.bundleSourceEid, "ipn:", 4) == 0) {
                sscanf(dlv.bundleSourceEid, "ipn:%lu", &sourceNode);
            }
            
            // Get content length
            int contentLength = 0;
            CHKZERO(sdr_begin_xn(sdr));
            contentLength = zco_source_data_length(sdr, dlv.adu);
            sdr_exit_xn(sdr);
            
            if (contentLength > 0 && contentLength < MAX_LINE_LENGTH) {
                // Read the bundle content
                zco_start_receiving(dlv.adu, &reader); // initialises the zco reader for the bundle payload, the equivalent of an open
                CHKZERO(sdr_begin_xn(sdr));
                int len = zco_receive_source(sdr, &reader, contentLength, buffer); // copies the payload into the buffer
                if (sdr_end_xn(sdr) < 0 || len < 0) {
                    dtnex_log("❌ Error reading bundle content");
                    bp_release_delivery(&dlv, 1);
                    continue;
                }
                
                // Build source info string
                // Source info is unused for now, but may be useful for future logging and debugging
                char sourceInfo[128];
                if (dlv.bundleSourceEid && strlen(dlv.bundleSourceEid) > 0) {
                    snprintf(sourceInfo, sizeof(sourceInfo), "%s", dlv.bundleSourceEid);
                } else if (sourceNode > 0) {
                    snprintf(sourceInfo, sizeof(sourceInfo), "ipn:%lu", sourceNode);
                } else {
                    snprintf(sourceInfo, sizeof(sourceInfo), "unknown");
                }
                
                // Bundle received - will be logged in message processing
                
                // Process the CBOR message
                // The bundle payload is now in buffer with length contentLength -> it gets processed
                processCborMessage(config, (unsigned char*)buffer, contentLength);
            } else {
                dtnex_log("⚠️ Bundle content invalid size (%d bytes), skipping", contentLength);
            }
        }
        
        // Always release the delivery
        bp_release_delivery(&dlv, 1);
    }
    
    dtnex_log("📥 Bundle reception thread terminated normally");
    return NULL;
}

/**
 * Stop bundle reception service
 */
void stopBundleReception(BundleReceptionState *state) {
    if (state) {
        state->running = 0;
        // Interrupt the blocking receive call only if ION is connected
        if (sap && ionConnected) {
            bp_interrupt(sap);
        }
    }
}

/**
 * Main function - Single-threaded implementation based on bpsink pattern
 */
int main(int argc, char **argv) {
    DtnexConfig config;

    /* The termination signals must be blocked BEFORE any thread is born —
     * including those ION may create inside bp_attach — because the mask is
     * inherited at creation time. From here on no thread receives them
     * asynchronously: signalWaitThread collects them with sigwait. */
    sigset_t terminationSignals;

    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGINT);
    sigaddset(&terminationSignals, SIGTERM);
    sigaddset(&terminationSignals, SIGTSTP);

    if (pthread_sigmask(SIG_BLOCK, &terminationSignals, NULL) != 0) {
        dtnex_log("❌ Cannot block the termination signals: "
                "shutdown would not be safe for ION, exiting");
        return 1;
    }

    if (pthread_create(&signalThread, NULL, signalWaitThread, NULL) != 0) {
        dtnex_log("❌ Cannot create the signal thread: "
                "shutdown would not be safe for ION, exiting");
        return 1;
    }

    // Store original arguments for potential restart
    original_argc = argc;
    original_argv = argv;

    // Load configuration
    loadConfig(&config);

    /* Command-line flags, applied AFTER loadConfig so that an explicit flag
     * wins over dtnex.conf. Until now argv was only kept for the re-exec and
     * every argument was ignored in silence, so `dtnex --debug` behaved
     * exactly like `dtnex` while the documentation promised otherwise.
     *
     * The flags survive an ION-restart re-exec for free: execv replays this
     * same argv (see restartDtnex). */
    for (int argIndex = 1; argIndex < argc; argIndex++) {
        if (strcmp(argv[argIndex], "--debug") == 0) {
            config.debugMode = 1;
        } else if (strcmp(argv[argIndex], "--service") == 0) {
            config.serviceMode = 1;
        } else {
            dtnex_log("⚠️  Unknown argument '%s' — supported: --debug, --service",
                    argv[argIndex]);
        }
    }

    // Initialize - now returns an error code if it fails
    if (init(&config) < 0) {
        dtnex_log("Initialization failed, exiting");
        return 1;
    }
    
    // Initialize bpecho service only if ION is connected
    if (ionConnected) {
        if (initBpechoService(&config, &bpechoState) < 0) {
            dtnex_log("⚠️ Bpecho service initialization failed, continuing without it");
        } else {
            // Create bpecho service thread
            if (pthread_create(&bpechoThread, NULL, runBpechoService, (void *)&config) != 0) {
                dtnex_log("⚠️ Failed to create bpecho service thread, continuing without it");
                // Clean up bpecho resources
                bp_close(bpechoState.sap);
                ionStopAttendant(&bpechoState.attendant);
            } else {
                bpechoThreadStarted = 1;
                dtnex_log("✅ Bpecho service thread started");
            }
        }
    } else {
        dtnex_log("⚠️ Skipping bpecho service initialization (ION not connected)");
    }
    
    // Initialize bundle reception service only if ION is connected
    if (ionConnected) {
        if (initBundleReception(&config, &bundleReceptionState) < 0) {
            dtnex_log("❌ Bundle reception service initialization failed");
            return 1;
        } else {
            // Create bundle reception thread
            if (pthread_create(&bundleReceptionState.thread, NULL, runBundleReception, (void *)&bundleReceptionState) != 0) {
                dtnex_log("❌ Failed to create bundle reception thread");
                return 1;
            } else {
                bundleReceptionThreadStarted = 1;
                dtnex_log("✅ Bundle reception thread started");
            }
        }
    } else {
        dtnex_log("⚠️ Skipping bundle reception service initialization (ION not connected)");
    }
    
    dtnex_log("DTNEXC running - Ctrl+C to exit");
    
    // Perform startup contact broadcast - send our contact list to all neighbors
    Plan plans[MAX_PLANS];
    int planCount = 0;
    
    dtnex_log("🚀 Performing startup contact broadcast to all neighbors...");
    getplanlist(&config, plans, &planCount);
    if (planCount > 0) {
        exchangeWithNeighbors(&config, plans, planCount);
        dtnex_log("✅ Startup contact broadcast completed to %d neighbors", planCount);
    } else {
        dtnex_log("⚠️ No neighbors found for startup broadcast");
    }
    
    // Main loop - use event-driven loop instead of continuous polling
    eventDrivenLoop(&config);
    
    // Clean up
    dtnex_log("Shutting down...");
    
    // Wait for bundle reception thread to terminate if it was ever started
    if (bundleReceptionThreadStarted) {
        dtnex_log("Waiting for bundle reception thread to terminate...");
        stopBundleReception(&bundleReceptionState);
        pthread_join(bundleReceptionState.thread, NULL);
        bundleReceptionThreadStarted = 0;
    }

    // Wait for bpecho thread to terminate if it was ever started
    if (bpechoThreadStarted) {
        dtnex_log("Waiting for bpecho service to terminate...");
        pthread_join(bpechoThread, NULL);
        bpechoThreadStarted = 0;
    }
    
    // Close the BP endpoint gracefully if we have one
    if (sap != NULL) {
        dtnex_log("🔌 Closing BP endpoint");
        bp_close(sap);
    }
    
    // Detach from BP
    dtnex_log("🧹 Detaching from ION BP system");
    bp_detach();
    
    dtnex_log("DTNEXC terminated normally");
    return 0;
}

/* ========================================================================
 * CBOR Message Implementation Functions
 * ======================================================================== */

/**
 * Generate a cryptographically random nonce
 */
void generateNonce(unsigned char *nonce) {
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(nonce, 1, DTNEX_NONCE_SIZE, urandom);
        fclose(urandom);
    } else {
        // Fallback to time-based pseudo-random
        srand(time(NULL));
        for (int i = 0; i < DTNEX_NONCE_SIZE; i++) {
            nonce[i] = rand() & 0xFF;
        }
    }
}

/**
 * Computes the HMAC-SHA256 of the message, truncated to DTNEX_HMAC_SIZE bytes.
 *
 * Until v3.00, the RFC 2104 construction was hand-written using OpenSSL's
 * SHA256_Init/Update/Final APIs, deprecated since 3.0. HMAC() is not
 * deprecated and produces the exact same bytes: the wire format does not
 * change (verified with dev/hmac_check.c).
 *
 * On error, clears the buffer and returns 0: verifyHmac compares byte by
 * byte, so a failure causes the message to be discarded instead of accepted.
 */
int calculateHmac(const unsigned char *message, int msgLen, const char *key, unsigned char *hmac) {
    unsigned char fullHmac[SHA256_DIGEST_SIZE];
    unsigned int  fullLen = 0;

    if (HMAC(EVP_sha256(), key, (int) strlen(key), message, (size_t) msgLen,
            fullHmac, &fullLen) == NULL
            || fullLen != SHA256_DIGEST_SIZE) {
        dtnex_log("❌ HMAC-SHA256 computation failed");
        memset(hmac, 0, DTNEX_HMAC_SIZE);
        return 0;
    }

    memcpy(hmac, fullHmac, DTNEX_HMAC_SIZE);
    return DTNEX_HMAC_SIZE;
}

/**
 * Writes the envelope common to all DTNEX messages: the 9-element outer
 * array and the first 7 fields.
 *
 *   [version, type, timestamp, expireTime, origin, from, nonce, ...]
 *
 * The originator of a message passes fresh timestamp, expireTime and nonce,
 * with origin == from == the local node; a forwarder passes the original
 * ones and only replaces "from". The structure is identical in both cases —
 * that's exactly why a single helper is correct.
 *
 * Returns the number of bytes written and advances *cursor.
 */
static int writeCborEnvelope(unsigned char **cursor, const char *type,
        time_t timestamp, time_t expireTime, unsigned long origin,
        unsigned long from, const unsigned char *nonce) {
    int bytesWritten = 0;

    bytesWritten += cbor_encode_array_open(9, cursor);
    bytesWritten += cbor_encode_integer(DTNEX_PROTOCOL_VERSION, cursor);
    bytesWritten += cbor_encode_text_string((char *) type, 1, cursor);
    bytesWritten += cbor_encode_integer((uvast) timestamp, cursor);
    bytesWritten += cbor_encode_integer((uvast) expireTime, cursor);
    bytesWritten += cbor_encode_integer(origin, cursor);
    bytesWritten += cbor_encode_integer(from, cursor);
    bytesWritten += cbor_encode_byte_string((unsigned char *) nonce,
            DTNEX_NONCE_SIZE, cursor);

    return bytesWritten;
}

/**
 * Computes the HMAC over the first bytesWritten bytes of the message — i.e.
 * everything except the HMAC itself — and appends it as the ninth element.
 *
 * If calculateHmac fails, the zeroed buffer it produces is appended anyway
 * and the failure is logged: a null HMAC gets discarded by every receiver,
 * so the system stays fail-closed without the encoder having to propagate
 * the error.
 *
 * Returns the number of bytes written and advances *cursor.
 */
static int appendCborHmac(DtnexConfig *config, unsigned char *buffer,
        int bytesWritten, unsigned char **cursor) {
    unsigned char hmac[DTNEX_HMAC_SIZE];

    if (calculateHmac(buffer, bytesWritten, config->presSharedNetworkKey, hmac) != DTNEX_HMAC_SIZE) {
        debug_log(config, "❌ HMAC calculation failed during encoding: appending a null HMAC");
    }
    return cbor_encode_byte_string(hmac, DTNEX_HMAC_SIZE, cursor);
}

/**
 * Verify HMAC
 */
int verifyHmac(DtnexConfig *config, const unsigned char *message, int msgLen, const unsigned char *receivedHmac, const char *key) {
    unsigned char calculatedHmac[DTNEX_HMAC_SIZE];

    // If HMAC computation fails, the message must be discarded, not accepted:
    // we cannot compare a buffer that was not actually computed.
    if (calculateHmac(message, msgLen, key, calculatedHmac) != DTNEX_HMAC_SIZE) {
        debug_log(config, "❌ HMAC computation failed during verification: message discarded");
        return 0;
    }

    // Debug logging for HMAC comparison
    debug_log(config, "🔍 HMAC verification details:");
    debug_log(config, "Message length: %d bytes", msgLen);
    debug_log(config, "Key: '%s'", key);
    
    if (config->debugMode) {
        printf("\033[90m[DEBUG] Calculated HMAC: ");
        for (int i = 0; i < DTNEX_HMAC_SIZE; i++) {
            printf("%02x", calculatedHmac[i]);
        }
        printf("\n[DEBUG] Received HMAC:   ");
        for (int i = 0; i < DTNEX_HMAC_SIZE; i++) {
            printf("%02x", receivedHmac[i]);
        }
        printf("\033[0m\n");
    }
    
    int result = memcmp(calculatedHmac, receivedHmac, DTNEX_HMAC_SIZE) == 0;
    debug_log(config, "HMAC match: %s", result ? "YES" : "NO");
    return result;
}

/**
 * Check if nonce is duplicate (replay protection)
 */
int isNonceDuplicate(unsigned char *nonce, unsigned long origin) {
    for (int i = 0; i < nonceCacheCount; i++) {
        if (nonceCache[i].origin == origin && 
            memcmp(nonceCache[i].nonce, nonce, DTNEX_NONCE_SIZE) == 0) {
            return 1; // Duplicate found
        }
    }
    return 0;
}

/**
 * Add nonce to cache (FIFO)
 */
void addNonceToCache(unsigned char *nonce, unsigned long origin) {
    if (nonceCacheCount < MAX_HASH_CACHE) {
        // Still have space
        memcpy(nonceCache[nonceCacheCount].nonce, nonce, DTNEX_NONCE_SIZE);
        nonceCache[nonceCacheCount].origin = origin;
        nonceCache[nonceCacheCount].timestamp = time(NULL);
        nonceCacheCount++;
    } else {
        // Cache full, implement FIFO
        for (int i = 0; i < MAX_HASH_CACHE - 1; i++) {
            nonceCache[i] = nonceCache[i + 1];
        }
        memcpy(nonceCache[MAX_HASH_CACHE - 1].nonce, nonce, DTNEX_NONCE_SIZE);
        nonceCache[MAX_HASH_CACHE - 1].origin = origin;
        nonceCache[MAX_HASH_CACHE - 1].timestamp = time(NULL);
    }
}

/**
 * Encode CBOR contact message
 * Format v3 (§5.2): [version, type, timestamp, expireTime, origin, from, nonce,
 *                     [fromNode, toNode, fromTime, toTime, xmitRate, confidence, owlt], hmac]
 */
int encodeCborContactMessage(DtnexConfig *config, ContactRecord *contact, unsigned char *buffer, int bufferSize) {
    unsigned char *cursor = buffer;
    unsigned char nonce[DTNEX_NONCE_SIZE];
    int bytesWritten = 0;

    (void) bufferSize;  // the v3 payload is ~67 bytes, MAX_CBOR_BUFFER is 128

    generateNonce(nonce);

    // §7.2: the message is useful exactly as long as the contact it describes
    // is valid, so expireTime IS the contact's toTime.
    bytesWritten += writeCborEnvelope(&cursor, "c", time(NULL), contact->toTime,
            config->nodeId, config->nodeId, nonce);

    // Contact data array v3 (§5.2): 7 fields, absolute times
    bytesWritten += cbor_encode_array_open(7, &cursor);
    bytesWritten += cbor_encode_integer(contact->fromNode, &cursor);
    bytesWritten += cbor_encode_integer(contact->toNode, &cursor);
    bytesWritten += cbor_encode_integer((uvast) contact->fromTime, &cursor);
    bytesWritten += cbor_encode_integer((uvast) contact->toTime, &cursor);
    bytesWritten += cbor_encode_integer(contact->xmitRate, &cursor);
    bytesWritten += cbor_encode_integer(contact->confidence, &cursor);
    bytesWritten += cbor_encode_integer(contact->owlt, &cursor);

    bytesWritten += appendCborHmac(config, buffer, bytesWritten, &cursor);

    debug_log(config, "[CBOR] Encoded contact message: %d bytes", bytesWritten);
    return bytesWritten;
}

/**
 * Encode CBOR metadata message  
 * Format: [version, type, timestamp, expireTime, origin, from, nonce, metadata_data, hmac]
 */
int encodeCborMetadataMessage(DtnexConfig *config, StructuredMetadata *metadata, unsigned char *buffer, int bufferSize) {
    unsigned char *cursor = buffer;
    unsigned char nonce[DTNEX_NONCE_SIZE];
    int bytesWritten = 0;
    
    // Generate nonce
    generateNonce(nonce);
    
    time_t currentTime = time(NULL);
    time_t expireTime = currentTime + config->contactLifetime;

    bytesWritten += writeCborEnvelope(&cursor, "m", currentTime, expireTime,
            config->nodeId, config->nodeId, nonce);

    // 8. Metadata array - format: [nodeId, name, contact, location?, lat?, lon?]
    int metadataElements = 3; // nodeId, name, contact (base)
    int hasLocation = strlen(metadata->location) > 0;
    int hasGPS = (metadata->latitude != 0 && metadata->longitude != 0);
    
    if (hasLocation && hasGPS) {
        metadataElements += 3; // Add location, latitude, and longitude
    } else if (hasGPS) {
        metadataElements += 2; // Add latitude and longitude only
    } else if (hasLocation) {
        metadataElements += 1; // Add location only
    }
    
    bytesWritten += cbor_encode_array_open(metadataElements, &cursor);
    bytesWritten += cbor_encode_integer(metadata->nodeId, &cursor);
    bytesWritten += cbor_encode_text_string(metadata->name, strlen(metadata->name), &cursor);
    bytesWritten += cbor_encode_text_string(metadata->contact, strlen(metadata->contact), &cursor);
    
    // Add fields in order: location (if available), then GPS (if available)
    if (hasLocation && hasGPS) {
        // Format: [nodeId, name, contact, location, lat, lon]
        bytesWritten += cbor_encode_text_string(metadata->location, strlen(metadata->location), &cursor);
        bytesWritten += cbor_encode_integer(metadata->latitude, &cursor);
        bytesWritten += cbor_encode_integer(metadata->longitude, &cursor);
    } else if (hasGPS) {
        // Format: [nodeId, name, contact, lat, lon]
        bytesWritten += cbor_encode_integer(metadata->latitude, &cursor);
        bytesWritten += cbor_encode_integer(metadata->longitude, &cursor);
    } else if (hasLocation) {
        // Format: [nodeId, name, contact, location]
        bytesWritten += cbor_encode_text_string(metadata->location, strlen(metadata->location), &cursor);
    }
    
    bytesWritten += appendCborHmac(config, buffer, bytesWritten, &cursor);

    debug_log(config, "[CBOR] Encoded metadata message: %d bytes", bytesWritten);
    return bytesWritten;
}

/**
 * Parse raw metadata string into structured format
 * Input format: "NodeName,contact@email.com,Location"
 */
void parseNodeMetadata(const char *rawMetadata, StructuredMetadata *metadata) {
    char buffer[MAX_METADATA_LENGTH];
    char *token;
    int fieldCount = 0;
    
    // Preserve nodeId and GPS coordinates, only clear the string fields
    unsigned long savedNodeId = metadata->nodeId;
    int savedLatitude = metadata->latitude;
    int savedLongitude = metadata->longitude;
    
    // Initialize only the string fields, preserve nodeId and GPS
    memset(metadata->name, 0, MAX_NODE_NAME_LENGTH);
    memset(metadata->contact, 0, MAX_CONTACT_INFO_LENGTH);
    memset(metadata->location, 0, MAX_LOCATION_LENGTH);
    
    // Restore preserved values
    metadata->nodeId = savedNodeId;
    metadata->latitude = savedLatitude;
    metadata->longitude = savedLongitude;
    
    // Copy raw metadata to avoid modifying original
    strncpy(buffer, rawMetadata, MAX_METADATA_LENGTH - 1);
    buffer[MAX_METADATA_LENGTH - 1] = '\0';
    
    // Parse comma-separated fields
    token = strtok(buffer, ",");
    while (token && fieldCount < 3) {
        // Remove leading/trailing whitespace
        while (*token == ' ') token++;
        
        switch (fieldCount) {
            case 0: // Node name
                strncpy(metadata->name, token, MAX_NODE_NAME_LENGTH - 1);
                break;
            case 1: // Contact info
                strncpy(metadata->contact, token, MAX_CONTACT_INFO_LENGTH - 1);
                break;
            case 2: // Location field
                strncpy(metadata->location, token, MAX_LOCATION_LENGTH - 1);
                break;
        }
        
        fieldCount++;
        token = strtok(NULL, ",");
    }
    
    // Ensure null termination
    metadata->name[MAX_NODE_NAME_LENGTH - 1] = '\0';
    metadata->contact[MAX_CONTACT_INFO_LENGTH - 1] = '\0';
    metadata->location[MAX_LOCATION_LENGTH - 1] = '\0';
}

/**
 * Send CBOR bundle with direct ION API - following bpsource.c pattern
 * This replaces the old sendBundle function for CBOR data
 */
int sendCborBundle(const char *destEid, unsigned char *cborData, int dataSize, int ttl) {
    // Check if destination EID is valid
    if (!destEid || strlen(destEid) == 0) {
        dtnex_log("\033[31m[ERROR] Invalid destination EID\033[0m");
        return -1;
    }
    
    // Get SDR and prepare to create and send bundle
    Sdr sdr = bp_get_sdr();
    Object bundleZco;
    Object extent;
    Object newBundle;
    int sendResult;
    
    // Start an SDR transaction
    if (sdr_begin_xn(sdr) < 0) {
        dtnex_log("Error starting SDR transaction for bundle creation");
        return -1;
    }
    
    // Allocate memory for the CBOR data
    extent = sdr_malloc(sdr, dataSize); // alloca spazio nell'sdr
    if (!extent) {
        dtnex_log("Failed to allocate memory for CBOR data");
        sdr_cancel_xn(sdr);
        return -1;
    }
    
    // Write the CBOR data to the SDR
    sdr_write(sdr, extent, (char*)cborData, dataSize);
    
    // End the transaction
    if (sdr_end_xn(sdr) < 0) {
        dtnex_log("No space for ZCO extent");
        return -1;
    }
    
    // Create ZCO from the extent
    // Now create a ZeroCopyObject bundle directly from the data held in SDR
    bundleZco = ionCreateZco(ZcoSdrSource, extent, 0, dataSize, 
                            BP_STD_PRIORITY, 0, ZcoOutbound, NULL);
    
    if (bundleZco == 0 || bundleZco == (Object) ERROR) {
        dtnex_log("Can't create ZCO extent");
        return -1;
    }
    
    // Send the bundle using direct ION API - no source EID for CBOR messages
    /* ION's API is not const-correct: bp_send declares char* even though it
     * doesn't modify the EID. The cast is preferable to removing the const from
     * our signature, which correctly describes how we use the pointer. */
    sendResult = bp_send(NULL, (char *) destEid, NULL, ttl, BP_STD_PRIORITY,
                        NoCustodyRequested, 0, 0, NULL, bundleZco, &newBundle);
    
    if (sendResult <= 0) {
        dtnex_log("\033[31m[ERROR] Failed to send CBOR bundle to %s\033[0m", destEid);
        return -1;
    }
    
    // Log the successful CBOR send operation with size
    // Sending handled by caller with log_message_sent
    
    return dataSize;
}

// generateNonce function is already defined earlier in the file

/**
 * Event-driven main loop - sleeps until next scheduled event
 * Avoids continuous polling and reduces CPU usage
 */

 /**
  * A stateful while loop with two parallel timelines: one for the scheduled
  * updates (updateInterval) and one for the ION connection retries.
  */
void eventDrivenLoop(DtnexConfig *config) {
    Plan plans[MAX_PLANS];
    int planCount = 0;
    time_t nextUpdateTime = 0;
    time_t nextIonRetry = 0;
    time_t currentTime;
    int messageReceived;
    // Use global ionConnected flag, but also verify current state
    if (!ionConnected) {
        ionConnected = (sap != NULL && config->nodeId != 0);
    }
    
    dtnex_log("🔄 Starting event-driven operation (update every %d minutes)", config->updateInterval / 60);
    
    // If ION is not connected, schedule immediate retry
    if (!ionConnected) {
        nextIonRetry = time(NULL);
        dtnex_log("⚠️ ION not connected - will retry every minute");
    } else {
        // Schedule first update immediately if ION is connected
        scheduleNextUpdate(config, &nextUpdateTime);
    }
    
    while (running) {
        currentTime = time(NULL);
        messageReceived = 0;
        
        // Check if we need to retry ION connection
        if (!ionConnected && currentTime >= nextIonRetry) {
            dtnex_log("🔌 Attempting to connect to ION...");
            if (tryConnectToIon(config) == 0) {
                ionConnected = 1;
                dtnex_log("✅ Successfully connected to ION");
                
                // Initialize services that were skipped during startup
                if (!bpechoState.running) {
                    dtnex_log("🚀 Initializing bpecho service after ION reconnection...");
                    if (initBpechoService(config, &bpechoState) == 0) {
                        if (pthread_create(&bpechoThread, NULL, runBpechoService, (void *)config) == 0) {
                            bpechoThreadStarted = 1;
                            dtnex_log("✅ Bpecho service thread started");
                        } else {
                            dtnex_log("❌ Failed to create bpecho service thread");
                            bpechoState.running = 0;
                        }
                    } else {
                        dtnex_log("❌ Failed to initialize bpecho service");
                    }
                }
                
                if (!bundleReceptionState.running) {
                    dtnex_log("🚀 Initializing bundle reception service after ION reconnection...");
                    if (initBundleReception(config, &bundleReceptionState) == 0) {
                        if (pthread_create(&bundleReceptionState.thread, NULL, runBundleReception, (void *)&bundleReceptionState) == 0) {
                            bundleReceptionThreadStarted = 1;
                            dtnex_log("✅ Bundle reception thread started");
                        } else {
                            dtnex_log("❌ Failed to create bundle reception thread");
                        }
                    } else {
                        dtnex_log("❌ Failed to initialize bundle reception service");
                    }
                }
                
                // Schedule first update immediately after connection
                scheduleNextUpdate(config, &nextUpdateTime);
            } else {
                // Check if ION is completely stopped
                int ionProcessCount = system("pgrep -c '^(ion|bp)' >/dev/null 2>&1");
                if (ionProcessCount != 0) {
                    // ION processes are running, quick retry
                    dtnex_log("🚨 Failed to connect to ION (processes running - may still be starting)");
                    nextIonRetry = currentTime + 10; // Retry in 10 seconds
                } else {
                    // No ION processes found, longer wait
                    dtnex_log("🚨 Failed to connect to ION (no ION processes detected)");
                    nextIonRetry = currentTime + 300; // Retry in 5 minutes
                }
            }
        }
        
        // If ION is connected, check if it's time for scheduled update
        if (ionConnected && currentTime >= nextUpdateTime) {
            // Check ION status before proceeding
            IonStatus ionStatus = checkIonStatus(config);
            if (ionStatus != ION_STATUS_RUNNING) {
                dtnex_log("⚠️ Lost connection to ION");
                ionConnected = 0;
                nextIonRetry = currentTime + 60; // Retry in 1 minute
                continue;
            }
            
            // Get current plan list
            getplanlist(config, plans, &planCount);
            
            // Only perform operations if we have ION connection
            if (ionConnected) {
                // Perform scheduled operations
                exchangeWithNeighbors(config, plans, planCount);
                getContacts(config);
            }
            
            // Display collected metadata (if debug mode)
            if (config->debugMode) {
                dtnex_log("📊 Collected metadata from %d nodes", nodeMetadataCount);
                for (int i = 0; i < nodeMetadataCount; i++) {
                    dtnex_log("  Node %lu: %s", nodeMetadataList[i].nodeId, nodeMetadataList[i].metadata);
                }
            }
            
            // Generate graph if enabled (independent of ION status)
            if (config->createGraph) {
                createGraph(config);
            }
            
            // Schedule next update
            scheduleNextUpdate(config, &nextUpdateTime);
        }
        
        // Generate graph even if ION is not connected, on first run or when the schedule would trigger
        // This ensures graph generation works even when ION is down
        if (config->createGraph && !ionConnected && currentTime >= nextUpdateTime) {
            createGraph(config);
            // Schedule next update even when ION is down
            scheduleNextUpdate(config, &nextUpdateTime);
        }
        
        // Bundle reception is now handled by the dedicated thread
        if (!running) break;
        
        // On every wake-up (<= 60s): re-evaluate the announcement triggers
        // (§7.1). exchangeWithNeighbors decides on its own whether there is
        // anything to do.
        if (ionConnected) {
            getplanlist(config, plans, &planCount);
            exchangeWithNeighbors(config, plans, planCount);
            getContacts(config);
        }
        
        // Calculate sleep time until next event
        currentTime = time(NULL);
        time_t sleepTime;
        
        if (!ionConnected) {
            // When ION is not connected, sleep until next retry
            sleepTime = nextIonRetry - currentTime;
        } else {
            // When ION is connected, sleep until next update
            sleepTime = nextUpdateTime - currentTime;
        }
        
        if (sleepTime > 0) {
            // Sleep in 1-second increments to allow for responsive Ctrl+C handling
            int actualSleepTime = (sleepTime > 60) ? 60 : (int)sleepTime;
            
            if (!messageReceived && actualSleepTime > 1) {
                dtnex_log("💤 Sleeping %ds until next event (next update in %lds)", 
                    actualSleepTime, sleepTime);
            }
            
            // Sleep in 1-second increments, checking running flag frequently
            // Use a more responsive sleep that can be interrupted by signals
            for (int i = 0; i < actualSleepTime && running; i++) {
                struct timespec req = {1, 0}; // 1 second
                struct timespec rem;
                
                // nanosleep can be interrupted by signals and returns remaining time
                while (nanosleep(&req, &rem) == -1 && running && errno == EINTR) {
                    req = rem; // Continue with remaining time if interrupted
                }
                
                // Bundle reception now handled by dedicated thread
                
                // Check running flag after each second to respond quickly to signals
                if (!running) break;
            }
        } else {
            // Short sleep to avoid busy waiting
            usleep(100000); // 100ms
        }
    }
}

/**
 * Schedule the next update time based on configuration
 */
void scheduleNextUpdate(DtnexConfig *config, time_t *nextUpdateTime) {
    *nextUpdateTime = time(NULL) + config->updateInterval;
    
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(nextUpdateTime));
    dtnex_log("⏰ Next update scheduled for %s (%d minutes)", timeStr, config->updateInterval / 60);
}

/**
 * Completely restart DTNEX when ION restart is detected
 * This is safer than trying to reinitialize ION connections
 */
void restartDtnex(DtnexConfig *config) {
    dtnex_log("🔄 ION restart detected - completely restarting DTNEX...");
    
    // Give ION time to fully restart
    sleep(2);
    
    // Clean shutdown first
    running = 0;
    ionConnected = 0;
    
    // Close file descriptors cleanly if possible
    if (sap != NULL) {
        bp_close(sap);
        sap = NULL;
    }
    
    // Log the restart
    dtnex_log("🔄 Executing DTNEX restart...");
    
    // Execute restart using execv
    if (original_argv != NULL && original_argc > 0) {
        execv(original_argv[0], original_argv);
        
        // If execv fails, log error and exit
        dtnex_log("❌ Failed to restart DTNEX: %s", strerror(errno));
        exit(1);
    } else {
        dtnex_log("❌ Cannot restart - original arguments not stored");
        exit(1);
    }
}


/**
 * Check if ION is running and accessible
 */
IonStatus checkIonStatus(DtnexConfig *config) {
    // Try a simple BP operation to test connectivity
    if (!sap) {
        return ION_STATUS_STOPPED;
    }
    
    // Test if we can still receive (non-blocking)
    BpDelivery testDlv;
    int result = bp_receive(sap, &testDlv, BP_NONBLOCKING);
    
    if (result < 0) {
        if (errno == 22) { // Invalid argument - ION likely restarted
            return ION_STATUS_ERROR;
        }
        return ION_STATUS_UNKNOWN;
    }
    
    // If we got a bundle, release it and return success
    if (testDlv.result == BpPayloadPresent) {
        bp_release_delivery(&testDlv, 1);
    }
    
    return ION_STATUS_RUNNING;
}

/**
 * Process received CBOR message - main entry point for CBOR message handling
 */
void processCborMessage(DtnexConfig *config, unsigned char *buffer, int bufferSize) {
    if (!buffer || bufferSize <= 0) { // only validates the buffer pointer and its size
        debug_log(config, "❌ Invalid CBOR buffer (null or zero size)");
        return;
    }
    
    debug_log(config, "🔍 Processing CBOR message (%d bytes)", bufferSize);
    
    // Debug: Print hex dump of received CBOR data
    if (config->debugMode) {
        printf("📊 CBOR hex dump (%d bytes): ", bufferSize);
        for (int i = 0; i < bufferSize; i++) {
            printf("%02x", buffer[i]);
            if (i < bufferSize - 1) printf(" ");
        }
        printf("\n");
    }
    
    // Decode the CBOR message
    int result = decodeCborMessage(config, buffer, bufferSize); // secondo passaggio
    if (result < 0) {
        log_message_error(config, "Failed to decode CBOR message - unknown bundle format");
        return;
    }
    
    debug_log(config, "✅ CBOR message processed successfully");
}

/**
 * Skip a single CBOR element manually
 */
/**
 * Manual CBOR string decoder to replace ION's cbor_decode_text_string
 */
int manualDecodeCborString(char *buffer, int maxLen, unsigned char **cursor, unsigned int *bytesBuffered) {
    if (*bytesBuffered < 1) {
        return 0; // No bytes left
    }
    
    unsigned char byte = **cursor;
    unsigned char majorType = byte >> 5;
    unsigned char additionalInfo = byte & 0x1F;
    
    // We expect text strings (major type 3)
    if (majorType != 3) {
        return 0; // Not a text string
    }
    
    (*cursor)++;
    (*bytesBuffered)--;
    
    unsigned long stringLen = 0;
    
    if (additionalInfo < 24) {
        // Length is directly in the additional info
        stringLen = additionalInfo;
    } else if (additionalInfo == 24) {
        // 1-byte follows
        if (*bytesBuffered < 1) return 0;
        stringLen = **cursor;
        (*cursor)++;
        (*bytesBuffered)--;
    } else if (additionalInfo == 25) {
        // 2-bytes follow (big-endian)
        if (*bytesBuffered < 2) return 0;
        stringLen = ((*cursor)[0] << 8) | (*cursor)[1];
        (*cursor) += 2;
        (*bytesBuffered) -= 2;
    } else {
        return 0; // Unsupported format for now
    }
    
    // Check if we have enough bytes and buffer space
    if (*bytesBuffered < stringLen || stringLen > maxLen - 1) {
        return 0;
    }
    
    // Copy string data
    memcpy(buffer, *cursor, stringLen);
    buffer[stringLen] = '\0';
    
    (*cursor) += stringLen;
    (*bytesBuffered) -= stringLen;
    
    return 1;
}

/**
 * Manual CBOR integer decoder to replace ION's cbor_decode_integer
 */
int manualDecodeCborInteger(unsigned long *value, unsigned char **cursor, unsigned int *bytesBuffered) {
    if (*bytesBuffered < 1) {
        return 0; // No bytes left
    }
    
    unsigned char byte = **cursor;
    unsigned char majorType = byte >> 5;
    unsigned char additionalInfo = byte & 0x1F;
    
    // We expect positive integers (major type 0)
    if (majorType != 0) {
        return 0; // Not a positive integer
    }
    
    (*cursor)++;
    (*bytesBuffered)--;
    
    if (additionalInfo < 24) {
        // Value is directly in the additional info
        *value = additionalInfo;
        return 1;
    } else if (additionalInfo == 24) {
        // 1-byte follows
        if (*bytesBuffered < 1) return 0;
        *value = **cursor;
        (*cursor)++;
        (*bytesBuffered)--;
        return 1;
    } else if (additionalInfo == 25) {
        // 2-bytes follow (big-endian)
        if (*bytesBuffered < 2) return 0;
        *value = ((*cursor)[0] << 8) | (*cursor)[1];
        (*cursor) += 2;
        (*bytesBuffered) -= 2;
        return 1;
    } else if (additionalInfo == 26) {
        // 4-bytes follow (big-endian)
        if (*bytesBuffered < 4) return 0;
        *value = (((unsigned long)(*cursor)[0]) << 24) | 
                 (((unsigned long)(*cursor)[1]) << 16) |
                 (((unsigned long)(*cursor)[2]) << 8) | 
                 (*cursor)[3];
        (*cursor) += 4;
        (*bytesBuffered) -= 4;
        return 1;
    } else if (additionalInfo == 27) {
        // 8-bytes follow (big-endian) - use full 64-bit value
        if (*bytesBuffered < 8) return 0;
        *value = (((unsigned long)(*cursor)[0]) << 56) | 
                 (((unsigned long)(*cursor)[1]) << 48) |
                 (((unsigned long)(*cursor)[2]) << 40) | 
                 (((unsigned long)(*cursor)[3]) << 32) |
                 (((unsigned long)(*cursor)[4]) << 24) | 
                 (((unsigned long)(*cursor)[5]) << 16) |
                 (((unsigned long)(*cursor)[6]) << 8) | 
                 (*cursor)[7];
        (*cursor) += 8;
        (*bytesBuffered) -= 8;
        return 1;
    }
    
    return 0; // Unsupported format
}

int skipCborElement(unsigned char **cursor, unsigned int *bytesBuffered) {
    if (*bytesBuffered < 1) {
        return 0; // No bytes left
    }
    
    unsigned char majorType = (**cursor) >> 5;
    unsigned char additionalInfo = (**cursor) & 0x1F;
    (*cursor)++;
    (*bytesBuffered)--;
    
    int bytesToSkip = 0;
    
    switch (additionalInfo) {
        case 24: // 1-byte follows
            bytesToSkip = 1;
            break;
        case 25: // 2-bytes follow  
            bytesToSkip = 2;
            break;
        case 26: // 4-bytes follow
            bytesToSkip = 4;
            break;
        case 27: // 8-bytes follow
            bytesToSkip = 8;
            break;
        default:
            if (additionalInfo < 24) {
                // Value is in the additional info
                bytesToSkip = 0;
            } else {
                return 0; // Unsupported format
            }
            break;
    }
    
    // For text strings and byte strings (major types 2,3), skip the content
    if (majorType == 2 || majorType == 3) {
        // Get the length
        int length = additionalInfo;
        if (additionalInfo >= 24) {
            if (*bytesBuffered < bytesToSkip) return 0;
            // Read length from following bytes
            length = 0;
            for (int i = 0; i < bytesToSkip; i++) {
                length = (length << 8) | **cursor;
                (*cursor)++;
                (*bytesBuffered)--;
            }
            bytesToSkip = 0; // Already consumed length bytes
        }
        bytesToSkip += length; // Add content bytes
    }
    
    // Skip the required bytes
    if (*bytesBuffered < bytesToSkip) {
        return 0; // Not enough bytes
    }
    
    *cursor += bytesToSkip;
    *bytesBuffered -= bytesToSkip;
    
    return 1; // Success
}

/**
 * Decode and process CBOR message format
 */
/**
 * The CBOR is parsed and then processed according to the message type
 * (contact or metadata).
 */
int decodeCborMessage(DtnexConfig *config, unsigned char *buffer, int bufferSize) {
    unsigned char *cursor = buffer;
    unsigned int bytesBuffered = bufferSize;
    uvast arraySize = 0;
    uvast version, timestamp, expireTime, origin, from;
    char messageType[2] = {0};
    uvast textSize;
    unsigned char nonce[DTNEX_NONCE_SIZE];
    uvast nonceSize = DTNEX_NONCE_SIZE;
    unsigned char receivedHmac[DTNEX_HMAC_SIZE];
    uvast hmacSize = DTNEX_HMAC_SIZE;
    
    // Decode main array (should have 9 elements)
    if (cbor_decode_array_open(&arraySize, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode CBOR array header");
        return -1;
    }
    
    if (arraySize != 9) {
        debug_log(config, "❌ Invalid CBOR message format - expected 9 elements, got %lu", arraySize);
        return -1;
    }
    
    // 1. Decode version
    if (cbor_decode_integer(&version, CborAny, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode message version");
        return -1;
    }
    
    if (version != DTNEX_PROTOCOL_VERSION) {
        debug_log(config, "❌ Unsupported protocol version %lu (expected %d)", version, DTNEX_PROTOCOL_VERSION);
        return -1;
    }
    
    // 2. Decode message type
    textSize = 1;
    if (cbor_decode_text_string(messageType, &textSize, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode message type");
        return -1;
    }
    
    // 3. Decode timestamp
    if (cbor_decode_integer(&timestamp, CborAny, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode timestamp");
        return -1;
    }
    
    // 4. Decode expire time
    if (cbor_decode_integer(&expireTime, CborAny, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode expire time");
        return -1;
    }
    
    // Check if message has expired
    time_t currentTime = time(NULL);
    if (currentTime > expireTime) {
        debug_log(config, "❌ Message expired (%ld seconds ago)", currentTime - expireTime);
        return -1;
    }
    
    // 5. Decode origin node
    if (cbor_decode_integer(&origin, CborAny, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode origin node");
        return -1;
    }
    
    // 6. Decode from node
    if (cbor_decode_integer(&from, CborAny, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode from node");
        return -1;
    }
    
    // 7. Decode nonce
    if (cbor_decode_byte_string(nonce, &nonceSize, &cursor, &bytesBuffered) <= 0) {
        debug_log(config, "❌ Failed to decode nonce");
        return -1;
    }
    
    if (nonceSize != DTNEX_NONCE_SIZE) {
        debug_log(config, "❌ Invalid nonce size %lu (expected %d)", nonceSize, DTNEX_NONCE_SIZE);
        return -1;
    }
    
    // Debug: Show what we've decoded so far
    debug_log(config, "🔍 Decoded so far: version=%lu, type='%s', timestamp=%lu, expire=%lu, origin=%lu, from=%lu", 
        version, messageType, timestamp, expireTime, origin, from);
    
    // Check for nonce replay
    if (isNonceDuplicate(nonce, origin)) {
        debug_log(config, "❌ Duplicate nonce detected - replay attack or old message");
        return -1;
    }
    
    // We'll set hmacPosition right before HMAC decoding
    unsigned char *hmacPosition;

    // Variables to store extracted contact/metadata data for later processing
    ContactRecord extractedContact = {0};
    StructuredMetadata extractedMetadata = {0};
    int hasExtractedData = 0;
    
    // Skip over the data section to find HMAC
    uvast dataArraySize;
    debug_log(config, "🔍 About to decode data array at cursor position (remaining bytes: %d)", bytesBuffered);
    if (cursor < buffer + bufferSize) {
        debug_log(config, "🔍 Next byte for data array: 0x%02x", *cursor);
    }
    
    // ION's CBOR decoder is failing, implement manual array header decode
    if (bytesBuffered < 1 || cursor >= buffer + bufferSize) {
        debug_log(config, "❌ No bytes left for array decode");
        return -1;
    }
    
    unsigned char arrayByte = *cursor;
    debug_log(config, "🔍 Manual CBOR decode: array byte = 0x%02x", arrayByte);
    
    // Check if it's a CBOR array (major type 4, bits 7-5 = 100)
    if ((arrayByte & 0xE0) == 0x80) {
        // Get array size from lower 5 bits
        dataArraySize = arrayByte & 0x1F;
        if (dataArraySize < 24) {
            // Simple array size (0-23)
            cursor++;
            bytesBuffered--;
            debug_log(config, "✅ Manual array decode successful, size: %lu", dataArraySize);
        } else {
            debug_log(config, "❌ Complex array format (size >= 24) not implemented");
            return -1;
        }
    } else {
        debug_log(config, "❌ Expected CBOR array (0x8x), got 0x%02x", arrayByte);
        return -1;
    }
    
    // Extract data elements based on message type and then skip them for HMAC verification
    if (messageType[0] == 'c') {
        // Contact message v3 (§5.2): 7 fields
        debug_log(config, "🔍 Extracting 7 contact elements manually");

        if (dataArraySize != 7) {
            debug_log(config, "❌ Contact payload with %lu fields (7 expected)", dataArraySize);
            return -1;
        }

        unsigned char *extractCursor = cursor;
        unsigned int extractBytesBuffered = bytesBuffered;

        unsigned long tFromNode, tToNode, tFromTime, tToTime, tXmitRate, tConfidence, tOwlt;
        if (manualDecodeCborInteger(&tFromNode, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tToNode, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tFromTime, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tToTime, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tXmitRate, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tConfidence, &extractCursor, &extractBytesBuffered) &&
            manualDecodeCborInteger(&tOwlt, &extractCursor, &extractBytesBuffered)) {

            extractedContact.fromNode = tFromNode;
            extractedContact.toNode = tToNode;
            extractedContact.fromTime = (time_t) tFromTime;
            extractedContact.toTime = (time_t) tToTime;
            extractedContact.xmitRate = tXmitRate;
            extractedContact.confidence = (unsigned int) tConfidence;
            extractedContact.owlt = (unsigned int) tOwlt;
            hasExtractedData = 1;
            debug_log(config, "✅ Extracted contact: %lu→%lu from=%ld to=%ld xmitRate=%lu conf=%u owlt=%u",
                      extractedContact.fromNode, extractedContact.toNode,
                      (long) extractedContact.fromTime, (long) extractedContact.toTime,
                      extractedContact.xmitRate, extractedContact.confidence,
                      extractedContact.owlt);
        } else {
            debug_log(config, "❌ Failed to extract contact elements");
        }

        // Now skip the elements for HMAC verification
        for (int i = 0; i < dataArraySize && i < 7; i++) {
            if (!skipCborElement(&cursor, &bytesBuffered)) {
                debug_log(config, "❌ Failed to skip contact element %d", i);
                return -1;
            }
        }
        debug_log(config, "✅ Successfully skipped contact elements for HMAC");

    } else if (messageType[0] == 'm') {
        // Metadata message: extract elements and then skip them
        debug_log(config, "🔍 Extracting %lu metadata elements manually", dataArraySize);
        
        // Create a cursor copy for extraction
        unsigned char *extractCursor = cursor;
        unsigned int extractBytesBuffered = bytesBuffered;
        
        // Try to extract metadata based on array size
        unsigned long tempNodeId, tempLat, tempLon;
        
        debug_log(config, "🔍 Metadata array size: %lu", dataArraySize);
        
        if (dataArraySize == 3) {
            // Standard metadata without GPS: [nodeId, name, contact]
            debug_log(config, "🔍 Attempting to decode standard metadata (3 elements)");
            if (manualDecodeCborInteger(&tempNodeId, &extractCursor, &extractBytesBuffered)) {
                debug_log(config, "🔍 Decoded tempNodeId: %lu", tempNodeId);
                if (manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered)) {
                    
                    extractedMetadata.nodeId = (unsigned long)tempNodeId;
                    extractedMetadata.latitude = 0;
                    extractedMetadata.longitude = 0;
                    hasExtractedData = 1;
                    debug_log(config, "✅ Extracted standard metadata: node=%lu, name=%s, contact=%s", 
                              extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact);
                } else {
                    debug_log(config, "❌ Failed to decode name/contact strings");
                }
            } else {
                debug_log(config, "❌ Failed to decode nodeId integer");
            }
        } else if (dataArraySize == 5) {
            // Full metadata with GPS: [nodeId, name, contact, lat, lon]
            debug_log(config, "🔍 Attempting to decode full metadata with GPS (5 elements)");
            if (manualDecodeCborInteger(&tempNodeId, &extractCursor, &extractBytesBuffered)) {
                debug_log(config, "🔍 Decoded tempNodeId: %lu", tempNodeId);
                if (manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLat, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLon, &extractCursor, &extractBytesBuffered)) {
                    
                    extractedMetadata.nodeId = (unsigned long)tempNodeId;
                    // Strings are already null-terminated by manualDecodeCborString
                    extractedMetadata.latitude = (int)tempLat;
                    extractedMetadata.longitude = (int)tempLon;
                    memset(extractedMetadata.location, 0, MAX_LOCATION_LENGTH);  // No location in this format
                    hasExtractedData = 1;
                    debug_log(config, "✅ Extracted full metadata with GPS: node=%lu, name=%s, contact=%s, lat=%d, lon=%d", 
                              extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact,
                              extractedMetadata.latitude, extractedMetadata.longitude);
                } else {
                    debug_log(config, "❌ Failed to decode name/contact/GPS data");
                }
            } else {
                debug_log(config, "❌ Failed to decode nodeId integer in GPS metadata");
            }
        } else if (dataArraySize == 6) {
            // Full metadata with location and GPS: [nodeId, name, contact, location, lat, lon]
            debug_log(config, "🔍 Attempting to decode full metadata with location and GPS (6 elements)");
            if (manualDecodeCborInteger(&tempNodeId, &extractCursor, &extractBytesBuffered)) {
                debug_log(config, "🔍 Decoded tempNodeId: %lu", tempNodeId);
                if (manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborString(extractedMetadata.location, MAX_LOCATION_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLat, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLon, &extractCursor, &extractBytesBuffered)) {
                    
                    extractedMetadata.nodeId = (unsigned long)tempNodeId;
                    extractedMetadata.latitude = (int)tempLat;
                    extractedMetadata.longitude = (int)tempLon;
                    hasExtractedData = 1;
                    debug_log(config, "✅ Extracted full metadata with location and GPS: node=%lu, name=%s, contact=%s, location=%s, lat=%d, lon=%d", 
                              extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact, extractedMetadata.location,
                              extractedMetadata.latitude, extractedMetadata.longitude);
                } else {
                    debug_log(config, "❌ Failed to decode name/contact/location/GPS data");
                }
            } else {
                debug_log(config, "❌ Failed to decode nodeId integer in GPS+location metadata");
            }
        } else if (dataArraySize == 2) {
            // Legacy metadata without nodeId: [name, contact]
            debug_log(config, "🔍 Attempting to decode legacy metadata (2 elements)");
            if (manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered)) {
                
                extractedMetadata.nodeId = origin;  // Use origin as nodeId for legacy format
                extractedMetadata.latitude = 0;  // No GPS data
                extractedMetadata.longitude = 0;
                hasExtractedData = 1;
                debug_log(config, "✅ Extracted legacy metadata: node=%lu, name=%s, contact=%s", 
                          extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact);
            } else {
                debug_log(config, "❌ Failed to decode legacy name/contact");
            }
        } else if (dataArraySize == 4) {
            // Check if this is the new format [nodeId, name, contact, location] or legacy GPS [name, contact, lat, lon]
            // We'll try the new format first by attempting to decode the first element as nodeId
            unsigned char *testCursor = cursor;
            unsigned int testBytesBuffered = bytesBuffered;
            unsigned long testNodeId;
            
            if (manualDecodeCborInteger(&testNodeId, &testCursor, &testBytesBuffered)) {
                // First element decoded as integer - this is likely the new format [nodeId, name, contact, location]
                debug_log(config, "🔍 Attempting to decode new 4-element metadata format (nodeId, name, contact, location)");
                // Use the advanced cursor from the test since we already consumed the nodeId
                extractCursor = testCursor;
                extractBytesBuffered = testBytesBuffered;
                debug_log(config, "🔍 About to decode name string, bytes left: %u", extractBytesBuffered);
                int nameResult = manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered);
                debug_log(config, "🔍 Name decode result: %d, name='%s', bytes left: %u", nameResult, extractedMetadata.name, extractBytesBuffered);
                
                int contactResult = 0, locationResult = 0;
                if (nameResult) {
                    debug_log(config, "🔍 About to decode contact string, bytes left: %u", extractBytesBuffered);
                    contactResult = manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered);
                    debug_log(config, "🔍 Contact decode result: %d, contact='%s', bytes left: %u", contactResult, extractedMetadata.contact, extractBytesBuffered);
                    
                    if (contactResult) {
                        debug_log(config, "🔍 About to decode location string, bytes left: %u", extractBytesBuffered);
                        locationResult = manualDecodeCborString(extractedMetadata.location, MAX_LOCATION_LENGTH - 1, &extractCursor, &extractBytesBuffered);
                        debug_log(config, "🔍 Location decode result: %d, location='%s', bytes left: %u", locationResult, extractedMetadata.location, extractBytesBuffered);
                    }
                }
                
                if (nameResult && contactResult && locationResult) {
                    
                    extractedMetadata.nodeId = (unsigned long)testNodeId;
                    extractedMetadata.latitude = 0;  // No GPS data in this format
                    extractedMetadata.longitude = 0;
                    hasExtractedData = 1;
                    debug_log(config, "✅ Extracted new 4-element metadata: node=%lu, name=%s, contact=%s, location=%s", 
                              extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact, extractedMetadata.location);
                } else {
                    debug_log(config, "❌ Failed to decode name/contact/location strings in new format");
                }
            } else {
                // First element is not an integer - assume legacy GPS format [name, contact, latitude, longitude]
                debug_log(config, "🔍 Attempting to decode legacy GPS metadata (4 elements)");
                if (manualDecodeCborString(extractedMetadata.name, MAX_NODE_NAME_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborString(extractedMetadata.contact, MAX_CONTACT_INFO_LENGTH - 1, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLat, &extractCursor, &extractBytesBuffered) &&
                    manualDecodeCborInteger(&tempLon, &extractCursor, &extractBytesBuffered)) {
                    
                    extractedMetadata.nodeId = origin;  // Use origin as nodeId for legacy format
                    extractedMetadata.latitude = (int)tempLat;
                    extractedMetadata.longitude = (int)tempLon;
                    memset(extractedMetadata.location, 0, MAX_LOCATION_LENGTH);  // No location data
                    hasExtractedData = 1;
                    debug_log(config, "✅ Extracted legacy GPS metadata: node=%lu, name=%s, contact=%s, lat=%d, lon=%d", 
                              extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact,
                              extractedMetadata.latitude, extractedMetadata.longitude);
                } else {
                    debug_log(config, "❌ Failed to decode legacy name/contact/GPS data");
                }
            }
        } else {
            debug_log(config, "❌ Unsupported metadata array size: %lu (expected 2, 3, 4, 5, or 6)", dataArraySize);
        }
        
        if (!hasExtractedData) {
            debug_log(config, "❌ Failed to extract metadata elements");
        }
        
        // Now skip the elements for HMAC verification
        for (int i = 0; i < dataArraySize; i++) {
            if (!skipCborElement(&cursor, &bytesBuffered)) {
                debug_log(config, "❌ Failed to skip metadata element %d", i);
                return -1;
            }
        }
        debug_log(config, "✅ Successfully skipped metadata elements for HMAC");
    } else {
        char error_msg[64];
        snprintf(error_msg, sizeof(error_msg), "Unknown message type '%c'", messageType[0]);
        log_message_error(config, error_msg);
        return -1;
    }
    
    // 9. Set HMAC position and decode HMAC manually
    hmacPosition = cursor; // Now cursor points to the start of the HMAC field
    debug_log(config, "🔍 Decoding HMAC manually (bytes left: %d)", bytesBuffered);
    if (bytesBuffered < 1) {
        debug_log(config, "❌ No bytes left for HMAC");
        return -1;
    }
    
    unsigned char hmacByte = *cursor;
    if ((hmacByte & 0xE0) == 0x40) { // CBOR byte string (major type 2)
        int hmacLength = hmacByte & 0x1F;
        cursor++;
        bytesBuffered--;
        
        if (hmacLength != DTNEX_HMAC_SIZE) {
            debug_log(config, "❌ Invalid HMAC length %d (expected %d)", hmacLength, DTNEX_HMAC_SIZE);
            return -1;
        }
        
        if (bytesBuffered < hmacLength) {
            debug_log(config, "❌ Not enough bytes for HMAC content");
            return -1;
        }
        
        memcpy(receivedHmac, cursor, hmacLength);
        hmacSize = hmacLength;
        cursor += hmacLength;
        bytesBuffered -= hmacLength;
        
        debug_log(config, "✅ HMAC decoded manually, size: %d", hmacLength);
    } else {
        debug_log(config, "❌ Expected CBOR byte string for HMAC, got 0x%02x", hmacByte);
        return -1;
    }
    
    if (hmacSize != DTNEX_HMAC_SIZE) {
        debug_log(config, "❌ Invalid HMAC size %lu (expected %d)", hmacSize, DTNEX_HMAC_SIZE);
        return -1;
    }
    
    // Verify HMAC (calculate HMAC over message without the HMAC field)
    int messageWithoutHmacSize = hmacPosition - buffer;
    debug_log(config, "🔍 HMAC calculation: message size without HMAC = %d bytes", messageWithoutHmacSize);
    if (!verifyHmac(config, buffer, messageWithoutHmacSize, receivedHmac, config->presSharedNetworkKey)) {
        debug_log(config, "❌ HMAC verification failed - wrong hash or corrupted message");
        return -1;
    }
    
    debug_log(config, "✅ CBOR message authentication verified");
    
    // Add nonce to cache to prevent replay
    addNonceToCache(nonce, origin);
    
    // Process the message based on type using pre-extracted data
    if (messageType[0] == 'c') {
        // Use pre-extracted contact data instead of re-decoding
        if (!hasExtractedData) {
            debug_log(config, "❌ No contact data was extracted during parsing");
            return -1;
        }
        
        debug_log(config, "🔍 Processing extracted contact data: %lu→%lu (from=%ld to=%ld)",
                  extractedContact.fromNode, extractedContact.toNode,
                  (long) extractedContact.fromTime, (long) extractedContact.toTime);

        return processCborContactMessage(config, nonce, timestamp, expireTime, origin, from, &extractedContact);
    } else if (messageType[0] == 'm') {
        // Use pre-extracted metadata instead of re-decoding
        if (!hasExtractedData) {
            debug_log(config, "❌ No metadata was extracted during parsing");
            return -1;
        }
        
        debug_log(config, "🔍 Processing extracted metadata: node=%lu, name=%s, contact=%s", 
                  extractedMetadata.nodeId, extractedMetadata.name, extractedMetadata.contact);
        
        return processCborMetadataMessage(config, nonce, timestamp, expireTime, origin, from, &extractedMetadata);
    }
    
    log_message_error(config, "Unknown message type or failed to extract data");
    return -1;
}

/**
 * Process CBOR contact message
 */

/* Beyond this distance into the future, a window looks like clock skew rather
 * than a legitimate one: it is discarded, and said so (§7.6). */
#define CLOCK_SKEW_FUTURE_LIMIT (30 * 24 * 3600)

int processCborContactMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime,
                             unsigned long origin, unsigned long from, ContactRecord *contact) {
    time_t currentTime = time(NULL);
    IoncApplyOutcome outcome;

    log_message_received(config, origin, from, "contact",
            contact->fromNode, contact->toNode, NULL);

    /* Validation pipeline (§6.1). Checks 1-3 (version, HMAC, nonce) have
     * already been performed in decodeCborMessage. Every failure discards the
     * message without inserting and without forwarding, and returns 0: this is
     * not a decoding error (the message decoded perfectly), it is a policy
     * rejection, already logged below with its reason. A -1 would travel back
     * up to decodeCborMessage and be printed as "Failed to decode CBOR
     * message", which is misleading diagnostics for a legitimate discard. */

    // 4. We do not process our own messages
    if (origin == config->nodeId) {
        debug_log(config, "⏭️ Skipping own contact message");
        return 0;
    }

    // 5. Only the source announces its own direction (§4)
    if (contact->fromNode != origin) {
        debug_log(config, "❌ Discarded: fromNode=%lu != origin=%lu",
                contact->fromNode, origin);
        return 0;
    }

    // 5b. A contact towards oneself is not topology: in ION that is the
    // semantics of registration contacts. ionc_get_own_contacts filters it out
    // at origination, but a peer holding the HMAC key could still inject one,
    // so the filter is needed on reception too.
    if (contact->fromNode == contact->toNode) {
        debug_log(config, "❌ Discarded: fromNode == toNode (%lu): registration "
                "contact, not topology", contact->fromNode);
        return 0;
    }

    // 6. toTime = 0 in ION means "discovered contact" -> MAX_POSIX_TIME
    if (contact->toTime == 0) {
        debug_log(config, "❌ Discarded: toTime = 0 (permanent-contact semantics)");
        return 0;
    }

    // 7. Window integrity
    if (contact->fromTime >= contact->toTime) {
        debug_log(config, "❌ Discarded: fromTime=%ld >= toTime=%ld",
                (long) contact->fromTime, (long) contact->toTime);
        return 0;
    }

    // 7b. fromTime <= 0 carries ION's "hypothetical contact" semantics
    // (rfx_insert_contact takes the CHKZERO branch on ownNodeNbr and can
    // return 0 without having written anything: a false success).
    if (contact->fromTime <= 0) {
        debug_log(config, "❌ Discarded: invalid fromTime: hypothetical-contact "
                "semantics in ION (fromTime=%ld)", (long) contact->fromTime);
        return 0;
    }

    // 7c. fromTime == MAX_POSIX_TIME is what triggers registration contacts in
    // ION, not a real network window.
    if (contact->fromTime >= MAX_POSIX_TIME) {
        debug_log(config, "❌ Discarded: fromTime = MAX_POSIX_TIME: "
                "registration-contact semantics");
        return 0;
    }

    // 7d. xmitRate == 0 would be refused by ION itself (rfx_insert_contact
    // returns 5).
    if (contact->xmitRate == 0) {
        debug_log(config, "❌ Discarded: zero xmitRate: ION would refuse the contact");
        return 0;
    }

    // 7e. Out-of-range confidence: ION returns 4 for confidence > 1.0.
    if (contact->confidence > 100) {
        debug_log(config, "❌ Discarded: confidence outside the 0-100 range (%u)",
                contact->confidence);
        return 0;
    }

    // 8. Window already expired
    if (contact->toTime <= currentTime) {
        debug_log(config, "❌ Discarded: window entirely in the past "
                "(toTime=%ld, now=%ld) — possible clock skew between nodes",
                (long) contact->toTime, (long) currentTime);
        return 0;
    }

    // 8b. Window too far in the future: suspected clock skew (§7.6)
    if (contact->fromTime > currentTime + CLOCK_SKEW_FUTURE_LIMIT) {
        debug_log(config, "❌ Discarded: window too far in the future "
                "(fromTime=%ld, now=%ld) — possible clock skew between nodes",
                (long) contact->fromTime, (long) currentTime);
        return 0;
    }

    /* There is no check 9 on the owlt: 0 is a legitimate OWLT (on a LAN it is
     * the physically correct value, and ION accepts "a range ... 0"). Discarding
     * it here contradicted origination, where findOwlt returns 0 as a valid
     * value, and on a local testbed it made every receiver discard everything.
     * The v3 format always carries the field, and ionc_get_own_contacts only
     * announces contacts for which a range really exists: the check was
     * redundant. */

    /* We write what we learn, but announce only what we are authoritative for
     * (§4.5): contacts with toNode == me are inserted as well. */
    outcome = ionc_apply_contact(contact, config->debugMode);

    /* Transitional: as long as the "c" message carries the owlt, the range is
     * still written here, derived from the contact. This will go away once the
     * "r" message carries ranges on its own. */
    /* Skipped on IONC_ERROR: that is a system error, so ION is unreachable or
     * broken and the range write would be a second call failing the same way.
     * On a user error we do carry on, because a local refusal of the contact
     * must not stop ION from learning the OWLT. */
    if (outcome != IONC_ERROR) {
        RangeRecord derivedRange;
        IoncApplyOutcome rangeOutcome;

        derivedRange.fromNode = contact->fromNode;
        derivedRange.toNode = contact->toNode;
        derivedRange.fromTime = contact->fromTime;
        derivedRange.toTime = contact->toTime;
        derivedRange.owlt = contact->owlt;

        /* IoncApplyOutcome is ordered by increasing severity: reporting the
         * higher of the two reproduces the previous rule, when a single call
         * wrote both. */
        rangeOutcome = ionc_apply_range(&derivedRange, config->debugMode);
        if (rangeOutcome > outcome) {
            outcome = rangeOutcome;
        }
    }

    if (outcome == IONC_ERROR) {
        /* IONC_ERROR is a system error from an rfx_* call (rc < 0) or ION
         * being unreachable: after the earlier fix, an overlap with a manually
         * configured contact is an expected user error and no longer reaches
         * this point. The message stays valid regardless and must be forwarded
         * (§6.5), otherwise a purely local problem would partition the
         * flooding. */
        dtnex_log("❌ Failed to apply %lu→%lu to ION",
                contact->fromNode, contact->toNode);
    } else if (outcome == IONC_LOST) {
        /* The previous entry was removed from ION but the insert meant to
         * replace it was refused: unlike the other outcomes, here ION got
         * worse than before, not merely unchanged. The detail (ION code and
         * its meaning) has already been logged by ion_contacts.c; here we
         * only flag that this is not a success, so the loss does not stay
         * invisible at the default log level. */
        dtnex_log("⚠️  Contact %lu→%lu lost in ION after a successful remove "
                "(the following insert was refused)",
                contact->fromNode, contact->toNode);
    } else if (outcome != IONC_NOOP) {
        dtnex_log("✅ Contact %lu→%lu %s in ION",
                contact->fromNode, contact->toNode, ionc_outcome_name(outcome));
    }

    // Forwarding is unchanged (§6.5): a message that passes validation is
    // always forwarded, regardless of the outcome of the local ION write.
    forwardCborContactMessage(config, nonce, timestamp, expireTime, origin, from, contact);

    return 0;
}

/**
 * Process CBOR metadata message
 */
int processCborMetadataMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime,
                              unsigned long origin, unsigned long from, StructuredMetadata *metadata) {
    log_message_received(config, origin, from, "metadata", metadata->nodeId, 0, metadata->name);
    
    // Skip processing our own messages
    if (origin == config->nodeId) {
        debug_log(config, "⏭️ Skipping own metadata message");
        return 0;
    }
    
    // Convert GPS coordinates back to decimal if present
    if (metadata->latitude != 0 || metadata->longitude != 0) {
        double lat = (double)metadata->latitude / GPS_PRECISION_FACTOR;
        double lon = (double)metadata->longitude / GPS_PRECISION_FACTOR;
        debug_log(config, "🌍 GPS: %.6f, %.6f", lat, lon);
    }
    
    // Update node metadata in our records
    char fullMetadata[MAX_METADATA_LENGTH];
    if (metadata->latitude != 0 || metadata->longitude != 0) {
        double lat = (double)metadata->latitude / GPS_PRECISION_FACTOR;
        double lon = (double)metadata->longitude / GPS_PRECISION_FACTOR;
        snprintf(fullMetadata, sizeof(fullMetadata), "%s,%s,%.6f,%.6f", 
                metadata->name, metadata->contact, lat, lon);
    } else if (strlen(metadata->location) > 0) {
        snprintf(fullMetadata, sizeof(fullMetadata), "%s,%s,%s", 
                metadata->name, metadata->contact, metadata->location);
    } else {
        snprintf(fullMetadata, sizeof(fullMetadata), "%s,%s", 
                metadata->name, metadata->contact);
    }
    
    updateNodeMetadata(config, metadata->nodeId, fullMetadata);
    
    dtnex_log("✅ Node %lu metadata updated", metadata->nodeId);
    
    // Forward CBOR metadata message to all neighbors (except origin and sender)
    forwardCborMetadataMessage(config, nonce, timestamp, expireTime, origin, from, metadata);
    
    return 0;
}

/**
 * Forward CBOR contact message to all neighbors (except origin and sender)
 */
void forwardCborContactMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp,
                              time_t expireTime, unsigned long origin, unsigned long from, ContactRecord *contact) {
    Plan plans[MAX_PLANS];
    int planCount = 0;
    char destEid[MAX_EID_LENGTH];
    unsigned char cborBuffer[MAX_CBOR_BUFFER];

    // Forwarding contact message - individual forwards logged in the loop
    
    // Get current neighbor list
    getplanlist(config, plans, &planCount);
    
    if (planCount <= 0) {
        debug_log(config, "⏭️ No neighbors to forward contact message to");
        return;
    }
    
    // Forward to all neighbors except origin, sender, and ourselves
    for (int i = 0; i < planCount; i++) {
        unsigned long neighborId = plans[i].planId;
        
        // Skip forwarding to origin, sender, or ourselves
        if (neighborId == origin || neighborId == from || neighborId == config->nodeId) {
            continue;
        }
        
        // Create CBOR forwarded message with preserved original nonce, timestamp and expireTime
        ContactRecord forwardContact = *contact; // Copy contact info

        // Create modified contact message for forwarding (from=our nodeId)
        unsigned char *cursor = cborBuffer;
        int bytesWritten = 0;

        // Encode forwarded CBOR message: only the "from" field changes, to
        // this node; origin, timestamp, expireTime and nonce stay the
        // original ones.
        bytesWritten += writeCborEnvelope(&cursor, "c", timestamp, expireTime,
                origin, config->nodeId, originalNonce);

        // Contact data v3
        bytesWritten += cbor_encode_array_open(7, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.fromNode, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.toNode, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.fromTime, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.toTime, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.xmitRate, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.confidence, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.owlt, &cursor);

        bytesWritten += appendCborHmac(config, cborBuffer, bytesWritten, &cursor);

        // Send forwarded CBOR bundle
        sprintf(destEid, "ipn:%lu.%s", neighborId, config->serviceNr);
        sendCborBundle(destEid, cborBuffer, bytesWritten, config->bundleTTL);

        log_message_forwarded(config, origin, from, neighborId, "contact",
                             contact->fromNode, contact->toNode, NULL);
    }
}

/**
 * Forward CBOR metadata message to all neighbors (except origin and sender)
 */
void forwardCborMetadataMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp,
                               time_t expireTime, unsigned long origin, unsigned long from, StructuredMetadata *metadata) {
    Plan plans[MAX_PLANS];
    int planCount = 0;
    char destEid[MAX_EID_LENGTH];
    unsigned char cborBuffer[MAX_CBOR_BUFFER];

    // Forwarding metadata message - individual forwards logged in the loop
    
    // Get current neighbor list
    getplanlist(config, plans, &planCount);
    
    if (planCount <= 0) {
        debug_log(config, "⏭️ No neighbors to forward metadata message to");
        return;
    }
    
    // Forward to all neighbors except origin, sender, and ourselves
    for (int i = 0; i < planCount; i++) {
        unsigned long neighborId = plans[i].planId;
        
        // Skip forwarding to origin, sender, or ourselves
        if (neighborId == origin || neighborId == from || neighborId == config->nodeId) {
            continue;
        }
        
        // Create CBOR forwarded message with preserved original nonce, timestamp and expireTime
        unsigned char *cursor = cborBuffer;
        int bytesWritten = 0;
        
        // Encode forwarded CBOR message: only the "from" field changes, to
        // this node; origin, timestamp, expireTime and nonce stay the
        // original ones.
        bytesWritten += writeCborEnvelope(&cursor, "m", timestamp, expireTime,
                origin, config->nodeId, originalNonce);

        // Metadata data
        int metadataElements = 3; // nodeId, name, contact (base)
        int hasLocation = strlen(metadata->location) > 0;
        int hasGPS = (metadata->latitude != 0 && metadata->longitude != 0);
        
        if (hasLocation && hasGPS) {
            metadataElements += 3; // Add location, latitude, and longitude
        } else if (hasGPS) {
            metadataElements += 2; // Add latitude and longitude only
        } else if (hasLocation) {
            metadataElements += 1; // Add location only
        }
        
        bytesWritten += cbor_encode_array_open(metadataElements, &cursor);
        bytesWritten += cbor_encode_integer(metadata->nodeId, &cursor);
        bytesWritten += cbor_encode_text_string(metadata->name, strlen(metadata->name), &cursor);
        bytesWritten += cbor_encode_text_string(metadata->contact, strlen(metadata->contact), &cursor);
        
        // Add fields in order: location (if available), then GPS (if available)
        if (hasLocation && hasGPS) {
            // Format: [nodeId, name, contact, location, lat, lon]
            bytesWritten += cbor_encode_text_string(metadata->location, strlen(metadata->location), &cursor);
            bytesWritten += cbor_encode_integer(metadata->latitude, &cursor);
            bytesWritten += cbor_encode_integer(metadata->longitude, &cursor);
        } else if (hasGPS) {
            // Format: [nodeId, name, contact, lat, lon]
            bytesWritten += cbor_encode_integer(metadata->latitude, &cursor);
            bytesWritten += cbor_encode_integer(metadata->longitude, &cursor);
        } else if (hasLocation) {
            // Format: [nodeId, name, contact, location]
            bytesWritten += cbor_encode_text_string(metadata->location, strlen(metadata->location), &cursor);
        }
        
        bytesWritten += appendCborHmac(config, cborBuffer, bytesWritten, &cursor);

        // Send forwarded CBOR bundle
        sprintf(destEid, "ipn:%lu.%s", neighborId, config->serviceNr);
        sendCborBundle(destEid, cborBuffer, bytesWritten, config->bundleTTL);

        log_message_forwarded(config, origin, from, neighborId, "metadata",
                             metadata->nodeId, 0, metadata->name);
    }
}