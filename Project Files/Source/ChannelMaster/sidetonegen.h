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
