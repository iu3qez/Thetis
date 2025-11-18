# PRD: CMASIO Local Sidetone Implementation
## Product Requirements Document

**Version:** 1.0
**Date:** 2025-11-18
**Author:** Analysis based on Thetis codebase
**Status:** Proposal

---

## 1. Executive Summary

### 1.1 Purpose
Implement synthetic local sidetone generation for CMASIO mode to resolve AGC saturation and provide independent volume control.

### 1.2 Current Problem
In CMASIO mode, the sidetone is NOT a true sidetone but a **full-duplex RX monitor** that demodulates the transmitted signal. This causes two critical issues:

1. **AGC Saturation:** RX receiver demodulates the transmitted signal (extremely strong), causing AGC to attenuate heavily. Recovery time is **1-2 seconds** after returning to RX.
2. **No Independent Volume Control:** RX volume and "sidetone" volume are the same (both are RX audio).

### 1.3 Solution
Inject a **synthetic local sidetone** (sine wave with fade in/out) during TX, replacing the demodulated signal approach.

---

## 2. Current Implementation Analysis

### 2.1 CW Break-In Modes

**File:** `Project Files/Source/Console/enums.cs:397-402`

```csharp
public enum BreakIn
{
    Manual,  // No break-in (manual PTT)
    Semi,    // Semi break-in (return to RX after delay)
    QSK      // Full break-in (fast RX/TX switching between CW characters)
}
```

### 2.2 Current CMASIO Audio Flow

**File:** `Project Files/Source/ChannelMaster/cmasio.c:111-120`

```c
void asioOUT(int id, int nsamples, double* buff)
{
    if (!pcma->run) return;
    xrmatchIN(pcma->rmatchOUT, buff);  // Audio from mixer (includes demodulated TX!)

    if (pcma->protocol == 0)  // Protocol 1 mode
    {
        memset(buff, 0, nsamples * sizeof(complex));  // Zero buffer
        OutBound(0, nsamples, buff);  // Send empty buffer to network
    }
}
```

**Problem:** `buff` contains RX audio which, during TX, is the **demodulated transmitted signal** (monitor mode).

### 2.3 Break-In Delay Management

**File:** `Project Files/Source/Console/console.cs:18419-18429`

```csharp
private double break_in_delay = 300;  // milliseconds
public double BreakInDelay
{
    get { return break_in_delay; }
    set
    {
        break_in_delay = value;
        udCWBreakInDelay.Value = (int)value;
        if (BreakInEnabledState != CheckState.Unchecked)
            NetworkIO.SetCWHangTime((int)value + key_up_delay);
        else
            NetworkIO.SetCWHangTime(0);
    }
}
```

**File:** `Project Files/Source/ChannelMaster/netInterface.c:1070-1077`

```c
PORT void SetCWSidetone(int enable)
{
    if (prn->cw.sidetone != enable)
    {
        prn->cw.sidetone = enable;
        if (listenSock != INVALID_SOCKET)
            CmdTx();  // Send command to hardware radio
    }
}
```

**Note:** Hardware sidetone (via network) has unacceptable latency for CMASIO low-latency mode.

---

## 3. Proposed Architecture

### 3.1 High-Level Design

```
┌─────────────────────────────────────────────┐
│          CW Mode (CMASIO)                   │
└─────────────────┬───────────────────────────┘
                  │
        ┌─────────▼──────────┐
        │   Semi Break-In?   │
        └─────────┬──────────┘
                  │
         ┌────────▼──────────┐
         │   QSK Mode?       │
         └────────┬──────────┘
                  │
        ┌─────────▼──────────────────┐
        │  TX Active (MOX=1)?        │
        └─────────┬──────────────────┘
                  │
         YES      │       NO
         ┌────────▼──────────────┐
         │ Generate Local        │    ┌──────────────────┐
         │ Sidetone (Synthetic)  │    │ Pass RX Audio    │
         │  • Sine wave          │    │ (Normal)         │
         │  • Fade in/out        │    └──────────────────┘
         │  • Independent volume │
         └───────────────────────┘
                  │
         ┌────────▼────────────────┐
         │   asioOUT() callback    │
         │   → ASIO local output   │
         └─────────────────────────┘
```

### 3.2 Sidetone Generation Flow

**Target Modes:**
- **Semi Break-In:** Currently "mute" (no sidetone) → **INJECT SYNTHETIC SIDETONE**
- **QSK Mode:** Currently uses RX monitor (AGC issue) → **Will remain as is**

---

## 4. Detailed Implementation

### 4.1 New Data Structures

**File:** `Project Files/Source/ChannelMaster/cmasio.h` (modify)

```c
typedef struct _cmasio
{
    // ... existing fields ...

    // NEW: Sidetone generation - LOCAL STATE ONLY
    // NOTE: Sidetone parameters (enabled, freq, volume) are NOT duplicated here.
    //       They are read via callbacks from existing C# application state:
    //       - sidetone_enabled → console.cs:cw_sidetone (bool)
    //       - sidetone_freq → console.cs:cw_pitch (int Hz)
    //       - sidetone_volume → console.cs:qsk_sidetone_volume (int 0-100)

    int tx_active;                   // Flag: 1 = TX mode, 0 = RX mode
    double sidetone_phase;           // Oscillator phase accumulator
    double* sidetone_buffer;         // Buffer for generated sidetone

    // Fade state management
    int fade_state;                  // 0=idle, 1=fade_in, 2=active, 3=fade_out
    int fade_counter;                // Current fade sample count
    int fade_samples;                // Fade duration (const: 48 @ 48kHz = 1ms)
} cmasio, *CMASIO;

// Callback function pointers for reading C# application state
static int (*pGetSidetoneEnabled)(void) = NULL;
static int (*pGetSidetoneFreq)(void) = NULL;
static double (*pGetSidetoneVolume)(void) = NULL;
```

### 4.2 Initialization

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (modify `create_cmasio()`)

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

### 4.3 Cleanup

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (modify `destroy_cmasio()`)

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

### 4.4 Sidetone Generator

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (new function)

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

### 4.5 Modified asioOUT()

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (modify existing function)

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

### 4.6 TX State Management and Callback Setup

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (new exports)

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

**File:** `Project Files/Source/ChannelMaster/cmasio.h` (add exports)

```c
extern __declspec(dllexport) void setCMASIO_TXActive(int tx_active);
extern __declspec(dllexport) void setCMASIO_Callbacks(
    int (*getEnabled)(void),
    int (*getFreq)(void),
    double (*getVolume)(void));
```

### 4.7 C# Integration (Callback-Based Architecture)

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs` (add imports)

```csharp
// Delegate types for C callbacks
public delegate int GetSidetoneEnabledDelegate();
public delegate int GetSidetoneFreqDelegate();
public delegate double GetSidetoneVolumeDelegate();

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_TXActive(int tx_active);

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_Callbacks(
    GetSidetoneEnabledDelegate getEnabled,
    GetSidetoneFreqDelegate getFreq,
    GetSidetoneVolumeDelegate getVolume);
```

**File:** `Project Files/Source/Console/cmaster.cs` (add wrapper class)

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
        // Create delegates and keep references to prevent GC
        _getEnabledDelegate = GetCMASIOSidetoneEnabled;
        _getFreqDelegate = GetCMASIOSidetoneFreq;
        _getVolumeDelegate = GetCMASIOSidetoneVolume;

        // Register callbacks with C layer
        NetworkIO.setCMASIO_Callbacks(
            _getEnabledDelegate,
            _getFreqDelegate,
            _getVolumeDelegate);
    }

    // Callback: Read sidetone enabled state from existing console parameter
    private static int GetCMASIOSidetoneEnabled()
    {
        // Uses existing console.cs:cw_sidetone (line 14987)
        return Console.CurrentConsole.CWSidetone ? 1 : 0;
    }

    // Callback: Read sidetone frequency from existing console parameter
    private static int GetCMASIOSidetoneFreq()
    {
        // Uses existing console.cs:cw_pitch (line 18099)
        return Console.CurrentConsole.CWPitch;
    }

    // Callback: Read sidetone volume from existing console parameter
    private static double GetCMASIOSidetoneVolume()
    {
        // Uses existing console.cs:qsk_sidetone_volume (line 12913)
        // Semi Break-In: use TXAF, QSK: use qsk_sidetone_volume
        if (Console.CurrentConsole.CurrentBreakInMode == BreakIn.Semi)
        {
            // For Semi mode, use TX AF level (0-100)
            return (double)Console.CurrentConsole.TXAF / 100.0;
        }
        else
        {
            // For other modes, use QSK sidetone volume
            return (double)Console.CurrentConsole.QSKSidetoneVolume / 100.0;
        }
    }

    // Notify C layer of TX state change
    public static void SetTXActive(bool tx_active)
    {
        NetworkIO.setCMASIO_TXActive(tx_active ? 1 : 0);
    }
}
```

**File:** `Project Files/Source/Console/console.cs` (hook MOX changes)

Find the MOX property setter (around line 13000-14000) and add:

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

**File:** `Project Files/Source/Console/console.cs` (initialize callbacks during startup)

In the Console constructor or initialization method (e.g., `setupPowerThread()` or `InitializeComponent()`):

```csharp
// Initialize CMASIO callbacks during startup
CMASIOSidetone.InitializeCallbacks();
```

### 4.8 Existing GUI Controls (No Changes Required!)

**Using existing controls - NO NEW GUI needed:**

The callback-based architecture uses **existing console parameters**, so the user interface remains unchanged:

1. **Sidetone Enable:** Existing "CW Sidetone" checkbox (`console.cs:cw_sidetone`)
2. **Sidetone Frequency:** Existing "CW Pitch" control (`console.cs:cw_pitch`)
3. **Sidetone Volume:**
   - Semi Break-In: Uses existing "TX AF" slider (`console.cs:TXAF`)
   - QSK Mode: Uses existing "QSK Sidetone Volume" (`console.cs:qsk_sidetone_volume`)

**Benefits:**
✅ Zero GUI changes required
✅ No new database columns needed
✅ Settings already persist via existing database schema
✅ Users already familiar with these controls

---

## 5. Benefits and Impact

### 5.1 Benefits

| Benefit | Current (RX Monitor) | Proposed (Synthetic) |
|---------|---------------------|----------------------|
| **AGC Recovery** | **1-2 seconds delay** | Instant (no AGC saturation) |
| **Volume Control** | Coupled with RX | Independent control (existing sliders) |
| **Audio Quality** | Demodulated (artifacts) | Pure sine wave |
| **Latency** | Demodulation delay | Generation only (~0.5ms) |
| **CPU Usage** | RX DSP chain active | Minimal (simple sine) |
| **GUI Changes** | N/A | **None required!** (uses existing controls) |
| **Parameter Duplication** | N/A | **Zero** (callback-based) |

### 5.2 Impact Analysis

**Low Risk Areas:**
- ✅ CMASIO mode only (doesn't affect normal HPSDR operation)
- ✅ Backward compatible (can be disabled via checkbox)
- ✅ Isolated changes (new functions, minimal modifications)

**Medium Risk Areas:**
- ⚠️ MOX hook integration (must ensure all MOX paths call notification)
- ⚠️ Fade timing (must be tested for click-free operation)

---

## 6. Testing Plan

### 6.1 Unit Tests

1. **Sidetone Generator:**
   - Verify sine wave generation at various frequencies (200-1200 Hz)
   - Verify fade in/out envelope (no clicks)
   - Verify volume control (0%-100%)

2. **State Management:**
   - Verify TX ON triggers fade-in
   - Verify TX OFF triggers fade-out
   - Verify phase continuity across multiple TX periods

### 6.2 Integration Tests

1. **Semi Break-In Mode:**
   - Enable Semi mode
   - Send CW (keyer or manual)
   - Verify sidetone audible during TX
   - Verify sidetone stops after break-in delay
   - Measure AGC recovery time (should be instant)

2. **QSK Mode:**
   - Enable QSK mode
   - Send CW at various speeds (15-40 WPM)
   - Verify sidetone follows keying accurately
   - Verify no clicks/pops during fast transitions

3. **Volume Independence:**
   - Set RX volume to 50%
   - Set CMASIO Sidetone volume to 10%
   - Verify sidetone is quieter than RX audio
   - Change RX volume: verify sidetone unchanged

### 6.3 Performance Tests

1. **Latency Measurement:**
   - Measure time from key-down to sidetone onset
   - Target: < 5ms (ASIO buffer size dependent)

2. **CPU Usage:**
   - Monitor CPU during CW TX with sidetone
   - Verify < 1% additional CPU usage

---

## 7. Configuration & Persistence (No Changes Required!)

**Using existing database schema - NO NEW COLUMNS needed:**

Settings automatically persist via existing console parameters:

- **CW Sidetone Enable:** Already saved via `console.cs:cw_sidetone`
- **CW Pitch (frequency):** Already saved via `console.cs:cw_pitch`
- **Sidetone Volume:** Already saved via `console.cs:qsk_sidetone_volume` and `console.cs:TXAF`

**Benefits:**
✅ Zero database schema changes
✅ Settings persist automatically via existing mechanisms
✅ No migration required
✅ Backward compatible with older database versions

---

## 8. Future Enhancements (Out of Scope for v1.0)

1. **Configurable Waveforms:**
   - Square wave (classic sidetone)
   - Triangle wave (softer)
   - Custom ADSR envelope

2. **Stereo Panning:**
   - Pan sidetone left/right
   - Useful for dual-watch operation

3. **Frequency Tracking:**
   - Auto-adjust sidetone to match CW pitch setting
   - Link to `udCWPitch` control

4. **Advanced Fade Control:**
   - User-configurable fade time (1-20ms)
   - Different fade curves (linear, exponential)

---

## 9. Code Reference Summary

### 9.1 Files to Modify

| File Path | Changes | Lines |
|-----------|---------|-------|
| `ChannelMaster/cmasio.h` | Add struct fields (LOCAL ONLY), callback pointers, exports | +12 |
| `ChannelMaster/cmasio.c` | Add generator, modify asioOUT(), callback setup, TX state | +85 |
| `Console/HPSDR/NetworkIOImports.cs` | Add DllImports, delegates | +10 |
| `Console/cmaster.cs` | Add wrapper class with callbacks | +65 |
| `Console/console.cs` | Hook MOX changes, initialize callbacks | +8 |

**Total Estimated LOC:** ~180 lines (including comments)

**Files NOT modified (using existing):**
- ❌ `Console/setup.cs` - No GUI changes
- ❌ `Console/Database.cs` - No schema changes

### 9.2 Key Functions

| Function | File | Purpose |
|----------|------|---------|
| `generateLocalSidetone()` | cmasio.c | Generate sine wave with fade (uses callbacks) |
| `asioOUT()` | cmasio.c | Modified to inject sidetone (reads enabled via callback) |
| `setCMASIO_TXActive()` | cmasio.c | Notify TX state change (fade in/out) |
| `setCMASIO_Callbacks()` | cmasio.c | Register C# callbacks for parameter access |
| `CMASIOSidetone.InitializeCallbacks()` | cmaster.cs | Register callbacks during startup |
| `GetCMASIOSidetoneEnabled()` | cmaster.cs | Callback to read `console.cw_sidetone` |
| `GetCMASIOSidetoneFreq()` | cmaster.cs | Callback to read `console.cw_pitch` |
| `GetCMASIOSidetoneVolume()` | cmaster.cs | Callback to read `console.qsk_sidetone_volume` or `TXAF` |
| `MOX` property | console.cs | Hook to notify CMASIO of TX state |

### 9.3 Integration Points

1. **Callback Registration (Startup):**
   - `CMASIOSidetone.InitializeCallbacks()` called during console initialization
   - Registers three callbacks: enabled, freq, volume
   - Delegates kept alive to prevent GC collection

2. **MOX State Detection:**
   - `console.cs:MOX` property setter
   - `Audio.MOX` changes
   - CAT command MOX changes
   - All call `CMASIOSidetone.SetTXActive()`

3. **Parameter Reading (via Callbacks):**
   - `console.cs:cw_sidetone` → sidetone enabled/disabled
   - `console.cs:cw_pitch` → sidetone frequency (200-1200 Hz)
   - `console.cs:qsk_sidetone_volume` or `TXAF` → sidetone volume (0-100%)

4. **Break-In Mode:**
   - `console.cs:CurrentBreakInMode` (QSK/Semi/Manual)
   - Used in volume callback to select TXAF vs qsk_sidetone_volume

5. **Audio Codec Selection:**
   - `cmaster.cs:audioCodecId == ASIO`
   - `CMASIOConfig.cs` (registry settings)

---

## 10. Acceptance Criteria

### 10.1 Functional Requirements

- [ ] Sidetone audible during TX in Semi Break-In mode (currently mute)
- [ ] QSK mode remains unchanged (keep "as is" per user request)
- [ ] No sidetone in Manual mode (PTT only)
- [ ] Sidetone frequency follows existing "CW Pitch" control (200-1200 Hz)
- [ ] Sidetone volume follows existing "TX AF" or "QSK Sidetone Volume" (independent of RX)
- [ ] Sidetone enable/disable follows existing "CW Sidetone" checkbox
- [ ] Fade in/out < 2ms (no clicks)
- [ ] Settings persist automatically via existing database (no new columns)

### 10.2 Performance Requirements

- [ ] AGC recovery < 10ms (vs. **1-2 seconds** current)
- [ ] Sidetone latency < 5ms (key-down to audio)
- [ ] CPU overhead < 1%
- [ ] No audio dropouts during fast CW (40+ WPM)
- [ ] No parameter duplication (callbacks read from C#)

### 10.3 User Experience

- [ ] **Zero GUI changes** (uses existing controls)
- [ ] **Zero new database columns** (uses existing parameters)
- [ ] Disable option available via existing "CW Sidetone" checkbox
- [ ] No impact on non-CMASIO modes
- [ ] Automatic synchronization (GUI changes immediate)

---

## 11. Rollout Plan

### Phase 1: Core Implementation (Week 1)
- Add callback function pointers to cmasio.h
- Implement `generateLocalSidetone()` function with callback-based parameter reading
- Modify `asioOUT()` for TX detection (read enabled via callback)
- Add `setCMASIO_TXActive()` for TX state management
- Add `setCMASIO_Callbacks()` for callback registration

### Phase 2: C# Integration (Week 2)
- Add DllImports and delegates in NetworkIOImports.cs
- Implement C# wrapper class with callback providers
- Create `GetCMASIOSidetoneEnabled()`, `GetCMASIOSidetoneFreq()`, `GetCMASIOSidetoneVolume()` callbacks
- Hook MOX changes in console.cs to call `SetTXActive()`
- Initialize callbacks during console startup

### Phase 3: Testing & Refinement (Week 3)
- Unit tests for generator (verify callbacks read correct values)
- Integration tests with real hardware
- Tune fade timing for click-free operation
- Verify existing GUI controls work correctly (no new controls!)
- Test parameter synchronization (callback accuracy)

### Phase 4: Documentation & Merge (Week 4)
- User documentation (explain which existing controls affect CMASIO sidetone)
- Code review
- Merge to main branch

---

## 12. Architecture: Callback-Based Parameter Access

### 12.1 Design Rationale

The **callback-based architecture** was chosen to avoid parameter duplication and maintain clean separation of concerns:

**Problem Avoided:**
- ❌ Duplicating `sidetone_enabled`, `sidetone_freq`, `sidetone_volume` in CMASIO struct
- ❌ Creating new GUI controls for CMASIO-specific settings
- ❌ Adding new database columns for persistence
- ❌ Synchronizing duplicate parameters between C and C#

**Solution:**
- ✅ CMASIO reads parameters **on-demand** via C→C# callbacks
- ✅ Single source of truth (existing console parameters)
- ✅ Zero duplication, automatic synchronization
- ✅ Minimal CMASIO state (only local: phase, fade, buffer)

### 12.2 Parameter Mapping

| CMASIO Needs | Existing Console Parameter | Callback |
|--------------|---------------------------|----------|
| Sidetone Enabled | `console.cs:cw_sidetone` (line 14987) | `GetCMASIOSidetoneEnabled()` |
| Sidetone Frequency | `console.cs:cw_pitch` (line 18099) | `GetCMASIOSidetoneFreq()` |
| Sidetone Volume (Semi) | `console.cs:TXAF` | `GetCMASIOSidetoneVolume()` |
| Sidetone Volume (QSK) | `console.cs:qsk_sidetone_volume` (line 12913) | `GetCMASIOSidetoneVolume()` |

### 12.3 Callback Lifecycle

```
1. Console Startup
   ↓
2. CMASIOSidetone.InitializeCallbacks()
   ↓
3. Create delegates (kept alive to prevent GC)
   ↓
4. Call setCMASIO_Callbacks() (C layer)
   ↓
5. C layer stores function pointers
   ↓
6. ASIO Callback (audio thread)
   ↓
7. generateLocalSidetone() called
   ↓
8. Reads freq/volume via pGetSidetoneFreq(), pGetSidetoneVolume()
   ↓
9. Callbacks execute in C# (read console properties)
   ↓
10. Return current values to C
```

### 12.4 Thread Safety Considerations

**Callback Execution:**
- Callbacks executed from ASIO audio thread (real-time priority)
- Read-only access to console properties (no modification)
- Simple property getters (minimal latency)

**GC Protection:**
- Delegates stored in static fields (`_getEnabledDelegate`, etc.)
- Prevents garbage collection during application lifetime
- Function pointers remain valid until application exit

### 12.5 Benefits Summary

| Aspect | Traditional (Duplicate) | Callback-Based (Chosen) |
|--------|------------------------|-------------------------|
| **LOC** | ~312 lines | **~180 lines** (42% reduction) |
| **GUI Changes** | New controls required | **Zero** |
| **Database Changes** | New columns required | **Zero** |
| **Synchronization** | Manual, error-prone | **Automatic** |
| **Single Source of Truth** | No (duplicated) | **Yes** |
| **User Confusion** | Two places to set values | **One place** (existing) |
| **Maintenance** | Higher (keep in sync) | **Lower** |

---

## 13. References

### 13.1 Related Code Files

- `Project Files/Source/ChannelMaster/cmasio.c` - CMASIO audio routing
- `Project Files/Source/ChannelMaster/cmasio.h` - CMASIO header
- `Project Files/Source/Console/console.cs` - Main console (MOX, break-in)
- `Project Files/Source/Console/enums.cs` - BreakIn enum definition
- `Project Files/Source/ChannelMaster/netInterface.c` - Network protocol
- `Project Files/Source/Console/HPSDR/NetworkIOImports.cs` - DllImports
- `Project Files/Source/Console/cmaster.cs` - C# wrapper classes

### 13.2 Related Functionality and Existing Parameters

**Existing Parameters Used by CMASIO (via callbacks):**
- **CW Sidetone Enable:** `console.cs:14987` (`cw_sidetone` bool)
- **CW Pitch (Frequency):** `console.cs:18099` (`cw_pitch` int, 200-1200 Hz)
- **QSK Sidetone Volume:** `console.cs:12913` (`qsk_sidetone_volume` int, 0-100)
- **TX AF Level:** `console.cs` (`TXAF` property, used for Semi Break-In volume)

**Related Functionality:**
- **CW Pitch Property:** `console.cs:18103-18119` (CWPitch property getter/setter)
- **CW Sidetone (Hardware):** `console.cs:12969-12994` (setCWSideToneVolume - network sidetone)
- **Break-In Delay:** `console.cs:18419-18429` (BreakInDelay property)
- **Break-In Mode:** `console.cs` (CurrentBreakInMode property - QSK/Semi/Manual)
- **ASIO Callback:** `cmasio.c:122-176` (CallbackASIO function)

---

## Appendix A: Current Audio Flow Diagram

```
┌────────────────────────────────────────────────────────────┐
│                   CURRENT CMASIO (Problem)                 │
└────────────────────────────────────────────────────────────┘

TX Mode:
  ┌──────────┐         ┌──────────────┐         ┌─────────┐
  │ TX Audio │────────▶│ Transmitter  │────────▶│  Radio  │
  │ (Mic)    │         │              │         │Hardware │
  └──────────┘         └──────────────┘         └────┬────┘
                                                       │
                                                  ┌────▼────┐
                                                  │ Radio   │
                                                  │ TX Out  │
                                                  └────┬────┘
                                                       │
  ┌──────────┐         ┌──────────────┐         ┌────▼────┐
  │ RX Still │◀────────│ RX Receiver  │◀────────│ Antenna │
  │ ACTIVE!  │         │ (demodulates │         │ (strong │
  │          │         │  own TX!)    │         │ signal) │
  └────┬─────┘         └──────────────┘         └─────────┘
       │
       │ Demodulated TX = "sidetone" (but saturates AGC!)
       │
  ┌────▼────────┐
  │   ASIO      │
  │  Headphones │
  └─────────────┘

Problem: AGC sees strong signal, attenuates heavily, slow recovery.
```

## Appendix B: Proposed Audio Flow Diagram

```
┌────────────────────────────────────────────────────────────┐
│                  PROPOSED CMASIO (Solution)                │
└────────────────────────────────────────────────────────────┘

TX Mode:
  ┌──────────┐         ┌──────────────┐         ┌─────────┐
  │ TX Audio │────────▶│ Transmitter  │────────▶│  Radio  │
  │ (Mic)    │         │              │         │Hardware │
  └──────────┘         └──────────────┘         └─────────┘

  ┌──────────────────┐
  │ RX MUTED         │  ← No AGC saturation!
  │ (or disabled)    │
  └──────────────────┘

  ┌──────────────────┐
  │ Generate LOCAL   │
  │ Sidetone:        │
  │ • Sine wave 600Hz│
  │ • Fade in/out    │
  │ • Volume control │
  └────────┬─────────┘
           │
  ┌────────▼────────┐
  │   ASIO Output   │
  │   (Direct)      │
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  Headphones     │
  │  (Clean tone,   │
  │   no AGC delay) │
  └─────────────────┘

Benefits:
✅ No AGC saturation
✅ Instant RX recovery
✅ Independent volume control
✅ Clean sine wave
```

---

## Document Change Log

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-11-18 | Analysis | Initial PRD creation |
| 1.1 | 2025-11-18 | Analysis | **Major revision: Callback-based architecture**<br>- Removed parameter duplication from CMASIO struct<br>- Added C→C# callbacks for parameter access<br>- Removed new GUI controls (uses existing)<br>- Removed database schema changes (uses existing)<br>- Updated AGC recovery time: 100-500ms → **1-2s**<br>- Reduced LOC estimate: 312 → **180** (42% reduction)<br>- Added Section 12: Architecture documentation |

---

**END OF DOCUMENT**
