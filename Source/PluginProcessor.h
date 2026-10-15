#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <array>

//==============================================================================
// QFinderAnalyzer — background-thread audio analysis engine.
//
// The audio thread pushes mono-summed samples into a lock-free FIFO; this
// thread pulls hop-sized blocks, runs an STFT (2048 FFT, 512 hop, Hann) and
// derives two things:
//
//  BPM: spectral-flux onset envelope -> autocorrelation of the envelope ->
//       peak-pick in the 60-200 BPM lag range. The published BPM only moves
//       when the new estimate is stable for a smoothing-dependent hold time,
//       so the readout doesn't jump around.
//
//  KEY: 12-bin chromagram (FFT bins mapped to pitch classes, ~4 s decay) ->
//       Pearson correlation against Krumhansl-Schmuckler major/minor profiles
//       in all 12 rotations -> winning key + confidence. Re-evaluated ~1/s.
//
// Audio passes through the plugin untouched; this class never touches the
// audio buffer, only the FIFO.
//==============================================================================
class QFinderAnalyzer : public juce::Thread
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder; // 2048
    static constexpr int hopSize  = 512;

    QFinderAnalyzer();
    ~QFinderAnalyzer() override;

    void run() override;

    // Called from the audio thread: push mono samples, never blocks.
    void pushSamples (const float* data, int numSamples);

    void prepare (double sampleRate);
    void requestReset();

    // ---- thread-safe published results ----
    float getBpm() const            { return bpm.load(); }          // 0 = unknown
    float getBpmConfidence() const  { return bpmConf.load(); }      // 0..1
    int   getKeyIndex() const       { return keyIndex.load(); }     // -1 = unknown, 0-11 major C..B, 12-23 minor C..B
    float getKeyConfidence() const  { return keyConf.load(); }      // 0..1
    float getFluxLevel() const      { return fluxLevel.load(); }    // 0..1 smoothed, for the UI meter
    double getAnalysisTimeSec() const { return analysisTimeSec.load(); }

    std::atomic<float> smoothing { 50.0f };  // 0..100, higher = steadier readouts
    std::atomic<bool>  autoMode  { true };   // false = analyze ~10 s after reset, then hold

    // ---- key name helpers (pitch class 0 = C) ----
    static juce::String keyNameForIndex (int keyIndex);   // e.g. "F# minor", "C major"
    static juce::String camelotForIndex (int keyIndex);    // e.g. "11A", "8B"

private:
    void processHop();
    void evaluateBpm();
    void evaluateKey();
    void clearState();

    // Krumhansl-Schmuckler profiles, C-rooted
    static constexpr float majorProfile[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
                                                2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr float minorProfile[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
                                                2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };

    static constexpr int fifoSize = 32768;
    juce::AbstractFifo fifo { fifoSize };
    std::array<float, fifoSize> fifoBuffer {};

    std::array<float, fftSize> windowBuf {};
    std::array<float, fftSize * 2> fftBuf {};
    std::array<float, fftSize / 2> prevMag {};

    static constexpr int fluxHistoryLen = 1024;
    std::array<float, fluxHistoryLen> fluxHistory {};
    int fluxWritePos = 0;
    int fluxCount = 0;

    std::array<float, 12> chroma {};

    double sampleRate = 44100.0;
    std::atomic<bool> resetRequested { false };

    // BPM stability tracking
    float candidateBpm = 0.0f;
    double stableTimeSec = 0.0;
    int framesSinceBpmEval = 0;

    // Key stability tracking
    int keyCandidate = -1;
    int keyStableEvals = 0;
    int framesSinceKeyEval = 0;
    double elapsedSec = 0.0;
    bool oneShotDone = false;

    std::atomic<float> bpm { 0.0f };
    std::atomic<float> bpmConf { 0.0f };
    std::atomic<int>   keyIndex { -1 };
    std::atomic<float> keyConf { 0.0f };
    std::atomic<float> fluxLevel { 0.0f };
    std::atomic<double> analysisTimeSec { 0.0 };
};

//==============================================================================
/*
    QFinder — real-time key + BPM analyzer for Headroom Audio.

    Insert on any track: audio passes through untouched while the background
    analyzer reports the track's tempo and musical key.
*/
class QFinderProcessor : public juce::AudioProcessor
{
public:
    QFinderProcessor();
    ~QFinderProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- analysis access (thread-safe) ----
    float getBpm() const            { return analyzer.getBpm(); }
    float getBpmConfidence() const  { return analyzer.getBpmConfidence(); }
    int   getKeyIndex() const       { return analyzer.getKeyIndex(); }
    float getKeyConfidence() const  { return analyzer.getKeyConfidence(); }
    float getFluxLevel() const      { return analyzer.getFluxLevel(); }
    double getAnalysisTimeSec() const { return analyzer.getAnalysisTimeSec(); }
    void resetAnalysis()            { analyzer.requestReset(); }

    // ---- parameters ----
    juce::AudioProcessorValueTreeState apvts;

    static constexpr const char* kSmoothing = "smoothing"; // 0..100
    static constexpr const char* kAutoMode  = "autoMode";  // bool

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void syncAnalyzerSettings();

    QFinderAnalyzer analyzer;

    float lastSmoothing = -1.0f;
    bool  lastAutoMode  = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QFinderProcessor)
};
