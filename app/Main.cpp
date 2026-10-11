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
        // Mender --snapshot in.wav out-prefix startSec endSec lowHz highHz
        // Loads, repairs and saves without any interaction, writing pictures of
        // the window before and after. Used for automated checks only.
        if (args.size() == 7 && args[0] == "--snapshot")
        {
            auto& main = window->getMain();
            const auto prefix = args[2].unquoted();
            bool ok = main.loadFile (juce::File (args[1].unquoted()));

            if (ok)
            {
                const double rate = main.getView().getSampleRate();
                main.getView().setSelection ({ (juce::int64) (args[3].getDoubleValue() * rate),
                                               (juce::int64) (args[4].getDoubleValue() * rate),
                                               args[5].getDoubleValue(), args[6].getDoubleValue() });
                main.getView().zoomToSelection();
                savePicture (main, prefix + "_before.png");
                ok = main.repairSelection();
                savePicture (main, prefix + "_after.png");
                ok = main.saveTo (juce::File (prefix + "_repaired.wav")) && ok;
            }

            setApplicationReturnValue (ok ? 0 : 1);
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
   #endif

    std::unique_ptr<MainWindow> window;
    juce::ScopedMessageBox quitPrompt;
};

START_JUCE_APPLICATION (MenderApplication)
