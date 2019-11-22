// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/ImageViewer.h>
#include <tev/Ipc.h>
#include <tev/ThreadPool.h>

#include <args.hxx>
#include <ImfThreading.h>

#include <utf8.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace args;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

int mainFunc(const vector<string>& arguments) {
    ArgumentParser parser{
        "tev — The EXR Viewer\n"
        "version " TEV_VERSION "\n"
        "Inspection tool for images with high dynamic range",
        "tev was developed by Thomas Müller <thomas94@gmx.net>. "
        "Its source code is available under the BSD 3-Clause License at https://tom94.net",
    };

    ValueFlag<float> exposureFlag{
        parser,
        "EXPOSURE",
        "Scales the brightness of an image prior to tonemapping by 2^EXPOSURE. Default is 0.",
        {'e', "exposure"},
    };

    ValueFlag<string> filterFlag{
        parser,
        "FILTER",
        "Filter visible images and layers according to a supplied string. "
        "The string must have the format 'image:layer'. "
        "Only images whose name contains 'image' and layers whose name contains 'layer' will be visible.",
        {'f', "filter"},
    };

    ValueFlag<float> gammaFlag{
        parser,
        "GAMMA",
        "The exponent used when TONEMAP is 'Gamma'. Default is 2.2.",
        {'g', "gamma"},
    };

    HelpFlag helpFlag{
        parser,
        "HELP",
        "Display this help menu.",
        {'h', "help"},
    };

    ValueFlag<bool> maximizeFlag{
        parser,
        "MAXIMIZE",
        "Maximize the window on startup. "
        "If no images were supplied via the command line, then the default is FALSE. "
        "Otherwise, the default is TRUE.",
        {"max", "maximize"},
    };

    ValueFlag<string> metricFlag{
        parser,
        "METRIC",
        "The metric to use when comparing two images. "
        "The available metrics are:\n"
        "E   - Error\n"
        "AE  - Absolute Error\n"
        "SE  - Squared Error\n"
        "RAE - Relative Absolute Error\n"
        "RSE - Relative Squared Error\n"
        "DIV - Division\n"
        "Default is E.",
        {'m', "metric"},
    };

    ValueFlag<string> postProcessingFlag{
        parser,
        "POSTPROC",
        "The post processing to use.",
        {'p', "post"},
    };

    Flag newWindowFlag{
        parser,
        "NEW WINDOW",
        "Open a new window of tev, even if one exists already.",
        {'n', "new"},
    };

    ValueFlag<float> offsetFlag{
        parser,
        "OFFSET",
        "Add an absolute offset to the image after EXPOSURE has been applied. Default is 0.",
        {'o', "offset"},
    };

    ValueFlag<string> tonemapFlag{
        parser,
        "TONEMAP",
        "The tonemapping algorithm to use. "
        "The available tonemaps are:\n"
        "sRGB   - sRGB\n"
        "Gamma  - Gamma curve\n"
        "FC     - False Color\n"
        "PN     - Positive=Green, Negative=Red\n"
        "Default is sRGB.",
        {'t', "tonemap"},
    };

    Flag versionFlag{
        parser,
        "VERSION",
        "Display the version of tev.",
        {'v', "version"},
    };

    PositionalList<string> imageFiles{
        parser,
        "images",
        "The image files to be opened by tev. "
        "If an argument starting with a ':' is encountered, "
        "then this argument is not treated as an image file "
        "but as a comma-separated channel selector. Until the next channel "
        "selector is encountered only channels containing "
        "elements from the current selector will be loaded. This is "
        "especially useful for selectively loading a specific "
        "part of a multi-part EXR file.",
    };

    // Parse command line arguments and react to parsing
    // errors using exceptions.
    try {
        TEV_ASSERT(arguments.size() > 0, "Number of arguments must be bigger than 0.");

        parser.Prog(arguments.front());
        parser.ParseArgs(begin(arguments) + 1, end(arguments));
    } catch (const Help&) {
        cout << parser;
        return 0;
    } catch (const ParseError& e) {
        cerr << e.what() << endl;
        cerr << parser;
        return -1;
    } catch (const ValidationError& e) {
        cerr << e.what() << endl;
        cerr << parser;
        return -2;
    }

    if (versionFlag) {
        tlog::none() << "tev — The EXR Viewer\nversion " TEV_VERSION;
        return 0;
    }

    auto ipc = make_shared<Ipc>();

    // If we're not the primary instance and did not request to open a new window,
    // simply send the to-be-opened images to the primary instance.
    if (!ipc->isPrimaryInstance() && !newWindowFlag) {
        string channelSelector;
        for (auto imageFile : get(imageFiles)) {
            if (!imageFile.empty() && imageFile[0] == ':') {
                channelSelector = imageFile.substr(1);
                continue;
            }

            try {
                ipc->sendToPrimaryInstance(tfm::format("%s:%s", path{imageFile}.make_absolute(), channelSelector));
            } catch (runtime_error e) {
                tlog::error() << tfm::format("Invalid file '%s': %s", imageFile, e.what());
            }
        }

        return 0;
    }

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    tlog::info() << "Loading window...";

    shared_ptr<BackgroundImagesLoader> imagesLoader = make_shared<BackgroundImagesLoader>();

    atomic<bool> shallShutdown{false};

    // Spawn a background thread that opens images passed via stdin.
    // To allow whitespace characters in filenames, we use the convention that
    // paths in stdin must be separated by newlines.
    thread stdinThread{[&]() {
        string channelSelector;
        while (!shallShutdown) {
            for (string line; getline(cin, line);) {
                string imageFile = tev::ensureUtf8(line);

                if (imageFile.empty()) {
                    continue;
                }

                if (imageFile[0] == ':') {
                    channelSelector = imageFile.substr(1);
                    continue;
                }

                imagesLoader->enqueue(imageFile, channelSelector, false);
            }

            this_thread::sleep_for(chrono::milliseconds{100});
        }
    }};

    // It is unfortunately not easily possible to poll/timeout on cin in a portable manner,
    // so instead we resort to simply detaching this thread, causing it to be forcefully
    // terminated as the main thread terminates.
    stdinThread.detach();

    // Spawn another background thread, this one dealing with images passed to us
    // via inter-process communication (IPC). This happens when
    // a user starts another instance of tev while one is already running. Note, that this
    // behavior can be overridden by the -n flag, so not _all_ secondary instances send their
    // paths to the primary instance.
    thread ipcThread;
    if (ipc->isPrimaryInstance()) {
        ipcThread = thread{[&]() {
            while (!shallShutdown) {
                while (ipc->receiveFromSecondaryInstance([&](const string& reveicedString) {
                    string imageString = ensureUtf8(reveicedString);
                    size_t colonPos = min(imageString.length() - 1, imageString.find_last_of(":"));
                    imagesLoader->enqueue(imageString.substr(0, colonPos), imageString.substr(colonPos + 1), true);
                })) { }

                this_thread::sleep_for(chrono::milliseconds{100});
            }
        }};
    }

    // Load images passed via command line in the background prior to
    // creating our main application such that they are not stalled
    // by the potentially slow initialization of opengl / glfw.
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        imagesLoader->enqueue(imageFile, channelSelector, false);
    }

    // Init nanogui application
    nanogui::init();

    {
        auto app = unique_ptr<ImageViewer>{new ImageViewer{imagesLoader, !imageFiles}};
        app->drawAll();
        app->setVisible(true);

        // Do what the maximize flag tells us---if it exists---and
        // maximize if we have images otherwise.
        if (maximizeFlag ? get(maximizeFlag) : imageFiles) {
            app->maximize();
        }

        // Apply parameter flags
        if (exposureFlag) { app->setExposure(get(exposureFlag)); }
        if (filterFlag)   { app->setFilter(get(filterFlag)); }
        if (gammaFlag)    { app->setGamma(get(gammaFlag)); }
        if (metricFlag)   { app->setMetric(toMetric(get(metricFlag))); }
        if (postProcessingFlag) { app->setPostProcessing(toPostProcessing(get(postProcessingFlag))); }
        if (offsetFlag)   { app->setOffset(get(offsetFlag)); }
        if (tonemapFlag)  { app->setTonemap(toTonemap(get(tonemapFlag))); }

        // Refresh only every 250ms if there are no user interactions.
        // This makes an idling tev surprisingly energy-efficient. :)
        nanogui::mainloop(250);
    }

    shallShutdown = true;

    // On some linux distributions glfwTerminate() (which is called by
    // nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
    //nanogui::shutdown();

    if (ipcThread.joinable()) {
        ipcThread.join();
    }

    if (stdinThread.joinable()) {
        stdinThread.join();
    }

    return 0;
}

TEV_NAMESPACE_END

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    try {
        vector<string> arguments;
        for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
            arguments.emplace_back(tev::utf16to8(argv[i]));
#else
            string arg = argv[i];
            // OSX sometimes (seemingly sporadically) passes the
            // process serial number via a command line parameter.
            // We would like to ignore this.
            if (arg.find("-psn") != 0) {
                arguments.emplace_back(tev::ensureUtf8(argv[i]));
            }
#endif
        }

        tev::mainFunc(arguments);
    } catch (const exception& e) {
        tlog::error() << tfm::format("Uncaught exception: %s", e.what());
        return 1;
    }
}
