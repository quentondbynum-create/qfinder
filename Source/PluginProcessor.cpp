#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
QFinderAnalyzer::QFinderAnalyzer() : juce::Thread ("QFinderAnalyzer")
{
    windowBuf.fill (0.0f);
    startThread();
}

QFinderAnalyzer::~QFinderAnalyzer()
{
    stopThread (2000);
}

void QFinderAnalyzer::prepare (double sr)
{
    sampleRate = sr;
    requestReset();
}

void QFinderAnalyzer::requestReset()
{
    resetRequested.store (true);
}

void QFinderAnalyzer::pushSamples (const float* data, int numSamples)
{
    int start1, size1, start2, size2;
    fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    // If the FIFO is full we drop the oldest data (analysis only — never block audio).
    int toWrite = juce::jmin (numSamples, size1 + size2);
    int written = 0;
    if (size1 > 0)
    {
        int n = juce::jmin (size1, toWrite);
        std::copy (data, data + n, fifoBuffer.begin() + start1);
        written += n;
    }
    if (written < toWrite && size2 > 0)
    {
        int n = juce::jmin (size2, toWrite - written);
        std::copy (data + written, data + written + n, fifoBuffer.begin() + start2);
        written += n;
    }
    fifo.finishedWrite (written);
}

void QFinderAnalyzer::clearState()
{
    windowBuf.fill (0.0f);
    prevMag.fill (0.0f);
    fluxHistory.fill (0.0f);
    fluxWritePos = 0;
    fluxCount = 0;
    chroma.fill (0.0f);
    candidateBpm = 0.0f;
    stableTimeSec = 0.0;
    framesSinceBpmEval = 0;
    keyCandidate = -1;
    keyStableEvals = 0;
    framesSinceKeyEval = 0;
    elapsedSec = 0.0;
    oneShotDone = false;

    // Drain the FIFO
    int start1, size1, start2, size2;
    fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);
    fifo.finishedRead (size1 + size2);

    bpm.store (0.0f);
    bpmConf.store (0.0f);
    keyIndex.store (-1);
    keyConf.store (0.0f);
    fluxLevel.store (0.0f);
    analysisTimeSec.store (0.0);
}

void QFinderAnalyzer::run()
{
    std::array<float, hopSize> hopBuf {};

    while (! threadShouldExit())
    {
        if (resetRequested.exchange (false))
            clearState();

        // Wait until a full hop is available
        if (fifo.getNumReady() < hopSize)
        {
            wait (20);
            continue;
        }

        int start1, size1, start2, size2;
        fifo.prepareToRead (hopSize, start1, size1, start2, size2);
        int read = 0;
        if (size1 > 0) { std::copy (fifoBuffer.begin() + start1, fifoBuffer.begin() + start1 + size1, hopBuf.begin()); read += size1; }
        if (size2 > 0) { std::copy (fifoBuffer.begin() + start2, fifoBuffer.begin() + start2 + size2, hopBuf.begin() + read); read += size2; }
        fifo.finishedRead (read);

        if (read < hopSize)
            continue;

        // Slide the window and append the new hop
        std::copy (windowBuf.begin() + hopSize, windowBuf.end(), windowBuf.begin());
        std::copy (hopBuf.begin(), hopBuf.end(), windowBuf.begin() + (fftSize - hopSize));

        processHop();
    }
}

void QFinderAnalyzer::processHop()
{
    const double hopSec = (double) hopSize / sampleRate;
    elapsedSec += hopSec;

    // Copy windowed data into the FFT buffer
    std::copy (windowBuf.begin(), windowBuf.end(), fftBuf.begin());
    std::fill (fftBuf.begin() + fftSize, fftBuf.end(), 0.0f);
    window.multiplyWithWindowingTable (fftBuf.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftBuf.data());

    // ---- spectral flux onset envelope ----
    float flux = 0.0f;
    for (int b = 2; b < fftSize / 2; ++b) // skip DC/rumble bins
    {
        float mag = fftBuf[(size_t) b];
        float diff = mag - prevMag[(size_t) b];
        if (diff > 0.0f)
            flux += diff;
        prevMag[(size_t) b] = mag;
    }
    fluxHistory[(size_t) fluxWritePos] = flux;
    fluxWritePos = (fluxWritePos + 1) % fluxHistoryLen;
    fluxCount = juce::jmin (fluxCount + 1, fluxHistoryLen);

    // Smooth the flux meter level for the UI
    float target = juce::jlimit (0.0f, 1.0f, flux * 0.02f);
    fluxLevel.store (fluxLevel.load() * 0.85f + target * 0.15f);

    // ---- chromagram (pitch-class accumulation) ----
    // 4 s decay time constant
    const float decay = std::pow (0.5f, (float) (hopSec / 4.0));
    for (auto& c : chroma) c *= decay;

    for (int b = 2; b < fftSize / 2; ++b)
    {
        float freq = (float) b * (float) sampleRate / (float) fftSize;
        if (freq < 55.0f || freq > 4200.0f)
            continue;
        float midi = 69.0f + 12.0f * std::log2 (freq / 440.0f);
        int pc = ((int) std::lround (midi) % 12 + 12) % 12;
        chroma[(size_t) pc] += std::sqrt (fftBuf[(size_t) b]);
    }

    analysisTimeSec.store (elapsedSec);

    if (++framesSinceBpmEval >= 43)  // ~0.5 s
    {
        framesSinceBpmEval = 0;
        evaluateBpm();
    }
    if (++framesSinceKeyEval >= 86)  // ~1 s
    {
        framesSinceKeyEval = 0;
        evaluateKey();
    }
}

void QFinderAnalyzer::evaluateBpm()
{
    constexpr int histLen = 512; // ~6 s of flux at 44.1 k
    if (fluxCount < histLen)
        return;

    // Gather the last histLen flux frames in chronological order
    std::array<float, histLen> x {};
    for (int i = 0; i < histLen; ++i)
    {
        int pos = (fluxWritePos - histLen + i + fluxHistoryLen * 2) % fluxHistoryLen;
        x[(size_t) i] = fluxHistory[(size_t) pos];
    }

    float mean = 0.0f;
    for (auto v : x) mean += v;
    mean /= (float) histLen;

    float var0 = 0.0f;
    for (auto v : x) { float d = v - mean; var0 += d * d; }
    if (var0 < 1e-9f)
        return; // silence — nothing to lock onto

    // Lag range for 60..200 BPM: lag = 60*sr / (hop*bpm)
    int lagMin = (int) std::ceil  (60.0 * sampleRate / (hopSize * 200.0));
    int lagMax = (int) std::floor (60.0 * sampleRate / (hopSize * 60.0));
    lagMin = juce::jmax (lagMin, 4);
    lagMax = juce::jmin (lagMax, histLen / 2);

    float bestR = -1.0f;
    int bestLag = 0;
    float secondR = -1.0f;
    float sumR = 0.0f;
    int nLags = 0;

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        float r = 0.0f;
        for (int i = lag; i < histLen; ++i)
            r += (x[(size_t) i] - mean) * (x[(size_t) (i - lag)] - mean);
        r /= var0;
        sumR += r;
        ++nLags;

        if (r > bestR) { secondR = bestR; bestR = r; bestLag = lag; }
        else if (r > secondR) { secondR = r; }
    }

    if (bestLag == 0)
        return;

    float candidate = (float) (60.0 * sampleRate / (hopSize * bestLag));

    // Fold into 60..200 (octave correction)
    while (candidate > 200.0f) candidate *= 0.5f;
    while (candidate < 60.0f)  candidate *= 2.0f;

    float meanR = sumR / (float) juce::jmax (1, nLags);
    float conf = juce::jlimit (0.0f, 1.0f, (bestR - meanR) * 2.0f);
    // Penalise weak separation from the runner-up
    if (secondR > 0.0f)
        conf *= juce::jlimit (0.3f, 1.0f, bestR / (secondR + 1e-6f) * 0.7f);

    // ---- stability gating ----
    float published = bpm.load();
    bool close = (published > 0.0f) && (std::abs (candidate - published) / published < 0.02f);

    if (close || (published <= 0.0f && std::abs (candidate - candidateBpm) / juce::jmax (1.0f, candidateBpm) < 0.03f))
        stableTimeSec += 0.5;
    else
    {
        candidateBpm = candidate;
        stableTimeSec = 0.0;
    }

    // Hold time scales with the smoothing setting: 1 s (fast) .. 4 s (stable)
    double holdSec = 1.0 + (smoothing.load() / 100.0) * 3.0;

    bool frozen = ! autoMode.load() && oneShotDone;
    if (! frozen && conf > 0.30f && stableTimeSec >= holdSec)
    {
        bpm.store (candidate);
        bpmConf.store (conf);
    }
    else if (! frozen && conf > 0.30f)
    {
        // Keep the confidence live even before the value locks
        bpmConf.store (juce::jmax (bpmConf.load() * 0.9f, conf * 0.5f));
    }

    if (! autoMode.load() && elapsedSec > 10.0)
        oneShotDone = true;
}

void QFinderAnalyzer::evaluateKey()
{
    // Pearson correlation of the chromagram against each rotated profile
    float cMean = 0.0f;
    for (auto v : chroma) cMean += v;
    cMean /= 12.0f;

    float cVar = 0.0f;
    for (auto v : chroma) { float d = v - cMean; cVar += d * d; }
    if (cVar < 1e-9f)
        return;

    int bestIdx = -1;
    float bestR = -2.0f, secondR = -2.0f;

    for (int mode = 0; mode < 2; ++mode)          // 0 = major, 1 = minor
    {
        const float* profile = (mode == 0) ? majorProfile : minorProfile;
        for (int root = 0; root < 12; ++root)
        {
            float pMean = 0.0f;
            for (int i = 0; i < 12; ++i) pMean += profile[i];
            pMean /= 12.0f;

            float num = 0.0f, pVar = 0.0f;
            for (int i = 0; i < 12; ++i)
            {
                float p = profile[(i + root) % 12] - pMean;
                num  += (chroma[(size_t) i] - cMean) * p;
                pVar += p * p;
            }
            float r = num / (std::sqrt (cVar * pVar) + 1e-9f);

            if (r > bestR) { secondR = bestR; bestR = r; bestIdx = mode * 12 + root; }
            else if (r > secondR) { secondR = r; }
        }
    }

    if (bestIdx < 0)
        return;

    float conf = juce::jlimit (0.0f, 1.0f, (bestR - 0.55f) / 0.35f);
    conf *= juce::jlimit (0.4f, 1.0f, (bestR - secondR) * 4.0f);

    // Require the same winner twice in a row before publishing
    if (bestIdx == keyCandidate)
        ++keyStableEvals;
    else
    {
        keyCandidate = bestIdx;
        keyStableEvals = 1;
    }

    bool frozen = ! autoMode.load() && oneShotDone;
    if (! frozen && keyStableEvals >= 2 && conf > 0.25f)
    {
        keyIndex.store (bestIdx);
        keyConf.store (conf);
    }
}

juce::String QFinderAnalyzer::keyNameForIndex (int keyIndex)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (keyIndex < 0 || keyIndex > 23)
        return "--";
    int root = keyIndex % 12;
    bool isMinor = keyIndex >= 12;
    return juce::String (names[root]) + (isMinor ? " minor" : " major");
}

juce::String QFinderAnalyzer::camelotForIndex (int keyIndex)
{
    // Circle-of-fifths order starting at C for major (8B..7B), A for minor (8A..7A)
    static const int majorOrder[12] = { 0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5 };
    static const int minorOrder[12] = { 9, 4, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2 };
    if (keyIndex < 0 || keyIndex > 23)
        return "--";
    int root = keyIndex % 12;
    bool isMinor = keyIndex >= 12;
    const int* order = isMinor ? minorOrder : majorOrder;
    int pos = 0;
    for (int i = 0; i < 12; ++i)
        if (order[i] == root) { pos = i; break; }
    int number = 8 + pos;
    if (number > 12) number -= 12;
    return juce::String (number) + (isMinor ? "A" : "B");
}

//==============================================================================
QFinderProcessor::QFinderProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    syncAnalyzerSettings();
}

QFinderProcessor::~QFinderProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout QFinderProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        kSmoothing, "Smoothing",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        juce::String(), juce::AudioProcessorParameter::genericParameter,
        [] (float v, int) { return juce::String ((int) v) + "%"; },
        [] (const juce::String& t) { return t.getFloatValue(); }));
    params.push_back (std::make_unique<juce::AudioParameterBool> (kAutoMode, "Auto Analyze", true));
    return { params.begin(), params.end() };
}

void QFinderProcessor::syncAnalyzerSettings()
{
    if (auto* p = apvts.getRawParameterValue (kSmoothing))
    {
        float s = p->load();
        if (s != lastSmoothing) { lastSmoothing = s; analyzer.smoothing.store (s); }
    }
    if (auto* p = apvts.getRawParameterValue (kAutoMode))
    {
        bool a = p->load() > 0.5f;
        if (a != lastAutoMode) { lastAutoMode = a; analyzer.autoMode.store (a); }
    }
}

void QFinderProcessor::prepareToPlay (double sampleRate, int)
{
    analyzer.prepare (sampleRate);
    syncAnalyzerSettings();
}

void QFinderProcessor::releaseResources() {}

bool QFinderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void QFinderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    syncAnalyzerSettings();

    // Feed the analyzer a mono sum (audio passes through untouched)
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    // Small stack scratch to avoid per-block allocation churn for typical sizes
    juce::HeapBlock<float> scratch ((size_t) numSamples);
    if (numCh >= 2)
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        for (int i = 0; i < numSamples; ++i)
            scratch[i] = (l[i] + r[i]) * 0.5f;
    }
    else if (numCh == 1)
    {
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + numSamples, scratch.get());
    }
    else
    {
        std::fill (scratch.get(), scratch.get() + numSamples, 0.0f);
    }
    analyzer.pushSamples (scratch.get(), numSamples);
}

juce::AudioProcessorEditor* QFinderProcessor::createEditor() { return new QFinderEditor (*this); }
bool QFinderProcessor::hasEditor() const { return true; }

const juce::String QFinderProcessor::getName() const { return "QFinder"; }
bool QFinderProcessor::acceptsMidi() const { return false; }
bool QFinderProcessor::producesMidi() const { return false; }
bool QFinderProcessor::isMidiEffect() const { return false; }
double QFinderProcessor::getTailLengthSeconds() const { return 0.0; }

int QFinderProcessor::getNumPrograms() { return 1; }
int QFinderProcessor::getCurrentProgram() { return 0; }
void QFinderProcessor::setCurrentProgram (int) {}
const juce::String QFinderProcessor::getProgramName (int) { return {}; }
void QFinderProcessor::changeProgramName (int, const juce::String&) {}

void QFinderProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void QFinderProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new QFinderProcessor(); }
