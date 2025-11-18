# CMASIO Local Sidetone - Piano Implementazione Esecutivo

**Version:** 1.0
**Date:** 2025-11-18
**Based on:** PRD_CMASIO_Local_Sidetone.md v1.1

---

## Indice

- [Phase 1: C Layer Implementation (ChannelMaster.dll)](#phase-1-c-layer-implementation-channelmasterdll)
- [Phase 2: C# Integration Layer](#phase-2-c-integration-layer)
- [Phase 3: Build & Testing](#phase-3-build--testing)
- [Rollback Plan](#rollback-plan)
- [Troubleshooting](#troubleshooting)

---

## Phase 1: C Layer Implementation (ChannelMaster.dll)

### Task 1.1: Modificare cmasio.h - Struct Fields

**File:** `Project Files/Source/ChannelMaster/cmasio.h`

**Obiettivo:** Aggiungere campi alla struct `_cmasio` per gestire sidetone locale

**Localizzazione:** Trovare `typedef struct _cmasio` (linea ~50-100)

**Modifiche da applicare:**
```c
typedef struct _cmasio
{
    // ... existing fields ...

    // NEW: Sidetone generation - LOCAL STATE ONLY
    // NOTE: Parameters (enabled, freq, volume) are read via C# callbacks
    int tx_active;                   // Flag: 1 = TX mode, 0 = RX mode
    double sidetone_phase;           // Oscillator phase accumulator
    double* sidetone_buffer;         // Buffer for generated sidetone

    // Fade state management
    int fade_state;                  // 0=idle, 1=fade_in, 2=active, 3=fade_out
    int fade_counter;                // Current fade sample count
    int fade_samples;                // Fade duration (const: 48 @ 48kHz = 1ms)
} cmasio, *CMASIO;
```

**Posizione:** Dopo gli existing fields, prima della chiusura della struct

**Dependencies:** Nessuna

**Acceptance Criteria:**
- [ ] 6 nuovi campi aggiunti alla struct
- [ ] Commenti documentano che i parametri sono letti via callback
- [ ] Compilazione senza errori

---

### Task 1.2: Modificare cmasio.h - Callback Pointers e Export Declarations

**File:** `Project Files/Source/ChannelMaster/cmasio.h`

**Obiettivo:** Dichiarare callback function pointers e export declarations

**Localizzazione:** Dopo la struct definition, prima di `#ifdef __cplusplus`

**Modifiche da applicare:**

**Parte A - Callback Function Pointers (file scope):**
```c
// Callback function pointers for reading C# application state
static int (*pGetSidetoneEnabled)(void) = NULL;
static int (*pGetSidetoneFreq)(void) = NULL;
static double (*pGetSidetoneVolume)(void) = NULL;
```

**Parte B - Export Declarations (prima di chiusura header):**
```c
#ifdef __cplusplus
extern "C" {
#endif

// Existing exports...

// NEW: CMASIO Sidetone exports
extern __declspec(dllexport) void setCMASIO_TXActive(int tx_active);
extern __declspec(dllexport) void setCMASIO_Callbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void));

#ifdef __cplusplus
}
#endif
```

**Dependencies:** Task 1.1

**Acceptance Criteria:**
- [ ] 3 callback pointers dichiarati come static
- [ ] 2 export declarations aggiunte
- [ ] Compilazione senza errori

---

### Task 1.3: Modificare cmasio.c - Inizializzazione in create_cmasio()

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Inizializzare i nuovi campi sidetone nella funzione `create_cmasio()`

**Localizzazione:** Trovare `void create_cmasio()` (linea ~200-300)

**Modifiche da applicare:**
```c
void create_cmasio()
{
    // ... existing initialization ...

    // NEW: Initialize sidetone state (LOCAL ONLY - no parameter duplication)
    pcma->tx_active = 0;
    pcma->sidetone_phase = 0.0;
    pcma->fade_samples = 48;                 // 1ms @ 48kHz
    pcma->fade_state = 0;
    pcma->fade_counter = 0;

    // Allocate sidetone buffer
    pcma->sidetone_buffer = (double*)malloc0(pcma->blocksize * sizeof(complex));

    if (pcma->sidetone_buffer == NULL) {
        OutputDebugStringA("ERROR: Failed to allocate sidetone buffer");
    }

    // NOTE: Callbacks for enabled/freq/volume will be set from C# during startup
    // via setCMASIO_Callbacks()
}
```

**Posizione:** Prima della chiusura della funzione, dopo le altre inizializzazioni

**Dependencies:** Task 1.1, 1.2

**Acceptance Criteria:**
- [ ] Tutti i campi inizializzati
- [ ] Buffer allocato con malloc0()
- [ ] Error logging se allocation fallisce
- [ ] Commento documenta che callbacks sono set da C#

---

### Task 1.4: Modificare cmasio.c - Cleanup in destroy_cmasio()

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Liberare sidetone_buffer nella funzione `destroy_cmasio()`

**Localizzazione:** Trovare `void destroy_cmasio()` (linea ~350-400)

**Modifiche da applicare:**
```c
void destroy_cmasio()
{
    if (pcm->audioCodecId != ASIO) return;

    // ... existing cleanup ...

    // NEW: Free sidetone buffer
    if (pcma->sidetone_buffer != NULL) {
        _aligned_free(pcma->sidetone_buffer);
        pcma->sidetone_buffer = NULL;
    }

    unloadASIO();
}
```

**Posizione:** Dopo existing cleanup, prima di `unloadASIO()`

**Dependencies:** Task 1.3

**Acceptance Criteria:**
- [ ] Buffer liberato con _aligned_free()
- [ ] Pointer impostato a NULL dopo free
- [ ] Check per NULL prima di free

---

### Task 1.5: Creare generateLocalSidetone() in cmasio.c

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Creare funzione generatore sinetone sintetico con fade envelope

**Localizzazione:** Aggiungere prima di `asioOUT()` function

**Codice completo:**
```c
// Generate synthetic sidetone with fade in/out
// Parameters are read dynamically via callbacks (no duplication)
void generateLocalSidetone(double* buffer, int nsamples, int samplerate)
{
    // Read parameters from C# via callbacks
    if (pGetSidetoneFreq == NULL || pGetSidetoneVolume == NULL)
    {
        memset(buffer, 0, nsamples * sizeof(complex));
        return;
    }

    double freq = (double)pGetSidetoneFreq();     // Read from console.cw_pitch
    double volume = pGetSidetoneVolume();          // Read from console.qsk_sidetone_volume

    double delta_phase = 2.0 * M_PI * freq / samplerate;

    for (int i = 0; i < nsamples; i++)
    {
        // Generate sine wave
        double sample = sin(pcma->sidetone_phase) * volume;

        // Apply fade envelope
        double envelope = 1.0;
        if (pcma->fade_state == 1)  // Fade IN
        {
            envelope = (double)(pcma->fade_counter) / pcma->fade_samples;
            pcma->fade_counter++;
            if (pcma->fade_counter >= pcma->fade_samples) {
                pcma->fade_state = 2;  // Transition to active
            }
        }
        else if (pcma->fade_state == 3)  // Fade OUT
        {
            envelope = 1.0 - ((double)(pcma->fade_counter) / pcma->fade_samples);
            pcma->fade_counter++;
            if (pcma->fade_counter >= pcma->fade_samples) {
                pcma->fade_state = 0;  // Transition to idle
                envelope = 0.0;
            }
        }

        sample *= envelope;

        // Stereo output (L+R identical)
        buffer[2*i] = sample;      // Left
        buffer[2*i+1] = sample;    // Right

        // Update phase
        pcma->sidetone_phase += delta_phase;
        if (pcma->sidetone_phase >= 2.0 * M_PI)
            pcma->sidetone_phase -= 2.0 * M_PI;
    }
}
```

**Dependencies:** Task 1.1, 1.2, 1.3

**Acceptance Criteria:**
- [ ] Funzione legge freq/volume via callbacks
- [ ] Genera sine wave pura
- [ ] Applica fade envelope (1ms = 48 samples)
- [ ] Output stereo identico L+R
- [ ] Phase wrapping corretto
- [ ] Handle NULL callbacks (return silent)

---

### Task 1.6: Modificare asioOUT() in cmasio.c

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Modificare `asioOUT()` per iniettare sidetone quando TX attivo

**Localizzazione:** Trovare `void asioOUT(int id, int nsamples, double* buff)` (linea ~111-120)

**Codice modificato:**
```c
void asioOUT(int id, int nsamples, double* buff)
{
    if (!pcma->run) return;

    // NEW: Check if TX is active and sidetone enabled (via callback)
    int sidetone_enabled = (pGetSidetoneEnabled != NULL) ? pGetSidetoneEnabled() : 0;

    if (pcma->tx_active && sidetone_enabled)
    {
        // GENERATE LOCAL SIDETONE instead of passing RX audio
        generateLocalSidetone(
            pcma->sidetone_buffer,
            nsamples,
            pcm->audio_outrate  // samplerate
        );

        // Send sidetone to ASIO output
        xrmatchIN(pcma->rmatchOUT, pcma->sidetone_buffer);
    }
    else
    {
        // NORMAL RX: Pass audio from mixer
        xrmatchIN(pcma->rmatchOUT, buff);
    }

    // Protocol 1: Always send empty buffer to network
    if (pcma->protocol == 0)
    {
        memset(buff, 0, nsamples * sizeof(complex));
        OutBound(0, nsamples, buff);
    }
}
```

**Logica:**
1. Check se TX attivo (`pcma->tx_active`)
2. Check se sidetone enabled via callback
3. Se entrambi true: genera sidetone, altrimenti passa RX audio normale

**Dependencies:** Task 1.5

**Acceptance Criteria:**
- [ ] Check callback prima di usarlo (NULL safety)
- [ ] Sidetone injected solo se TX && enabled
- [ ] RX audio passa normalmente quando sidetone off
- [ ] Protocol 1 logic preservata

---

### Task 1.7: Creare setCMASIO_TXActive() in cmasio.c

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Creare funzione per notificare transizioni TX/RX da C#

**Localizzazione:** Aggiungere dopo `asioOUT()` function

**Codice completo:**
```c
// Set TX active state (called from C# when MOX changes)
PORT void setCMASIO_TXActive(int tx_active)
{
    if (pcm->audioCodecId != ASIO) return;

    if (pcma->tx_active != tx_active)
    {
        pcma->tx_active = tx_active;

        if (tx_active == 1)  // RX → TX transition
        {
            pcma->sidetone_phase = 0.0;  // Reset phase
            pcma->fade_state = 1;         // Start fade-in
            pcma->fade_counter = 0;

            OutputDebugStringA("CMASIO: TX ON - Sidetone fade-in started");
        }
        else  // TX → RX transition
        {
            pcma->fade_state = 3;  // Start fade-out
            pcma->fade_counter = 0;

            OutputDebugStringA("CMASIO: TX OFF - Sidetone fade-out started");
        }
    }
}
```

**Logica:**
- RX → TX: reset phase, start fade-in (state=1)
- TX → RX: start fade-out (state=3)
- Debug logging per troubleshooting

**Dependencies:** Task 1.1, 1.6

**Acceptance Criteria:**
- [ ] Funzione PORT exported
- [ ] Check audioCodecId == ASIO
- [ ] Phase reset al TX on
- [ ] Fade states corretti
- [ ] Debug logging presente

---

### Task 1.8: Creare setCMASIO_Callbacks() in cmasio.c

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Creare funzione per registrare callbacks da C#

**Localizzazione:** Aggiungere dopo `setCMASIO_TXActive()`

**Codice completo:**
```c
// Set callback function pointers (called once during C# initialization)
PORT void setCMASIO_Callbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void))
{
    if (pcm->audioCodecId != ASIO) return;

    pGetSidetoneEnabled = getEnabled;
    pGetSidetoneFreq = getFreq;
    pGetSidetoneVolume = getVolume;

    OutputDebugStringA("CMASIO: Callbacks registered for sidetone parameters");
}
```

**Logica:**
- Registra 3 function pointers
- Called una volta durante startup C#
- Debug logging per conferma

**Dependencies:** Task 1.2

**Acceptance Criteria:**
- [ ] Funzione PORT exported
- [ ] Check audioCodecId == ASIO
- [ ] 3 callbacks registrati
- [ ] Debug logging presente

---

## Phase 2: C# Integration Layer

### Task 2.1: Aggiungere Delegate Types in NetworkIOImports.cs

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs`

**Obiettivo:** Dichiarare delegate types per callbacks C→C#

**Localizzazione:** Nella classe `NetworkIO`, vicino ad altri delegates

**Codice da aggiungere:**
```csharp
// Delegate types for CMASIO callbacks (C → C#)
public delegate int GetSidetoneEnabledDelegate();
public delegate int GetSidetoneFreqDelegate();
public delegate double GetSidetoneVolumeDelegate();
```

**Dependencies:** Nessuna (parallela a Phase 1)

**Acceptance Criteria:**
- [ ] 3 delegate types dichiarati
- [ ] Signatures corrette (return types, no parameters)
- [ ] Compilazione senza errori

---

### Task 2.2: Aggiungere DllImport Functions in NetworkIOImports.cs

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs`

**Obiettivo:** Dichiarare P/Invoke imports per funzioni C

**Localizzazione:** Nella classe `NetworkIO`, vicino ad altri [DllImport]

**Codice da aggiungere:**
```csharp
// CMASIO Sidetone P/Invoke imports
[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_TXActive(int tx_active);

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_Callbacks(
    GetSidetoneEnabledDelegate getEnabled,
    GetSidetoneFreqDelegate getFreq,
    GetSidetoneVolumeDelegate getVolume);
```

**Dependencies:** Task 2.1

**Acceptance Criteria:**
- [ ] CallingConvention.Cdecl specificato
- [ ] Signatures matchano le funzioni C
- [ ] Delegate types usati per callbacks
- [ ] Compilazione senza errori

---

### Task 2.3: Creare Class CMASIOSidetone in cmaster.cs

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Creare wrapper class per gestire sidetone CMASIO

**Localizzazione:** Aggiungere alla fine del file, fuori da altre classi

**Struttura base:**
```csharp
public static class CMASIOSidetone
{
    // Callback delegates (must be kept alive to prevent GC collection!)
    private static NetworkIO.GetSidetoneEnabledDelegate _getEnabledDelegate;
    private static NetworkIO.GetSidetoneFreqDelegate _getFreqDelegate;
    private static NetworkIO.GetSidetoneVolumeDelegate _getVolumeDelegate;

    // Initialize callbacks (called during Console startup)
    public static void InitializeCallbacks()
    {
        // TODO: Task 2.7
    }

    // Callback implementations
    private static int GetCMASIOSidetoneEnabled()
    {
        // TODO: Task 2.4
        return 0;
    }

    private static int GetCMASIOSidetoneFreq()
    {
        // TODO: Task 2.5
        return 600;
    }

    private static double GetCMASIOSidetoneVolume()
    {
        // TODO: Task 2.6
        return 0.5;
    }

    // Notify C layer of TX state change
    public static void SetTXActive(bool tx_active)
    {
        // TODO: Task 2.8
    }
}
```

**Dependencies:** Task 2.1, 2.2

**Acceptance Criteria:**
- [ ] Static class creata
- [ ] 3 static delegate fields per GC protection
- [ ] 5 methods definiti (stub ok)
- [ ] Compilazione senza errori

---

### Task 2.4: Implementare GetCMASIOSidetoneEnabled()

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Implementare callback che determina se sidetone è enabled

**Codice completo:**
```csharp
// Callback: Read sidetone enabled state from existing console parameters
private static int GetCMASIOSidetoneEnabled()
{
    try
    {
        // CMASIO sidetone is enabled when BOTH conditions are true:
        // 1. CWSidetone enabled (setup.cs:chkDSPKeyerSidetone)
        // 2. Semi Break-In mode active (console.cs:chkQSK CheckState.Checked)

        bool sidetone_on = Console.CurrentConsole.CWSidetone;
        bool semi_breakin = (Console.CurrentConsole.BreakInEnabledState == CheckState.Checked);

        return (sidetone_on && semi_breakin) ? 1 : 0;
    }
    catch (Exception ex)
    {
        System.Diagnostics.Debug.WriteLine($"CMASIO GetEnabled exception: {ex.Message}");
        return 0;
    }
}
```

**Logica:**
- Sidetone enabled = `CWSidetone==true` AND `BreakInEnabledState==Checked`
- QSK mode (Indeterminate) → return 0 (keep RX monitor)
- Manual mode (Unchecked) → return 0 (no sidetone)
- Exception handling per sicurezza (called da audio thread)

**Dependencies:** Task 2.3

**Acceptance Criteria:**
- [ ] Check entrambe le condizioni
- [ ] Return 1 solo per Semi Break-In
- [ ] Exception handling presente
- [ ] Debug logging su errori

---

### Task 2.5: Implementare GetCMASIOSidetoneFreq()

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Implementare callback che legge frequenza sidetone

**Codice completo:**
```csharp
// Callback: Read sidetone frequency from existing console parameter
private static int GetCMASIOSidetoneFreq()
{
    try
    {
        // Uses existing console.cs:cw_pitch (line 18099)
        // Set by setup.cs:udDSPCWPitch (Setup > DSP > Keyer > CW Pitch)
        int freq = Console.CurrentConsole.CWPitch;

        // Sanity check (200-1200 Hz range)
        if (freq < 200) freq = 200;
        if (freq > 1200) freq = 1200;

        return freq;
    }
    catch (Exception ex)
    {
        System.Diagnostics.Debug.WriteLine($"CMASIO GetFreq exception: {ex.Message}");
        return 600; // Default fallback
    }
}
```

**Logica:**
- Legge `console.CWPitch` (controllato da `udDSPCWPitch`)
- Sanity check range 200-1200 Hz
- Fallback a 600 Hz su errore

**Dependencies:** Task 2.3

**Acceptance Criteria:**
- [ ] Legge CWPitch correttamente
- [ ] Range check 200-1200 Hz
- [ ] Exception handling con fallback
- [ ] Debug logging su errori

---

### Task 2.6: Implementare GetCMASIOSidetoneVolume()

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Implementare callback che legge volume sidetone

**Codice completo:**
```csharp
// Callback: Read sidetone volume from existing console parameter
private static double GetCMASIOSidetoneVolume()
{
    try
    {
        // Uses existing console.cs:qsk_sidetone_volume (line 12913)
        // Semi Break-In: use TXAF, QSK: use qsk_sidetone_volume

        int volume_percent;

        if (Console.CurrentConsole.BreakInEnabledState == CheckState.Checked)
        {
            // Semi Break-In mode: use TX AF level (0-100)
            volume_percent = Console.CurrentConsole.TXAF;
        }
        else
        {
            // QSK mode: use QSK sidetone volume (0-100)
            volume_percent = Console.CurrentConsole.QSKSidetoneVolume;
        }

        // Convert to 0.0-1.0 range
        double volume = (double)volume_percent / 100.0;

        // Sanity check
        if (volume < 0.0) volume = 0.0;
        if (volume > 1.0) volume = 1.0;

        return volume;
    }
    catch (Exception ex)
    {
        System.Diagnostics.Debug.WriteLine($"CMASIO GetVolume exception: {ex.Message}");
        return 0.5; // Default fallback 50%
    }
}
```

**Logica:**
- Semi Break-In: usa `TXAF` (0-100%)
- QSK: usa `QSKSidetoneVolume` (0-100%)
- Converte a 0.0-1.0 per C layer
- Fallback a 0.5 (50%) su errore

**Dependencies:** Task 2.3

**Acceptance Criteria:**
- [ ] Switch corretto tra TXAF e QSKSidetoneVolume
- [ ] Conversione 0-100 → 0.0-1.0 corretta
- [ ] Range check 0.0-1.0
- [ ] Exception handling con fallback

---

### Task 2.7: Implementare InitializeCallbacks()

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Implementare funzione che registra callbacks con C layer

**Codice completo:**
```csharp
// Initialize callbacks (called during Console startup)
public static void InitializeCallbacks()
{
    try
    {
        // Create delegates and keep references to prevent GC
        _getEnabledDelegate = GetCMASIOSidetoneEnabled;
        _getFreqDelegate = GetCMASIOSidetoneFreq;
        _getVolumeDelegate = GetCMASIOSidetoneVolume;

        // Register callbacks with C layer
        NetworkIO.setCMASIO_Callbacks(
            _getEnabledDelegate,
            _getFreqDelegate,
            _getVolumeDelegate);

        System.Diagnostics.Debug.WriteLine("CMASIO: Callbacks initialized successfully");
    }
    catch (Exception ex)
    {
        System.Diagnostics.Debug.WriteLine($"CMASIO InitializeCallbacks exception: {ex.Message}");
    }
}
```

**Logica:**
- Crea delegates e li memorizza in static fields (GC protection)
- Chiama P/Invoke per registrare con C layer
- Debug logging per conferma/errori

**IMPORTANTE:** I delegates DEVONO essere memorizzati in static fields, altrimenti il GC li può raccogliere e il C layer avrà dangling pointers!

**Dependencies:** Task 2.2, 2.4, 2.5, 2.6

**Acceptance Criteria:**
- [ ] Delegates memorizzati in static fields
- [ ] setCMASIO_Callbacks chiamato correttamente
- [ ] Exception handling presente
- [ ] Debug logging presente

---

### Task 2.8: Implementare SetTXActive()

**File:** `Project Files/Source/Console/cmaster.cs`

**Obiettivo:** Implementare wrapper per notificare C layer di TX state changes

**Codice completo:**
```csharp
// Notify C layer of TX state change
public static void SetTXActive(bool tx_active)
{
    try
    {
        NetworkIO.setCMASIO_TXActive(tx_active ? 1 : 0);
    }
    catch (Exception ex)
    {
        System.Diagnostics.Debug.WriteLine($"CMASIO SetTXActive exception: {ex.Message}");
    }
}
```

**Logica:**
- Simple wrapper su P/Invoke
- Converte bool → int
- Exception handling per sicurezza

**Dependencies:** Task 2.2

**Acceptance Criteria:**
- [ ] Conversione bool → int corretta
- [ ] Exception handling presente
- [ ] Debug logging su errori

---

### Task 2.9: Modificare console.cs MOX Property

**File:** `Project Files/Source/Console/console.cs`

**Obiettivo:** Hook MOX property per notificare CMASIO di TX state changes

**Localizzazione:** Trovare `public bool MOX` property (linea ~13000-14000)

**Modifiche da applicare:**
```csharp
public bool MOX
{
    get { return _mox; }
    set
    {
        if (_mox != value)
        {
            _mox = value;

            // ... existing MOX logic ...

            // NEW: Notify CMASIO of TX state change
            if (CurrentAudioCodec == AudioCodec.ASIO)
            {
                CMASIOSidetone.SetTXActive(value);
            }

            // ... rest of existing code ...
        }
    }
}
```

**Posizione:** Dopo l'assignment `_mox = value;`, ma prima della fine del setter

**IMPORTANTE:** Non modificare la logica esistente, solo aggiungere la notifica CMASIO!

**Dependencies:** Task 2.8

**Acceptance Criteria:**
- [ ] Check `CurrentAudioCodec == ASIO` prima di chiamare
- [ ] Chiamata inserita nel setter, dopo _mox assignment
- [ ] Logica esistente MOX preservata
- [ ] Compilazione senza errori

---

### Task 2.10: Modificare console.cs Startup - Initialize Callbacks

**File:** `Project Files/Source/Console/console.cs`

**Obiettivo:** Chiamare `InitializeCallbacks()` durante startup console

**Localizzazione:** Trovare il costruttore `Console()` o metodo di inizializzazione principale (es. `setupPowerThread()`, dopo `InitializeComponent()`)

**Opzione A - Nel costruttore Console():**
```csharp
public Console()
{
    InitializeComponent();

    // ... existing initialization ...

    // NEW: Initialize CMASIO callbacks
    CMASIOSidetone.InitializeCallbacks();

    // ... rest of initialization ...
}
```

**Opzione B - In metodo di startup (se presente):**
```csharp
private void Console_Load(object sender, EventArgs e)
{
    // ... existing code ...

    // NEW: Initialize CMASIO callbacks
    CMASIOSidetone.InitializeCallbacks();

    // ... rest of code ...
}
```

**NOTA:** Identificare il punto migliore guardando dove altri subsystems sono inizializzati.

**Dependencies:** Task 2.7

**Acceptance Criteria:**
- [ ] InitializeCallbacks() chiamato durante startup
- [ ] Chiamato DOPO InitializeComponent()
- [ ] Chiamato una volta sola
- [ ] Compilazione senza errori

---

## Phase 3: Build & Testing

### Task 3.1: Build ChannelMaster.dll

**Obiettivo:** Compilare C layer con le modifiche

**Steps:**
1. Aprire solution ChannelMaster in Visual Studio
2. Selezionare configurazione (Debug/Release)
3. Build → Build Solution (Ctrl+Shift+B)
4. Verificare nessun errore di compilazione
5. Verificare ChannelMaster.dll generato in output directory

**Expected Output:**
- `ChannelMaster.dll` aggiornato
- Nessun warning critico
- Export symbols verificati con `dumpbin /EXPORTS ChannelMaster.dll`

**Troubleshooting:**
- Se errore "symbol not found": verificare export declarations in .h
- Se errore "undefined reference": verificare implementazione funzioni

**Dependencies:** Task 1.1-1.8 completati

**Acceptance Criteria:**
- [ ] Build successo senza errori
- [ ] DLL generato con exports corretti
- [ ] Size DLL aumentato (~5-10 KB)

---

### Task 3.2: Build Thetis

**Obiettivo:** Compilare C# layer con le modifiche

**Steps:**
1. Aprire solution Thetis in Visual Studio
2. Selezionare configurazione (Debug/Release)
3. Build → Build Solution (Ctrl+Shift+B)
4. Verificare nessun errore di compilazione
5. Copiare ChannelMaster.dll aggiornato nella bin directory di Thetis

**Expected Output:**
- `Thetis.exe` aggiornato
- Nessun warning critico
- ChannelMaster.dll nella stessa directory di Thetis.exe

**Troubleshooting:**
- Se errore P/Invoke: verificare signatures DllImport
- Se errore "method not found": verificare namespace CMASIOSidetone

**Dependencies:** Task 2.1-2.10 completati, Task 3.1 completato

**Acceptance Criteria:**
- [ ] Build successo senza errori
- [ ] Thetis.exe aggiornato
- [ ] ChannelMaster.dll presente in bin directory

---

### Task 3.3: Test Funzionale - Sidetone in Semi Break-In

**Obiettivo:** Verificare sidetone audible in Semi Break-In mode

**Precondizioni:**
- Hardware ASIO connesso e funzionante
- Thetis in CW mode
- Audio codec impostato su ASIO

**Steps:**
1. Aprire Setup > DSP > Keyer > Options
2. Verificare `chkDSPKeyerSidetone` checked
3. Chiudere Setup, tornare a Console
4. Impostare `chkQSK` in stato "SEMI" (CheckState.Checked)
5. Premere MOX o paddle CW
6. **VERIFY:** Sidetone audible in cuffia

**Expected Result:**
- ✅ Sidetone audible durante TX
- ✅ Tono puro, senza distorsioni
- ✅ Nessun sidetone in RX

**Troubleshooting:**
- Se nessun audio: verificare callbacks registrati (check debug output)
- Se audio distorto: verificare sample rate (48 kHz)
- Se sidetone continua in RX: verificare fade-out logic

**Dependencies:** Task 3.1, 3.2 completati

**Acceptance Criteria:**
- [ ] Sidetone audible solo durante TX
- [ ] Audio pulito senza click
- [ ] Silenzio in RX

---

### Task 3.4: Test Fade - Verifica Assenza Click

**Obiettivo:** Verificare fade in/out senza click audibili

**Precondizioni:**
- Test 3.3 passato
- Cuffia a volume alto per sentire eventuali click

**Steps:**
1. Impostare CW mode, Semi Break-In
2. Premere paddle CW con pattern: DIT-pause-DIT-pause-DIT
3. Ascoltare attentamente inizio e fine ogni dit
4. **VERIFY:** Nessun click audible
5. Ripetere a varie velocità (15-40 WPM)

**Expected Result:**
- ✅ Fade in smooth (1ms = 48 samples)
- ✅ Fade out smooth (1ms = 48 samples)
- ✅ Nessun click o pop

**Troubleshooting:**
- Se click al TX on: verificare fade_state inizializzato a 1
- Se click al TX off: verificare fade_state = 3
- Se fade troppo lento: verificare fade_samples = 48 @ 48kHz

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] Nessun click durante TX on
- [ ] Nessun click durante TX off
- [ ] Test passato a varie velocità

---

### Task 3.5: Test Parametri - Verifica Frequenza Sidetone

**Obiettivo:** Verificare frequenza sidetone segue `udDSPCWPitch`

**Precondizioni:**
- Test 3.3 passato
- Frequency counter o spectrum analyzer disponibile (opzionale)

**Steps:**
1. Aprire Setup > DSP > Keyer > CW Pitch
2. Impostare `udDSPCWPitch` = 400 Hz
3. Premere MOX, ascoltare tono
4. **VERIFY:** Tono basso (~400 Hz)
5. Cambiare a 800 Hz durante TX (se possibile)
6. **VERIFY:** Tono alto (~800 Hz), cambio immediato
7. Testare range 200-1200 Hz

**Expected Result:**
- ✅ Frequenza corrisponde a udDSPCWPitch
- ✅ Cambio frequenza real-time (callback based)
- ✅ Range 200-1200 Hz funzionante

**Troubleshooting:**
- Se frequenza fissa: verificare callback GetCMASIOSidetoneFreq
- Se frequenza sbagliata: verificare conversione Hz
- Se nessun cambio real-time: verificare callbacks chiamati da audio thread

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] Frequenza corrisponde a GUI control
- [ ] Cambio real-time funziona
- [ ] Range completo 200-1200 Hz testato

---

### Task 3.6: Test Volume - Verifica Controllo Indipendente

**Obiettivo:** Verificare volume sidetone indipendente da RX volume

**Precondizioni:**
- Test 3.3 passato

**Steps:**
1. Impostare Semi Break-In mode
2. Impostare RX AF = 50%
3. Impostare TX AF = 10%
4. Premere MOX
5. **VERIFY:** Sidetone quieto (~10% volume)
6. Return to RX, verificare audio RX normal (~50%)
7. Cambiare TX AF a 100% durante TX
8. **VERIFY:** Sidetone aumenta immediatamente

**Expected Result:**
- ✅ Sidetone volume ≠ RX volume (indipendente)
- ✅ Sidetone segue TX AF slider in Semi mode
- ✅ Cambio volume real-time

**Troubleshooting:**
- Se volume accoppiato con RX: verificare callback GetCMASIOSidetoneVolume
- Se nessun cambio real-time: verificare callbacks
- Se volume errato: verificare conversione 0-100 → 0.0-1.0

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] Volume indipendente da RX
- [ ] Segue TX AF slider
- [ ] Cambio real-time funziona
- [ ] Range 0-100% testato

---

### Task 3.7: Test AGC Recovery - Misura Tempo Recupero

**Obiettivo:** Misurare tempo recupero AGC dopo TX (deve essere < 10ms vs 1-2s precedente)

**Precondizioni:**
- Test 3.3 passato
- Access a segnale debole per test RX

**Steps:**
1. Sintonizzare su segnale debole CW (S3-S5)
2. Abilitare sidetone CMASIO (Semi Break-In)
3. Premere MOX per 1 secondo
4. Rilasciare MOX, tornare a RX
5. **VERIFY:** Segnale RX immediately audible
6. **MEASURE:** Tempo prima che AGC recuperi livello normale
7. Ripetere test 5 volte per conferma

**Expected Result:**
- ✅ Recupero AGC **< 10ms** (praticamente istantaneo)
- ✅ vs precedente **1-2 secondi** con RX monitor

**Comparison Test:**
- Disabilitare sidetone CMASIO (chkDSPKeyerSidetone = unchecked)
- Ripetere test: dovresti vedere 1-2s recovery time (old behavior)

**Troubleshooting:**
- Se recovery lento: verificare sidetone injected (non RX demodulato)
- Se nessuna differenza: verificare callback GetCMASIOSidetoneEnabled

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] Recupero AGC < 10ms con sidetone enabled
- [ ] Differenza visibile vs old behavior
- [ ] Test ripetibile e consistente

---

### Task 3.8: Test Callbacks - Verifica Sincronizzazione Real-Time

**Obiettivo:** Verificare callbacks sincronizzano parametri real-time

**Precondizioni:**
- Test 3.3, 3.5, 3.6 passati

**Steps:**
1. Entrare in TX (MOX on)
2. Durante TX, cambiare udDSPCWPitch da 600 a 800 Hz
3. **VERIFY:** Frequenza cambia immediatamente
4. Cambiare TX AF da 50% a 10%
5. **VERIFY:** Volume diminuisce immediatamente
6. Uscire da TX, verificare RX normale

**Expected Result:**
- ✅ Cambio frequenza istantaneo (< 100ms)
- ✅ Cambio volume istantaneo (< 100ms)
- ✅ Nessun lag o buffering

**Troubleshooting:**
- Se lag: verificare callbacks called ad ogni audio block
- Se nessun cambio: verificare callbacks implementation
- Se crash: verificare thread safety (read-only access)

**Dependencies:** Task 3.5, 3.6 completati

**Acceptance Criteria:**
- [ ] Parametri cambiano real-time durante TX
- [ ] Latency < 100ms
- [ ] Nessun artifact audio durante cambio

---

### Task 3.9: Test QSK Mode - Verifica Comportamento Originale

**Obiettivo:** Verificare QSK mode mantiene RX monitor (nessun synthetic sidetone)

**Precondizioni:**
- Test 3.3 passato

**Steps:**
1. Impostare `chkQSK` in stato "QSK" (CheckState.Indeterminate)
2. Verificare `chkDSPKeyerSidetone` checked
3. Premere paddle CW veloce (30+ WPM)
4. **VERIFY:** Sidetone è RX demodulated (old behavior)
5. **VERIFY:** NO synthetic sidetone injected

**Expected Result:**
- ✅ QSK usa RX monitor (comportamento originale)
- ✅ Nessun synthetic sidetone in QSK mode
- ✅ GetCMASIOSidetoneEnabled() returns 0 in QSK

**Troubleshooting:**
- Se synthetic sidetone in QSK: verificare check `BreakInEnabledState == CheckState.Checked` (non Indeterminate)
- Se nessun sidetone: QSK hardware/firmware issue (fuori scope)

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] QSK mode preservato (no changes)
- [ ] Nessun synthetic sidetone
- [ ] Behavior identico a versione precedente

---

### Task 3.10: Test Disable - Verifica Disabilitazione Sidetone

**Obiettivo:** Verificare disabilitazione tramite GUI controls

**Precondizioni:**
- Test 3.3 passato

**Steps:**

**Test A - Disable via chkDSPKeyerSidetone:**
1. Entrare in Semi Break-In mode con sidetone on
2. Aprire Setup > DSP > Keyer > Options
3. Uncheck `chkDSPKeyerSidetone`
4. Chiudere Setup, premere MOX
5. **VERIFY:** Nessun sidetone (silenzio)

**Test B - Disable via chkQSK (Manual mode):**
1. Impostare `chkDSPKeyerSidetone` checked
2. Impostare `chkQSK` in stato "OFF" (CheckState.Unchecked)
3. Premere MOX
4. **VERIFY:** Nessun sidetone (Manual mode)

**Test C - Re-enable:**
1. Check `chkDSPKeyerSidetone`
2. Impostare `chkQSK` in "SEMI"
3. Premere MOX
4. **VERIFY:** Sidetone ritorna

**Expected Result:**
- ✅ Disable funziona via entrambi i controlli
- ✅ Re-enable ripristina sidetone
- ✅ Nessun crash o hang

**Troubleshooting:**
- Se sidetone persiste: verificare GetCMASIOSidetoneEnabled logic
- Se crash al disable: verificare NULL checks in generateLocalSidetone

**Dependencies:** Task 3.3 completato

**Acceptance Criteria:**
- [ ] Disable via chkDSPKeyerSidetone funziona
- [ ] Disable via chkQSK (Manual) funziona
- [ ] Re-enable ripristina correttamente
- [ ] Nessun crash durante disable/enable

---

## Rollback Plan

Se l'implementazione causa problemi critici, seguire questi step per rollback:

### Step 1: Identify Issue
- Crash during startup? → Rollback Phase 2 (C# changes)
- Audio artifacts? → Rollback Phase 1 (C layer)
- Build errors? → Revert specific files

### Step 2: Rollback Commands

**Full Rollback (tutti i cambiamenti):**
```bash
# Revert all changes
git revert HEAD~N  # N = number of commits to revert
git push -u origin claude/repository-quality-analysis-011CUgN1YPS3Ka8rphMsE4tu
```

**Partial Rollback (solo C# layer):**
```bash
# Revert only C# files
git checkout HEAD~1 -- "Project Files/Source/Console/HPSDR/NetworkIOImports.cs"
git checkout HEAD~1 -- "Project Files/Source/Console/cmaster.cs"
git checkout HEAD~1 -- "Project Files/Source/Console/console.cs"
git commit -m "Rollback: Revert C# integration layer"
```

**Partial Rollback (solo C layer):**
```bash
# Revert only C files
git checkout HEAD~1 -- "Project Files/Source/ChannelMaster/cmasio.h"
git checkout HEAD~1 -- "Project Files/Source/ChannelMaster/cmasio.c"
git commit -m "Rollback: Revert C layer changes"
```

### Step 3: Rebuild Clean
```bash
# Clean build directories
rm -rf "Project Files/Source/Console/bin"
rm -rf "Project Files/Source/Console/obj"
rm -rf "Project Files/Source/ChannelMaster/x64"

# Rebuild
msbuild Thetis.sln /t:Rebuild /p:Configuration=Release
```

### Step 4: Verify Rollback
- Launch Thetis
- Verify old behavior restored
- Test basic CW functionality

---

## Troubleshooting

### Issue: No Sidetone Audio

**Symptoms:** Sidetone enabled, TX active, but no audio

**Diagnostic Steps:**
1. Check debug output:
   ```
   CMASIO: Callbacks registered for sidetone parameters
   CMASIO: TX ON - Sidetone fade-in started
   ```
2. Verify `GetCMASIOSidetoneEnabled()` returns 1
3. Verify `generateLocalSidetone()` called

**Possible Causes:**
- Callbacks not registered → Check `InitializeCallbacks()` called
- AudioCodec not ASIO → Check current audio codec
- Frequency out of range → Check udDSPCWPitch value

**Fix:**
- Add debug logging in callbacks
- Verify callback return values
- Check ChannelMaster.dll loaded correctly

---

### Issue: Click/Pop Durante TX Transitions

**Symptoms:** Click audibile all'inizio o fine TX

**Diagnostic Steps:**
1. Measure fade time (should be 1ms = 48 samples @ 48kHz)
2. Verify fade_state transitions: 0→1→2 (TX on), 2→3→0 (TX off)
3. Check phase reset on TX on

**Possible Causes:**
- fade_samples wrong value
- fade_state not initialized
- Phase discontinuity

**Fix:**
- Increase fade_samples (try 96 = 2ms)
- Add debug logging for fade_state
- Ensure phase reset on TX on

---

### Issue: Crash on Startup

**Symptoms:** Thetis crashes during initialization

**Diagnostic Steps:**
1. Check exception details in debugger
2. Look for P/Invoke signature mismatch
3. Verify ChannelMaster.dll exports

**Possible Causes:**
- DllImport signature wrong
- ChannelMaster.dll not found
- Callback delegates GC collected

**Fix:**
- Verify CallingConvention = Cdecl
- Check DLL in same directory as Thetis.exe
- Ensure delegates stored in static fields

---

### Issue: Frequency/Volume Non Cambiano

**Symptoms:** Parametri GUI non riflessi in sidetone

**Diagnostic Steps:**
1. Add debug logging in callback functions
2. Verify callbacks called every audio block
3. Check console properties accessible

**Possible Causes:**
- Callbacks returning cached values
- Console.CurrentConsole null
- Property getters throwing exceptions

**Fix:**
- Remove any caching in callbacks
- Add null checks for CurrentConsole
- Add try-catch in all callbacks

---

### Issue: AGC Recovery Still Slow

**Symptoms:** AGC still takes 1-2s to recover dopo TX

**Diagnostic Steps:**
1. Verify synthetic sidetone used (not RX demodulated)
2. Check `asioOUT()` routing logic
3. Verify `tx_active` flag set correctly

**Possible Causes:**
- RX audio still passing during TX
- Sidetone not injecting correctly
- AudioCodec check failing

**Fix:**
- Add debug logging in `asioOUT()` branching
- Verify `pcma->tx_active == 1` during TX
- Check `CurrentAudioCodec == AudioCodec.ASIO`

---

## Summary Checklist

### Phase 1: C Layer
- [ ] Task 1.1: cmasio.h struct fields
- [ ] Task 1.2: cmasio.h callbacks & exports
- [ ] Task 1.3: create_cmasio() initialization
- [ ] Task 1.4: destroy_cmasio() cleanup
- [ ] Task 1.5: generateLocalSidetone() implementation
- [ ] Task 1.6: asioOUT() modification
- [ ] Task 1.7: setCMASIO_TXActive() implementation
- [ ] Task 1.8: setCMASIO_Callbacks() implementation

### Phase 2: C# Integration
- [ ] Task 2.1: Delegate types in NetworkIOImports.cs
- [ ] Task 2.2: DllImport functions in NetworkIOImports.cs
- [ ] Task 2.3: CMASIOSidetone class creation
- [ ] Task 2.4: GetCMASIOSidetoneEnabled() implementation
- [ ] Task 2.5: GetCMASIOSidetoneFreq() implementation
- [ ] Task 2.6: GetCMASIOSidetoneVolume() implementation
- [ ] Task 2.7: InitializeCallbacks() implementation
- [ ] Task 2.8: SetTXActive() implementation
- [ ] Task 2.9: MOX property hook in console.cs
- [ ] Task 2.10: Startup initialization in console.cs

### Phase 3: Build & Testing
- [ ] Task 3.1: Build ChannelMaster.dll
- [ ] Task 3.2: Build Thetis
- [ ] Task 3.3: Test sidetone in Semi Break-In
- [ ] Task 3.4: Test fade (no clicks)
- [ ] Task 3.5: Test frequency parameter
- [ ] Task 3.6: Test volume parameter
- [ ] Task 3.7: Test AGC recovery time
- [ ] Task 3.8: Test real-time callbacks
- [ ] Task 3.9: Test QSK mode preserved
- [ ] Task 3.10: Test disable functionality

---

**END OF IMPLEMENTATION PLAN**
