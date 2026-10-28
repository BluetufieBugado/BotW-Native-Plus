#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Extract a zip (store or deflate, no encryption, no zip64) into dest.
// Rejects paths that escape dest, and refuses archives that expand too far.
// With `keep`, entries whose name it rejects are skipped and do not count toward the file and size limits.
bool unzip_to(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& err,
              const std::function<bool(const std::string&)>& keep = {});
