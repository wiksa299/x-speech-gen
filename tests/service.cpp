#include "service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

using namespace xsg;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

void test_models_folder(const fs::path& engine, const fs::path& model_assets) {
    const auto scratch = fs::temp_directory_path() / ("xsg-models-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(scratch), "Could not create folder fixture");
    const auto preferences = scratch / "preferences.json";
    std::ofstream(scratch / "voices.jsonl") << "incomplete entry";
    const auto wait_for = [](auto predicate, const char* message) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        require(predicate(), message);
    };
    {
        Service service(engine, scratch, preferences, 150ms);
        wait_for([&] { return service.ready(); }, "Folder fixture did not start");
        require(service.models_folder().path == scratch.u8string(), "Wrong initial models folder");
        auto change = [&](ModelsAction action, const fs::path& path = {}) {
            require(service.configure_models(action, path.u8string()) == XSG_OK, "Folder change rejected");
            wait_for([&] { return !service.models_folder().pending; }, "Folder change did not finish");
        };
        require(!service.models_folder().error.empty(), "Unreadable catalog did not report an error");
        fs::remove(scratch / "voices.jsonl");
        change(ModelsAction::Apply, scratch / "missing");
        require(service.models_folder().path == scratch.u8string() &&
            !service.models_folder().error.empty(), "Invalid folder replaced the active folder");
        change(ModelsAction::Apply, model_assets);
        require(service.models_folder().path == model_assets.u8string() &&
            service.models_folder().error.empty(), "Valid folder not applied");
        const auto previous = service.catalog_revision();
        change(ModelsAction::Rescan);
        require(service.catalog_revision() > previous, "Rescan did not refresh the catalog");
        service.save_profile({{"radio_profile","faa"}});
        wait_for([&] {
            std::ifstream file(preferences);
            const auto saved = Json::parse(file, nullptr, false);
            return saved.is_object() && saved.value("radio_profile", "") == "faa" &&
                saved.value("model_root", "") == model_assets.u8string();
        }, "Saving preferences lost the models folder");
        const auto revision_before_sleep = service.catalog_revision();
        std::this_thread::sleep_for(500ms);
        require(service.ready() && service.catalog_revision() == revision_before_sleep, "Idle catalog became unavailable");
        {
            std::ifstream file(preferences);
            auto saved = Json::parse(file);
            file.close();
            saved["radio_profile"] = "sera";
            std::ofstream(preferences) << saved;
        }
        std::uint64_t id = 0;
        require(service.submit(-1, "{frequency:118.050}", {{"model","phonon-31m-en"}}, XSG_COMPLETE, id, true) == XSG_OK,
            "Model in the new folder cannot be selected");
        bool previewed = false;
        wait_for([&] {
            for (const auto& event : service.drain()) {
                require(event.id == id && event.kind == preview_event && event.status == XSG_OK,
                    "New folder did not serve the request");
                previewed = true;
            }
            return previewed;
        }, "No response from the new models folder");
        require(service.profile().value("radio_profile", "") == "sera", "Worker did not reload preferences on wake");
        require(service.catalog_revision() == revision_before_sleep, "Wake needlessly invalidated the cached catalog");
    }
    {
        Service service(engine, scratch, preferences);
        wait_for([&] { return service.ready(); }, "Folder fixture did not restart");
        require(service.models_folder().path == model_assets.u8string(), "Models folder lost after restart");
        require(service.configure_models(ModelsAction::Default) == XSG_OK, "Default folder rejected");
        wait_for([&] { return !service.models_folder().pending; }, "Default folder reset did not finish");
        require(service.models_folder().path == scratch.u8string(), "Wrong reset folder");
        std::ifstream file(preferences);
        const auto saved = Json::parse(file);
        require(!saved.contains("model_root") && saved.value("radio_profile", "") == "sera",
            "Resetting the folder lost other preferences");
    }
    fs::remove_all(scratch);
}

int main(int argc, char** argv) {
    require(argc == 3, "Expected engine and model root");
    Service service(fs::u8path(argv[1]), fs::u8path(argv[2]));
    const auto startup = std::chrono::steady_clock::now() + 15s;
    while (!service.ready() && std::chrono::steady_clock::now() < startup) std::this_thread::sleep_for(5ms);
    require(service.ready(), "Worker did not become ready");
    const auto catalog = service.catalog();
    require(catalog.at("models").size() == 3, "Wrong model catalog");
    require(catalog.at("radio_profiles") == Json::array({"sera","faa"}), "Missing radio profiles");
    require(catalog.at("aviation_tags") == 2, "Spelling capability missing");
    std::uint64_t unsupported = 0;
    require(service.submit(1, "hello", {{"model","phonon-31m-en"},{"guidance",2.}}, XSG_COMPLETE, unsupported) == XSG_INCOMPATIBLE,
            "Unsupported guidance was accepted");

    std::uint64_t streamed = 0, completed = 0, cancelled = 0;
    const std::string words = "Runway {runway:09L}, heading {heading:270}.";
    const Json options = {{"model","phonon-31m-en"},{"temperature",0.3},{"seed",42}};
    require(service.submit(10, words, options, XSG_STREAM, streamed) == XSG_OK, "Stream not accepted");
    require(service.submit(20, words, options, XSG_COMPLETE, completed) == XSG_OK, "Complete not accepted");
    require(service.submit(20, words, options, XSG_STREAM, cancelled) == XSG_OK, "Cancellation request not accepted");
    require(service.cancel(10, cancelled) == XSG_NOT_FOUND, "Cancellation from wrong owner");
    require(service.cancel(20, cancelled) == XSG_OK, "Cancellation failed");
    require(service.submit(40, words, {{"model","phonon-31m-en"},{"language","fr"}}, XSG_STREAM, cancelled) == XSG_INCOMPATIBLE,
            "French request accepted on the 40M English preset");

    std::map<std::uint64_t, std::vector<float>> pcm;
    std::map<std::uint64_t, int> chunks, statuses;
    std::map<std::uint64_t, double> queue_ms;
    const auto timeout = std::chrono::steady_clock::now() + 90s;
    while (statuses.size() < 3 && std::chrono::steady_clock::now() < timeout) {
        for (auto& event : service.drain()) {
            require(event.client == (event.id == streamed ? 10 : 20), "Foreign event received");
            if (event.kind == XSG_AUDIO) {
                auto& audio = pcm[event.id];
                require(!statuses.count(event.id) && audio.size() == event.offset, "Audio out of order");
                audio.insert(audio.end(), event.samples.begin(), event.samples.end());
                ++chunks[event.id];
            } else if (event.kind == XSG_DONE) {
                require(statuses.emplace(event.id, event.status).second, "Duplicate result");
                queue_ms.emplace(event.id, event.queue_ms);
                if (event.status != XSG_OK && event.status != XSG_CANCELLED) std::cerr << event.detail << '\n';
            }
        }
        std::this_thread::sleep_for(4ms);
    }
    require(statuses.size() == 3 && statuses.at(streamed) == XSG_OK &&
        statuses.at(completed) == XSG_OK && statuses.at(cancelled) == XSG_CANCELLED, "Wrong terminal results");
    require(chunks.at(streamed) > 1 && chunks.at(completed) == 1 && !chunks.count(cancelled), "Wrong chunk counts");
    require(queue_ms.at(streamed) >= 0. && queue_ms.at(completed) > queue_ms.at(streamed) &&
        queue_ms.at(cancelled) >= 0., "Missing or invalid queue timings");
    require(pcm.at(streamed).size() == pcm.at(completed).size(), "PCM lengths differ");
    double error = 0., energy = 0.;
    for (std::size_t index = 0; index < pcm.at(streamed).size(); ++index) {
        const auto reference = double(pcm.at(streamed)[index]);
        const auto delta = reference - double(pcm.at(completed)[index]);
        error += delta * delta;
        energy += reference * reference;
    }
    std::cout << "Relative PCM squared error: " << error / energy << '\n';
    require(energy > 0. && error <= energy * 0.0001, "PCM modes are not equivalent");
    std::string long_text;
    for (int i = 0; i < 16; ++i) long_text += "Hold short runway zero nine left. ";
    std::uint64_t interrupted = 0;
    require(service.submit(10, long_text, options, XSG_STREAM, interrupted) == XSG_OK,
        "Long request not accepted");
    bool terminal = false, heard = false;
    const auto cancel_deadline = std::chrono::steady_clock::now() + 10s;
    while (!terminal && std::chrono::steady_clock::now() < cancel_deadline) {
        for (const auto& event : service.drain()) {
            require(event.id == interrupted && event.client == 10 && !terminal, "Unexpected interrupted event");
            if (event.kind == XSG_AUDIO && !heard) {
                heard = true;
                require(service.cancel(10, interrupted) == XSG_OK, "Active cancellation failed");
            } else if (event.kind == XSG_DONE) {
                require(event.status == XSG_CANCELLED && event.queue_ms >= 0., "Active request did not cancel");
                terminal = true;
            }
        }
        std::this_thread::sleep_for(4ms);
    }
    require(heard && terminal, "Active request never reached a cancelled terminal state");
    std::cout << "Two clients, queued and active cancellation, and PCM equivalence verified\n";

    const std::string tagged = "{speed:300}";
    XsgRequest radio{};
    radio.version = XSG_ABI_VERSION;
    radio.struct_size = sizeof(radio);
    radio.operation = XSG_SPEAK;
    radio.text = {tagged.data(), std::uint32_t(tagged.size())};
    radio.radio_profile = 99;
    require(service.request(-1, radio, options) == XSG_BAD_REQUEST, "Invalid radio profile accepted");

    auto faa_default = options;
    faa_default["radio_profile"] = "faa";
    std::map<std::uint64_t, std::string> expected;
    auto submit_radio = [&](const Json& defaults, const char* prepared) {
        require(service.request(-1, radio, defaults) == XSG_OK, "Radio request rejected");
        expected.emplace(radio.request_id, prepared);
    };
    radio.struct_size = offsetof(XsgRequest, radio_profile);
    require(service.request(-1, radio, faa_default) == XSG_BAD_ABI, "Short radio request accepted");
    radio.struct_size = sizeof(radio);
    radio.radio_profile = XSG_DEFAULT_RADIO_PROFILE;
    submit_radio(options, "tree hundred");
    submit_radio(faa_default, "tree zero zero");
    radio.radio_profile = XSG_SERA;
    submit_radio(faa_default, "tree hundred");
    radio.radio_profile = XSG_FAA;
    submit_radio(options, "tree zero zero");

    const XsgAnnotation heading{0, 3, XSG_HEADING};
    radio.text = {"150", 3};
    radio.annotations = &heading;
    radio.annotation_count = 1;
    submit_radio(options, "heading one fife zero");

    const XsgAnnotation letters{0, 3, XSG_SPELL};
    radio.text = {"ILS", 3};
    radio.annotations = &letters;
    submit_radio(options, "eye el ess");

    const auto radio_count = expected.size();
    unsigned radio_done = 0;
    const auto radio_deadline = std::chrono::steady_clock::now() + 30s;
    while (radio_done < radio_count && std::chrono::steady_clock::now() < radio_deadline) {
        for (const auto& event : service.drain()) {
            if (event.kind == prepared_event) {
                require(event.detail == expected.at(event.id), "Wrong effective radio profile");
                expected.erase(event.id);
            } else if (event.kind == XSG_DONE) {
                require(event.status == XSG_OK, "Radio synthesis failed");
                ++radio_done;
            }
        }
        std::this_thread::sleep_for(4ms);
    }
    require(expected.empty() && radio_done == radio_count, "Radio requests did not terminate");

    const auto check_diagnostic = [&](const char* detail) {
        std::uint64_t id = 0;
        require(service.submit(-1, "{unknown:1}", options, XSG_COMPLETE, id, true) == XSG_OK,
            "Localized preview was not accepted");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto events = service.drain();
            if (!events.empty()) {
                require(events.size() == 1 && events.front().id == id && events.front().detail == detail,
                    "Worker did not apply the latest translation catalog");
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        require(false, "No worker diagnostic after changing the interface language");
    };
    service.set_ui_strings({{"error.tag_unknown", "Balise aéronautique inconnue : {tag}"}});
    check_diagnostic("Balise aéronautique inconnue : unknown");
    service.set_ui_strings(Json::object());
    check_diagnostic("Unknown aviation tag: unknown");

    const auto wait_model = [&](ModelState state, const char* failure) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto model = service.model_status();
            require(model.error.empty(), model.error.c_str());
            if (model.state == state) return;
            std::this_thread::sleep_for(5ms);
        }
        require(false, failure);
    };
    service.set_idle_timeout(0ms);
    wait_model(ModelState::Unloaded, "The current model was retained after switching to after each use");
    service.schedule_preload(options);
    wait_model(ModelState::Ready, "Activation preload did not load a model and voice");
    std::this_thread::sleep_for(150ms);
    require(service.model_status().state == ModelState::Ready && service.drain().empty(),
        "Preloading sent audio or unloaded the model before its first use");

    const auto check_model_queue = [&](int count, bool cold) {
        for (int i = 0; i < count; ++i) {
            std::uint64_t id = 0;
            require(service.submit(1, "The cabin is ready.", options, XSG_COMPLETE, id) == XSG_OK,
                "Synthesis was rejected during model lifetime test");
        }
        int done = 0;
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (done < count && std::chrono::steady_clock::now() < deadline) {
            for (const auto& event : service.drain()) {
                if (event.kind != XSG_DONE) continue;
                require(event.status == XSG_OK && event.stats.at("cold") == (done == 0 && cold),
                    "Unexpected model residency across queued requests");
                ++done;
            }
            std::this_thread::sleep_for(5ms);
        }
        require(done == count, "Model lifetime requests did not terminate");
    };
    check_model_queue(2, false);
    wait_model(ModelState::Unloaded, "Model was not unloaded after the synthesis queue drained");
    require(service.ready(), "Unloading lost the catalog");
    service.set_idle_timeout(std::nullopt);
    check_model_queue(1, true);
    std::this_thread::sleep_for(200ms);
    require(service.model_status().state == ModelState::Ready, "Model was unloaded in deactivation mode");
    service.set_idle_timeout(100ms);
    wait_model(ModelState::Unloaded, "Model was not unloaded after inactivity");

    test_models_folder(fs::u8path(argv[1]), fs::absolute(fs::u8path(argv[2])));
}
