/**
 * dtnex.c
 * DTNEX - DTN Network Information Exchange
 * High-performance C implementation for exchanging DTN contact and metadata information
 * Author: Samo Grasic (samo@grasic.net)
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
    variabile di flag che tiene in vita il loop, volatile per evitare problemi di ottimizzazione con il signal handler,
    infatti viene modificata dal signal hadler quando arriva un segnale (SIGINT/SIGTERM) e la setta a 0, così il
    loop principale termina ordinatamante 
*/
volatile int ionConnected = 0;  // Global ION connection status, volatile perché può cambiare in modo asincrono (es. se ION si riavvia mentre DTNEX è in esecuzione)
volatile int ionRestartDetected = 0;  // Flag to trigger complete restart
/* 
    Se viene settatto a 1, dtnex non esce semplicemente dal loop, ma invoca restartDtnex() che fa un execv del
    processo su se stesso, non crea processi figli, sostituisce semplicemente l'immagine
*/
static char **original_argv = NULL;  // Store original argv for restart
static int original_argc = 0;        // Store original argc for restart
/* Variabili con cui lanciare l'execv, vengono salvate all'inizio del main()*/
BpSAP sap;
/*
    Bundle Protocol Service Access Point (SAP) handle principale verso il layer BP di ION. come una socket, ma per
    bundle DTN, si ottiene con bp_open() e si usa per inviare e ricevere bundle. 
    Viene usato in tutto il codice per tutte le operazioni BP,
*/
Sdr sdr;
/*
    handle per la accedere alla memoria persistente di ION condivisa di ION, non usa malloc standard
*/

// Cache per duplicazione dei messaggi
HashCache hashCache[MAX_HASH_CACHE];
int hashCacheCount = 0;
/* 
    Serve per la deduplicazione dei messaggi processati, ogni messaggio ricevuto viene hashato e confrontato con
    gli hash già salvati, se viene trovato un hash uguale, significa che è un messaggio duplicato e viene scartato.
 */
NonceCache nonceCache[MAX_HASH_CACHE]; // For CBOR replay protection
int nonceCacheCount = 0;
/* 
    Stessa cosa però per i nonce, serve per evitare il replay dei messaggi, la cache tiene traccia di (nonce, origin), 
    se un attaccante ad esempio intercetta e reinvia un messaggio già processato viene scartato 
*/

NodeMetadata nodeMetadataList[MAX_PLANS];
int nodeMetadataCount = 0;
/*  
    "rubrica locale", nodeId + metadata. Quando arriva un messaggio di tipo metadata, viene aggiornato con 
    updateNodeMetadata(), poi consultato per in getContacts().
*/

BpechoState bpechoState;   // Bpecho service state
pthread_t bpechoThread;    // Thread for bpecho service
/* 
    Gestione del servizio di echo, BpechoState è una struttua che tiene lo stato, un sap e un attendant.
*/

BundleReceptionState bundleReceptionState;  // Bundle reception service state
/* 
    Gestione del thread di ricezione dei bundle, BundleReceptionState è una struttura che tiene lo stato,
    la config e l'identificatore del thread.
*/

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

/* Strumentazione di analisi su file. Resta spenta finche' loadConfig non
 * accerta debugMode: senza debug non deve esistere nessun dtnex_debug.log,
 * altrimenti in --service il file cresce senza fine. */
static int dtnexDbgToFile = 0;

static void dtnex_dbg(const char *fmt, ...) {
    static FILE *_dbgf = NULL;
    if (!dtnexDbgToFile) return;
    if (_dbgf == NULL) {
        _dbgf = fopen("dtnex_debug.log", "a");
        if (_dbgf == NULL) return;
        fprintf(_dbgf, "\n=== DTNEX DEBUG SESSION START ===\n");
        fflush(_dbgf);
    }
    struct timespec _ts;
    clock_gettime(CLOCK_REALTIME, &_ts);
    struct tm *_tm = localtime(&_ts.tv_sec);
    char _tbuf[32];
    strftime(_tbuf, sizeof(_tbuf), "%H:%M:%S", _tm);
    fprintf(_dbgf, "[%s.%03ld] ", _tbuf, _ts.tv_nsec / 1000000);
    va_list _ap;
    va_start(_ap, fmt);
    vfprintf(_dbgf, fmt, _ap);
    va_end(_ap);
    fprintf(_dbgf, "\n");
    fflush(_dbgf);
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
    config->updateInterval = DEFAULT_UPDATE_INTERVAL; // 600, 10 minuti tra un update e l'altro
    config->contactLifetime = DEFAULT_CONTACT_LIFETIME; // 3600, 1 ora di validità dei contatti
    config->contactTimeTolerance = DEFAULT_CONTACT_TIME_TOLERANCE;
    config->bundleTTL = DEFAULT_BUNDLE_TTL; // 1800, 30 minuti di TTL per i bundle (3x update interval)
    strcpy(config->presSharedNetworkKey, DEFAULT_PRESHARED_KEY);
    sprintf(config->serviceNr, "%d", DEFAULT_SERVICE_NR);
    sprintf(config->bpechoServiceNr, "%d", DEFAULT_BPECHO_SERVICE_NR);
    config->nodeId = 0; // viene settato a 0 perché il nodo non conosce ancora il proprio ID finché non si connette a ION, poi viene aggiornato in tryConnectToIon(
    memset(config->nodemetadata, 0, MAX_METADATA_LENGTH);
    config->createGraph = 0;
    strcpy(config->graphFile, "contactGraph.png");
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
                } else if (strcmp(key, "contactTimeTolerance") == 0) {
                    config->contactTimeTolerance = atoi(value);
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

    /* dtnex_dbg e' static e non vede DtnexConfig: gli passiamo qui l'unico
     * dato che gli serve. Prima di questo punto la strumentazione su file e'
     * spenta, quindi nessun dtnex_debug.log viene creato senza debugMode. */
    dtnexDbgToFile = config->debugMode;
}


// No global endpoint status needed in the single-threaded version

/**
 * Try to connect to ION - returns 0 on success, -1 on failure
 * This function handles all ION connection logic cleanly
 */

/**
 * Questa funzione è il ponte tra DTNEX e ION. Si compone di 5 fasi sequenziali, dove ogni fallimento causa un
 * rollback pulito e ritorna -1, al contrario ritorna 0 e lascia DTNEX pronto a usare ION, ritorna 0.
 */
int tryConnectToIon(DtnexConfig *config) {
    char endpointId[MAX_EID_LENGTH];
    
    // Try to attach to ION BP system
    /**
     * Aggancia il processo all'istanza di ION già in esecuzione
     */
    if (bp_attach() < 0) {
        return -1;
    }

    /**
     * Lettura del Node ID da ION,
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
    IonDB iondb;
    Object iondbObject = getIonDbObject(); //è il puntatore all'oggetto iondb nella SDR di ION
    if (iondbObject == 0) {
        sdr_exit_xn(ionsdr);
        bp_detach();
        return -1;
    }
    
    // Read the iondb object to get the node number
    sdr_read(ionsdr, (char *) &iondb, iondbObject, sizeof(IonDB));
    /**
     * Legge i dati dell'oggetto iondb dalla SDR di ION e li copia nella struttura iondb locale,
     * da cui poi si ricava il nodeId. sdr_read legge direttamente dalla memoria sdr, non usa malloc.
     * iondbObject è un puntatore alla memoria sdr.
     */
    config->nodeId = iondb.ownNodeNbr;
    sdr_exit_xn(ionsdr);

    /* ---- DEBUG: dump IonDB fields ---- */
    dtnex_dbg("[tryConnectToIon] IonDB dump after sdr_read:");
    dtnex_dbg("  ownNodeNbr = %lu", (unsigned long)iondb.ownNodeNbr);
    dtnex_dbg("  ranges     = 0x%lx", (unsigned long)iondb.ranges);
    /* Niente dump di iondb.contacts: quel campo non esiste nell'IonDB della
     * libreria ION installata, a quell'offset c'e' 'ranges'. La riga stampava
     * lo stesso valore della precedente sotto un altro nome. */
    /* ---- END DEBUG ---- */

    if (config->nodeId == 0) {
        bp_detach();
        return -1;
    }
    
    dtnex_log("Using node ID: %lu detected from ION configuration", config->nodeId);
    
    // Construct endpoint ID
    snprintf(endpointId, MAX_EID_LENGTH, "ipn:%lu.%s", config->nodeId, config->serviceNr);
    dtnex_log("Using endpoint: %s", endpointId);
    
    // Get the SDR
    sdr = bp_get_sdr(); //sdr ottenuto non come getIonsdr() ma tramite bp_get_sdr() che è la funzione corretta per ottenere l'sdr da usare con BP
    if (sdr == NULL) {
        bp_detach();
        return -1;
    }
    
    // First try to add/register the endpoint in ION's routing database

    /**
     * addEndpoint() è una api di ION che aggiunge l'EID al database di routing. Dice ad ION che se arriva un bundle a
     * questo EID, deve essere messo in coda qui, non discardBundle. Se fallisce, non fatale, perché vuole dire che
     * l'endpoint era già registrato da avvii precedenti.
     */

    if (addEndpoint(endpointId, EnqueueBundle, NULL) < 0) {
        debug_log(config, "Warning: Could not register endpoint %s in routing database", endpointId);
    }

    /**
     * A questo punto registrato l'endpoint, ottengo il sap relativo a quell'EID, 
     * da cui potrò ricevere ed inviare messaggi -> lo mette nella variabile globale sap
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
         * Aggiunge i propri metadati, presi dal file di configurazione, alla lista di metadata locali,
         * dalla quale poi li prenderà per inserirli nei messaggi di metadata che invia ai vicini.
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
    dtnex_log("Starting DTNEXC v%s (built %s %s), author: Samo Grasic (samo@grasic.net)", 
              DTNEXC_VERSION, DTNEXC_BUILD_DATE, DTNEXC_BUILD_TIME);
    
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
 * A cosa serve questa funzione? -> serve per sapere quali nodi vicini conosce ION in questo momento.
 * Interroga direttamente le STRUTTURE INTERNE di ION per ottenere la lista dei nodi vicini (plans).
 */

/* getplanlist e' chiamata sia dal main loop sia dal thread di ricezione
 * (via forwardCborContactMessage) e scrive nella propria cache statica:
 * l'accesso va serializzato (§7.5). */
static pthread_mutex_t planListMutex = PTHREAD_MUTEX_INITIALIZER;

void getplanlist(DtnexConfig *config, Plan *plans, int *planCount) {
    time_t currentTime;

    // Variabili statiche per caching dei piani, persistono tra una chiamata e l'altra per evitare troppe interrogazioni
    // ad ION. Se abbiamo aggiornato i piani recentemente (meno di 20 secondi fa), usiamo quelli in cache.
    //essendo variabili static l'inizializzazione viene fatta solo una volta.
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
     * Il TTL della cache è scaduto, ma non riesco comunque ad ottenere l'SDR da ION,
     * se ho qualcosa in cache la uso lo stesso.
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
    
    // Parte ION-Specific di interrogazione di ION per ottenere la lista dei plans

    // Start a transaction
    if (sdr_begin_xn(sdr) < 0) {
        dtnex_log("Error: can't begin SDR transaction");
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    // Get the BP constants
    bpConstants = getBpConstants(); // Ottengo il puntatore all'oggetto database che risiede nell'SDR
    if (bpConstants == NULL) {
        dtnex_log("Error: can't get BP constants");
        sdr_exit_xn(sdr);
        pthread_mutex_unlock(&planListMutex);
        return;
    }
    
    // Get the list of plans and iterate through it with safe access

    /**
     * bpConstants->plans è una lista di plans gestita da SDR, non una lista c. 
     * Deve essere iterata tramite le opportune api 
     */

    Object planElt = 0;
    for (planElt = sdr_list_first(sdr, bpConstants->plans); 
         planElt && planElt != 0; 
         planElt = sdr_list_next(sdr, planElt)) {
        
        // Get the plan data with careful error checking
        Object planData = sdr_list_data(sdr, planElt); // è un offset relativo alla base della memoria condivisa, non un puntatore effettivo
        if (planData == 0) {
            dtnex_log("Warning: Null plan data, skipping");
            continue;
        }
        
        // Get the plan object - cast to BpPlan* with proper error checking
        BpPlan *plan = (BpPlan*) sdr_pointer(sdr, planData); //trasforma l'object in un puntatore leggibile e refernziabile, puntatore alla shared memory non una copia
        if (plan == NULL) {
            dtnex_log("Warning: Null plan pointer, skipping");
            continue;
        }
        dtnex_dbg("[getplanlist] BpPlan raw read: neighborNodeNbr=%lu planData_obj=0x%lx",
            (unsigned long)plan->neighborNodeNbr,
            (unsigned long)planData);

        // Only include plans with a valid neighbor node number (CBHE-compliant)
        if (plan->neighborNodeNbr == 0) {
            continue;
        }

        // Skip our own node
        if (plan->neighborNodeNbr == config->nodeId) { // per evitare che il nodo si spedisca piani a se stessa
            continue;
        }

        // No verbose output for each plan found

        // Add this plan to our lists (both the output and the cache)
        if (*planCount < MAX_PLANS) {
            plans[*planCount].planId = plan->neighborNodeNbr; // Salvo il neighborNodeNbr come planId, è l'informazione più importante che mi serve per identificare il vicino, lo userò poi per costruire l'EID -> IPN:neighborNodeNbr.serviceNr
            dtnex_dbg("[getplanlist] Added to plan list: index=%d planId=%lu",
                *planCount,
                (unsigned long)plans[*planCount].planId);
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

    dtnex_dbg("[getplanlist] FINAL RESULT: planCount=%d nodeId=%lu",
        *planCount,
        (unsigned long)config->nodeId);
    for (int _i = 0; _i < *planCount; _i++) {
        dtnex_dbg("[getplanlist]   plan[%d] = neighborId %lu",
            _i, (unsigned long)plans[_i].planId);
    }

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
 * Funzione che decide se, cosa e a chi inviare le informazioni, poi delega la costruzione e l'invio del bundle
 * alle funzioni CBOR. Ha 3 fasi: gate temporale, loop di invio dei contatti, loop di invio dei metadata.
 */

/**
 * Snapshot dei contatti annunciabili (§3.2).
 *
 * INVARIANTE DI CORRETTEZZA DELLA CACHE (§7.4): questa cache e' a solo TTL,
 * senza invalidazione su scrittura, e cio' e' corretto SOLO perche' lo
 * snapshot contiene esclusivamente contatti con fromNode == nodo locale
 * (regola di autorita', §4). Nessuna scrittura che dtnex fa in ION puo'
 * quindi rientrare in questo insieme. Se qualcuno rilassa quel filtro,
 * questa cache diventa silenziosamente sbagliata.
 *
 * Il TTL governa la reattivita' del trigger 3 di §7.1: e' il tempo massimo
 * fra una modifica a ionrc e la sua scoperta da parte della rete. 60 secondi
 * e' il compromesso scelto: coincide con il periodo massimo di risveglio del
 * main loop, quindi non aggiunge accessi a ION rispetto al ritmo del loop.
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
 * Rinfresca lo snapshot se il TTL e' scaduto. Scrive in *changed 1 se il
 * nuovo snapshot differisce dal precedente, 0 altrimenti (o se non e' stato
 * rinfrescato). Ritorna il numero di contatti nello snapshot, -1 su errore
 * di accesso a ION (nel qual caso lo snapshot precedente resta valido).
 *
 * Mono-thread (§7.5): solo il main loop chiama questa funzione.
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
        debug_log(config, "⚠️ Impossibile rileggere i contatti annunciabili da ION");
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
        dtnex_log("🔄 Lo snapshot dei contatti annunciabili e' cambiato (%d contatti)",
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

    // Trigger 3 (§7.1): rinfresca lo snapshot e guarda se e' cambiato.
    refreshMyContacts(config, &contactsChanged);

    // Trigger 2: la lista dei vicini e' cambiata
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

    // Trigger 1: e' passato updateInterval
    if (!(lastExchangeTime == 0
            || (currentTime - lastExchangeTime) >= config->updateInterval
            || planListChanged
            || contactsChanged)) {
        int remainingTime = config->updateInterval - (int) (currentTime - lastExchangeTime);
        debug_log(config, "Skipping neighbor exchange (next in %d seconds)", remainingTime);
        return;
    }

    dtnex_log("📤 Annuncio %d contatti a %d vicini...", myContactCount, planCount);

    lastExchangeTime = currentTime;
    lastPlanCount = planCount;
    for (i = 0; i < planCount && i < MAX_PLANS; i++) {
        lastPlanList[i] = plans[i].planId;
    }

    // Un messaggio per contatto annunciabile, a ogni vicino (§5.4).
    for (i = 0; i < planCount; i++) {
        unsigned long neighborId = plans[i].planId;

        if (neighborId == config->nodeId) {
            continue;  // plan locale di loopback
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
     * Se lo scambio di metadata è abilitato, il nodo scambia i SUOI metadata, prendendoli dal file di configurazione
     * e li invia a tutti i vicini
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
            // Scrive i dati di metadata dentro cborBuffer e ritorna la dimensione del messaggio
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
 * Signal handler for clean shutdown
 * Based on bpsink's handleQuit pattern
 */
void signalHandler(int sig) {
    // Re-arm signal handlers in case we receive multiple signals
    isignal(SIGINT, signalHandler);
    isignal(SIGTERM, signalHandler);
    isignal(SIGTSTP, signalHandler);
    
    // Prevent re-entrancy (in case signal is received again during shutdown)
    static int inShutdown = 0;
    if (inShutdown) {
        dtnex_log("Already in shutdown process, forcing immediate exit...");
        exit(1);  // Force exit if shutdown takes too long
    }
    inShutdown = 1;
    
    // Provide feedback based on signal type
    if (sig == SIGINT) {
        dtnex_log("Received interrupt signal (Ctrl+C), shutting down gracefully...");
    } else if (sig == SIGTERM) {
        dtnex_log("Received termination signal, shutting down gracefully...");
    } else if (sig == SIGTSTP) {
        dtnex_log("Received suspend signal (Ctrl+Z), shutting down gracefully instead of suspending...");
    } else {
        dtnex_log("Received signal %d, shutting down gracefully...", sig);
    }
    
    // Set global flag to stop the main loop
    running = 0;
    
    // Only perform ION cleanup if we're actually connected to ION
    if (ionConnected && sap != NULL) {
        // Interrupt any pending receives if we have an open endpoint
        dtnex_log("Interrupting BP endpoint");
        bp_interrupt(sap);
        
        // Stop bundle reception service
        stopBundleReception(&bundleReceptionState);
        
        // Stop bpecho service
        bpechoState.running = 0;
        if (bpechoState.sap != NULL) {
            bp_interrupt(bpechoState.sap);
            ionPauseAttendant(&bpechoState.attendant);
            
            // Wait for bpecho thread to terminate before closing its resources
            dtnex_log("Waiting for bpecho service to terminate...");
            pthread_join(bpechoThread, NULL);
        }
        
        // Force cleanup and exit for all signals since main loop might be blocked
        dtnex_log("Performing cleanup and immediate exit...");
        
        // Close endpoints directly
        dtnex_log("🔌 Closing BP endpoint");
        bp_close(sap);
        sap = NULL;
        
        // Close bpecho endpoint if it exists (should be closed by thread, but double-check)
        if (bpechoState.sap != NULL) {
            bp_close(bpechoState.sap);
            bpechoState.sap = NULL;
        }
        
        // Detach from BP
        dtnex_log("🧹 Detaching from ION BP system");
        bp_detach();
    } else {
        dtnex_log("Performing cleanup without ION detachment (not connected)...");
        // Reset states even if not connected to ION
        bpechoState.running = 0;
        bundleReceptionState.running = 0;
    }
    
    dtnex_log("DTNEXC shutdown complete");
    exit(0);
}

/**
 * Wrapper sottile attorno al modulo ion_contacts: stampa la tabella
 * diagnostica, verifica esplicitamente che ION sia vivo e coerente
 * (§6.6), poi genera lo snapshot dei contatti annunciabili e il grafo.
 */
void getContacts(DtnexConfig *config) {
    int contactCount;
    int alive;

    contactCount = ionc_print_contact_table(config->debugMode);

    if (contactCount < 0) {
        // ION non accessibile: puo' essere un restart o una disconnessione.
        dtnex_log("⚠️  Cannot access ION contact database - ION may have been restarted");

        if (sap != NULL) {
            bp_close(sap);
            sap = NULL;
        }
        ionConnected = 0;
        restartDtnex(config);
        return;
    }

    // Rilevazione esplicita del restart (§6.6): "zero contatti" NON e' un
    // indizio di restart, un nodo appena avviato ne ha legittimamente zero.
    alive = ionc_check_alive(config->nodeId);
    if (alive != 1) {
        dtnex_log("⚠️  ION restart rilevato (ownNodeNbr non piu' %lu)", config->nodeId);

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

    // Snapshot dei contatti annunciabili: e' esattamente cio' che finisce
    // sul filo, quindi va confrontato con 'l contact' di ionadmin.
    if (config->debugMode) {
        ContactRecord snapshot[IONC_MAX_CONTACTS];
        int snapshotCount = ionc_get_own_contacts(config->nodeId, snapshot,
                IONC_MAX_CONTACTS, config->debugMode);

        if (snapshotCount < 0) {
            dtnex_log("⚠️  Impossibile leggere lo snapshot dei contatti annunciabili");
        } else {
            dtnex_log("\033[36mContatti annunciabili (fromNode == %lu): %d\033[0m",
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
    
    graphFile = fopen(graphvizFile, "w");
    if (!graphFile) {
        dtnex_log("Failed to open graph file for writing: %s", graphvizFile);
        return;
    }
    
    // Write the comment header with instructions
    fprintf(graphFile, "// DTN Contact Graph generated by DTNEXC\n");
    fprintf(graphFile, "// To generate an image from this file, run:\n");
    fprintf(graphFile, "// dot -Tpng %s -o %s\n", graphvizFile, config->graphFile);
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
    /** Come prende i contatti pr graficare gli archi tra i nodi:
     * Invia l contact allo stdin di ionadmin
     * cattura l'output
     * lo filtra con grep per estrarre solo le linee che contengono i contatti
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
    
    // Don't set separate signal handler for bpecho - use main process handler
    
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
 *  Pipeline di recezione
 * runBundleReception()          ← thread dedicato, legge bundle da ION
    └── processCborMessage()  ← entry point, validazione minima
            └── decodeCborMessage()   ← parsing CBOR + autenticazione
                    ├── processCborContactMessage()   ← tipo "c"
                    │       └── forwardCborContactMessage()
                    └── processCborMetadataMessage()  ← tipo "m"
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
                zco_start_receiving(dlv.adu, &reader); // Inizializza lo zco reader per leggere il payload del bundle, analogo a una open
                CHKZERO(sdr_begin_xn(sdr));
                int len = zco_receive_source(sdr, &reader, contentLength, buffer); // copia il payload nel buffer
                if (sdr_end_xn(sdr) < 0 || len < 0) {
                    dtnex_log("❌ Error reading bundle content");
                    bp_release_delivery(&dlv, 1);
                    continue;
                }
                
                // Build source info string
                // Source info mai usata per ora ma potrebbe essere utile per logging e debugging futuri
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
                // Il payload del bundle è ora in buffer con lunghezza contentLength -> viene procesato
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
    
    // Store original arguments for potential restart
    original_argc = argc;
    original_argv = argv;
    
    // Set up signal handlers for clean shutdown with signal masking
    struct sigaction sa;
    sigset_t mask;
    
    // Block signals during handler execution
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGTSTP);
    
    sa.sa_handler = signalHandler;
    sa.sa_mask = mask;
    sa.sa_flags = SA_RESTART; // Restart interrupted system calls
    
    sigaction(SIGINT, &sa, NULL);   // Ctrl+C
    sigaction(SIGTERM, &sa, NULL);  // kill
    sigaction(SIGTSTP, &sa, NULL);  // Ctrl+Z
    
    // Load configuration
    loadConfig(&config);
    
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
    dtnex_dbg("[main] About to call getplanlist for startup contact broadcast to all neighbors");
    getplanlist(&config, plans, &planCount);
    dtnex_dbg("[main] getplanlist returned planCount=%d", planCount);
    for (int _i = 0; _i < planCount; _i++) {
        char _ts_str[32] = "N/A";
        if (plans[_i].timestamp > 0) {
            struct tm *_tm = gmtime(&plans[_i].timestamp);
            strftime(_ts_str, sizeof(_ts_str), "%Y-%m-%dT%H:%M:%SZ", _tm);
        }
        dtnex_dbg("[main] plans[%d]: planId=%lu timestamp=%s", _i, plans[_i].planId, _ts_str);
    }
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
    
    // Wait for bundle reception thread to terminate if it's running
    if (bundleReceptionState.running) {
        dtnex_log("Waiting for bundle reception thread to terminate...");
        stopBundleReception(&bundleReceptionState);
        pthread_join(bundleReceptionState.thread, NULL);
    }
    
    // Wait for bpecho thread to terminate if it's running
    if (bpechoState.running) {
        dtnex_log("Waiting for bpecho service to terminate...");
        pthread_join(bpechoThread, NULL);
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
 * Calculate HMAC-SHA256 (truncated to DTNEX_HMAC_SIZE for size efficiency)
 */
int calculateHmac(const unsigned char *message, int msgLen, const char *key, unsigned char *hmac) {
    unsigned char fullHmac[SHA256_DIGEST_SIZE];
    
    // Simple HMAC implementation (not OpenSSL's HMAC for minimal dependencies)
    unsigned char keyPad[64]; // Block size for SHA-256
    memset(keyPad, 0, sizeof(keyPad));
    
    int keyLen = strlen(key);
    if (keyLen > 64) {
        // Hash key if longer than block size
        SHA256((unsigned char*)key, keyLen, keyPad);
    } else {
        memcpy(keyPad, key, keyLen);
    }
    
    // Create inner and outer padding
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = keyPad[i] ^ 0x36;
        opad[i] = keyPad[i] ^ 0x5c;
    }
    
    // Inner hash: SHA256(key XOR ipad || message)
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, ipad, 64);
    SHA256_Update(&ctx, message, msgLen);
    SHA256_Final(fullHmac, &ctx);
    
    // Outer hash: SHA256(key XOR opad || inner_hash)
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, opad, 64);
    SHA256_Update(&ctx, fullHmac, SHA256_DIGEST_SIZE);
    SHA256_Final(fullHmac, &ctx);
    
    // Use only first DTNEX_HMAC_SIZE bytes for size optimization
    memcpy(hmac, fullHmac, DTNEX_HMAC_SIZE);
    return DTNEX_HMAC_SIZE;
}

/**
 * Verify HMAC
 */
int verifyHmac(DtnexConfig *config, const unsigned char *message, int msgLen, const unsigned char *receivedHmac, const char *key) {
    unsigned char calculatedHmac[DTNEX_HMAC_SIZE];
    calculateHmac(message, msgLen, key, calculatedHmac);
    
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

    (void) bufferSize;  // il payload v3 e' ~67 byte, MAX_CBOR_BUFFER e' 128

    // Generate nonce
    generateNonce(nonce);

    time_t currentTime = time(NULL);

    // §7.2: il messaggio e' utile esattamente finche' e' valido il contatto
    // che descrive, quindi expireTime E' il toTime del contatto.
    time_t expireTime = contact->toTime;

    // Encode main array with 9 elements [version, type, ts, exp, orig, from, nonce, data, hmac]
    bytesWritten += cbor_encode_array_open(9, &cursor);

    // 1. Version
    bytesWritten += cbor_encode_integer(DTNEX_PROTOCOL_VERSION, &cursor);

    // 2. Message type "c"
    bytesWritten += cbor_encode_text_string("c", 1, &cursor);

    // 3. Timestamp (istante di invio, non ha piu' effetto sulla finestra)
    bytesWritten += cbor_encode_integer(currentTime, &cursor);

    // 4. Expire time
    bytesWritten += cbor_encode_integer(expireTime, &cursor);

    // 5. Origin node
    bytesWritten += cbor_encode_integer(config->nodeId, &cursor);

    // 6. From node (same as origin for originating messages)
    bytesWritten += cbor_encode_integer(config->nodeId, &cursor);

    // 7. Nonce
    bytesWritten += cbor_encode_byte_string(nonce, DTNEX_NONCE_SIZE, &cursor);

    // 8. Contact data array v3 (§5.2): 7 campi, tempi assoluti
    bytesWritten += cbor_encode_array_open(7, &cursor);
    bytesWritten += cbor_encode_integer(contact->fromNode, &cursor);
    bytesWritten += cbor_encode_integer(contact->toNode, &cursor);
    bytesWritten += cbor_encode_integer((uvast) contact->fromTime, &cursor);
    bytesWritten += cbor_encode_integer((uvast) contact->toTime, &cursor);
    bytesWritten += cbor_encode_integer(contact->xmitRate, &cursor);
    bytesWritten += cbor_encode_integer(contact->confidence, &cursor);
    bytesWritten += cbor_encode_integer(contact->owlt, &cursor);

    // 9. Calculate HMAC over everything except the HMAC field itself
    unsigned char hmac[DTNEX_HMAC_SIZE];
    calculateHmac(buffer, bytesWritten, config->presSharedNetworkKey, hmac);
    bytesWritten += cbor_encode_byte_string(hmac, DTNEX_HMAC_SIZE, &cursor);

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
    
    // Encode main array with 9 elements
    bytesWritten += cbor_encode_array_open(9, &cursor);
    
    // 1. Version
    bytesWritten += cbor_encode_integer(DTNEX_PROTOCOL_VERSION, &cursor);
    
    // 2. Message type "m"
    bytesWritten += cbor_encode_text_string("m", 1, &cursor);
    
    // 3. Timestamp
    bytesWritten += cbor_encode_integer(currentTime, &cursor);
    
    // 4. Expire time
    bytesWritten += cbor_encode_integer(expireTime, &cursor);
    
    // 5. Origin node
    bytesWritten += cbor_encode_integer(config->nodeId, &cursor);
    
    // 6. From node
    bytesWritten += cbor_encode_integer(config->nodeId, &cursor);
    
    // 7. Nonce
    bytesWritten += cbor_encode_byte_string(nonce, DTNEX_NONCE_SIZE, &cursor);
    
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
    
    // 9. Calculate HMAC
    unsigned char hmac[DTNEX_HMAC_SIZE];
    calculateHmac(buffer, bytesWritten, config->presSharedNetworkKey, hmac);
    bytesWritten += cbor_encode_byte_string(hmac, DTNEX_HMAC_SIZE, &cursor);
    
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
    // Adessp crea un bundle di tipo ZeroCopyObject partendo direttamente dai dati in SDR
    bundleZco = ionCreateZco(ZcoSdrSource, extent, 0, dataSize, 
                            BP_STD_PRIORITY, 0, ZcoOutbound, NULL);
    
    if (bundleZco == 0 || bundleZco == (Object) ERROR) {
        dtnex_log("Can't create ZCO extent");
        return -1;
    }
    
    // Send the bundle using direct ION API - no source EID for CBOR messages
    dtnex_dbg("[sendCborBundle] bp_send: destEid=%s dataSize=%d ttl=%d",
        destEid, dataSize, ttl);
    {
        char _hexbuf[256] = {0};
        int _hlen = dataSize < 64 ? dataSize : 64;
        for (int _b = 0; _b < _hlen; _b++) {
            snprintf(_hexbuf + _b * 3, 4, "%02x ", ((unsigned char*)cborData)[_b]);
        }
        dtnex_dbg("[sendCborBundle] CBOR payload (first %d bytes): %s", _hlen, _hexbuf);
    }
    sendResult = bp_send(NULL, destEid, NULL, ttl, BP_STD_PRIORITY,
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
  * while loop a stati con due timeline parallele: una per gli update programmati (updateInterval)
  * e una per i retry di connessione a ION.
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
        
        // A ogni risveglio (<= 60s): rivaluta i trigger di annuncio (§7.1).
        // exchangeWithNeighbors decide da sola se c'e' qualcosa da fare.
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
    if (!buffer || bufferSize <= 0) { // fa solo validazione del puntatore del buffer e della dimensione
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
 * Viene fatto il parsing di CBOR e poi processato in base al tipo di messaggio (contact o metadata).
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
    // Store the data array position for later processing
    unsigned char *dataArrayPosition = cursor;
    
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
        // Contact message v3 (§5.2): 7 campi
        debug_log(config, "🔍 Extracting 7 contact elements manually");

        if (dataArraySize != 7) {
            debug_log(config, "❌ Payload contatto con %lu campi (attesi 7)", dataArraySize);
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

/* Oltre questa distanza nel futuro una finestra e' sospetta di clock skew
 * anziche' legittima: si scarta e lo si dice (§7.6). */
#define CLOCK_SKEW_FUTURE_LIMIT (30 * 24 * 3600)

int processCborContactMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime,
                             unsigned long origin, unsigned long from, ContactRecord *contact) {
    time_t currentTime = time(NULL);
    IoncApplyOutcome outcome;

    log_message_received(config, origin, from, "contact",
            contact->fromNode, contact->toNode, NULL);

    /* Pipeline di validazione (§6.1). I controlli 1-3 (versione, HMAC,
     * nonce) sono gia' stati fatti in decodeCborMessage. Ogni fallimento
     * scarta senza inserire e senza inoltrare, e restituisce 0: non e' un
     * errore di decodifica (il messaggio si e' decodificato perfettamente),
     * e' un rifiuto di policy gia' loggato qui sotto col suo motivo. Un -1
     * risalirebbe fino a decodeCborMessage e verrebbe stampato come "Failed
     * to decode CBOR message", diagnostica fuorviante per uno scarto valido. */

    // 4. Non processiamo i nostri stessi messaggi
    if (origin == config->nodeId) {
        debug_log(config, "⏭️ Skipping own contact message");
        return 0;
    }

    // 5. Solo la sorgente annuncia la propria direzione (§4)
    if (contact->fromNode != origin) {
        debug_log(config, "❌ Scartato: fromNode=%lu != origin=%lu",
                contact->fromNode, origin);
        return 0;
    }

    // 5b. Un contatto verso se stessi non e' topologia: in ION e' la semantica
    // dei contatti di registrazione. ionc_get_own_contacts lo filtra in
    // origination, ma un peer in possesso della chiave HMAC potrebbe comunque
    // iniettarlo, quindi il filtro serve anche in ricezione.
    if (contact->fromNode == contact->toNode) {
        debug_log(config, "❌ Scartato: fromNode == toNode (%lu): contatto di "
                "registrazione, non topologia", contact->fromNode);
        return 0;
    }

    // 6. toTime = 0 in ION significa "contatto scoperto" -> MAX_POSIX_TIME
    if (contact->toTime == 0) {
        debug_log(config, "❌ Scartato: toTime = 0 (semantica di contatto permanente)");
        return 0;
    }

    // 7. Integrita' della finestra
    if (contact->fromTime >= contact->toTime) {
        debug_log(config, "❌ Scartato: fromTime=%ld >= toTime=%ld",
                (long) contact->fromTime, (long) contact->toTime);
        return 0;
    }

    // 7b. fromTime <= 0 e' la semantica di "contatto ipotetico" per ION
    // (rfx_insert_contact prende il ramo CHKZERO su ownNodeNbr e puo'
    // restituire 0 senza aver scritto nulla: falso successo).
    if (contact->fromTime <= 0) {
        debug_log(config, "❌ Scartato: fromTime non valido: semantica di "
                "contatto ipotetico in ION (fromTime=%ld)", (long) contact->fromTime);
        return 0;
    }

    // 7c. fromTime == MAX_POSIX_TIME e' il trigger dei contatti di
    // registrazione in ION, non una finestra di rete reale.
    if (contact->fromTime >= MAX_POSIX_TIME) {
        debug_log(config, "❌ Scartato: fromTime = MAX_POSIX_TIME: semantica "
                "di contatto di registrazione");
        return 0;
    }

    // 7d. xmitRate == 0 verrebbe rifiutato da ION stesso (rfx_insert_contact
    // restituisce 5).
    if (contact->xmitRate == 0) {
        debug_log(config, "❌ Scartato: xmitRate nullo: ION rifiuterebbe il contatto");
        return 0;
    }

    // 7e. confidence fuori range: ION restituisce 4 per confidence > 1.0.
    if (contact->confidence > 100) {
        debug_log(config, "❌ Scartato: confidence fuori range 0-100 (%u)",
                contact->confidence);
        return 0;
    }

    // 8. Finestra gia' scaduta
    if (contact->toTime <= currentTime) {
        debug_log(config, "❌ Scartato: finestra interamente nel passato "
                "(toTime=%ld, adesso=%ld) — possibile clock skew fra i nodi",
                (long) contact->toTime, (long) currentTime);
        return 0;
    }

    // 8b. Finestra troppo nel futuro: sospetto di clock skew (§7.6)
    if (contact->fromTime > currentTime + CLOCK_SKEW_FUTURE_LIMIT) {
        debug_log(config, "❌ Scartato: finestra troppo nel futuro "
                "(fromTime=%ld, adesso=%ld) — possibile clock skew fra i nodi",
                (long) contact->fromTime, (long) currentTime);
        return 0;
    }

    /* Non c'e' un controllo 9 sull'owlt: 0 e' un OWLT legittimo (su una LAN e'
     * il valore fisicamente corretto, e ION accetta "a range ... 0"). Scartarlo
     * qui contraddiceva l'origination, dove findOwlt restituisce 0 come valore
     * valido, e su un testbed locale faceva scartare tutto a ogni ricevente.
     * Il formato v3 porta sempre il campo e ionc_get_own_contacts annuncia solo
     * contatti per cui un range esiste davvero: il controllo era ridondante. */

    /* Si scrive cio' che si impara, si annuncia solo cio' di cui si e'
     * autoritativi (§4.5): anche i contatti con toNode == me si inseriscono. */
    outcome = ionc_apply_contact(contact, config->debugMode);
    if (outcome == IONC_ERROR) {
        /* IONC_ERROR e' uno stato locale di ION (es. sovrapposizione con un
         * contatto configurato a mano), non un fallimento di validazione:
         * il messaggio resta valido e va comunque inoltrato (§6.5), altrimenti
         * un problema puramente locale partizionerebbe il flooding. */
        dtnex_log("❌ Applicazione in ION fallita per %lu→%lu",
                contact->fromNode, contact->toNode);
    } else if (outcome != IONC_NOOP) {
        dtnex_log("✅ Contatto %lu→%lu %s in ION",
                contact->fromNode, contact->toNode, ionc_outcome_name(outcome));
    }

    // Inoltro invariato (§6.5): un messaggio che supera la validazione si
    // inoltra sempre, indipendentemente dall'esito della scrittura locale in ION.
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
    int messageSize;
    
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

        // Encode forwarded CBOR message
        bytesWritten += cbor_encode_array_open(9, &cursor);
        bytesWritten += cbor_encode_integer(DTNEX_PROTOCOL_VERSION, &cursor);
        bytesWritten += cbor_encode_text_string("c", 1, &cursor);
        bytesWritten += cbor_encode_integer(timestamp, &cursor);
        bytesWritten += cbor_encode_integer(expireTime, &cursor);
        bytesWritten += cbor_encode_integer(origin, &cursor);  // Keep original origin
        bytesWritten += cbor_encode_integer(config->nodeId, &cursor);  // Update "from" to our node, CAMBIA SOLO IL FROM CON IL NODE ID DI QUESTO NODO
        bytesWritten += cbor_encode_byte_string(originalNonce, DTNEX_NONCE_SIZE, &cursor);

        // Contact data v3
        bytesWritten += cbor_encode_array_open(7, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.fromNode, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.toNode, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.fromTime, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.toTime, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.xmitRate, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.confidence, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.owlt, &cursor);

        // Calculate HMAC over everything except HMAC itself
        unsigned char hmac[DTNEX_HMAC_SIZE];
        calculateHmac(cborBuffer, bytesWritten, config->presSharedNetworkKey, hmac);
        bytesWritten += cbor_encode_byte_string(hmac, DTNEX_HMAC_SIZE, &cursor);

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
    int messageSize;
    
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
        
        // Encode forwarded CBOR message
        bytesWritten += cbor_encode_array_open(9, &cursor);
        bytesWritten += cbor_encode_integer(DTNEX_PROTOCOL_VERSION, &cursor);
        bytesWritten += cbor_encode_text_string("m", 1, &cursor);
        bytesWritten += cbor_encode_integer(timestamp, &cursor);
        bytesWritten += cbor_encode_integer(expireTime, &cursor);
        bytesWritten += cbor_encode_integer(origin, &cursor);  // Keep original origin
        bytesWritten += cbor_encode_integer(config->nodeId, &cursor);  // Update "from" to our node
        bytesWritten += cbor_encode_byte_string(originalNonce, DTNEX_NONCE_SIZE, &cursor);
        
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
        
        // Calculate HMAC over everything except HMAC itself
        unsigned char hmac[DTNEX_HMAC_SIZE];
        calculateHmac(cborBuffer, bytesWritten, config->presSharedNetworkKey, hmac);
        bytesWritten += cbor_encode_byte_string(hmac, DTNEX_HMAC_SIZE, &cursor);
        
        // Send forwarded CBOR bundle
        sprintf(destEid, "ipn:%lu.%s", neighborId, config->serviceNr);
        sendCborBundle(destEid, cborBuffer, bytesWritten, config->bundleTTL);
        
        log_message_forwarded(config, origin, from, neighborId, "metadata", 
                             metadata->nodeId, 0, metadata->name);
    }
}