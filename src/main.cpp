#include "overlay/DirectionalEventParser.h"
#include "overlay/OverlayService.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using audio_assist::overlay::OverlayAnchor;
using audio_assist::overlay::OverlayConfig;
using audio_assist::overlay::OverlayService;
using audio_assist::overlay::VisualEvent;

namespace {

struct AppOptions {
    OverlayConfig config;
    float min_confidence = 0.50f;
    std::wstring log_path;
    std::vector<std::wstring> disabled_classes;
};

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

bool parseAnchor(const std::wstring& value, OverlayAnchor& anchor) {
    if (value == L"center") {
        anchor = OverlayAnchor::Center;
    } else if (value == L"top-left") {
        anchor = OverlayAnchor::TopLeft;
    } else if (value == L"top-right") {
        anchor = OverlayAnchor::TopRight;
    } else if (value == L"bottom-left") {
        anchor = OverlayAnchor::BottomLeft;
    } else if (value == L"bottom-right") {
        anchor = OverlayAnchor::BottomRight;
    } else {
        return false;
    }
    return true;
}

void printUsage() {
    std::wcerr
        << L"audio_assist_overlay reads classifier/spatial blocks from stdin and renders directional cues.\n\n"
        << L"Options:\n"
        << L"  --size <px>                 Overlay square size, default 360\n"
        << L"  --opacity <0..1>            Window opacity, default 0.82\n"
        << L"  --anchor <center|top-left|top-right|bottom-left|bottom-right>\n"
        << L"  --monitor <index>           Monitor index, default 0\n"
        << L"  --persistence-ms <ms>       Cue decay window, default 650\n"
        << L"  --min-confidence <0..1>     Classifier confidence threshold, default 0.50\n"
        << L"  --icon-only                 Hide text labels\n"
        << L"  --interactive               Disable click-through for settings/testing\n"
        << L"  --critical-only             Show only urgent classes such as gunshots and footsteps\n"
        << L"  --disable-class <label>     Suppress a classifier label; repeatable\n"
        << L"  --log <path>                Append displayed events for usability analysis\n";
}

bool parseOptions(int argc, wchar_t** argv, AppOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto needValue = [&](const wchar_t* name) -> std::wstring {
            if (i + 1 >= argc) {
                std::wcerr << L"Missing value for " << name << L"\n";
                return L"";
            }
            return argv[++i];
        };

        if (arg == L"--help" || arg == L"-h") {
            printUsage();
            return false;
        }
        if (arg == L"--size") {
            options.config.size_px = std::stoi(needValue(L"--size"));
        } else if (arg == L"--opacity") {
            options.config.opacity = std::stof(needValue(L"--opacity"));
        } else if (arg == L"--anchor") {
            OverlayAnchor anchor{};
            if (!parseAnchor(needValue(L"--anchor"), anchor)) {
                std::wcerr << L"Unknown anchor.\n";
                return false;
            }
            options.config.anchor = anchor;
        } else if (arg == L"--monitor") {
            options.config.monitor_id = std::stoi(needValue(L"--monitor"));
        } else if (arg == L"--persistence-ms") {
            options.config.persistence_ms = static_cast<std::uint32_t>(std::stoul(needValue(L"--persistence-ms")));
        } else if (arg == L"--min-confidence") {
            options.min_confidence = std::stof(needValue(L"--min-confidence"));
        } else if (arg == L"--icon-only") {
            options.config.icon_only = true;
        } else if (arg == L"--interactive") {
            options.config.click_through = false;
        } else if (arg == L"--critical-only") {
            options.config.critical_sounds_only = true;
        } else if (arg == L"--disable-class") {
            options.disabled_classes.push_back(needValue(L"--disable-class"));
        } else if (arg == L"--log") {
            options.log_path = needValue(L"--log");
        } else {
            std::wcerr << L"Unknown option: " << arg << L"\n";
            return false;
        }
    }
    return true;
}

void appendLog(std::wofstream& log, const VisualEvent& event) {
    if (!log) {
        return;
    }
    log << audio_assist::overlay::monotonicNowMs()
        << L",label=" << event.label
        << L",confidence=" << event.confidence
        << L",direction=" << audio_assist::overlay::directionName(event.coarse_direction)
        << L",intensity=" << event.intensity
        << L"\n";
}

void stdinReader(OverlayService* service, AppOptions options, std::atomic_bool* done) {
    std::wofstream log;
    if (!options.log_path.empty()) {
        log.open(options.log_path, std::ios::app);
    }

    std::vector<std::string> block;
    std::string line;
    auto flushBlock = [&]() {
        const auto parsed = audio_assist::overlay::parseInputBlock(block);
        block.clear();
        if (!parsed) {
            return;
        }
        for (VisualEvent event : parsed->visual_events) {
            if (event.confidence < options.min_confidence) {
                continue;
            }
            event.coarse_direction = audio_assist::overlay::estimateDirection(*parsed, event.confidence);
            event.intensity = std::clamp((0.55f * event.confidence) + (0.30f * parsed->peak) + (0.15f * parsed->mono_rms * 2.5f),
                                         0.0f,
                                         1.0f);
            event.ttl_ms = options.config.persistence_ms;
            service->pushEvent(event);
            appendLog(log, event);
        }
    };

    while (!done->load() && std::getline(std::cin, line)) {
        if (line.empty()) {
            flushBlock();
            continue;
        }
        if (line.rfind("window_start_ms=", 0) == 0 && !block.empty()) {
            flushBlock();
        }
        block.push_back(line);
    }
    flushBlock();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    AppOptions options;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (!parseOptions(argc, argv, options)) {
            LocalFree(argv);
            return 2;
        }
        LocalFree(argv);
    }

    OverlayService service;
    if (!service.initialize(options.config, instance)) {
        std::wstring message = L"Failed to initialize the directional overlay.";
        if (!service.lastError().empty()) {
            message += L"\n\n";
            message += service.lastError();
        }
        MessageBoxW(nullptr, message.c_str(), L"Audio Assist", MB_ICONERROR);
        return 1;
    }
    for (const std::wstring& label : options.disabled_classes) {
        service.setClassEnabled(label, false);
    }

    std::atomic_bool done{false};
    std::thread reader(stdinReader, &service, options, &done);
    const int result = service.runMessageLoop();
    done.store(true);
    if (reader.joinable()) {
        reader.detach();
    }
    return result;
}
