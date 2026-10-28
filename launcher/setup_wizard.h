// Setup Assistant helpers: toolchain inspection, Wii U -> C++ and Ninja progress parsing,
// runtime DLL deployment, and user settings.ini preference helpers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace setupwizard {

struct ToolchainStatus {
    std::filesystem::path repo_dir;
    std::filesystem::path exe_path;
    std::filesystem::path python_exe;
    std::filesystem::path msys_bin;
    std::filesystem::path cmake_exe;
    std::filesystem::path ninja_exe;
    std::filesystem::path gxx_exe;
    bool has_repo = false;
    bool has_exe = false;
    bool has_generated_cpp = false;
    int generated_cpp_count = 0;
    bool has_python = false;
    bool has_compiler = false;
    bool has_vulkan = false;
};

enum class BuildStage {
    Idle,
    RecompileWiiUToCpp,
    CompileCppToExe,
    Complete,
    Failed,
    Cancelled,
};

struct BuildProgress {
    BuildStage stage = BuildStage::Idle;
    int recompile_step = 0;      // 1..4 inside tools/recompile.py
    int recompile_total = 4;
    int sub_done = 0;            // e.g. shard 42 or object 412
    int sub_total = 0;           // e.g. 258 shards or 1267 objects
    int stage_percent = 0;       // 0..100 within current stage
    int overall_percent = 0;     // 0..100 across the whole pipeline
    std::string phase_text;      // human-readable status line
    std::string current_item;    // e.g. "fn_020c0000.cpp"
    bool matched = false;        // true if line updated progress state
};

struct GamePrefs {
    float scale = 1;            // 0.5 = 360p, 0.75 = 540p, 1 = 720p (native), 2 = 1440p, 3 = 2160p
    int fps = 30;                // 30 or 60
    int filter = 1;              // output scaling: 0 bilinear, 1 bicubic, 2 hermite, 3 nearest (runtime default 1)
    int display = 0;             // 0 fit (letterbox), 1 stretch, 2 fill (crop) (runtime default 0)
    int layout = -1;             // -1 = Auto, 1 = Xbox, 0 = Nintendo
    bool gamepad_view = false;
    bool gyro = true;
};

ToolchainStatus inspect_toolchain(const std::filesystem::path& launcher_dir,
                                  const std::filesystem::path& configured_exe);

// Parses one stdout/stderr line from tools/recompile.py or cmake/ninja and updates state.
bool parse_progress_line(const std::string& line, BuildProgress& state);

// Copies SDL3.dll, libwinpthread-1.dll, and gamecontrollerdb.txt beside BotWRecomp.exe if available.
void deploy_runtime_files(const std::filesystem::path& exe_dir,
                          const std::filesystem::path& msys_bin,
                          const std::filesystem::path& repo_dir);

GamePrefs read_game_prefs(const std::filesystem::path& user_root);
bool write_game_prefs(const std::filesystem::path& user_root, const GamePrefs& prefs);

}  // namespace setupwizard
