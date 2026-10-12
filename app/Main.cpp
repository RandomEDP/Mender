#include "MainComponent.h"

class MenderApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<MainWindow> (getApplicationName());
        const auto args = juce::StringArray::fromTokens (commandLine, true);

       #if MENDER_DEV_TOOLS
        // Mender --script commands.txt
        // Runs editor commands without interaction, one per line, for automated
        // checks only. Exit code 0 if every command succeeded.
        if (args.size() == 2 && args[0] == "--script")
        {
            setApplicationReturnValue (runScript (window->getMain(), juce::File (args[1].unquoted())) ? 0 : 1);
            quit();
            return;
        }
       #endif

        // A file passed on the command line (or dropped on the app icon) opens straight away.
        if (args.size() > 0 && juce::File::isAbsolutePath (args[0].unquoted()))
            window->getMain().loadFile (juce::File (args[0].unquoted()));
    }

    void shutdown() override { window = nullptr; }

    void systemRequestedQuit() override
    {
        if (window == nullptr || ! window->getMain().hasUnsavedChanges())
        {
            quit();
            return;
        }

        const auto options = juce::MessageBoxOptions()
                                 .withIconType (juce::MessageBoxIconType::WarningIcon)
                                 .withTitle ("Unsaved repairs")
                                 .withMessage ("The repaired clip has not been saved. Quit anyway?")
                                 .withButton ("Quit without saving")
                                 .withButton ("Cancel")
                                 .withAssociatedComponent (window.get());

        quitPrompt = juce::AlertWindow::showScopedAsync (options, [] (int button)
        {
            if (button == 1) // first button
                quit();
        });
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff1e2026), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (820, 420, 8000, 8000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        MainComponent& getMain() { return *static_cast<MainComponent*> (getContentComponent()); }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

   #if MENDER_DEV_TOOLS
    static void savePicture (juce::Component& component, const juce::String& path)
    {
        const auto picture = component.createComponentSnapshot (component.getLocalBounds());
        juce::File file (path);
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (picture, *stream);
    }

    static bool writeWav (const juce::AudioBuffer<float>& buffer, double rate, const juce::File& file)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                       .withNumChannels (buffer.getNumChannels()).withBitsPerSample (32)
                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Presses at `from`, drags to `to` and releases, in view coordinates given as
    // fractions of its width and height.
    static void simulateDrag (juce::Component& c, juce::Point<float> from, juce::Point<float> to)
    {
        const auto scale = [&c] (juce::Point<float> p) { return juce::Point<float> (p.x * (float) c.getWidth(), p.y * (float) c.getHeight()); };
        const auto a = scale (from), b = scale (to);
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        const auto event = [&] (juce::Point<float> p, bool dragged)
        {
            return juce::MouseEvent (source, p, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &c, &c, now, a, now, 1, dragged);
        };

        const bool moves = a.getDistanceFrom (b) > 0.5f;
        c.mouseDown (event (a, false));
        for (int i = 1; moves && i <= 10; ++i)
            c.mouseDrag (event (a + (b - a) * ((float) i / 10.0f), true));
        c.mouseUp (event (b, moves));
    }

    static bool runScript (MainComponent& main, const juce::File& script)
    {
        auto& view = main.getView();
        for (const auto& rawLine : juce::StringArray::fromLines (script.loadFileAsString()))
        {
            const auto t = juce::StringArray::fromTokens (rawLine.trim(), true);
            if (t.isEmpty() || t[0].startsWith ("#"))
                continue;

            const auto& c = t[0];
            const double rate = view.getSampleRate();
            bool ok = true;

            if (c == "load")                 ok = main.loadFile (juce::File (t[1].unquoted()));
            else if (c == "save")            ok = main.saveTo (juce::File (t[1].unquoted()));
            else if (c == "channel")         main.setChannel (t[1].getIntValue());
            else if (c == "fft")             main.setFftSize (t[1].getIntValue());
            else if (c == "display")         main.setDisplay (t[1].getDoubleValue(), t[2].getDoubleValue());
            else if (c == "select")          view.setSelection ({ (juce::int64) (t[1].getDoubleValue() * rate), (juce::int64) (t[2].getDoubleValue() * rate),
                                                                  t[3].getDoubleValue(), t[4].getDoubleValue() });
            else if (c == "cursor")          view.setCursor ((juce::int64) (t[1].getDoubleValue() * rate));
            else if (c == "repair")          ok = main.repairSelection();
            else if (c == "copy")            ok = main.copySelection();
            else if (c == "paste")           ok = main.pasteAtCursor();
            else if (c == "undo")            main.undo();
            else if (c == "redo")            main.redo();
            else if (c == "zoomfit")         view.zoomToFit();
            else if (c == "zoombox")         view.zoomToSelection();
            else if (c == "zoomtime")        view.zoomTime (t[1].getDoubleValue());
            else if (c == "zoomfreq")        view.zoomFrequency (t[1].getDoubleValue());
            else if (c == "snapshot")        savePicture (main, t[1].unquoted());
            else if (c == "drag")            simulateDrag (view, { t[1].getFloatValue(), t[2].getFloatValue() }, { t[3].getFloatValue(), t[4].getFloatValue() });
            else if (c == "click")           simulateDrag (view, { t[1].getFloatValue(), t[2].getFloatValue() }, { t[1].getFloatValue(), t[2].getFloatValue() });
            else if (c == "print")
            {
                const auto sel = view.getSelection();
                std::printf ("cursor=%.4f box=%d %.4f %.4f %.1f %.1f\n", (double) view.getCursor() / rate, view.hasSelection() ? 1 : 0,
                             (double) sel.startSample / rate, (double) sel.endSample / rate, sel.lowHz, sel.highHz);
            }
            else if (c == "audition")        ok = writeWav (main.makeAudition (t[1] == "band"), rate, juce::File (t[2].unquoted()));
            else                             ok = false;

            if (! ok)
            {
                std::fprintf (stderr, "script failed at: %s\n", rawLine.toRawUTF8());
                return false;
            }
        }
        return true;
    }
   #endif

    std::unique_ptr<MainWindow> window;
    juce::ScopedMessageBox quitPrompt;
};

START_JUCE_APPLICATION (MenderApplication)
