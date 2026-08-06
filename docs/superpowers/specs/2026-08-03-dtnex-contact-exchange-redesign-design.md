# DTNEX — Redesign dello scambio di contatti

**Data:** 2026-08-03
**Stato:** implementato sul branch `redesign`, con i limiti noti registrati in fondo
**Versione protocollo risultante:** 3 (da 2)

---

## 1. Problemi da risolvere

Tre difetti dell'implementazione attuale, tutti verificati sul codice.

### 1.1 Il contatto inviato non corrisponde a quello salvato

`exchangeWithNeighbors` (`dtnex.c:734-777`) non legge il contact plan di ION. Costruisce il `ContactInfo` sinteticamente dai *plan* (adiacenze del convergence layer): `nodeA = nodeId locale`, `nodeB = un vicino`, `duration = contactLifetime / 60` preso dal file di configurazione.

`getContacts` (`dtnex.c:1009`) legge davvero l'RBT dei contatti di ION, ma è una funzione puramente diagnostica: stampa una tabella, conta le entry, rileva il restart di ION. Il suo output non alimenta nulla.

I due mondi non si toccano mai. La sorgente di verità di ciò che viene annunciato sono i plan, non i contatti.

Seconda fonte di divergenza: in ricezione (`dtnex.c:3241-3244`) il `timestamp` ricevuto viene scartato e lo start time riscritto a `time(NULL)` locale. Su un flood multi-hop lo stesso contatto assume una finestra assoluta diversa su ogni nodo.

### 1.2 Il range è inventato

`owlt = 1` secondo, hardcoded (`dtnex.c:3269`). I range non vengono mai scambiati fra nodi. Non è l'unico parametro fabbricato: anche `xmitRate = 100000` e `confidence = 1.0` sono hardcoded all'inserimento (`dtnex.c:3224-3225`).

### 1.3 Sovrascrittura indiscriminata

`rfx_remove_contact(regionNbr, NULL, ...)` con timestamp `NULL` equivale allo scope `*` di ionadmin: cancella **tutti** i contatti della coppia, inclusi quelli configurati a mano dall'operatore, che dtnex non ha mai inserito. Idem per i range (`dtnex.c:3230-3233`, `3274-3277`).

Accade a ogni bundle ricevuto, quindi anche sui re-flood: churn continuo del contact plan, che invalida ripetutamente le rotte CGR.

---

## 2. Principio guida

Tre nozioni oggi confuse vengono separate nettamente:

| concetto | significato | ruolo nel design |
|---|---|---|
| **plan** | "ho un outduct verso X" | livello trasporto: a chi spedisco i bundle |
| **contact** | "c'è una finestra di trasmissione X→Y" | payload: quale topologia annuncio |
| **range** | OWLT fra due nodi | payload, trasportato insieme al contatto |

In ION plan e contact si configurano indipendentemente. La confusione fra i due è la causa d'origine del problema 1.

---

## 3. Modello dati

### 3.1 `plans[]` — peer set del flooding

Ruolo: a chi spedisco i bundle. Nient'altro.

Struttura e popolamento invariati (`planId`, `timestamp`, da `getplanlist()` sui `BpPlan` di ION, cache a TTL). Tutto il codice che lo usa per costruire il `destEid` resta valido. Cambia solo il ruolo concettuale: non è più una sorgente di dati di contatto.

### 3.2 `myContacts[]` — set di origination

Ruolo: cosa annuncio. **Sola lettura da ION**, snapshot con cache a TTL, nessuno stato locale.

Popolato camminando `ionvdb->contactIndex` con filtro `fromNode == config->nodeId`. Per ogni contatto si cerca in `ionvdb->rangeIndex` il range corrispondente per ricavare l'`owlt`.

| campo | origine |
|---|---|
| `regionNbr` | `IonCXref` |
| `fromNode` | `IonCXref` — sempre `== nodeId` per costruzione |
| `toNode` | `IonCXref` |
| `fromTime`, `toTime` | `IonCXref` — assoluti, mai riscritti |
| `xmitRate` | `IonCXref` — byte/secondo |
| `confidence` | `IonCXref` |
| `owlt` | `IonRXref`, via join |

Nessun campo di bookkeeping: niente `origin` (sono sempre io), niente `owned`, niente `lastHeard`. Un solo timestamp di validità per l'intero snapshot.

Questa struttura risolve i problemi 1.1 e 1.2 insieme: ciò che si annuncia è letteralmente ciò che ION ha, campo per campo.

### 3.3 Nessuna terza struttura

Non serve un registro di "cosa ho inserito io". La rimozione mirata per `fromTime` esatto è sufficiente (§6.2). I contatti ricevuti passano da ION e basta; dtnex non ne tiene copia in memoria.

### 3.4 Contatti privi di range

Un contatto senza range locale **non viene annunciato**, con log a livello debug. Motivo: CGR scarta dalla considerazione come next-hop un contatto privo di range, quindi annunciarlo occuperebbe SDR e genererebbe churn senza mai produrre una rotta.

In ricezione **non** c'è un filtro simmetrico: il formato v3 porta sempre il campo `owlt` e `0` è un valore legittimo (§6.1). Il filtro vive tutto in origination.

Effetto collaterale voluto: un errore di configurazione (contatto senza range in ionrc) diventa visibile invece di propagarsi silenziosamente.

---

## 4. Regola di autorità: `fromNode == me`

**Un nodo annuncia solo i contatti in cui è lui il `fromNode`. Rifiuta ogni messaggio ricevuto che dichiari `fromNode == me`.**

### 4.1 Perché non annunciare entrambe le direzioni

Sebbene una configurazione ionrc tipica contenga entrambe le direzioni, la copertura di rete è già completa senza. Sul link A↔B: A annuncia `A→B`, B annuncia `B→A`. Entrambe le direzioni raggiungono la rete, da sorgenti diverse. Annunciarle da entrambi i capi è ridondanza, non copertura aggiuntiva.

### 4.2 Cosa si guadagna

- ogni direzione ha esattamente una sorgente autoritativa → nessuna regola di conflict resolution
- nessuna scrittura di dtnex può rientrare nel set di origination → **cache a solo TTL**, senza invalidazione su scrittura
- niente flag `owned` nella struttura, niente stato persistente fra riavvii

### 4.3 Cosa si rinuncia

La copertura di `B→A` quando il dtnex di B è spento con ION acceso, o quando B non esegue dtnex. In quel caso la rete vede il link a metà.

### 4.4 Escape hatch

Se un domani quella copertura serve: si annunciano anche i contatti con `toNode == me` e si aggiunge in ricezione la regola di precedenza *vince l'annuncio con `origin == fromNode`*. La sorgente autoritativa batte quella di rimbalzo, quindi niente flapping.

**Non cambia il formato del messaggio CBOR.** Rilassa il controllo di validazione n. 5 (§6.1). È un interruttore attivabile in seguito, non una scelta da bloccare ora.

### 4.5 Contatti ricevuti con `toNode == me`

**Vengono inseriti in ION.** Senza, il nodo non ha le rotte di ritorno: conoscerebbe `me→B` ma non `B→me`.

La regola è: si scrive ciò che si impara, si annuncia solo ciò di cui si è autoritativi. Le due cose sono indipendenti.

---

## 5. Formato del messaggio

### 5.1 Envelope: invariato

Resta l'array CBOR a 9 elementi: `[version, tipo, timestamp, expireTime, origin, from, nonce, payload, hmac]`. Nonce, HMAC, dedup e forwarding non vengono toccati.

`version` passa **da 2 a 3**. I messaggi v2 vengono **scartati**, con log a debug: una rete a versioni miste ricreerebbe esattamente l'incoerenza che il redesign elimina.

### 5.2 Payload contatto: da 3 a 7 campi

| campo | tipo sul filo | note |
|---|---|---|
| `fromNode` | intero | direzionale, sempre `== origin` |
| `toNode` | intero | |
| `fromTime` | intero | epoch assoluto, mai riscritto |
| `toTime` | intero | epoch assoluto |
| `xmitRate` | intero | byte/secondo, come ION |
| `confidence` | intero 0-100 | percentuale |
| `owlt` | intero | secondi |

### 5.3 Scelte di codifica

**Tempi assoluti, non durata.** È la decisione che risolve strutturalmente il problema 1.1: il ricevente non ricalcola mai la finestra. Richiede sincronizzazione oraria fra i nodi, che ION già impone per CGR — non è un requisito nuovo.

**`toTime` assoluto, non delta da `fromTime`.** Il delta risparmierebbe ~3 byte introducendo un'asimmetria fra i due estremi. Chiarezza sopra 3 byte.

**`confidence` come intero 0-100.** Obbligato: `include/ion/cbor.h` non ha encoding per i float. Si perde la granularità dello 0.01 rispetto al `float` di ION; in pratica i valori usati sono 1.0 per i contatti schedulati e valori grossolani per quelli predetti. Codificare il float come byte string grezza sarebbe fragile fra architetture diverse.

**`regionNbr` non va sul filo.** Il ricevente inserisce nella propria region di default. Propagarlo rischierebbe che il ricevente tenti di inserire in una region che non conosce, facendo fallire l'inserimento. **Limitazione consapevole: dtnex resta mono-region**, e va documentata come tale.

### 5.4 Budget dimensionale e traffico

Envelope ~33-37 byte, payload ~26-30 byte → **circa 60-67 byte per messaggio**, contro i ~50 attuali. `MAX_CBOR_BUFFER` resta 128, con margine abbondante.

**Volume di messaggi invariato.** Oggi: N contatti (`me↔B`) × N vicini = N². Domani: N contatti (`me→B`) × N vicini = N². Il passaggio a messaggi direzionali non raddoppia nulla, perché la direzione opposta la annuncia l'altro capo. Il volume cresce solo se ION ha più finestre temporali per la stessa coppia — nel qual caso si sta propagando informazione reale in più.

---

## 6. Ricezione e scrittura in ION

### 6.1 Pipeline di validazione

In ordine, prima di toccare ION. Ogni fallimento: **scarta senza inserire e senza inoltrare**.

| # | controllo | motivo |
|---|---|---|
| 1 | `version == 3` | rete a versioni miste ricrea l'incoerenza |
| 2 | HMAC valido | invariato |
| 3 | nonce non duplicato | invariato |
| 4 | `origin != me` | invariato (`dtnex.c:3196`) |
| 5 | `fromNode == origin` | solo la sorgente annuncia la propria direzione |
| 5b | `fromNode != toNode` | un contatto verso se stessi è la semantica dei *contatti di registrazione*, non topologia; l'origination lo filtra già, ma un peer in possesso della chiave potrebbe iniettarlo |
| 6 | `toTime != 0` | in ION `toTime = 0` significa *contatto scoperto* → `MAX_POSIX_TIME`, cioè permanente (`rfx.h:53-56`) |
| 7 | `fromTime < toTime` | integrità |
| 7b | `fromTime <= 0` | in ION `fromTime = 0` significa *contatto ipotetico* |
| 7c | `fromTime >= MAX_POSIX_TIME` | in ION è il trigger dei *contatti di registrazione* |
| 7d | `xmitRate == 0` | ION rifiuta il contatto con errore utente 5 |
| 7e | `confidence > 100` | fuori dal range 0-100: ION rifiuta con errore utente 4 |
| 8 | `toTime > now` | già scaduto: inutile inserirlo e inondarlo |
| 8b | `fromTime <= now + 30 giorni` | finestra troppo nel futuro: sospetto di clock skew (§7.6) |

**Non c'è un controllo sull'`owlt`.** La prima stesura ne prevedeva uno ("`owlt` presente e valido"), ma `owlt = 0` è un valore legittimo — su una LAN è quello fisicamente corretto, e ION accetta `a range ... 0`. Il controllo contraddiceva l'origination, dove `findOwlt` restituisce 0 come valore valido, e su un testbed locale avrebbe fatto scartare ogni contatto a ogni ricevente. Il formato v3 porta sempre il campo e §3.4 garantisce che si annuncino solo contatti per cui un range esiste davvero: il controllo in ricezione era ridondante.

Un messaggio scartato da questa pipeline restituisce **0**, non -1: lo scarto è una decisione di policy, non un fallimento di decodifica — il messaggio si è decodificato correttamente, si è solo deciso di non applicarlo. Un -1 risalirebbe fino a `decodeCborMessage` e produrrebbe un fuorviante "formato bundle sconosciuto" per un messaggio perfettamente valido.

Il controllo 5 rende superfluo un controllo esplicito su `fromNode == me`: cadrebbe già al 4. È la regola che si rilassa attivando l'escape hatch (§4.4).

Il controllo 6 non è teorico: un messaggio malformato che innescasse quella semantica inserirebbe un contatto eterno, propagato per flooding a tutta la rete.

### 6.2 Scrittura del contatto

Identità: `K = (regionNbr locale, fromNode, toNode, fromTime)`.

| stato in ION | azione |
|---|---|
| `K` non esiste | `rfx_insert_contact` |
| `K` esiste, tutto identico | **no-op** |
| `K` esiste, cambiano solo `xmitRate` / `confidence` | **`rfx_revise_contact`** — in place |
| `K` esiste, cambia `toTime` | `rfx_remove_contact(&fromTime)` + `rfx_insert_contact` |
| `K` non esiste ma la finestra si sovrappone a un altro contatto locale | ION rifiuta con codice 9, si mantiene quello locale, si logga a debug: **non è un fallimento** |

**La riga che risolve il problema 1.3 è una sola: `&fromTime` al posto di `NULL`.** Con `NULL` ION applica lo scope `*` e cancella tutti i contatti della coppia; con il puntatore al `fromTime` esatto colpisce solo quello che si sta davvero aggiornando.

Le prime tre righe risolvono il churn: oggi *ogni* bundle ricevuto fa remove+insert. Con queste regole il refresh periodico — il caso dominante — non tocca ION.

`rfx_revise_contact` (`include/ion/rfx.h:101`) ha chiave `(regionNbr, fromTime, fromNode, toNode)`, esattamente la tupla di identità.

### 6.3 Scrittura del range

Stessa struttura, identità `(fromNode, toNode, fromTime)`, ma **non esiste `rfx_revise_range`**: non esiste → insert; identico → no-op; diverso → `rfx_remove_range(&fromTime)` + insert.

Contatto e range si inseriscono **nello stesso passaggio, dal medesimo messaggio**. È il motivo per cui `owlt` sta nel messaggio di contatto invece di avere un tipo `RangeMessage` separato: la coppia non può mai arrivare dimezzata, lasciando un contatto senza range (inutile a CGR) o un range orfano.

> Trade-off registrato: in ION "reale" i range hanno spesso finestre più lunghe dei contatti, perché il ritardo di propagazione è una proprietà della geometria e resta valido a link spento. Per l'uso attuale di dtnex — annunciare raggiungibilità fra vicini — l'accoppiamento è più semplice e più robusto. Scelta reversibile.

### 6.4 Controllo di esistenza

Negli header non esiste nessuna `rfx_find_*`: il controllo richiede la camminata dell'RBT su `ionvdb->contactIndex` e `ionvdb->rangeIndex`, stesso pattern di `getContacts` (`dtnex.c:1120-1122`).

Non è un hot path: un messaggio per vicino per `updateInterval`, più i flood.

Serve una **primitiva di camminata riusabile**, perché la stessa serve a costruire lo snapshot `myContacts`. Una primitiva, due chiamanti — non un'unica funzione che fa entrambe le cose, altrimenti si riaccoppia la cache alle scritture.

### 6.5 Inoltro

Invariato: un messaggio che supera la validazione viene inserito e poi inoltrato a tutti i vicini tranne `origin` e `from`. Un messaggio scartato non viene inoltrato.

I contatti con `toNode == me` seguono la regola generale: si inseriscono **e** si inoltrano. Sono topologia utile anche al resto della rete.

### 6.6 Rilevazione del restart di ION

`getContacts` oggi fa doppio lavoro: stampa la tabella diagnostica **e** rileva il restart con la regola *"zero contatti → ION è ripartito → riavvia dtnex"* (`dtnex.c:1192-1194`).

Quella regola diventa dannosa nel design nuovo: un nodo appena avviato, o un nodo di bordo senza contatti ancora configurati, ha legittimamente zero contatti e si riavvierebbe in loop.

**Va sostituita** con un rilevamento esplicito: il campo `ownNodeNbr` dell'IonDB, oppure il fallimento della transazione SDR, già gestito a `dtnex.c:1100-1106`.

È un bug preesistente e scollegato, ma il redesign lo rende attivamente dannoso: incluso in scope.

---

## 7. Ciclo di vita, cache, concorrenza

### 7.1 Quando si annuncia

Tre trigger, invece dei due attuali:

1. è passato `updateInterval`
2. la lista dei `plans` è cambiata (già oggi)
3. **lo snapshot `myContacts` è cambiato** rispetto al precedente

Il terzo è nuovo: si confronta lo snapshot corrente con il precedente. Serve perché oggi un contatto aggiunto a ionrc viene scoperto dalla rete solo entro `updateInterval` (fino a 30 minuti).

**Cadenza:** il confronto avviene a ogni rinfresco della cache `myContacts`, quindi la reattività del trigger 3 è governata dal TTL della cache, non da `updateInterval`. È il TTL a fissare il compromesso fra reattività ai cambi di ionrc e frequenza di accesso a ION.

### 7.2 Cambio di semantica di `contactLifetime`

Oggi `contactLifetime` determina la durata del contatto annunciato (`dtnex.c:748`). Nel design nuovo la durata viene da ION: **quel parametro non governa più nulla sui contatti**.

`expireTime` dell'envelope diventa il `toTime` del contatto stesso — il messaggio è utile esattamente finché è valido il contatto che descrive. `contactLifetime` resta in uso solo per i messaggi di metadata.

**È un cambio di comportamento visibile a chi ha un `dtnex.conf` in produzione**: dopo l'aggiornamento la durata dei contatti annunciati non dipende più dalla configurazione ma da ionrc. Va nel changelog.

### 7.3 Soft state e scadenza

I contatti restano soft state, con una proprietà nuova che discende direttamente dai tempi assoluti: **la scadenza è consistente su tutta la rete**. Il contatto `A→B` scade allo stesso istante su A, su B e su ogni nodo raggiunto dal flooding. Prima ogni hop ricalcolava la propria finestra e la stessa informazione moriva in momenti diversi.

Il refresh periodico riannuncia la stessa finestra assoluta → per le regole di §6.2 è un no-op.

**Limite accettato, da documentare: non esiste revoca.** Se l'operatore cancella un contatto da ION locale, le copie remote restano fino al loro `toTime`. Un messaggio di withdraw aprirebbe problemi di autenticazione e di flooding di cancellazioni: fuori scope. La scadenza naturale è una rete di sicurezza sufficiente.

### 7.4 Cache

**Solo TTL, nessuna invalidazione su scrittura**, sia per `plans` che per `myContacts`, con il meccanismo già in uso.

La correttezza poggia interamente sulla regola `fromNode == me` (§4): nessuna scrittura che dtnex fa in ION può rientrare nel set di origination. **Va scritto come commento nel codice accanto alla cache**: se qualcuno rilassa quel filtro senza accorgersene, la cache diventa silenziosamente sbagliata.

Il valore del TTL non è una micro-ottimizzazione: governa la reattività del trigger 3 di §7.1. Un TTL breve fa scoprire prima i cambi di ionrc al costo di più accessi a ION; un TTL lungo fa il contrario. Va scelto con quel compromesso in mente, non copiato dai 20 secondi della cache dei plan senza ragionarci.

### 7.5 Concorrenza

**`myContacts` è mono-thread.** Lo tocca solo il main loop, in origination. Il thread di ricezione fa le proprie camminate RBT per il controllo di esistenza, su una primitiva separata che non scrive nell'array. **Nessun mutex nuovo**; la protezione sulle letture concorrenti di ION la fornisce già la transazione SDR.

**`plans` ha invece una race che esiste già oggi.** `getplanlist()` scrive nelle statiche `cachedPlans[]` / `cachedPlanCount` ed è chiamata sia dal main loop sia dal thread di ricezione via `forwardCborContactMessage` (`dtnex.c:3358`). Non è introdotta dal redesign, **ma il redesign la risolve**: è un mutex su una funzione sola, con l'unlock su tutti i cammini di uscita.

Il precedente testo di questa sezione diceva che la race restava aperta pur essendo "inclusa in scope" — due affermazioni incompatibili. Vale la seconda: si corregge.

### 7.6 Errori e diagnostica

**Fallimenti di inserimento.** Le `rfx_*` restituiscono `-1` su errore di sistema e `> 0` su errore utente. Le due classi vanno tenute separate, perché hanno significato opposto.

Il controllo di esistenza di §6.4 cerca per `fromTime` **esatto**: per costruzione non può rilevare le sovrapposizioni. Un errore utente è quindi non solo possibile ma **atteso in regime stazionario**: in un ionrc bidirezionale convenzionale entrambi i nodi dichiarano entrambe le direzioni con tempi relativi, quindi il contatto `A→B` che B annuncia si sovrappone a quello che B stesso ha già configurato in locale, ma con un `fromTime` assoluto diverso. `rfx_insert_contact` lo rifiuta con il codice 9 ("overlapping contact ignored"), e questo è il comportamento corretto: si mantiene la voce configurata dall'operatore.

Regole:

- **`rc < 0`** è l'unica anomalia: log a livello non-debug, esito `IONC_ERROR`.
- **`rc > 0`** è una condizione locale attesa: log **a debug** con il *significato* del codice (9 = sovrapposizione con voce locale, 7 = region non corrispondente, 5 = xmitRate nullo, 4 = confidence fuori range, 2 su revise = contatto bersaglio non Scheduled), esito `IONC_NOOP`, **nessun return anticipato**: la scrittura del range prosegue comunque, perché un rifiuto sul contatto non deve impedire a ION di imparare l'OWLT.
- **`rfx_insert_range` che restituisce 1** non è nemmeno un errore utente: il sorgente di ION lo commenta come *idempotente* (il range c'è già con lo stesso `owlt`). È un successo, esito `IONC_NOOP`.
- `*cxaddr` / `*rxaddr` valgono come riscontro incrociato del rifiuto (`rfx.h:62-63`), con l'eccezione dei codici emessi dalla scansione dei conflitti (insert contatto 8 e 9, insert range 1 e 2), dove ION lascia di proposito l'indirizzo della voce in conflitto.

**Sfasamento degli orologi.** I tempi assoluti presuppongono nodi sincronizzati. ION lo richiede già per CGR, ma il guasto diventa visibile: un nodo con l'orologio indietro di un'ora vedrebbe tutti i contatti altrui cadere sul controllo 8 e resterebbe isolato senza spiegazione.

Mitigazione: quando un messaggio viene scartato perché la sua finestra è interamente nel passato **o interamente troppo nel futuro**, loggarlo esplicitamente come sospetto di clock skew. Costa una riga e trasforma un guasto muto in uno diagnosticabile.

**Disconnessione di ION a metà operazione.** Invariata: la gestione esistente su fallimento della transazione SDR (`dtnex.c:1100-1106`) copre il caso, ed è la stessa che al §6.6 sostituisce la regola "zero contatti = restart".

---

## 8. Impatto sul codice

| punto | intervento |
|---|---|
| `ContactInfo` (`dtnex.h:119`) | sostituita dalla struttura a 7 campi (§3.2) |
| `DTNEX_PROTOCOL_VERSION` | 2 → 3 |
| `exchangeWithNeighbors` (`dtnex.c:674`) | riscritta: sorgente `myContacts`, non più i plan |
| `getplanlist` (`dtnex.c:498`) | ruolo invariato, aggiunto mutex (§7.5) |
| `getContacts` (`dtnex.c:1009`) | scorporata: resta la stampa diagnostica, esce la rilevazione restart (§6.6) |
| nuove primitive | camminata RBT contatti, camminata range, join per `owlt` |
| `encodeCborContactMessage` | nuovo payload (§5.2) |
| `decodeCborMessage` | nuovo payload + tabella di validazione (§6.1) |
| `processCborContactMessage` (`dtnex.c:3190`) | riscritta: regole idempotenti, `&fromTime` al posto di `NULL` |
| `forwardCborContactMessage` (`dtnex.c:3358`) | logica invariata, si adegua ai nuovi campi |
| `CLAUDE.md` | correzioni (§10) |

### 8.1 Estrazione di `ion_contacts.c`

Tutto ciò che parla direttamente con ION viene estratto in un file separato: camminate RBT, join contatto/range, e le regole di scrittura di §6.2-6.3.

**Perché il rischio è basso:** non è "sposta e poi cambia". La lettura di `myContacts` e il join sono codice nuovo; le regole di scrittura sono una riscrittura completa di `processCborContactMessage`, di cui non sopravvive quasi nulla. L'unica cosa realmente spostata è la camminata RBT, una ventina di righe dal pattern di `getContacts`.

**Payoff concreto:** le prove 1, 4 e 5 del piano di validazione (§9) esercitano solo questo modulo, contro un ION vivo, senza bundle né rete.

**Confine, da rispettare:** il modulo espone tre operazioni —

1. leggi il set di contatti annunciabili (filtro `fromNode == me`, join con i range)
2. applica un contatto+range ricevuto (regole idempotenti)
3. stampa la tabella diagnostica

— e **non conosce CBOR, bundle, HMAC, vicini o flooding**. Se l'encoding vi scivola dentro perché "è roba di contatti", il modulo smette di essere testabile da solo e si perde l'unica ragione per cui esiste.

---

## 9. Validazione

Non esiste test suite: la validazione è manuale in `--debug`. Ogni prova è mappata su un problema specifico, così la verifica è "questo problema è chiuso" e non "sembra funzionare".

| # | prova | verifica |
|---|---|---|
| 1 | Nodo singolo | Lo snapshot `myContacts` coincide campo per campo con `l contact` / `l range` di ionadmin → **problema 1.1** |
| 2 | Due nodi | Il contatto inserito su B ha `fromTime`, `toTime`, `xmitRate`, `owlt` identici a quelli che A ha letto da ION; nessuna riscrittura di start time → **problema 1.1 end-to-end** |
| 3 | Due nodi | L'`owlt` su B corrisponde al range configurato su A, non a 1 secondo. Contatto senza range su A → non annunciato, e il log lo dice → **problema 1.2** |
| 4 | Regressione mirata | Si configura a mano via ionadmin un contatto per la coppia, poi arriva un messaggio per la stessa coppia con `fromTime` diverso. **Il contatto manuale deve sopravvivere** (oggi sparisce) → **problema 1.3** |
| 5 | Anti-churn | Due cicli di update consecutivi senza cambiamenti: il secondo non produce nessuna `rfx_insert` o `rfx_remove` → **churn** |
| 6 | Tre nodi (se disponibile il testbed) | Lo stesso contatto propagato in due hop ha `fromTime`/`toTime` identici su tutti e tre → **proprietà di §7.3** |

---

## 10. Correzioni alla documentazione

`CLAUDE.md` contiene due imprecisioni verificate:

- dichiara il payload di tipo 1 come `[nodeA, nodeB, duration_min, datarate_bps, reliability]`; sul filo i campi sono tre (`[nodeA, nodeB, duration]`), come da `ContactInfo` in `dtnex.h:119-123`
- dichiara il data rate in **bit** al secondo; `xmitRate` in ION è in **byte** al secondo (`include/ion/ion.h:203`)

Entrambe vanno corrette insieme all'aggiornamento per il nuovo payload.

---

## 11. Decisioni registrate e loro reversibilità

| decisione | reversibile? |
|---|---|
| Tempi assoluti sul filo | No — è il fondamento del design |
| Messaggi direzionali (uno per direzione) | No — determina struttura e regola di autorità |
| Filtro di origination `fromNode == me` | **Sì** — via escape hatch §4.4, senza toccare il formato del messaggio |
| Inserire i contatti con `toNode == me` | Sì — indipendente da cosa si annuncia |
| `owlt` dentro il messaggio di contatto, niente `RangeMessage` | Sì — §6.3 |
| Mono-region, `regionNbr` fuori dal protocollo | Sì — richiede bump di versione |
| `confidence` come intero 0-100 | Vincolato da `cbor.h`, non da scelta |
| Nessun flag `owned`, nessun registro locale | Conseguenza dei tempi assoluti |
| Nessuna revoca esplicita dei contatti | Sì — richiederebbe un nuovo tipo di messaggio |

---

## 12. Limiti noti emersi in implementazione

Tre punti aperti che la review finale del branch ha trovato e che **non** vengono
corretti nell'ondata di fix pre-merge. Sono registrati qui per non perderli, non
per essere risolti adesso.

### 12.1 Rilevazione del restart di ION insufficiente

§6.6 prescrive `ownNodeNbr` oppure il fallimento della transazione SDR. Ma un ciclo
`ionstop && ionstart` **conserva** `ownNodeNbr`: il restart più comune non viene
rilevato. Serve un marcatore d'istanza — per esempio l'identità della partizione di
working memory, oppure il `PsmAddress` di `contactIndex` memorizzato all'avvio, che
una vdb nuova rialloca. Richiede una decisione di design.

### 12.2 Il join simmetrico dei range indebolisce §3.4

`findOwlt` accetta anche il verso opposto, e `ionc_apply_contact` inserisce un range
`(origin → me)` per ogni contatto accettato: quel range è un candidato valido per il
contatto locale `(me → origin)`, quindi un contatto locale **privo** di range può
comunque essere annunciato usando un OWLT imparato da un peer. §3.4 prometteva che un
errore di configurazione dell'`ionrc` diventasse visibile; qui può venire
silenziosamente coperto.

La review ha verificato che non produce churn né oscillazione: il valore è
deterministico e in configurazione simmetrica i due OWLT coincidono. Il costo è la
diagnostica persa, non l'instabilità.

Mitigazione futura: preferire il verso esatto e ricadere sull'opposto solo in sua
assenza. Va inoltre annotato che l'invariante della cache scritto in `dtnex.c` vale
per i campi di identità del contatto ma **non** per `owlt`.

### 12.3 Gli header ION di `include/ion/` non corrispondono alla libreria installata

Sono di una release diversa: `IonRegion`, `IonDB`, `IonNode` e `IonContact`
divergono, e `MAX_POSIX_TIME` vale 2147397247 nel bundle contro 2147483647
nell'installato. La review ha verificato che i layout da cui questo branch dipende —
`IonCXref`, `IonRXref`, `IonVdb`, l'offset di `ownNodeNbr` in `IonDB` — **coincidono**,
quindi oggi il modulo è salvo; ma il commit `87590d4` di questo stesso branch esiste
proprio perché una di queste divergenze aveva corrotto silenziosamente le letture RBT.

Mitigazione futura: risincronizzare gli header, oppure aggiungere in `ion_contacts.c`
un paio di `_Static_assert` su `sizeof(IonCXref)`, `offsetof(IonCXref, fromTime)` e
`sizeof(IonVdb)`, per trasformare il prossimo disallineamento in un errore di
compilazione anziché in una corruzione muta.
