#pragma once

#include "PluginProcessor.h"

//==============================================================================
// QFinderEditor — clean analyzer UI: big BPM + key readouts, confidence bars,
// flux meter, analyze/reset + auto controls, logo mark, preset menu with hearts.
//==============================================================================
struct QFinderPreset
{
    juce::String name;
    juce::ValueTree state;   // APVTS state snapshot
    bool isFactory = false;
    bool isUser = false;
};

class QFinderEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit QFinderEditor (QFinderProcessor&);
    ~QFinderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // ---- presets ----
    void buildPresetList();
    void refreshPresetCombo();
    void applyPreset (const QFinderPreset& p);
    void cyclePreset (int direction);
    void promptSavePreset();
    void saveUserPreset (const juce::String& name);
    QFinderPreset presetFromFile (const juce::File& f);
    static juce::File getPresetDir();
    static juce::File getDataDir();
    juce::File getFavoritesFile();
    void loadFavorites();
    void saveFavorites();
    void updateHeartButton();
    juce::String currentPresetName;

    QFinderProcessor& processor;

    juce::Image logoImage;

    // Cached display values (updated by the timer)
    float shownBpm = 0.0f, shownBpmConf = 0.0f;
    int shownKey = -1;
    float shownKeyConf = 0.0f;
    float fluxHistory[120] = {};

    juce::Label titleLabel { {}, "QFINDER" };
    juce::Label subtitleLabel { {}, "key + bpm analyzer" };

    juce::Label bpmValueLabel;
    juce::Label bpmUnitLabel { {}, "BPM" };
    juce::Label bpmConfLabel;
    double bpmConfProgress = 0.0;
    juce::ProgressBar bpmConfBar { bpmConfProgress };

    juce::Label keyValueLabel;
    juce::Label keyCamelotLabel;
    juce::Label keyConfLabel;
    double keyConfProgress = 0.0;
    juce::ProgressBar keyConfBar { keyConfProgress };

    juce::Label statusLabel;

    juce::TextButton analyzeButton { "ANALYZE" };
    juce::TextButton autoButton { "AUTO" };

    juce::Slider smoothingSlider;
    juce::Label smoothingLabel { {}, "Stability" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> smoothingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoAttachment;

    // Preset menu
    juce::ComboBox presetBox;
    juce::TextButton prevPresetButton { "<" };
    juce::TextButton nextPresetButton { ">" };
    juce::TextButton savePresetButton { "SAVE" };
    juce::TextButton heartButton;

    juce::Array<QFinderPreset> presets;
    juce::Array<int> presetItemToIndex; // combo item id -> index into presets
    juce::StringArray favoriteNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QFinderEditor)
};
