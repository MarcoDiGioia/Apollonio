# Apollonio + Artemide: Stato di Avanzamento (Work in Progress)

> **Nota per sviluppatori e agenti AI**: Questo file traccia in tempo reale lo stato dei lavori, le decisioni tecniche approvate e i prossimi passi esatti. Consultare sempre [DEVELOPMENT_PLAN.md](file:///home/antigravity/DEVELOPMENT_PLAN.md) per l'architettura generale e le specifiche di dettaglio.

---

## 1. Stato Attuale del Progetto

- **Fase Attiva**: **Fase 0 - Analisi Preliminare & Spike di Validazione**
- **Data Ultimo Aggiornamento**: 2026-11-06
- **Stato Generale**: Architettura di sistema approvata al 100%. Nessun codice in produzione è stato ancora modificato nei branch master. È stato creato e compilato il tool diagnostico standalone per la validazione host (`shell_probe.exe`). In corso: analisi dettagliata della parte client (Artemide / Tizen).

---

## 2. Decisioni Architetturali Approvate (Lock In)

0. **Upstream Git e Base Codice Host (LizardByte/Sunshine)**:
   - `Apollonio` è configurato con remote `upstream` puntato direttamente al repository ufficiale di **LizardByte/Sunshine** (`https://github.com/LizardByte/Sunshine.git`).
   - Manteniamo la massima aderenza agli aggiornamenti di driver video (NVIDIA/AMD/Intel) e codec ufficiali, arricchendo Sunshine con i tre moduli proprietari: Virtual Display HDR (`IddSampleDriver`), Window Supervisor & Backdrop Kiosk (`apollonio-shell.exe`) e Control Protocol esteso `0x31xx`.

1. **Topologia del Supervisor (`apollonio-shell.exe`)**:
   - Processo helper dedicato C++ residente nello stesso repo `Apollonio` (nella cartella `tools/`).
   - Eseguito nella **Sessione Utente attiva** (non come servizio SYSTEM) per interagire correttamente con le finestre Win32 (`SetForegroundWindow`, Z-order) senza incorrere nelle restrizioni UIPI di Windows.
   - Comunica con il servizio principale di Apollonio tramite una Named Pipe locale: `\\.\pipe\ApollonioShellPipe`.

2. **Nero Assoluto & Z-Order Management**:
   - Finestra hardware a tutto schermo (`Backdrop Kiosk`, nera `#000000`) posizionata sul display virtuale di Apollonio.
   - Ordine Z rigoroso:
     ```
     [1. Gioco attivo]             -> In primo piano / focus esclusivo
     [2. Base App (Big Picture)]    -> Subito sotto il gioco, mai chiusa
     [3. Backdrop Nero]            -> Schermo hardware nero
     [4. Launcher & Desktop]       -> Forzati SOTTO il backdrop (100% invisibili)
     ```

3. **Modello Base App Flessibile (`BaseAppProvider`)**:
   - Astrazione C++ modulare.
   - Implementazione primaria (Fase 1): **Steam Big Picture** (rilevamento tramite `RunningAppID`, avvio giochi tramite `steam://rungameid/<id>`).
   - Predisposto per future integrazioni con **Playnite Fullscreen** e **Xbox Console Mode**.

4. **Pausa & Audio Ducking (Niente Freeze)**:
   - Scartato `NtSuspendProcess` per eliminare qualsiasi rischio di ban anti-cheat (Easy Anti-Cheat, BattlEye), crash DirectX 12 e disconnessioni socket multiplayer.
   - Si sfrutta l'evento naturale di **Lost Focus (`WM_KILLFOCUS`)** indotto dallo spostamento del focus, unito all'**Audio Ducking WASAPI** lato server (volume gioco attenuato al 15% quando Drawer o Home sono aperti).

5. **Gestione Desktop Config di Steam Input**:
   - Gestita in modo completamente automatico dal Supervisor all'avvio della sessione streaming (impostando un profilo passthrough privo di azioni mouse) per evitare conflitti con l'emulazione del client. Ripristinata alla chiusura.

6. **Trigger Drawer Client (`Back + Start` su Tizen)**:
   - Il tasto Guida/Home è intercettato dal sistema operativo della TV Samsung (Smart Hub) e non è disponibile per l'app.
   - La combinazione scelta è: **Back + Start premuti simultaneamente per 800 ms**.
   - Dotata di **Filtro Anti-Falsi Positivi con Grace Period di 80 ms** in `wasm/gamepad.cpp` per trattenere il primo tasto premuto ed evitare aperture spurie di menu nel gioco.

7. **Tastiera a Schermo (Overlay Nativo Artemide)**:
   - Overlay HTML/JS nativo sul client, navigabile con il controller e azionato tramite pacchetti Moonlight `LiSendKeyboardEvent`. Indipendente dal gioco e da Steam Overlay.

8. **Schermata Iniziale Artemide**:
   - Viene mantenuta la griglia delle app, aggiungendo un'opzione *"Avvia automaticamente all'avvio dello stream"* per permettere l'ingresso diretto a schermo intero nella Base App.

---

## 3. Elementi Realizzati / Pronti

| Elemento | Percorso File | Stato | Descrizione |
|---|---|---|---|
| **Documento di Design** | `/home/antigravity/.gemini/.../console_shell_design.md` | ✅ Completato | Specifica iniziale dettagliata |
| **Piano di Sviluppo** | [DEVELOPMENT_PLAN.md](file:///home/antigravity/DEVELOPMENT_PLAN.md) | ✅ Completato | Piano esecutivo completo di tutte le fasi |
| **Tool Diagnostico Host** | [shell_probe.cpp](file:///home/antigravity/MarcoDiGioia/Apollonio/tools/shell_probe.cpp) | ✅ Compilato | Strumento standalone Windows C++20 per validare monitor, Z-order, Big Picture e switch di focus |
| **Binario shell_probe.exe** | `/tmp/probe/shell_probe.exe` | ✅ Pronto per test | Eseguibile Windows a 64-bit standalone cross-compilato con Zig |

---

## 4. Prossimi Passi Immediati

1. **Continuare l'analisi dettagliata della parte client (Artemide / Tizen)**:
   - Esaminare la gestione del ciclo di vita dei gamepad in `wasm/gamepad.cpp` e `wasm/platform/gamepad.js`.
   - Analizzare l'architettura del layout DOM e della navigazione da telecomando/controller in `wasm/platform/navigation.js` per pianificare l'inserimento non invasivo del Drawer.
   - Definire l'interfaccia esatta tra WASM (C++) e JS (`messages.js`) per lo scambio bidirezionale dei dati shell.
2. **Definire la procedura di test per la Fase 0**:
   - Istruzioni per l'utente per l'esecuzione di `shell_probe.exe` sul PC gaming Windows.
   - Test di cattura dell'input del controller collegato alla Smart TV.
