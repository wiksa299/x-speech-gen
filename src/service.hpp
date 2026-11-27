#pragma once

#include "process.hpp"
#include "xspeech_gen.h"

#include <chrono>
#include <cstddef>
#include <optional>

namespace xsg {
struct Event {
    int client;
    std::uint64_t id;
    std::uint32_t kind;
    std::uint32_t status = XSG_OK;
    std::uint64_t offset = 0;
    std::vector<float> samples;
    std::string detail;
    Json stats;
    double queue_ms = -1.;
};

constexpr std::uint32_t prepared_event = 3;
constexpr std::uint32_t preview_event = 4;
constexpr std::size_t max_samples = XSG_SAMPLE_RATE * 60;

enum class ModelsAction { Apply, Default, Rescan, Browse };

struct ModelsFolder {
    std::string path;
    std::string error;
    bool pending = false;
};

enum class ModelState { Unloaded, Loading, Ready };
struct ModelStatus {
    ModelState state;
    std::string error;
};

class Service {
public:
    static constexpr auto default_idle_timeout = std::chrono::minutes(5);
    Service(fs::path engine, fs::path root, fs::path preferences = {},
            std::chrono::milliseconds idle_timeout = default_idle_timeout, Json ui_strings = Json::object());
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    std::uint32_t request(int client, XsgRequest& request, const Json& defaults);
    std::uint32_t submit(int client, std::string text, Json options, std::uint32_t delivery,
                         std::uint64_t& id, bool preview = false);
    std::uint32_t cancel(int client, std::uint64_t id);
    std::vector<Event> drain();
    Json catalog() const;
    std::uint64_t catalog_revision() const;
    ModelsFolder models_folder() const;
    ModelStatus model_status() const;
    void set_idle_timeout(std::optional<std::chrono::milliseconds> timeout);
    void schedule_preload(Json options);
    std::uint32_t configure_models(ModelsAction action, std::string path = {});
    Json profile() const;
    void save_profile(Json profile);
    void set_ui_strings(Json strings);
    std::string preferences_error() const;
    bool ready() const;
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
