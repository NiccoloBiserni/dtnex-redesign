/**
 * ion_contacts.c
 * DTNEX - accesso a ION per contatti e range.
 */

#include "include/ion/ion.h"
#include "include/ion/rfx.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ion_contacts.h"

/* Il logger vive in dtnex.c. Lo dichiariamo qui invece di includere
 * dtnex.h per non trascinare dentro il modulo bundle, config e CBOR. */
extern void dtnex_log(const char *format, ...);

/**
 * Cerca nel rangeIndex di ION un range fra fromNode e toNode la cui
 * finestra si sovrappone a [fromTime, toTime]. In ION l'OWLT e' una
 * proprieta' geometrica simmetrica, quindi se non troviamo il verso
 * richiesto proviamo quello opposto.
 *
 * Va chiamata con una transazione SDR gia' aperta.
 * Ritorna 1 e scrive *owlt se trova un range, 0 altrimenti.
 */
static int findOwlt(PsmPartition ionwm, IonVdb *ionvdb, uvast fromNode,
        uvast toNode, time_t fromTime, time_t toTime, unsigned int *owlt)
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

        if (!((range->fromNode == fromNode && range->toNode == toNode)
                || (range->fromNode == toNode && range->toNode == fromNode))) {
            continue;
        }

        /* Finestre che si sovrappongono */
        if (range->fromTime > toTime || range->toTime < fromTime) {
            continue;
        }

        *owlt = range->owlt;
        return 1;
    }

    return 0;
}

/**
 * Cerca il contatto con chiave esatta (fromNode, toNode, fromTime).
 * Va chiamata con una transazione SDR gia' aperta.
 * Ritorna 1 e copia il contatto in *copy, oppure 0 se non esiste.
 */
static int findContact(PsmPartition ionwm, IonVdb *ionvdb, uvast fromNode,
        uvast toNode, time_t fromTime, IonCXref *copy)
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

        if (contact->fromNode == fromNode && contact->toNode == toNode
                && contact->fromTime == fromTime) {
            memcpy(copy, contact, sizeof(IonCXref));
            return 1;
        }
    }

    return 0;
}

/**
 * Cerca il range con chiave esatta (fromNode, toNode, fromTime).
 * Va chiamata con una transazione SDR gia' aperta.
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

    if (sdr_begin_xn(sdr) < 0) {
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
        unsigned int owlt = 0;

        if (count >= maxRecords) {
            dtnex_log("Snapshot dei contatti pieno (%d): i restanti non "
                    "verranno annunciati", maxRecords);
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

        /* Regola di autorita' (§4): annunciamo solo la nostra direzione. */
        if ((unsigned long) contact->fromNode != myNodeId) {
            continue;
        }

        /* I contatti di registrazione (fromNode == toNode) non sono
         * topologia: non vanno annunciati. */
        if (contact->fromNode == contact->toNode) {
            continue;
        }

        /* Scheduled e Predicted sono le uniche finestre reali. In
         * particolare i Discovered hanno toTime = MAX_POSIX_TIME e
         * annunciarli propagherebbe un contatto eterno (§6.1 controllo 6). */
        if (contact->type != CtScheduled && contact->type != CtPredicted) {
            continue;
        }

        if (contact->toTime <= now) {
            continue;
        }

        /* §3.4: senza range CGR scarta il contatto come next-hop, quindi
         * annunciarlo genererebbe churn senza mai produrre una rotta. */
        if (!findOwlt(ionwm, ionvdb, contact->fromNode, contact->toNode,
                contact->fromTime, contact->toTime, &owlt)) {
            if (debugMode) {
                dtnex_log("Contatto %lu→%lu (from %ld) senza range: non "
                        "annunciato — controllare ionrc",
                        (unsigned long) contact->fromNode,
                        (unsigned long) contact->toNode,
                        (long) contact->fromTime);
            }
            continue;
        }

        out[count].fromNode = (unsigned long) contact->fromNode;
        out[count].toNode = (unsigned long) contact->toNode;
        out[count].fromTime = contact->fromTime;
        out[count].toTime = contact->toTime;
        out[count].xmitRate = (unsigned long) contact->xmitRate;
        out[count].confidence =
                (unsigned int) (contact->confidence * 100.0f + 0.5f);
        out[count].owlt = owlt;
        count++;
    }

    sdr_exit_xn(sdr);
    return count;
}

/**
 * Riga di log [ion] con lo stato raggiunto fino a quel punto. Chiamata sia
 * al termine normale di ionc_apply_contact sia sui rami di errore, cosi'
 * uno stato parziale (es. contatto scritto, range fallito) resta visibile
 * in debug invece di sparire dietro un return anticipato.
 */
static void logApplyOutcome(int debugMode, const ContactRecord *rec,
        IoncApplyOutcome contactOutcome, IoncApplyOutcome rangeOutcome)
{
    if (!debugMode) {
        return;
    }

    dtnex_log("[ion] %lu→%lu from=%ld to=%ld: contatto=%s range=%s",
            rec->fromNode, rec->toNode, (long) rec->fromTime,
            (long) rec->toTime, ionc_outcome_name(contactOutcome),
            ionc_outcome_name(rangeOutcome));
}

IoncApplyOutcome ionc_apply_contact(const ContactRecord *rec, int debugMode)
{
    Sdr              sdr;
    IonVdb          *ionvdb;
    PsmPartition     ionwm;
    IonCXref         existingContact;
    IonRXref         existingRange;
    int              haveContact;
    int              haveRange;
    PsmAddress       cxaddr = 0;
    PsmAddress       rxaddr = 0;
    time_t           key;
    float            confidence;
    int              rc;
    IoncApplyOutcome contactOutcome = IONC_NOOP;
    IoncApplyOutcome rangeOutcome = IONC_NOOP;

    if (rec == NULL) {
        return IONC_ERROR;
    }

    confidence = rec->confidence / 100.0f;

    sdr = getIonsdr();
    if (sdr == NULL) {
        return IONC_ERROR;
    }

    /* Fase 1: controllo di esistenza, sotto transazione (§6.4). */
    if (sdr_begin_xn(sdr) < 0) {
        return IONC_ERROR;
    }

    ionvdb = getIonVdb();
    ionwm = getIonwm();
    if (ionvdb == NULL || ionwm == NULL) {
        sdr_exit_xn(sdr);
        return IONC_ERROR;
    }

    haveContact = findContact(ionwm, ionvdb, (uvast) rec->fromNode,
            (uvast) rec->toNode, rec->fromTime, &existingContact);
    haveRange = findRange(ionwm, ionvdb, (uvast) rec->fromNode,
            (uvast) rec->toNode, rec->fromTime, &existingRange);

    sdr_exit_xn(sdr);

    /* Fase 2: scritture. Le rfx_* aprono la propria transazione, quindi
     * vanno chiamate a transazione chiusa. */

    if (!haveContact) {
        rc = rfx_insert_contact(IONC_DEFAULT_REGION, rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, &cxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_contact %lu→%lu (from %ld) ha "
                    "restituito %d", rec->fromNode, rec->toNode,
                    (long) rec->fromTime, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }
        contactOutcome = IONC_INSERTED;
    } else if (existingContact.toTime != rec->toTime) {
        /* Finestra cambiata: rimozione MIRATA per fromTime esatto. */
        key = rec->fromTime;
        rc = rfx_remove_contact(IONC_DEFAULT_REGION, &key,
                (uvast) rec->fromNode, (uvast) rec->toNode, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_remove_contact %lu→%lu (from %ld) ha "
                    "restituito %d", rec->fromNode, rec->toNode,
                    (long) rec->fromTime, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }

        rc = rfx_insert_contact(IONC_DEFAULT_REGION, rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, &cxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_contact (dopo remove) %lu→%lu "
                    "ha restituito %d", rec->fromNode, rec->toNode, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }
        contactOutcome = IONC_REPLACED;
    } else if ((unsigned long) existingContact.xmitRate != rec->xmitRate
            || existingContact.confidence < confidence - 0.005f
            || existingContact.confidence > confidence + 0.005f) {
        /* Solo xmitRate/confidence: revisione in place. */
        rc = rfx_revise_contact(IONC_DEFAULT_REGION, rec->fromTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_revise_contact %lu→%lu ha restituito %d",
                    rec->fromNode, rec->toNode, rc);
            contactOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }
        contactOutcome = IONC_REVISED;
    }

    /* Range: stessa struttura, ma rfx_revise_range non esiste (§6.3). */
    if (!haveRange) {
        rc = rfx_insert_range(rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode, rec->owlt,
                &rxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_range %lu→%lu ha restituito %d",
                    rec->fromNode, rec->toNode, rc);
            rangeOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }
        rangeOutcome = IONC_INSERTED;
    } else if (existingRange.owlt != rec->owlt
            || existingRange.toTime != rec->toTime) {
        key = rec->fromTime;
        rc = rfx_remove_range(&key, (uvast) rec->fromNode,
                (uvast) rec->toNode, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_remove_range %lu→%lu ha restituito %d",
                    rec->fromNode, rec->toNode, rc);
            rangeOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }

        rc = rfx_insert_range(rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode, rec->owlt,
                &rxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_range (dopo remove) %lu→%lu ha "
                    "restituito %d", rec->fromNode, rec->toNode, rc);
            rangeOutcome = IONC_ERROR;
            logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);
            return IONC_ERROR;
        }
        rangeOutcome = IONC_REPLACED;
    }

    logApplyOutcome(debugMode, rec, contactOutcome, rangeOutcome);

    return (rangeOutcome > contactOutcome) ? rangeOutcome : contactOutcome;
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

    if (sdr_begin_xn(sdr) < 0) {
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
            char        durationStr[24];
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
    IonDB    iondb;

    sdr = getIonsdr();
    if (sdr == NULL) {
        return -1;
    }

    if (sdr_begin_xn(sdr) < 0) {
        return -1;
    }

    iondbObject = getIonDbObject();
    if (iondbObject == 0) {
        sdr_exit_xn(sdr);
        return -1;
    }

    sdr_read(sdr, (char *) &iondb, iondbObject, sizeof(IonDB));
    sdr_exit_xn(sdr);

    if (iondb.ownNodeNbr == 0) {
        return 0;
    }

    return ((unsigned long) iondb.ownNodeNbr == expectedNodeId) ? 1 : 0;
}
