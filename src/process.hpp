#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace xsg {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Frame {
    Json message;
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    std::vector<float> samples;
};

class Process {
public:
    Process(fs::path executable, std::function<void()> on_input);
    ~Process();
    bool start();
    bool send(const Json& message);
    std::vector<Frame> receive();
    bool failed() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
