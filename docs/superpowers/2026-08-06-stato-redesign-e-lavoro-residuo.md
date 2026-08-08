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

## 3. Minor residui, non bloccanti

- `dtnex.c:393-395` — il commento che motiva la rimozione del dump di `iondb.contacts`
  dice che quella riga "stampava lo stesso valore della precedente sotto un altro nome".
  Falso: i due dump leggevano campi diversi. Nel layout installato `IonRegion` e' 16 byte
  (non 8), quindi `ranges` sta a offset 48 e `contacts` a 56 nel bundle, mentre
  nell'installato a 48 c'e' `cpsNotices` e a 56 `ranges`.
- `dtnex.c:392` — la riga di dump sopravvissuta stampa `iondb.ranges`, che nel layout
  installato e' `cpsNotices`. Il FIX 5.2 ha tolto la riga sbagliata indicata dal brief e
  ne ha lasciata una altrettanto sbagliata. Impatto limitato al debug.
- ~~`dtnex.c:3260-3261` — commento stantio su `IONC_ERROR`.~~ Corretto in `c54a8cb`
  insieme alla regressione del §2.
- `ion_contacts.c:550` e `585` — l'argomento `(rc == 1 || rc == 2)` passato a
  `checkRejectAddr`: `rc == 1` e' gia' consumato dal ramo sopra, quindi solo `2` arriva
  fin li'. Innocuo, documenta l'intento.
- `CLAUDE.md:63-66` — etichetta ancora i due tipi di messaggio come "Type 1" / "Type 2"
  interi, la stessa incoerenza che il FIX 6 ha tolto dal `README.md`. La descrizione del
  payload e' gia' corretta. (`CLAUDE.md` e' in `.gitignore` dal commit `ab2f973`: si
  aggiorna su disco, non entra nei commit.)
- Spec §10 elenca ancora come da fare due correzioni a `CLAUDE.md` che sono gia' state
  applicate.
- `sdr_read(sdr, (char *) &iondb, iondbObject, sizeof(IonDB))` in `tryConnectToIon` e in
  `ionc_check_alive` usa la `sizeof(IonDB)` del bundle, piu' grande della struct
  installata: e' un over-read oltre la fine dell'oggetto in SDR. Innocuo oggi perche'
  entrambi i chiamanti leggono solo `ownNodeNbr`, che sta a offset 0. E' il rischio
  registrato in §12.3 della spec.
- `contactTimeTolerance` e' ora un parametro morto: letto in configurazione
  (`dtnex.c:207`, `269-270`) e mai usato in nessun calcolo. Va deciso se rimuoverlo o
  dargli un uso.

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

## 5. Validazione mai eseguita

**Nessuna delle sei prove di §9 della spec e' stata eseguita: ION non era in esecuzione
sulla macchina di sviluppo.** Ogni task e' stato verificato con una build pulita piu' la
rilettura del codice, piu' il controllo del sorgente reale di ION per la semantica delle
`rfx_*`.

Cosa questo **non** lascia in dubbio: il percorso di encode/decode, le larghezze dei
campi CBOR, il round-trip di `confidence`, l'invariante della cache, il confine del
modulo. Sono verificabili leggendo, e sono stati verificati.

Cosa lascia in dubbio: tutto cio' che dipende da come ION risponde davvero. In ordine di
valore diagnostico, quando ci sara' un ION vivo:

1. **prova 4** (contatto manuale via ionadmin, poi un messaggio per la stessa coppia con
   `fromTime` diverso) — il contatto manuale deve sopravvivere, ed e' anche la prova che
   esercita direttamente il percorso della regressione descritta al §2 di questo
   documento;
2. **prova 2** (due nodi, finestra identica sui due capi) — in un ionrc convenzionale con
   entrambe le direzioni e' la piu' rapida da far fallire se la gestione del codice 9 non
   e' giusta;
3. **prova 5** (anti-churn: due cicli senza cambiamenti, zero scritture) — attesa in
   successo, verificata analiticamente;
4. prove 1, 3, 6 per il resto.

---

## 6. Dove riprendere

1. ~~Applicare la correzione del §2.~~ Fatto il 2026-08-08, commit `c54a8cb`.
2. Sistemare i minor del §3 che vale la pena chiudere.
3. Con un ION vivo, eseguire le prove nell'ordine del §5.
4. Solo dopo, valutare il merge di `redesign` in `main`.

Il ledger completo dell'esecuzione, con i report di ogni task e di ogni review, e' in
`.superpowers/sdd/2026-08-06-dtnex-contact-exchange-redesign/` — **directory ignorata da
git**, quindi un `git clean -fdx` la distrugge. Questo documento e' il riassunto che
sopravvive.
