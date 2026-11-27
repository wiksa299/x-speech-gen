#include "locale.hpp"
#include "player.hpp"
#include "plugin.hpp"
#include "window.hpp"

#include <XPLMMenus.h>
#include <XPLMUtilities.h>
#include <misc/cpp/imgui_stdlib.h>

#include <memory>

namespace {
xsg::Locale ui;
std::unique_ptr<xsg::Window> window;
std::unique_ptr<xsg::Player> player;
XPLMMenuID menu = nullptr;
int menu_item = -1;
int test_item = -1;
std::filesystem::path locale_root, locale_preferences;
std::uint64_t pending = 0, samples = 0;
std::string text = "Runway {runway:09L}, heading {heading:270}. Contact {frequency:118.050}.";
std::string message;
bool stream = true;
int radio_profile = XSG_DEFAULT_RADIO_PROFILE;

XPLMPluginID service() {
    return XPLMFindPluginBySignature(XSG_PROVIDER_SIGNATURE);
}
void stop() {
    const auto provider = service();
    if (provider != XPLM_NO_PLUGIN_ID && pending) {
        auto request = make_request(XSG_CANCEL);
        request.request_id = pending;
        XPLMSendMessageToPlugin(provider, XSG_MESSAGE_REQUEST, &request);
    }
    if (player) player->finish(false);
}
void draw() {
    ImGui::InputTextMultiline(ui.label("diagnostic.text"), &text, {-1, 110});
    ImGui::Checkbox(ui.label("activity.stream"), &stream);
    const char* profiles[] = {ui.text("diagnostic.use_provider"), ui.text("radio.sera"), ui.text("radio.faa")};
    ImGui::Combo(ui.label("diagnostic.profile"), &radio_profile, profiles, IM_ARRAYSIZE(profiles));
    ImGui::BeginDisabled(pending != 0);
    if (ImGui::Button(ui.label("diagnostic.speak"))) {
        auto request = make_request(XSG_SPEAK);
        request.text = view(text);
        request.delivery = stream ? XSG_STREAM : XSG_COMPLETE;
        request.radio_profile = std::uint32_t(radio_profile);
        const auto provider = service();
        if (provider != XPLM_NO_PLUGIN_ID) XPLMSendMessageToPlugin(provider, XSG_MESSAGE_REQUEST, &request);
        message = ui.text(status_key(request.status));
        if (request.status == XSG_OK) {
            pending = request.request_id;
            samples = 0;
            player->begin();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ui.label("voice.stop"))) stop();
    ImGui::TextWrapped("%s", ui.format("diagnostic.samples", {{"count", std::to_string(samples)}}).c_str());
    ImGui::TextWrapped("%s", message.c_str());
    const auto error = player->error();
    if (!error.empty()) ImGui::TextWrapped("%s", ui.text(error.c_str()));
}
void show(void*, void*) { window->show(ui.text("diagnostic.title")); }
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    metadata(name, signature, description, "X-Speech Gen Diagnostic", xsg::diagnostic_signature, "TTS client and audio playback demonstration");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    return 1;
}
PLUGIN_API void XPluginDisable() {
    stop();
    pending = 0;
    if (menu) XPLMDestroyMenu(menu);
    if (menu_item >= 0) XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_item);
    menu = nullptr;
    menu_item = -1;
    player.reset();
    window.reset();
}
PLUGIN_API void XPluginStop() { if (window) XPluginDisable(); }
PLUGIN_API int XPluginEnable() {
    try {
        char file[2048]{};
        const auto provider = service();
        XPLMGetPluginInfo(provider == XPLM_NO_PLUGIN_ID ? XPLMGetMyID() : provider, nullptr, file, nullptr, nullptr);
        auto root = std::filesystem::u8path(file).parent_path().parent_path();
        if (provider == XPLM_NO_PLUGIN_ID) root = root.parent_path() / "X-Speech-Gen";
        locale_root = root;
        locale_preferences = xsg::interface_preferences_path();
        ui.load(root, xsg::read_interface_language(locale_preferences));
        message.clear();
        window = std::make_unique<xsg::Window>(draw, 490, 310, root / "NotoSans-Regular.ttf");
        player = std::make_unique<xsg::Player>();
        menu_item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), ui.text("diagnostic.title"), nullptr, 0);
        menu = XPLMCreateMenu(ui.text("diagnostic.title"), XPLMFindPluginsMenu(), menu_item, show, nullptr);
        test_item = XPLMAppendMenuItem(menu, ui.text("diagnostic.menu"), nullptr, 0);
        return 1;
    } catch (const std::exception&) {
        XPluginDisable();
        return 0;
    }
}
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int kind, void* data) {
    if (sender != service() || !player) return;
    if (kind == xsg::locale_changed_message) {
        ui.load(locale_root, xsg::read_interface_language(locale_preferences));
        message.clear();
        const auto title = ui.text("diagnostic.title");
        XPLMSetMenuItemName(XPLMFindPluginsMenu(), menu_item, title, 0);
        XPLMSetMenuItemName(menu, test_item, ui.text("diagnostic.menu"), 0);
        window->set_title(title);
        return;
    }
    if (kind != XSG_MESSAGE_EVENT || !data) return;
    const auto& event = *static_cast<const XsgEvent*>(data);
    if (event.version != XSG_ABI_VERSION || event.struct_size < sizeof(event) || event.request_id != pending) return;
    if (event.kind == XSG_AUDIO && event.offset == samples && event.samples) {
        player->append(event.samples, event.sample_count);
        samples += event.sample_count;
    } else if (event.kind == XSG_DONE) {
        pending = 0;
        player->finish(event.status == XSG_OK);
        message = event.detail ? event.detail : ui.text(status_key(event.status));
    }
}
