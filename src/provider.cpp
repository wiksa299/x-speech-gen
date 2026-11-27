#include "locale.hpp"
#include "player.hpp"
#include "plugin.hpp"
#include "service.hpp"
#include "window.hpp"

#include <XPLMDataAccess.h>
#include <XPLMMenus.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <map>
#include <utility>

namespace {
using namespace xsg;
Locale ui;
const char* default_model = "phonon-40m-en";
constexpr int local_client = -100;
using Clock = std::chrono::steady_clock;
constexpr auto card_flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY;
constexpr ImVec4 primary_button{0.16f, 0.36f, 0.57f, 1.f};

struct SpokenLanguage {
    const char* code;
    const char* name;
    const char* sample;
};
constexpr SpokenLanguage languages[] = {
    {"en", "language.en", "The cabin is ready for departure."},
    {"fr", "language.fr", "La cabine est prête pour le départ."},
    {"es", "language.es", "La cabina está lista para la salida."},
    {"de", "language.de", "Die Kabine ist bereit für den Abflug."},
    {"pt", "language.pt", "A cabine está pronta para a partida."},
};

const char* model_key(const std::string& id) {
    if (id == "phonon-31m-en") return "model.en31";
    if (id == "phonon-40m-en") return "model.en40";
    if (id == "phonon-40m-multilingual") return "model.multi";
    return "voice.model";
}

struct RequestActivity {
    XPLMPluginID client;
    std::string name;
    std::string time;
    std::uint32_t delivery;
    Clock::time_point accepted;
    bool cancelled = false;
    double first_ms = -1.;
    std::size_t chunks = 0, samples = 0;
    double queue_ms = -1., total_ms = 0.;
    std::uint32_t status = XSG_OK;
    std::string detail;

    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - accepted).count();
    }
};

enum class LocalAction { Pronunciation, Listen, Voice };
enum class LoadPolicy { FirstUse, Activation };
enum class UnloadPolicy { AfterUse, Deactivation, Inactivity };
constexpr int default_idle_minutes = 5;

class Provider : public std::enable_shared_from_this<Provider> {
public:
    void enable();
    void disable();
    void poll();
    void draw();
    void request(XPLMPluginID client, XsgRequest& request);

private:
    void load_profile();
    void draw_settings(bool ready);
    void draw_models_folder(bool ready);
    void draw_model_settings(bool ready);
    void apply_unload_policy();
    void schedule_preload();
    void draw_test(bool ready);
    void draw_activity();
    void apply_interface_language();
    void draw_listen(LocalAction action, bool ready);
    void start_local(LocalAction action);
    std::unique_ptr<Service> service;
    std::unique_ptr<Window> window;
    std::unique_ptr<Player> player;
    std::map<std::uint64_t, RequestActivity> owners;
    std::deque<std::pair<std::uint64_t, RequestActivity>> activity;
    XPLMMenuID menu = nullptr;
    int menu_item = -1;
    int settings_item = -1;
    fs::path locale_root, locale_preferences;
    int interface_language = 0, requested_language = 0;
    bool language_save_failed = false;
    std::string model = default_model, language = "en", voice = "default";
    std::string search, prepared, info, voice_info;
    std::string folder_draft, folder_feedback;
    std::string text = "Maintain {speed:300} knots. Contact tower on {frequency:118.050}.";
    bool profile_loaded = false, accepting = false;
    bool faa = false;
    LoadPolicy load_policy = LoadPolicy::FirstUse;
    UnloadPolicy unload_policy = UnloadPolicy::Inactivity;
    int idle_minutes = default_idle_minutes;
    int idle_minutes_draft = default_idle_minutes;
    Json catalog_cache;
    std::uint64_t catalog_revision = 0;
    std::uint64_t local_request = 0;
    LocalAction local_action = LocalAction::Pronunciation;
    int test_profile = XSG_DEFAULT_RADIO_PROFILE, activity_filter = 0;

    Json options() const {
        return {{"model",model}, {"language",language}, {"voice",voice},
                {"radio_profile",faa ? "faa" : "sera"}};
    }
    Json settings() const {
        auto saved = options();
        saved["model_load"] = load_policy == LoadPolicy::Activation ? "activation" : "first_use";
        saved["model_unload"] = unload_policy == UnloadPolicy::AfterUse ? "after_use" :
            unload_policy == UnloadPolicy::Deactivation ? "deactivation" : "inactivity";
        saved["model_idle_minutes"] = idle_minutes;
        return saved;
    }
    const SpokenLanguage& selected_language() const {
        const auto found = std::find_if(std::begin(languages), std::end(languages),
            [&](const auto& value) { return language == value.code; });
        return found == std::end(languages) ? languages[0] : *found;
    }
    static float tick(float, float, int, void* ref) {
        auto provider = static_cast<Provider*>(ref)->shared_from_this();
        try { provider->poll(); } catch (const std::exception&) {
            XPLMDebugString("X-Speech Gen: event dispatch failed\n");
        }
        return -1.f;
    }
    static void menu_click(void* ref, void*) { static_cast<Provider*>(ref)->window->show("X-Speech Gen"); }
};

std::shared_ptr<Provider> provider;

void Provider::enable() {
    char file[2048]{}, prefs[2048]{};
    XPLMGetPrefsPath(prefs);
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, file, nullptr, nullptr);
    auto root = fs::u8path(file).parent_path().parent_path();
#ifdef _WIN32
    auto executable = root / "bin" / "xsg-engine.exe";
#else
    auto executable = root / "bin" / "xsg-engine";
#endif
    locale_root = root;
    locale_preferences = interface_preferences_path();
    interface_language = requested_language = read_interface_language(locale_preferences);
    ui.load(root, interface_language);
    auto models = root;
    if (const char* override_root = std::getenv("XSG_MODEL_ROOT")) models = fs::u8path(override_root);
    window = std::make_unique<Window>([this] { draw(); }, 660, 610, root / "NotoSans-Regular.ttf", true);
    player = std::make_unique<Player>();
    auto preferences = fs::u8path(prefs).parent_path() / "X-Speech-Gen.json";
    service = std::make_unique<Service>(std::move(executable), std::move(models), preferences, Service::default_idle_timeout, ui.engine_strings());
    accepting = true;
    menu_item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Speech Gen", nullptr, 0);
    menu = XPLMCreateMenu("X-Speech Gen", XPLMFindPluginsMenu(), menu_item, menu_click, this);
    settings_item = XPLMAppendMenuItem(menu, ui.text("menu.settings"), nullptr, 0);
    XPLMRegisterFlightLoopCallback(tick, -1.f, this);
}

void Provider::disable() {
    accepting = false;
    XPLMUnregisterFlightLoopCallback(tick, this);
    if (menu) XPLMDestroyMenu(menu);
    if (menu_item >= 0) XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_item);
    for (const auto& entry : owners) {
        if (!XPLMIsPluginEnabled(entry.second.client)) continue;
        XsgEvent update{XSG_ABI_VERSION, sizeof(XsgEvent), XSG_DONE, XSG_ENGINE_LOST, entry.first, 0, nullptr, 0, nullptr};
        XPLMSendMessageToPlugin(entry.second.client, XSG_MESSAGE_EVENT, &update);
    }
    owners.clear();
    service.reset();
    player.reset();
    window.reset();
}

void Provider::request(XPLMPluginID client, XsgRequest& input) {
    if (!accepting || !service) { input.status = XSG_UNAVAILABLE; return; }
    load_profile();
    if (!profile_loaded && input.operation == XSG_SPEAK) { input.status = XSG_UNAVAILABLE; return; }
    const auto accepted = Clock::now();
    const auto status = service->request(client, input, options());
    input.status = status;
    if (status == XSG_OK && input.operation == XSG_CANCEL) {
        const auto it = owners.find(input.request_id);
        if (it != owners.end()) it->second.cancelled = true;
    }
    if (status == XSG_OK && input.operation == XSG_SPEAK) {
        char name[256]{}, time[16]{};
        XPLMGetPluginInfo(client, name, nullptr, nullptr, nullptr);
        const auto now = std::time(nullptr);
        if (const auto* local = std::localtime(&now)) std::strftime(time, sizeof(time), "%H:%M:%S", local);
        owners.emplace(input.request_id, RequestActivity{client,
            *name ? std::string(name) : ui.format("activity.client", {{"id", std::to_string(client)}}), time, input.delivery, accepted});
    }
}

void Provider::load_profile() {
    if (!profile_loaded && service->ready()) {
        const auto saved = service->profile();
        if (saved.is_object()) {
            model = saved.value("model", model);
            language = saved.value("language", language);
            voice = saved.value("voice", voice);
            faa = saved.contains("radio_profile") && saved["radio_profile"] == "faa";
            if (saved.contains("model_load") && saved["model_load"] == "activation")
                load_policy = LoadPolicy::Activation;
            if (const auto value = saved.find("model_unload"); value != saved.end()) {
                if (*value == "after_use") unload_policy = UnloadPolicy::AfterUse;
                if (*value == "deactivation") unload_policy = UnloadPolicy::Deactivation;
            }
            if (const auto value = saved.find("model_idle_minutes"); value != saved.end() &&
                value->is_number_integer() && *value >= 1 && *value <= 60)
                idle_minutes = idle_minutes_draft = value->get<int>();
        }
        profile_loaded = true;
        apply_unload_policy();
    }
    const auto revision = service->catalog_revision();
    if (!profile_loaded || revision == catalog_revision) return;
    catalog_revision = revision;
    catalog_cache = service->catalog();
    folder_draft = service->models_folder().path;
    folder_feedback.clear();
    const auto previous = options();
    const auto& models = catalog_cache.at("models");
    const auto available = [](const Json& entry) { return entry.value("available", false); };
    const auto selected = std::find_if(models.begin(), models.end(), [&](const Json& entry) {
        return available(entry) && entry.value("id", "") == model;
    });
    if (selected == models.end()) {
        const auto first = std::find_if(models.begin(), models.end(), available);
        if (first != models.end()) {
            model = first->at("id").get<std::string>();
            language = "en";
            voice = "default";
        }
    }
    if (voice != "default") {
        const auto& voices = catalog_cache.at("voices");
        if (std::none_of(voices.begin(), voices.end(), [&](const Json& entry) {
            return entry.value("voice_id", "") == voice && entry.value("language", "") == language;
        })) voice = "default";
    }
    if (previous != options()) service->save_profile(settings());
    schedule_preload();
}

void Provider::apply_unload_policy() {
    switch (unload_policy) {
    case UnloadPolicy::AfterUse: service->set_idle_timeout(std::chrono::milliseconds(0)); break;
    case UnloadPolicy::Deactivation: service->set_idle_timeout(std::nullopt); break;
    case UnloadPolicy::Inactivity: service->set_idle_timeout(std::chrono::minutes(idle_minutes)); break;
    }
}

void Provider::schedule_preload() {
    const auto& models = catalog_cache.at("models");
    const bool available = std::any_of(models.begin(), models.end(), [&](const auto& entry) {
        return entry.value("available", false) && entry.value("id", "") == model;
    });
    service->schedule_preload(load_policy == LoadPolicy::Activation && available ? options() : Json());
}

void Provider::apply_interface_language() {
    if (requested_language == interface_language) return;
    Locale translated;
    translated.load(locale_root, requested_language);
    language_save_failed = !save_interface_language(locale_preferences, requested_language);
    if (language_save_failed) {
        requested_language = interface_language;
        return;
    }
    interface_language = requested_language;
    ui = std::move(translated);
    service->set_ui_strings(ui.engine_strings());
    info.clear();
    voice_info.clear();
    folder_feedback.clear();
    XPLMSetMenuItemName(menu, settings_item, ui.text("menu.settings"), 0);
    const auto diagnostic = XPLMFindPluginBySignature(diagnostic_signature);
    if (diagnostic != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(diagnostic))
        XPLMSendMessageToPlugin(diagnostic, locale_changed_message, nullptr);
}

void Provider::poll() {
    apply_interface_language();
    load_profile();
    for (auto it = owners.begin(); it != owners.end();) {
        if (XPLMIsPluginEnabled(it->second.client)) { ++it; continue; }
        service->cancel(it->second.client, it->first);
        it = owners.erase(it);
    }
    for (const auto& event : service->drain()) {
        if (!accepting) break;
        if (event.client == local_client) {
            if (event.id != local_request) continue;
            auto& feedback = local_action == LocalAction::Voice ? voice_info : info;
            if (event.kind == XSG_AUDIO) player->append(event.samples.data(), event.samples.size());
            else if (event.kind == prepared_event || event.kind == preview_event) {
                if (event.status == XSG_OK && local_action != LocalAction::Voice) prepared = event.detail;
                if (event.status != XSG_OK) feedback = event.detail.empty() ? ui.text(status_key(event.status)) : event.detail;
                if (event.kind == preview_event) local_request = 0;
            } else {
                player->finish(event.status == XSG_OK);
                local_request = 0;
                if (event.status != XSG_OK) feedback = event.detail.empty() ? ui.text(status_key(event.status)) : event.detail;
            }
            continue;
        }
        const auto found = owners.find(event.id);
        if (found == owners.end() || found->second.client != event.client || !XPLMIsPluginEnabled(event.client)) continue;
        auto& row = found->second;
        const XsgEvent update{XSG_ABI_VERSION, sizeof(XsgEvent), event.kind, event.status, event.id, event.offset,
            event.samples.data(), std::uint32_t(event.samples.size()), event.detail.empty() ? nullptr : event.detail.c_str()};
        if (event.kind == XSG_AUDIO) {
            if (row.cancelled) continue;
            if (!row.chunks) row.first_ms = row.elapsed_ms();
            ++row.chunks;
            row.samples += event.samples.size();
        } else if (event.kind == XSG_DONE) {
            row.total_ms = row.elapsed_ms();
            row.queue_ms = event.queue_ms;
            row.status = event.status;
            row.detail = event.detail;
            if (activity.size() == 12) activity.pop_back();
            activity.emplace_front(event.id, std::move(row));
            owners.erase(found);
        }
        XPLMSendMessageToPlugin(event.client, XSG_MESSAGE_EVENT, const_cast<XsgEvent*>(&update));
    }
}

void Provider::start_local(LocalAction action) {
    const bool voice_preview = action == LocalAction::Voice;
    const bool prepare_only = action == LocalAction::Pronunciation;
    auto selected = options();
    if (!voice_preview && test_profile != XSG_DEFAULT_RADIO_PROFILE)
        selected["radio_profile"] = test_profile == XSG_FAA ? "faa" : "sera";
    auto& feedback = voice_preview ? voice_info : info;
    feedback.clear();
    if (!voice_preview) prepared.clear();
    const auto status = service->submit(local_client, voice_preview ? selected_language().sample : text,
        std::move(selected), prepare_only ? XSG_COMPLETE : XSG_STREAM, local_request, prepare_only);
    if (status == XSG_OK) {
        local_action = action;
        if (!prepare_only) player->begin();
    } else {
        feedback = ui.text(status_key(status));
    }
}

void Provider::draw_listen(LocalAction action, bool ready) {
    const bool playing = player->playing();
    const bool stopping = local_action == action && (local_request || playing);
    ImGui::BeginDisabled(!stopping && (!ready || local_request || playing));
    if (!stopping) ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    const bool clicked = ImGui::Button(stopping ? ui.label("voice.stop") : ui.label("voice.listen"));
    if (!stopping) ImGui::PopStyleColor();
    if (clicked) {
        if (stopping) {
            if (local_request) service->cancel(local_client, local_request);
            player->finish(false);
        } else {
            start_local(action);
        }
    }
    ImGui::EndDisabled();
    const auto& feedback = action == LocalAction::Voice ? voice_info : info;
    if (!feedback.empty()) ImGui::TextWrapped("%s", feedback.c_str());
    if (local_action == action) {
        const auto audio_error = player->error();
        if (!audio_error.empty()) ImGui::TextWrapped("%s", ui.text(audio_error.c_str()));
    }
}

void Provider::draw_models_folder(bool ready) {
    if (ImGui::BeginChild("models-folder", {}, card_flags)) {
        const auto folder = service->models_folder();
        window->section_heading(ui.text("folder.title"));
        if (!folder.path.empty()) ImGui::TextWrapped("%s", folder.path.c_str());
        const auto change = [&](ModelsAction action) {
            const auto status = service->configure_models(action, action == ModelsAction::Apply ? folder_draft : "");
            folder_feedback = status == XSG_OK ? "" :
                status == XSG_BUSY ? ui.text("folder.wait") : ui.text(status_key(status));
        };
        ImGui::BeginDisabled(!ready || service->busy());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##models-path", ui.text("folder.hint"), &folder_draft);
        ImGui::BeginDisabled(folder_draft.empty() || folder_draft == folder.path);
        if (ImGui::Button(ui.label("folder.apply"))) change(ModelsAction::Apply);
        ImGui::EndDisabled();
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button(ui.label("folder.browse"))) change(ModelsAction::Browse);
#endif
        ImGui::SameLine();
        if (ImGui::Button(ui.label("folder.rescan"))) change(ModelsAction::Rescan);
        ImGui::SameLine();
        if (ImGui::Button(ui.label("folder.reset"))) change(ModelsAction::Default);
        ImGui::EndDisabled();
        if (folder.pending) ImGui::TextDisabled("%s", ui.text("folder.pending"));
        else if (!folder_feedback.empty()) ImGui::TextWrapped("%s", folder_feedback.c_str());
        else if (!folder.error.empty()) ImGui::TextWrapped("%s", folder.error.c_str());
        else if (ready && catalog_cache.is_object()) {
            const auto& models = catalog_cache.at("models");
            if (std::none_of(models.begin(), models.end(), [](const Json& entry) { return entry.value("available", false); }))
                ImGui::TextWrapped("%s", ui.text("folder.empty"));
        }
    }
    ImGui::EndChild();
}

void Provider::draw_model_settings(bool ready) {
    if (ImGui::BeginChild("model-settings", {}, card_flags)) {
        window->section_heading(ui.text("models.title"));
        ImGui::BeginDisabled(!ready || local_request || player->playing());
        if (catalog_cache.is_object()) {
            ImGui::TextUnformatted(ui.text("voice.model"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##model", ui.text(model_key(model)))) {
                for (const auto& entry : catalog_cache.at("models")) {
                    if (!entry.value("available", false)) continue;
                    const auto id = entry.value("id", "");
                    if (ImGui::Selectable(ui.label(model_key(id)), id == model) && id != model) {
                        model = id;
                        language = "en";
                        voice = "default";
                    }
                }
                ImGui::EndCombo();
            }
        }

        const auto load_labels = std::array{ui.text("models.first_use"), ui.text("models.activation")};
        const auto unload_labels = std::array{ui.text("models.after_use"), ui.text("models.deactivation"),
                                             ui.text("models.inactivity")};
        const float room = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const bool two_columns = room >= 430.0f;
        const float column = (room - gap) / 2;
        const float right = ImGui::GetCursorPosX() + column + gap;
        ImGui::TextUnformatted(ui.text("models.load"));
        if (two_columns) {
            ImGui::SameLine(right);
            ImGui::TextUnformatted(ui.text("models.unload"));
        }
        int load = static_cast<int>(load_policy), unload = static_cast<int>(unload_policy);
        ImGui::SetNextItemWidth(two_columns ? column : -1);
        if (ImGui::Combo("##load-model", &load, load_labels.data(), int(load_labels.size())))
            load_policy = static_cast<LoadPolicy>(load);
        if (two_columns) ImGui::SameLine(right);
        else ImGui::TextUnformatted(ui.text("models.unload"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##unload-model", &unload, unload_labels.data(), int(unload_labels.size())))
            unload_policy = static_cast<UnloadPolicy>(unload);
        if (unload_policy == UnloadPolicy::Inactivity) {
            ImGui::TextUnformatted(ui.text("models.idle_minutes"));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##model-idle-minutes", &idle_minutes_draft, 1, 60);
            if (ImGui::IsItemDeactivatedAfterEdit()) idle_minutes = idle_minutes_draft;
        }
        ImGui::EndDisabled();
        const auto model_status = service->model_status();
        const char* state = model_status.state == ModelState::Loading ? "models.loading" :
            model_status.state == ModelState::Ready ? "models.ready" : "models.unloaded";
        ImGui::TextUnformatted(ui.text(state));
        if (!model_status.error.empty()) ImGui::TextWrapped("%s", model_status.error.c_str());
    }
    ImGui::EndChild();
}

void Provider::draw_settings(bool ready) {
    const char* choices[] = {ui.text("interface.auto"), "English", "Français", "Español"};
    ImGui::Combo(ui.label("interface.language"), &requested_language, choices, IM_ARRAYSIZE(choices));
    if (language_save_failed) ImGui::TextWrapped("%s", ui.text("error.preferences"));
    const auto original = settings();
    draw_model_settings(ready);
    draw_models_folder(ready);
    ImGui::BeginDisabled(!ready || local_request || player->playing());
    if (ImGui::BeginChild("voice-settings", {}, card_flags)) {
        window->section_heading(ui.text("voice.title"));
        if (catalog_cache.is_object()) {
            ImGui::BeginDisabled(model != "phonon-40m-multilingual");
            if (ImGui::BeginCombo(ui.label("voice.language"), ui.text(selected_language().name))) {
                for (const auto& item : languages) {
                    if (ImGui::Selectable(ui.label(item.name), language == item.code) && language != item.code) {
                        language = item.code;
                        voice = "default";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            std::string current = ui.text("voice.default");
            for (const auto& entry : catalog_cache.at("voices"))
                if (entry.value("voice_id", "") == voice) current = entry.value("name", "");
            if (ImGui::BeginCombo(ui.label("voice.selection"), current.c_str())) {
                ImGui::InputTextWithHint("##voice-search", ui.text("voice.search"), &search);
                if (ImGui::Selectable(ui.label("voice.default"), voice == "default")) voice = "default";
                auto folded = [](std::string value) {
                    for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                    return value;
                };
                const auto query = folded(search);
                for (const auto& entry : catalog_cache.at("voices")) {
                    if (entry.value("language", "") != language) continue;
                    const auto name = entry.value("name", "") + " (" + entry.value("locale", "") + ")";
                    if (folded(name).find(query) == std::string::npos) continue;
                    const auto id = entry.value("voice_id", "");
                    if (ImGui::Selectable((name + "###" + id).c_str(), id == voice)) voice = id;
                }
                ImGui::EndCombo();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    if (ImGui::BeginChild("voice-preview", {}, card_flags)) {
        window->section_heading(ui.text("voice.preview"));
        ImGui::TextWrapped("%s", selected_language().sample);
        draw_listen(LocalAction::Voice, ready);
    }
    ImGui::EndChild();

    if (ImGui::BeginChild("radio-settings", {}, card_flags)) {
        window->section_heading(ui.text("radio.default"));
        ImGui::BeginDisabled(!ready || local_request);
        if (ImGui::RadioButton(ui.label("radio.sera"), !faa)) faa = false;
        ImGui::SameLine();
        if (ImGui::RadioButton(ui.label("radio.faa"), faa)) faa = true;
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", ui.text("radio.hint"));
        const auto error = service->preferences_error();
        if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    }
    ImGui::EndChild();
    auto updated = settings();
    if (profile_loaded && updated != original) {
        if (updated["model_unload"] != original["model_unload"] ||
            updated["model_idle_minutes"] != original["model_idle_minutes"])
            apply_unload_policy();
        if (updated["model_load"] != original["model_load"] || updated["model"] != original["model"] ||
            updated["language"] != original["language"] || updated["voice"] != original["voice"])
            schedule_preload();
        prepared.clear();
        info.clear();
        voice_info.clear();
        service->save_profile(std::move(updated));
    }
}

void Provider::draw_test(bool ready) {
    if (ImGui::BeginChild("pronunciation-card", {}, card_flags)) {
        window->section_heading(ui.text("test.title"));
        ImGui::BeginDisabled(local_request != 0);
        const char* profiles[] = {faa ? ui.text("radio.inherit_faa") : ui.text("radio.inherit_sera"),
                                  ui.text("radio.sera"), ui.text("radio.faa")};
        if (ImGui::Combo(ui.label("test.profile"), &test_profile, profiles, IM_ARRAYSIZE(profiles))) {
            prepared.clear();
            info.clear();
        }
        ImGui::TextUnformatted(ui.text("test.sample"));
        if (ImGui::InputTextMultiline("##sample", &text, {-1, 92})) {
            prepared.clear();
            info.clear();
        }
        ImGui::EndDisabled();
        ImGui::TextUnformatted(ui.text("test.prepared"));
        if (prepared.empty()) ImGui::TextDisabled("%s", ui.text("test.hint"));
        else ImGui::TextWrapped("%s", prepared.c_str());
        ImGui::BeginDisabled(!ready || local_request || player->playing());
        if (ImGui::Button(ui.label("test.prepare"))) start_local(LocalAction::Pronunciation);
        ImGui::EndDisabled();
        ImGui::SameLine();
        draw_listen(LocalAction::Listen, ready);
    }
    ImGui::EndChild();
}

void Provider::draw_activity() {
    window->section_heading(ui.text("activity.title"));
    const char* modes[] = {ui.text("activity.all"), ui.text("activity.stream"), ui.text("activity.complete")};
    ImGui::Combo(ui.label("activity.mode"), &activity_filter, modes, IM_ARRAYSIZE(modes));
    bool visible = false;
    for (const auto& entry : activity) {
        const auto& row = entry.second;
        const bool stream = row.delivery == XSG_STREAM;
        if ((activity_filter == 1 && !stream) || (activity_filter == 2 && stream)) continue;
        visible = true;
        const auto id = std::to_string(entry.first);
        ImGui::PushID(id.c_str());
        const char* status = row.status == XSG_OK ? ui.text("activity.completed") :
                             row.chunks && row.status != XSG_CANCELLED ? ui.text("activity.interrupted") : ui.text(status_key(row.status));
        if (ImGui::BeginChild("request", {}, card_flags)) {
            const bool open = ImGui::TreeNodeEx("detail", ImGuiTreeNodeFlags_SpanAvailWidth,
                "%s  ·  %s", row.name.c_str(), status);
            ImGui::TextDisabled("%s  ·  %s", stream ? ui.text("activity.stream") : ui.text("activity.complete"), row.time.c_str());
            if (open) {
                const auto timing = [](const char* label, double value) {
                    ImGui::TextDisabled("%s", label);
                    if (value >= 0.) ImGui::Text("%.0f ms", value);
                    else ImGui::TextUnformatted("—");
                };
                if (ImGui::BeginTable("timings", 3)) {
                    ImGui::TableNextColumn();
                    timing(ui.text("activity.queue"), row.queue_ms);
                    ImGui::TableNextColumn();
                    timing(ui.text("activity.first"), row.first_ms);
                    ImGui::TableNextColumn();
                    timing(ui.text("activity.total"), row.total_ms);
                    ImGui::EndTable();
                }
                if (row.chunks) {
                    char seconds[32]{};
                    std::snprintf(seconds, sizeof(seconds), "%.2f", double(row.samples) / XSG_SAMPLE_RATE);
                    const auto summary = ui.format(stream ? "activity.blocks" : "activity.buffer",
                        {{"count", std::to_string(row.chunks)}, {"seconds", seconds}});
                    ImGui::TextWrapped("%s", summary.c_str());
                }
                if (row.status != XSG_OK)
                    ImGui::TextWrapped("%s", row.detail.empty() ? ui.text(status_key(row.status)) : row.detail.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    if (!visible) ImGui::TextDisabled("%s", ui.text("activity.empty"));
    else ImGui::TextWrapped("%s", ui.text("activity.hint"));
}

void Provider::draw() {
    load_profile();
    const bool ready = profile_loaded && service->ready();
    ImGui::TextUnformatted(ready ? (service->busy() ? ui.text("engine.busy") : ui.text("engine.ready")) : ui.text("engine.starting"));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.164f, 0.176f, 0.199f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.16f, 0.176f, 0.199f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Border, {0.25f, 0.27f, 0.30f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.207f, 0.226f, 0.267f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Button, {0.205f, 0.256f, 0.330f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.23f, 0.45f, 0.68f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_Tab, {0.165f, 0.187f, 0.225f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_TabSelected, {0.21f, 0.32f, 0.48f, 1.f});
    if (ImGui::BeginTabBar("settings-tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (ImGui::BeginTabItem(ui.label("menu.settings"))) {
            if (ImGui::BeginChild("settings-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
                draw_settings(ready);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui.label("tab.test"))) {
            if (ImGui::BeginChild("test-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
                draw_test(ready);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui.label("tab.activity"))) {
            if (ImGui::BeginChild("activity-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
                draw_activity();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
}

}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    metadata(name, signature, description, "X-Speech Gen", XSG_PROVIDER_SIGNATURE, "Offline text to speech for X-Plane plugins");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    return 1;
}
PLUGIN_API void XPluginStop() {
    auto instance = std::exchange(provider, {});
    if (instance) instance->disable();
}
PLUGIN_API int XPluginEnable() {
    try {
        provider = std::make_shared<Provider>();
        provider->enable();
        return 1;
    } catch (const std::exception&) {
        if (provider) provider->disable();
        provider.reset();
        XPLMDebugString("X-Speech Gen: enable failed\n");
        return 0;
    }
}
PLUGIN_API void XPluginDisable() { XPluginStop(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void* parameter) {
    if (!provider || message != XSG_MESSAGE_REQUEST || !parameter) return;
    // Inspect the stable header before interpreting the payload as this ABI's XsgRequest.
    std::uint32_t header[2];
    std::memcpy(header, parameter, sizeof(header));
    const auto [version, size] = header;
    constexpr auto status_offset = offsetof(XsgRequest, status);
    if (size < status_offset + sizeof(std::uint32_t)) return;
    if (version != XSG_ABI_VERSION || size < sizeof(XsgRequest)) {
        const std::uint32_t status = XSG_BAD_ABI;
        std::memcpy(static_cast<char*>(parameter) + status_offset, &status, sizeof(status));
        return;
    }
    try { provider->request(sender, *static_cast<XsgRequest*>(parameter)); }
    catch (const std::exception&) { XPLMDebugString("X-Speech Gen: request failed\n"); }
}
