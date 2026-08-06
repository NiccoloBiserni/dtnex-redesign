# DTNEX — Redesign dello scambio di contatti: piano di implementazione

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fare in modo che dtnex annunci esattamente i contatti che ION ha realmente configurati — con tempi assoluti, xmitRate, confidence e OWLT reali — e li scriva sui nodi remoti in modo idempotente e mirato, senza cancellare i contatti dell'operatore.

**Architecture:** Tutto l'accesso a ION per contatti e range viene estratto in un nuovo modulo `ion_contacts.{h,c}` che espone tre operazioni (leggi i contatti annunciabili, applica un contatto+range ricevuto, stampa la tabella diagnostica) e non conosce CBOR, bundle, HMAC, vicini o flooding. `dtnex.c` resta il livello protocollo: legge lo snapshot dal modulo, lo codifica in CBOR v3 con tempi assoluti, e in ricezione valida e delega la scrittura al modulo.

**Tech Stack:** C (C99, gcc), ION-DTN system libraries (`libbp`, `libici`), OpenSSL per HMAC, CBOR tramite `include/ion/cbor.h`. Nessun framework di test: la validazione è manuale in `--debug` contro un ION vivo.

**Spec di riferimento:** `docs/superpowers/specs/2026-08-03-dtnex-contact-exchange-redesign-design.md` (i riferimenti `§N` qui sotto puntano a quel documento).

## Global Constraints

- **Versione protocollo:** `DTNEX_PROTOCOL_VERSION` passa da 2 a 3. I messaggi v2 vengono scartati, non tradotti (§5.1).
- **Unità di misura ION:** `xmitRate` in **byte/secondo**, `owlt` in **secondi**, tempi in epoch UNIX assoluti (`time_t`).
- **`confidence` sul filo:** intero 0-100 (percentuale). ION lo usa come `float` 0.0-1.0; conversione ai due estremi (§5.3).
- **Mono-region:** `regionNbr` non viaggia sul filo; il ricevente inserisce sempre in `IONC_DEFAULT_REGION` (1) (§5.3).
- **Regola di autorità:** si annunciano solo i contatti con `fromNode == config->nodeId`; si scartano i messaggi con `fromNode != origin` (§4).
- **Rimozione mirata:** ogni `rfx_remove_contact` / `rfx_remove_range` passa il puntatore al `fromTime` esatto, **mai `NULL`** (§6.2). `NULL` equivale allo scope `*` di ionadmin e cancella i contatti dell'operatore.
- **Confine del modulo:** `ion_contacts.c` non include `dtnex.h` e non conosce `DtnexConfig`, CBOR, bundle, vicini (§8.1). Per loggare dichiara `extern void dtnex_log(const char *format, ...);` e riceve `debugMode` come parametro `int`.
- **Transazioni SDR:** le camminate RBT vanno fatte dentro `sdr_begin_xn()` / `sdr_exit_xn()`; le `rfx_*` aprono la propria transazione, quindi **vanno chiamate fuori** dalla nostra.
- **Build:** dopo ogni task il progetto deve compilare con `./build_standalone.sh` senza **nuovi** warning. I warning preesistenti noti sono: `SHA256 deprecated`, `const qualifier discarded in bp_send`, `snprintf truncation in getContacts`.
- **Commit:** messaggi in italiano, nessun trailer `Co-Authored-By`.

---

## File Structure

| file | responsabilità |
|---|---|
| `ion_contacts.h` (nuovo) | Tipo `ContactRecord` (7 campi), enum `IoncApplyOutcome`, costanti, prototipi delle tre operazioni. Include solo `<time.h>`: nessun tipo ION nell'interfaccia. |
| `ion_contacts.c` (nuovo) | Camminate RBT su `contactIndex` e `rangeIndex`, join contatto↔range per l'OWLT, regole di scrittura idempotenti, tabella diagnostica, health-check di ION. |
| `dtnex.h` | Rimuove `ContactInfo`, include `ion_contacts.h`, bump versione protocollo, prototipi aggiornati a `ContactRecord`. |
| `dtnex.c` | Snapshot `myContacts` con cache TTL e rilevazione dei cambiamenti, origination, encode/decode CBOR v3, pipeline di validazione in ricezione, forwarding, mutex su `getplanlist`. |
| `Makefile`, `build_standalone.sh`, `build.sh` | Compilano e linkano anche `ion_contacts.c`. |
| `CLAUDE.md`, `README.md`, `dtnex.conf` | Documentazione: payload reale, unità di misura, nuova semantica di `contactLifetime`. |

---

## Ordine dei task

1. Modulo `ion_contacts`: lettura dello snapshot annunciabile (chiude §1.1 lato origination, §1.2 lato lettura)
2. Protocollo v3: `ContactRecord` end-to-end, encode/decode/forward, origination dallo snapshot (chiude §1.1 e §1.2 end-to-end)
3. Scrittura idempotente e mirata in ION + pipeline di validazione (chiude §1.3 e il churn)
4. Scorporo di `getContacts` e rilevazione esplicita del restart di ION (§6.6)
5. Mutex su `getplanlist` (§7.5)
6. Documentazione e bump di versione applicativa (§10, §7.2)

---

### Task 1: Modulo `ion_contacts` — lettura dei contatti annunciabili

Crea il modulo e la prima delle sue tre operazioni: leggere da ION i contatti di cui il nodo locale è `fromNode`, unendo a ciascuno l'OWLT preso dal range corrispondente. Al termine del task il binario stampa lo snapshot in `--debug` a ogni ciclo, il che rende verificabile la prova 1 di §9 senza scrivere una riga di protocollo.

**Files:**
- Create: `ion_contacts.h`
- Create: `ion_contacts.c`
- Modify: `dtnex.h` (include del nuovo header)
- Modify: `dtnex.c:1009-1215` (`getContacts`: aggiunta della stampa dello snapshot in coda alla tabella diagnostica esistente)
- Modify: `Makefile`, `build_standalone.sh`, `build.sh`

**Interfaces:**
- Consumes: niente (primo task).
- Produces:
  - `typedef struct { unsigned long fromNode; unsigned long toNode; time_t fromTime; time_t toTime; unsigned long xmitRate; unsigned int confidence; unsigned int owlt; } ContactRecord;`
  - `int ionc_get_own_contacts(unsigned long myNodeId, ContactRecord *out, int maxRecords, int debugMode);` → numero di record scritti, `-1` se ION non è accessibile.
  - `#define IONC_MAX_CONTACTS 200`, `#define IONC_DEFAULT_REGION 1`

- [ ] **Step 1: Creare `ion_contacts.h`**

```c
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

#endif /* ION_CONTACTS_H */
```

- [ ] **Step 2: Creare `ion_contacts.c` con la camminata RBT e il join dei range**

```c
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
```

- [ ] **Step 3: Includere il nuovo header in `dtnex.h`**

In `dtnex.h`, subito dopo `#include "include/ion/cbor.h"` (riga 27), aggiungere:

```c
// Accesso a ION per contatti e range (modulo separato, vedi ion_contacts.h)
#include "ion_contacts.h"
```

- [ ] **Step 4: Stampare lo snapshot in coda alla tabella diagnostica**

In `dtnex.c`, dentro `getContacts()`, subito prima del blocco `if (config->createGraph)` finale (attuale riga 1211), inserire:

```c
    // Snapshot dei contatti annunciabili: e' esattamente cio' che verra'
    // messo sul filo, quindi va confrontato con 'l contact' di ionadmin.
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
```

- [ ] **Step 5: Aggiornare i tre script di build**

In `Makefile` sostituire le righe delle sorgenti e la regola di compilazione:

```make
# Target and source files
TARGET = dtnex
SOURCES = dtnex.c ion_contacts.c
OBJECTS = dtnex.o ion_contacts.o

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LIBS)

# Compile source files
dtnex.o: dtnex.c dtnex.h ion_contacts.h
	$(CC) $(CFLAGS) $(INCLUDES) -c dtnex.c

ion_contacts.o: ion_contacts.c ion_contacts.h
	$(CC) $(CFLAGS) $(INCLUDES) -c ion_contacts.c
```

e la regola `clean` resta valida perché usa `$(OBJECTS)`.

In `build_standalone.sh` sostituire il blocco di clean/compile/link:

```bash
# Clean previous build
rm -f dtnex.o ion_contacts.o dtnex

# Compile with local headers
gcc -Wall -g -I. -c dtnex.c -o dtnex.o
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

gcc -Wall -g -I. -c ion_contacts.c -o ion_contacts.o
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

# Link with system ION libraries
gcc dtnex.o ion_contacts.o -o dtnex -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto
```

In `build.sh` (riga 23 la compilazione, riga 30 il link), sostituire:

```bash
gcc -Wall -g -I../ione-code/bpv7/include -I../ione-code/ici/include -c dtnex.c -o dtnex.o
```

con:

```bash
gcc -Wall -g -I../ione-code/bpv7/include -I../ione-code/ici/include -c dtnex.c -o dtnex.o
gcc -Wall -g -I../ione-code/bpv7/include -I../ione-code/ici/include -c ion_contacts.c -o ion_contacts.o
```

e:

```bash
gcc dtnex.o -o dtnex -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto
```

con:

```bash
gcc dtnex.o ion_contacts.o -o dtnex -L/usr/local/lib -lbp -lici -lm -lpthread -lcrypto
```

- [ ] **Step 6: Compilare**

Run: `./build_standalone.sh`
Expected: `✅ Build complete!`, nessun warning nuovo oltre a quelli preesistenti elencati nei Global Constraints.

- [ ] **Step 7: Verifica manuale contro ION (prova 1 di §9)**

Con ION avviato e almeno un contatto + range configurati in ionrc per il nodo locale:

```bash
echo 'l contact' | ionadmin
echo 'l range' | ionadmin
./dtnex --debug 2>&1 | grep -A 20 "Contatti annunciabili"
```

Expected: ogni contatto con `From <nodo locale>` presente in `l contact` (di tipo scheduled/predicted e non scaduto) compare nello snapshot con `from`/`to`/`xmitRate` identici, e con l'`owlt` del range corrispondente in `l range`. Un contatto locale privo di range **non** compare, e nel log si legge la riga "senza range: non annunciato".

- [ ] **Step 8: Commit**

```bash
git add ion_contacts.h ion_contacts.c dtnex.h dtnex.c Makefile build_standalone.sh build.sh
git commit -m "feat(ion): modulo ion_contacts con lettura dei contatti annunciabili

Legge dal contactIndex di ION i contatti con fromNode == nodo locale e
li unisce al range corrispondente per ricavare l'owlt. I contatti senza
range non vengono annunciati (CGR li scarterebbe comunque)."
```

---

### Task 2: Protocollo v3 — `ContactRecord` end-to-end e origination dallo snapshot

Sostituisce `ContactInfo` con `ContactRecord` ovunque, porta il payload da 3 a 7 campi con tempi assoluti, e fa sì che `exchangeWithNeighbors` annunci lo snapshot letto da ION invece di fabbricare i contatti dai plan. La ricezione viene adattata ai nuovi campi ma **conserva** temporaneamente la vecchia logica di scrittura in ION: la riscrive il Task 3.

**Files:**
- Modify: `dtnex.h:73` (`DTNEX_PROTOCOL_VERSION`), `dtnex.h:118-123` (rimozione di `ContactInfo`), `dtnex.h:190-215` (prototipi)
- Modify: `dtnex.c:674-849` (`exchangeWithNeighbors`)
- Modify: `dtnex.c:1998-2046` (`encodeCborContactMessage`)
- Modify: `dtnex.c:2851`, `dtnex.c:2890-2921`, `dtnex.c:3158-3168` (`decodeCborMessage`)
- Modify: `dtnex.c:3191-3308` (`processCborContactMessage`, adattamento minimo)
- Modify: `dtnex.c:3358-3420` (`forwardCborContactMessage`)

**Interfaces:**
- Consumes: `ContactRecord`, `ionc_get_own_contacts()`, `IONC_MAX_CONTACTS` dal Task 1.
- Produces:
  - `int encodeCborContactMessage(DtnexConfig *config, ContactRecord *contact, unsigned char *buffer, int bufferSize);`
  - `int processCborContactMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime, unsigned long origin, unsigned long from, ContactRecord *contact);`
  - `void forwardCborContactMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp, time_t expireTime, unsigned long origin, unsigned long from, ContactRecord *contact);`
  - In `dtnex.c`, statici del file: `static int refreshMyContacts(DtnexConfig *config, int *changed);` con lo snapshot in `static ContactRecord myContacts[IONC_MAX_CONTACTS];` e `static int myContactCount;`

- [ ] **Step 1: Bump della versione di protocollo e rimozione di `ContactInfo`**

In `dtnex.h` riga 73:

```c
#define DTNEX_PROTOCOL_VERSION 3
```

Rimuovere completamente il blocco righe 118-123:

```c
// Ultra-minimal contact information structure for CBOR messages
typedef struct {
    unsigned long nodeA;
    unsigned long nodeB;
    unsigned short duration;    // Duration in minutes (0-65535)
} ContactInfo;
```

(`ContactRecord`, definito in `ion_contacts.h`, prende il suo posto ovunque.)

Aggiornare i prototipi che la citavano, righe 190-215:

```c
int encodeCborContactMessage(DtnexConfig *config, ContactRecord *contact, unsigned char *buffer, int bufferSize);
```

```c
int processCborContactMessage(DtnexConfig *config, unsigned char *nonce, time_t timestamp, time_t expireTime, 
                             unsigned long origin, unsigned long from, ContactRecord *contact);
```

```c
void forwardCborContactMessage(DtnexConfig *config, unsigned char *originalNonce, time_t timestamp, 
                               time_t expireTime, unsigned long origin, unsigned long from, ContactRecord *contact);
```

- [ ] **Step 2: Nuovo payload in `encodeCborContactMessage`**

Sostituire integralmente il corpo di `encodeCborContactMessage` (`dtnex.c:1998-2046`):

```c
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
```

- [ ] **Step 3: Snapshot con cache TTL e rilevazione dei cambiamenti**

In `dtnex.c`, immediatamente **prima** di `void exchangeWithNeighbors(...)` (attuale riga 674), inserire:

```c
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
```

- [ ] **Step 4: Riscrivere il gate e il loop di origination di `exchangeWithNeighbors`**

Sostituire il corpo di `exchangeWithNeighbors` (`dtnex.c:674-849`) mantenendo intatta la sezione metadata. Il corpo nuovo, dalla dichiarazione delle variabili fino alla fine del loop dei contatti:

```c
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
    
    // [la sezione metadata resta identica: da
    //  "if (!config->noMetadataExchange && strlen(config->nodemetadata) > 0) {"
    //  fino alla chiusura del suo else-if, senza modifiche]
}
```

Attenzione a due dettagli nel riportare la sezione metadata: il gate temporale che prima la racchiudeva ora è un `return` anticipato, quindi il blocco metadata va **de-indentato di un livello** e la variabile `expireTime` (prima calcolata a riga 731 e mai usata dai metadata) va rimossa — questo elimina anche il warning preesistente `expireTime set but not used`.

- [ ] **Step 5: Estrarre 7 campi in `decodeCborMessage`**

In `decodeCborMessage`, sostituire la dichiarazione di riga 2851:

```c
    ContactRecord extractedContact = {0};
```

Sostituire il ramo `if (messageType[0] == 'c')` dell'estrazione (righe 2890-2921):

```c
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
```

E la riga di log prima della dispatch (righe 3165-3166):

```c
        debug_log(config, "🔍 Processing extracted contact data: %lu→%lu (from=%ld to=%ld)", 
                  extractedContact.fromNode, extractedContact.toNode,
                  (long) extractedContact.fromTime, (long) extractedContact.toTime);
```

Il controllo `if (version != DTNEX_PROTOCOL_VERSION)` a riga 2781 non va toccato: con `DTNEX_PROTOCOL_VERSION == 3` implementa già il controllo 1 di §6.1 (i v2 vengono scartati con log a debug).

- [ ] **Step 6: Adattare `processCborContactMessage` ai nuovi campi (logica di scrittura invariata)**

Questo step è un ponte: la logica di scrittura resta quella vecchia (verrà sostituita nel Task 3), ma smette di ricalcolare la finestra temporale in locale e usa i valori ricevuti. Nel corpo di `processCborContactMessage` (`dtnex.c:3191-3308`):

- sostituire la firma con `ContactRecord *contact`;
- sostituire ogni `contact->nodeA` con `contact->fromNode` e ogni `contact->nodeB` con `contact->toNode`;
- sostituire il calcolo dei tempi (righe 3203-3204) con:

```c
    time_t startTime = contact->fromTime;
    time_t endTime = contact->toTime;
```

- **eliminare** il blocco righe 3240-3244 che riscrive `startTime = currentTime` (è la seconda fonte di divergenza di §1.1);
- sostituire i valori hardcoded (righe 3224-3225, 3269) con quelli ricevuti:

```c
    size_t xmitRate = (size_t) contact->xmitRate;
    float confidence = contact->confidence / 100.0f;
```

```c
    unsigned int owlt = contact->owlt;
```

- adeguare la stringa di log `contactCmd` a usare `contact->fromNode` / `contact->toNode` e a stampare `xmitRate` invece del letterale `100000`.

Le chiamate `rfx_remove_*` con `NULL` restano per ora: le sostituisce il Task 3.

- [ ] **Step 7: Adeguare `forwardCborContactMessage`**

In `forwardCborContactMessage` (`dtnex.c:3358-3420`): firma con `ContactRecord *contact`, e sostituire il blocco del payload (righe 3403-3406) con:

```c
        // Contact data v3
        bytesWritten += cbor_encode_array_open(7, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.fromNode, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.toNode, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.fromTime, &cursor);
        bytesWritten += cbor_encode_integer((uvast) forwardContact.toTime, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.xmitRate, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.confidence, &cursor);
        bytesWritten += cbor_encode_integer(forwardContact.owlt, &cursor);
```

più la dichiarazione `ContactRecord forwardContact = *contact;` (riga 3386) e la chiamata a `log_message_forwarded` con `contact->fromNode, contact->toNode`.

- [ ] **Step 8: Chiamare l'exchange a ogni giro del main loop**

Perché il trigger 3 sia reattivo entro il TTL della cache e non entro `updateInterval` (§7.1), `exchangeWithNeighbors` va valutata a ogni risveglio del loop, non solo alla scadenza programmata. In `eventDrivenLoop` (`dtnex.c:2400-2403`), sostituire:

```c
        // Update contact info to ensure we have the latest topology (only if ION connected)
        if (ionConnected) {
            getContacts(config);
        }
```

con:

```c
        // A ogni risveglio (<= 60s): rivaluta i trigger di annuncio (§7.1).
        // exchangeWithNeighbors decide da sola se c'e' qualcosa da fare.
        if (ionConnected) {
            getplanlist(config, plans, &planCount);
            exchangeWithNeighbors(config, plans, planCount);
            getContacts(config);
        }
```

- [ ] **Step 9: Compilare**

Run: `./build_standalone.sh`
Expected: build pulita. Il warning preesistente `expireTime set but not used` è sparito (Step 4).

- [ ] **Step 10: Verifica manuale a due nodi (prove 2 e 3 di §9)**

Su A configurare in ionrc un contatto A→B con `xmitRate` e un range con OWLT riconoscibili (es. xmitRate 25000, owlt 7). Avviare dtnex su A e su B in `--debug`.

```bash
# su A
echo 'l contact' | ionadmin ; echo 'l range' | ionadmin
# su B, dopo aver ricevuto
echo 'l contact' | ionadmin ; echo 'l range' | ionadmin
```

Expected: su B il contatto A→B ha `fromTime` e `toTime` **identici** a quelli letti su A (nessuna riscrittura allo start time locale), `xmitRate` 25000 e OWLT 7 — non 1. Nel log di B compare il contatto decodificato con gli stessi valori.

- [ ] **Step 11: Commit**

```bash
git add dtnex.h dtnex.c
git commit -m "feat(proto): protocollo v3 con contatti direzionali e tempi assoluti

Il payload di contatto passa da 3 a 7 campi (fromNode, toNode, fromTime,
toTime, xmitRate, confidence, owlt) con tempi assoluti mai riscritti dal
ricevente. L'origination legge lo snapshot dei contatti da ION invece di
fabbricarlo dai plan e dalla config; expireTime dell'envelope e' il toTime
del contatto. I messaggi v2 vengono scartati."
```

---

### Task 3: Scrittura idempotente e mirata + pipeline di validazione

Chiude il problema 1.3 e il churn. Il modulo `ion_contacts` acquisisce la seconda operazione — applicare un contatto+range ricevuto secondo le regole di §6.2-6.3 — e `processCborContactMessage` diventa un validatore che delega.

**Files:**
- Modify: `ion_contacts.h` (enum `IoncApplyOutcome`, prototipi `ionc_apply_contact`, `ionc_outcome_name`)
- Modify: `ion_contacts.c` (helper di lookup, `ionc_apply_contact`)
- Modify: `dtnex.c:3191-3308` (`processCborContactMessage`: riscrittura completa)

**Interfaces:**
- Consumes: `ContactRecord`, `IONC_DEFAULT_REGION` (Task 1); `processCborContactMessage(..., ContactRecord *contact)` (Task 2).
- Produces:
  - `typedef enum { IONC_NOOP = 0, IONC_REVISED, IONC_INSERTED, IONC_REPLACED, IONC_ERROR } IoncApplyOutcome;` — l'ordine è severità crescente e viene usato dal confronto `>` che combina l'esito del contatto con quello del range.
  - `IoncApplyOutcome ionc_apply_contact(const ContactRecord *rec, int debugMode);`
  - `const char *ionc_outcome_name(IoncApplyOutcome outcome);`

- [ ] **Step 1: Dichiarare l'operazione di scrittura in `ion_contacts.h`**

Aggiungere dopo la definizione di `ContactRecord`:

```c
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
```

- [ ] **Step 2: Implementare i lookup per chiave esatta in `ion_contacts.c`**

Aggiungere dopo `findOwlt`:

```c
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
```

- [ ] **Step 3: Implementare `ionc_apply_contact`**

Aggiungere in fondo a `ion_contacts.c`:

```c
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
            return IONC_ERROR;
        }

        rc = rfx_insert_contact(IONC_DEFAULT_REGION, rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode,
                (size_t) rec->xmitRate, confidence, &cxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_contact (dopo remove) %lu→%lu "
                    "ha restituito %d", rec->fromNode, rec->toNode, rc);
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
            return IONC_ERROR;
        }

        rc = rfx_insert_range(rec->fromTime, rec->toTime,
                (uvast) rec->fromNode, (uvast) rec->toNode, rec->owlt,
                &rxaddr, 0);
        if (rc != 0) {
            dtnex_log("⚠️  Anomalia: rfx_insert_range (dopo remove) %lu→%lu ha "
                    "restituito %d", rec->fromNode, rec->toNode, rc);
            return IONC_ERROR;
        }
        rangeOutcome = IONC_REPLACED;
    }

    if (debugMode) {
        dtnex_log("[ion] %lu→%lu from=%ld to=%ld: contatto=%s range=%s",
                rec->fromNode, rec->toNode, (long) rec->fromTime,
                (long) rec->toTime, ionc_outcome_name(contactOutcome),
                ionc_outcome_name(rangeOutcome));
    }

    return (rangeOutcome > contactOutcome) ? rangeOutcome : contactOutcome;
}
```

- [ ] **Step 4: Riscrivere `processCborContactMessage`**

Sostituire integralmente la funzione (`dtnex.c:3191-3308`):

```c
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
     * scarta senza inserire e senza inoltrare. */
    
    // 4. Non processiamo i nostri stessi messaggi
    if (origin == config->nodeId) {
        debug_log(config, "⏭️ Skipping own contact message");
        return 0;
    }
    
    // 5. Solo la sorgente annuncia la propria direzione (§4)
    if (contact->fromNode != origin) {
        debug_log(config, "❌ Scartato: fromNode=%lu != origin=%lu",
                contact->fromNode, origin);
        return -1;
    }
    
    // 6. toTime = 0 in ION significa "contatto scoperto" -> MAX_POSIX_TIME
    if (contact->toTime == 0) {
        debug_log(config, "❌ Scartato: toTime = 0 (semantica di contatto permanente)");
        return -1;
    }
    
    // 7. Integrita' della finestra
    if (contact->fromTime >= contact->toTime) {
        debug_log(config, "❌ Scartato: fromTime=%ld >= toTime=%ld",
                (long) contact->fromTime, (long) contact->toTime);
        return -1;
    }
    
    // 8. Finestra gia' scaduta
    if (contact->toTime <= currentTime) {
        debug_log(config, "❌ Scartato: finestra interamente nel passato "
                "(toTime=%ld, adesso=%ld) — possibile clock skew fra i nodi",
                (long) contact->toTime, (long) currentTime);
        return -1;
    }
    
    // 8b. Finestra troppo nel futuro: sospetto di clock skew (§7.6)
    if (contact->fromTime > currentTime + CLOCK_SKEW_FUTURE_LIMIT) {
        debug_log(config, "❌ Scartato: finestra troppo nel futuro "
                "(fromTime=%ld, adesso=%ld) — possibile clock skew fra i nodi",
                (long) contact->fromTime, (long) currentTime);
        return -1;
    }
    
    // 9. Senza range CGR scarta il contatto: inutile inserirlo
    if (contact->owlt == 0) {
        debug_log(config, "❌ Scartato: owlt assente o nullo per %lu→%lu",
                contact->fromNode, contact->toNode);
        return -1;
    }
    
    /* Si scrive cio' che si impara, si annuncia solo cio' di cui si e'
     * autoritativi (§4.5): anche i contatti con toNode == me si inseriscono. */
    outcome = ionc_apply_contact(contact, config->debugMode);
    if (outcome == IONC_ERROR) {
        dtnex_log("❌ Applicazione in ION fallita per %lu→%lu",
                contact->fromNode, contact->toNode);
        return -1;
    }
    
    if (outcome != IONC_NOOP) {
        dtnex_log("✅ Contatto %lu→%lu %s in ION",
                contact->fromNode, contact->toNode, ionc_outcome_name(outcome));
    }
    
    // Inoltro invariato (§6.5): un messaggio valido si inoltra sempre
    forwardCborContactMessage(config, nonce, timestamp, expireTime, origin, from, contact);
    
    return 0;
}
```

- [ ] **Step 5: Compilare**

Run: `./build_standalone.sh`
Expected: build pulita.

- [ ] **Step 6: Verifica manuale — il contatto dell'operatore sopravvive (prova 4 di §9)**

Su B, prima di far arrivare i messaggi:

```bash
# su B: contatto manuale per la stessa coppia A-B, con una finestra diversa
ionadmin <<'EOF'
a contact +7200 +10800 <A> <B> 50000 1.0
a range +7200 +10800 <A> <B> 3
EOF
echo 'l contact' | ionadmin
```

Avviare dtnex su A e su B, attendere l'arrivo di un contatto A→B con `fromTime` diverso da +7200, poi:

```bash
echo 'l contact' | ionadmin
```

Expected: su B compaiono **entrambi** i contatti — quello manuale con la sua finestra intatta e quello ricevuto. Prima di questa modifica il manuale spariva.

- [ ] **Step 7: Verifica manuale — anti-churn (prova 5 di §9)**

Con i due nodi in regime stabile e nessuna modifica a ionrc, osservare due cicli di refresh consecutivi sul log di B in `--debug`:

```bash
grep "\[ion\]" dtnex_debug.log | tail -20   # se si usa dtnex_dbg
# oppure, sullo stdout di --debug:
./dtnex --debug 2>&1 | grep "\[ion\]"
```

Expected: dal secondo annuncio in poi ogni riga riporta `contatto=no-op range=no-op`, e `l contact` su B non cambia fra un ciclo e l'altro.

- [ ] **Step 8: Commit**

```bash
git add ion_contacts.h ion_contacts.c dtnex.c
git commit -m "fix(ion): scrittura idempotente dei contatti con rimozione mirata

ionc_apply_contact confronta il contatto ricevuto con quello gia' presente
in ION e scrive solo se serve: no-op se identico, rfx_revise_contact se
cambiano xmitRate o confidence, remove+insert solo se cambia la finestra.
Le rimozioni passano il fromTime esatto invece di NULL, quindi non
cancellano piu' i contatti configurati dall'operatore.

La ricezione applica la pipeline di validazione: fromNode == origin,
toTime != 0, fromTime < toTime, finestra non scaduta ne' assurdamente
futura, owlt presente. Un messaggio scartato non viene inoltrato."
```

---

### Task 4: Scorporo di `getContacts` e rilevazione esplicita del restart di ION

La regola "zero contatti ⇒ ION è ripartito ⇒ riavvia dtnex" diventa dannosa nel design nuovo: un nodo appena avviato o senza contatti configurati si riavvierebbe in loop. Va sostituita da un controllo esplicito, e la stampa diagnostica va nel modulo.

**Files:**
- Modify: `ion_contacts.h` (prototipi `ionc_print_contact_table`, `ionc_check_alive`)
- Modify: `ion_contacts.c` (implementazioni)
- Modify: `dtnex.c:1009-1215` (`getContacts` diventa un wrapper sottile)

**Interfaces:**
- Consumes: modulo `ion_contacts` (Task 1), `ContactRecord`.
- Produces:
  - `int ionc_print_contact_table(int debugMode);` → numero di contatti nell'RBT, `-1` se ION non è accessibile.
  - `int ionc_check_alive(unsigned long expectedNodeId);` → `1` ION vivo e con lo stesso node number, `0` ION ripartito o riconfigurato, `-1` ION non accessibile.

- [ ] **Step 1: Dichiarare le due operazioni in `ion_contacts.h`**

```c
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
```

- [ ] **Step 2: Implementare `ionc_print_contact_table` in `ion_contacts.c`**

```c
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
```

Nota: `durationStr` passa da 20 a 24 byte, il che elimina il warning preesistente `snprintf truncation in getContacts`.

- [ ] **Step 3: Implementare `ionc_check_alive` in `ion_contacts.c`**

```c
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
```

- [ ] **Step 4: Ridurre `getContacts` a un wrapper**

Sostituire integralmente `getContacts` (`dtnex.c:1009-1215`):

```c
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
```

- [ ] **Step 5: Compilare**

Run: `./build_standalone.sh`
Expected: build pulita; il warning `snprintf truncation in getContacts` non compare più.

- [ ] **Step 6: Verifica manuale — nessun riavvio a contatti zero**

```bash
# svuotare il contact plan di ION
ionadmin <<'EOF'
l contact
EOF
# rimuovere manualmente tutti i contatti, poi:
./dtnex --debug
```

Expected: dtnex resta in esecuzione, stampa `Total contacts: 0` e uno snapshot vuoto, **senza** entrare nel ciclo di riavvio. Fermando ION (`ionstop`), invece, dtnex rileva la perdita e tenta la riconnessione.

- [ ] **Step 7: Commit**

```bash
git add ion_contacts.h ion_contacts.c dtnex.c
git commit -m "refactor(ion): scorpora la tabella diagnostica e corregge la rilevazione del restart

La stampa dei contatti passa nel modulo ion_contacts. La regola 'zero
contatti = ION ripartito' viene sostituita dal confronto di ownNodeNbr
letto dall'IonDB: un nodo appena avviato o di bordo ha legittimamente
zero contatti e non deve riavviarsi in loop."
```

---

### Task 5: Mutex su `getplanlist`

`getplanlist()` scrive nelle statiche `cachedPlans[]` / `cachedPlanCount` ed è chiamata sia dal main loop sia dal thread di ricezione via `forwardCborContactMessage`. La race esiste già oggi; il redesign non la introduce ma nemmeno la risolve (§7.5).

**Files:**
- Modify: `dtnex.c:498-654` (`getplanlist`)

**Interfaces:**
- Consumes: niente di nuovo.
- Produces: nessuna modifica di firma. `getplanlist` diventa thread-safe.

- [ ] **Step 1: Aggiungere il mutex e serializzare la funzione**

In `dtnex.c`, subito prima di `void getplanlist(...)` (riga 498):

```c
/* getplanlist e' chiamata sia dal main loop sia dal thread di ricezione
 * (via forwardCborContactMessage) e scrive nella propria cache statica:
 * l'accesso va serializzato (§7.5). */
static pthread_mutex_t planListMutex = PTHREAD_MUTEX_INITIALIZER;
```

- [ ] **Step 2: Prendere e rilasciare il lock su tutti i cammini di uscita**

Nel corpo di `getplanlist`, prendere il lock subito dopo `*planCount = 0;` e `time(&currentTime);`:

```c
    pthread_mutex_lock(&planListMutex);
```

e inserire `pthread_mutex_unlock(&planListMutex);` immediatamente prima di **ognuno** dei quattro `return;` esistenti e prima della caduta finale fuori dalla funzione. I punti di uscita, nell'ordine in cui compaiono oggi:

1. cache ancora valida (dopo `dtnex_log("Using cached plan list ...")`)
2. `sdr == NULL` (dopo il fallback sulla cache)
3. `sdr_begin_xn(sdr) < 0`
4. `bpConstants == NULL` (dopo `sdr_exit_xn(sdr)`)
5. fine naturale della funzione, dopo `dtnex_log("%d neighbors found in ION configuration", *planCount);`

Verificare a occhio, prima di compilare, che ogni `return` interno alla funzione sia preceduto dall'unlock e che il `break` del limite `MAX_PLANS` non salti fuori dalla funzione (non lo fa: esce solo dal `for`).

- [ ] **Step 3: Compilare**

Run: `./build_standalone.sh`
Expected: build pulita.

- [ ] **Step 4: Verifica statica del lock**

Run: `grep -n "planListMutex\|return;" dtnex.c | sed -n '/getplanlist/,/^$/p'`

Più semplicemente, ispezionare il corpo della funzione:

Run: `awk '/^void getplanlist/,/^}/' dtnex.c | grep -n "pthread_mutex\|return"`
Expected: ogni riga `return` è preceduta immediatamente da una riga `pthread_mutex_unlock`; c'è esattamente una `pthread_mutex_lock` e cinque `pthread_mutex_unlock`.

- [ ] **Step 5: Verifica a runtime**

Run: `./dtnex --debug` con almeno un vicino attivo, lasciandolo girare finché non arriva e viene inoltrato almeno un messaggio di contatto.
Expected: nessun deadlock (il log continua ad avanzare), la lista dei vicini viene stampata normalmente sia dal main loop sia durante gli inoltri.

- [ ] **Step 6: Commit**

```bash
git add dtnex.c
git commit -m "fix(thread): serializza getplanlist con un mutex

La funzione scrive nella propria cache statica ed e' chiamata sia dal main
loop sia dal thread di ricezione durante l'inoltro."
```

---

### Task 6: Documentazione e versione applicativa

**Files:**
- Modify: `CLAUDE.md` (sezioni "Message pipeline", "Two CBOR message types", "ION Contact Management Notes", "Configuration")
- Modify: `README.md` (descrizione del protocollo, se presente)
- Modify: `dtnex.conf` (commento su `contactLifetime`)
- Modify: `dtnex.h:50` (`DTNEXC_VERSION`)

**Interfaces:**
- Consumes: tutto il lavoro dei Task 1-5.
- Produces: nessuna interfaccia di codice.

- [ ] **Step 1: Bump della versione applicativa**

In `dtnex.h` riga 50:

```c
#define DTNEXC_VERSION "3.00"
```

- [ ] **Step 2: Correggere le due imprecisioni verificate in `CLAUDE.md` (§10)**

Nella sezione "Two CBOR message types", sostituire:

```markdown
**Two CBOR message types** (protocol version 3, max 128 bytes):
- **Type 1 (Contact):** `[fromNode, toNode, fromTime, toTime, xmitRate, confidence, owlt]`
  — tempi epoch assoluti, `xmitRate` in **byte** al secondo (come ION), `confidence` in percentuale 0-100, `owlt` in secondi. Messaggi direzionali: un nodo annuncia solo i contatti in cui è lui il `fromNode`.
- **Type 2 (Metadata):** node identity, GPS coords, contact info
```

- [ ] **Step 3: Aggiornare la pipeline e le note su ION in `CLAUDE.md`**

Sostituire il blocco "Message pipeline":

```markdown
**Message pipeline:**
```
Bundle received → decodeCborMessage() → version 3 + HMAC verify + nonce dedup
    → processCborContactMessage()  → validazione (fromNode==origin, finestra
                                      valida e non scaduta, owlt presente)
                                   → ionc_apply_contact() [scrittura idempotente]
    → processCborMetadataMessage()
    → forwardCborXxxMessage() to all neighbors except origin/sender
```
```

Sostituire la sezione "ION Contact Management Notes":

```markdown
## ION Contact Management Notes

Tutto l'accesso a ION per contatti e range vive in `ion_contacts.c`, che non
conosce CBOR, bundle né vicini. Tre operazioni: leggere i contatti
annunciabili (`fromNode == nodo locale`, uniti al range per l'OWLT),
applicare un contatto ricevuto, stampare la tabella diagnostica.

Le scritture sono **idempotenti**: se ION contiene già il contatto non si
scrive nulla; se cambiano solo `xmitRate`/`confidence` si usa
`rfx_revise_contact`; solo un cambio di `toTime` comporta remove+insert.

Le rimozioni passano **sempre** il puntatore al `fromTime` esatto. Passare
`NULL` equivale allo scope `*` di ionadmin e cancella tutti i contatti della
coppia, inclusi quelli configurati a mano dall'operatore: è il bug corretto
nella v3.00, da non reintrodurre.

Un contatto senza range non viene annunciato e un messaggio senza `owlt`
valido non viene inserito: CGR scarterebbe comunque un contatto privo di
range.

Limitazioni consapevoli: dtnex è **mono-region** (`regionNbr` non viaggia sul
filo) e **non esiste revoca** — se l'operatore cancella un contatto in
locale, le copie remote restano fino al loro `toTime`.
```

- [ ] **Step 4: Documentare il cambio di semantica di `contactLifetime` (§7.2)**

Nella sezione "Configuration" di `CLAUDE.md`, sostituire la riga di `contactLifetime`:

```markdown
- `bundleTTL` — durata di validità dei bundle
- `contactLifetime` — **dalla v3.00 non governa più la durata dei contatti annunciati**, che viene letta da ION (ionrc). Resta in uso solo per i messaggi di metadata.
```

In `dtnex.conf`, sostituire il commento sopra `contactLifetime`:

```
# Contact lifetime - dalla v3.00 vale SOLO per i messaggi di metadata.
# La durata dei contatti annunciati viene letta dal contact plan di ION
# (ionrc), non piu' da questo parametro.
contactLifetime=1800  # 30 minutes
```

- [ ] **Step 5: Aggiornare la sezione architetturale di `CLAUDE.md`**

Nella sezione "Architecture", sostituire l'intestazione del core:

```markdown
### Core: `dtnex.c` (~3400 lines) + `dtnex.h`, più il modulo `ion_contacts.{c,h}`
```

e aggiungere in coda al punto 2 del "Main loop":

```markdown
2. Contact exchange with neighbors (`exchangeWithNeighbors`) — tre trigger:
   `updateInterval` scaduto, lista dei plan cambiata, oppure snapshot dei
   contatti annunciabili cambiato (rilevato entro `MY_CONTACTS_CACHE_TTL`)
```

- [ ] **Step 6: Allineare `README.md`**

Run: `grep -n "duration\|bit per second\|bps\|protocol version\|nodeA\|nodeB" README.md`

Per ogni occorrenza che descriva il vecchio payload a 3 campi, la versione 2 del protocollo o il data rate in bit/secondo, applicare le stesse correzioni fatte in `CLAUDE.md`. Se `README.md` non contiene nessuna di queste descrizioni, non modificarlo.

- [ ] **Step 7: Verifica finale end-to-end (prova 6 di §9, se il testbed a tre nodi è disponibile)**

Con A—B—C in catena e dtnex in `--debug` su tutti e tre:

```bash
# su ciascun nodo
echo 'l contact' | ionadmin | grep "<A>"
```

Expected: il contatto originato da A ha `fromTime` e `toTime` **identici** su A, B e C. Su C il messaggio è arrivato con due hop e la finestra non è stata ricalcolata da nessuno.

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md README.md dtnex.conf dtnex.h
git commit -m "docs: allinea la documentazione al protocollo v3

Payload di contatto a 7 campi, xmitRate in byte/secondo, contactLifetime
non governa piu' la durata dei contatti annunciati, limitazioni mono-region
e assenza di revoca. Versione applicativa 3.00."
```

---

## Copertura della spec

| sezione spec | task |
|---|---|
| §1.1 contatto annunciato ≠ salvato | 1 (lettura), 2 (origination + tempi assoluti) |
| §1.2 range inventato | 1 (join owlt), 2 (owlt sul filo) |
| §1.3 sovrascrittura indiscriminata | 3 (`&fromTime`) |
| §3.2 `myContacts` sola lettura da ION | 1, 2 (cache) |
| §3.4 contatti privi di range | 1 (origination), 3 (controllo 9 in ricezione) |
| §4 regola di autorità | 1 (filtro), 3 (controllo 5) |
| §5.1 versione 3, v2 scartati | 2 |
| §5.2 payload a 7 campi | 2 |
| §6.1 pipeline di validazione | 2 (controllo 1), 3 (controlli 4-9) |
| §6.2 scrittura idempotente del contatto | 3 |
| §6.3 scrittura del range | 3 |
| §6.4 primitiva di camminata riusabile | 1 (`findOwlt`), 3 (`findContact`/`findRange`) |
| §6.5 inoltro | 3 (scartato ⇒ non inoltrato) |
| §6.6 rilevazione del restart | 4 |
| §7.1 tre trigger di annuncio | 2 |
| §7.2 semantica di `contactLifetime` | 2 (`expireTime = toTime`), 6 (docs) |
| §7.4 cache a solo TTL + commento sull'invariante | 2 |
| §7.5 concorrenza, mutex su `getplanlist` | 5 |
| §7.6 errori come anomalie, clock skew | 3 |
| §8.1 estrazione di `ion_contacts.c` | 1, 3, 4 |
| §9 piano di validazione | prove 1→T1, 2/3→T2, 4/5→T3, 6→T6 |
| §10 correzioni alla documentazione | 6 |

## Decisioni derivate, non presenti nella spec

Vanno segnalate in review perché sono interpretazioni, non trascrizioni:

1. **Filtro per `ContactType`** in `ionc_get_own_contacts`: si annunciano solo `CtScheduled` e `CtPredicted`. Discende dal controllo 6 di §6.1 — i `CtDiscovered` hanno `toTime = MAX_POSIX_TIME` e annunciarli propagherebbe un contatto eterno.
2. **Controllo 8b (finestra troppo nel futuro)**: §7.6 chiede di loggare come sospetto di clock skew i messaggi scartati perché "interamente troppo nel futuro", ma la tabella di §6.1 non ha il controllo corrispondente. Aggiunto con soglia di 30 giorni.
3. **TTL dello snapshot = 60 secondi**, allineato al periodo massimo di risveglio del main loop, e chiamata di `exchangeWithNeighbors` a ogni risveglio invece che solo alla scadenza programmata: senza questo il trigger 3 di §7.1 resterebbe governato da `updateInterval`.
4. **Join del range con corrispondenza simmetrica** (`fromNode/toNode` o `toNode/fromNode`) e per sovrapposizione di finestre: l'OWLT in ION è una proprietà geometrica simmetrica e §6.3 registra che i range hanno spesso finestre più lunghe dei contatti.
5. **Versione applicativa 3.00**: la spec bumpa il protocollo ma non la versione del binario.
