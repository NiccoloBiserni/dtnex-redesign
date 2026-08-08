/**
 * dtnex.h
 * DTNEX - DTN Network Information Exchange
 * High-performance C implementation for exchanging DTN contact and metadata information
 * Author: Samo Grasic (samo@grasic.net)
 */

#ifndef DTNEX_H
#define DTNEX_H

#include "include/ion/bp.h"
#include "include/ion/rfx.h"
#include "include/ion/ion.h"
// Internal ION structures we need
#include "include/ion/bpP.h"
#include "include/ion/smlist.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <openssl/sha.h>

// CBOR support using ION's implementation
#include "include/ion/cbor.h"

// Accesso a ION per contatti e range (modulo separato, vedi ion_contacts.h)
#include "ion_contacts.h"

// Added for bpecho service
#include "include/ion/zco.h"
#include "include/ion/lyst.h"
#include "include/ion/platform.h"

// External functions from BP/ION library needed
extern Sdr getIonsdr();
extern IonVdb *getIonVdb();
extern PsmPartition getIonwm();

// External functions from libbpP.c needed for direct endpoint registration
extern int addEndpoint(char *endpointName, BpRecvRule recvAction, char *recvScript);
extern int updateEndpoint(char *endpointName, BpRecvRule recvAction, char *recvScript);
extern int removeEndpoint(char *endpointName);

// BPlan, BpDB, and other structures are imported from bpP.h

// Function prototypes for functions that might not be fully defined
char *strptime(const char *s, const char *format, struct tm *tm);

// Version information
#define DTNEXC_VERSION "3.00"
#define DTNEXC_BUILD_DATE __DATE__
#define DTNEXC_BUILD_TIME __TIME__

// Configuration constants - event-driven operation
#define DEFAULT_UPDATE_INTERVAL 600        // 10 minutes between updates
#define DEFAULT_CONTACT_LIFETIME 3600      // 1 hour contact validity
#define DEFAULT_BUNDLE_TTL 1800            // 30 minutes bundle TTL (3x update interval)
#define DEFAULT_SERVICE_NR 12160
#define DEFAULT_BPECHO_SERVICE_NR 12161
#define DEFAULT_PRESHARED_KEY "open"
#define MAX_HASH_CACHE 5000
#define MAX_METADATA_LENGTH 512  // Increased to handle longer metadata strings
#define MAX_EID_LENGTH 64
#define MAX_LINE_LENGTH 1024
#define MAX_PLANS 100
#define SHA256_DIGEST_SIZE 32

// For bpecho service
#define BPECHO_ADU_LEN 1024

// CBOR message constants
#define DTNEX_PROTOCOL_VERSION 3
#define DTNEX_NONCE_SIZE 3        // 3-byte nonce for ultra-minimal size
#define DTNEX_HMAC_SIZE 8         // 64-bit HMAC for size optimization
#define MAX_CBOR_BUFFER 128       // Maximum CBOR message size (increased for metadata)
#define GPS_PRECISION_FACTOR 1000000  // GPS coordinate precision (6 decimal places)
#define MAX_NODE_NAME_LENGTH 64   // Maximum node name length
#define MAX_CONTACT_INFO_LENGTH 128  // Maximum contact info length
#define MAX_LOCATION_LENGTH 64    // Maximum location field length

// Structure definitions
typedef struct {
    int updateInterval;
    int contactLifetime;
    int bundleTTL;
    char presSharedNetworkKey[64];
    char serviceNr[16];
    char bpechoServiceNr[16];
    unsigned long nodeId;  // Using unsigned long to store larger node IDs
    char nodemetadata[MAX_METADATA_LENGTH];
    int createGraph;
    char graphFile[256];
    int noMetadataExchange; // Flag to indicate if we should exchange our own metadata
    int serviceMode;        // Run as background service
    int debugMode;          // Enable debug output
    double gpsLatitude;     // Node GPS latitude
    double gpsLongitude;    // Node GPS longitude
    int hasGpsCoordinates;  // Flag indicating GPS coordinates are set
} DtnexConfig;

typedef struct {
    unsigned long planId;  // Using unsigned long to store larger plan IDs
    time_t timestamp;
} Plan;

typedef struct {
    char hash[20];  // Store the first 10 characters of SHA-256 hash
    time_t timestamp;
} HashCache;

typedef struct {
    unsigned long nodeId;  // Using unsigned long to store larger node IDs
    char metadata[MAX_METADATA_LENGTH];
} NodeMetadata;

// Ultra-minimal metadata structure for CBOR messages
typedef struct {
    unsigned long nodeId;
    char name[MAX_NODE_NAME_LENGTH];
    char contact[MAX_CONTACT_INFO_LENGTH]; 
    char location[MAX_LOCATION_LENGTH];    // Text-based location field
    int latitude;              // Latitude * GPS_PRECISION_FACTOR (0 = not set)
    int longitude;             // Longitude * GPS_PRECISION_FACTOR (0 = not set)
} StructuredMetadata;

// Nonce cache for replay protection
typedef struct {
    unsigned char nonce[DTNEX_NONCE_SIZE];
    unsigned long origin;
    time_t timestamp;
} NonceCache;

// ION connection status
typedef enum {
    ION_STATUS_UNKNOWN = 0,
    ION_STATUS_RUNNING,
    ION_STATUS_STOPPED,
    ION_STATUS_ERROR
} IonStatus;

// No thread argument needed in the single-threaded version

// Bpecho service state structure
typedef struct {
    BpSAP sap;
    int running;
    ReqAttendant attendant;
} BpechoState;

// Bundle reception state structure
typedef struct {
    DtnexConfig *config;
    int running;
    pthread_t thread;
} BundleReceptionState;

// Function prototypes
void loadConfig(DtnexConfig *config);
int tryConnectToIon(DtnexConfig *config);
int init(DtnexConfig *config);
void getplanlist(DtnexConfig *config, Plan *plans, int *planCount);
void exchangeWithNeighbors(DtnexConfig *config, Plan *plans, int planCount);
void getContacts(DtnexConfig *config);
void createGraph(DtnexConfig *config);
void updateNodeMetadata(DtnexConfig *config, unsigned long nodeId, const char *metadata);
void dtnex_log(const char *format, ...);

// Optimized message logging functions
void log_message_sent(DtnexConfig *config, unsigned long origin, unsigned long to, const char *type, 
                     unsigned long nodeA, unsigned long nodeB, const char *metadata);
void log_message_received(DtnexConfig *config, unsigned long origin, unsigned long from, const char *type,
                         unsigned long nodeA, unsigned long nodeB, const char *metadata);
void log_message_forwarded(DtnexConfig *config, unsigned long origin, unsigned long from, unsigned long to,
                          const char *type, unsigned long nodeA, unsigned long nodeB, const char *metadata);
void log_message_error(DtnexConfig *config, const char *error_msg);
void log_contact_update(DtnexConfig *config, int contactCount);

// CBOR message functions (using ION's CBOR implementation)
int encodeCborContactMessage(DtnexConfig *config, ContactRecord *contact, unsigned char *buffer, int bufferSize);
int encodeCborMetadataMessage(DtnexConfig *config, StructuredMetadata *metadata, unsigned char *buffer, int bufferSize);
void processCborMessage(DtnexConfig *config, unsigned char *buffer, int bufferSize);
int decodeCborMessage(DtnexConfig *config, unsigned char *buffer, int bufferSize);
int processCborContactMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime,
                             unsigned long origin, unsigned long from, ContactRecord *contact);
int processCborMetadataMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime,
                              unsigned long origin, unsigned long from, StructuredMetadata *metadata);

// Helper functions for CBOR-only implementation
void parseNodeMetadata(const char *rawMetadata, StructuredMetadata *metadata);
int sendCborBundle(const char *destEid, unsigned char *cborData, int dataSize, int ttl);
void generateNonce(unsigned char *nonce);
int calculateHmac(const unsigned char *message, int msgLen, const char *key, unsigned char *hmac);
int verifyHmac(DtnexConfig *config, const unsigned char *message, int msgLen, const unsigned char *receivedHmac, const char *key);
int isNonceDuplicate(unsigned char *nonce, unsigned long origin);
void addNonceToCache(unsigned char *nonce, unsigned long origin);
int manualDecodeCborInteger(unsigned long *value, unsigned char **cursor, unsigned int *bytesBuffered);
int manualDecodeCborString(char *buffer, int maxLen, unsigned char **cursor, unsigned int *bytesBuffered);

// CBOR message forwarding functions
void forwardCborContactMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp,
                               time_t expireTime, unsigned long origin, unsigned long from, ContactRecord *contact);
void forwardCborMetadataMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp,
                                time_t expireTime, unsigned long origin, unsigned long from, StructuredMetadata *metadata);

// Service mode and event-driven operation functions
IonStatus checkIonStatus(DtnexConfig *config);
void serviceLoop(DtnexConfig *config);
void parseArguments(int argc, char **argv, DtnexConfig *config);
void eventDrivenLoop(DtnexConfig *config);
void restartDtnex(DtnexConfig *config);
void scheduleNextUpdate(DtnexConfig *config, time_t *nextUpdateTime);

// Logging with debug mode support
void debug_log(DtnexConfig *config, const char *format, ...);
void minimal_log(const char *format, ...);


// Bpecho service functions
int initBpechoService(DtnexConfig *config, BpechoState *state);
void *runBpechoService(void *arg);

// Bundle reception functions
int initBundleReception(DtnexConfig *config, BundleReceptionState *state);
void *runBundleReception(void *arg);
void stopBundleReception(BundleReceptionState *state);

// Global variables
extern volatile int running;
extern volatile int ionConnected;  // Global ION connection status
extern BpSAP sap;
extern Sdr sdr;
extern NonceCache nonceCache[MAX_HASH_CACHE]; // Reuse same size as hash cache
extern int nonceCacheCount;

#endif // DTNEXC_H