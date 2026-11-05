#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include "FreeFilterDSP.h"

class FreeFilterProcessor : public juce::AudioProcessor,
                            private juce::AudioProcessorValueTreeState::Listener
{
public:
    FreeFilterProcessor();
    ~FreeFilterProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FreeFilter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Init"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::String bandId (int band)  { return "band"  + juce::String (band).paddedLeft ('0', 2); }
    static juce::String matchId (int band) { return "match" + juce::String (band).paddedLeft ('0', 2); }

    //==========================================================================
    // Acceso para el editor
    double getSampleRate_() const { return currentSampleRate.load(); }
    int readAnalyzer (float* dest, int numSamples, bool output);

    bool isThirdOctave() const { return apvts.getRawParameterValue ("thirds")->load() > 0.5f; }
    bool isMatchOn() const { return apvts.getRawParameterValue ("matchOn")->load() > 0.5f; }
    bool isLearnOn() const { return apvts.getRawParameterValue ("learn")->load() > 0.5f; }
    bool isLearnDest() const { return apvts.getRawParameterValue ("learnDest")->load() > 0.5f; }
    juce::String activeBandId (int b) const { return isMatchOn() ? matchId (b) : bandId (b); }
    float getBandValue (int band) const { return apvts.getRawParameterValue (activeBandId (band))->load(); }

    // Fotogramas aprendidos en el modo actual (0 = nada aprendido)
    int learnedFrames (bool dest) const { return (dest ? framesDest : framesSource).load(); }
    bool matchAvailable() const { return learnedFrames (true) > 0 && learnedFrames (false) > 0; }

    // Respuesta del filtro en dB a una frecuencia (para dibujar)
    float responseDbAt (float hz) const;

    void requestReset();     // sliders a 0, aprendizaje borrado, Match y Learn desactivados

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& id, float newValue) override;

    // ---- diseno del filtro (hilo secundario) ----
    struct DesignJob
    {
        ff::Mode mode = ff::Mode::Equal;
        double sampleRate = 44100.0;
        std::array<float, ff::kBands> sliders {};
        float morph = 1.0f;
        bool match = false;
        uint32_t version = 0;
    };
    class Designer;
    void postDesign (const DesignJob& job);
    void runDesign (const DesignJob& job);   // en el hilo de diseno
    void publishResponse (const std::vector<double>& response, ff::Mode mode, double sr);

    void takeMatchSnapshot();

    std::unique_ptr<Designer> designer;

    // parametros
    std::array<std::atomic<float>*, ff::kBands> bandParams {}, matchParams {};

    // ---- audio ----
    ff::Mode activeMode = ff::Mode::Equal;
    std::array<float, ff::kEqTaps> taps {}, prevTaps {};
    std::array<ff::FirChannel, 2> fir;
    int xfadeLeft = 0;
    static constexpr int kXfade = 256;
    static constexpr int kChunk = 4096;
    std::array<float, kChunk> monoIn {}, monoOut {};
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 256 } };

    std::atomic<double> currentSampleRate { 44100.0 };

    // publicacion de coeficientes (modo bandas iguales)
    juce::SpinLock pubLock;
    std::array<float, ff::kEqTaps> pubTaps {};
    std::atomic<bool> pubReady { false };

    // respuesta para dibujar (1024 puntos log entre 10 Hz y 0.995 Nyquist)
    static constexpr int kRespPoints = 1024;
    mutable juce::SpinLock respLock;
    std::array<float, kRespPoints> respDb {};
    float respFHi = 20000.0f;

    // aprendizaje
    ff::SpectrumLearner learnerEq, learnerTh;
    std::atomic<int> framesDest { 0 }, framesSource { 0 };
    std::vector<double> snapDest, snapSource;     // medias fijadas al activar Match
    juce::SpinLock snapLock;
    uint32_t snapVersion = 0;
    bool lastMatchOn = false, matchEffective = false;
    std::atomic<bool> resetRequested { false };

    // firma del ultimo diseno enviado
    ff::Mode lastSigMode = ff::Mode::Equal;
    bool lastSigMatch = false;
    float lastSigMorph = -1.0f;
    uint32_t lastSigSnap = 0;
    std::array<float, ff::kBands> lastSigSliders {};
    bool haveSig = false;

    // anillos de captura para el analizador
    static constexpr int kRing = 16384;
    std::array<float, kRing> ringIn {}, ringOut {};
    std::atomic<int> ringWrite { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreeFilterProcessor)
};
