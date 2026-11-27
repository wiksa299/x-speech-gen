#include "service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace xsg {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int protocol_version = 2;

struct Job {
    int client;
    std::uint64_t id;
    std::uint32_t delivery;
    Json message;
    bool preview;
    Clock::time_point accepted;
    std::optional<Clock::time_point> started;

    Event terminal(std::uint32_t status, std::string detail = {}, Json stats = {}) const {
        const auto queue_ms = std::chrono::duration<double, std::milli>(
            started.value_or(Clock::now()) - accepted).count();
        return {client, id, preview ? preview_event : XSG_DONE, status, 0, {},
                std::move(detail), std::move(stats), queue_ms};
    }
};

std::string bytes(XsgString string, unsigned limit) {
    if (string.bytes > limit || (string.bytes && !string.data)) throw std::invalid_argument("Invalid string");
    if (!string.bytes) return {};
    std::string value(string.data, string.bytes);
    if (value.find('\0') != std::string::npos) throw std::invalid_argument("Embedded NUL");
    return value;
}

Json request_options(const XsgRequest& request, Json result) {
    if (request.delivery > XSG_STREAM ||
        request.options & ~63u || request.rule_count > XSG_MAX_RULES ||
        request.annotation_count > XSG_MAX_RULES ||
        (request.rule_count && !request.rules) || (request.annotation_count && !request.annotations)) {
        throw std::invalid_argument("Invalid options");
    }
    auto add_string = [&](const char* key, XsgString field, unsigned limit) {
        auto value = bytes(field, limit);
        if (!value.empty()) result[key] = std::move(value);
    };
    if (request.model.bytes || request.language.bytes) result.erase("voice");
    if (request.model.bytes) result.erase("language");
    if (request.voice.bytes && !request.language.bytes) result.erase("language");
    add_string("model", request.model, 48);
    add_string("language", request.language, 8);
    add_string("voice", request.voice, 48);
    switch (request.radio_profile) {
    case XSG_DEFAULT_RADIO_PROFILE: break;
    case XSG_SERA: result["radio_profile"] = "sera"; break;
    case XSG_FAA: result["radio_profile"] = "faa"; break;
    default: throw std::invalid_argument("Invalid radio profile");
    }
    if (request.options & XSG_SEED) result["seed"] = request.seed;
    auto add_number = [&](unsigned mask, const char* name, float value, float low, float high) {
        if (!(request.options & mask)) return;
        if (!std::isfinite(value) || value < low || value > high) throw std::invalid_argument("Invalid number");
        result[name] = value;
    };
    add_number(XSG_TEMPERATURE, "temperature", request.temperature, 0.f, 2.f);
    add_number(XSG_GUIDANCE, "guidance", request.guidance, 1.f, 5.f);
    add_number(XSG_DURATION_DELTA, "duration_delta", request.duration_delta, -.5f, .5f);
    add_number(XSG_GAIN_DB, "gain_db", request.gain_db, -24.f, 12.f);
    if (request.options & XSG_MAX_TOKENS) {
        if (request.max_tokens_per_chunk < 1 || request.max_tokens_per_chunk > 200) throw std::invalid_argument("Invalid token limit");
        result["max_tokens_per_chunk"] = request.max_tokens_per_chunk;
    }
    if (request.rule_count) {
        auto rules = Json::array();
        for (unsigned i = 0; i < request.rule_count; ++i) {
            const auto& rule = request.rules[i];
            auto value = Json{{"expression", bytes(rule.expression, 128)}, {"spoken", bytes(rule.spoken, 256)},
                              {"case_sensitive", rule.case_sensitive != 0}};
            if (rule.language.bytes) value["language"] = bytes(rule.language, 8);
            rules.push_back(std::move(value));
        }
        result["lexicon"] = std::move(rules);
    }
    if (request.annotation_count) {
        constexpr const char* kinds[] = {"", "runway", "heading", "flight_level", "altitude", "speed",
            "wind", "frequency", "qnh", "squawk", "registration", "call_sign",
            "number", "digits", "decimal", "icao", "visibility", "rvr",
            "cloud_height", "clock", "time", "radio_number", "spell"};
        auto annotations = Json::array();
        for (unsigned i = 0; i < request.annotation_count; ++i) {
            const auto& annotation = request.annotations[i];
            if (!annotation.kind || annotation.kind >= std::size(kinds)) throw std::invalid_argument("Invalid annotation kind");
            annotations.push_back({{"start", annotation.start}, {"end", annotation.end}, {"kind", kinds[annotation.kind]}});
        }
        result["annotations"] = std::move(annotations);
    }
    return result;
}
}

struct Service::Impl {
    Impl(fs::path path, fs::path model_root, fs::path settings, std::chrono::milliseconds idle, Json strings)
        : engine(std::move(path), [this] {
              std::lock_guard lock(mutex);
              input_changed = true;
              changed.notify_one();
          }), root(std::move(model_root)), preferences(std::move(settings)), idle_timeout(idle), ui_strings(std::move(strings)) {}
    Process engine;
    fs::path root;
    fs::path preferences;
    std::optional<std::chrono::milliseconds> idle_timeout;
    Json ui_strings;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::vector<Event> events;
    std::optional<std::pair<int, std::uint64_t>> active;
    Json catalog;
    Json profile;
    Json pending_profile;
    Json pending_models;
    Json pending_preload;
    std::string loaded_model;
    std::string preload_error;
    bool model_loading = false;
    bool preload_in_flight = false;
    bool profile_in_flight = false;
    std::string preferences_error;
    ModelsFolder models;
    std::uint64_t catalog_revision = 0;
    std::uint64_t next_id = 1;
    std::uint64_t cancellation = 0;
    bool running = false; // The catalog can serve requests while the helper sleeps.
    bool input_changed = false;
    bool locale_changed = false;
    bool stopping = false;
    std::thread worker;

    bool emit(Event event) {
        std::lock_guard lock(mutex);
        if (event.kind == XSG_AUDIO && events.size() >= 1024) return false;
        events.push_back(std::move(event));
        return true;
    }

    void run();
    void reset(std::uint32_t status, const std::optional<Job>& current) {
        std::lock_guard lock(mutex);
        running = false;
        loaded_model.clear();
        model_loading = false;
        if (preload_in_flight)
            preload_error = ui_strings.value("status.engine_lost", "Engine disconnected");
        if (profile_in_flight)
            preferences_error = ui_strings.value("error.preferences", "Preferences could not be saved");
        preload_in_flight = false;
        profile_in_flight = false;
        if (current) events.push_back(current->terminal(status));
        for (const auto& job : jobs) events.push_back(job.terminal(XSG_ENGINE_LOST));
        jobs.clear();
        active.reset();
        cancellation = 0;
        if (models.pending) {
            pending_models = nullptr;
            models.pending = false;
            models.error = status == XSG_TIMEOUT ? ui_strings.value("error.folder_timeout", "Models folder operation timed out") : ui_strings.value("status.engine_lost", "Engine disconnected");
        }
    }
};

void Service::Impl::run() {
    std::optional<Job> current;
    std::size_t offset = 0;
    std::vector<float> complete;
    auto deadline = Clock::now();
    auto idle_since = deadline;
    bool awaiting_first_synthesis = false;
    const auto can_dispatch = [&] {
        return !current && !preload_in_flight && !profile_in_flight &&
            (!models.pending || pending_models.is_object());
    };
    const auto has_work = [&] {
        return !jobs.empty() || pending_profile.is_object() || pending_models.is_object() || pending_preload.is_object();
    };
    const auto idle_at = [&] {
        if (!idle_timeout || (*idle_timeout == 0ms && awaiting_first_synthesis))
            return Clock::time_point::max();
        return idle_since + *idle_timeout;
    };
    std::uint64_t sent_cancel = 0;
    enum class Phase { Launch, Starting, Ready, Sleeping };
    auto phase = Phase::Launch;
    while (true) {
        {
            std::unique_lock lock(mutex);
            const auto wake = [&] {
                return stopping || input_changed || phase == Phase::Launch ||
                    (phase == Phase::Sleeping && has_work()) ||
                    (phase == Phase::Ready && can_dispatch() && (has_work() || locale_changed)) ||
                    (cancellation && cancellation != sent_cancel);
            };
            const auto wait_until = phase == Phase::Sleeping ? Clock::time_point::max() :
                phase == Phase::Ready && can_dispatch() ? idle_at() : deadline;
            if (wait_until == Clock::time_point::max()) changed.wait(lock, wake);
            else changed.wait_until(lock, wait_until, wake);
            if (stopping) break;
            input_changed = false;
            if (phase == Phase::Sleeping) {
                if (!has_work()) continue;
                phase = Phase::Launch;
            }
            if (phase == Phase::Ready && can_dispatch() && !has_work() && !locale_changed &&
                !engine.failed() && Clock::now() >= idle_at()) {
                phase = Phase::Sleeping;
                loaded_model.clear();
                lock.unlock();
                std::vector<float>().swap(complete);
                engine.stop();
                continue;
            }
        }
        if (phase != Phase::Launch && engine.failed()) {
            reset(XSG_ENGINE_LOST, current);
            current.reset();
            complete.clear();
            sent_cancel = 0;
            phase = Phase::Launch;
            std::unique_lock lock(mutex);
            if (changed.wait_for(lock, 1s, [&] { return stopping; })) break;
        }
        if (phase == Phase::Launch) {
            Json init{{"type","init"}, {"root",root.u8string()}, {"protocol",protocol_version}};
            {
                std::lock_guard lock(mutex);
                if (!ui_strings.empty()) init["ui_strings"] = ui_strings;
                locale_changed = false;
            }
            if (!preferences.empty()) init["preferences"] = preferences.u8string();
            else if (!models.path.empty() && models.path != root.u8string()) init["active_root"] = models.path;
            if (!engine.start() || !engine.send(init)) {
                reset(XSG_ENGINE_LOST, current);
                std::unique_lock lock(mutex);
                if (changed.wait_for(lock, 1s, [&] { return stopping; })) break;
                continue;
            }
            phase = Phase::Starting;
            deadline = Clock::now() + 10s;
        }

        std::uint64_t cancel = 0;
        {
            std::unique_lock lock(mutex);
            if (phase == Phase::Ready && can_dispatch()) {
                bool sent = true;
                if (locale_changed) {
                    sent = engine.send({{"type", "locale"}, {"ui_strings", ui_strings}});
                    if (sent) locale_changed = false;
                }
                if (sent && pending_profile.is_object()) {
                    sent = engine.send({{"type", "profile"}, {"profile", pending_profile}});
                    if (sent) {
                        pending_profile = nullptr;
                        profile_in_flight = true;
                        deadline = Clock::now() + 10s;
                    }
                } else if (sent && pending_models.is_object()) {
                    sent = engine.send(pending_models);
                    if (sent) {
                        deadline = Clock::now() + (pending_models["action"] == "browse" ? 10min : 15s);
                        pending_models = nullptr;
                    }
                } else if (sent && pending_preload.is_object()) {
                    sent = engine.send({{"type", "preload"}, {"options", pending_preload}});
                    if (sent) {
                        pending_preload = nullptr;
                        preload_in_flight = true;
                        model_loading = true;
                        deadline = Clock::now() + 30s;
                    }
                } else if (sent && !jobs.empty()) {
                    current.emplace(std::move(jobs.front()));
                    jobs.pop_front();
                    active.emplace(current->client, current->id);
                    offset = 0;
                    complete.clear();
                    sent_cancel = 0;
                    current->started = Clock::now();
                    deadline = Clock::now() + 30s;
                    if (!current->preview) {
                        preload_error.clear();
                        model_loading = loaded_model != current->message["options"].value("model", "phonon-40m-en");
                    }
                    sent = engine.send(current->message);
                }
                if (!sent) {
                    lock.unlock();
                    engine.stop();
                    continue;
                }
            }
            cancel = cancellation;
        }
        if (current && cancel == current->id && cancel != sent_cancel) {
            sent_cancel = cancel;
            deadline = Clock::now() + 3s;
            if (!engine.send({{"type","cancel"}, {"id",cancel}})) deadline = Clock::now();
        }

        bool malformed = false;
        for (auto& frame : engine.receive()) {
            if (!frame.samples.empty()) {
                if (!current || frame.id != current->id || frame.offset != offset ||
                    offset + frame.samples.size() > max_samples ||
                    !std::all_of(frame.samples.begin(), frame.samples.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1.f; })) {
                    malformed = true; break;
                }
                if (!offset) {
                    std::lock_guard lock(mutex);
                    loaded_model = current->message["options"].value("model", "phonon-40m-en");
                    model_loading = false;
                    awaiting_first_synthesis = false;
                }
                offset += frame.samples.size();
                if (sent_cancel) continue;
                deadline = Clock::now() + 15s;
                if (current->delivery == XSG_COMPLETE) {
                    complete.insert(complete.end(), frame.samples.begin(), frame.samples.end());
                } else if (!emit({current->client, current->id, XSG_AUDIO, XSG_OK, frame.offset, std::move(frame.samples)})) {
                    malformed = true; break;
                }
                continue;
            }
            const auto& message = frame.message;
            const auto type = message.value("type", "");
            if (type == "ready" && message.value("protocol", 0) == protocol_version && message.contains("catalog")) {
                std::lock_guard lock(mutex);
                const auto path = message.value("model_root", root.u8string());
                if (!running || catalog != message["catalog"] || models.path != path) {
                    catalog = message["catalog"];
                    models.path = path;
                    ++catalog_revision;
                }
                if (!preferences.empty() || profile.is_null())
                    profile = message.value("profile", Json());
                models.error = message.value("model_root_error", "");
                preferences_error.clear();
                phase = Phase::Ready;
                idle_since = Clock::now();
                awaiting_first_synthesis = false;
                running = true;
                continue;
            }
            if (type == "preloaded") {
                std::lock_guard lock(mutex);
                if (!preload_in_flight) { malformed = true; break; }
                preload_in_flight = false;
                model_loading = false;
                const auto& model = message.at("loaded_model");
                loaded_model = model.is_string() ? model.get<std::string>() : std::string{};
                const auto& detail = message.at("detail");
                preload_error = detail.is_string() ? detail.get<std::string>() : std::string{};
                awaiting_first_synthesis = !loaded_model.empty() && preload_error.empty();
                idle_since = Clock::now();
                continue;
            }
            if (type == "profile" || type == "preferences_error") {
                std::lock_guard lock(mutex);
                if (!profile_in_flight) { malformed = true; break; }
                profile_in_flight = false;
                if (type == "profile") profile = message.at("profile");
                else preferences_error = message.value("detail", ui_strings.value("error.preferences", "Preferences could not be saved"));
                continue;
            }
            if (type == "models") {
                std::lock_guard lock(mutex);
                if (!models.pending) { malformed = true; break; }
                models.pending = false;
                if (message.contains("catalog")) {
                    loaded_model.clear();
                    preload_error.clear();
                    awaiting_first_synthesis = false;
                    catalog = message.at("catalog");
                    models.path = message.at("model_root").get<std::string>();
                    ++catalog_revision;
                }
                models.error = message.value("detail", "");
                continue;
            }
            if (!current || message.value("id", std::uint64_t(0)) != current->id) { malformed = true; break; }
            if (type == "prepared") {
                if (current->client < 0 && !sent_cancel)
                    emit({current->client, current->id, prepared_event, XSG_OK, 0, {}, message.value("text", "")});
                continue;
            }
            if (type != "done" && type != "preview") { malformed = true; break; }
            {
                std::lock_guard lock(mutex);
                const auto status_text = message.value("status", "");
                const auto status = cancellation == current->id || status_text == "cancelled" ? XSG_CANCELLED :
                    type == "preview" ? (message.contains("text") ? XSG_OK : XSG_ENGINE_ERROR) :
                    status_text == "ok" && offset ? XSG_OK : XSG_ENGINE_ERROR;
                if (type == "done") {
                    const auto& model = message.at("loaded_model");
                    auto actual_model = model.is_string() ? model.get<std::string>() : std::string{};
                    if (actual_model != loaded_model) awaiting_first_synthesis = false;
                    loaded_model = std::move(actual_model);
                    model_loading = false;
                }
                if (type == "done" && status == XSG_OK && current->delivery == XSG_COMPLETE)
                    events.push_back({current->client, current->id, XSG_AUDIO, XSG_OK, 0, std::move(complete)});
                events.push_back(current->terminal(status,
                    message.value(type == "preview" && status == XSG_OK ? "text" : "detail", ""),
                    message.value("stats", Json())));
                active.reset();
                cancellation = 0;
            }
            current.reset();
            sent_cancel = 0;
            idle_since = Clock::now();
        }
        bool pending;
        {
            std::lock_guard lock(mutex);
            pending = phase == Phase::Starting || !can_dispatch();
        }
        if (malformed || (pending && Clock::now() >= deadline)) {
            reset(malformed ? XSG_ENGINE_LOST : (sent_cancel ? XSG_CANCELLED : XSG_TIMEOUT), current);
            current.reset();
            complete.clear();
            engine.stop();
            phase = Phase::Launch;
            sent_cancel = 0;
        }
    }
    engine.stop();
}

Service::Service(fs::path engine, fs::path root, fs::path preferences, std::chrono::milliseconds idle_timeout, Json ui_strings)
    : impl_(std::make_unique<Impl>(std::move(engine), std::move(root), std::move(preferences), idle_timeout, std::move(ui_strings))) {
    impl_->worker = std::thread([state = impl_.get()] {
        try { state->run(); }
        catch (const std::exception&) {
            state->engine.stop();
            std::lock_guard lock(state->mutex);
            state->running = false;
            if (state->active)
                state->events.push_back({state->active->first, state->active->second, XSG_DONE, XSG_ENGINE_LOST});
            for (const auto& job : state->jobs)
                state->events.push_back(job.terminal(XSG_ENGINE_LOST));
            state->active.reset();
            state->jobs.clear();
            state->loaded_model.clear();
            state->model_loading = false;
            state->pending_preload = nullptr;
            state->pending_models = nullptr;
            state->models.pending = false;
            state->models.error = state->ui_strings.value("status.engine_lost", "Engine disconnected");
        }
    });
}
Service::~Service() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    impl_->worker.join();
}

std::uint32_t Service::request(int client, XsgRequest& request, const Json& defaults) {
    if (request.version != XSG_ABI_VERSION || request.struct_size < sizeof(XsgRequest)) return XSG_BAD_ABI;
    request.ready = ready();
    request.busy = busy();
    try {
        switch (request.operation) {
        case XSG_GET_STATUS: return XSG_OK;
        case XSG_GET_CAPABILITIES: {
            if (!request.ready) return XSG_UNAVAILABLE;
            const auto value = catalog().dump();
            request.result_bytes = std::uint32_t(value.size() + 1);
            if (!request.result || request.result_capacity < request.result_bytes) return XSG_OUTPUT_LIMIT;
            std::memcpy(request.result, value.c_str(), request.result_bytes);
            return XSG_OK;
        }
        case XSG_SPEAK:
            return submit(client, bytes(request.text, XSG_MAX_TEXT_BYTES), request_options(request, defaults),
                          request.delivery, request.request_id);
        case XSG_CANCEL: return cancel(client, request.request_id);
        default: return XSG_BAD_REQUEST;
        }
    } catch (const std::exception&) { return XSG_BAD_REQUEST; }
}

std::uint32_t Service::submit(int client, std::string text, Json options, std::uint32_t delivery,
                              std::uint64_t& id, bool preview) {
    if (text.empty() || text.size() > XSG_MAX_TEXT_BYTES || text.find_first_not_of(" \r\n\t") == std::string::npos)
        return XSG_BAD_REQUEST;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return XSG_UNAVAILABLE;
    if (impl_->models.pending) return XSG_BUSY;
    if (impl_->jobs.size() >= 8 || impl_->events.size() >= 1024) return XSG_BUSY;
    const auto model_id = options.value("model", "phonon-40m-en");
    const auto& models = impl_->catalog.at("models");
    const auto model = std::find_if(models.begin(), models.end(), [&](const Json& m) {
        return m.value("id", "") == model_id && m.value("available", false);
    });
    if (model == models.end()) return XSG_MODEL_UNAVAILABLE;
    if (options.value("guidance", 1.) != 1. && !model->value("guidance", false)) return XSG_INCOMPATIBLE;
    const auto voice_id = options.value("voice", "default");
    const auto& voices = impl_->catalog.at("voices");
    const auto voice = std::find_if(voices.begin(), voices.end(), [&](const Json& v) { return v.value("voice_id", "") == voice_id; });
    if (voice == voices.end() && voice_id != "default") return XSG_INCOMPATIBLE;
    const auto language = options.value("language", voice == voices.end() ? "en" : voice->value("language", "en"));
    const auto& languages = model->at("languages");
    if (std::find(languages.begin(), languages.end(), language) == languages.end() ||
        (voice != voices.end() && voice->value("language", "") != language)) return XSG_INCOMPATIBLE;
    auto message = Json{{"type",preview ? "preview" : "speak"}, {"id",impl_->next_id},
                        {"text",std::move(text)}, {"options",std::move(options)}};
    if (message.dump().size() >= 128 * 1024) return XSG_BAD_REQUEST;
    id = impl_->next_id++;
    impl_->jobs.push_back({client,id,delivery,std::move(message),preview,Clock::now(),{}});
    impl_->changed.notify_one();
    return XSG_OK;
}

std::uint32_t Service::cancel(int client, std::uint64_t id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->active == std::make_pair(client, id)) {
        impl_->cancellation = id;
        impl_->events.erase(std::remove_if(impl_->events.begin(), impl_->events.end(), [&](const Event& e) {
            return e.client == client && e.id == id && e.kind == XSG_AUDIO;
        }), impl_->events.end());
        impl_->changed.notify_one();
        return XSG_OK;
    }
    auto found = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
        [&](const Job& job) { return job.client == client && job.id == id; });
    if (found == impl_->jobs.end()) return XSG_NOT_FOUND;
    impl_->events.push_back(found->terminal(XSG_CANCELLED));
    impl_->jobs.erase(found);
    return XSG_OK;
}

std::vector<Event> Service::drain() {
    std::lock_guard lock(impl_->mutex);
    std::vector<Event> events;
    events.swap(impl_->events);
    return events;
}
Json Service::catalog() const { std::lock_guard lock(impl_->mutex); return impl_->catalog; }
std::uint64_t Service::catalog_revision() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->catalog_revision;
}
ModelsFolder Service::models_folder() const { std::lock_guard lock(impl_->mutex); return impl_->models; }
ModelStatus Service::model_status() const {
    std::lock_guard lock(impl_->mutex);
    const auto state = impl_->model_loading || impl_->pending_preload.is_object() ? ModelState::Loading :
        impl_->loaded_model.empty() ? ModelState::Unloaded : ModelState::Ready;
    return {state, impl_->preload_error};
}
void Service::set_idle_timeout(std::optional<std::chrono::milliseconds> timeout) {
    std::lock_guard lock(impl_->mutex);
    impl_->idle_timeout = timeout;
    impl_->input_changed = true;
    impl_->changed.notify_one();
}
void Service::schedule_preload(Json options) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_preload = std::move(options);
    impl_->preload_error.clear();
    impl_->changed.notify_one();
}
std::uint32_t Service::configure_models(ModelsAction action, std::string path) {
    if (path.size() > 8192 || path.find('\0') != std::string::npos) return XSG_BAD_REQUEST;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return XSG_UNAVAILABLE;
    if (impl_->models.pending || impl_->active || impl_->preload_in_flight || !impl_->jobs.empty()) return XSG_BUSY;
    using Action = ModelsAction;
    const char* name = action == Action::Apply ? "apply" : action == Action::Default ? "default" :
                       action == Action::Rescan ? "rescan" : "browse";
    impl_->pending_models = Json{{"type","models"},{"action",name}};
    if (action == Action::Apply) impl_->pending_models["root"] = std::move(path);
    impl_->models.pending = true;
    impl_->models.error.clear();
    impl_->changed.notify_one();
    return XSG_OK;
}
Json Service::profile() const { std::lock_guard lock(impl_->mutex); return impl_->profile; }
void Service::save_profile(Json profile) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_profile = std::move(profile);
    impl_->preferences_error.clear();
    impl_->changed.notify_one();
}
void Service::set_ui_strings(Json strings) {
    std::lock_guard lock(impl_->mutex);
    impl_->ui_strings = std::move(strings);
    impl_->locale_changed = true;
    impl_->changed.notify_one();
}
std::string Service::preferences_error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->preferences_error;
}
bool Service::ready() const { std::lock_guard lock(impl_->mutex); return impl_->running; }
bool Service::busy() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->models.pending || impl_->active.has_value() || !impl_->jobs.empty();
}
}
