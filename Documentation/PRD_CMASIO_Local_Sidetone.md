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

1. **AGC Saturation:** RX receiver demodulates the transmitted signal (extremely strong), causing AGC to attenuate heavily. Recovery time is 1-2s after returning to RX.
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

    // NEW: Sidetone generation
    int tx_active;                   // Flag: 1 = TX mode, 0 = RX mode
    int sidetone_enabled;            // Flag: enable sidetone generation
    double sidetone_freq;            // Frequency in Hz (default: 600.0)
    double sidetone_volume;          // Volume 0.0-1.0 (default: 0.5)
    double sidetone_phase;           // Oscillator phase accumulator
    int sidetone_fade_samples;       // Fade in/out samples (default: 48 @ 48kHz = 1ms)
    double* sidetone_buffer;         // Buffer for generated sidetone

    // NEW: Fade state management
    int fade_state;                  // 0=idle, 1=fade_in, 2=active, 3=fade_out
    int fade_counter;                // Current fade sample count
} cmasio, *CMASIO;
```

### 4.2 Initialization

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (modify `create_cmasio()`)

```c
void create_cmasio()
{
    // ... existing initialization ...

    // NEW: Initialize sidetone parameters
    pcma->tx_active = 0;
    pcma->sidetone_enabled = 1;              // Enabled by default
    pcma->sidetone_freq = 600.0;             // 600 Hz default
    pcma->sidetone_volume = 0.5;             // 50% volume
    pcma->sidetone_phase = 0.0;
    pcma->sidetone_fade_samples = 48;        // 1ms @ 48kHz
    pcma->fade_state = 0;
    pcma->fade_counter = 0;

    // Allocate sidetone buffer
    pcma->sidetone_buffer = (double*)malloc0(pcma->blocksize * sizeof(complex));

    if (pcma->sidetone_buffer == NULL) {
        OutputDebugStringA("ERROR: Failed to allocate sidetone buffer");
    }
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
void generateLocalSidetone(double* buffer, int nsamples,
                           double freq, double volume,
                           double* phase, int fade_state,
                           int* fade_counter, int fade_samples,
                           int samplerate)
{
    double delta_phase = 2.0 * M_PI * freq / samplerate;

    for (int i = 0; i < nsamples; i++)
    {
        // Generate sine wave
        double sample = sin(*phase) * volume;

        // Apply fade envelope
        double envelope = 1.0;
        if (fade_state == 1)  // Fade IN
        {
            envelope = (double)(*fade_counter) / fade_samples;
            (*fade_counter)++;
            if (*fade_counter >= fade_samples) {
                fade_state = 2;  // Transition to active
            }
        }
        else if (fade_state == 3)  // Fade OUT
        {
            envelope = 1.0 - ((double)(*fade_counter) / fade_samples);
            (*fade_counter)++;
            if (*fade_counter >= fade_samples) {
                fade_state = 0;  // Transition to idle
                envelope = 0.0;
            }
        }

        sample *= envelope;

        // Stereo output (L+R identical)
        buffer[2*i] = sample;      // Left
        buffer[2*i+1] = sample;    // Right

        // Update phase
        *phase += delta_phase;
        if (*phase >= 2.0 * M_PI)
            *phase -= 2.0 * M_PI;
    }
}
```

### 4.5 Modified asioOUT()

**File:** `Project Files/Source/ChannelMaster/cmasio.c` (modify existing function)

```c
void asioOUT(int id, int nsamples, double* buff)
{
    if (!pcma->run) return;

    // NEW: Check if TX is active and sidetone should be generated
    if (pcma->tx_active && pcma->sidetone_enabled)
    {
        // GENERATE LOCAL SIDETONE instead of passing RX audio
        generateLocalSidetone(
            pcma->sidetone_buffer,
            nsamples,
            pcma->sidetone_freq,
            pcma->sidetone_volume,
            &pcma->sidetone_phase,
            pcma->fade_state,
            &pcma->fade_counter,
            pcma->sidetone_fade_samples,
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

### 4.6 TX State Management

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

// Set sidetone frequency (Hz)
PORT void setCMASIO_SidetoneFreq(double freq)
{
    if (pcm->audioCodecId != ASIO) return;
    if (freq >= 200.0 && freq <= 1200.0)  // Sanity check
    {
        pcma->sidetone_freq = freq;
    }
}

// Set sidetone volume (0.0-1.0)
PORT void setCMASIO_SidetoneVolume(double volume)
{
    if (pcm->audioCodecId != ASIO) return;
    if (volume >= 0.0 && volume <= 1.0)  // Sanity check
    {
        pcma->sidetone_volume = volume;
    }
}

// Enable/disable sidetone generation
PORT void setCMASIO_SidetoneEnable(int enable)
{
    if (pcm->audioCodecId != ASIO) return;
    pcma->sidetone_enabled = enable;
}
```

**File:** `Project Files/Source/ChannelMaster/cmasio.h` (add exports)

```c
extern __declspec(dllexport) void setCMASIO_TXActive(int tx_active);
extern __declspec(dllexport) void setCMASIO_SidetoneFreq(double freq);
extern __declspec(dllexport) void setCMASIO_SidetoneVolume(double volume);
extern __declspec(dllexport) void setCMASIO_SidetoneEnable(int enable);
```

### 4.7 C# Integration

**File:** `Project Files/Source/Console/HPSDR/NetworkIOImports.cs` (add imports)

```csharp
[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_TXActive(int tx_active);

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_SidetoneFreq(double freq);

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_SidetoneVolume(double volume);

[DllImport("ChannelMaster.dll", CallingConvention = CallingConvention.Cdecl)]
public static extern void setCMASIO_SidetoneEnable(int enable);
```

**File:** `Project Files/Source/Console/cmaster.cs` (add wrapper class)

```csharp
public static class CMASIOSidetone
{
    public static void SetTXActive(bool tx_active)
    {
        NetworkIO.setCMASIO_TXActive(tx_active ? 1 : 0);
    }

    public static void SetFrequency(int freq_hz)
    {
        NetworkIO.setCMASIO_SidetoneFreq((double)freq_hz);
    }

    public static void SetVolume(int volume_percent)
    {
        double volume = (double)volume_percent / 100.0;
        NetworkIO.setCMASIO_SidetoneVolume(volume);
    }

    public static void SetEnabled(bool enabled)
    {
        NetworkIO.setCMASIO_SidetoneEnable(enabled ? 1 : 0);
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
            if (CurrentAudioCodec == AudioCodec.ASIO)  // Check if using CMASIO
            {
                CMASIOSidetone.SetTXActive(value);
            }

            // ... rest of existing code ...
        }
    }
}
```

### 4.8 GUI Controls

**File:** `Project Files/Source/Console/setup.cs` (add to CW/ASIO section)

```csharp
// Add these controls to the setup form CW/ASIO tab:

private NumericUpDownTS udCMASIOSidetoneFreq;
private NumericUpDownTS udCMASIOSidetoneVolume;
private CheckBoxTS chkCMASIOSidetoneEnable;

private void InitializeCMASIOControls()
{
    // Sidetone Frequency (Hz)
    udCMASIOSidetoneFreq = new NumericUpDownTS();
    udCMASIOSidetoneFreq.Minimum = 200;
    udCMASIOSidetoneFreq.Maximum = 1200;
    udCMASIOSidetoneFreq.Value = 600;  // Default
    udCMASIOSidetoneFreq.Increment = 50;
    udCMASIOSidetoneFreq.ValueChanged += udCMASIOSidetoneFreq_ValueChanged;

    // Sidetone Volume (%)
    udCMASIOSidetoneVolume = new NumericUpDownTS();
    udCMASIOSidetoneVolume.Minimum = 0;
    udCMASIOSidetoneVolume.Maximum = 100;
    udCMASIOSidetoneVolume.Value = 50;  // Default 50%
    udCMASIOSidetoneVolume.Increment = 5;
    udCMASIOSidetoneVolume.ValueChanged += udCMASIOSidetoneVolume_ValueChanged;

    // Sidetone Enable
    chkCMASIOSidetoneEnable = new CheckBoxTS();
    chkCMASIOSidetoneEnable.Text = "CMASIO Local Sidetone";
    chkCMASIOSidetoneEnable.Checked = true;
    chkCMASIOSidetoneEnable.CheckedChanged += chkCMASIOSidetoneEnable_CheckedChanged;
}

private void udCMASIOSidetoneFreq_ValueChanged(object sender, EventArgs e)
{
    if (initializing) return;
    CMASIOSidetone.SetFrequency((int)udCMASIOSidetoneFreq.Value);
}

private void udCMASIOSidetoneVolume_ValueChanged(object sender, EventArgs e)
{
    if (initializing) return;
    CMASIOSidetone.SetVolume((int)udCMASIOSidetoneVolume.Value);
}

private void chkCMASIOSidetoneEnable_CheckedChanged(object sender, EventArgs e)
{
    if (initializing) return;
    CMASIOSidetone.SetEnabled(chkCMASIOSidetoneEnable.Checked);

    // Enable/disable frequency and volume controls
    udCMASIOSidetoneFreq.Enabled = chkCMASIOSidetoneEnable.Checked;
    udCMASIOSidetoneVolume.Enabled = chkCMASIOSidetoneEnable.Checked;
}
```

---

## 5. Benefits and Impact

### 5.1 Benefits

| Benefit | Current (RX Monitor) | Proposed (Synthetic) |
|---------|---------------------|----------------------|
| **AGC Recovery** | 100-500ms delay | Instant (no AGC saturation) |
| **Volume Control** | Coupled with RX | Independent control |
| **Audio Quality** | Demodulated (artifacts) | Pure sine wave |
| **Latency** | Demodulation delay | Generation only (~0.5ms) |
| **CPU Usage** | RX DSP chain active | Minimal (simple sine) |

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

## 7. Configuration & Persistence

### 7.1 Database Schema

Add to `Project Files/Source/Console/Database.cs`:

```csharp
// Table: State
// New columns:
CREATE TABLE IF NOT EXISTS State (
    ...
    CMASIOSidetoneEnable INTEGER DEFAULT 1,
    CMASIOSidetoneFreq INTEGER DEFAULT 600,
    CMASIOSidetoneVolume INTEGER DEFAULT 50,
    ...
);
```

### 7.2 Save/Load

```csharp
// In console.cs or setup.cs
private void SaveCMASIOSettings()
{
    DB.SaveVar("State", "CMASIOSidetoneEnable", chkCMASIOSidetoneEnable.Checked);
    DB.SaveVar("State", "CMASIOSidetoneFreq", (int)udCMASIOSidetoneFreq.Value);
    DB.SaveVar("State", "CMASIOSidetoneVolume", (int)udCMASIOSidetoneVolume.Value);
}

private void LoadCMASIOSettings()
{
    chkCMASIOSidetoneEnable.Checked = DB.GetVarBool("State", "CMASIOSidetoneEnable", true);
    udCMASIOSidetoneFreq.Value = DB.GetVarInt("State", "CMASIOSidetoneFreq", 600);
    udCMASIOSidetoneVolume.Value = DB.GetVarInt("State", "CMASIOSidetoneVolume", 50);

    // Apply to CMASIO
    CMASIOSidetone.SetEnabled(chkCMASIOSidetoneEnable.Checked);
    CMASIOSidetone.SetFrequency((int)udCMASIOSidetoneFreq.Value);
    CMASIOSidetone.SetVolume((int)udCMASIOSidetoneVolume.Value);
}
```

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
| `ChannelMaster/cmasio.h` | Add struct fields, exports | +15 |
| `ChannelMaster/cmasio.c` | Add generator, modify asioOUT(), exports | +150 |
| `Console/HPSDR/NetworkIOImports.cs` | Add DllImports | +12 |
| `Console/cmaster.cs` | Add wrapper class | +30 |
| `Console/console.cs` | Hook MOX changes | +5 |
| `Console/setup.cs` | Add GUI controls, event handlers | +80 |
| `Console/Database.cs` | Add schema columns, save/load | +20 |

**Total Estimated LOC:** ~312 lines (including comments)

### 9.2 Key Functions

| Function | File | Purpose |
|----------|------|---------|
| `generateLocalSidetone()` | cmasio.c | Generate sine wave with fade |
| `asioOUT()` | cmasio.c | Modified to inject sidetone |
| `setCMASIO_TXActive()` | cmasio.c | Notify TX state change |
| `CMASIOSidetone.*` | cmaster.cs | C# wrapper API |
| `MOX` property | console.cs | Hook to notify CMASIO |

### 9.3 Integration Points

1. **MOX State Detection:**
   - `console.cs:MOX` property setter
   - `Audio.MOX` changes
   - CAT command MOX changes

2. **Break-In Mode:**
   - `console.cs:CurrentBreakInMode` (QSK/Semi/Manual)
   - `NetworkIO.SetCWHangTime()` (break-in delay)

3. **Audio Codec Selection:**
   - `cmaster.cs:audioCodecId == ASIO`
   - `CMASIOConfig.cs` (registry settings)

---

## 10. Acceptance Criteria

### 10.1 Functional Requirements

- [ ] Sidetone audible during TX in Semi and QSK modes
- [ ] No sidetone in Manual mode (PTT only)
- [ ] Sidetone frequency adjustable 200-1200 Hz
- [ ] Sidetone volume adjustable 0-100% (independent of RX)
- [ ] Fade in/out < 2ms (no clicks)
- [ ] Settings persist across restarts

### 10.2 Performance Requirements

- [ ] AGC recovery < 10ms (vs. 100-500ms current)
- [ ] Sidetone latency < 5ms (key-down to audio)
- [ ] CPU overhead < 1%
- [ ] No audio dropouts during fast CW (40+ WPM)

### 10.3 User Experience

- [ ] GUI controls intuitive and accessible
- [ ] Settings saved per-band (optional enhancement)
- [ ] Disable option available (fallback to current behavior)
- [ ] No impact on non-CMASIO modes

---

## 11. Rollout Plan

### Phase 1: Core Implementation (Week 1)
- Implement `generateLocalSidetone()` function
- Modify `asioOUT()` for TX detection
- Add C exports and DllImports

### Phase 2: Integration (Week 2)
- Hook MOX changes in console.cs
- Add C# wrapper class
- Basic GUI controls (frequency, volume, enable)

### Phase 3: Testing & Refinement (Week 3)
- Unit tests for generator
- Integration tests with real hardware
- Tune fade timing for click-free operation

### Phase 4: Polish & Documentation (Week 4)
- Database persistence
- User documentation
- Code review and merge

---

## 12. References

### 12.1 Related Code Files

- `Project Files/Source/ChannelMaster/cmasio.c` - CMASIO audio routing
- `Project Files/Source/ChannelMaster/cmasio.h` - CMASIO header
- `Project Files/Source/Console/console.cs` - Main console (MOX, break-in)
- `Project Files/Source/Console/enums.cs` - BreakIn enum definition
- `Project Files/Source/ChannelMaster/netInterface.c` - Network protocol
- `Project Files/Source/Console/setup.cs` - Setup form (CW controls)

### 12.2 Related Functionality

- **CW Pitch:** `console.cs:18103-18119` (CWPitch property)
- **CW Sidetone (Hardware):** `console.cs:12969-12994` (setCWSideToneVolume)
- **Break-In Delay:** `console.cs:18419-18429` (BreakInDelay property)
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

---

**END OF DOCUMENT**
