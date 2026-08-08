# Redesign dello scambio di contatti — stato e lavoro residuo

**Data:** 2026-08-06
**Branch:** `redesign` — **NON fuso in `main`, per scelta.** Il merge si fa piu' avanti,
quando il resto della repo sara' sistemato.
**Ultimo commit del redesign:** `bccb15b`
**Base del lavoro:** `956f621` (commit del piano)

Documenti collegati:
- design: `docs/superpowers/specs/2026-08-03-dtnex-contact-exchange-redesign-design.md`
- piano: `docs/superpowers/plans/2026-08-06-dtnex-contact-exchange-redesign.md`

---

## 1. Cosa e' stato fatto

Undici commit, sei task piu' l'ondata di fix finale, ognuno passato per un review
dedicato:

| commit | contenuto |
|---|---|
| `6815851` | modulo `ion_contacts` — lettura dei contatti annunciabili (walk RBT + join owlt) |
| `c4fcb2e` | protocollo v3: payload a 7 campi, tempi assoluti, origination dallo snapshot |
| `41d2825` | scrittura idempotente con rimozione mirata per `fromTime` |
| `dacae2d` | correzioni al Task 3: scarti di policy, buchi di validazione, inoltro |
| `6f20a94` | scorporo di `getContacts`, rilevazione esplicita del restart |
| `68b5e81` | mutex su `getplanlist` |
| `42ec416` `5debba9` `a023ef3` | documentazione allineata alla v3 |
| `4cd080e` | ondata finale FIX 1-5: separazione errori di sistema / errori utente di ION |
| `bccb15b` | ondata finale FIX 6-7: documentazione e correzioni alla spec |

I tre difetti che il redesign doveva chiudere sono chiusi alla radice, verificato dal
review finale leggendo il sorgente reale di ION:

1. il contatto annunciato ora e' letto da ION campo per campo, e il ricevente non
   riscrive piu' la finestra temporale;
2. `owlt`, `xmitRate` e `confidence` viaggiano sul filo con i valori reali; nessun
   valore hardcoded sopravvive;
3. ogni `rfx_remove_*` passa il `fromTime` esatto: i contatti configurati
   dall'operatore non vengono piu' cancellati.

Verificato inoltre: il round-trip campo per campo e' esatto (inclusa la conversione
`confidence` float↔percentuale, che e' reversibile senza deriva), l'idempotenza regge
davvero — un riannuncio invariato produce zero scritture in ION — e il confine del
modulo `ion_contacts` ha tenuto per tutti e sei i task.

---

## 2. Regressione dell'ondata finale — CORRETTA il 2026-08-08 (commit `c54a8cb`)

**Chiusa.** L'opzione 1 e' stata applicata ai due soli siti post-remove: log con
`dtnex_log` invece di `noteUserError`, nuovo esito `IONC_LOST` fra `IONC_REPLACED`
e `IONC_ERROR`, e un ramo dedicato in `processCborContactMessage` che non stampa
piu' un falso `✅`. Gli altri cinque siti restano come li aveva lasciati il FIX 1.
Inoltro invariato (§6.5). Build pulita; review del diff passato (spec ✅, qualita'
approvata). Resta da eseguire la **prova 4** del §5, che e' quella che esercita
davvero questo percorso con un ION vivo.

Il resto di questa sezione descrive il difetto com'era, per riferimento.

**Decisione gia' presa: si applica l'opzione 1 descritta qui sotto.**

Il FIX 1 ha declassato tutti gli errori utente di ION (`rc > 0`) a condizione attesa,
loggata solo in `debugMode`. Corretto per cinque dei sette punti di scrittura, ma **due
sono qualitativamente diversi**: quelli dove la insert segue una remove riuscita.

**Siti:**
- `ion_contacts.c:500-504` — ramo `rc > 0` di `rfx_insert_contact (dopo remove)`
- `ion_contacts.c:581-585` — ramo `rc > 0` di `rfx_insert_range (dopo remove)`

**Il difetto.** Se `rfx_remove_contact` restituisce 0 (rimozione avvenuta) e la
`rfx_insert_contact` successiva viene rifiutata con codice 9 — la nuova finestra si
sovrappone a un altro contatto locale della stessa coppia — allora la voce vecchia e'
sparita e la nuova non e' stata scritta. `contactOutcome` resta `IONC_NOOP`, quindi
`noteUserError` logga solo in debug e il chiamante (`dtnex.c:3266`,
`else if (outcome != IONC_NOOP)`) non stampa nulla. Il ciclo successivo prende il ramo
`!haveContact`, viene rifiutato di nuovo con 9, e cosi' via: **la perdita e' permanente
e invisibile al livello di log predefinito.** Prima dell'ondata lo stesso caso produceva
un `⚠️ Anomalia` rumoroso a ogni ciclo.

La differenza con gli altri cinque siti: una insert rifiutata a secco lascia ION
invariato, una insert rifiutata dopo una remove riuscita lascia ION **peggiorato**.

**Correzione da applicare (opzione 1).** Solo ai due siti post-remove:
- loggare a livello non-debug (`dtnex_log`, non `noteUserError`), perche' e' un'anomalia
  vera e non una condizione attesa;
- far si' che l'esito lo dica, invece di restare `IONC_NOOP` — la voce e' stata rimossa
  e non rimpiazzata, e chi legge il log deve poterlo capire;
- lasciare intatti gli altri cinque siti, che il FIX 1 ha sistemato correttamente.

Poi: build, e un re-review ristretto a quel diff.

---

## 3. Minor residui — CHIUSI il 2026-08-08 (commit `c54a8cb`, `30effd5`, `8dc1f8f`)

Tutte le voci di questa sezione sono state chiuse. Restano elencate per memoria di
cosa c'era e di come e' stato deciso.

- ~~Il commento che motivava la rimozione del dump di `iondb.contacts` diceva il falso
  ("stampava lo stesso valore della precedente sotto un altro nome": i due dump
  leggevano campi diversi).~~ Commento riscritto in `30effd5` con la ragione vera: nel
  layout installato `IonRegion` e' 16 byte e non 8, quindi gli offset dei campi dopo di
  esso slittano.
- ~~La riga di dump sopravvissuta stampava `iondb.ranges`, che nel layout installato e'
  `cpsNotices`.~~ Riga rimossa in `30effd5`: si stampa solo `ownNodeNbr`, primo campo
  della struct e unico offset che coincide nei due layout.
- ~~`dtnex.c:3260-3261` — commento stantio su `IONC_ERROR`.~~ Corretto in `c54a8cb`
  insieme alla regressione del §2.
- `checkRejectAddr` riceve `(rc == 1 || rc == 2)` mentre `rc == 1` e' gia' consumato dal
  ramo sopra. **Lasciato com'e', deliberatamente:** l'espressione documenta l'intero
  contratto di `rfx_insert_range` descritto nel commento della funzione e resta corretta
  se un domani il ramo `rc == 1` venisse spostato.
- ~~`CLAUDE.md` etichettava i due tipi di messaggio come "Type 1" / "Type 2" interi.~~
  Allineato su disco al valore reale del campo `type`, che e' una stringa (`"c"` / `"m"`).
  (`CLAUDE.md` e' in `.gitignore` dal commit `ab2f973`: non entra nei commit.)
- ~~Spec §10 elencava come da fare due correzioni a `CLAUDE.md` gia' applicate.~~ §10
  riscritta al passato in `8dc1f8f`, con l'aggiunta della terza correzione (etichette
  `type`).
- ~~`sdr_read(sdr, (char *) &iondb, iondbObject, sizeof(IonDB))` leggeva oltre la fine
  dell'oggetto in SDR.~~ Corretto in `30effd5` nei due siti (`tryConnectToIon` e
  `ionc_check_alive`): si legge il solo campo `ownNodeNbr`, con
  `offsetof(IonDB, ownNodeNbr)` come offset e la `sizeof` del campo come lunghezza.
  Il rischio §12.3 della spec resta aperto per gli altri usi degli header.
- ~~`contactTimeTolerance` era un parametro morto.~~ **Rimosso** in `30effd5` (campo,
  default, ramo del parser, `dtnex.conf`) e documentato in `8dc1f8f`: il parser ignora
  le chiavi sconosciute, quindi un `dtnex.conf` gia' installato che la contiene ancora
  continua a funzionare.

Un solo minor nuovo, cosmetico e non tracciato altrove: l'etichetta di log
`"[tryConnectToIon] IonDB dump after sdr_read:"` parla di "dump" per una riga sola.

## 4. Limiti di progetto aperti

Registrati in §12 della spec, richiedono decisioni di design, non solo codice:

- **§12.1 — la rilevazione del restart di ION non copre il caso comune.** Un ciclo
  `ionstop && ionstart` conserva `ownNodeNbr`, quindi il controllo attuale lo considera
  "vivo e coerente". Serve un marcatore d'istanza.
- **§12.2 — il join simmetrico dei range indebolisce §3.4.** Un contatto locale privo di
  range puo' essere annunciato usando un OWLT imparato da un peer, quindi un errore di
  configurazione in ionrc puo' venire coperto invece che reso visibile. Verificato che
  non produce churn ne' oscillazione.
- **§12.3 — gli header in `include/ion/` non corrispondono alla libreria installata.**
  I layout da cui il branch dipende coincidono (verificato), ma il commit `87590d4`
  esiste proprio perche' una di queste divergenze aveva corrotto le letture RBT.
  Mitigazione proposta: `_Static_assert` su `sizeof(IonCXref)`,
  `offsetof(IonCXref, fromTime)` e `sizeof(IonVdb)`.

---

## 5. Validazione — ESEGUITA il 2026-08-08 su ION vivo (nodo 5)

**Cinque prove su sei passate; la sesta non e' eseguibile su un nodo solo.**

Ambiente: nodo ION 5 (`hostiondtn2.rc`), contatti 5↔2, 5→6, 5↔5; range solo per
2↔5 e 5↔5 — il contatto 5→6 e' privo di range perche' la riga `a range ... 5 6 1`
e' commentata nell'ionrc. Vicino secondo il piano BP: solo il nodo 2, irraggiungibile.

Con un nodo solo il percorso di ricezione non si esercita da se': i messaggi si sono
iniettati costruendo il bundle a mano con uno script Python che replica byte per byte
`encodeCborContactMessage` (dtnex.c:1939-1976) e `calculateHmac`, consegnato con
`bpsendfile`. Il decoder li ha accettati con HMAC valido al primo colpo.

| # | esito | evidenza |
|---|---|---|
| 1 | ✅ | snapshot `5→2 from=1786171309 to=1786207308 xmitRate=10000000 B/s conf=100% owlt=1s` contro ionadmin `06:41:49→16:41:48, 10000000 bytes/sec, confidence 1.000000, OWLT 1`: i sette campi coincidono, conversione epoch verificata |
| 2 | ✅ (meta' ricevente) | iniettato `7→8 from=1786186288 to=1786189888 xmitRate=54321 conf=77 owlt=7`; in ION: `54321 bytes/sec, confidence 0.770000`, finestra identica, **nessuna riscrittura dello start time** |
| 3 | ✅ | `Contatto 5→6 (from ...) senza range: non annunciato — controllare ionrc`, e 5→6 non compare fra i contatti annunciabili |
| 3b | ✅ | `owlt=7` iniettato → in ION `OWLT from node 7 to node 8 is 7 seconds`, non 1 |
| 4 | ✅ | contatto manuale 7→8 `[08:51:28..09:51:28]` messo via ionadmin; iniettato 7→8 con `fromTime` diverso `[10:51:28..11:51:28]`: **dopo, ION contiene entrambi**. Problema 1.3 chiuso sul campo |
| 5 | ✅ | reiniettato lo stesso contatto (nonce diverso): `[ion] 7→8 ...: contatto=no-op range=no-op`, zero scritture. Osservato due volte |
| L | ✅ | vedi sotto |
| 6 | non eseguibile | serve un testbed a tre nodi; qui c'e' un solo ION |

### Prova L — il fix del §2 verificato sul campo

Costruita apposta: ION con due contatti 7→8 (W1 manuale, W3 iniettato, non
sovrapposti); iniettato un messaggio con `fromTime` di W1 e `toTime` esteso dentro W3.
La remove riesce, la insert viene rifiutata con codice 9. Log prodotto:

```
⚠️  Anomalia: rfx_insert_contact (dopo remove) 7→8 (from 1786179088) rifiutato con
   codice 9 — si sovrappone a un contatto configurato localmente; si mantiene quello
   locale: il contatto precedente e' stato rimosso e non sostituito
[ion] 7→8 from=1786179088 to=1786186888: contatto=perso range=no-op
⚠️  Contatto 7→8 perso in ION dopo una remove riuscita (insert successiva rifiutata)
```

Entrambe le righe `⚠️` sono a livello non-debug ed esito `perso`, non `no-op`: e'
esattamente il comportamento che `c54a8cb` doveva produrre. Prima del fix: silenzio.

### Due cose emerse durante le prove, da guardare separatamente

- **`ionadmin` si blocca mentre dtnex e' in esecuzione. APERTO, e ora circoscritto.**
  Misurato il 2026-08-08 due volte, la seconda con la terminazione cooperativa gia'
  implementata (§13 della spec): `ionadmin` bloccato 2 minuti e 13 secondi senza mai
  completare, sbloccato 1,0 secondi dopo il SIGTERM a dtnex, mentre a dtnex fermo le
  stesse interrogazioni tornavano in 8-10 ms.

  **Cosa NON e': la terminazione.** Il lavoro sulla terminazione e' stato fatto e
  verificato, ma non tocca questo sintomo — il blocco avviene a dtnex vivo e
  inattivo. L'ipotesi del §13.1 della spec, che attribuiva anche questo all'`exit(0)`
  asincrono, e' smentita.

  **E non e' nemmeno detto che sia dtnex.** Misure successive, stesso giorno e nodo
  appena riavviato, hanno mostrato `ionadmin` bloccato oltre dieci minuti **con dtnex
  spento**, per poi completare da solo. Il nodo ha stalli pluriminuto propri, quindi
  "bloccato mentre dtnex gira" non basta a incolpare dtnex.

  **Come riprenderla.** Serve una base di misure pulite, una variabile per volta:
  nodo nudo, poi solo `bprecvfile`, poi solo dtnex, poi entrambi. L'ipotesi da testare
  per prima e' un thread parcheggiato in `bp_receive` che trattiene un lock di ION —
  forma comune ai thread di servizio di dtnex e al `bprecvfile` dell'operatore.
  Tabella di cosa e' stabilito e cosa no in §13.9 della spec.

  **Avvertenza:** uccidere `ionadmin` mentre attende blocca il nodo per davvero e
  falsa ogni misura successiva. Va lanciato solo dove nessun timeout lo interrompa.
- **`bpsendfile` con il file del payload cancellato subito dopo l'invio manda ION in
  `Unrecoverable SDR error`** (`Can't compute payload block CRC` → `Can't serialize
  bundle payload`). ION serializza il payload dopo, rileggendo il file. E' un limite
  dello strumento di prova, non di dtnex, ma blocca il nodo: annotato nello script.

### Cosa resta non verificato

La prova 6 e la meta' "mittente" della prova 2 — cioe' che un secondo nodo ION legga e
riannunci senza deriva — richiedono un testbed multi-nodo. Il resto del rischio
elencato qui sotto e' chiuso.

Ogni task era gia' stato verificato con una build pulita piu' la rilettura del codice,
piu' il controllo del sorgente reale di ION per la semantica delle `rfx_*`.

Il percorso di encode/decode, le larghezze dei campi CBOR, il round-trip di
`confidence`, l'invariante della cache e il confine del modulo erano gia' verificabili
leggendo, ed erano gia' stati verificati. Le prove del 2026-08-08 hanno chiuso la
parte che dipendeva da come ION risponde davvero.

---

## 6. Dove riprendere

1. ~~Applicare la correzione del §2.~~ Fatto il 2026-08-08, commit `c54a8cb`.
2. ~~Sistemare i minor del §3.~~ Fatto il 2026-08-08, commit `30effd5` e `8dc1f8f`.
3. ~~Con un ION vivo, eseguire le prove del §5.~~ Fatto il 2026-08-08: cinque prove
   su sei passate, la sesta non eseguibile su un nodo solo.
4. **Da guardare prima del merge, e non sono difetti del redesign:** il blocco di
   `ionadmin` mentre dtnex gira (§5, non spiegato) e i limiti di progetto del §4.
5. Poi, valutare il merge di `redesign` in `main`.

Il ledger completo dell'esecuzione, con i report di ogni task e di ogni review, e' in
`.superpowers/sdd/2026-08-06-dtnex-contact-exchange-redesign/` — **directory ignorata da
git**, quindi un `git clean -fdx` la distrugge. Questo documento e' il riassunto che
sopravvive.
