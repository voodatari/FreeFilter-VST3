#pragma once

#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <set>

// Interfaz inspirada en la del original (panel rojo 454x417, analizador, 30 sliders, botones).
// Todo se dibuja con vectores: no se usan los mapas de bits originales.
class FreeFilterEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit FreeFilterEditor (FreeFilterProcessor&);
    ~FreeFilterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int designW = 454, designH = 417;

private:
    enum class DrawMode { freehand, level, line };

    void timerCallback() override;
    juce::AffineTransform designToScreen() const;
    juce::Point<float> toDesign (juce::Point<float> p) const;
    void rebuildBackground();

    void paintAnalyzer (juce::Graphics&);
    void paintBands (juce::Graphics&);
    void paintButtons (juce::Graphics&);
    void paintAbout (juce::Graphics&);
    void drawButton (juce::Graphics&, juce::Rectangle<float>, const juce::String&, bool on, bool enabled = true);

    // geometría (espacio de diseño)
    int bandAt (float x) const;
    float valueAt (float y) const;
    float freqToX (float hz) const;
    float xToFreq (float x) const;
    float dbToY (float db) const;

    void setBand (int b, float dB);
    void resetAll();
    bool isLogAxis() const;

    FreeFilterProcessor& processor;

    // analizador
    static constexpr int kFftOrder = 11, kFftSize = 1 << kFftOrder;
    juce::dsp::FFT fft { kFftOrder };
    std::array<float, kFftSize> window {};
    std::array<float, kFftSize * 2> fftBuf {};
    std::array<float, kFftSize> sampBuf {};
    std::array<float, kFftSize / 2> specIn {}, specOut {};
    std::array<float, ff::kTaps> tapsCopy {};

    juce::Image background;
    bool showAbout = false;

    DrawMode drawMode = DrawMode::freehand;
    std::set<int> touched;
    bool draggingBoard = false;
    juce::Point<float> dragStart;
    int lastBand = -1;
    std::array<float, ff::kBands> startValues {};

    juce::Point<float> hover { -1.0f, -1.0f };
    juce::String readHz, readDb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreeFilterEditor)
};
