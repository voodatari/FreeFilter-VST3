// FreeFilter: nucleo DSP (sin dependencias de JUCE).
//
// Modelo deducido del analisis del plugin original y verificado contra el (ver docs/ANALISIS.md):
//
//  Modo "bandas iguales" (Lin)
//   * 30 sliders en dB (-15..+15) en las frecuencias (k + 0.5) * fs / 60.
//   * Se interpolan linealmente (en dB) sobre los 513 bins de una FFT de 1024, con extremos planos.
//   * Match: dB[bin] = morph * 10*log10(Dest[bin] / Source[bin]) + interpolacion(sliders de Match).
//   * G = 10^(dB/20) -> IFFT -> ventana Hann de 257 puntos -> FIR lineal de 257 taps (latencia 128).
//  Aprendizaje (Learn): espectro de potencia medio de bloques rectangulares de 1024 muestras con avance
//   de 768 (el primer bloque empieza 256 muestras antes del primer audio).
//
//  Modo "tercios de octava" (Log): mismos calculos con los 30 puntos de control en las frecuencias
//   de tercio de octava (25 Hz .. 20 kHz, interpolacion lineal en Hz) y resolucion mucho mayor
//   (FIR largo), porque las bandas graves son estrechas.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ff
{
constexpr int kBands = 30;
constexpr double kPi = 3.14159265358979323846;

// Cuadricula de calculo de cada modo
constexpr int kEqFFT = 1024, kEqHalf = 128;                 // 257 taps, latencia 128
constexpr int kThFFT = 16384, kThHalf = 4096;               // 8193 taps, latencia 4096
constexpr int kEqTaps = 2 * kEqHalf + 1, kThTaps = 2 * kThHalf + 1;
constexpr int kEqBins = kEqFFT / 2 + 1, kThBins = kThFFT / 2 + 1;

constexpr double kThirdOctave[kBands] = { 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                          800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                                          10000, 12500, 16000, 20000 };

enum class Mode { Equal, ThirdOctave };

//==============================================================================
// FFT radix-2 compleja (iterativa). inverse = true -> normalizada por 1/N.
inline void fft (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= (double) n;
}

//==============================================================================
struct Grid
{
    int M, half, taps, bins;
    static Grid of (Mode m)
    {
        return m == Mode::Equal ? Grid { kEqFFT, kEqHalf, kEqTaps, kEqBins }
                                : Grid { kThFFT, kThHalf, kThTaps, kThBins };
    }
};

// Posicion (en bins) del punto de control de cada banda.
inline double bandPosition (Mode mode, int band, int M, double sampleRate)
{
    if (mode == Mode::Equal)
        return (band + 0.5) * (double) (M / 2) / kBands;
    return kThirdOctave[band] / (sampleRate / (double) M);
}

// Respuesta deseada por bin (dB): interpolacion lineal entre los puntos de control, extremos planos.
inline void slidersToBinDb (Mode mode, const float* sliderDb, double sampleRate, double* binDb)
{
    const Grid g = Grid::of (mode);
    double pos[kBands];
    for (int j = 0; j < kBands; ++j) pos[j] = bandPosition (mode, j, g.M, sampleRate);

    int j = 0;
    for (int k = 0; k < g.bins; ++k)
    {
        const double p = (double) k;
        if (p <= pos[0])             { binDb[k] = sliderDb[0]; continue; }
        if (p >= pos[kBands - 1])    { binDb[k] = sliderDb[kBands - 1]; continue; }
        while (j < kBands - 2 && p >= pos[j + 1]) ++j;
        const double t = (p - pos[j]) / (pos[j + 1] - pos[j]);
        binDb[k] = sliderDb[j] + t * (sliderDb[j + 1] - sliderDb[j]);
    }
}

// Ventana Hann de (2*half+1) puntos, semiancho half + 0.5.
inline double firWindow (int n, int half) { return 0.5 * (1.0 + std::cos (kPi * n / (half + 0.5))); }

// De la curva en dB por bin al FIR lineal simetrico. 'response' (opcional) recibe la magnitud real
// (zero-phase) del FIR resultante en los 'bins' bins (para dibujar).
inline void designTapsFromDb (Mode mode, const double* binDb, float* taps, double* response = nullptr)
{
    const Grid g = Grid::of (mode);
    std::vector<std::complex<double>> a ((size_t) g.M);
    for (int k = 0; k < g.bins; ++k)
    {
        const double G = std::pow (10.0, binDb[k] / 20.0);
        a[(size_t) k] = G;
        if (k > 0 && k < g.M / 2) a[(size_t) (g.M - k)] = G;
    }
    fft (a, true);                                   // h[n] = IFFT(G), real y simetrica

    std::vector<std::complex<double>> w ((size_t) g.M, 0.0);
    for (int n = 0; n <= g.half; ++n)
    {
        const double h = a[(size_t) n].real() * firWindow (n, g.half);
        taps[g.half + n] = (float) h;
        taps[g.half - n] = (float) h;
        w[(size_t) n] = h;
        if (n > 0) w[(size_t) (g.M - n)] = h;
    }
    if (response)
    {
        fft (w, false);
        for (int k = 0; k < g.bins; ++k) response[k] = w[(size_t) k].real();
    }
}

// Modo tercios de octava. El original resuelve las bandas graves con un procesado de menor tasa
// (ventana de 257 puntos a tasa reducida => resolucion fina) y las agudas con la ventana de 257 puntos a
// tasa completa (la misma que en bandas iguales; en las medidas, un slider a +12 dB da +11.36 dB de pico).
// Aqui se disenan las dos respuestas sobre una FFT larga y se mezclan con una transicion suave
// (smoothstep) entre 1 y 1.6 kHz. El FIR resultante tiene 8193 taps (latencia 4096).
inline void designThirdFromDb (const double* binDb, double sampleRate, float* taps, double* response = nullptr)
{
    const Grid g = Grid::of (Mode::ThirdOctave);
    std::vector<std::complex<double>> a ((size_t) g.M);
    for (int k = 0; k < g.bins; ++k)
    {
        const double G = std::pow (10.0, binDb[k] / 20.0);
        a[(size_t) k] = G;
        if (k > 0 && k < g.M / 2) a[(size_t) (g.M - k)] = G;
    }
    fft (a, true);                                     // h[n] = IFFT(G)

    constexpr int shortHalf = kEqHalf;                 // 257 taps a tasa completa
    std::vector<std::complex<double>> lo ((size_t) g.M, 0.0), hi ((size_t) g.M, 0.0);
    for (int n = 0; n <= g.half; ++n)
    {
        const double h = a[(size_t) n].real();
        const double hl = h * firWindow (n, g.half);
        const double hs = n <= shortHalf ? h * firWindow (n, shortHalf) : 0.0;
        lo[(size_t) n] = hl; hi[(size_t) n] = hs;
        if (n > 0) { lo[(size_t) (g.M - n)] = hl; hi[(size_t) (g.M - n)] = hs; }
    }
    fft (lo, false);
    fft (hi, false);

    std::vector<std::complex<double>> mix ((size_t) g.M, 0.0);
    for (int k = 0; k < g.bins; ++k)
    {
        const double f = k * sampleRate / g.M;
        double t = (f - 1000.0) / 600.0;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const double c = t * t * (3.0 - 2.0 * t);
        const double H = (1.0 - c) * lo[(size_t) k].real() + c * hi[(size_t) k].real();
        mix[(size_t) k] = H;
        if (k > 0 && k < g.M / 2) mix[(size_t) (g.M - k)] = H;
        if (response) response[k] = H;
    }
    fft (mix, true);                                   // de vuelta al tiempo (zero-phase)
    for (int n = 0; n <= g.half; ++n)
    {
        const float h = (float) mix[(size_t) n].real();
        taps[g.half + n] = h;
        taps[g.half - n] = h;
    }
}

// Conveniencia: sliders -> taps (modo bandas iguales, sin Match).
inline void designTaps (const float* sliderDb, float* taps)
{
    double binDb[kEqBins];
    slidersToBinDb (Mode::Equal, sliderDb, 44100.0, binDb);
    designTapsFromDb (Mode::Equal, binDb, taps);
}

// Respuesta de magnitud (zero-phase) del FIR de bandas iguales en un bin de la FFT de 1024.
inline double tapsResponse (const float* taps, int bin)
{
    const double w = 2.0 * kPi * bin / kEqFFT;
    double acc = taps[kEqHalf];
    for (int n = 1; n <= kEqHalf; ++n) acc += 2.0 * taps[kEqHalf + n] * std::cos (w * n);
    return acc;
}

// dB[bin] = morph * 10*log10(A/B) + sliders interpolados.  A = destino (numerador), B = origen.
// Un bin a cero se toma como 1, igual que el original.
inline void applyMatch (const std::vector<double>& A, const std::vector<double>& B, double morph, double* binDb, int bins)
{
    for (int k = 0; k < bins; ++k)
    {
        const double a = A[(size_t) k] == 0.0 ? 1.0 : A[(size_t) k];
        const double b = B[(size_t) k] == 0.0 ? 1.0 : B[(size_t) k];
        binDb[k] += morph * 10.0 * std::log10 (a / b);
    }
}

//==============================================================================
// Aprendiz de espectros: bloques rectangulares de N muestras con avance 'hop'. El primer bloque
// empieza (N - hop) muestras antes del primer audio. Acumula la potencia |X|^2 de cada bin.
class SpectrumLearner
{
public:
    // El analizador es continuo (como en el original: la fase de los bloques es global); lo que se acumula
    // depende del destino activo: 0 = nada, 1 = Dest (array A), 2 = Source (array B).
    enum Target { None = 0, Dest = 1, Source = 2 };

    void configure (int fftSize, int hop)
    {
        N = fftSize; H = hop;
        ring.assign ((size_t) N, 0.0f);
        sumA.assign ((size_t) N / 2 + 1, 0.0);
        sumB.assign ((size_t) N / 2 + 1, 0.0);
        scratch.assign ((size_t) N, std::complex<double> (0.0, 0.0));
        reset();
    }
    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        clearAll();
        sinceFrame = 0; pos = 0;
    }
    void clearAll() { clear (Dest); clear (Source); }
    void clear (Target t)
    {
        auto& s = t == Dest ? sumA : sumB;
        std::fill (s.begin(), s.end(), 0.0);
        (t == Dest ? countA : countB) = 0;
    }
    void setTarget (Target t) { target = t; }

    // Entra una muestra (mono: media de canales). Devuelve true si se completo un bloque.
    bool push (float x)
    {
        ring[(size_t) pos] = x;
        pos = (pos + 1) % N;
        if (++sinceFrame < H) return false;
        sinceFrame = 0;
        if (target != None) analyse();
        return true;
    }
    int frames (Target t) const { return t == Dest ? countA : countB; }
    // Espectro de potencia medio por bloque
    std::vector<double> average (Target t) const
    {
        std::vector<double> r (sumA.size(), 0.0);
        averageInto (t, r.data());
        return r;
    }
    // Igual, sin reservar memoria (dst debe tener N/2+1 elementos)
    void averageInto (Target t, double* dst) const
    {
        const auto& s = t == Dest ? sumA : sumB;
        const int c = frames (t);
        for (size_t k = 0; k < s.size(); ++k) dst[k] = c > 0 ? s[k] / c : 0.0;
    }

private:
    void analyse()
    {
        for (int i = 0; i < N; ++i) scratch[(size_t) i] = ring[(size_t) ((pos + i) % N)];
        fft (scratch, false);
        auto& s = target == Dest ? sumA : sumB;
        for (int k = 0; k <= N / 2; ++k) s[(size_t) k] += std::norm (scratch[(size_t) k]);
        ++(target == Dest ? countA : countB);
    }

    int N = 1024, H = 768, pos = 0, sinceFrame = 0, countA = 0, countB = 0;
    Target target = None;
    std::vector<float> ring;
    std::vector<double> sumA, sumB;
    std::vector<std::complex<double>> scratch;
};

//==============================================================================
// FIR por canal (convolucion directa), para el modo de bandas iguales (257 taps).
class FirChannel
{
public:
    void reset()
    {
        hist.assign ((size_t) kEqTaps * 2, 0.0f);
        pos = 0;
    }

    void push (float x) noexcept
    {
        pos = (pos == 0) ? kEqTaps - 1 : pos - 1;
        hist[(size_t) pos] = hist[(size_t) pos + kEqTaps] = x;
    }

    float dot (const float* taps) const noexcept
    {
        const float* h = &hist[(size_t) pos];
        float acc = 0.0f;
        for (int j = 0; j < kEqTaps; ++j) acc += taps[j] * h[j];
        return acc;
    }

    float process (float x, const float* taps) noexcept
    {
        push (x);
        return dot (taps);
    }

private:
    std::vector<float> hist = std::vector<float> ((size_t) kEqTaps * 2, 0.0f);
    int pos = 0;
};

} // namespace ff
