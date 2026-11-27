#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace xsg {
class Player {
public:
    Player();
    ~Player();
    void begin();
    bool append(const float* samples, std::size_t count);
    void finish(bool success);
    bool playing() const;
    std::string error() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
