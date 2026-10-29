#pragma once
// The importer's file access: UTF-8 paths on every desktop host (wide paths on Windows), 64-bit offsets, positioned reads
// and writes, and the folder operations through std::filesystem. Desktop only (the launcher and the mod tool: the importer
// is never compiled for the Vita).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace te::modimport::io {

std::filesystem::path Path(const std::string& utf8);
std::string Utf8(const std::filesystem::path& p);
std::string Join(const std::string& a, const std::string& b);   // a + '/' + b (std::filesystem's operator/ in UTF-8)

class File {
public:
    File() = default;
    ~File() { Close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept : f_(o.f_), size_(o.size_) { o.f_ = nullptr; }

    bool OpenRead(const std::string& path);
    bool Create(const std::string& path);          // truncates
    bool OpenReadWrite(const std::string& path);   // an existing file
    bool IsOpen() const { return f_ != nullptr; }
    bool Close();                                  // false when buffered writes failed
    uint64_t Size() const { return size_; }
    bool ReadAt(uint64_t offset, void* dst, size_t n);
    bool WriteAt(uint64_t offset, const void* src, size_t n);
    bool Append(const void* src, size_t n) { return WriteAt(size_, src, n); }

private:
    bool Seek(uint64_t offset);
    std::FILE* f_ = nullptr;
    uint64_t size_ = 0;
    uint64_t pos_ = UINT64_MAX;   // the stream's position when known (skips redundant seeks)
    bool writing_ = false;        // the last operation wrote (a read after it needs a seek)
};

bool ReadWhole(const std::string& path, uint64_t limit, std::string* out);
bool WriteWhole(const std::string& path, const std::string& data);
bool IsDir(const std::string& path);
bool IsFile(const std::string& path);
bool MakeDirs(const std::string& path);
bool RemoveAll(const std::string& path);   // true when nothing is left
bool Rename(const std::string& from, const std::string& to);
uint64_t FreeBytes(const std::string& path);   // the space available to this user at `path` (or its nearest existing parent)
// Every file under `dir` (recursively), as '/'-separated paths relative to it, sorted; hidden names and the file
// managers' Thumbs.db / desktop.ini left out
bool ListFiles(const std::string& dir, std::vector<std::string>* out);
bool HostClutter(const std::string& name);

}  // namespace te::modimport::io
