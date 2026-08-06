/**
 * ion_contacts.h
 * DTNEX - accesso a ION per contatti e range.
 *
 * Confine del modulo (design §8.1): questo modulo parla SOLO con ION.
 * Non conosce CBOR, bundle, HMAC, vicini o flooding. Se l'encoding
 * finisce qui dentro, il modulo smette di essere verificabile da solo
 * e si perde l'unica ragione per cui esiste.
 */

#ifndef ION_CONTACTS_H
#define ION_CONTACTS_H

#include <time.h>

/* Region in cui vengono inseriti i contatti ricevuti dalla rete.
 * dtnex e' mono-region per scelta di design (§5.3): regionNbr non viaggia
 * sul filo, il ricevente usa sempre la propria region di default. */
#define IONC_DEFAULT_REGION 1

/* Dimensione massima di uno snapshot di contatti annunciabili. */
#define IONC_MAX_CONTACTS 200

/**
 * Un contatto con il suo range, nelle unita' di ION:
 *   fromTime/toTime : epoch UNIX assoluti
 *   xmitRate        : byte al secondo
 *   confidence      : percentuale 0-100 (ION usa un float 0.0-1.0;
 *                     cbor.h non sa codificare i float, §5.3)
 *   owlt            : secondi
 */
typedef struct {
    unsigned long fromNode;
    unsigned long toNode;
    time_t        fromTime;
    time_t        toTime;
    unsigned long xmitRate;
    unsigned int  confidence;
    unsigned int  owlt;
} ContactRecord;

/**
 * Legge da ION i contatti annunciabili dal nodo locale.
 *
 * Filtri applicati (§3.2, §3.4):
 *   - fromNode == myNodeId          (regola di autorita', §4)
 *   - toNode != fromNode            (esclude i contatti di registrazione)
 *   - type in {CtScheduled, CtPredicted}
 *   - toTime > adesso               (i contatti scaduti non si annunciano)
 *   - esiste un range corrispondente da cui ricavare l'owlt
 *
 * Ritorna il numero di record scritti in out, oppure -1 se ION non e'
 * accessibile (SDR, vdb o working memory non disponibili).
 */
int ionc_get_own_contacts(unsigned long myNodeId, ContactRecord *out,
        int maxRecords, int debugMode);

/**
 * Esito dell'applicazione di un contatto ricevuto. Ordine di severita'
 * crescente: se contatto e range danno esiti diversi si riporta il piu' alto.
 */
typedef enum {
    IONC_NOOP = 0,      /* ION era gia' allineato: nessuna scrittura */
    IONC_REVISED,       /* xmitRate/confidence aggiornati in place */
    IONC_INSERTED,      /* contatto e/o range nuovi */
    IONC_REPLACED,      /* finestra cambiata: remove mirato + insert */
    IONC_ERROR          /* fallimento di una rfx_*: stato di ION inatteso */
} IoncApplyOutcome;

/**
 * Applica in ION il contatto ricevuto e il suo range, con identita'
 * (regione locale, fromNode, toNode, fromTime) (§6.2-6.3).
 *
 * Idempotente: se ION contiene gia' esattamente questo contatto non viene
 * eseguita nessuna scrittura. Le rimozioni passano SEMPRE il puntatore al
 * fromTime esatto, mai NULL: con NULL ION applica lo scope '*' e cancella
 * tutti i contatti della coppia, inclusi quelli configurati dall'operatore.
 */
IoncApplyOutcome ionc_apply_contact(const ContactRecord *rec, int debugMode);

/* Nome leggibile dell'esito, per i log. */
const char *ionc_outcome_name(IoncApplyOutcome outcome);

/**
 * Stampa la tabella diagnostica dei contatti presenti in ION (tutti, non
 * solo i nostri). Ritorna il numero di contatti, -1 se ION non e'
 * accessibile. La stampa dettagliata avviene solo con debugMode != 0.
 */
int ionc_print_contact_table(int debugMode);

/**
 * Verifica che ION sia vivo e che sia ancora la stessa istanza: legge
 * ownNodeNbr dall'IonDB e lo confronta con quello atteso.
 *
 * Ritorna 1 se ION e' vivo e coerente, 0 se e' ripartito o riconfigurato
 * con un altro node number, -1 se non e' accessibile (§6.6).
 *
 * NON usare "zero contatti" come indizio di restart: un nodo appena
 * avviato o di bordo ha legittimamente zero contatti.
 */
int ionc_check_alive(unsigned long expectedNodeId);

#endif /* ION_CONTACTS_H */
