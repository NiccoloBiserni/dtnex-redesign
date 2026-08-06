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
