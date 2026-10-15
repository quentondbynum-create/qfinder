#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    const juce::Colour kBg      = juce::Colour (0xff0d0d10);
    const juce::Colour kPanel   = juce::Colour (0xff15151a);
    const juce::Colour kPanel2  = juce::Colour (0xff1b1b22);
    const juce::Colour kGrid    = juce::Colour (0xff26262e);
    const juce::Colour kText    = juce::Colour (0xfff2f2f5);
    const juce::Colour kTextDim = juce::Colour (0xff8a8a94);
    const juce::Colour kCyan    = juce::Colour (0xff4cc9f0);
    const juce::Colour kAmber   = juce::Colour (0xffffb703);
    const juce::Colour kGreen   = juce::Colour (0xff3ddc84);
    const juce::Colour kHeartOn = juce::Colour (0xffff3b5c);

    QFinderPreset makeFactoryPreset (const juce::String& name, float smoothing, bool autoMode)
    {
        QFinderPreset p;
        p.name = name;
        p.isFactory = true;
        juce::ValueTree v ("PARAMETERS");
        v.setProperty (QFinderProcessor::kSmoothing, smoothing, nullptr);
        v.setProperty (QFinderProcessor::kAutoMode, autoMode ? 1.0f : 0.0f, nullptr);
        p.state = v;
        return p;
    }
}

//==============================================================================
QFinderEditor::QFinderEditor (QFinderProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setSize (800, 600);

    logoImage = juce::ImageCache::getFromMemory (BinaryData::headroomlogo_png,
                                                 BinaryData::headroomlogo_pngSize);

    titleLabel.setFont (juce::Font (20.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setFont (juce::Font (12.0f));
    subtitleLabel.setColour (juce::Label::textColourId, kTextDim);
    addAndMakeVisible (subtitleLabel);

    // ---- preset menu ----
    presetBox.setColour (juce::ComboBox::backgroundColourId, kPanel2);
    presetBox.setColour (juce::ComboBox::textColourId, kText);
    presetBox.setColour (juce::ComboBox::outlineColourId, kGrid);
    presetBox.onChange = [this]
    {
        auto id = presetBox.getSelectedId();
        if (id > 0 && id <= presetItemToIndex.size())
        {
            int pidx = presetItemToIndex[id - 1];
            if (pidx >= 0 && pidx < presets.size())
                applyPreset (presets[pidx]);
        }
    };
    addAndMakeVisible (presetBox);

    prevPresetButton.onClick = [this] { cyclePreset (-1); };
    nextPresetButton.onClick = [this] { cyclePreset (1); };
    addAndMakeVisible (prevPresetButton);
    addAndMakeVisible (nextPresetButton);

    savePresetButton.onClick = [this] { promptSavePreset(); };
    addAndMakeVisible (savePresetButton);

    heartButton.setButtonText (juce::String::fromUTF8 ("♥"));
    heartButton.setClickingTogglesState (true);
    heartButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    heartButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    heartButton.setColour (juce::TextButton::textColourOffId, kTextDim);
    heartButton.setColour (juce::TextButton::textColourOnId, kHeartOn);
    heartButton.onClick = [this]
    {
        if (currentPresetName.isEmpty())
        {
            heartButton.setToggleState (false, juce::dontSendNotification);
            return;
        }
        if (heartButton.getToggleState())
        {
            if (! favoriteNames.contains (currentPresetName))
                favoriteNames.add (currentPresetName);
        }
        else
            favoriteNames.removeString (currentPresetName);
        saveFavorites();
        refreshPresetCombo();
    };
    addAndMakeVisible (heartButton);

    // ---- BPM readout (hero) ----
    bpmValueLabel.setFont (juce::Font (112.0f, juce::Font::bold));
    bpmValueLabel.setColour (juce::Label::textColourId, kCyan);
    bpmValueLabel.setJustificationType (juce::Justification::centred);
    bpmValueLabel.setText ("--", juce::dontSendNotification);
    addAndMakeVisible (bpmValueLabel);

    bpmUnitLabel.setFont (juce::Font (18.0f, juce::Font::bold));
    bpmUnitLabel.setColour (juce::Label::textColourId, kTextDim);
    bpmUnitLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmUnitLabel);

    bpmConfLabel.setFont (juce::Font (12.0f));
    bpmConfLabel.setColour (juce::Label::textColourId, kTextDim);
    bpmConfLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmConfLabel);

    bpmConfBar.setColour (juce::ProgressBar::backgroundColourId, kPanel2);
    bpmConfBar.setColour (juce::ProgressBar::foregroundColourId, kCyan);
    addAndMakeVisible (bpmConfBar);

    // ---- Key readout (hero) ----
    keyValueLabel.setFont (juce::Font (88.0f, juce::Font::bold));
    keyValueLabel.setColour (juce::Label::textColourId, kAmber);
    keyValueLabel.setJustificationType (juce::Justification::centred);
    keyValueLabel.setText ("--", juce::dontSendNotification);
    addAndMakeVisible (keyValueLabel);

    keyCamelotLabel.setFont (juce::Font (20.0f, juce::Font::bold));
    keyCamelotLabel.setColour (juce::Label::textColourId, kBg);
    keyCamelotLabel.setColour (juce::Label::backgroundColourId, kAmber);
    keyCamelotLabel.setJustificationType (juce::Justification::centred);
    keyCamelotLabel.setText ("--", juce::dontSendNotification);
    addAndMakeVisible (keyCamelotLabel);

    keyConfLabel.setFont (juce::Font (12.0f));
    keyConfLabel.setColour (juce::Label::textColourId, kTextDim);
    keyConfLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (keyConfLabel);

    keyConfBar.setColour (juce::ProgressBar::backgroundColourId, kPanel2);
    keyConfBar.setColour (juce::ProgressBar::foregroundColourId, kAmber);
    addAndMakeVisible (keyConfBar);

    statusLabel.setFont (juce::Font (13.0f));
    statusLabel.setColour (juce::Label::textColourId, kTextDim);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText ("drop QFinder on a track — listening…", juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    // ---- controls ----
    analyzeButton.setColour (juce::TextButton::buttonColourId, kCyan.darker (0.35f));
    analyzeButton.setColour (juce::TextButton::textColourOffId, kBg);
    analyzeButton.onClick = [this]
    {
        processor.resetAnalysis();
        shownBpm = 0.0f; shownKey = -1;
        statusLabel.setText ("analyzing…", juce::dontSendNotification);
    };
    addAndMakeVisible (analyzeButton);

    autoButton.setClickingTogglesState (true);
    autoButton.setColour (juce::TextButton::buttonColourId, kPanel2);
    autoButton.setColour (juce::TextButton::buttonOnColourId, kCyan.darker (0.4f));
    autoButton.setColour (juce::TextButton::textColourOffId, kTextDim);
    autoButton.setColour (juce::TextButton::textColourOnId, kText);
    addAndMakeVisible (autoButton);

    smoothingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    smoothingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 18);
    smoothingSlider.setColour (juce::Slider::backgroundColourId, kPanel2);
    smoothingSlider.setColour (juce::Slider::trackColourId, kCyan);
    smoothingSlider.setColour (juce::Slider::textBoxTextColourId, kText);
    addAndMakeVisible (smoothingSlider);
    smoothingLabel.setColour (juce::Label::textColourId, kTextDim);
    smoothingLabel.setFont (juce::Font (12.0f));
    addAndMakeVisible (smoothingLabel);

    smoothingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.apvts, QFinderProcessor::kSmoothing, smoothingSlider);
    autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, QFinderProcessor::kAutoMode, autoButton);

    loadFavorites();
    buildPresetList();
    refreshPresetCombo();

    startTimerHz (20);
}

QFinderEditor::~QFinderEditor()
{
    stopTimer();
}

void QFinderEditor::timerCallback()
{
    float bpm = processor.getBpm();
    float bpmC = processor.getBpmConfidence();
    int key = processor.getKeyIndex();
    float keyC = processor.getKeyConfidence();
    float flux = processor.getFluxLevel();
    double t = processor.getAnalysisTimeSec();

    // Scroll the flux history
    std::copy (std::begin (fluxHistory) + 1, std::end (fluxHistory), std::begin (fluxHistory));
    fluxHistory[119] = flux;

    bool changed = false;
    if (bpm != shownBpm || bpmC != shownBpmConf || key != shownKey || keyC != shownKeyConf)
        changed = true;

    shownBpm = bpm; shownBpmConf = bpmC; shownKey = key; shownKeyConf = keyC;
    bpmConfProgress = bpmC;
    keyConfProgress = keyC;

    if (bpm > 0.5f)
        bpmValueLabel.setText (juce::String (bpm, 1), juce::dontSendNotification);
    else
        bpmValueLabel.setText ("--", juce::dontSendNotification);

    bpmConfLabel.setText ("confidence " + juce::String ((int) std::round (bpmC * 100.0f)) + "%",
                          juce::dontSendNotification);

    if (key >= 0)
    {
        keyValueLabel.setText (QFinderAnalyzer::keyNameForIndex (key), juce::dontSendNotification);
        keyCamelotLabel.setText (QFinderAnalyzer::camelotForIndex (key), juce::dontSendNotification);
    }
    else
    {
        keyValueLabel.setText ("--", juce::dontSendNotification);
        keyCamelotLabel.setText ("--", juce::dontSendNotification);
    }
    keyConfLabel.setText ("confidence " + juce::String ((int) std::round (keyC * 100.0f)) + "%",
                          juce::dontSendNotification);

    if (bpm <= 0.5f && key < 0)
    {
        statusLabel.setText (t < 1.0 ? "drop QFinder on a track — listening…"
                                     : "analyzing… feed it some audio",
                             juce::dontSendNotification);
    }
    else
    {
        bool autoOn = processor.apvts.getRawParameterValue (QFinderProcessor::kAutoMode)->load() > 0.5f;
        statusLabel.setText (autoOn ? "locked — tracking live"
                                    : "locked — holding (AUTO off)",
                             juce::dontSendNotification);
    }

    if (changed)
        repaint (getLocalBounds().withTop (300).withBottom (360)); // flux strip only
    repaint (bpmConfBar.getBounds());
    repaint (keyConfBar.getBounds());
}

void QFinderEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // Top bar
    g.setColour (kPanel);
    g.fillRect (0, 0, getWidth(), 52);

    if (logoImage.isValid())
        g.drawImage (logoImage, 10, 10, 32, 32, 0, 0,
                     logoImage.getWidth(), logoImage.getHeight());

    // Divider panels for the two readouts — the heroes of this UI
    auto bpmArea = juce::Rectangle<int> (24, 80, 368, 300);
    auto keyArea = juce::Rectangle<int> (408, 80, 368, 300);
    g.setColour (kPanel);
    g.fillRoundedRectangle (bpmArea.toFloat(), 10.0f);
    g.fillRoundedRectangle (keyArea.toFloat(), 10.0f);
    g.setColour (kGrid);
    g.drawRoundedRectangle (bpmArea.toFloat(), 10.0f, 1.0f);
    g.drawRoundedRectangle (keyArea.toFloat(), 10.0f, 1.0f);

    g.setColour (kTextDim);
    g.setFont (juce::Font (14.0f, juce::Font::bold));
    g.drawText ("TEMPO", bpmArea.withHeight (26).translated (0, 10),
                juce::Justification::centredTop, false);
    g.drawText ("KEY", keyArea.withHeight (26).translated (0, 10),
                juce::Justification::centredTop, false);

    // Flux strip — slim, secondary
    auto fluxArea = juce::Rectangle<int> (24, 396, 752, 52);
    g.setColour (kPanel);
    g.fillRoundedRectangle (fluxArea.toFloat(), 8.0f);
    g.setColour (kTextDim);
    g.setFont (juce::Font (11.0f));
    g.drawText ("ONSET ACTIVITY", fluxArea.withHeight (16).translated (10, 4),
                juce::Justification::left, false);

    auto meter = fluxArea.reduced (10, 22);
    meter.removeFromTop (6);
    g.setColour (kPanel2);
    g.fillRoundedRectangle (meter.toFloat(), 4.0f);

    g.setColour (kGreen);
    const int n = 120;
    float w = (float) meter.getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        float v = juce::jlimit (0.0f, 1.0f, fluxHistory[i] * 3.0f);
        float h = v * (float) meter.getHeight();
        if (h > 0.5f)
            g.fillRect (meter.getX() + (int) (i * w), meter.getBottom() - (int) h,
                        juce::jmax (1, (int) w - 1), (int) h);
    }
}

void QFinderEditor::resized()
{
    titleLabel.setBounds (52, 8, 140, 24);
    subtitleLabel.setBounds (52, 30, 180, 16);

    // Preset menu, top-right
    int px = getWidth() - 24;
    heartButton.setBounds (px - 34, 10, 34, 32);       px -= 40;
    savePresetButton.setBounds (px - 64, 10, 64, 32);  px -= 70;
    nextPresetButton.setBounds (px - 34, 10, 34, 32);  px -= 38;
    prevPresetButton.setBounds (px - 34, 10, 34, 32);  px -= 38;
    presetBox.setBounds (px - 190, 10, 190, 32);

    // BPM panel contents — hero readout
    bpmValueLabel.setBounds (24, 128, 368, 140);
    bpmUnitLabel.setBounds (24, 262, 368, 26);
    bpmConfBar.setBounds (92, 296, 232, 8);
    bpmConfLabel.setBounds (24, 308, 368, 18);

    // Key panel contents — hero readout
    keyValueLabel.setBounds (408, 136, 368, 110);
    keyCamelotLabel.setBounds (558, 250, 68, 34);
    keyConfBar.setBounds (476, 296, 232, 8);
    keyConfLabel.setBounds (408, 308, 368, 18);

    statusLabel.setBounds (24, 456, 752, 20);

    // Bottom controls — minimal: ANALYZE hero button, AUTO, Stability
    analyzeButton.setBounds (24, 492, 130, 40);
    autoButton.setBounds (162, 492, 90, 40);
    smoothingLabel.setBounds (272, 492, 80, 40);
    smoothingSlider.setBounds (350, 492, 220, 40);
}

//==============================================================================
// Presets
//==============================================================================
juce::File QFinderEditor::getDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Headroom Audio/QFinder");
}

juce::File QFinderEditor::getPresetDir()
{
    auto d = getDataDir().getChildFile ("Presets");
    d.createDirectory();
    return d;
}

juce::File QFinderEditor::getFavoritesFile()
{
    return getDataDir().getChildFile ("favorites.txt");
}

void QFinderEditor::loadFavorites()
{
    favoriteNames.clear();
    const auto file = getFavoritesFile();
    if (file.existsAsFile())
        favoriteNames.addLines (file.loadFileAsString());
    favoriteNames.trim();
    favoriteNames.removeEmptyStrings();
}

void QFinderEditor::saveFavorites()
{
    getDataDir().createDirectory();
    getFavoritesFile().replaceWithText (favoriteNames.joinIntoString ("\n"));
}

void QFinderEditor::buildPresetList()
{
    presets.clear();
    presets.add (makeFactoryPreset ("Init", 50.0f, true));
    presets.add (makeFactoryPreset ("Fast Analysis", 5.0f, true));
    presets.add (makeFactoryPreset ("Stable Analysis", 95.0f, true));

    juce::Array<juce::File> files;
    getPresetDir().findChildFiles (files, juce::File::findFiles, false, "*.qfinderpreset");
    for (auto& f : files)
    {
        auto p = presetFromFile (f);
        if (p.name.isNotEmpty())
        {
            p.isUser = true;
            presets.add (p);
        }
    }
}

QFinderPreset QFinderEditor::presetFromFile (const juce::File& f)
{
    QFinderPreset p;
    std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (f));
    if (xml == nullptr)
        return p;
    p.name = f.getFileNameWithoutExtension();
    p.state = juce::ValueTree::fromXml (*xml);
    return p;
}

void QFinderEditor::refreshPresetCombo()
{
    presetBox.clear (juce::dontSendNotification);
    presetItemToIndex.clear();

    int itemId = 1;
    auto addItem = [this, &itemId] (const juce::String& label, int presetIdx)
    {
        presetBox.addItem (label, itemId++);
        presetItemToIndex.add (presetIdx);
    };

    // Favorites section
    bool hasFavs = false;
    for (int i = 0; i < presets.size(); ++i)
        if (favoriteNames.contains (presets[i].name)) { hasFavs = true; break; }
    if (hasFavs)
    {
        presetBox.addSectionHeading (juce::String::fromUTF8 ("♥ Favorites"));
        for (int i = 0; i < presets.size(); ++i)
            if (favoriteNames.contains (presets[i].name))
                addItem (juce::String::fromUTF8 ("♥ ") + presets[i].name, i);
    }

    // Factory
    presetBox.addSectionHeading ("Factory");
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].isFactory)
            addItem (presets[i].name, i);

    // User
    bool hasUser = false;
    for (int i = 0; i < presets.size(); ++i) if (presets[i].isUser) { hasUser = true; break; }
    if (hasUser)
    {
        presetBox.addSectionHeading ("User");
        for (int i = 0; i < presets.size(); ++i)
            if (presets[i].isUser)
                addItem (presets[i].name, i);
    }

    updateHeartButton();
}

void QFinderEditor::applyPreset (const QFinderPreset& p)
{
    // Values are stored as direct APVTS properties (denormalized)
    if (auto* param = processor.apvts.getParameter (QFinderProcessor::kSmoothing))
        param->setValueNotifyingHost (param->convertTo0to1 (
            (float) p.state.getProperty (QFinderProcessor::kSmoothing, 50.0f)));
    if (auto* param = processor.apvts.getParameter (QFinderProcessor::kAutoMode))
        param->setValueNotifyingHost (
            ((float) p.state.getProperty (QFinderProcessor::kAutoMode, 1.0f)) > 0.5f ? 1.0f : 0.0f);

    currentPresetName = p.name;

    // Reflect in UI controls
    smoothingSlider.setValue (processor.apvts.getRawParameterValue (QFinderProcessor::kSmoothing)->load(),
                              juce::dontSendNotification);
    autoButton.setToggleState (processor.apvts.getRawParameterValue (QFinderProcessor::kAutoMode)->load() > 0.5f,
                               juce::dontSendNotification);

    // Select the matching combo entry
    for (int i = 0; i < presetBox.getNumItems(); ++i)
    {
        // match by stripping the heart prefix
        auto t = presetBox.getItemText (i).removeCharacters (juce::String::fromUTF8 ("♥ "));
        if (t.trim() == p.name) { presetBox.setSelectedItemIndex (i, juce::dontSendNotification); break; }
    }
    updateHeartButton();
}

void QFinderEditor::cyclePreset (int direction)
{
    if (presets.isEmpty())
        return;
    int idx = -1;
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].name == currentPresetName) { idx = i; break; }
    idx = (idx + direction + presets.size()) % presets.size();
    applyPreset (presets[idx]);
}

void QFinderEditor::promptSavePreset()
{
    auto* alert = new juce::AlertWindow ("Save Preset", "Name your preset:",
                                         juce::AlertWindow::NoIcon);
    alert->addTextEditor ("name", currentPresetName.isNotEmpty() ? currentPresetName : "My Preset");
    alert->addButton ("Save", 1);
    alert->addButton ("Cancel", 0);
    alert->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, alert] (int result)
        {
            if (result == 1)
            {
                auto name = alert->getTextEditorContents ("name").trim();
                if (name.isNotEmpty())
                    saveUserPreset (name);
            }
            delete alert;
        }));
}

void QFinderEditor::saveUserPreset (const juce::String& name)
{
    juce::ValueTree v ("PARAMETERS");
    v.setProperty (QFinderProcessor::kSmoothing,
                   processor.apvts.getRawParameterValue (QFinderProcessor::kSmoothing)->load(), nullptr);
    v.setProperty (QFinderProcessor::kAutoMode,
                   processor.apvts.getRawParameterValue (QFinderProcessor::kAutoMode)->load(), nullptr);

    std::unique_ptr<juce::XmlElement> xml (v.createXml());
    if (xml != nullptr)
        xml->writeTo (getPresetDir().getChildFile (name + ".qfinderpreset"));

    currentPresetName = name;
    buildPresetList();
    refreshPresetCombo();
    for (auto& p : presets)
        if (p.name == name) { applyPreset (p); break; }
}

void QFinderEditor::updateHeartButton()
{
    bool fav = currentPresetName.isNotEmpty() && favoriteNames.contains (currentPresetName);
    heartButton.setToggleState (fav, juce::dontSendNotification);
    heartButton.setEnabled (currentPresetName.isNotEmpty());
}
