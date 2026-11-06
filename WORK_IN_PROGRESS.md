# Apollonio + Artemide: Stato di Avanzamento (Work in Progress)

> **Nota per sviluppatori e agenti AI**: Questo file traccia in tempo reale lo stato dei lavori, le decisioni tecniche approvate e i prossimi passi esatti. Consultare sempre [DEVELOPMENT_PLAN.md](file:///home/antigravity/DEVELOPMENT_PLAN.md) per l'architettura generale e le specifiche di dettaglio.

---

## 1. Stato Attuale del Progetto

- **Fase Attiva**: **Fase 0 - Analisi Preliminare & Spike di Validazione**
- **Data Ultimo Aggiornamento**: 2026-11-06
- **Topologia di Sviluppo**:
  - **Macchina di Sviluppo**: **Linux** (workstation/laptop per scrittura codice, git, Docker e cross-compilazione).
  - **Host di Gioco (Server)**: **PC Gaming Windows 10/11** (GPU NVIDIA/AMD, Steam Big Picture, display virtuale, Apollonio).
  - **Client (Ricevitore TV)**: **Samsung Smart TV** (Tizen OS 5.5+, Artemide `.wgt`).
- **Stato Generale**: Architettura approvata al 100%. Nessun codice di produzione modificato su master. Tool diagnostico Windows (`shell_probe.exe`) cross-compilato con successo da Linux per il PC gaming. In corso: definizione dei test di Fase 0.

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

9. **Architettura Host Multipiattaforma (Windows + Linux)**:
   - Il Supervisor `apollonio-shell` adotta un'interfaccia astratta `IShellPlatform`:
     - **Windows Backend**: Win32 HWND Backdrop Kiosk, `SetWinEventHook`, `IddSampleDriver`, registro Steam.
     - **Linux Backend**: Gamescope / Wayland compositor sandbox, PipeWire/DRM/KMS, Steam process monitoring.
   - Permette lo sviluppo e il testing sia su workstation Linux sia su macchine gaming Windows.

10. **Artemide come Console Companion Client per Sunshine**:
    - Artemide evolve da semplice visualizzatore passivo a **Companion Client Console per qualsiasi server Sunshine / Apollonio**.
    - Include l'overlay Drawer locale, la tastiera virtuale nativa e il routing avanzato di input.
    - Se l'host dispone del modulo Supervisor, sblocca l'esperienza console al 99% (locandine offline, switch finestre, isolamento a nero). Se l'host è un Sunshine standard senza supervisor, Artemide continua a funzionare perfettamente fornendo tutte le funzionalità locali del Drawer.

---

## 3. Elementi Realizzati / Pronti

| Elemento | Percorso File | Stato | Descrizione |
|---|---|---|---|
| **Documento di Design** | `console_shell_design.md` | ✅ Completato | Specifica iniziale dettagliata |
| **Piano di Sviluppo** | [DEVELOPMENT_PLAN.md](file:///home/antigravity/DEVELOPMENT_PLAN.md) | ✅ Completato | Piano esecutivo completo di tutte le 5 fasi |
| **Stato Avanzamento** | [WORK_IN_PROGRESS.md](file:///home/antigravity/WORK_IN_PROGRESS.md) | ✅ Aggiornato | Registro decisioni e guida per agenti AI |
| **Tool Diagnostico Host** | `Apollonio/tools/shell_probe.cpp` | ✅ Pronto | Strumento C++20 per validare monitor, Z-order, Big Picture e focus |
| **CMake Integration** | `Apollonio/tools/CMakeLists.txt` | ✅ Integrato | Target `shell_probe` aggiunto al CMake di Apollonio |

---

## 4. Prossimi Passi di Sviluppo (Roadmap Esecutiva)

1. **Fase 0 (Validazione Hardware)**:
   - Eseguire `shell_probe.exe` sul PC gaming Windows (`monitors`, `watch`, `backdrop`) per confermare il comportamento del Virtual Display e di Steam Big Picture.
   - Testare su Artemide / TV Tizen la reattività del Gamepad per verificare l'assenza di ritardi con il grace period di 80 ms su Back+Start.
2. **Fase 1 (Implementazione Supervisor Base `apollonio-shell`)**:
   - Creare la struttura modulare `Apollonio/tools/apollonio-shell/` con l'interfaccia `IShellPlatform`.
   - Implementare `WindowsShellPlatform` (Backdrop Kiosk nero, Z-order hook, Steam Desktop Config neutralizer).
   - Implementare `LinuxShellPlatform` (Gamescope session launcher).
   - Implementare la Named Pipe / Unix Socket per la telemetria.
3. **Fase 2 (Drawer UI su Artemide & Canale 0x31xx)**:
   - Implementare il Drawer overlay in `Artemide/wasm/index.html` e CSS.
   - Agganciare la navigazione controller in `Artemide/wasm/platform/navigation.js` (`Views.ShellDrawer`).
   - Abilitare la macchina a stati Back+Start in `Artemide/wasm/gamepad.cpp`.

---

## 5. Istruzioni di Handover per l'Agente AI Locale

> **Messaggio per l'Agente AI che apre questo progetto**:
> Ciao! Sei stato aperto per proseguire lo sviluppo della **Console-Like Shell** per streaming locale basata su **Apollonio** (Host) e **Artemide** (Client Tizen).
>
> 1. **NON ricominciare da capo e NON chiedere all'utente di rispiegare la visione**:
>    - Leggi attentamente [DEVELOPMENT_PLAN.md](file:///home/antigravity/DEVELOPMENT_PLAN.md) (architettura, protocolli `0x31xx`, Z-order, smart mouse).
>    - Leggi le decisioni bloccate nella Sezione 2 di questo file.
> 2. **Stato dei Branch**:
>    - Sia `Apollonio` sia `Artemide` si trovano sul branch dedicato: **`feature/console-shell`**. Lavora sempre su questo branch.
> 3. **Cosa devi fare ora**:
>    - Procedi con i compiti elencati nella **Sezione 4 (Prossimi Passi)** partendo dalla Fase 0/1.
>    - Mantieni aggiornato questo file `WORK_IN_PROGRESS.md` man mano che completi i punti.
