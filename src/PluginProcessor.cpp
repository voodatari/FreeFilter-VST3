#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Hilo de diseno: calcula taps y respuesta fuera del hilo de audio.
class FreeFilterProcessor::Designer : public juce::Thread
{
public:
    explicit Designer (FreeFilterProcessor& o) : juce::Thread ("FreeFilter designer"), owner (o) {}

    void post (const DesignJob& j)
    {
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            pending = j;
            havePending = true;
        }
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            for (;;)
            {
                DesignJob j;
                {
                    const juce::SpinLock::ScopedLockType sl (lock);
                    if (! havePending) break;
                    j = pending;
                    havePending = false;
                }
                if (threadShouldExit()) return;
                owner.runDesign (j);
            }
        }
    }

private:
    FreeFilterProcessor& owner;
    juce::SpinLock lock;
    DesignJob pending;
    bool havePending = false;
};

//==============================================================================
FreeFilterProcessor::FreeFilterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int b = 0; b < ff::kBands; ++b)
    {
        bandParams[(size_t) b]  = apvts.getRawParameterValue (bandId (b));
        matchParams[(size_t) b] = apvts.getRawParameterValue (matchId (b));
    }

    taps.fill (0.0f);
    taps[(size_t) ff::kEqHalf] = 1.0f;
    prevTaps = taps;
    respDb.fill (0.0f);

    snapDest.assign ((size_t) ff::kThBins, 0.0);
    snapSource.assign ((size_t) ff::kThBins, 0.0);
    learnerEq.configure (ff::kEqFFT, 768);
    learnerTh.configure (ff::kThFFT, 12288);

    apvts.addParameterListener ("thirds", this);
    setLatencySamples (ff::kEqHalf);

    designer = std::make_unique<Designer> (*this);
    designer->startThread();
}

FreeFilterProcessor::~FreeFilterProcessor()
{
    apvts.removeParameterListener ("thirds", this);
    designer->signalThreadShouldExit();
    designer->notify();
    designer->stopThread (3000);
}

juce::AudioProcessorValueTreeState::ParameterLayout FreeFilterProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto dbRange = juce::NormalisableRange<float> (-15.0f, 15.0f, 0.01f);
    for (int b = 0; b < ff::kBands; ++b)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandId (b), 1 }, "Band " + juce::String (b + 1), dbRange, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { matchId (b), 1 }, "Match band " + juce::String (b + 1), dbRange, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
    }
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "morph", 1 }, "Morph", juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "freeze", 1 }, "Freeze", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "thirds", 1 }, "Third octave", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "learn", 1 }, "Learn", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "matchOn", 1 }, "Match", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "learnDest", 1 }, "Learn destination", false));
    return layout;
}

bool FreeFilterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void FreeFilterProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == "thirds")
        setLatencySamples (isThirdOctave() ? ff::kThHalf : ff::kEqHalf);
}

void FreeFilterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (samplesPerBlock, 512), 2 };
    convolution.prepare (spec);
    convolution.reset();

    for (auto& f : fir) f.reset();
    xfadeLeft = 0;
    learnerEq.resetRing();          // el anillo se reinicia, lo aprendido NO (el host llama a esto al parar/arrancar)
    learnerTh.resetRing();
    if (lastLearnSr > 0.0 && std::abs (lastLearnSr - sampleRate) > 1.0)
    {
        const juce::SpinLock::ScopedLockType sl (learnLock);   // otra frecuencia de muestreo: lo aprendido ya no vale
        learnerEq.clearAll();
        learnerTh.clearAll();
        ++dataVersion;
    }
    lastLearnSr = sampleRate;
    lastMatchOn = matchEffective = false;
    haveSig = false;
    activeMode = isThirdOctave() ? ff::Mode::ThirdOctave : ff::Mode::Equal;
    setLatencySamples (isThirdOctave() ? ff::kThHalf : ff::kEqHalf);
    ringWrite = 0;
}

void FreeFilterProcessor::postDesign (const DesignJob& job) { designer->post (job); }

//==============================================================================
void FreeFilterProcessor::runDesign (const DesignJob& job)
{
    const ff::Grid g = ff::Grid::of (job.mode);
    std::vector<double> binDb ((size_t) g.bins);
    ff::slidersToBinDb (job.mode, job.sliders.data(), job.sampleRate, binDb.data());

    if (job.match)
    {
        std::vector<double> A ((size_t) g.bins), B ((size_t) g.bins);
        {
            const juce::SpinLock::ScopedLockType sl (snapLock);
            std::copy (snapDest.begin(),   snapDest.begin()   + g.bins, A.begin());
            std::copy (snapSource.begin(), snapSource.begin() + g.bins, B.begin());
        }
        ff::applyMatch (A, B, (double) job.morph, binDb.data(), g.bins);
    }

    std::vector<float> t ((size_t) g.taps);
    std::vector<double> resp ((size_t) g.bins);
    if (job.mode == ff::Mode::Equal)
        ff::designTapsFromDb (job.mode, binDb.data(), t.data(), resp.data());
    else
        ff::designThirdFromDb (binDb.data(), job.sampleRate, t.data(), resp.data());
    publishResponse (resp, job.mode, job.sampleRate);

    if (job.mode == ff::Mode::Equal)
    {
        const juce::SpinLock::ScopedLockType sl (pubLock);
        std::copy (t.begin(), t.end(), pubTaps.begin());
        pubReady.store (true, std::memory_order_release);
    }
    else
    {
        juce::AudioBuffer<float> ir (1, g.taps);
        std::copy (t.begin(), t.end(), ir.getWritePointer (0));
        convolution.loadImpulseResponse (std::move (ir), job.sampleRate,
                                         juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);
    }
}

void FreeFilterProcessor::publishResponse (const std::vector<double>& response, ff::Mode mode, double sr)
{
    const ff::Grid g = ff::Grid::of (mode);
    const float nyq = (float) (sr * 0.5);
    const float fHi = nyq * 0.995f;
    std::array<float, kRespPoints> out {};
    for (int i = 0; i < kRespPoints; ++i)
    {
        const float f = 10.0f * std::pow (fHi / 10.0f, (float) i / (float) (kRespPoints - 1));
        const double bin = (double) f / (sr / (double) g.M);
        const int i0 = juce::jlimit (0, g.bins - 2, (int) bin);
        const double t = juce::jlimit (0.0, 1.0, bin - i0);
        auto toDb = [&] (double v) { return 20.0 * std::log10 (std::max (std::abs (v), 1.0e-6)); };
        out[(size_t) i] = (float) (toDb (response[(size_t) i0]) * (1.0 - t) + toDb (response[(size_t) i0 + 1]) * t);
    }
    const juce::SpinLock::ScopedLockType sl (respLock);
    respDb = out;
    respFHi = fHi;
}

float FreeFilterProcessor::responseDbAt (float hz) const
{
    const juce::SpinLock::ScopedLockType sl (respLock);
    const float fHi = respFHi;
    if (hz <= 10.0f) return respDb[0];
    if (hz >= fHi) return respDb[kRespPoints - 1];
    const float pos = std::log (hz / 10.0f) / std::log (fHi / 10.0f) * (float) (kRespPoints - 1);
    const int i0 = juce::jlimit (0, kRespPoints - 2, (int) pos);
    const float t = pos - (float) i0;
    return respDb[(size_t) i0] * (1.0f - t) + respDb[(size_t) i0 + 1] * t;
}

//==============================================================================
void FreeFilterProcessor::takeMatchSnapshot()
{
    auto& learner = activeMode == ff::Mode::Equal ? learnerEq : learnerTh;
    const juce::SpinLock::ScopedLockType sl0 (learnLock);
    const juce::SpinLock::ScopedLockType sl (snapLock);
    learner.averageInto (ff::SpectrumLearner::Dest,   snapDest.data());
    learner.averageInto (ff::SpectrumLearner::Source, snapSource.data());
    ++snapVersion;
}

void FreeFilterProcessor::requestReset()
{
    for (int b = 0; b < ff::kBands; ++b)
        for (auto id : { bandId (b), matchId (b) })
            if (auto* p = apvts.getParameter (id))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
                p->endChangeGesture();
            }
    for (auto id : { "learn", "matchOn" })
        if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (0.0f);
    if (auto* p = apvts.getParameter ("morph")) p->setValueNotifyingHost (p->convertTo0to1 (100.0f));
    resetRequested = true;
}

int FreeFilterProcessor::readAnalyzer (float* dest, int numSamples, bool output)
{
    const int w = ringWrite.load (std::memory_order_acquire);
    const auto& ring = output ? ringOut : ringIn;
    const int start = (w - numSamples + kRing) % kRing;
    for (int i = 0; i < numSamples; ++i) dest[i] = ring[(size_t) ((start + i) % kRing)];
    return numSamples;
}

//==============================================================================
void FreeFilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (resetRequested.exchange (false))
    {
        {
            const juce::SpinLock::ScopedLockType sl (learnLock);
            learnerEq.clearAll();
            learnerTh.clearAll();
        }
        framesDest = framesSource = 0;
        lastMatchOn = matchEffective = false;
        haveSig = false;
    }

    // ---- modo ----
    const ff::Mode mode = isThirdOctave() ? ff::Mode::ThirdOctave : ff::Mode::Equal;
    if (mode != activeMode)
    {
        activeMode = mode;
        for (auto& f : fir) f.reset();
        convolution.reset();
        xfadeLeft = 0;
        haveSig = false;
    }
    auto& learner = mode == ff::Mode::Equal ? learnerEq : learnerTh;

    // ---- aprendizaje ----
    const bool learnOn = isLearnOn();
    const auto target = ! learnOn ? ff::SpectrumLearner::None
                                  : (isLearnDest() ? ff::SpectrumLearner::Dest : ff::SpectrumLearner::Source);
    learnerEq.setTarget (target);
    learnerTh.setTarget (target);
    framesDest.store (learner.frames (ff::SpectrumLearner::Dest));
    framesSource.store (learner.frames (ff::SpectrumLearner::Source));

    // lo aprendido cambio desde fuera (archivo cargado, estado del proyecto): rehacer la instantanea
    if (dataVersion.load() != lastDataVersion) { lastDataVersion = dataVersion.load(); matchEffective = false; }

    // ---- Match: se fija una instantanea (medias) al activarlo, como el original ----
    const bool matchWanted = isMatchOn() && ! learnOn;
    const bool avail = framesDest.load() > 0 && framesSource.load() > 0;
    if (matchWanted && avail)
    {
        if (! matchEffective) { takeMatchSnapshot(); matchEffective = true; }
    }
    else
        matchEffective = false;

    // ---- diseno: solo cuando cambia algo ----
    std::array<float, ff::kBands> cur {};
    const auto& set = isMatchOn() ? matchParams : bandParams;
    for (int b = 0; b < ff::kBands; ++b) cur[(size_t) b] = set[(size_t) b]->load();
    const float morph = apvts.getRawParameterValue ("morph")->load() / 100.0f;

    const bool changed = ! haveSig || mode != lastSigMode || matchEffective != lastSigMatch
                         || (matchEffective && (morph != lastSigMorph || snapVersion != lastSigSnap)) || cur != lastSigSliders;
    if (changed)
    {
        DesignJob job;
        job.mode = mode;
        job.sampleRate = currentSampleRate.load();
        job.sliders = cur;
        job.morph = morph;
        job.match = matchEffective;
        postDesign (job);
        haveSig = true;
        lastSigMode = mode; lastSigMatch = matchEffective; lastSigMorph = morph;
        lastSigSnap = snapVersion; lastSigSliders = cur;
    }

    // coeficientes nuevos (modo bandas iguales): crossfade desde los anteriores
    if (mode == ff::Mode::Equal && pubReady.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType tl (pubLock);
        if (tl.isLocked())
        {
            prevTaps = taps;
            taps = pubTaps;
            pubReady.store (false, std::memory_order_release);
            xfadeLeft = kXfade;
        }
    }

    // ---- procesado ----
    const int total = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);

    for (int offset = 0; offset < total; offset += kChunk)
    {
        const int n = juce::jmin (kChunk, total - offset);

        for (int i = 0; i < n; ++i) monoIn[(size_t) i] = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = buffer.getReadPointer (ch) + offset;
            for (int i = 0; i < n; ++i) monoIn[(size_t) i] += src[i] / (float) numCh;
        }
        for (int i = 0; i < n; ++i)
        {
            const float x = monoIn[(size_t) i];
            if (learnerEq.advance (x) && learnOn)
            {
                const juce::SpinLock::ScopedLockType sl (learnLock);
                learnerEq.analyseFrame();
            }
            if (learnerTh.advance (x) && learnOn)
            {
                const juce::SpinLock::ScopedLockType sl (learnLock);
                learnerTh.analyseFrame();
            }
        }

        if (mode == ff::Mode::Equal)
        {
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
                        const float t = (float) xf / (float) kXfade;
                        y = y + t * (fir[(size_t) ch].dot (prevTaps.data()) - y);
                        --xf;
                    }
                    data[i] = y;
                }
            }
            xfadeLeft = juce::jmax (0, xfadeLeft - n);
        }
        else
        {
            juce::dsp::AudioBlock<float> block (buffer);
            auto sub = block.getSubsetChannelBlock (0, (size_t) numCh).getSubBlock ((size_t) offset, (size_t) n);
            convolution.process (juce::dsp::ProcessContextReplacing<float> (sub));
        }

        for (int i = 0; i < n; ++i) monoOut[(size_t) i] = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = buffer.getReadPointer (ch) + offset;
            for (int i = 0; i < n; ++i) monoOut[(size_t) i] += src[i] / (float) numCh;
        }

        int w = ringWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            ringIn[(size_t) w]  = monoIn[(size_t) i];
            ringOut[(size_t) w] = monoOut[(size_t) i];
            w = (w + 1) % kRing;
        }
        ringWrite.store (w, std::memory_order_release);
    }
}

juce::AudioProcessorEditor* FreeFilterProcessor::createEditor()
{
    return new FreeFilterEditor (*this);
}

//==============================================================================
// Lo aprendido (Dest/Source, ambos modos) se guarda en el estado del proyecto y en archivos .ffcurve.
static juce::String floatsToBase64 (const std::vector<double>& v)
{
    std::vector<float> f (v.size());
    for (size_t i = 0; i < v.size(); ++i) f[i] = (float) v[i];
    return juce::Base64::toBase64 (f.data(), f.size() * sizeof (float));
}

static bool base64ToDoubles (const juce::String& s, size_t expected, std::vector<double>& out)
{
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64 (mo, s)) return false;
    if (mo.getDataSize() != expected * sizeof (float)) return false;
    const auto* f = static_cast<const float*> (mo.getData());
    out.assign (expected, 0.0);
    for (size_t i = 0; i < expected; ++i) out[i] = (double) f[i];
    return true;
}

bool FreeFilterProcessor::exportSide (bool dest, std::vector<double>& eq, std::vector<double>& th, int& framesEq, int& framesTh)
{
    const auto t = dest ? ff::SpectrumLearner::Dest : ff::SpectrumLearner::Source;
    const juce::SpinLock::ScopedLockType sl (learnLock);
    framesEq = learnerEq.frames (t);
    framesTh = learnerTh.frames (t);
    eq.assign ((size_t) ff::kEqBins, 0.0);
    th.assign ((size_t) ff::kThBins, 0.0);
    learnerEq.averageInto (t, eq.data());
    learnerTh.averageInto (t, th.data());
    return framesEq > 0 || framesTh > 0;
}

void FreeFilterProcessor::importSide (bool dest, const std::vector<double>& eq, const std::vector<double>& th, int framesEq, int framesTh)
{
    const auto t = dest ? ff::SpectrumLearner::Dest : ff::SpectrumLearner::Source;
    {
        const juce::SpinLock::ScopedLockType sl (learnLock);
        if (framesEq > 0 && eq.size() == (size_t) ff::kEqBins) learnerEq.importAverage (t, eq.data(), framesEq);
        else learnerEq.clear (t);
        if (framesTh > 0 && th.size() == (size_t) ff::kThBins) learnerTh.importAverage (t, th.data(), framesTh);
        else learnerTh.clear (t);
    }
    ++dataVersion;
}

juce::ValueTree FreeFilterProcessor::makeLearnedTree()
{
    juce::ValueTree t ("learned");
    for (bool dest : { true, false })
    {
        std::vector<double> eq, th;
        int fe = 0, ft = 0;
        exportSide (dest, eq, th, fe, ft);
        const juce::String p = dest ? "dest" : "source";
        t.setProperty (p + "FramesEq", fe, nullptr);
        t.setProperty (p + "FramesTh", ft, nullptr);
        if (fe > 0) t.setProperty (p + "Eq", floatsToBase64 (eq), nullptr);
        if (ft > 0) t.setProperty (p + "Th", floatsToBase64 (th), nullptr);
    }
    return t;
}

void FreeFilterProcessor::readLearnedTree (const juce::ValueTree& t)
{
    if (! t.isValid()) return;
    for (bool dest : { true, false })
    {
        const juce::String p = dest ? "dest" : "source";
        const int fe = (int) t.getProperty (p + "FramesEq", 0), ft = (int) t.getProperty (p + "FramesTh", 0);
        std::vector<double> eq, th;
        const bool okE = fe > 0 && base64ToDoubles (t.getProperty (p + "Eq").toString(), (size_t) ff::kEqBins, eq);
        const bool okT = ft > 0 && base64ToDoubles (t.getProperty (p + "Th").toString(), (size_t) ff::kThBins, th);
        importSide (dest, eq, th, okE ? fe : 0, okT ? ft : 0);
    }
}

bool FreeFilterProcessor::saveLearned (bool dest, const juce::File& file)
{
    std::vector<double> eq, th;
    int fe = 0, ft = 0;
    if (! exportSide (dest, eq, th, fe, ft)) return false;

    juce::XmlElement root ("FreeFilterCurve");
    root.setAttribute ("version", 1);
    root.setAttribute ("side", dest ? "dest" : "source");
    root.setAttribute ("sampleRate", currentSampleRate.load());
    root.setAttribute ("framesEq", fe);
    root.setAttribute ("framesTh", ft);
    if (fe > 0) root.createNewChildElement ("eq")->addTextElement (floatsToBase64 (eq));
    if (ft > 0) root.createNewChildElement ("third")->addTextElement (floatsToBase64 (th));
    return root.writeTo (file);
}

bool FreeFilterProcessor::loadLearned (bool dest, const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("FreeFilterCurve")) return false;

    const int fe = xml->getIntAttribute ("framesEq", 0), ft = xml->getIntAttribute ("framesTh", 0);
    std::vector<double> eq, th;
    const auto* eqEl = xml->getChildByName ("eq");
    const auto* thEl = xml->getChildByName ("third");
    const bool okE = fe > 0 && eqEl != nullptr && base64ToDoubles (eqEl->getAllSubText().trim(), (size_t) ff::kEqBins, eq);
    const bool okT = ft > 0 && thEl != nullptr && base64ToDoubles (thEl->getAllSubText().trim(), (size_t) ff::kThBins, th);
    if (! okE && ! okT) return false;
    importSide (dest, eq, th, okE ? fe : 0, okT ? ft : 0);
    return true;
}

void FreeFilterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("learned"), nullptr);
    state.appendChild (makeLearnedTree(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FreeFilterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            const auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            readLearnedTree (tree.getChildWithName ("learned"));
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FreeFilterProcessor();
}
