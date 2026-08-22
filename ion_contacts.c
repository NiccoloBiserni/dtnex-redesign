/**
 * ion_contacts.c
 * DTNEX - access to ION for contacts and ranges.
 */

#include "include/ion/ion.h"
#include "include/ion/rfx.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ion_contacts.h"

/* The logger lives in dtnex.c. It is declared here rather than including
 * dtnex.h, so that bundles, config and CBOR are not dragged into this module. */
extern void dtnex_log(const char *format, ...);

/**
 * Looks up the contact with the exact key (regionNbr, fromNode, toNode,
 * fromTime). Must be called with an SDR transaction already open.
 * Returns 1 and copies the contact into *copy, or 0 if it does not exist.
 */
static int findContact(PsmPartition ionwm, IonVdb *ionvdb, uint32_t regionNbr,
        uvast fromNode, uvast toNode, time_t fromTime, IonCXref *copy)
{
    PsmAddress   elt;
    PsmAddress   addr;
    IonCXref    *contact;

    if (ionvdb->contactIndex == 0) {
        return 0;
    }

    for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
            elt = sm_rbt_next(ionwm, elt)) {
        addr = sm_rbt_data(ionwm, elt);
        if (addr == 0) {
            continue;
        }

        contact = (IonCXref *) psp(ionwm, addr);
        if (contact == NULL) {
            continue;
        }

        if (contact->regionNbr == regionNbr
                && contact->fromNode == fromNode && contact->toNode == toNode
                && contact->fromTime == fromTime) {
            memcpy(copy, contact, sizeof(IonCXref));
            return 1;
        }
    }

    return 0;
}

/**
 * Looks up the range with the exact key (fromNode, toNode, fromTime).
 * Must be called with an SDR transaction already open.
 */
static int findRange(PsmPartition ionwm, IonVdb *ionvdb, uvast fromNode,
        uvast toNode, time_t fromTime, IonRXref *copy)
{
    PsmAddress   elt;
    PsmAddress   addr;
    IonRXref    *range;

    if (ionvdb->rangeIndex == 0) {
        return 0;
    }

    for (elt = sm_rbt_first(ionwm, ionvdb->rangeIndex); elt;
            elt = sm_rbt_next(ionwm, elt)) {
        addr = sm_rbt_data(ionwm, elt);
        if (addr == 0) {
            continue;
        }

        range = (IonRXref *) psp(ionwm, addr);
        if (range == NULL) {
            continue;
        }

        if (range->fromNode == fromNode && range->toNode == toNode
                && range->fromTime == fromTime) {
            memcpy(copy, range, sizeof(IonRXref));
            return 1;
        }
    }

    return 0;
}

const char *ionc_outcome_name(IoncApplyOutcome outcome)
{
    switch (outcome) {
    case IONC_NOOP:     return "no-op";
    case IONC_REVISED:  return "revised";
    case IONC_INSERTED: return "inserted";
    case IONC_REPLACED: return "replaced";
    case IONC_LOST:     return "lost";
    default:            return "error";
    }
}

int ionc_get_own_contacts(unsigned long myNodeId, ContactRecord *out,
        int maxRecords, int debugMode)
{
    Sdr           sdr;
    IonVdb       *ionvdb;
    PsmPartition  ionwm;
    PsmAddress    elt;
    PsmAddress    addr;
    IonCXref     *contact;
    time_t        now;
    int           count = 0;

    if (out == NULL || maxRecords <= 0) {
        return -1;
    }

    sdr = getIonsdr();
    if (sdr == NULL) {
        return -1;
    }

    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return -1;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL || ionvdb->contactIndex == 0) {
        sdr_exit_xn(sdr);
        return -1;
    }

    now = time(NULL);

    for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
            elt = sm_rbt_next(ionwm, elt)) {
        if (count >= maxRecords) {
            dtnex_log("Contact snapshot full (%d): the remaining contacts "
                    "will not be announced", maxRecords);
            break;
        }

        addr = sm_rbt_data(ionwm, elt);
        if (addr == 0) {
            continue;
        }

        contact = (IonCXref *) psp(ionwm, addr);
        if (contact == NULL) {
            continue;
        }

        /* Authority rule (§4): we announce our own direction only. */
        if ((unsigned long) contact->fromNode != myNodeId) {
            continue;
        }

        /* Registration contacts (fromNode == toNode) are not topology:
         * they must not be announced. */
        if (contact->fromNode == contact->toNode) {
            continue;
        }

        /* Scheduled and Predicted are the only real windows. Discovered
         * contacts in particular have toTime = MAX_POSIX_TIME, and
         * announcing them would propagate an eternal contact (§6.1 check 6). */
        if (contact->type != CtScheduled && contact->type != CtPredicted) {
            continue;
        }

        if (contact->toTime <= now) {
            continue;
        }

        out[count].regionNbr = contact->regionNbr;
        out[count].fromNode = (unsigned long) contact->fromNode;
        out[count].toNode = (unsigned long) contact->toNode;
        out[count].fromTime = contact->fromTime;
        out[count].toTime = contact->toTime;
        out[count].xmitRate = (unsigned long) contact->xmitRate;
        out[count].confidence =
                (unsigned int) (contact->confidence * 100.0f + 0.5f);
        count++;
    }

    sdr_exit_xn(sdr);
    return count;
}

int ionc_get_own_ranges(unsigned long myNodeId, RangeRecord *out,
        int maxRecords, int debugMode)
{
    Sdr           sdr;
    IonVdb       *ionvdb;
    PsmPartition  ionwm;
    PsmAddress    elt;
    PsmAddress    addr;
    IonRXref     *range;
    time_t        now;
    int           count = 0;

    (void) debugMode;

    if (out == NULL || maxRecords <= 0) {
        return -1;
    }

    sdr = getIonsdr();
    if (sdr == NULL) {
        return -1;
    }

    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return -1;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL || ionvdb->rangeIndex == 0) {
        sdr_exit_xn(sdr);
        return -1;
    }

    now = time(NULL);

    for (elt = sm_rbt_first(ionwm, ionvdb->rangeIndex); elt;
            elt = sm_rbt_next(ionwm, elt)) {
        if (count >= maxRecords) {
            dtnex_log("Range snapshot full (%d): the remaining ranges "
                    "will not be announced", maxRecords);
            break;
        }

        addr = sm_rbt_data(ionwm, elt);
        if (addr == 0) {
            continue;
        }

        range = (IonRXref *) psp(ionwm, addr);
        if (range == NULL) {
            continue;
        }

        /* Authority rule (§4): we announce our own direction only. Each
         * endpoint announces the ranges it asserted, and the receiver's ION
         * imputes the reverse by itself; the network stays consistent thanks
         * to the asserted-only filter below, not in spite of it. */
        if ((unsigned long) range->fromNode != myNodeId) {
            continue;
        }

        if (range->fromNode == range->toNode) {
            continue;
        }

        /* Asserted ranges only. When a canonical range is asserted, ION
         * automatically creates the reverse one with rangeElt == 0, which its
         * own source comments as "imputed" (rfx.c:2490) and deleteRange uses
         * to tell the two apart (rfx.c:2765). A range we LEARNED from the
         * network gets such a reverse too, and when the learnt range ends at
         * the local node that reverse has fromNode == the local node: with no
         * filter we would re-announce something ION merely inferred as if we
         * were its authoritative source. Worse, for the receiver that
         * record has fromNode > toNode, which ION reads as a non-canonical
         * assertion, i.e. an explicit override of OWLT symmetry
         * (rfx.c:2449-2457): inserting it deletes the imputed range and
         * replaces it with an asserted one (rfx.c:2607), which removing the
         * canonical range later does not clean up. Same principle as
         * ionc_get_own_contacts keeping only CtScheduled/CtPredicted: we
         * announce what we asserted, not what ION derived. */
        if (range->rangeElt == 0) {
            continue;
        }

        if (range->toTime <= now) {
            continue;
        }

        out[count].fromNode = (unsigned long) range->fromNode;
        out[count].toNode = (unsigned long) range->toNode;
        out[count].fromTime = range->fromTime;
        out[count].toTime = range->toTime;
        out[count].owlt = range->owlt;
        count++;
    }

    sdr_exit_xn(sdr);
    return count;
}

/*
 * ---------------------------------------------------------------------------
 * Classifying the outcomes of the rfx_* calls
 *
 * Contract documented in rfx.h:70-73 and confirmed in ici/library/rfx.c:
 *   0  = success
 *   -1 = system error (a genuine anomaly: ION is broken or unreachable)
 *   >0 = user error, i.e. ION refused the write because of a local
 *        condition it is able to describe.
 *
 * A user error is NOT an anomaly. By far the most frequent case is code 9 of
 * rfx_insert_contact: in a conventional ionrc both nodes declare both
 * directions using relative times, so the contact a peer announces to us
 * overlaps the one the operator already configured locally, but with a
 * different fromTime. findContact looks up an exact fromTime and cannot see
 * it. The overlap is the expected steady state, not a fault: it is logged at
 * debug level and we carry on.
 * ---------------------------------------------------------------------------
 */

/* Meaning of the > 0 codes of rfx_insert_contact (rfx.c:1411-1735). */
static const char *insertContactUserError(int rc)
{
    switch (rc) {
    case 1: return "region 0 not allowed";
    case 2: return "fromNode 0 not allowed";
    case 3: return "toNode 0 not allowed";
    case 4: return "confidence out of range, refused by ION";
    case 5: return "zero xmitRate refused by ION";
    case 6: return "toTime earlier than fromTime";
    case 7: return "contact is for a foreign region: not one of this node's own regions";
    case 8: return "the corresponding hypothetical contact is already discovered";
    case 9: return "overlaps a locally configured contact; "
                   "the local one is kept";
    default: return "uncatalogued local condition";
    }
}

/* Meaning of the > 0 codes of rfx_revise_contact (rfx.c:1758-1866). */
static const char *reviseContactUserError(int rc)
{
    switch (rc) {
    case 1: return "the target contact no longer exists";
    case 2: return "the target contact is not Scheduled";
    default: return "uncatalogued local condition";
    }
}

/* Meaning of the > 0 codes of rfx_insert_range (rfx.c:2539-2685).
 * Code 1 never reaches this function: ION documents it as idempotent and the
 * caller treats it as success. */
static const char *insertRangeUserError(int rc)
{
    switch (rc) {
    case 1: return "range already asserted with the same owlt (idempotent)";
    case 2: return "different owlt on an already asserted range: ION will not revise it";
    case 3: return "overlaps the end of an existing range";
    case 4: return "overlaps the start of an existing range";
    default: return "uncatalogued local condition";
    }
}

/* rfx_remove_contact and rfx_remove_range currently return only 0 or -1; the
 * > 0 branch exists so as not to depend on that implementation detail. */
static const char *removeUserError(int rc)
{
    (void) rc;
    return "uncatalogued local condition";
}

/**
 * User-error branch: debug log carrying the meaning of the code.
 * This is not an error and does not interrupt the sequence of writes.
 */
static void noteUserError(int debugMode, const char *op, const ContactRecord *rec,
        int rc, const char *meaning)
{
    if (!debugMode) {
        return;
    }

    dtnex_log("[ion] %s %lu→%lu (from %ld): ION refused with code %d — %s",
            op, rec->fromNode, rec->toNode, (long) rec->fromTime, rc, meaning);
}

/* Same as noteUserError, for ranges. The logged fields are the same three, but
 * the record type is not: contacts and ranges are distinct entities in ION,
 * with different keys, APIs and scopes. Merging the two into one function would
 * mean either a void * or a conditional on the record type, which is worse than
 * the parallel pair. */
static void noteUserErrorRange(int debugMode, const char *op,
        const RangeRecord *rec, int rc, const char *meaning)
{
    if (!debugMode) {
        return;
    }

    dtnex_log("[ion] %s %lu→%lu (from %ld): ION refused with code %d — %s",
            op, rec->fromNode, rec->toNode, (long) rec->fromTime, rc, meaning);
}

/**
 * Cross-check on the returned address: rfx.h:62-63 documents *cxaddr /
 * *rxaddr left at 0 as confirmation of the refusal.
 *
 * In ION's source that confirmation holds for every code except those emitted
 * by the conflict scan — rfx_insert_contact 8 and 9, rfx_insert_range 1 and 2
 * — where the address left behind is that of the conflicting entry, so a
 * non-zero value is expected there. Outside those cases a divergence means
 * the installed library has a different contract from the documented one,
 * and that deserves to be said.
 */
static void checkRejectAddr(int debugMode, const char *op, int rc,
        PsmAddress addr, int addrExpectedSet)
{
    if (!debugMode || addrExpectedSet) {
        return;
    }

    if (addr != 0) {
        dtnex_log("[ion] %s: refused with code %d but the returned address is "
                "non-zero (0x%lx) — the contract of the installed library "
                "differs from rfx.h", op, rc, (unsigned long) addr);
    }
}

/**
 * The [ion] log line carrying the state reached so far. Called both on the
 * normal completion of ionc_apply_contact and on the error branches, so that
 * the outcome reached before an early return stays visible under debug
 * instead of disappearing.
 */
static void logApplyOutcome(int debugMode, const ContactRecord *rec,
        IoncApplyOutcome contactOutcome)
{
    if (!debugMode) {
        return;
    }

    dtnex_log("[ion] %lu→%lu from=%ld to=%ld: contact=%s",
            rec->fromNode, rec->toNode, (long) rec->fromTime,
            (long) rec->toTime, ionc_outcome_name(contactOutcome));
}

/* The same line for the range side of ionc_apply_range. Kept parallel to
 * logApplyOutcome for the same reason as noteUserErrorRange: the record type
 * differs even where the logged fields do not. */
static void logApplyRangeOutcome(int debugMode, const RangeRecord *rec,
        IoncApplyOutcome rangeOutcome)
{
    if (!debugMode) {
        return;
    }

    dtnex_log("[ion] %lu→%lu from=%ld to=%ld: range=%s",
            rec->fromNode, rec->toNode, (long) rec->fromTime,
            (long) rec->toTime, ionc_outcome_name(rangeOutcome));
}

IoncApplyOutcome ionc_apply_contact(const ContactRecord *rec, int debugMode)
{
    Sdr              sdr;
    IonVdb          *ionvdb;
    PsmPartition     ionwm;
    IonCXref         existingContact;
    int              haveContact;
    PsmAddress       cxaddr = 0;
    time_t           key;
    float            confidence;
    int              rc;
    IoncApplyOutcome contactOutcome = IONC_NOOP;

    /* The region comes from the message. If it is not one of the local
     * node's own regions, ION refuses with user error 7 ("contact is for a
     * foreign region"), which noteUserError logs in debug like every other
     * user error: the decision about a region is ION's, not dtnex's. */

    if (rec == NULL) {
        return IONC_ERROR;
    }

    confidence = rec->confidence / 100.0f;

    sdr = getIonsdr();
    if (sdr == NULL) {
        return IONC_ERROR;
    }

    /* Phase 1: existence check, inside a transaction (§6.4). */
    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return IONC_ERROR;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL) {
        sdr_exit_xn(sdr);
        return IONC_ERROR;
    }

    haveContact = findContact(ionwm, ionvdb, (uint32_t) rec->regionNbr,
            (uvast) rec->fromNode, (uvast) rec->toNode, rec->fromTime,
            &existingContact);

    sdr_exit_xn(sdr);

    /* Phase 2: writes. The rfx_* calls open their own transaction, so they
     * must be called with no transaction open. */

    if (!haveContact) {
        rc = rfx_insert_contact(rec->regionNbr, rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, &cxaddr, 0);
        if (rc < 0) {
            dtnex_log("⚠️  Anomaly: rfx_insert_contact %lu→%lu (from %ld) "
                    "returned %d", rec->fromNode, rec->toNode,
                    (long) rec->fromTime, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome);
            return IONC_ERROR;
        } else if (rc > 0) {
            noteUserError(debugMode, "rfx_insert_contact", rec, rc,
                    insertContactUserError(rc));
            checkRejectAddr(debugMode, "rfx_insert_contact", rc, cxaddr,
                    (rc == 8 || rc == 9));
            /* Nothing written: the contact phase stays a no-op. */
        } else {
            contactOutcome = IONC_INSERTED;
        }
    } else if (existingContact.toTime != rec->toTime) {
        /* Window changed: TARGETED removal by exact fromTime. */
        key = rec->fromTime;
        rc = rfx_remove_contact(rec->regionNbr, &key,
                (uvast) rec->fromNode, (uvast) rec->toNode, 0);
        if (rc < 0) {
            dtnex_log("⚠️  Anomaly: rfx_remove_contact %lu→%lu (from %ld) "
                    "returned %d", rec->fromNode, rec->toNode,
                    (long) rec->fromTime, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome);
            return IONC_ERROR;
        } else if (rc > 0) {
            /* The removal did not happen: re-inserting now would find the old
             * entry and be refused. ION is left as it is. */
            noteUserError(debugMode, "rfx_remove_contact", rec, rc,
                    removeUserError(rc));
        } else {
            rc = rfx_insert_contact(rec->regionNbr, rec->fromTime,
                    rec->toTime, (uvast) rec->fromNode, (uvast) rec->toNode,
                    (size_t) rec->xmitRate, confidence, &cxaddr, 0);
            if (rc < 0) {
                dtnex_log("⚠️  Anomaly: rfx_insert_contact (after remove) %lu→%lu "
                        "returned %d", rec->fromNode, rec->toNode, rc);
                contactOutcome = IONC_ERROR;
                logApplyOutcome(debugMode, rec, contactOutcome);
                return IONC_ERROR;
            } else if (rc > 0) {
                /* The remove succeeded but the insert was refused: the old
                 * entry is gone from ION and was not replaced. Unlike a plain
                 * refused insert, here ION got worse: this is always logged,
                 * not only under debug. */
                dtnex_log("⚠️  Anomaly: rfx_insert_contact (after remove) "
                        "%lu→%lu (from %ld) refused with code %d — %s: "
                        "the previous contact was removed and not "
                        "replaced",
                        rec->fromNode, rec->toNode, (long) rec->fromTime, rc,
                        insertContactUserError(rc));
                checkRejectAddr(debugMode, "rfx_insert_contact (after remove)",
                        rc, cxaddr, (rc == 8 || rc == 9));
                contactOutcome = IONC_LOST;
            } else {
                contactOutcome = IONC_REPLACED;
            }
        }
    } else if ((unsigned long) existingContact.xmitRate != rec->xmitRate
            || existingContact.confidence < confidence - 0.005f
            || existingContact.confidence > confidence + 0.005f) {
        /* Only xmitRate/confidence differ: revise in place. */
        rc = rfx_revise_contact(rec->regionNbr, rec->fromTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, 0);
        if (rc < 0) {
            dtnex_log("⚠️  Anomaly: rfx_revise_contact %lu→%lu returned %d",
                    rec->fromNode, rec->toNode, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome);
            return IONC_ERROR;
        } else if (rc > 0) {
            noteUserError(debugMode, "rfx_revise_contact", rec, rc,
                    reviseContactUserError(rc));
        } else {
            contactOutcome = IONC_REVISED;
        }
    }

    logApplyOutcome(debugMode, rec, contactOutcome);

    return contactOutcome;
}

/**
 * What the insertion is going over. It is not decoration: it decides how a
 * refusal has to be read, that is whether ION is left as it was or short of
 * something it used to hold.
 *
 * INSERT_OVER_IMPUTED is the delicate one. rfx_insert_range deletes the
 * imputed entry and then carries on into the overlap scan (rfx.c:2655), so a
 * refusal raised there arrives *after* the deletion.
 */
typedef enum {
    INSERT_OVER_NOTHING = 0,    /* no previous entry: a refusal changes nothing */
    INSERT_OVER_IMPUTED,        /* ION drops the imputed entry, then may still
                                 * refuse */
    INSERT_AFTER_REMOVE         /* we removed the previous entry ourselves */
} InsertRangeSite;

/**
 * The rfx_insert_range call and the reading of its return code, shared by the
 * three sites that perform it.
 *
 * `okOutcome` is what to report on success: inserting over nothing is an
 * insertion, inserting over an entry that was already there is a replacement.
 *
 * `site` says whether a refusal leaves ION short of an entry it held. Where it
 * does, the outcome is IONC_LOST and the refusal is logged unconditionally,
 * not only under debug, because ION has lost something. Where it does not,
 * nothing was written, so the outcome is a no-op and the note belongs to the
 * debug log.
 *
 * Returns the outcome reached, IONC_ERROR included: the caller records it and
 * stops, the log line having already been written here.
 */
static IoncApplyOutcome insertRange(const RangeRecord *rec, const char *op,
        IoncApplyOutcome okOutcome, InsertRangeSite site, int debugMode)
{
    PsmAddress  rxaddr = 0;
    int         rc;
    int         lost;

    rc = rfx_insert_range(rec->fromTime, rec->toTime, (uvast) rec->fromNode,
            (uvast) rec->toNode, rec->owlt, &rxaddr, 0);
    if (rc < 0) {
        dtnex_log("⚠️  Anomaly: %s %lu→%lu returned %d", op, rec->fromNode,
                rec->toNode, rc);
        return IONC_ERROR;
    }

    if (rc == 0) {
        return okOutcome;
    }

    /* ION explicitly documents this case as idempotent: the range is already
     * there with the same owlt. It is a success, not a refusal. */
    if (rc == 1) {
        return IONC_NOOP;
    }

    /* Code 2 leaves ION untouched: like code 1 it is raised by the asserted
     * branch (rfx.c:2637-2651), which returns before touching anything. From
     * code 3 up the refusal comes from the overlap scan instead, and that is
     * the one the imputed branch reaches only after having deleted the
     * entry. */
    lost = (site == INSERT_AFTER_REMOVE)
            || (site == INSERT_OVER_IMPUTED && rc >= 3);

    if (lost) {
        dtnex_log("⚠️  Anomaly: %s %lu→%lu (from %ld) refused with code %d — "
                "%s: the previous range was removed and not replaced",
                op, rec->fromNode, rec->toNode, (long) rec->fromTime, rc,
                insertRangeUserError(rc));
    } else {
        noteUserErrorRange(debugMode, op, rec, rc, insertRangeUserError(rc));
    }

    checkRejectAddr(debugMode, op, rc, rxaddr, (rc == 1 || rc == 2));

    return lost ? IONC_LOST : IONC_NOOP;
}

IoncApplyOutcome ionc_apply_range(const RangeRecord *rec, int debugMode)
{
    Sdr              sdr;
    IonVdb          *ionvdb;
    PsmPartition     ionwm;
    IonRXref         existingRange;
    int              haveRange;
    time_t           key;
    int              rc;
    IoncApplyOutcome rangeOutcome = IONC_NOOP;

    if (rec == NULL) {
        return IONC_ERROR;
    }

    sdr = getIonsdr();
    if (sdr == NULL) {
        return IONC_ERROR;
    }

    /* Phase 1: existence check, inside a transaction (§6.4). */
    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return IONC_ERROR;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL) {
        sdr_exit_xn(sdr);
        return IONC_ERROR;
    }

    haveRange = findRange(ionwm, ionvdb, (uvast) rec->fromNode,
            (uvast) rec->toNode, rec->fromTime, &existingRange);

    sdr_exit_xn(sdr);

    /* Phase 2: writes. The rfx_* calls open their own transaction, so they
     * must be called with no transaction open. */

    /* Same structure as the contact, but rfx_revise_range does not exist
     * (§6.3): any difference is a remove + insert, except over an imputed
     * range, where rfx_insert_range performs the swap on its own. */
    if (!haveRange) {
        rangeOutcome = insertRange(rec, "rfx_insert_range", IONC_INSERTED,
                INSERT_OVER_NOTHING, debugMode);
    } else if (existingRange.owlt != rec->owlt
            || existingRange.toTime != rec->toTime) {
        if (existingRange.rangeElt == 0) {
            /* Imputed range: ION deduced it on its own from the canonical
             * reverse assertion (rfx.c:2478-2496), so there is no IonRange
             * object behind it, only an index entry. rfx_insert_range replaces
             * it by itself, in a single transaction (rfx.c:2604-2620).
             *
             * Removing it first would be worse than redundant. It would split
             * the swap across two transactions, leaving the pair without a
             * current OWLT in between, and it would widen the exposure to the
             * overlap scan: sm_rbt_search zeroes the successor when it finds
             * the key (smrbt.c), so over an existing key the code-3 check is
             * skipped, while an insert after a removal goes through it in
             * full. Code 4 stays reachable either way — with no successor the
             * scan falls back on sm_rbt_last over the whole index
             * (rfx.c:2671-2674) — and ION raises it after having deleted the
             * imputed entry, which is why this site reports IONC_LOST there.
             *
             * Codes 1 and 2 come from the asserted branch of rfx_insert_range,
             * which the rangeElt check has just ruled out: seeing them here
             * means another writer asserted this range between our lookup and
             * the call. */
            rangeOutcome = insertRange(rec, "rfx_insert_range (over imputed)",
                    IONC_REPLACED, INSERT_OVER_IMPUTED, debugMode);
        } else {
            /* Asserted range whose window or owlt changed: TARGETED removal by
             * exact fromTime, never NULL. */
            key = rec->fromTime;
            rc = rfx_remove_range(&key, (uvast) rec->fromNode,
                    (uvast) rec->toNode, 0);
            if (rc < 0) {
                dtnex_log("⚠️  Anomaly: rfx_remove_range %lu→%lu returned %d",
                        rec->fromNode, rec->toNode, rc);
                rangeOutcome = IONC_ERROR;
            } else if (rc > 0) {
                /* Without the removal, re-insertion would be refused: ION is
                 * left as it is. */
                noteUserErrorRange(debugMode, "rfx_remove_range", rec, rc,
                        removeUserError(rc));
            } else {
                rangeOutcome = insertRange(rec,
                        "rfx_insert_range (after remove)", IONC_REPLACED,
                        INSERT_AFTER_REMOVE, debugMode);
            }
        }
    }

    logApplyRangeOutcome(debugMode, rec, rangeOutcome);

    return rangeOutcome;
}

int ionc_print_contact_table(int debugMode)
{
    Sdr           sdr;
    IonVdb       *ionvdb;
    PsmPartition  ionwm;
    PsmAddress    elt;
    PsmAddress    addr;
    IonCXref     *contact;
    time_t        now;
    int           count = 0;

    sdr = getIonsdr();
    if (sdr == NULL) {
        return -1;
    }

    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return -1;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL) {
        sdr_exit_xn(sdr);
        return -1;
    }

    if (ionvdb->contactIndex == 0) {
        sdr_exit_xn(sdr);
        return 0;
    }

    now = time(NULL);

    if (debugMode) {
        dtnex_log("\033[36m%-12s %-12s %-20s %-20s %-15s %-12s\033[0m",
                "FROM NODE", "TO NODE", "START TIME", "END TIME",
                "DURATION", "STATUS");
        dtnex_log("\033[36m--------------------------------------------------"
                "---------------------\033[0m");
    }

    for (elt = sm_rbt_first(ionwm, ionvdb->contactIndex); elt;
            elt = sm_rbt_next(ionwm, elt)) {
        addr = sm_rbt_data(ionwm, elt);
        if (addr == 0) {
            continue;
        }

        contact = (IonCXref *) psp(ionwm, addr);
        if (contact == NULL) {
            continue;
        }

        count++;

        if (debugMode) {
            time_t      timediff = contact->toTime - now;
            char        durationStr[32];
            char        startTimeStr[25];
            char        endTimeStr[25];
            struct tm  *timeinfo;
            const char *status;

            if (timediff > 86400) {
                snprintf(durationStr, sizeof(durationStr), "%.1f days",
                        timediff / 86400.0);
            } else if (timediff > 3600) {
                snprintf(durationStr, sizeof(durationStr), "%.1f hours",
                        timediff / 3600.0);
            } else if (timediff > 60) {
                snprintf(durationStr, sizeof(durationStr), "%.1f minutes",
                        timediff / 60.0);
            } else {
                snprintf(durationStr, sizeof(durationStr), "%ld seconds",
                        (long) timediff);
            }

            timeinfo = localtime(&contact->fromTime);
            strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S",
                    timeinfo);
            timeinfo = localtime(&contact->toTime);
            strftime(endTimeStr, sizeof(endTimeStr), "%Y-%m-%d %H:%M:%S",
                    timeinfo);

            status = (contact->fromTime <= now && now <= contact->toTime)
                    ? "\033[32mACTIVE\033[0m" : "\033[33mFUTURE\033[0m";

            dtnex_log("%-12lu %-12lu %-20s %-20s %-15s %s",
                    (unsigned long) contact->fromNode,
                    (unsigned long) contact->toNode,
                    startTimeStr, endTimeStr, durationStr, status);
        }
    }

    sdr_exit_xn(sdr);

    if (debugMode) {
        dtnex_log("\033[36m--------------------------------------------------"
                "---------------------\033[0m");
        dtnex_log("Total contacts: %d", count);
    }

    return count;
}

int ionc_check_alive(unsigned long expectedNodeId)
{
    Sdr      sdr;
    Object   iondbObject;
    uvast    ownNodeNbr;

    sdr = getIonsdr();
    if (sdr == NULL) {
        return -1;
    }

    /* sdr_begin_xn returns 1 on success and 0 on failure: a comparison
     * against < 0 would never fire. */
    if (sdr_begin_xn(sdr) != 1) {
        return -1;
    }

    iondbObject = getIonDbObject();
    if (iondbObject == 0) {
        sdr_exit_xn(sdr);
        return -1;
    }

    /* Only the ownNodeNbr field is read, not the whole IonDB: the
     * sizeof(IonDB) of the bundled headers does not match that of the
     * installed ION library, so reading the entire struct would over-read
     * past the end of the object in SDR. ownNodeNbr is the first field of the
     * struct, so its offset is the same in both layouts. */
    sdr_read(sdr, (char *) &ownNodeNbr, iondbObject + offsetof(IonDB, ownNodeNbr), sizeof(ownNodeNbr));
    sdr_exit_xn(sdr);

    if (ownNodeNbr == 0) {
        return 0;
    }

    return ((unsigned long) ownNodeNbr == expectedNodeId) ? 1 : 0;
}
