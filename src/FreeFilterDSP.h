// FreeFilter: núcleo DSP (sin dependencias de JUCE).
//
// Modelo deducido del análisis del plugin original y verificado contra él (ver docs/ANALISIS.md):
//
//  * 30 sliders, valores en dB (-15..+15), situados en las frecuencias (k + 0.5) * fs / 60.
//  * Los valores se interpolan linealmente (en dB) entre esos puntos, con extremos planos, sobre 513 bins
//    de una FFT de 1024 puntos: G[bin] = 10^(x[bin] / 20).
//  * La respuesta se pasa al tiempo (IFFT de 1024, simétrica), se multiplica por una ventana Hann de
//    257 puntos y queda un FIR lineal de 257 taps (latencia 128 muestras).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ff
{
constexpr int kBands = 30;
constexpr int kFFT = 1024;
constexpr int kBins = kFFT / 2 + 1;   // 513
constexpr int kHalf = 128;
constexpr int kTaps = 2 * kHalf + 1;  // 257
constexpr int kLatency = kHalf;       // muestras
constexpr double kPi = 3.14159265358979323846;

// Posición (en bins de 1024-FFT) del punto de control de cada banda.
inline double bandPosition (int band) { return (band + 0.5) * (double) (kFFT / 2) / kBands; }

// Respuesta deseada por bin (en dB), interpolación lineal con extremos planos.
inline void bandsToBinDb (const float* sliderDb, double* binDb)
{
    for (int k = 0; k < kBins; ++k)
    {
        const double p = (double) k;
        if (p <= bandPosition (0))              { binDb[k] = sliderDb[0]; continue; }
        if (p >= bandPosition (kBands - 1))     { binDb[k] = sliderDb[kBands - 1]; continue; }
        int j = (int) std::floor (p / ((double) (kFFT / 2) / kBands) - 0.5);
        const double p0 = bandPosition (j), p1 = bandPosition (j + 1);
        const double t = (p - p0) / (p1 - p0);
        binDb[k] = sliderDb[j] + t * (sliderDb[j + 1] - sliderDb[j]);
    }
}

// Ventana de 257 puntos (Hann, semiancho 128.5).
inline double window (int n) { return 0.5 * (1.0 + std::cos (kPi * n / (kHalf + 0.5))); }

// Diseña los 257 coeficientes (simétricos) a partir de los 30 valores de slider en dB.
inline void designTaps (const float* sliderDb, float* taps /* kTaps */)
{
    double binDb[kBins], G[kBins];
    bandsToBinDb (sliderDb, binDb);
    for (int k = 0; k < kBins; ++k)
        G[k] = std::pow (10.0, binDb[k] / 20.0);

    // Tabla de cosenos: cos(2 pi k n / 1024) = cosT[(k * n) & 1023]
    static const std::vector<double> cosT = []
    {
        std::vector<double> t (kFFT);
        for (int i = 0; i < kFFT; ++i) t[(size_t) i] = std::cos (2.0 * kPi * i / kFFT);
        return t;
    }();

    for (int n = 0; n <= kHalf; ++n)
    {
        double acc = G[0] + G[kBins - 1] * ((n & 1) ? -1.0 : 1.0);
        for (int k = 1; k < kBins - 1; ++k)
            acc += 2.0 * G[k] * cosT[(size_t) ((k * n) & (kFFT - 1))];
        const double h = acc / kFFT * window (n);
        taps[kHalf + n] = (float) h;
        taps[kHalf - n] = (float) h;
    }
}

// Respuesta de magnitud (zero-phase) del FIR en un bin (para dibujar y para tests).
inline double tapsResponse (const float* taps, int bin)
{
    const double w = 2.0 * kPi * bin / kFFT;
    double acc = taps[kHalf];
    for (int n = 1; n <= kHalf; ++n) acc += 2.0 * taps[kHalf + n] * std::cos (w * n);
    return acc;
}

//==============================================================================
// FIR de 257 taps por canal, con cambio de coeficientes suave (crossfade).
class FirChannel
{
public:
    void reset()
    {
        hist.assign ((size_t) kTaps * 2, 0.0f);
        pos = 0;
    }

    // Introduce una muestra de entrada en el historial.
    void push (float x) noexcept
    {
        pos = (pos == 0) ? kTaps - 1 : pos - 1;
        hist[(size_t) pos] = hist[(size_t) pos + kTaps] = x;
    }

    // y[n] = sum_j taps[j] * x[n-j]  (tras push). Se puede llamar con varios juegos de taps.
    float dot (const float* taps) const noexcept
    {
        const float* h = &hist[(size_t) pos];
        float acc = 0.0f;
        for (int j = 0; j < kTaps; ++j) acc += taps[j] * h[j];
        return acc;
    }

    float process (float x, const float* taps) noexcept
    {
        push (x);
        return dot (taps);
    }

private:
    std::vector<float> hist = std::vector<float> ((size_t) kTaps * 2, 0.0f);
    int pos = 0;
};

} // namespace ff
