// io.h implementation.
#include "app/host/mod_import/io.h"

#include <algorithm>
#include <system_error>

#include "engine/platform/host/platform.h"

namespace te::modimport::io {

namespace fs = std::filesystem;

fs::path Path(const std::string& utf8) {
#ifdef _WIN32
    return fs::u8path(utf8);
#else
    return fs::path(utf8);
#endif
}

std::string Utf8(const fs::path& p) {
#ifdef _WIN32
    return p.u8string();
#else
    return p.string();
#endif
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

namespace {

std::FILE* OpenFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide = Path(path).wstring();
    std::wstring wmode;
    for (const char* m = mode; *m; ++m) wmode.push_back(wchar_t(*m));
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), wmode.c_str()) != 0) return nullptr;
    return f;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

bool File::OpenRead(const std::string& path) {
    Close();
    f_ = OpenFile(path, "rb");
    if (!f_) return false;
    if (ps2r::host::FileSeek64(f_, 0, SEEK_END) != 0) { Close(); return false; }
    const int64_t end = ps2r::host::FileTell64(f_);
    if (end < 0) { Close(); return false; }
    size_ = uint64_t(end);
    pos_ = size_;
    return true;
}

bool File::Create(const std::string& path) {
    Close();
    f_ = OpenFile(path, "w+b");
    size_ = 0;
    pos_ = 0;
    return f_ != nullptr;
}

bool File::OpenReadWrite(const std::string& path) {
    Close();
    f_ = OpenFile(path, "r+b");
    if (!f_) return false;
    if (ps2r::host::FileSeek64(f_, 0, SEEK_END) != 0) { Close(); return false; }
    const int64_t end = ps2r::host::FileTell64(f_);
    if (end < 0) { Close(); return false; }
    size_ = uint64_t(end);
    pos_ = size_;
    return true;
}

bool File::Close() {
    bool ok = true;
    if (f_) ok = std::fclose(f_) == 0;
    f_ = nullptr;
    size_ = 0;
    pos_ = UINT64_MAX;
    writing_ = false;
    return ok;
}

bool File::Seek(uint64_t offset) {
    if (pos_ == offset) return true;
    if (offset > uint64_t(INT64_MAX) || ps2r::host::FileSeek64(f_, int64_t(offset), SEEK_SET) != 0) {
        pos_ = UINT64_MAX;
        return false;
    }
    pos_ = offset;
    return true;
}

bool File::ReadAt(uint64_t offset, void* dst, size_t n) {
    if (!f_ || offset > size_ || n > size_ - offset) return false;
    if (writing_) {   // C: a read after a write needs a positioning call between them
        pos_ = UINT64_MAX;
        writing_ = false;
    }
    if (!Seek(offset)) return false;
    const size_t got = std::fread(dst, 1, n, f_);
    pos_ = got == n ? offset + n : UINT64_MAX;
    return got == n;
}

bool File::WriteAt(uint64_t offset, const void* src, size_t n) {
    if (!f_) return false;
    if (!writing_) {   // a write after a read needs a positioning call between them
        pos_ = UINT64_MAX;
        writing_ = true;
    }
    if (!Seek(offset)) return false;
    const size_t put = std::fwrite(src, 1, n, f_);
    pos_ = put == n ? offset + n : UINT64_MAX;
    if (put != n) return false;
    size_ = std::max(size_, offset + n);
    return true;
}

bool ReadWhole(const std::string& path, uint64_t limit, std::string* out) {
    File f;
    if (!f.OpenRead(path) || f.Size() > limit) return false;
    out->assign(size_t(f.Size()), '\0');
    return out->empty() || f.ReadAt(0, &(*out)[0], out->size());
}

bool WriteWhole(const std::string& path, const std::string& data) {
    File f;
    if (!f.Create(path)) return false;
    const bool ok = data.empty() || f.Append(data.data(), data.size());
    return f.Close() && ok;
}

bool IsDir(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(Path(path), ec);
}

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(Path(path), ec);
}

bool MakeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(Path(path), ec);
    return IsDir(path);
}

bool RemoveAll(const std::string& path) {
    std::error_code ec;
    fs::remove_all(Path(path), ec);
    return !fs::exists(Path(path), ec);
}

bool Rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(Path(from), Path(to), ec);
    return !ec;
}

uint64_t FreeBytes(const std::string& path) {
    std::error_code ec;
    fs::path p = Path(path);
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    const fs::space_info s = fs::space(p.empty() ? fs::path(".") : p, ec);
    return ec ? 0 : uint64_t(s.available);
}

bool HostClutter(const std::string& name) {
    std::string l = name;
    for (char& c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return name.empty() || name[0] == '.' || l == "thumbs.db" || l == "desktop.ini";
}

bool ListFiles(const std::string& dir, std::vector<std::string>* out) {
    out->clear();
    std::error_code ec;
    const fs::path root = Path(dir);
    if (!fs::is_directory(root, ec)) return false;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return false;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const fs::path& p = it->path();
        const std::string name = Utf8(p.filename());
        if (HostClutter(name)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string rel = Utf8(fs::relative(p, root, ec));
        if (ec) return false;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out->push_back(rel);
    }
    std::sort(out->begin(), out->end());
    return true;
}

}  // namespace te::modimport::io
