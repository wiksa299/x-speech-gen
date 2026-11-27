#include "player.hpp"
#include "xspeech_gen.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#include <miniaudio.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace xsg {
struct Player::State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    static constexpr std::size_t capacity = XSG_SAMPLE_RATE * 60;
    std::unique_ptr<float[]> pcm;
    std::size_t read = 0;
    std::size_t written = 0;
    bool playing = false;
    bool terminal = false;
    bool stopping = false;
    std::string failure;
    std::thread worker;

    static void audio(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto& state = *static_cast<State*>(device->pUserData);
        std::lock_guard lock(state.mutex);
        auto* dst = static_cast<float*>(output);
        const auto count = std::min<std::size_t>(state.written - state.read, frames);
        std::copy_n(state.pcm.get() + state.read, count, dst);
        std::fill(dst + count, dst + frames, 0.f);
        state.read += count;
        if (state.terminal && state.read == state.written) {
            state.playing = false;
            state.changed.notify_one();
        }
    }

    void run() {
        bool opened = false;
        ma_device device{};
        std::unique_lock lock(mutex);
        while (!stopping) {
            changed.wait(lock, [&] { return stopping || (playing && written != read) || (opened && !playing); });
            if (stopping) break;
            if (opened && !playing) {
                lock.unlock();
                ma_device_uninit(&device);
                lock.lock();
                opened = false;
            } else if (!opened) {
                auto config = ma_device_config_init(ma_device_type_playback);
                config.playback.format = ma_format_f32;
                config.playback.channels = 1;
                config.sampleRate = XSG_SAMPLE_RATE;
                config.pUserData = this;
                config.dataCallback = &State::audio;
                lock.unlock();
                auto result = ma_device_init(nullptr, &config, &device);
                if (result == MA_SUCCESS) {
                    result = ma_device_start(&device);
                    if (result != MA_SUCCESS) ma_device_uninit(&device);
                }
                lock.lock();
                opened = result == MA_SUCCESS;
                if (!opened) {
                    failure = "error.audio";
                    playing = false;
                }
            } else {
                changed.wait(lock, [&] { return stopping || !playing; });
            }
        }
        lock.unlock();
        if (opened) ma_device_uninit(&device);
    }
};

Player::Player() : state_(std::make_unique<State>()) {
    state_->worker = std::thread([state = state_.get()] { state->run(); });
}
Player::~Player() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->changed.notify_one();
    state_->worker.join();
}
void Player::begin() {
    std::lock_guard lock(state_->mutex);
    // Only the prefix filled by append() is ever read by the audio device.
    if (!state_->pcm) state_->pcm.reset(new float[State::capacity]);
    state_->read = state_->written = 0;
    state_->terminal = false;
    state_->playing = true;
    state_->failure.clear();
    state_->changed.notify_one();
}
bool Player::append(const float* samples, std::size_t count) {
    std::lock_guard lock(state_->mutex);
    if (!state_->playing || count > State::capacity - state_->written) return false;
    std::copy_n(samples, count, state_->pcm.get() + state_->written);
    state_->written += count;
    state_->changed.notify_one();
    return true;
}
void Player::finish(bool success) {
    std::lock_guard lock(state_->mutex);
    state_->terminal = true;
    if (!success || state_->read == state_->written) {
        state_->read = state_->written;
        state_->playing = false;
    }
    state_->changed.notify_one();
}
std::string Player::error() const {
    std::lock_guard lock(state_->mutex);
    return state_->failure;
}
bool Player::playing() const {
    std::lock_guard lock(state_->mutex);
    return state_->playing;
}
}
