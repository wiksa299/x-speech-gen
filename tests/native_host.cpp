// Minimal SDK host for the provider and the diagnostic module on Linux.
#include "plugin.hpp"
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPanelGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <signal.h>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
const auto started = Clock::now();
int current = 1;
XPLMLanguageCode sim_language = xplm_Language_English;
unsigned locale_warnings = 0;
fs::path root;
unsigned draw_calls = 0;
std::set<std::uint32_t> provider_colors;
struct Plugin {
    void* module = nullptr;
    std::string signature;
    std::string menu_entry;
    fs::path path;
    XPLMFlightLoop_f flight = nullptr;
    XPLMMenuHandler_f menu = nullptr;
    void* flight_ref = nullptr;
    void* menu_ref = nullptr;
    XPLMCreateWindow_t window{};
    bool enabled = false;
    int (*enable)() = nullptr;
    void (*disable)() = nullptr;
    void (*stop)() = nullptr;
    void (*receive)(int, int, void*) = nullptr;
};
std::array<Plugin, 5> plugins;
struct Result { int owner = 0; std::uint64_t samples = 0; unsigned chunks = 0, status = XSG_UNAVAILABLE; bool done = false; std::string detail; };
std::map<std::uint64_t, Result> results;
std::uint64_t cancel_at_first_audio = 0;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template <typename T> T symbol(void* module, const char* name) {
    auto result = reinterpret_cast<T>(dlsym(module, name));
    require(result != nullptr, name);
    return result;
}
void load(int id, const char* binary, fs::path apparent) {
    current = id;
    auto& plugin = plugins[id];
    plugin.path = std::move(apparent);
    plugin.module = dlopen(binary, RTLD_NOW | RTLD_LOCAL);
    if (!plugin.module) require(false, dlerror());
    char name[256]{}, signature[256]{}, description[256]{};
    require(symbol<int(*)(char*, char*, char*)>(plugin.module, "XPluginStart")(name, signature, description) == 1, "start");
    plugin.signature = signature;
    plugin.enable = symbol<int(*)()>(plugin.module, "XPluginEnable");
    plugin.disable = symbol<void(*)()>(plugin.module, "XPluginDisable");
    plugin.stop = symbol<void(*)()>(plugin.module, "XPluginStop");
    plugin.receive = symbol<void(*)(int, int, void*)>(plugin.module, "XPluginReceiveMessage");
    require(plugin.enable() == 1, "enable");
    plugin.enabled = true;
}
void pump() {
    current = 1;
    auto& provider = plugins[1];
    provider.flight(-1.f, -1.f, 0, provider.flight_ref);
}
template<class Predicate> void wait_for(Predicate ready, const char* error) {
    const auto deadline = Clock::now() + 30s;
    while (Clock::now() < deadline) {
        pump();
        if (ready()) return;
        std::this_thread::sleep_for(5ms);
    }
    require(false, error);
}
XsgRequest request(int sender, XsgRequest input) {
    current = sender;
    XPLMSendMessageToPlugin(1, XSG_MESSAGE_REQUEST, &input);
    return input;
}
void show(int id) {
    current = id;
    auto& plugin = plugins[id];
    plugin.menu(plugin.menu_ref, nullptr);
    plugin.window.drawWindowFunc(reinterpret_cast<XPLMWindowID>(&plugin.window), plugin.window.refcon);
}
}

extern "C" {
XPLMPluginID XPLMGetMyID() { return current; }
int XPLMIsPluginEnabled(XPLMPluginID id) { return id > 0 && id < int(plugins.size()) && plugins[id].enabled; }
XPLMPluginID XPLMFindPluginBySignature(const char* signature) {
    for (unsigned i = 1; i < plugins.size(); ++i) if (plugins[i].signature == signature) return int(i);
    return XPLM_NO_PLUGIN_ID;
}
void XPLMGetPluginInfo(XPLMPluginID id, char*, char* path, char*, char*) {
    if (path) std::strcpy(path, plugins[id].path.c_str());
}
void XPLMGetPrefsPath(char* path) { std::strcpy(path, (root / "X-Plane.prf").c_str()); }
void XPLMGetSystemPath(char* path) { std::strcpy(path, root.c_str()); }
void XPLMEnableFeature(const char*, int) {}
void XPLMDebugString(const char* message) {
    if (std::strstr(message, "/locales/")) ++locale_warnings;
    std::cerr << message;
}
void XPLMSendMessageToPlugin(XPLMPluginID id, int message, void* parameter) {
    if (!XPLMIsPluginEnabled(id)) return;
    const int sender = current;
    current = id;
    if (plugins[id].receive) plugins[id].receive(sender, message, parameter);
    else {
        require(sender == 1 && message == XSG_MESSAGE_EVENT, "Unexpected event");
        const auto& event = *static_cast<const XsgEvent*>(parameter);
        auto& result = results.at(event.request_id);
        require(result.owner == id && !result.done, "Unowned or duplicate event");
        if (event.kind == XSG_AUDIO) {
            require(event.offset == result.samples && event.sample_count > 0 && event.samples, "Invalid audio");
            if (event.request_id == cancel_at_first_audio) {
                require(result.chunks == 0, "Audio delivered after CANCEL was accepted");
                auto cancel = make_request(XSG_CANCEL);
                cancel.request_id = event.request_id;
                require(request(id, cancel).status == XSG_OK, "Reentrant cancel rejected");
            }
            ++result.chunks;
            result.samples += event.sample_count;
        } else {
            require(event.kind == XSG_DONE, "Unknown event kind");
            result.done = true;
            result.status = event.status;
            if (event.detail) result.detail = event.detail;
        }
    }
    current = sender;
}
XPLMLanguageCode XPLMGetLanguage() { return sim_language; }
float XPLMGetElapsedTime() { return std::chrono::duration<float>(Clock::now() - started).count(); }
XPLMMenuID XPLMFindPluginsMenu() { return reinterpret_cast<XPLMMenuID>(&plugins); }
int XPLMAppendMenuItem(XPLMMenuID menu, const char* label, void*, int) {
    if (menu == reinterpret_cast<XPLMMenuID>(&plugins[current])) plugins[current].menu_entry = label;
    return 0;
}
void XPLMRemoveMenuItem(XPLMMenuID, int) {}
void XPLMSetMenuItemName(XPLMMenuID menu, int, const char* label, int) {
    if (menu == reinterpret_cast<XPLMMenuID>(&plugins[current])) plugins[current].menu_entry = label;
}
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f handler, void* ref) {
    plugins[current].menu = handler;
    plugins[current].menu_ref = ref;
    return reinterpret_cast<XPLMMenuID>(&plugins[current]);
}
void XPLMDestroyMenu(XPLMMenuID) {}
void XPLMRegisterFlightLoopCallback(XPLMFlightLoop_f callback, float, void* ref) {
    plugins[current].flight = callback;
    plugins[current].flight_ref = ref;
}
void XPLMUnregisterFlightLoopCallback(XPLMFlightLoop_f, void*) { plugins[current].flight = nullptr; }
void XPLMGetScreenBoundsGlobal(int* left, int* top, int* right, int* bottom) {
    *left = 0; *top = 900; *right = 1200; *bottom = 0;
}
void XPLMGetMouseLocationGlobal(int* x, int* y) { *x = 0; *y = 0; }
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* params) {
    plugins[current].window = *params;
    return reinterpret_cast<XPLMWindowID>(&plugins[current].window);
}
void XPLMDestroyWindow(XPLMWindowID) {}
int XPLMGetWindowIsVisible(XPLMWindowID window) { return reinterpret_cast<XPLMCreateWindow_t*>(window)->visible; }
void XPLMSetWindowIsVisible(XPLMWindowID window, int visible) { reinterpret_cast<XPLMCreateWindow_t*>(window)->visible = visible; }
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMTakeKeyboardFocus(XPLMWindowID) {}
void XPLMGetWindowGeometry(XPLMWindowID window, int* left, int* top, int* right, int* bottom) {
    const auto& w = *reinterpret_cast<XPLMCreateWindow_t*>(window);
    *left = w.left; *top = w.top; *right = w.right; *bottom = w.bottom;
}
void* XPLMCreateTexture(const unsigned char*, int width, int height) {
    require(width > 0 && height > 0, "Missing font texture");
    return &plugins;
}
void XPLMDestroyTexture(void*) {}
void XPLMTransformPush() {}
void XPLMTransformPop() {}
void XPLMTransformTranslate(float, float) {}
void XPLMTransformScale(float, float) {}
void XPLMDrawCalls(const XPLMMesh_t* mesh, int count, const XPLMDrawCall_t*) {
    draw_calls += unsigned(count);
    if (current != 1) return;
    for (int i = 0; i < mesh->vertex_count; ++i) {
        std::uint32_t color;
        std::memcpy(&color, mesh->vertices + 5 * i + 4, sizeof(color));
        provider_colors.insert(color);
    }
}
}

void check_locales(const fs::path& catalogs) {
    auto reload = [&](XPLMLanguageCode language, const char* settings, const char* diagnostic) {
        sim_language = language;
        for (int id : {2, 1}) {
            current = id;
            plugins[id].disable();
            plugins[id].enabled = false;
        }
        for (int id : {1, 2}) {
            current = id;
            require(plugins[id].enable() == 1, "Cannot reactivate localized plugin");
            plugins[id].enabled = true;
            show(id);
        }
        require(plugins[1].menu_entry == settings, "Incorrect provider menu translation");
        require(plugins[2].menu_entry == diagnostic, "Incorrect diagnostic menu translation");
    };
    reload(xplm_Language_French, "Paramètres", "Client de test");
    wait_for([] { return request(3, make_request(XSG_GET_STATUS)).ready != 0; }, "French engine unavailable");
    const std::string invalid = "{unknown:1}";
    auto speak = make_request(XSG_SPEAK);
    speak.text = view(invalid);
    speak = request(3, speak);
    require(speak.status == XSG_OK, "Localized error test rejected");
    results[speak.request_id].owner = 3;
    wait_for([&] { return results[speak.request_id].done; }, "Missing localized error");
    require(results[speak.request_id].detail == "Balise aéronautique inconnue : unknown", "Wrong engine error language");
    results.erase(speak.request_id);
    reload(xplm_Language_Spanish, "Ajustes", "Cliente de prueba");
    const auto preference = root / "X-Speech-Gen-ui.json";
    std::ofstream(preference) << R"({"interface_language":"fr"})";
    reload(xplm_Language_Spanish, "Paramètres", "Client de test");
    std::ofstream(preference) << R"({"interface_language":"en"})";
    reload(xplm_Language_Spanish, "Settings", "Test client");
    std::ofstream(preference) << R"({"interface_language":123})";
    reload(xplm_Language_Spanish, "Ajustes", "Cliente de prueba");
    fs::remove(preference);
    reload(xplm_Language_Unknown, "Settings", "Test client");
    std::ofstream(root / "locales/fr.json") << R"({"menu.settings":"Paramètres personnalisés"})";
    reload(xplm_Language_French, "Paramètres personnalisés", "Test client");
    std::ofstream(root / "locales/fr.json") << "{";
    const auto before = locale_warnings;
    reload(xplm_Language_French, "Settings", "Test client");
    show(1);
    show(2);
    require(locale_warnings == before + 2, "Expected one invalid-file warning per module");
    fs::remove(root / "locales/en.json");
    reload(xplm_Language_English, "Settings", "Test client");
    fs::copy(catalogs, root / "locales", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}
int main(int argc, char** argv) {
    require(argc == 5 || argc == 6, "Expected provider, diagnostic, engine and asset root");
    std::string temporary = (fs::temp_directory_path() / "xsg-native-XXXXXX").string();
    require(mkdtemp(temporary.data()) != nullptr, "Cannot create test directory");
    root = temporary;
    if (argc == 6) fs::copy(argv[5], root / "locales", fs::copy_options::recursive);
    fs::create_directories(root / "bin");
    fs::create_symlink(fs::absolute(argv[3]), root / "bin/xsg-engine");
    std::ofstream(root / "X-Speech-Gen.json") << R"({"radio_profile":"faa"})";
    setenv("XSG_MODEL_ROOT", argv[4], 1);
    load(1, argv[1], root / "64/lin.xpl");
    load(2, argv[2], root / "diagnostic/64/lin.xpl");
    require(plugins[1].menu_entry == "Settings", "Provider menu entry must be Settings");
    plugins[3].enabled = plugins[4].enabled = true;
    if (argc == 6) check_locales(argv[5]);
    // Exercise the message boundary with raw request headers and no valid current payload.
    current = 3;
    auto unrelated = make_request(XSG_GET_STATUS);
    XPLMSendMessageToPlugin(1, 0x58534701, &unrelated);
    require(unrelated.status == XSG_UNAVAILABLE, "Unrecognized message modified a request");
    std::array<std::uint32_t, 48> unsupported{};
    unsupported[0] = XSG_ABI_VERSION + 1;
    unsupported[1] = sizeof(unsupported);
    unsupported[2] = XSG_SPEAK;
    unsupported[3] = XSG_UNAVAILABLE;
    const auto unsupported_payload = unsupported;
    XPLMSendMessageToPlugin(1, XSG_MESSAGE_REQUEST, unsupported.data());
    require(unsupported[3] == XSG_BAD_ABI, "Unsupported ABI was not rejected");
    unsupported[3] = XSG_UNAVAILABLE;
    require(unsupported == unsupported_payload, "ABI rejection modified the unsupported payload");
    std::array<std::uint32_t, 4> short_request = {
        XSG_ABI_VERSION, sizeof(short_request), XSG_SPEAK, XSG_UNAVAILABLE};
    XPLMSendMessageToPlugin(1, XSG_MESSAGE_REQUEST, short_request.data());
    require(short_request[3] == XSG_BAD_ABI, "Short request was not rejected");
    short_request[1] = 3 * sizeof(std::uint32_t);
    short_request[3] = XSG_UNAVAILABLE;
    XPLMSendMessageToPlugin(1, XSG_MESSAGE_REQUEST, short_request.data());
    require(short_request[3] == XSG_UNAVAILABLE, "Rejection wrote beyond the declared request size");

    auto ready = [] { return request(3, make_request(XSG_GET_STATUS)).ready != 0; };
    wait_for(ready, "Provider did not start");
    auto sequence = make_request(XSG_GET_CAPABILITIES);
    sequence = request(3, sequence);
    require(sequence.status == XSG_OUTPUT_LIMIT && sequence.result_bytes > 100, "Capability size probe");
    std::string catalog(sequence.result_bytes, '\0');
    sequence.result = catalog.data();
    sequence.result_capacity = sequence.result_bytes;
    require(request(3, sequence).status == XSG_OK && catalog.find("phonon-40m-en") != std::string::npos, "Capability content");
    show(1);
    show(1);
    show(2);
    require(draw_calls > 0, "No Panel Graphics calls");
    require(provider_colors.count(IM_COL32(128, 189, 255, 255)), "Section heading blue missing");
    require(provider_colors.count(IM_COL32(54, 82, 122, 255)), "Selected tab theme missing");
    require(provider_colors.count(IM_COL32(53, 58, 68, 255)), "Dropdown field theme missing");

    const std::string text = "Heading {heading:270}, runway {runway:09L}, speed {speed:300}.";
    auto speak = make_request(XSG_SPEAK);
    speak.text = view(text);
    speak.options = XSG_SEED;
    speak.seed = 42;
    speak.delivery = XSG_STREAM;
    const auto first = request(3, speak);
    speak.delivery = XSG_COMPLETE;
    speak.radio_profile = XSG_FAA;
    const auto other = request(4, speak);
    speak.radio_profile = XSG_SERA;
    const auto sera = request(3, speak);
    require(first.status == XSG_OK && other.status == XSG_OK && sera.status == XSG_OK, "Native requests rejected");
    results[first.request_id].owner = 3;
    results[other.request_id].owner = 4;
    results[sera.request_id].owner = 3;
    wait_for([&] { return results[first.request_id].done && results[other.request_id].done &&
        results[sera.request_id].done; }, "Requests did not terminate");
    require(results[first.request_id].status == XSG_OK && results[other.request_id].status == XSG_OK &&
        results[first.request_id].chunks > 1 && results[other.request_id].chunks == 1, "Audio delivery");
    require(results[first.request_id].samples == results[other.request_id].samples &&
        results[sera.request_id].status == XSG_OK &&
        results[sera.request_id].samples != results[first.request_id].samples, "Saved FAA default or SERA override ignored");

    auto worker_pid = [] {
        pid_t child = -1;
        for (const auto& task : fs::directory_iterator("/proc/self/task")) {
            std::ifstream children(task.path() / "children");
            pid_t candidate;
            while (children >> candidate) {
                std::error_code error;
                const auto executable = fs::read_symlink(fs::path("/proc") / std::to_string(candidate) / "exe", error);
                if (!error && executable.filename() == "xsg-engine") child = candidate;
            }
        }
        require(child > 0, "No worker PID");
        return child;
    };
    std::string longer;
    for (int i = 0; i < 16; ++i) longer += "Hold short of runway {runway:09L}. ";
    speak.delivery = XSG_STREAM;
    speak.text = view(longer);
    const auto cancel_test = request(3, speak);
    require(cancel_test.status == XSG_OK, "Cancellation test rejected");
    results[cancel_test.request_id].owner = 3;
    cancel_at_first_audio = cancel_test.request_id;
    std::this_thread::sleep_for(400ms);
    wait_for([&] { return results[cancel_test.request_id].done; }, "Cancellation did not finish");
    require(results[cancel_test.request_id].status == XSG_CANCELLED, "Wrong cancelled status");

    const auto interrupted = request(3, speak);
    require(interrupted.status == XSG_OK, "Recovery request rejected");
    results[interrupted.request_id].owner = 3;
    wait_for([&] { return results[interrupted.request_id].chunks > 0; }, "No audio before interruption");
    require(!results[interrupted.request_id].done, "Request completed before the interruption");
    require(kill(worker_pid(), SIGKILL) == 0, "Could not simulate worker loss");
    wait_for([&] { return results[interrupted.request_id].done && ready(); }, "Worker did not recover");
    require(results[interrupted.request_id].status == XSG_ENGINE_LOST, "Wrong worker loss status");

    const auto stalled = request(3, speak);
    require(stalled.status == XSG_OK, "Stalled request rejected");
    results[stalled.request_id].owner = 3;
    wait_for([&] { return results[stalled.request_id].chunks > 0; }, "No audio before cancellation");
    require(kill(worker_pid(), SIGSTOP) == 0, "Could not suspend test worker");
    const auto waiting = request(4, speak);
    require(waiting.status == XSG_OK, "Queued request rejected");
    results[waiting.request_id].owner = 4;
    auto cancel = make_request(XSG_CANCEL);
    cancel.request_id = stalled.request_id;
    require(request(3, cancel).status == XSG_OK, "Cancel rejected");
    wait_for([&] { return results[waiting.request_id].done && results[stalled.request_id].done && ready(); },
             "No recovery after cancellation timeout");
    require(results[stalled.request_id].status == XSG_CANCELLED &&
            results[waiting.request_id].status == XSG_ENGINE_LOST, "Incorrect per-client timeout status");

    for (int id : {2, 1}) {
        current = id;
        plugins[id].disable();
        plugins[id].enabled = false;
        plugins[id].stop();
        dlclose(plugins[id].module);
    }
    fs::remove_all(root);
    std::cout << "Loaded both modules, tested SDK callbacks, two clients and worker recovery\n";
}
