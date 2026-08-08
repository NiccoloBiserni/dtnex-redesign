# Terminazione cooperativa e integrità della transazione SDR — Piano di implementazione

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fare in modo che dtnex non possa mai terminare mentre uno dei suoi thread è dentro una transazione SDR di ION, perché il lock di quella transazione vive nella memoria condivisa e sopravvive al processo, bloccando ogni altro client ION.

**Architecture:** I tre segnali di terminazione (SIGINT, SIGTERM, SIGTSTP) vengono bloccati in tutti i thread e raccolti da un thread dedicato con `sigwait()`. Quel thread non è un handler asincrono: si limita ad abbassare i flag di esecuzione e a svegliare chi è bloccato in `bp_receive`. La pulizia vera — join dei thread, chiusura dei SAP, detach da BP — resta dove già si trova, in fondo a `main`, che viene raggiunto quando `eventDrivenLoop` ritorna: un punto per costruzione fuori da ogni transazione.

**Tech Stack:** C, POSIX threads e segnali, ION-DTN (`libbp`, `libici`).

## Global Constraints

- **Ambito:** solo SIGINT, SIGTERM, SIGTSTP e uscita ordinaria — gli stessi modi di morte della v2.52. Segfault, abort e SIGKILL restano fuori ambito per scelta (spec §13.7).
- **Invariante:** ogni `sdr_begin_xn` deve avere il suo `sdr_exit_xn` / `sdr_end_xn` su **tutti** i cammini di uscita. Oggi è vero: non romperlo (spec §13.5).
- **Non introdurre** nessun recovery del `planListMutex`: è un mutex locale al processo, non può bloccare altri processi ION, e lo shutdown cooperativo lo rilascia da solo (spec §13.6).
- **Non toccare** `ion_contacts.c`: questo lavoro non ha niente a che vedere con lo scambio di contatti, e il modulo non deve conoscere segnali né ciclo di vita del processo.
- **Lingua:** commenti, messaggi di log e messaggi di commit in **italiano**, come tutto `dtnex.c`. `README.md` è in inglese: se lo si tocca, si scrive in inglese.
- **Commit:** nessun trailer `Co-Authored-By`.
- **Non esiste una suite di test.** La verifica è build pulita con `./build_standalone.sh` più le prove manuali della spec §13.8 su un ION vivo.

## Regole operative su ION (imparate sul campo, non negoziabili)

Queste regole esistono perché durante la validazione del 2026-08-08 il nodo ION si è bloccato tre volte. Violarle costa un `killm` e un riavvio.

- **Non uccidere mai `ionadmin`, `bplist` o altri strumenti ION mentre girano.** Se sembrano appesi, aspettare: stanno attendendo il lock. Ucciderli mentre tengono la transazione blocca l'intero nodo.
- **Non usare `timeout` su di loro**, e non lanciarli in comandi che un timeout esterno possa interrompere. Lanciarli in background e attendere.
- Se il nodo si blocca comunque: `ionunlock ion`, e se non basta `killm` seguito da `ionstart -I hostiondtn2.rc` dalla directory `/home/bise/ion`.
- `ionstart` su questo nodo impiega alcuni minuti (l'outduct TCP verso un indirizzo irraggiungibile va in timeout). **Non interromperlo.**

## File Structure

| file | responsabilità in questo lavoro |
|---|---|
| `dtnex.c` | tutto il cambiamento: due flag globali nuovi, la funzione `signalWaitThread`, la maschera dei segnali in `main`, la rimozione di `signalHandler`, la correzione delle join nel teardown |
| `dtnex.h` | rimozione della dichiarazione di `signalHandler` |
| `CLAUDE.md` | il modello a thread passa da 2 a 3 thread di servizio. **File in `.gitignore`: si modifica su disco e non entra in nessun commit** |

Nessun file nuovo. Il cambiamento è concentrato e non giustifica uno scorporo.

---

## Task 1: Rendere corretto il teardown esistente di `main`

Il teardown completo esiste già (`dtnex.c:1761-1788`) ma è irraggiungibile, perché oggi `signalHandler` chiama `exit(0)` prima che `eventDrivenLoop` ritorni. Prima di renderlo raggiungibile va corretto un difetto latente, altrimenti il Task 2 sostituirebbe un problema con un altro.

**Il difetto:** le due join sono protette da `if (bundleReceptionState.running)` (`dtnex.c:1765`) e `if (bpechoState.running)` (`dtnex.c:1772`). Sono le stesse variabili che la richiesta di arresto azzera. Quando il controllo viene eseguito valgono già zero, le join vengono **saltate**, e si arriva a `bp_close(sap)` / `bp_detach()` mentre i thread di servizio stanno ancora usando quelle risorse. Il thread bpecho, in particolare, chiude e azzera il proprio SAP durante la propria pulizia (`dtnex.c:1523-1526`): saltare la join significa correre contro quella pulizia.

**La correzione:** distinguere «il thread è stato creato» da «il thread deve continuare a girare», con due flag separati.

**Files:**
- Modify: `dtnex.c` — zona delle globali (intorno a `dtnex.c:64-70`), creazione dei thread in `main` (intorno a `dtnex.c:1698-1731`), **creazione dei thread nel percorso di riconnessione a ION** (`dtnex.c:2254` e `dtnex.c:2268`), teardown (`dtnex.c:1761-1788`)

> **Correzione al piano, 2026-08-08.** La prima stesura di questo task elencava solo i siti di creazione in `main`. Sono quattro, non due: `eventDrivenLoop` ricrea entrambi i thread quando dtnex si riconnette dopo un restart di ION. Se i flag non vengono alzati anche lì, il difetto resta aperto proprio nello scenario di riconnessione — e se dtnex parte con ION non raggiungibile i thread nascono *solo* lì, quindi i flag resterebbero a zero per tutta la vita del processo e il teardown salterebbe entrambe le join. Lo step 2 copre tutti e quattro i siti.

**Interfaces:**
- Consumes: niente da task precedenti.
- Produces: due globali che il Task 2 non usa ma che deve lasciare intatte:
  - `int bpechoThreadStarted;` — 1 dopo una `pthread_create` riuscita del thread bpecho
  - `int bundleReceptionThreadStarted;` — 1 dopo una `pthread_create` riuscita del thread di ricezione

- [ ] **Step 1: Dichiarare i due flag accanto alle altre globali di servizio**

In `dtnex.c`, subito sotto `pthread_t bpechoThread;` e `BundleReceptionState bundleReceptionState;` (intorno alle righe 64-70), aggiungere:

```c
/* "Il thread e' stato creato" e' un fatto diverso da "il thread deve continuare
 * a girare": il secondo viene azzerato per CHIEDERE l'arresto, quindi non puo'
 * fare da guardia alla join, o la join verrebbe saltata proprio quando serve. */
int bpechoThreadStarted = 0;
int bundleReceptionThreadStarted = 0;
```

- [ ] **Step 2: Alzare i flag dopo ogni `pthread_create` riuscita**

In `main`, nel ramo che oggi logga `"✅ Bpecho service thread started"`, aggiungere prima del log:

```c
                bpechoThreadStarted = 1;
```

E nel ramo che logga `"✅ Bundle reception thread started"`, aggiungere prima del log:

```c
                bundleReceptionThreadStarted = 1;
```

Gli stessi due assegnamenti vanno nei rami di successo delle **altre due** `pthread_create`, quelle del percorso di riconnessione a ION dentro `eventDrivenLoop`: `dtnex.c:2254` (bpecho) e `dtnex.c:2268` (ricezione), entrambe nel ramo `== 0`, prima della rispettiva riga di log di successo.

Non spostare né modificare la logica di `pthread_create` esistente, e non toccare le guardie `if (!bpechoState.running)` / `if (!bundleReceptionState.running)` che decidono se ricreare: si aggiunge solo l'assegnazione, nel ramo di successo. A fine task, `grep -n "pthread_create" dtnex.c` deve mostrare quattro siti, ognuno con il proprio flag alzato.

- [ ] **Step 3: Usare i flag nuovi come guardia delle join**

Sostituire in `dtnex.c:1764-1775` le due guardie. Il blocco diventa:

```c
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
```

L'azzeramento dopo la join rende l'operazione idempotente: se un giorno questo blocco venisse eseguito due volte, la seconda non farebbe una `pthread_join` su un thread già raccolto (comportamento indefinito).

- [ ] **Step 4: Build pulita**

Run: `./build_standalone.sh`
Expected: compilazione senza errori e senza warning nuovi sulle righe toccate. I warning preesistenti (deprecazioni OpenSSL, `const` su `bp_send`, due variabili non usate, una troncatura di `snprintf`) sono attesi e non riguardano questo cambiamento.

- [ ] **Step 5: Rilettura mirata del diff**

Non esiste una prova a runtime per questo task da solo: il teardown resta irraggiungibile finché il Task 2 non toglie l'`exit(0)` dall'handler. La verifica a runtime avviene nelle prove del Task 2, e la prova 1 in particolare esercita proprio questo blocco. Qui la verifica è la rilettura:

- i due flag nuovi vengono alzati **solo** nei rami di successo di `pthread_create`;
- nessun'altra guardia nel file usa ancora `bpechoState.running` o `bundleReceptionState.running` per decidere se fare una join;
- `stopBundleReception` resta chiamata prima della join del thread di ricezione (serve a interrompere la `bp_receive` bloccante).

- [ ] **Step 6: Commit**

```bash
git add dtnex.c
git commit -m "fix(shutdown): la join dei thread di servizio non dipende piu' dal flag di arresto

Le due join in main erano protette dalle stesse variabili che la richiesta di
arresto azzera, quindi al momento del teardown risultavano gia' a zero e le
join venivano saltate. Finora non si notava perche' quel codice non veniva mai
raggiunto: signalHandler chiama exit(0) prima. Introdotti due flag distinti che
registrano la sola creazione del thread."
```

---

## Task 2: Sostituire l'handler asincrono con un thread `sigwait`

**Files:**
- Modify: `dtnex.c` — globali (accanto a quelle del Task 1), nuova funzione `signalWaitThread`, `main` (maschera dei segnali in testa; rimozione del blocco `sigaction` a `dtnex.c:1669-1685`), rimozione di `signalHandler` (`dtnex.c:995-1069`)
- Modify: `dtnex.h` — rimozione della dichiarazione a `dtnex.h:168`
- Modify: `CLAUDE.md` — **su disco soltanto, non committare** (è in `.gitignore`)

**Interfaces:**
- Consumes dal Task 1: `bpechoThreadStarted`, `bundleReceptionThreadStarted` — questo task non li usa, ma non deve toccarli né reintrodurre le vecchie guardie.
- Produces: nessuna interfaccia per task successivi (è l'ultimo task).

Globali e simboli esistenti che questo task usa, con la loro dichiarazione reale:

```c
volatile int running;                          /* dtnex.c:17  */
volatile int ionConnected;                     /* dtnex.c:23  */
BpSAP sap;                                     /* dtnex.c:32  */
BpechoState bpechoState;                       /* dtnex.c:64  — campi: sap, running, attendant */
BundleReceptionState bundleReceptionState;     /* dtnex.c:70  — campi: config, running, thread */
```

- [ ] **Step 1: Dichiarare il thread dei segnali fra le globali**

In `dtnex.c`, accanto ai flag del Task 1:

```c
pthread_t signalThread;    /* Thread dedicato alla raccolta dei segnali via sigwait */
```

- [ ] **Step 2: Scrivere `signalWaitThread`**

Inserirla dove oggi sta `signalHandler` (che il passo 4 rimuove), cosi' il diff resta leggibile:

```c
/**
 * Raccolta dei segnali di terminazione.
 *
 * NON e' un signal handler: i tre segnali sono bloccati in tutti i thread
 * (pthread_sigmask in main) e questo thread li preleva con sigwait(). Gira
 * quindi in contesto ordinario, dove loggare e chiamare le API di ION e'
 * lecito — cosa che in un handler asincrono non lo era: dtnex_log e' printf,
 * e ne' pthread_join ne' bp_close sono async-signal-safe.
 *
 * Al primo segnale abbassa i flag di esecuzione e sveglia chi e' fermo in
 * bp_receive. NON fa join, non chiude endpoint, non fa detach e non chiama
 * exit: la pulizia la fa main quando eventDrivenLoop ritorna, che e' per
 * costruzione un punto fuori da ogni transazione SDR. Uscire di qui mentre un
 * altro thread e' dentro una transazione lascerebbe il lock preso nella
 * memoria condivisa di ION, bloccando ogni altro client fino a ionunlock.
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
            dtnex_log("⚠️  Uscita forzata richiesta: se un thread e' dentro una "
                    "transazione SDR il lock di ION restera' preso e bloccherra' "
                    "gli altri client. In quel caso sbloccare con: ionunlock ion");
            _exit(1);
        }

        if (sig == SIGINT) {
            dtnex_log("Ricevuto SIGINT (Ctrl+C), arresto in corso...");
        } else if (sig == SIGTERM) {
            dtnex_log("Ricevuto SIGTERM, arresto in corso...");
        } else {
            dtnex_log("Ricevuto SIGTSTP (Ctrl+Z), arresto in corso invece della "
                    "sospensione...");
        }

        /* Chiedere l'arresto: il ciclo principale controlla running fra
         * un'iterazione e l'altra, e dorme a fette da un secondo, quindi
         * risponde entro il secondo (dtnex.c, eventDrivenLoop). */
        running = 0;
        bpechoState.running = 0;
        bundleReceptionState.running = 0;

        /* Risvegli: senza questi i thread di servizio resterebbero fermi nella
         * bp_receive bloccante e la join non tornerebbe mai. Condizionati,
         * perche' un segnale puo' arrivare con ION non raggiungibile. */
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
```

Nota sul `_exit(1)`: si usa `_exit` e non `exit` perché con altri thread ancora vivi non si vogliono eseguire gli handler `atexit` né il flush di stdio, che è esattamente il tipo di operazione che può bloccarsi.

Gli header necessari sono già disponibili: `<pthread.h>` (`dtnex.h:17`), `<unistd.h>` (`dtnex.h:21` e `dtnex.c:14`), `<signal.h>` (`dtnex.h:23`). Non aggiungere include.

- [ ] **Step 3: Bloccare i segnali e creare il thread, in testa a `main`**

La maschera va impostata **prima di qualunque cosa possa creare thread**, inclusa `init()` che si attacca a ION e le librerie ION stesse: un thread eredita la maschera del thread che lo crea, quindi impostarla dopo lascerebbe scoperti i thread già nati.

Rimuovere il blocco `sigaction` esistente a `dtnex.c:1669-1685` (le tre `sigaction`, la `struct sigaction sa`, la `sigset_t mask` e i relativi commenti) e mettere all'inizio del corpo di `main`, prima di `original_argc = argc;`:

```c
    /* I segnali di terminazione vanno bloccati PRIMA che nasca qualunque
     * thread — inclusi quelli che ION puo' creare in bp_attach — perche' la
     * maschera si eredita alla creazione. Da qui in poi nessun thread li
     * riceve in modo asincrono: li raccoglie signalWaitThread con sigwait. */
    sigset_t terminationSignals;

    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGINT);
    sigaddset(&terminationSignals, SIGTERM);
    sigaddset(&terminationSignals, SIGTSTP);

    if (pthread_sigmask(SIG_BLOCK, &terminationSignals, NULL) != 0) {
        dtnex_log("❌ Impossibile bloccare i segnali di terminazione: "
                "l'arresto non sarebbe sicuro per ION, esco");
        return 1;
    }

    if (pthread_create(&signalThread, NULL, signalWaitThread, NULL) != 0) {
        dtnex_log("❌ Impossibile creare il thread dei segnali: "
                "l'arresto non sarebbe sicuro per ION, esco");
        return 1;
    }
```

Il thread dei segnali non viene raccolto con join in uscita: resta bloccato in `sigwait` e muore con il processo quando `main` ritorna. Non tiene risorse di ION.

- [ ] **Step 4: Rimuovere `signalHandler` e la sua dichiarazione**

- In `dtnex.c`: eliminare l'intera funzione `signalHandler` (`dtnex.c:995-1069`), commento di intestazione incluso.
- In `dtnex.h`: eliminare la riga `void signalHandler(int sig);` (`dtnex.h:168`).
- Verificare con `grep -n "signalHandler\|isignal" dtnex.c dtnex.h` che non resti nessun riferimento. Le tre `isignal(...)` vivevano solo dentro l'handler e spariscono con lui.

- [ ] **Step 5: Correggere i commenti che il cambiamento rende falsi**

Due commenti descrivono il meccanismo vecchio e diventerebbero bugie:

- `dtnex.c:19-20`, sopra `running`: dice che la variabile «viene modificata dal signal handler quando arriva un segnale (SIGINT/SIGTERM)». Riscriverlo dicendo che a modificarla è `signalWaitThread`, e che `volatile` serve perché il cambiamento arriva da un altro thread.
- `dtnex.c:1411`: «Don't set separate signal handler for bpecho - use main process handler». Non esiste più un handler di processo. Riscriverlo dicendo che il thread bpecho non tocca i segnali perché li ha bloccati per eredità dalla maschera di `main`, e che si ferma tramite `bpechoState.running` più il risveglio di `bp_interrupt`.

Con un `grep -n "signal handler\|signal hadler\|handler" dtnex.c` verificare che non resti altro testo che descriva il meccanismo rimosso.

- [ ] **Step 6: Build pulita**

Run: `./build_standalone.sh`
Expected: nessun errore, nessun warning nuovo sulle righe toccate.

- [ ] **Step 7: Aggiornare `CLAUDE.md` su disco**

Nella sezione «Threading model», che oggi dice `main loop + 2 service threads`, portare il conteggio a 3 e aggiungere la riga del thread nuovo accanto alle due esistenti, per esempio:

```
- `signalWaitThread()` — raccoglie SIGINT/SIGTERM/SIGTSTP con `sigwait`; abbassa i
  flag di esecuzione e sveglia i servizi. La pulizia la fa `main`: nessuna uscita
  avviene dentro una transazione SDR.
```

**Non fare `git add` di `CLAUDE.md`**: è in `.gitignore` dal commit `ab2f973` e non deve entrare nei commit.

- [ ] **Step 8: Commit**

```bash
git add dtnex.c dtnex.h
git commit -m "fix(shutdown): raccolta dei segnali con sigwait invece di un handler asincrono

signalHandler chiamava exit(0) da contesto asincrono: se il segnale arrivava
mentre un thread era dentro una transazione SDR, il processo moriva con la
transazione aperta e il lock — che vive nella memoria condivisa di ION —
restava preso, bloccando ogni altro client fino a ionunlock.

I tre segnali sono ora bloccati in tutti i thread e raccolti da un thread
dedicato con sigwait, che si limita ad abbassare i flag e a svegliare i
servizi. La pulizia resta in main, raggiunta quando eventDrivenLoop ritorna:
un punto per costruzione fuori da ogni transazione. Cade anche l'uso di
printf, pthread_join e bp_close da contesto asincrono, che non sono
async-signal-safe. Il secondo segnale forza ancora l'uscita, ma dicendo
esplicitamente che puo' servire ionunlock."
```

- [ ] **Step 9: Prove manuali su ION vivo — prova 1**

Prerequisito: ION in esecuzione (`pgrep -x ipnfw` deve dare un pid). Rileggere le «Regole operative su ION» in testa a questo piano prima di iniziare.

1. Avviare dtnex: `./dtnex --debug` in un terminale.
2. Attendere la riga `🔄 Starting event-driven operation`.
3. Da un altro terminale: `pkill -x -TERM dtnex`.

Expected: dtnex logga la ricezione di SIGTERM, poi `Waiting for bundle reception thread to terminate...`, `Waiting for bpecho service to terminate...`, la chiusura dell'endpoint, il detach, e infine `DTNEXC terminated normally` — che è la riga finale di `main` e **oggi non compare mai**. Il processo esce entro pochi secondi.

Subito dopo, in background: `{ echo "l contact"; } | ionadmin`. Deve rispondere senza attese.

- [ ] **Step 10: Prove manuali — prova 2 (quella che discrimina)**

È l'unica prova che mette il segnale e una transazione nella stessa finestra temporale.

1. Avviare dtnex e attendere che sia pronto.
2. Iniettare un contatto per una coppia sintetica, estranea al piano reale del nodo:

```bash
T0=$(date +%s)
dev/inject_contact.py --from-node 7 --to-node 8 \
    --from-time $((T0+3600)) --to-time $((T0+7200)) \
    --xmit-rate 10000 --confidence 100 --owlt 3 \
    --origin 7 --from-node-hdr 7 --target-node-id 5 \
    --own-eid ipn:5.1 --dest-eid ipn:5.12160
```

`--own-eid` deve essere un endpoint locale libero e **diverso** da quello che dtnex tiene aperto (`ipn:5.12160`), altrimenti `bpsendfile` non riesce ad aprire il proprio SAP. Lo script lascia di proposito il file del payload su disco: non cancellarlo, ION lo rilegge quando serializza il bundle e senza il file va in `Unrecoverable SDR error`.

3. **Mentre dtnex sta applicando il contatto** — cioè entro un paio di secondi dall'iniezione, quando nel log compaiono le righe `[ion]` — inviare `pkill -x -TERM dtnex`.

A fine prova, ripulire la coppia sintetica: `echo "d contact * 7 8" | ionadmin` (in background, senza `timeout`).

Expected: uscita pulita come nella prova 1, e `ionadmin` che risponde subito dopo senza bisogno di `ionunlock`. Ripetere due o tre volte variando l'istante del segnale, perché la finestra è stretta.

- [ ] **Step 11: Prove manuali — prove 3 e 4**

**Prova 3 (uscita forzata):** avviare dtnex, inviare due SIGTERM ravvicinati (`pkill -x -TERM dtnex; sleep 0.2; pkill -x -TERM dtnex`). Expected: compare l'avviso di uscita forzata con il suggerimento `ionunlock ion`, e il processo esce immediatamente.

**Prova 4 (modalità servizio):** impostare `serviceMode=true` in `dtnex.conf`, avviare dtnex, inviare un SIGTERM. Expected: stesso esito della prova 1.

- [ ] **Step 12: Registrare l'esito**

Aggiornare la tabella della spec §13.8 (`docs/superpowers/specs/2026-08-03-dtnex-contact-exchange-redesign-design.md`) con l'esito effettivo di ciascuna prova, e cambiare l'intestazione del documento, che oggi dice che il §13 è «progettato ma non ancora implementato».

Aggiornare anche `docs/superpowers/2026-08-06-stato-redesign-e-lavoro-residuo.md`: il §5 elenca il blocco di `ionadmin` fra le cose «da guardare prima del merge, non spiegate». Se le prove 1 e 2 passano, quella voce si chiude con il riferimento a questo lavoro; se il blocco si ripresenta, va detto, perché significherebbe che la causa era un'altra.

```bash
git add -f docs/superpowers/specs/2026-08-03-dtnex-contact-exchange-redesign-design.md docs/superpowers/2026-08-06-stato-redesign-e-lavoro-residuo.md
git commit -m "docs: esito delle prove di terminazione cooperativa"
```

Il `-f` serve perché `docs/superpowers/` è in `.gitignore`; i due file sono però già tracciati e devono restare versionati.

---

## Note per chi esegue

**Perché i due task sono separati.** Il Task 1 corregge un difetto che esiste indipendentemente dal cambio dei segnali e che un revisore può approvare o respingere per conto suo. Il Task 2 rende raggiungibile quel codice. Invertirli, o fonderli, significherebbe attivare il teardown con le join rotte.

**Cosa questo lavoro non fa.** Non protegge da segfault, abort o SIGKILL: lì il processo muore senza eseguire niente e, se una transazione era aperta, il lock resta preso. È una scelta registrata nella spec §13.7, non una dimenticanza. Il rimedio documentato resta `ionunlock ion`.

**Un'onestà da mantenere.** Il nesso fra l'`exit(0)` asincrono e i blocchi di `ionadmin` osservati il 2026-08-08 è un'ipotesi coerente, non una dimostrazione: l'istante del segnale non è stato catturato. Se dopo questo lavoro il blocco si ripresenta, il difetto corretto qui era reale ma non era la causa, e va cercata altrove. Non dare per chiusa la voce del §5 senza aver eseguito davvero le prove 1 e 2.
