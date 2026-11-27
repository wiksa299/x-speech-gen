// Process management adapted from X-Speech Rekon (MIT, copyright 2026 wiksa).
#include "process.hpp"

#include <chrono>
#include <cstring>
#include <string>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace xsg {
using namespace std::chrono_literals;
constexpr std::size_t max_frame = 128 * 1024;

static std::uint64_t little_endian(const char* bytes, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        value |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
    }
    return value;
}

static std::string wire_frame(const Json& message) {
    const auto payload = message.dump();
    const auto size = payload.size() + 1;
    if (size > max_frame) return {};
    std::string frame(5, '\0');
    for (unsigned i = 0; i < 4; ++i) frame[i] = char((size >> (i * 8)) & 255);
    frame[4] = 'J';
    frame += payload;
    return frame;
}


#ifdef _WIN32
using Pipe = HANDLE;
constexpr Pipe no_pipe = nullptr;
static void close_pipe(Pipe& pipe) {
    if (pipe) CloseHandle(std::exchange(pipe, no_pipe));
}
#else
using Pipe = int;
constexpr Pipe no_pipe = -1;
static void close_pipe(Pipe& pipe) {
    if (pipe != no_pipe) close(std::exchange(pipe, no_pipe));
}
#endif

struct Process::Impl {
    Impl(fs::path executable, std::function<void()> notify) : path(std::move(executable)), on_input(std::move(notify)) {}
    fs::path path;
    std::function<void()> on_input;
    Pipe from_engine = no_pipe;
    Pipe to_engine = no_pipe;
#ifdef _WIN32
    HANDLE child = nullptr;
    HANDLE job = nullptr;
#else
    pid_t child = -1;
#endif
    std::thread reader;
    std::thread writer;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::string> outbound;
    std::vector<Frame> inbound;
    std::atomic<bool> dead{true};

    bool spawn();
    void terminate();
    void read_messages();
    void write_messages();
    void fail() {
        {
            std::lock_guard lock(mutex);
            dead = true;
        }
        available.notify_all();
    }
};

#ifdef _WIN32
bool Process::Impl::spawn() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input = nullptr, output = nullptr, error = nullptr;
    bool success = false;
    if (CreatePipe(&input, &to_engine, &security, 0) &&
        CreatePipe(&from_engine, &output, &security, 0) &&
        SetHandleInformation(from_engine, HANDLE_FLAG_INHERIT, 0) &&
        SetHandleInformation(to_engine, HANDLE_FLAG_INHERIT, 0)) {
        error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (error != INVALID_HANDLE_VALUE) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input;
            startup.StartupInfo.hStdOutput = output;
            startup.StartupInfo.hStdError = error;
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<unsigned char> storage(size);
            startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
                HANDLE handles[] = {input, output, error};
                if (UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) {
                    job = CreateJobObjectW(nullptr, nullptr);
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    PROCESS_INFORMATION process{};
                    auto executable = path.wstring();
                    auto command = L"\"" + executable + L"\"";
                    if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                                       &limits, sizeof(limits)) &&
                        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &process)) {
                        child = process.hProcess;
                        success = AssignProcessToJobObject(job, child) &&
                                  ResumeThread(process.hThread) != DWORD(-1);
                        if (!success) TerminateProcess(child, 1);
                        CloseHandle(process.hThread);
                    }
                }
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            }
            close_pipe(error);
        }
    }
    close_pipe(input);
    close_pipe(output);
    return success;
}

void Process::Impl::terminate() {
    if (child) {
        if (WaitForSingleObject(child, 200) == WAIT_TIMEOUT) {
            TerminateProcess(child, 1);
            WaitForSingleObject(child, 500);
        }
        close_pipe(child);
    }
    close_pipe(job);
}

#else
static bool pipe_cloexec(int (&ends)[2]) {
#ifdef __APPLE__
    if (pipe(ends) != 0) return false;
    return fcntl(ends[0], F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(ends[1], F_SETFD, FD_CLOEXEC) == 0;
#else
    return pipe2(ends, O_CLOEXEC) == 0;
#endif
}

bool Process::Impl::spawn() {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    bool success = false;
    if (pipe_cloexec(input) && pipe_cloexec(output)) {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) == 0) {
            auto executable = path.string();
            char* args[] = {executable.data(), nullptr};
            if (posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) == 0 &&
                posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0 &&
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0) {
                pid_t pid = -1;
                #ifdef __APPLE__
                auto* environment = *_NSGetEnviron();
#else
                auto* environment = environ;
#endif
                if (posix_spawn(&pid, executable.c_str(), &actions, nullptr, args, environment) == 0) {
                    child = pid;
                    to_engine = std::exchange(input[1], no_pipe);
                    from_engine = std::exchange(output[0], no_pipe);
                    success = true;
                }
            }
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    for (int& fd : input) close_pipe(fd);
    for (int& fd : output) close_pipe(fd);
    return success;
}

void Process::Impl::terminate() {
    if (child < 0) return;
    auto result = waitpid(child, nullptr, WNOHANG);
    for (int i = 0; result == 0 && i < 10; ++i) {
        std::this_thread::sleep_for(10ms);
        result = waitpid(child, nullptr, WNOHANG);
    }
    if (result == 0 || (result < 0 && errno == EINTR)) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    child = -1;
}
#endif

void Process::Impl::read_messages() {
    auto read_exact = [&](char* bytes, std::size_t size) {
        while (size > 0) {
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(from_engine, bytes, DWORD(size), &count, nullptr) || !count) return false;
#else
            const auto count = read(from_engine, bytes, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
#endif
            bytes += count;
            size -= std::size_t(count);
        }
        return true;
    };
    while (!dead) {
        char header[4]{};
        if (!read_exact(header, sizeof(header))) break;
        const auto size = little_endian(header, 4);
        if (size < 2 || size > max_frame) break;
        std::string bytes(std::size_t(size), '\0');
        if (!read_exact(bytes.data(), bytes.size())) break;
        Frame frame;
        if (bytes[0] == 'J') {
            frame.message = Json::parse(bytes.begin() + 1, bytes.end(), nullptr, false);
            if (!frame.message.is_object() || !frame.message.value("type", Json()).is_string()) break;
        } else if (bytes[0] == 'P' && size >= 21 && (size - 17) % 4 == 0 && size <= 7697) {
            frame.id = little_endian(bytes.data() + 1, 8);
            frame.offset = little_endian(bytes.data() + 9, 8);
            frame.samples.resize((size - 17) / 4);
            for (std::size_t index = 0; index < frame.samples.size(); ++index) {
                const auto bits = std::uint32_t(little_endian(bytes.data() + 17 + index * 4, 4));
                std::memcpy(&frame.samples[index], &bits, sizeof(bits));
            }
        } else break;
        std::unique_lock lock(mutex);
        available.wait(lock, [&] { return dead || inbound.size() < 128; });
        if (dead) break;
        const bool first = inbound.empty();
        inbound.push_back(std::move(frame));
        lock.unlock();
        if (first) on_input();
    }
    fail();
    on_input();
}

void Process::Impl::write_messages() {
#ifndef _WIN32
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
    while (!dead) {
        std::string next;
        {
            std::unique_lock lock(mutex);
            available.wait(lock, [&] { return dead || !outbound.empty(); });
            if (dead) break;
            next = std::move(outbound.front());
            outbound.pop_front();
        }
        size_t offset = 0;
        while (offset < next.size()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(to_engine, next.data() + offset, DWORD(next.size() - offset), &count, nullptr) ||
                !count) { fail(); on_input(); break; }
#else
            const auto count = write(to_engine, next.data() + offset, next.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { fail(); on_input(); break; }
#endif
            offset += size_t(count);
        }
    }
}

Process::Process(fs::path executable, std::function<void()> on_input)
    : impl_(std::make_unique<Impl>(fs::absolute(executable), std::move(on_input))) {}
Process::~Process() { stop(); }

bool Process::start() {
    stop();
    auto& state = *impl_;
    if (!state.spawn()) {
        stop();
        return false;
    }
    {
        std::lock_guard lock(state.mutex);
        state.dead = false;
    }
    try {
        state.reader = std::thread([&state] { state.read_messages(); });
        state.writer = std::thread([&state] { state.write_messages(); });
    } catch (const std::system_error&) {
        stop();
        return false;
    }
    return true;
}

bool Process::send(const Json& message) {
    auto line = wire_frame(message);
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    const auto type = message.value("type", "");
    const bool control = type == "end" || type == "cancel" || type == "shutdown" || type == "cancel_download";
    if (state.dead || line.empty() || state.outbound.size() >= (control ? 68u : 64u)) return false;
    state.outbound.push_back(std::move(line));
    state.available.notify_all();
    return true;
}

std::vector<Frame> Process::receive() {
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    std::vector<Frame> messages;
    messages.swap(state.inbound);
    if (messages.size() == 128) state.available.notify_all();
    return messages;
}

bool Process::failed() const { return impl_->dead; }

void Process::stop() {
    auto& state = *impl_;
    if (!state.dead && state.writer.joinable()) {
        send({{"type", "shutdown"}});
#ifdef _WIN32
        if (state.child) WaitForSingleObject(state.child, 75);
#else
        std::this_thread::sleep_for(25ms);
#endif
    }
    state.fail();
    state.terminate();
    if (state.writer.joinable()) state.writer.join();
    if (state.reader.joinable()) state.reader.join();
    close_pipe(state.to_engine);
    close_pipe(state.from_engine);
    std::lock_guard lock(state.mutex);
    state.outbound.clear();
    state.inbound.clear();
}

} // namespace xsg
