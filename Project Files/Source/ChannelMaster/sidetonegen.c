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
