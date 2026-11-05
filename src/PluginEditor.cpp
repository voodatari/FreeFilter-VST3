#include "PluginEditor.h"

namespace
{
    // Geometría en el espacio de diseño 454 x 417
    const juce::Rectangle<float> analyzerRect { 22.0f, 70.0f, 410.0f, 172.0f };
    const juce::Rectangle<float> plotRect     { 28.0f, 76.0f, 360.0f, 160.0f };
    const juce::Rectangle<float> boardRect    { 25.0f, 270.0f, 360.0f, 70.0f };
    const juce::Rectangle<float> titleRect    { 200.0f, 8.0f, 245.0f, 26.0f };
    constexpr float bandW = 12.0f;
    constexpr float dbTop = 20.0f, dbBottom = -80.0f;

    const juce::Colour panelTop    { 0xffc2432f };
    const juce::Colour panelBottom { 0xff8f2a1f };

    enum ButtonId { Source, Dest, Learn, Match, ModeFree, ModeLevel, ModeLine, Reset, Freeze, Lin, Log, NumButtons };

    juce::Rectangle<float> buttonRect (int id)
    {
        switch (id)
        {
            case Source:    return { 28.0f, 10.0f, 40.0f, 17.0f };
            case Dest:      return { 85.0f, 10.0f, 40.0f, 17.0f };
            case Learn:     return { 143.0f, 10.0f, 40.0f, 17.0f };
            case Match:     return { 199.0f, 10.0f, 40.0f, 17.0f };
            case ModeFree:  return { 30.0f, 362.0f, 32.0f, 20.0f };
            case ModeLevel: return { 67.0f, 362.0f, 32.0f, 20.0f };
            case ModeLine:  return { 104.0f, 362.0f, 32.0f, 20.0f };
            case Reset:     return { 192.0f, 362.0f, 40.0f, 18.0f };
            case Freeze:    return { 242.0f, 362.0f, 40.0f, 18.0f };
            case Lin:       return { 294.0f, 362.0f, 32.0f, 18.0f };
            case Log:       return { 334.0f, 362.0f, 32.0f, 18.0f };
            default:        return {};
        }
    }

    juce::Font uiFont (float h, bool bold = false, bool italic = false)
    {
        int style = juce::Font::plain;
        if (bold)   style |= juce::Font::bold;
        if (italic) style |= juce::Font::italic;
        return juce::Font (juce::FontOptions (h, style));
    }

    juce::String formatHz (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + "k" : juce::String (juce::roundToInt (hz));
    }
}

//==============================================================================
FreeFilterEditor::FreeFilterEditor (FreeFilterProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (kFftSize - 1)));

    specIn.fill (-120.0f);
    specOut.fill (-120.0f);
    startValues.fill (0.0f);

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) designW / designH);
    setResizeLimits (designW, designH, designW * 3, designH * 3);
    setSize (designW * 5 / 4, designH * 5 / 4);

    startTimerHz (30);
}

FreeFilterEditor::~FreeFilterEditor() { stopTimer(); }

juce::AffineTransform FreeFilterEditor::designToScreen() const
{
    return juce::AffineTransform::scale ((float) getWidth() / designW, (float) getHeight() / designH);
}

juce::Point<float> FreeFilterEditor::toDesign (juce::Point<float> p) const
{
    return p.transformedBy (designToScreen().inverted());
}

bool FreeFilterEditor::isLogAxis() const
{
    return processor.isThirdOctave();   // Log = tercios de octava, Lin = bandas iguales
}

void FreeFilterEditor::setStatus (const juce::String& s)
{
    statusText = s;
    statusUntil = juce::Time::currentTimeMillis() + 3500;
}

juce::String FreeFilterEditor::idleStatus() const
{
    const bool d = processor.learnedFrames (true) > 0, s = processor.learnedFrames (false) > 0;
    if (processor.isLearnOn()) return processor.isLearnDest() ? "Learning destination..." : "Learning source...";
    if (d && s) return "Source and Destination learned.";
    if (d) return "Destination learned.";
    if (s) return "Source learned.";
    return {};
}

void FreeFilterEditor::setMorphFromX (float x)
{
    const juce::Rectangle<float> bar (28.0f, 40.0f, 105.0f, 14.0f);
    const float t = juce::jlimit (0.0f, 1.0f, (x - bar.getX()) / bar.getWidth());
    if (auto* p = processor.apvts.getParameter ("morph"))
        p->setValueNotifyingHost (p->convertTo0to1 (t * 200.0f));
}

float FreeFilterEditor::freqToX (float hz) const
{
    const float nyq = (float) processor.getSampleRate_() * 0.5f;
    if (isLogAxis())
    {
        const float lo = 20.0f, hi = juce::jmin (20000.0f, nyq);
        const float t = std::log (juce::jmax (lo, hz) / lo) / std::log (hi / lo);
        return plotRect.getX() + juce::jlimit (0.0f, 1.0f, t) * plotRect.getWidth();
    }
    return plotRect.getX() + juce::jlimit (0.0f, 1.0f, hz / nyq) * plotRect.getWidth();
}

float FreeFilterEditor::xToFreq (float x) const
{
    const float nyq = (float) processor.getSampleRate_() * 0.5f;
    const float t = juce::jlimit (0.0f, 1.0f, (x - plotRect.getX()) / plotRect.getWidth());
    if (isLogAxis())
    {
        const float lo = 20.0f, hi = juce::jmin (20000.0f, nyq);
        return lo * std::pow (hi / lo, t);
    }
    return t * nyq;
}

float FreeFilterEditor::dbToY (float db) const
{
    return plotRect.getY() + (dbTop - juce::jlimit (dbBottom, dbTop, db)) / (dbTop - dbBottom) * plotRect.getHeight();
}

int FreeFilterEditor::bandAt (float x) const
{
    return juce::jlimit (0, ff::kBands - 1, (int) std::floor ((x - boardRect.getX()) / bandW));
}

float FreeFilterEditor::valueAt (float y) const
{
    const float t = juce::jlimit (0.0f, 1.0f, (y - boardRect.getY()) / boardRect.getHeight());
    return 15.0f - t * 30.0f;
}

void FreeFilterEditor::setBand (int b, float dB)
{
    if (auto* prm = processor.apvts.getParameter (processor.activeBandId (b)))
    {
        if (touched.insert (b).second) prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (-15.0f, 15.0f, dB)));
    }
}

void FreeFilterEditor::resetAll() { processor.requestReset(); }

//==============================================================================
void FreeFilterEditor::resized() { rebuildBackground(); }

void FreeFilterEditor::rebuildBackground()
{
    const float scale = juce::Component::getApproximateScaleFactorForComponent (this);
    const int pw = juce::jmax (1, juce::roundToInt (getWidth() * scale));
    const int ph = juce::jmax (1, juce::roundToInt (getHeight() * scale));
    background = juce::Image (juce::Image::RGB, pw, ph, true);
    juce::Graphics g (background);

    g.setGradientFill (juce::ColourGradient (panelTop, 0.0f, 0.0f, panelBottom, 0.0f, (float) ph, false));
    g.fillAll();

    // trama metálica fina y fija
    juce::Random rng (0xF1F1);
    for (int y = 0; y < ph; ++y)
    {
        const float a = rng.nextFloat();
        g.setColour ((a > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.015f + 0.05f * std::abs (a - 0.5f)));
        g.drawHorizontalLine (y, 0.0f, (float) pw);
    }
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.drawRect (0, 0, pw, ph, 1);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawHorizontalLine (ph - 1, 0.0f, (float) pw);
    g.drawVerticalLine (pw - 1, 0.0f, (float) ph);
}

void FreeFilterEditor::drawButton (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, bool enabled)
{
    g.setGradientFill (juce::ColourGradient (juce::Colour (on ? 0xffb8b8b8 : 0xfff0f0f0), r.getX(), r.getY(),
                                             juce::Colour (on ? 0xff8c8c8c : 0xffbdbdbd), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
    if (! enabled)
    {
        g.setColour (juce::Colour (0xffb04030).withAlpha (0.45f));
        g.fillRoundedRectangle (r, 2.0f);
    }
    g.setColour (enabled ? juce::Colours::black : juce::Colours::black.withAlpha (0.4f));
    g.setFont (uiFont (10.0f, true));
    g.drawText (text, r, juce::Justification::centred, false);
}

void FreeFilterEditor::paintButtons (juce::Graphics& g)
{
    // fila superior: Source/Dest (selector), Learn y Match
    drawButton (g, buttonRect (Source), "Source", ! processor.isLearnDest());
    drawButton (g, buttonRect (Dest),   "Dest.",  processor.isLearnDest());
    drawButton (g, buttonRect (Learn),  "Learn",  processor.isLearnOn());
    drawButton (g, buttonRect (Match),  "Match",  processor.isMatchOn(), processor.matchAvailable() || processor.isMatchOn());

    // modos de dibujo
    const DrawMode modes[3] = { DrawMode::freehand, DrawMode::level, DrawMode::line };
    for (int i = 0; i < 3; ++i)
    {
        const auto r = buttonRect (ModeFree + i);
        drawButton (g, r, "", drawMode == modes[i]);
        g.setColour (juce::Colours::black);
        juce::Path p;
        const auto c = r.reduced (6.0f, 5.0f);
        if (i == 0)
        {
            p.startNewSubPath (c.getX(), c.getBottom());
            p.cubicTo (c.getX() + c.getWidth() * 0.3f, c.getY() - 2, c.getX() + c.getWidth() * 0.6f, c.getBottom() + 2, c.getRight(), c.getY());
            g.strokePath (p, juce::PathStrokeType (1.4f));
        }
        else if (i == 1)
        {
            g.drawLine (c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.4f);
        }
        else
        {
            g.drawLine (c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.4f);
            g.fillEllipse (c.getX() - 2, c.getBottom() - 2, 4, 4);
            g.fillEllipse (c.getRight() - 2, c.getY() - 2, 4, 4);
        }
    }

    g.setColour (juce::Colours::white);
    g.setFont (uiFont (11.0f, true));
    g.drawText ("Global", juce::Rectangle<float> (146.0f, 362.0f, 44.0f, 18.0f), juce::Justification::centredRight, false);

    const bool frozen = processor.apvts.getRawParameterValue ("freeze")->load() > 0.5f;
    drawButton (g, buttonRect (Reset),  "Reset",  false);
    drawButton (g, buttonRect (Freeze), "Freeze", frozen);
    drawButton (g, buttonRect (Lin),    "Lin",    ! isLogAxis());
    drawButton (g, buttonRect (Log),    "Log",    isLogAxis());
}

void FreeFilterEditor::paintBands (juce::Graphics& g)
{
    // fondo oscuro de los sliders
    g.setColour (juce::Colour (0xff1b1c20));
    g.fillRoundedRectangle (boardRect.expanded (2.0f, 4.0f), 2.0f);

    const float cy = boardRect.getCentreY();
    for (int b = 0; b < ff::kBands; ++b)
    {
        const float x = boardRect.getX() + b * bandW;
        const float v = processor.getBandValue (b);
        const float vy = boardRect.getY() + (15.0f - v) / 30.0f * boardRect.getHeight();

        g.setColour (juce::Colour (0xff3a3c44));
        g.fillRect (x + 1.5f, boardRect.getY(), bandW - 3.0f, boardRect.getHeight());

        // relleno desde 0 dB hasta el valor
        g.setColour (juce::Colour (0xff6d7fd8).withAlpha (0.55f));
        g.fillRect (x + 1.5f, juce::jmin (vy, cy), bandW - 3.0f, std::abs (vy - cy));

        // marcador de valor
        g.setColour (juce::Colour (0xffe9e9ef));
        g.fillRect (x + 1.0f, vy - 1.5f, bandW - 2.0f, 3.0f);

        // "LED" superior
        const bool active = std::abs (v) > 0.05f;
        g.setColour (active ? juce::Colour (0xffd25cff) : juce::Colour (0xff5b3b86));
        g.fillEllipse (x + 2.0f, boardRect.getY() - 9.0f, bandW - 4.0f, bandW - 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (x + 2.0f, boardRect.getY() - 9.0f, bandW - 4.0f, bandW - 4.0f, 0.8f);
    }

    // línea de 0 dB
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawHorizontalLine ((int) cy, boardRect.getX(), boardRect.getRight());

    // escala dB a la derecha
    g.setFont (uiFont (9.0f, true));
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    const int marks[7] = { 15, 10, 5, 0, -5, -10, -15 };
    g.drawText ("dB", juce::Rectangle<float> (394.0f, 254.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
    for (int m : marks)
    {
        const float y = boardRect.getY() + (15.0f - (float) m) / 30.0f * boardRect.getHeight();
        g.drawText (juce::String (m), juce::Rectangle<float> (396.0f, y - 6.0f, 28.0f, 12.0f), juce::Justification::centredLeft, false);
        g.drawHorizontalLine ((int) y, 390.0f, 394.0f);
    }

    // cuadro de valor
    const juce::Rectangle<float> box { 394.0f, 358.0f, 36.0f, 16.0f };
    g.setColour (juce::Colour (0xff0b0b12));
    g.fillRect (box);
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.drawRect (box, 1.0f);
    g.setColour (juce::Colours::white);
    g.setFont (uiFont (10.0f, true));
    g.drawText (readDb, box, juce::Justification::centred, false);
}

void FreeFilterEditor::paintAnalyzer (juce::Graphics& g)
{
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (analyzerRect, 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.drawRoundedRectangle (analyzerRect, 3.0f, 1.0f);

    g.saveState();
    g.reduceClipRegion (plotRect.toNearestInt());

    // rejilla
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    for (int d = -80; d <= 20; d += 20)
        g.drawHorizontalLine ((int) dbToY ((float) d), plotRect.getX(), plotRect.getRight());
    const float nyq = (float) processor.getSampleRate_() * 0.5f;
    if (isLogAxis())
    {
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            if (f < nyq) g.drawVerticalLine ((int) freqToX (f), plotRect.getY(), plotRect.getBottom());
    }
    else
    {
        for (float f = 5000.0f; f < nyq; f += 5000.0f)
            g.drawVerticalLine ((int) freqToX (f), plotRect.getY(), plotRect.getBottom());
    }

    auto drawSpec = [&] (const std::array<float, kFftSize / 2>& spec, juce::Colour c)
    {
        juce::Path p;
        bool started = false;
        for (float x = plotRect.getX(); x <= plotRect.getRight(); x += 1.0f)
        {
            const float f = xToFreq (x);
            const float bin = f / nyq * (float) (kFftSize / 2 - 1);
            const int i0 = juce::jlimit (0, kFftSize / 2 - 2, (int) bin);
            const float t = bin - (float) i0;
            const float db = spec[(size_t) i0] * (1.0f - t) + spec[(size_t) i0 + 1] * t;
            const float y = dbToY (db);
            if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (1.0f));
    };
    drawSpec (specIn,  juce::Colour (0xff3cd23c));
    drawSpec (specOut, juce::Colour (0xffe53935));

    // curva del filtro (amarilla)
    {
        juce::Path p;
        bool started = false;
        for (float x = plotRect.getX(); x <= plotRect.getRight(); x += 2.0f)
        {
            const float y = dbToY (processor.responseDbAt (xToFreq (x)));
            if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
        }
        g.setColour (juce::Colour (0xfff2c418));
        g.strokePath (p, juce::PathStrokeType (1.4f));
    }
    g.restoreState();

    // etiquetas
    g.setFont (uiFont (9.0f, true));
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.drawText ("dB", juce::Rectangle<float> (394.0f, 72.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
    for (int d = 20; d >= -80; d -= 20)
        g.drawText (juce::String (d), juce::Rectangle<float> (394.0f, dbToY ((float) d) - 6.0f, 34.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    g.drawText ("kHz", juce::Rectangle<float> (394.0f, 224.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
    g.setColour (juce::Colours::white.withAlpha (0.55f));
    if (isLogAxis())
    {
        for (float f : { 100.0f, 1000.0f, 10000.0f })
            if (f < nyq)
                g.drawText (juce::String (f / 1000.0f, f < 1000.0f ? 1 : 0), juce::Rectangle<float> (freqToX (f) + 2.0f, 224.0f, 30.0f, 12.0f),
                            juce::Justification::centredLeft, false);
    }
    else
    {
        for (float f = 5000.0f; f < nyq; f += 5000.0f)
            g.drawText (juce::String (f / 1000.0f, 0), juce::Rectangle<float> (freqToX (f) + 2.0f, 224.0f, 30.0f, 12.0f),
                        juce::Justification::centredLeft, false);
    }

    // leyenda
    g.setFont (uiFont (10.0f, true));
    const struct { const char* name; juce::Colour c; float x; } legend[3] = {
        { "Input", juce::Colour (0xff3cd23c), 138.0f }, { "Output", juce::Colour (0xffe53935), 196.0f }, { "Filter", juce::Colour (0xfff2c418), 262.0f } };
    for (auto& l : legend)
    {
        g.setColour (l.c);
        g.fillRect (l.x, 254.0f, 14.0f, 2.0f);
        g.drawText (l.name, juce::Rectangle<float> (l.x + 18.0f, 247.0f, 50.0f, 14.0f), juce::Justification::centredLeft, false);
    }
}

void FreeFilterEditor::paint (juce::Graphics& g)
{
    g.drawImage (background, getLocalBounds().toFloat());
    if (showAbout) { paintAbout (g); return; }

    g.addTransform (designToScreen());

    // título
    {
        auto f = uiFont (22.0f, true, true);
        g.setFont (f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawText ("FreeFilter", titleRect.translated (1.5f, 1.5f), juce::Justification::centredRight, false);
        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.drawText ("FreeFilter", titleRect, juce::Justification::centredRight, false);
    }

    // fila de lecturas (Morph / Hz / dB)
    {
        const juce::Rectangle<float> strip { 22.0f, 33.0f, 410.0f, 28.0f };
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillRoundedRectangle (strip, 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (uiFont (10.0f, true));
        g.drawText ("Morph %", juce::Rectangle<float> (138.0f, 40.0f, 50.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("Hz", juce::Rectangle<float> (244.0f, 40.0f, 30.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("dB", juce::Rectangle<float> (338.0f, 40.0f, 28.0f, 14.0f), juce::Justification::centredRight, false);

        auto box = [&] (juce::Rectangle<float> r, const juce::String& txt, bool dim)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.drawRect (r, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (dim ? 0.35f : 1.0f));
            g.drawText (txt, r, juce::Justification::centred, false);
        };
        // Morph 0..200 %
        const float morphPct = processor.apvts.getRawParameterValue ("morph")->load();
        const bool morphOn = processor.isMatchOn();
        const juce::Rectangle<float> bar (28.0f, 40.0f, 105.0f, 14.0f);
        g.setColour (juce::Colour (0xff1a1a1a));
        g.fillRect (bar);
        g.setColour (juce::Colour (morphOn ? 0xff6d7fd8 : 0xff4a4a55));
        g.fillRect (bar.withWidth (bar.getWidth() * morphPct / 200.0f));
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawRect (bar, 1.0f);
        g.drawVerticalLine ((int) (bar.getX() + bar.getWidth() * 0.5f), bar.getY(), bar.getBottom());
        box ({ 192.0f, 38.0f, 50.0f, 18.0f }, juce::String (juce::roundToInt (morphPct)), ! morphOn);
        box ({ 278.0f, 38.0f, 56.0f, 18.0f }, readHz, false);
        box ({ 370.0f, 38.0f, 52.0f, 18.0f }, readDb, false);
    }

    paintAnalyzer (g);
    paintBands (g);
    paintButtons (g);

    // estado (Learn/Match)
    {
        juce::String msg = juce::Time::currentTimeMillis() < statusUntil ? statusText : idleStatus();
        if (msg.isNotEmpty())
        {
            g.setFont (uiFont (10.0f, false, true));
            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.drawText (msg, juce::Rectangle<float> (28.0f, 392.0f, 220.0f, 18.0f), juce::Justification::centredLeft, false);
        }
    }

    // firma en la esquina inferior derecha
    {
        const juce::Rectangle<float> sig { 250.0f, 392.0f, 196.0f, 18.0f };
        g.setFont (uiFont (12.0f, true));
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawText ("Recomp by VooD", sig.translated (1.0f, 1.0f), juce::Justification::centredRight, false);
        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.drawText ("Recomp by VooD", sig, juce::Justification::centredRight, false);
    }
}

void FreeFilterEditor::paintAbout (juce::Graphics& g)
{
    g.addTransform (designToScreen());
    g.setFont (uiFont (26.0f, true, true));
    g.setColour (juce::Colours::white);
    g.drawText ("FreeFilter", juce::Rectangle<float> (0.0f, 30.0f, (float) designW, 34.0f), juce::Justification::centred, false);

    g.setFont (uiFont (12.0f));
    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.drawFittedText (juce::String::fromUTF8 (
                          "Algoritmo original: ME FreeFilter (Steinberg Mastering Edition)\n"
                          "\xC2\xA9 1998-2001 Spectral Design GmbH.\n\n"
                          "Reimplementaci\xC3\xB3n en JUCE para VST3 / AU,\n"
                          "Windows x64 y macOS Apple Silicon.\n"
                          "Bandas iguales, Learn / Match / Morph y tercios de octava.\n"
                          "Contrastado con el original (ver docs/ANALISIS.md).\n\n"
                          "Clic para cerrar."),
                      juce::Rectangle<int> (30, 80, designW - 60, 240), juce::Justification::centred, 12);

    g.setFont (uiFont (12.0f, true));
    g.drawText ("Recomp by VooD", juce::Rectangle<float> (250.0f, 392.0f, 196.0f, 18.0f), juce::Justification::centredRight, false);
}

//==============================================================================
void FreeFilterEditor::timerCallback()
{
    const bool frozen = processor.apvts.getRawParameterValue ("freeze")->load() > 0.5f;
    if (! frozen)
    {
        auto update = [&] (std::array<float, kFftSize / 2>& spec, bool output)
        {
            processor.readAnalyzer (sampBuf.data(), kFftSize, output);
            for (int i = 0; i < kFftSize; ++i) fftBuf[(size_t) i] = sampBuf[(size_t) i] * window[(size_t) i];
            for (int i = kFftSize; i < kFftSize * 2; ++i) fftBuf[(size_t) i] = 0.0f;
            fft.performFrequencyOnlyForwardTransform (fftBuf.data());
            for (int k = 0; k < kFftSize / 2; ++k)
            {
                const float db = 20.0f * std::log10 (fftBuf[(size_t) k] / (kFftSize * 0.25f) + 1.0e-9f);
                spec[(size_t) k] = juce::jmax (db, spec[(size_t) k] - 1.5f);   // ataque inmediato, caída suave
            }
        };
        update (specIn, false);
        update (specOut, true);
    }
    repaint();
}

//==============================================================================
void FreeFilterEditor::mouseMove (const juce::MouseEvent& e)
{
    hover = toDesign (e.position);
    readHz.clear();
    readDb.clear();
    if (boardRect.expanded (0.0f, 8.0f).contains (hover))
    {
        const int b = bandAt (hover.x);
        const float centre = processor.isThirdOctave() ? (float) ff::kThirdOctave[b]
                                                       : (float) ((b + 0.5) * processor.getSampleRate_() / 60.0);
        readHz = formatHz (centre);
        readDb = juce::String (processor.getBandValue (b), 1);
    }
    else if (plotRect.contains (hover))
    {
        const float f = xToFreq (hover.x);
        readHz = formatHz (f);
        readDb = juce::String (processor.responseDbAt (f), 1);
    }
}

void FreeFilterEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = toDesign (e.position);

    if (showAbout) { showAbout = false; repaint(); return; }
    if (titleRect.contains (p)) { showAbout = true; repaint(); return; }

    // botones
    for (int id = 0; id < NumButtons; ++id)
    {
        if (! buttonRect (id).contains (p)) continue;
        switch (id)
        {
            case ModeFree:  drawMode = DrawMode::freehand; break;
            case ModeLevel: drawMode = DrawMode::level;    break;
            case ModeLine:  drawMode = DrawMode::line;     break;
            case Reset:     resetAll();                    break;
            case Freeze:
                if (auto* prm = processor.apvts.getParameter ("freeze"))
                    prm->setValueNotifyingHost (prm->getValue() > 0.5f ? 0.0f : 1.0f);
                break;
            case Lin: if (auto* prm = processor.apvts.getParameter ("thirds")) prm->setValueNotifyingHost (0.0f); break;
            case Log: if (auto* prm = processor.apvts.getParameter ("thirds")) prm->setValueNotifyingHost (1.0f); break;
            case Source: if (auto* prm = processor.apvts.getParameter ("learnDest")) prm->setValueNotifyingHost (0.0f); break;
            case Dest:   if (auto* prm = processor.apvts.getParameter ("learnDest")) prm->setValueNotifyingHost (1.0f); break;
            case Learn:
                if (auto* prm = processor.apvts.getParameter ("learn"))
                {
                    const bool turnOn = prm->getValue() < 0.5f;
                    if (turnOn)
                        if (auto* m = processor.apvts.getParameter ("matchOn")) m->setValueNotifyingHost (0.0f);
                    prm->setValueNotifyingHost (turnOn ? 1.0f : 0.0f);
                }
                break;
            case Match:
                if (processor.isLearnOn())                       setStatus ("Disabled. PlugIn is in learn mode.");
                else if (! processor.matchAvailable() && ! processor.isMatchOn())
                                                                 setStatus ("Disabled. Learn both source and destination data !");
                else if (auto* prm = processor.apvts.getParameter ("matchOn"))
                    prm->setValueNotifyingHost (prm->getValue() > 0.5f ? 0.0f : 1.0f);
                break;
            default: break;
        }
        repaint();
        return;
    }

    // barra de Morph
    if (juce::Rectangle<float> (28.0f, 38.0f, 105.0f, 18.0f).contains (p))
    {
        if (processor.isMatchOn()) { morphDragging = true; setMorphFromX (p.x); }
        else setStatus ("Disabled. Activate match button first !");
        return;
    }

    // tablero de sliders
    if (boardRect.expanded (0.0f, 6.0f).contains (p))
    {
        dragStart = p;
        lastBand = -1;
        draggingBoard = true;
        for (int b = 0; b < ff::kBands; ++b) startValues[(size_t) b] = processor.getBandValue (b);
        mouseDrag (e);
    }
}

void FreeFilterEditor::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = toDesign (e.position);
    if (morphDragging) { setMorphFromX (p.x); return; }
    if (showAbout || ! draggingBoard) return;

    const int b = bandAt (p.x);
    const float v = valueAt (p.y);

    switch (drawMode)
    {
        case DrawMode::freehand:
        {
            if (lastBand < 0) { setBand (b, v); }
            else
            {
                const int lo = juce::jmin (lastBand, b), hi = juce::jmax (lastBand, b);
                const float vl = processor.getBandValue (lastBand);
                for (int i = lo; i <= hi; ++i)
                {
                    const float t = hi == lo ? 1.0f : (float) (i - lo) / (float) (hi - lo);
                    const float from = lastBand <= b ? vl : v, to = lastBand <= b ? v : vl;
                    setBand (i, from + t * (to - from));
                }
            }
            break;
        }
        case DrawMode::level:
            for (int i = 0; i < ff::kBands; ++i) setBand (i, v);
            break;
        case DrawMode::line:
        {
            const int b0 = bandAt (dragStart.x);
            const float v0 = valueAt (dragStart.y);
            const int lo = juce::jmin (b0, b), hi = juce::jmax (b0, b);
            for (int i = lo; i <= hi; ++i)
            {
                const float t = hi == lo ? 1.0f : (float) (i - b0) / (float) (b - b0 == 0 ? 1 : (b - b0));
                setBand (i, v0 + t * (v - v0));
            }
            break;
        }
    }
    lastBand = b;
    mouseMove (e);
}

void FreeFilterEditor::mouseUp (const juce::MouseEvent&)
{
    for (int b : touched)
        if (auto* prm = processor.apvts.getParameter (processor.activeBandId (b)))
            prm->endChangeGesture();
    touched.clear();
    lastBand = -1;
    draggingBoard = false;
    morphDragging = false;
}

void FreeFilterEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = toDesign (e.position);
    if (! boardRect.expanded (0.0f, 6.0f).contains (p)) return;
    if (drawMode == DrawMode::level) { resetAll(); return; }
    const int b = bandAt (p.x);
    setBand (b, 0.0f);
    mouseUp (e);
}
