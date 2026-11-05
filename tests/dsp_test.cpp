// Tests del núcleo DSP (sin JUCE). Devuelve 0 si todo pasa.
#include "../src/FreeFilterDSP.h"

#include <algorithm>
#include <cstdio>
#include <random>

static int failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++failures;                                        \
            std::printf ("FALLO %s:%d  ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                         \
            std::printf ("\n");                                \
        }                                                      \
    } while (0)

static double dbAt (const float* taps, int bin)
{
    return 20.0 * std::log10 (std::max (std::abs (ff::tapsResponse (taps, bin)), 1e-9));
}

int main()
{
    float taps[ff::kTaps];
    float v[ff::kBands];

    // 1) Todo a 0 dB -> delta unitaria
    std::fill (v, v + ff::kBands, 0.0f);
    ff::designTaps (v, taps);
    CHECK (std::abs (taps[ff::kHalf] - 1.0f) < 1e-4f, "centro = %f", taps[ff::kHalf]);
    for (int n = 0; n < ff::kTaps; ++n)
        if (n != ff::kHalf) CHECK (std::abs (taps[n]) < 1e-4f, "tap %d = %g", n, taps[n]);

    // 2) Simetría (fase lineal)
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> ud (-15.0f, 15.0f);
    for (auto& x : v) x = ud (rng);
    ff::designTaps (v, taps);
    for (int n = 0; n < ff::kHalf; ++n)
        CHECK (taps[ff::kHalf - 1 - n] == taps[ff::kHalf + 1 + n], "asimetria en %d", n);

    // 3) Todo a +6 dB -> ganancia plana de +6 dB
    std::fill (v, v + ff::kBands, 6.0f);
    ff::designTaps (v, taps);
    for (int bin = 8; bin < 505; bin += 7)
        CHECK (std::abs (dbAt (taps, bin) - 6.0) < 0.05, "+6 dB plano: bin %d da %.3f", bin, dbAt (taps, bin));

    // 4) Regresión contra el plugin original: slider 17 = +12 dB (medido en la DLL original a 44.1 kHz)
    std::fill (v, v + ff::kBands, 0.0f);
    v[17] = 12.0f;
    ff::designTaps (v, taps);
    struct Ref { int bin; double db; };
    const Ref refs[] = { { 276, -0.0002 }, { 280, 0.4046 }, { 284, 2.0853 }, { 288, 4.7109 }, { 292, 7.5631 },
                         { 296, 9.8768 },  { 300, 10.2742 }, { 304, 8.4642 }, { 308, 5.6553 }, { 312, 2.9052 },
                         { 316, 0.8017 },  { 320, 0.0427 },  { 324, -0.0039 } };
    double worst = 0;
    for (const auto& r : refs)
    {
        const double got = dbAt (taps, r.bin);
        worst = std::max (worst, std::abs (got - r.db));
        CHECK (std::abs (got - r.db) < 0.12, "bin %d: original %.4f dB, mio %.4f dB", r.bin, r.db, got);
    }
    std::printf ("regresion slider17=+12 dB: error maximo vs original = %.4f dB\n", worst);

    // 5) Respuesta de un impulso: el filtrado FIR coincide con la respuesta teórica
    {
        ff::FirChannel fir;
        fir.reset();
        std::fill (v, v + ff::kBands, 0.0f);
        v[5] = -9.0f; v[20] = 7.5f;
        ff::designTaps (v, taps);
        std::vector<float> out (ff::kTaps + 8);
        for (size_t i = 0; i < out.size(); ++i) out[i] = fir.process (i == 0 ? 1.0f : 0.0f, taps);
        for (int n = 0; n < ff::kTaps; ++n)
            CHECK (std::abs (out[(size_t) n] - taps[n]) < 1e-6f, "impulso en %d", n);
    }

    if (failures == 0)
        std::printf ("OK: todos los tests pasan\n");
    return failures == 0 ? 0 : 1;
}
