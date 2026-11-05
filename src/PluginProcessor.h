#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "FreeFilterDSP.h"

class FreeFilterProcessor : public juce::AudioProcessor
{
public:
    FreeFilterProcessor();
    ~FreeFilterProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FreeFilter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return ff::kLatency / 44100.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Init"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::String bandId (int band) { return "band" + juce::String (band).paddedLeft ('0', 2); }

    //==========================================================================
    // Acceso para el editor (analizador y curva del filtro)
    double getSampleRate_() const { return currentSampleRate.load(); }
    int readAnalyzer (float* dest, int numSamples, bool output);   // devuelve muestras leídas
    void getCurrentTaps (float* dest) const;                        // copia de los 257 taps actuales
    float getBandValue (int band) const { return bandParams[(size_t) band]->load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pushAnalyzer (const float* in, const float* out, int n);

    std::array<std::atomic<float>*, ff::kBands> bandParams {};
    std::array<float, ff::kBands> lastValues {};

    std::array<float, ff::kTaps> taps {}, prevTaps {};
    std::array<ff::FirChannel, 2> fir;
    int xfadeLeft = 0;
    static constexpr int kXfade = 256;
    static constexpr int kChunk = 4096;
    std::array<float, kChunk> monoIn {}, monoOut {};

    std::atomic<double> currentSampleRate { 44100.0 };

    // Anillos de captura para el analizador (mono)
    static constexpr int kRing = 16384;
    std::array<float, kRing> ringIn {}, ringOut {};
    std::atomic<int> ringWrite { 0 };
    int ringReadIn = 0, ringReadOut = 0;

    mutable juce::SpinLock tapsLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreeFilterProcessor)
};
