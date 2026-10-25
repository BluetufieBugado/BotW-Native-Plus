// One-time game build used by the launcher's first-run setup: recompile U-King.rpx to C++
// (tools/recompile.py), configure with CMake + Ninja, compile, then copy BotWRecomp.exe and the
// DLLs it needs next to the launcher. Mirrors the "Build on Windows" steps in README.md.
#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gamebuild {

struct Toolchain {
    std::filesystem::path ucrt_bin;   // ...\msys64\ucrt64\bin
    std::filesystem::path msys_root;  // ...\msys64
    bool gxx = false, cmake = false, ninja = false, python = false;
    bool ready() const { return !ucrt_bin.empty() && gxx && cmake && ninja && python; }
};

Toolchain detect_toolchain();
// True when `dir` holds tools/recompile.py and CMakeLists.txt.
bool is_source_dir(const std::filesystem::path& dir);
// Looks beside and above `start` for a source checkout.
std::filesystem::path find_source(const std::filesystem::path& start);

// Parallel compile jobs from this machine's cores and memory (about 2.5 GB per job, like README's
// -j6 on 16 GB), at least 1.
int auto_jobs(int* cores_out = nullptr, double* ram_gb_out = nullptr);

// The pacman line from README.md, for the guided toolchain install.
const wchar_t* pacman_packages();

enum class Stage { Idle, Prepare, Recompile, Configure, Compile, Install, Done, Failed, Cancelled };
const wchar_t* stage_name(Stage s);
int stage_index(Stage s);  // 1..5 for the working stages
constexpr int kStageCount = 5;

struct Options {
    std::filesystem::path source;      // checkout root
    std::filesystem::path rpx;         // <game>\code\U-King.rpx
    std::filesystem::path install_dir; // where BotWRecomp.exe and DLLs are copied
    Toolchain tc;
    int jobs = 1;
    bool dry_run = false;              // recompile.py --demo-progress and ninja -n; nothing is installed
};

class Runner {
public:
    ~Runner();
    void start(const Options& o);
    void cancel();
    bool running() const { return running_; }

    // Snapshot for the UI thread.
    Stage stage() const { return stage_; }
    int percent() const { return percent_; }
    std::string take_log();  // new log text since the last call
    std::string error() { std::lock_guard<std::mutex> l(mu_); return err_; }
    std::string detail() { std::lock_guard<std::mutex> l(mu_); return detail_; }

private:
    void work(Options o);
    bool run(const std::wstring& cmd, const std::filesystem::path& cwd, const Options& o);
    void log(const std::string& s);

    std::thread th_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<int> percent_{0};
    std::mutex mu_;
    std::string log_, err_, detail_;
    HANDLE job_ = nullptr;
};

// Name of the marker written into generated/ after a complete recompile of a given RPX.
constexpr const char* kCodegenMarker = ".codegen_complete";
bool codegen_complete(const std::filesystem::path& source, const std::string& rpx_sha1);

// Copies exe plus every DLL it imports (recursively) that lives in `dll_dir`, into `dest`.
bool install_with_dlls(const std::filesystem::path& exe, const std::filesystem::path& dll_dir,
                       const std::filesystem::path& dest, std::string& err, std::function<void(const std::string&)> note);

}  // namespace gamebuild
