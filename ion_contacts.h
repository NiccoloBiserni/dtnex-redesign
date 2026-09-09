/** \file ion_contacts.h
 * 
 *  \brief DTNEX - This module provides access to ION for contacts and ranges.
 *
 ** \copyright da inserire

 ** \par License
 **
 **    da inserire
 *
 *  \author Niccolò Biserni, niccolo.biserni@studio.unibo.it
 *
 *  \par Supervisor
 *          Carlo Caini, carlo.caini@unibo.it
 * 
 *  \par Co-Supervisor
 *          Samo Grasic, samo@grasic.net 
 *
 *
 *  \par Revision History:
 *
 *  DD/MM/YYYY |  AUTHOR         |   DESCRIPTION
 *  ---------- | --------------- | -----------------------------------------------
 *  28/08/2026 | N. Biserni      |  Initial implementation and documentation.
 */

/**
 * Module boundary: this module talks ONLY to ION. It knows
 * nothing about CBOR, bundles, HMAC, neighbours or flooding. If encoding
 * ever leaks in here, the module stops being verifiable on its own and the
 * only reason it exists is lost.
 */

#ifndef ION_CONTACTS_H
#define ION_CONTACTS_H

#include <time.h>

/* Maximum size of a snapshot of announceable contacts. */
#define IONC_MAX_CONTACTS 200

/**
 * A contact, in ION's own units:
 *   regionNbr       : ION region number (IonCXref.regionNbr)
 *   fromTime/toTime : absolute UNIX epoch
 *   xmitRate        : bytes per second
 *   confidence      : percentage 0-100 (ION uses a 0.0-1.0 float;
 *                     cbor.h cannot encode floats)
 *
 * The owlt no longer lives here: the range is an entity of its own, with its
 * own RangeRecord and its own message type "r".
 */
typedef struct {
    unsigned int  regionNbr;
    unsigned long fromNode;
    unsigned long toNode;
    time_t        fromTime;
    time_t        toTime;
    unsigned long xmitRate;
    unsigned int  confidence;
} ContactRecord;

/* Maximum size of a snapshot of announceable ranges. */
#define IONC_MAX_RANGES 200

/**
 * A range, in ION's own units:
 *   fromTime/toTime : absolute UNIX epoch
 *   owlt            : seconds
 *
 * The range carries no region, and that is not an oversight:
 * rfx_insert_range() and rfx_remove_range() have no region parameter
 * (rfx.h:130 and :165) and IonRXref has no such field (ion.h:418-426). In
 * ION ranges are global; only contacts are regional.
 */
typedef struct {
    unsigned long fromNode;
    unsigned long toNode;
    time_t        fromTime;
    time_t        toTime;
    unsigned int  owlt;
} RangeRecord;

/**
 * Reads from ION the contacts the local node is allowed to announce.
 *
 * Filters applied:
 *   - fromNode == myNodeId          (authority rule)
 *   - toNode != fromNode            (excludes registration contacts)
 *   - type in {CtScheduled, CtPredicted}
 *   - toTime > now                  (expired contacts are not announced)
 *
 * Returns the number of records written to out, or -1 if ION is not
 * reachable (SDR, vdb or working memory unavailable).
 */
int ionc_get_own_contacts(unsigned long myNodeId, ContactRecord *out,
        int maxRecords, int debugMode);

/**
 * Reads from ION the ranges the local node is allowed to announce.
 *
 * Filters applied, the same as for contacts as far as they make sense:
 *   - fromNode == myNodeId          (authority rule)
 *   - toNode != fromNode
 *   - rangeElt != 0                 (asserted ranges only, never the reverse
 *                                    ones ION imputes; the why is in the
 *                                    comment on that filter in the .c)
 *   - toTime > now                  (expired ranges are not announced)
 *
 * Returns the number of records written to out, or -1 if ION is not
 * reachable.
 */
int ionc_get_own_ranges(unsigned long myNodeId, RangeRecord *out,
        int maxRecords, int debugMode);

/**
 * Outcome of applying a received contact or range. Each of ionc_apply_contact
 * and ionc_apply_range reports its own outcome; the values are listed in
 * increasing order of severity, so a caller that applies both and wants a
 * single answer keeps the higher of the two. That merge is the caller's
 * business, not this module's.
 */
typedef enum {
    IONC_NOOP = 0,      /* ION was already in sync: nothing was written */
    IONC_REVISED,       /* xmitRate/confidence updated in place */
    IONC_INSERTED,      /* the entry was new: it was inserted */
    IONC_REPLACED,      /* window changed: targeted remove + insert */
    IONC_LOST,          /* the previous entry is gone and was not replaced,
                          * so the topology held by ION is worse than it was
                          * before the call. Either the remove succeeded and
                          * the insert that followed was rejected, or ION
                          * itself dropped the entry before refusing */
    IONC_ERROR          /* an rfx_* call failed: unexpected ION state */
} IoncApplyOutcome;

/**
 * Applies the received contact to ION, keyed by identity
 * (regionNbr, fromNode, toNode, fromTime).
 *
 * The contact only: the range that goes with it is written by
 * ionc_apply_range, which the caller invokes separately.
 *
 * Idempotent: if ION already holds exactly this contact, nothing is
 * written. Removals ALWAYS pass a pointer to the exact fromTime, never
 * NULL: with NULL, ION applies the '*' scope and deletes every contact
 * between the pair, including those configured by the operator.
 */
IoncApplyOutcome ionc_apply_contact(const ContactRecord *rec, int debugMode);

/**
 * Applies the received range to ION, keyed by (fromNode, toNode, fromTime).
 *
 * Idempotent like ionc_apply_contact. rfx_revise_range does not exist, so any
 * difference in owlt or in the window means remove + insert — except over a
 * range ION imputed by itself (rangeElt == 0), where rfx_insert_range performs
 * the substitution on its own and removing first is harmful, not merely
 * redundant. Do not turn that exception back into a remove + insert.
 *
 * Removals ALWAYS pass a pointer to the exact fromTime, never NULL: with
 * NULL, ION applies the '*' scope and deletes every range between the pair,
 * including those configured by the operator.
 */
IoncApplyOutcome ionc_apply_range(const RangeRecord *rec, int debugMode);

/* Human-readable name of the outcome, for logging. */
const char *ionc_outcome_name(IoncApplyOutcome outcome);

/**
 * Prints the diagnostic table of the contacts held by ION (all of them,
 * not just ours). Returns the number of contacts, or -1 if ION is not
 * reachable. The detailed dump is produced only when debugMode != 0.
 */
int ionc_print_contact_table(int debugMode);

/**
 * Checks that ION is alive and is still the same instance: reads
 * ownNodeNbr from the IonDB and compares it against the expected value.
 *
 * Returns 1 if ION is alive and consistent, 0 if it restarted or was
 * reconfigured with a different node number, -1 if it is not reachable.
 *
 * Do NOT use "zero contacts" as a hint of a restart: a freshly started or
 * edge node legitimately has zero contacts.
 */
int ionc_check_alive(unsigned long expectedNodeId);

#endif /* ION_CONTACTS_H */
