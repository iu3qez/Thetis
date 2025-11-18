# CMASIO Local Sidetone - Piano Implementazione v2 (SidetoneGen Module)

**Version:** 2.0 (Approccio Modulo Separato - MINIMAMENTE INVASIVO)
**Date:** 2025-11-18
**Based on:** PRD_CMASIO_Local_Sidetone.md v1.1

---

## Indice

- [Approccio: Modulo Separato](#approccio-modulo-separato)
- [Phase 1: Creare Modulo SidetoneGen](#phase-1-creare-modulo-sidetonegen)
- [Phase 2: Integrare con CMASIO](#phase-2-integrare-con-cmasio)
- [Phase 3: C# Integration Layer](#phase-3-c-integration-layer)
- [Phase 4: Build & Testing](#phase-4-build--testing)
- [Rollback Plan](#rollback-plan)

---

## Approccio: Modulo Separato

### Vantaggi di questo approccio:

✅ **ZERO modifiche a `cmasio.h`** (struct `_cmasio` completamente intatta)
✅ **MINIME modifiche a `cmasio.c`** (solo 5-10 righe aggiunte in `asioOUT()`)
✅ **Completamente isolato** - tutto il codice sidetone in modulo separato
✅ **Facile da disabilitare** - basta commentare #include e chiamate
✅ **Testabile separatamente** - unit test possibili
✅ **Chiara separazione responsabilità**

### Struttura File:

```
Project Files/Source/ChannelMaster/
├── sidetonegen.h       (NEW - 50 righe)
├── sidetonegen.c       (NEW - 200 righe)
├── cmasio.h            (ZERO modifiche!)
├── cmasio.c            (MINIME modifiche - solo chiamate)
└── ChannelMaster.vcxproj (ADD sidetonegen.c al progetto)
```

### Interfaccia Modulo:

```c
// sidetonegen.h - API pulita

void SidetoneGen_Initialize(int blocksize);
void SidetoneGen_Destroy(void);
void SidetoneGen_SetTXActive(int tx_active);
void SidetoneGen_SetCallbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void));
void SidetoneGen_Generate(double* buffer, int nsamples, int samplerate);
int SidetoneGen_IsActive(void);
```

---

## Phase 1: Creare Modulo SidetoneGen

### Task 1.1: Creare sidetonegen.h - Header File

**File:** `Project Files/Source/ChannelMaster/sidetonegen.h` (NEW)

**Obiettivo:** Definire interfaccia pubblica del modulo sidetone

**Codice completo:**

```c
#ifndef _sidetonegen_h
#define _sidetonegen_h

// ============================================================================
// CMASIO Local Sidetone Generator
//
// Generates synthetic sidetone (sine wave with fade envelope) for CMASIO mode
// to avoid AGC saturation issues caused by RX monitoring own TX signal.
//
// Architecture: Callback-based parameter access (zero duplication)
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Callback typedefs for C# parameter access
typedef int (*SidetoneGetEnabledCallback)(void);
typedef int (*SidetoneGetFreqCallback)(void);
typedef double (*SidetoneGetVolumeCallback)(void);

// ============================================================================
// Public API
// ============================================================================

// Initialize sidetone generator
// blocksize: audio block size for buffer allocation
void SidetoneGen_Initialize(int blocksize);

// Destroy and cleanup resources
void SidetoneGen_Destroy(void);

// Set TX active state (triggers fade in/out)
// tx_active: 1 = TX on (start fade in), 0 = TX off (start fade out)
void SidetoneGen_SetTXActive(int tx_active);

// Register C# callbacks for parameter access
void SidetoneGen_SetCallbacks(
    SidetoneGetEnabledCallback getEnabled,
    SidetoneGetFreqCallback getFreq,
    SidetoneGetVolumeCallback getVolume);

// Generate sidetone audio
// buffer: output buffer (stereo interleaved, nsamples * 2)
// nsamples: number of samples per channel
// samplerate: audio sample rate (typically 48000)
void SidetoneGen_Generate(double* buffer, int nsamples, int samplerate);

// Check if sidetone is currently active (TX on AND enabled)
// Returns: 1 if active, 0 if not
int SidetoneGen_IsActive(void);

#ifdef __cplusplus
}
#endif

#endif // _sidetonegen_h
```

**Dependencies:** Nessuna

**Acceptance Criteria:**
- [ ] Header file created
- [ ] 6 public functions declared
- [ ] Documentation comments clear
- [ ] Include guards correct
- [ ] C++ compatibility (extern "C")

---

### Task 1.2: Creare sidetonegen.c - Implementation

**File:** `Project Files/Source/ChannelMaster/sidetonegen.c` (NEW)

**Obiettivo:** Implementare generatore sidetone completamente isolato

**Codice completo:**

```c
#include <math.h>
#include <string.h>
#include <windows.h>  // For OutputDebugStringA
#include "sidetonegen.h"

// ============================================================================
// Private State (file scope - completely isolated from cmasio.c)
// ============================================================================

// TX state
static int g_tx_active = 0;

// Oscillator state
static double g_phase = 0.0;

// Fade envelope state
static int g_fade_state = 0;       // 0=idle, 1=fade_in, 2=active, 3=fade_out
static int g_fade_counter = 0;
static const int FADE_SAMPLES = 48;  // 1ms @ 48kHz

// Audio buffer
static double* g_buffer = NULL;
static int g_blocksize = 0;

// Callback pointers (set by C# via SidetoneGen_SetCallbacks)
static SidetoneGetEnabledCallback g_getEnabled = NULL;
static SidetoneGetFreqCallback g_getFreq = NULL;
static SidetoneGetVolumeCallback g_getVolume = NULL;

// ============================================================================
// Private Helpers
// ============================================================================

static void* malloc0(size_t size)
{
    void* ptr = _aligned_malloc(size, 16);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

// ============================================================================
// Public API Implementation
// ============================================================================

void SidetoneGen_Initialize(int blocksize)
{
    g_blocksize = blocksize;

    // Allocate audio buffer (stereo = blocksize * 2 * sizeof(double))
    g_buffer = (double*)malloc0(blocksize * 2 * sizeof(double));

    if (g_buffer == NULL) {
        OutputDebugStringA("ERROR: SidetoneGen failed to allocate buffer");
        return;
    }

    // Initialize state
    g_tx_active = 0;
    g_phase = 0.0;
    g_fade_state = 0;
    g_fade_counter = 0;

    OutputDebugStringA("SidetoneGen: Initialized successfully");
}

void SidetoneGen_Destroy(void)
{
    if (g_buffer != NULL) {
        _aligned_free(g_buffer);
        g_buffer = NULL;
    }

    g_blocksize = 0;
    g_tx_active = 0;
    g_phase = 0.0;
    g_fade_state = 0;
    g_fade_counter = 0;

    // Clear callbacks
    g_getEnabled = NULL;
    g_getFreq = NULL;
    g_getVolume = NULL;

    OutputDebugStringA("SidetoneGen: Destroyed");
}

void SidetoneGen_SetTXActive(int tx_active)
{
    if (g_tx_active == tx_active) return;  // No change

    g_tx_active = tx_active;

    if (tx_active == 1)  // RX → TX transition
    {
        g_phase = 0.0;           // Reset phase for clean start
        g_fade_state = 1;        // Start fade-in
        g_fade_counter = 0;

        OutputDebugStringA("SidetoneGen: TX ON - fade-in started");
    }
    else  // TX → RX transition
    {
        g_fade_state = 3;        // Start fade-out
        g_fade_counter = 0;

        OutputDebugStringA("SidetoneGen: TX OFF - fade-out started");
    }
}

void SidetoneGen_SetCallbacks(
    SidetoneGetEnabledCallback getEnabled,
    SidetoneGetFreqCallback getFreq,
    SidetoneGetVolumeCallback getVolume)
{
    g_getEnabled = getEnabled;
    g_getFreq = getFreq;
    g_getVolume = getVolume;

    OutputDebugStringA("SidetoneGen: Callbacks registered");
}

void SidetoneGen_Generate(double* buffer, int nsamples, int samplerate)
{
    // Safety checks
    if (buffer == NULL || g_buffer == NULL) {
        return;
    }

    // Read parameters via callbacks
    if (g_getFreq == NULL || g_getVolume == NULL) {
        // No callbacks registered - return silence
        memset(buffer, 0, nsamples * 2 * sizeof(double));
        return;
    }

    double freq = (double)g_getFreq();      // Hz (200-1200)
    double volume = g_getVolume();           // 0.0-1.0

    double delta_phase = 2.0 * M_PI * freq / (double)samplerate;

    // Generate samples
    for (int i = 0; i < nsamples; i++)
    {
        // Generate sine wave
        double sample = sin(g_phase) * volume;

        // Apply fade envelope
        double envelope = 1.0;

        if (g_fade_state == 1)  // Fade IN
        {
            envelope = (double)g_fade_counter / FADE_SAMPLES;
            g_fade_counter++;
            if (g_fade_counter >= FADE_SAMPLES) {
                g_fade_state = 2;  // Transition to active
            }
        }
        else if (g_fade_state == 3)  // Fade OUT
        {
            envelope = 1.0 - ((double)g_fade_counter / FADE_SAMPLES);
            g_fade_counter++;
            if (g_fade_counter >= FADE_SAMPLES) {
                g_fade_state = 0;  // Transition to idle
                envelope = 0.0;
            }
        }
        else if (g_fade_state == 0)  // Idle (silent)
        {
            envelope = 0.0;
        }
        // else g_fade_state == 2 (active): envelope = 1.0

        sample *= envelope;

        // Stereo output (L+R identical)
        buffer[2*i] = sample;      // Left
        buffer[2*i+1] = sample;    // Right

        // Update phase with wrapping
        g_phase += delta_phase;
        if (g_phase >= 2.0 * M_PI)
            g_phase -= 2.0 * M_PI;
    }
}

int SidetoneGen_IsActive(void)
{
    // Active if TX on AND enabled (via callback)
    if (g_tx_active == 0) return 0;

    if (g_getEnabled != NULL) {
        return g_getEnabled();
    }

    return 0;  // Default: not active
}
```

**Dependencies:** Task 1.1

**Acceptance Criteria:**
- [ ] All 6 functions implemented
- [ ] Private state completely isolated (static)
- [ ] Fade envelope logic correct
- [ ] Phase wrapping correct
- [ ] NULL checks for safety
- [ ] Debug logging present
- [ ] No external dependencies except math.h, string.h, windows.h

---

## Phase 2: Integrare con CMASIO

### Task 2.1: Aggiungere sidetonegen.c al Progetto ChannelMaster

**File:** `Project Files/Source/ChannelMaster/ChannelMaster.vcxproj`

**Obiettivo:** Includere sidetonegen.c nella compilazione

**Modifiche da applicare:**

Trovare la sezione `<ClCompile>` e aggiungere:

```xml
<ClCompile Include="sidetonegen.c" />
```

Trovare la sezione `<ClInclude>` e aggiungere:

```xml
<ClInclude Include="sidetonegen.h" />
```

**Alternative:** Se usi Visual Studio IDE:
1. Right-click su progetto ChannelMaster
2. Add → Existing Item...
3. Selezionare `sidetonegen.c` e `sidetonegen.h`

**Dependencies:** Task 1.1, 1.2

**Acceptance Criteria:**
- [ ] sidetonegen.c included in project
- [ ] sidetonegen.h included in project
- [ ] Project compiles without errors

---

### Task 2.2: Modificare cmasio.c - Include Header

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Includere sidetonegen.h

**Localizzazione:** Top of file, dopo altri includes

**Modifiche da applicare:**

```c
// Existing includes...
#include "cmasio.h"
// ... other includes ...

// NEW: Include sidetone generator
#include "sidetonegen.h"
```

**Posizione:** Dopo includes esistenti, prima delle funzioni

**Dependencies:** Task 1.1, 2.1

**Acceptance Criteria:**
- [ ] #include "sidetonegen.h" aggiunto
- [ ] Compilazione senza errori

---

### Task 2.3: Modificare cmasio.c - Inizializzazione in create_cmasio()

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Inizializzare modulo SidetoneGen durante creazione CMASIO

**Localizzazione:** Funzione `create_cmasio()` (linea ~200-300)

**Modifiche da applicare:**

```c
void create_cmasio()
{
    // ... existing initialization ...

    // NEW: Initialize sidetone generator module
    SidetoneGen_Initialize(pcma->blocksize);

    // ... rest of existing code ...
}
```

**Posizione:** Vicino alla fine di create_cmasio(), dopo allocazioni buffer

**Dependencies:** Task 2.2

**Acceptance Criteria:**
- [ ] SidetoneGen_Initialize() chiamato con blocksize corretto
- [ ] Chiamato DOPO pcma->blocksize è impostato
- [ ] Compilazione senza errori

---

### Task 2.4: Modificare cmasio.c - Cleanup in destroy_cmasio()

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Distruggere modulo SidetoneGen durante cleanup CMASIO

**Localizzazione:** Funzione `destroy_cmasio()` (linea ~350-400)

**Modifiche da applicare:**

```c
void destroy_cmasio()
{
    if (pcm->audioCodecId != ASIO) return;

    // ... existing cleanup ...

    // NEW: Destroy sidetone generator module
    SidetoneGen_Destroy();

    unloadASIO();
}
```

**Posizione:** Prima di unloadASIO(), dopo existing cleanup

**Dependencies:** Task 2.2

**Acceptance Criteria:**
- [ ] SidetoneGen_Destroy() chiamato
- [ ] Chiamato PRIMA di unloadASIO()
- [ ] Compilazione senza errori

---

### Task 2.5: Modificare cmasio.c - Modificare asioOUT()

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Iniettare sidetone in asioOUT() quando attivo

**Localizzazione:** Funzione `asioOUT(int id, int nsamples, double* buff)` (linea ~111-120)

**Codice COMPLETO modificato:**

```c
void asioOUT(int id, int nsamples, double* buff)
{
    if (!pcma->run) return;

    // NEW: Check if sidetone should be injected
    if (SidetoneGen_IsActive())
    {
        // Generate synthetic sidetone
        SidetoneGen_Generate(buff, nsamples, pcm->audio_outrate);

        // Send to ASIO output
        xrmatchIN(pcma->rmatchOUT, buff);
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
- `SidetoneGen_IsActive()` check: TX on AND enabled (via callback)
- Se active: genera sidetone in `buff`, poi invia a ASIO
- Altrimenti: passa RX audio normale

**IMPORTANTE:** Questo è l'UNICO punto di modifica sostanziale in cmasio.c!

**Dependencies:** Task 2.2

**Acceptance Criteria:**
- [ ] SidetoneGen_IsActive() check corretto
- [ ] Sidetone injected quando active
- [ ] RX audio passa quando not active
- [ ] Protocol 1 logic preservata
- [ ] Compilazione senza errori

---

### Task 2.6: Creare Wrapper Exports in cmasio.c

**File:** `Project Files/Source/ChannelMaster/cmasio.c`

**Obiettivo:** Creare funzioni PORT export che wrappano SidetoneGen per C#

**Localizzazione:** Dopo asioOUT(), prima di fine file

**Codice da aggiungere:**

```c
// ============================================================================
// CMASIO Sidetone Exports (wrappers for SidetoneGen module)
// ============================================================================

PORT void setCMASIO_TXActive(int tx_active)
{
    if (pcm->audioCodecId != ASIO) return;

    SidetoneGen_SetTXActive(tx_active);
}

PORT void setCMASIO_Callbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void))
{
    if (pcm->audioCodecId != ASIO) return;

    SidetoneGen_SetCallbacks(getEnabled, getFreq, getVolume);
}
```

**Rationale:** Questi wrappers:
- Mantengono API esistente per C#
- Aggiungono check `audioCodecId == ASIO`
- Delegano a SidetoneGen module

**Dependencies:** Task 2.2

**Acceptance Criteria:**
- [ ] 2 funzioni PORT exported
- [ ] Check audioCodecId == ASIO presente
- [ ] Delegano a SidetoneGen_*
- [ ] Compilazione senza errori

---

### Task 2.7: Aggiungere Export Declarations in cmasio.h

**File:** `Project Files/Source/ChannelMaster/cmasio.h`

**Obiettivo:** Dichiarare exports per C# P/Invoke

**Localizzazione:** Sezione exports, prima di `#ifdef __cplusplus` close

**Codice da aggiungere:**

```c
// NEW: CMASIO Sidetone exports
extern __declspec(dllexport) void setCMASIO_TXActive(int tx_active);
extern __declspec(dllexport) void setCMASIO_Callbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void));
```

**Posizione:** Con altri `__declspec(dllexport)`, prima di closing bracket

**IMPORTANTE:** Questa è l'UNICA modifica a cmasio.h (solo export declarations, ZERO struct changes!)

**Dependencies:** Task 2.6

**Acceptance Criteria:**
- [ ] 2 export declarations aggiunte
- [ ] __declspec(dllexport) presente
- [ ] Signatures corrette
- [ ] ZERO modifiche a struct _cmasio
- [ ] Compilazione senza errori

---

## Phase 3: C# Integration Layer

**NOTA:** Phase 3 è identica alla versione precedente del piano (Task 2.1-2.10 del piano v1.0)

Includo qui per completezza:

### Task 3.1: Aggiungere Delegate Types in NetworkIOImports.cs

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs`

**Codice da aggiungere:**

```csharp
// Delegate types for CMASIO callbacks (C → C#)
public delegate int GetSidetoneEnabledDelegate();
public delegate int GetSidetoneFreqDelegate();
public delegate double GetSidetoneVolumeDelegate();
```

**Acceptance Criteria:**
- [ ] 3 delegate types dichiarati
- [ ] Compilazione senza errori

---

### Task 3.2: Aggiungere DllImport Functions in NetworkIOImports.cs

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs`

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

**Acceptance Criteria:**
- [ ] CallingConvention.Cdecl specificato
- [ ] Signatures corrette
- [ ] Compilazione senza errori

---

### Task 3.3: Creare Class CMASIOSidetone in cmaster.cs

**File:** `Project Files/Source/Console/cmaster.cs`

**Codice completo:**

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
}
```

**Acceptance Criteria:**
- [ ] Delegates stored in static fields (GC protection)
- [ ] All 3 callbacks implemented
- [ ] Exception handling presente
- [ ] Compilazione senza errori

---

### Task 3.4: Modificare console.cs - Hook MOX Property

**File:** `Project Files/Source/Console/console.cs`

**Localizzazione:** Trovare `public bool MOX` property setter (linea ~13000-14000)

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

**Acceptance Criteria:**
- [ ] Check CurrentAudioCodec == ASIO
- [ ] CMASIOSidetone.SetTXActive() chiamato
- [ ] Logica MOX esistente preservata
- [ ] Compilazione senza errori

---

### Task 3.5: Modificare console.cs - Initialize Callbacks

**File:** `Project Files/Source/Console/console.cs`

**Localizzazione:** Costruttore Console() o metodo inizializzazione

**Modifiche da applicare:**

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

**Acceptance Criteria:**
- [ ] InitializeCallbacks() chiamato durante startup
- [ ] Chiamato DOPO InitializeComponent()
- [ ] Compilazione senza errori

---

## Phase 4: Build & Testing

### Task 4.1: Build ChannelMaster.dll

**Steps:**
1. Aprire soluzione ChannelMaster
2. Verify `sidetonegen.c` e `sidetonegen.h` nel progetto
3. Build → Build Solution
4. Verificare output: `ChannelMaster.dll`

**Expected Output:**
- Build SUCCESS
- DLL size increased ~10-15 KB
- No errors, minimal warnings

**Acceptance Criteria:**
- [ ] Build successo
- [ ] sidetonegen.obj generated
- [ ] DLL exports verified (dumpbin /EXPORTS)

---

### Task 4.2: Build Thetis

**Steps:**
1. Aprire soluzione Thetis
2. Copy nuovo ChannelMaster.dll in bin directory
3. Build → Build Solution
4. Verificare output: `Thetis.exe`

**Acceptance Criteria:**
- [ ] Build successo
- [ ] Thetis.exe updated
- [ ] ChannelMaster.dll in bin directory

---

### Task 4.3-4.12: Test Funzionali

**IDENTICI a Task 3.3-3.10 del piano v1.0:**

- Task 4.3: Test sidetone in Semi Break-In
- Task 4.4: Test fade (no clicks)
- Task 4.5: Test frequency parameter
- Task 4.6: Test volume parameter
- Task 4.7: Test AGC recovery (<10ms)
- Task 4.8: Test real-time callbacks
- Task 4.9: Test QSK mode preserved
- Task 4.10: Test disable functionality

(Vedi piano v1.0 per dettagli completi)

---

## Rollback Plan

### Full Rollback (rimuovere modulo SidetoneGen)

**Step 1: Rimuovere file:**
```bash
rm "Project Files/Source/ChannelMaster/sidetonegen.h"
rm "Project Files/Source/ChannelMaster/sidetonegen.c"
```

**Step 2: Revert modifiche cmasio.c:**
```bash
git checkout HEAD -- "Project Files/Source/ChannelMaster/cmasio.c"
git checkout HEAD -- "Project Files/Source/ChannelMaster/cmasio.h"
```

**Step 3: Revert modifiche C#:**
```bash
git checkout HEAD -- "Project Files/Source/Console/HPSDR/NetworkIOImports.cs"
git checkout HEAD -- "Project Files/Source/Console/cmaster.cs"
git checkout HEAD -- "Project Files/Source/Console/console.cs"
```

**Step 4: Rimuovere da progetto:**
- Aprire ChannelMaster.vcxproj
- Rimuovere references a sidetonegen.c/h
- Rebuild

**Step 5: Commit rollback:**
```bash
git commit -m "Rollback: Remove SidetoneGen module"
git push
```

### Partial Rollback (disabilitare senza rimuovere)

**In cmasio.c - asioOUT():**
```c
void asioOUT(int id, int nsamples, double* buff)
{
    if (!pcma->run) return;

    // DISABLE SIDETONE: Always use RX audio
    // if (SidetoneGen_IsActive()) {
    //     SidetoneGen_Generate(buff, nsamples, pcm->audio_outrate);
    //     xrmatchIN(pcma->rmatchOUT, buff);
    // }
    // else {
        xrmatchIN(pcma->rmatchOUT, buff);
    // }

    // ... rest unchanged ...
}
```

Rebuild ChannelMaster.dll - sidetone completamente disabilitato ma codice preservato.

---

## Summary: Modifiche per File

### File NUOVI (2):
- `sidetonegen.h` - 50 righe
- `sidetonegen.c` - 200 righe

### File MODIFICATI (6):

**ChannelMaster (C layer):**
- `cmasio.h` - **SOLO 2 export declarations** (5 righe)
- `cmasio.c` - **~15 righe totali**:
  - 1 riga: #include "sidetonegen.h"
  - 2 righe: Initialize/Destroy calls
  - 5 righe: asioOUT() modification
  - 7 righe: Wrapper exports
- `ChannelMaster.vcxproj` - 2 righe (file includes)

**Thetis (C# layer):**
- `NetworkIOImports.cs` - 10 righe (delegates + DllImports)
- `cmaster.cs` - 100 righe (class CMASIOSidetone)
- `console.cs` - 5 righe (MOX hook + Initialize call)

### Totale LOC Aggiunte:
- **C layer:** ~250 righe (220 in modulo isolato + 30 integration)
- **C# layer:** ~115 righe
- **Totale:** ~365 righe

### Invasività Modifiche:
✅ **ZERO modifiche a struct `_cmasio`**
✅ **MINIME modifiche a cmasio.c** (~15 righe su ~2000)
✅ **ZERO modifiche a cmasio.h struct** (solo exports)
✅ **Completamente disabilitabile** (comment 3 righe in asioOUT)

---

## Checklist Finale

### Phase 1: Modulo SidetoneGen
- [ ] Task 1.1: sidetonegen.h created
- [ ] Task 1.2: sidetonegen.c implemented

### Phase 2: Integrazione CMASIO
- [ ] Task 2.1: sidetonegen.c added to project
- [ ] Task 2.2: #include in cmasio.c
- [ ] Task 2.3: Initialize in create_cmasio()
- [ ] Task 2.4: Destroy in destroy_cmasio()
- [ ] Task 2.5: asioOUT() modified
- [ ] Task 2.6: Wrapper exports in cmasio.c
- [ ] Task 2.7: Export declarations in cmasio.h

### Phase 3: C# Integration
- [ ] Task 3.1: Delegates in NetworkIOImports.cs
- [ ] Task 3.2: DllImports in NetworkIOImports.cs
- [ ] Task 3.3: CMASIOSidetone class
- [ ] Task 3.4: MOX hook
- [ ] Task 3.5: Initialize callbacks

### Phase 4: Build & Testing
- [ ] Task 4.1: Build ChannelMaster.dll
- [ ] Task 4.2: Build Thetis
- [ ] Task 4.3: Test sidetone funzionale
- [ ] Task 4.4: Test fade envelope
- [ ] Task 4.5: Test frequency
- [ ] Task 4.6: Test volume
- [ ] Task 4.7: Test AGC recovery
- [ ] Task 4.8: Test callbacks real-time
- [ ] Task 4.9: Test QSK preserved
- [ ] Task 4.10: Test disable

---

**END OF IMPLEMENTATION PLAN v2.0**
