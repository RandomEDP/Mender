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
            script = std::make_unique<ScriptRunner> (window->getMain(), juce::File (args[1].unquoted()));
            return;
        }
       #endif

        // A file passed on the command line (or dropped on the app icon) opens straight away.
        if (args.size() > 0 && juce::File::isAbsolutePath (args[0].unquoted()))
            window->getMain().loadFile (juce::File (args[0].unquoted()));
    }

    void shutdown() override
    {
       #if MENDER_DEV_TOOLS
        script = nullptr;
       #endif
        window = nullptr;
    }

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
    // Steps through a script on the message thread, so the app keeps running
    // between commands (needed for "waitdetail").
    class ScriptRunner : private juce::Timer
    {
    public:
        ScriptRunner (MainComponent& m, const juce::File& file)
            : main (m), lines (juce::StringArray::fromLines (file.loadFileAsString()))
        {
            startTimer (1);
        }

    private:
        void timerCallback() override
        {
            if (waitingSince > 0)
            {
                if (! main.getView().isDetailCurrent())
                {
                    if (juce::Time::getMillisecondCounter() - waitingSince < 30000)
                        return;
                    return finish (false, "waitdetail timed out");
                }
                waitingSince = 0;
            }

            while (next < lines.size())
            {
                const auto line = lines[next++].trim();
                if (line.isEmpty() || line.startsWith ("#"))
                    continue;

                if (line == "waitdetail")
                {
                    waitingSince = juce::Time::getMillisecondCounter();
                    return;
                }

                if (! run (line))
                    return finish (false, line);
                return; // one command per tick, so repaints and timers can run in between
            }

            finish (true, {});
        }

        void finish (bool ok, const juce::String& failed)
        {
            stopTimer();
            if (! ok)
                std::fprintf (stderr, "script failed at: %s\n", failed.toRawUTF8());
            juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue (ok ? 0 : 1);
            juce::JUCEApplicationBase::quit();
        }

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

        // Presses at `from`, drags to `to` and releases, in the view's pixel coordinates.
        static void simulateDrag (juce::Component& c, juce::Point<float> a, juce::Point<float> b, bool shift = false)
        {
            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto now = juce::Time::getCurrentTime();
            const auto mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | (shift ? juce::ModifierKeys::shiftModifier : 0));
            const auto event = [&] (juce::Point<float> p, bool dragged)
            {
                return juce::MouseEvent (source, p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, a, now, 1, dragged);
            };

            const bool moves = a.getDistanceFrom (b) > 0.5f;
            c.mouseDown (event (a, false));
            for (int i = 1; moves && i <= 10; ++i)
                c.mouseDrag (event (a + (b - a) * ((float) i / 10.0f), true));
            c.mouseUp (event (b, moves));
        }

        static SpectrogramView::Handle handleNamed (const juce::String& name)
        {
            using H = SpectrogramView::Handle;
            const std::pair<const char*, H> names[] = {
                { "move", H::move }, { "left", H::left }, { "right", H::right }, { "top", H::top }, { "bottom", H::bottom },
                { "topleft", H::topLeft }, { "topright", H::topRight }, { "bottomleft", H::bottomLeft },
                { "bottomright", H::bottomRight }, { "before", H::reachBefore }, { "after", H::reachAfter },
                { "below", H::reachBelow }, { "above", H::reachAbove }, { "cursor", H::cursor } };
            for (const auto& [n, h] : names)
                if (name == n)
                    return h;
            return H::none;
        }

        bool run (const juce::String& line)
        {
            auto& view = main.getView();
            const auto t = juce::StringArray::fromTokens (line, true);
            const auto& c = t[0];
            const double rate = view.getSampleRate();
            const auto f = [&t] (int i) { return t[i].getFloatValue(); };
            const auto d = [&t] (int i) { return t[i].getDoubleValue(); };

            if (c == "load")              return main.loadFile (juce::File (t[1].unquoted()));
            if (c == "save")              return main.saveTo (juce::File (t[1].unquoted()));
            if (c == "channel")           { main.setChannel (t[1].getIntValue()); return true; }
            if (c == "editall")           { main.setEditAllChannels (t[1].getIntValue() != 0); return true; }
            if (c == "fullband")          { main.setFullBand (t[1].getIntValue() != 0); return true; }
            if (c == "reference")         { main.setReference (t[1] == "freq" ? mender::Reference::frequency : mender::Reference::time); return true; }
            if (c == "fft")               { main.setFftSize (t[1].getIntValue()); return true; }
            if (c == "display")           { main.setDisplay (d (1), d (2)); return true; }
            if (c == "select")            { view.setSelection ({ (juce::int64) (d (1) * rate), (juce::int64) (d (2) * rate), d (3), d (4) }); return true; }
            if (c == "reach")             { view.setReach ({ d (1), d (2), d (3), d (4) }); return true; }
            if (c == "cursor")            { view.setCursor ((juce::int64) (d (1) * rate)); return true; }
            if (c == "repair")            return main.repairSelection();
            if (c == "copy")              return main.copySelection();
            if (c == "paste")             return main.pasteAtCursor();
            if (c == "undo")              { main.undo(); return true; }
            if (c == "redo")              { main.redo(); return true; }
            if (c == "zoomfit")           { view.zoomToFit(); return true; }
            if (c == "zoombox")           { view.zoomToSelection(); return true; }
            if (c == "zoomtime")          { view.zoomTime (d (1)); return true; }
            if (c == "zoomfreq")          { view.zoomFrequency (d (1)); return true; }
            if (c == "snapshot")          { savePicture (main, t[1].unquoted()); return true; }
            if (c == "audition")          return writeWav (main.makeAudition (t[1] == "band"), rate, juce::File (t[2].unquoted()));

            // dragpx x1 y1 x2 y2 [shift]: pixels in the view
            if (c == "dragpx")            { simulateDrag (view, { f (1), f (2) }, { f (3), f (4) }, t[5] == "shift"); return true; }
            // grab handle dx dy [shift]: drag a handle of the box by some pixels
            if (c == "grab")
            {
                const auto h = handleNamed (t[1]);
                if (h == SpectrogramView::Handle::none)
                    return false;
                const auto from = view.getHandlePosition (h);
                simulateDrag (view, from, from + juce::Point<float> (f (2), f (3)), t[4] == "shift");
                return true;
            }
            if (c == "print")
            {
                const auto s = view.getSelection();
                const auto r = view.getEffectiveReach();
                std::printf ("%s cursor=%.4f box=%d t=%.4f..%.4f f=%.1f..%.1f reach=%.3f,%.3f,%.2f,%.2f\n",
                             t[1].toRawUTF8(), (double) view.getCursor() / rate, view.hasSelection() ? 1 : 0,
                             (double) s.startSample / rate, (double) s.endSample / rate, s.lowHz, s.highHz,
                             r.beforeSeconds, r.afterSeconds, r.belowOctaves, r.aboveOctaves);
                std::fflush (stdout);
                return true;
            }
            return false;
        }

        MainComponent& main;
        juce::StringArray lines;
        int next = 0;
        juce::uint32 waitingSince = 0;
    };

    std::unique_ptr<ScriptRunner> script;
   #endif

    std::unique_ptr<MainWindow> window;
    juce::ScopedMessageBox quitPrompt;
};

START_JUCE_APPLICATION (MenderApplication)
