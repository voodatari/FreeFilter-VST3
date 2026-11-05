#include "PluginProcessor.h"
#include "PluginEditor.h"

FreeFilterProcessor::FreeFilterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int b = 0; b < ff::kBands; ++b)
        bandParams[(size_t) b] = apvts.getRawParameterValue (bandId (b));

    for (auto& v : lastValues) v = 1.0e9f; // fuerza el primer diseño
    taps.fill (0.0f);
    taps[(size_t) ff::kHalf] = 1.0f;
    prevTaps = taps;
}

juce::AudioProcessorValueTreeState::ParameterLayout FreeFilterProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int b = 0; b < ff::kBands; ++b)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandId (b), 1 }, "Band " + juce::String (b + 1),
            juce::NormalisableRange<float> (-15.0f, 15.0f, 0.01f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
    }
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "freeze", 1 }, "Freeze", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "logaxis", 1 }, "Log axis", true));
    return layout;
}

bool FreeFilterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void FreeFilterProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    for (auto& f : fir) f.reset();
    xfadeLeft = 0;
    for (auto& v : lastValues) v = 1.0e9f;
    setLatencySamples (ff::kLatency);
    ringWrite = 0; ringReadIn = ringReadOut = 0;
}

void FreeFilterProcessor::pushAnalyzer (const float* in, const float* out, int n)
{
    int w = ringWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        ringIn[(size_t) w]  = in[i];
        ringOut[(size_t) w] = out[i];
        w = (w + 1) % kRing;
    }
    ringWrite.store (w, std::memory_order_release);
}

int FreeFilterProcessor::readAnalyzer (float* dest, int numSamples, bool output)
{
    const int w = ringWrite.load (std::memory_order_acquire);
    const auto& ring = output ? ringOut : ringIn;
    int start = (w - numSamples + kRing) % kRing;
    for (int i = 0; i < numSamples; ++i) dest[i] = ring[(size_t) ((start + i) % kRing)];
    return numSamples;
}

void FreeFilterProcessor::getCurrentTaps (float* dest) const
{
    const juce::SpinLock::ScopedLockType sl (tapsLock);
    for (int i = 0; i < ff::kTaps; ++i) dest[i] = taps[(size_t) i];
}

void FreeFilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // ¿Cambió algún slider? -> nuevo diseño con crossfade desde los coeficientes anteriores
    std::array<float, ff::kBands> cur {};
    bool changed = false;
    for (int b = 0; b < ff::kBands; ++b)
    {
        cur[(size_t) b] = bandParams[(size_t) b]->load();
        if (cur[(size_t) b] != lastValues[(size_t) b]) changed = true;
    }
    if (changed)
    {
        prevTaps = taps;
        std::array<float, ff::kTaps> fresh {};
        ff::designTaps (cur.data(), fresh.data());
        {
            const juce::SpinLock::ScopedLockType sl (tapsLock);
            taps = fresh;
        }
        lastValues = cur;
        xfadeLeft = kXfade;
    }

    const int total = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);

    for (int offset = 0; offset < total; offset += kChunk)
    {
        const int n = juce::jmin (kChunk, total - offset);

        // Entrada mono para el analizador (antes de filtrar); sin reservas de memoria
        for (int i = 0; i < n; ++i) { monoIn[(size_t) i] = 0.0f; monoOut[(size_t) i] = 0.0f; }
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = buffer.getReadPointer (ch) + offset;
            for (int i = 0; i < n; ++i) monoIn[(size_t) i] += src[i] / (float) numCh;
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data = buffer.getWritePointer (ch) + offset;
            int xf = xfadeLeft;
            for (int i = 0; i < n; ++i)
            {
                fir[(size_t) ch].push (data[i]);
                float y = fir[(size_t) ch].dot (taps.data());
                if (xf > 0)
                {
                    const float t = (float) xf / (float) kXfade;               // 1 -> 0
                    y = y + t * (fir[(size_t) ch].dot (prevTaps.data()) - y);  // mezcla vieja -> nueva
                    --xf;
                }
                data[i] = y;
                monoOut[(size_t) i] += y / (float) numCh;
            }
        }
        xfadeLeft = juce::jmax (0, xfadeLeft - n);

        pushAnalyzer (monoIn.data(), monoOut.data(), n);
    }
}

juce::AudioProcessorEditor* FreeFilterProcessor::createEditor()
{
    return new FreeFilterEditor (*this);
}

void FreeFilterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FreeFilterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FreeFilterProcessor();
}
