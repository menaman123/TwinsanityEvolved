#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace title {
namespace fs = std::filesystem;
namespace {
bool Mutation(SaveStorage::Operation op) {
    return op == SaveStorage::Operation::Write || op == SaveStorage::Operation::Create ||
           op == SaveStorage::Operation::Format;
}
int32_t IoError() { return errno == ENOSPC ? SaveStorage::kNoSpace : SaveStorage::kIoError; }
int32_t IoError(const std::error_code& error) {
    if (error == std::errc::no_space_on_device) return SaveStorage::kNoSpace;
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
        return SaveStorage::kDenied;
    return SaveStorage::kIoError;
}
bool Directory(const fs::path& path) {
    std::error_code error;
    const auto type = fs::symlink_status(path, error).type();
    return !error && type == fs::file_type::directory;
}
bool Regular(const fs::path& path) {
    std::error_code error;
    const auto type = fs::symlink_status(path, error).type();
    return !error && type == fs::file_type::regular;
}
uint32_t LastStep(SaveStorage::Operation op) { return op == SaveStorage::Operation::Write ? 5 : 1; }
}

SaveStorage::SaveStorage(Config config) : config_(std::move(config)) {}
bool SaveStorage::Name(const std::string& name, size_t max, bool empty) {
    if ((!empty && name.empty()) || name.size() > max || name == "." || name == "..") return false;
    for (unsigned char c : name) if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*') return false;
    return true;
}
bool SaveStorage::Supported(Operation op) {
    return op == Operation::Format || op == Operation::Create || op == Operation::Measure ||
           op == Operation::Find || op == Operation::Read || op == Operation::Write;
}
fs::path SaveStorage::Root(unsigned port, unsigned slot) const {
    // Preserve mc0 / mc1 for the standard slots; multitap slots are isolated.
    return config_.directory / ("mc" + std::to_string(port) + (slot ? "_slot" + std::to_string(slot) : ""));
}
fs::path SaveStorage::SavePath(const Request& request) const {
    return Root(request.port, request.slot) / (state_.region + state_.product + request.save);
}
uint64_t SaveStorage::FileKilobytes(uint64_t bytes) { return bytes / 1024 + (bytes % 1024 != 0); }
uint64_t SaveStorage::SaveBytes(uint64_t kilobytes, uint64_t files) {
    const uint64_t entries = files / 2 + files % 2;
    const uint64_t max = UINT64_MAX / 1024;
    if (kilobytes > max - 2 || entries > max - 2 - kilobytes) return UINT64_MAX;
    return (kilobytes + entries + 2) * 1024;
}
bool SaveStorage::Start() {
    if (config_.directory.empty()) return false;
    std::error_code error;
    for (unsigned port = 0; port < 2; ++port) for (unsigned slot = 0; slot < 2; ++slot) {
        if (!config_.present[port * 2 + slot]) continue;
        fs::create_directories(Root(port, slot), error);
        if (error || !Directory(Root(port, slot))) return false;
    }
    return true;
}
bool SaveStorage::Initialise(const std::string& region, const std::string& product) {
    if (state_.initialised) return state_.region == region && state_.product == product;
    if (!Name(region, 7) || !Name(product, 15) || !Start()) return false;
    state_.region = region;
    state_.product = product;
    state_.initialised = true;
    for (unsigned port = 0; port < 2; ++port) for (unsigned slot = 0; slot < 2; ++slot)
        state_.info[port * 2 + slot] = Inspect(port, slot);
    return true;
}
SaveStorage::Info SaveStorage::Inspect(unsigned port, unsigned slot) const {
    if (port >= 2 || slot >= 2 || !config_.present[port * 2 + slot] || !Directory(Root(port, slot)))
        return {0, 0, 0};
    uint64_t used = 0;
    std::map<fs::path, uint64_t> directory_files;
    std::error_code error;
    fs::recursive_directory_iterator it(Root(port, slot), fs::directory_options::none, error), end;
    for (; !error && it != end; it.increment(error)) {
        const auto status = it->symlink_status(error);
        if (error) break;
        if (it->path().filename().string().find(".save-pending-") == 0) continue;
        if (fs::is_symlink(status)) { it.disable_recursion_pending(); continue; }
        if (fs::is_directory(status)) {
            used += 2;
            directory_files.emplace(it->path(), 0);
        }
        else if (fs::is_regular_file(status)) {
            const uint64_t bytes = it->file_size(error);
            if (error) break;
            used += FileKilobytes(bytes);
            ++directory_files[it->path().parent_path()];
        }
    }
    if (error) return {0, 0, 0};
    for (const auto& directory : directory_files) used += (directory.second + 1) / 2;
    const uint64_t remaining = used >= config_.capacity_kilobytes ? 0 : config_.capacity_kilobytes - used;
    return {2, static_cast<int32_t>(std::min<uint64_t>(remaining, INT32_MAX)), 1};
}
SaveStorage::Info SaveStorage::StorageInfo(unsigned port, unsigned slot) const {
    return port < 2 && slot < 2 ? state_.info[port * 2 + slot] : Info{0, 0, 0};
}
SaveStorage::Result SaveStorage::GetResult(Operation operation) const {
    const auto index = static_cast<unsigned>(operation);
    return index < state_.results.size() ? state_.results[index] : Result{0, 0, kInvalid};
}
bool SaveStorage::Submit(Request request, uint64_t boundary) {
    last_submit_error_ = 0;
    if (Busy() || state_.completion) { last_submit_error_ = -200; return false; }
    if (!state_.initialised || request.port >= 2 || request.slot >= 2 ||
        !config_.present[request.port * 2 + request.slot]) {
        last_submit_error_ = kNoStorage; return false;
    }
    if (!Supported(request.operation) || !Name(request.save, 31, true) ||
        ((request.operation == Operation::Read || request.operation == Operation::Write ||
          request.operation == Operation::Find) && !Name(request.file, 31)) ||
        request.file.rfind(".save-pending-", 0) == 0 ||
        request.size > kMaxBytes ||
        state_.sequence == UINT64_MAX ||
        (request.operation != Operation::Write && !request.bytes.empty()) ||
        (request.operation == Operation::Write &&
         (request.bytes.size() != request.size || request.save.size() >= 10))) {
        last_submit_error_ = kInvalid; return false;
    }
    state_.results = {};
    state_.pending = std::move(request);
    state_.active = Operation::Checking;
    state_.progress = 0;
    state_.last_boundary = boundary;
    ++state_.sequence;
    return true;
}
SaveStorage::Operation SaveStorage::Update(unsigned port, unsigned slot, uint64_t boundary) {
    if (!Busy()) {
        if (port < 2 && slot < 2) {
            const unsigned index = port * 2 + slot;
            if (!inspected_[index] || state_.info[index].type != 2 || boundary < inspected_at_[index] ||
                boundary - inspected_at_[index] >= kInspectBoundaries) {
                state_.info[index] = Inspect(port, slot);
                inspected_[index] = true;
                inspected_at_[index] = boundary;
            }
        }
        return Operation::None;
    }
    // Repeated polls within one boundary do not advance storage time.
    if (boundary <= state_.last_boundary) return state_.active;
    state_.last_boundary = boundary;
    const auto& request = state_.pending;
    state_.info[request.port * 2 + request.slot] = Inspect(request.port, request.slot);
    if (state_.info[request.port * 2 + request.slot].type != 2) {
        Completion completion;
        completion.operation = request.operation;
        completion.result = {0, static_cast<int32_t>(state_.progress), kNoStorage};
        Finish(std::move(completion));
        return Operation::NoStorage;
    }
    if (state_.active == Operation::Checking) {
        state_.active = request.operation;
    } else if (state_.progress++ >= LastStep(request.operation)) {
        Finish(Execute(request));
    }
    return state_.active;
}
void SaveStorage::Finish(Completion completion) {
    completion.generation = state_.generation;
    completion.sequence = state_.sequence;
    state_.results[static_cast<unsigned>(completion.operation)] = completion.result;
    state_.completion = std::move(completion);
    ++completed_;
    if (state_.completion->committed) ++committed_;
    state_.active = Operation::None;
    state_.pending = {};
    state_.progress = 0;
    inspected_.fill(false);   // the operation may have changed what the slots hold
}
std::optional<SaveStorage::Completion> SaveStorage::TakeCompletion() {
    auto completion = std::move(state_.completion);
    state_.completion.reset();
    return completion;
}
SaveStorage::Completion SaveStorage::Execute(const Request& request) {
    Completion out;
    out.operation = request.operation;
    out.result = {1, static_cast<int32_t>(LastStep(request.operation)), 0};
    const auto fail = [&](int32_t error) { out.result.succeeded = 0; out.result.error = error; };
    const fs::path root = Root(request.port, request.slot), save = SavePath(request);
    std::error_code error;
    if (request.operation == Operation::Format) {
        // This request follows the game's format confirmation. Replace this
        // slot only. Keep its old directory until the new empty one is live;
        // a failed second rename rolls back rather than reporting success.
        const std::string suffix = std::to_string(state_.generation) + "-" + std::to_string(state_.sequence);
        const fs::path empty = config_.directory / (".save-format-" + root.filename().string() + "-" + suffix);
        const fs::path backup = config_.directory / (".save-backup-" + root.filename().string() + "-" + suffix);
        if (fs::exists(empty, error) || fs::exists(backup, error) || error ||
            !fs::create_directory(empty, error) || error) { fail(kIoError); return out; }
        fs::rename(root, backup, error);
        if (error) { fs::remove(empty, error); fail(kIoError); return out; }
        fs::rename(empty, root, error);
        if (error) {
            std::error_code rollback;
            fs::rename(backup, root, rollback);
            fs::remove(empty, rollback);
            fail(kIoError);
            return out;
        }
        out.committed = true;
#ifndef _WIN32
        const int directory = open(config_.directory.c_str(), O_RDONLY);
        if (directory < 0) { fail(kIoError); return out; }
        const int synced = fsync(directory);
        close(directory);
        if (synced != 0) { fail(kIoError); return out; }
#endif
        fs::remove_all(backup, error);
        if (error) fail(kIoError);
        return out;
    }
    if (request.operation == Operation::Create) {
        const auto status = fs::symlink_status(save, error);
        if (error && error != std::errc::no_such_file_or_directory) { fail(IoError(error)); return out; }
        if (fs::is_directory(status)) return out;
        if (status.type() != fs::file_type::not_found) { fail(kDenied); return out; }
        error.clear();
        if (StorageInfo(request.port, request.slot).free_kilobytes < 2) { fail(kNoSpace); return out; }
        if (!fs::create_directory(save, error) || error) fail(IoError(error));
        else out.committed = true;
        return out;
    }
    const auto save_status = fs::symlink_status(save, error);
    if (error && error != std::errc::no_such_file_or_directory) { fail(IoError(error)); return out; }
    if (save_status.type() == fs::file_type::not_found) {
        if (request.operation == Operation::Measure) out.value = -1;
        else fail(kMissing);
        return out;
    }
    if (!fs::is_directory(save_status)) { fail(kDenied); return out; }
    error.clear();
    std::vector<fs::directory_entry> files;
    fs::directory_iterator it(save, error), end;
    for (; !error && it != end; it.increment(error)) {
        // Interrupted temporary writes are not game files.
        if (it->path().filename().string().find(".save-pending-") == 0) continue;
        if (!Regular(it->path())) { fail(kDenied); return out; }
        files.push_back(*it);
    }
    if (error) { fail(kIoError); return out; }
    if (files.size() > 18) { fail(kNoSpace); return out; }
    if (request.operation == Operation::Measure) {
        uint64_t size = 0;
        for (const auto& file : files) {
            const auto bytes = file.file_size(error);
            if (error) { fail(kIoError); return out; }
            size += FileKilobytes(bytes);
        }
        out.value = static_cast<int32_t>(size + (files.size() + 1) / 2 + 2);
        return out;
    }
    const fs::path file = save / request.file;
    const bool exists = Regular(file);
    if (request.operation == Operation::Find) {
        if (!exists) { out.value = -1; return out; }
        const uint64_t bytes = fs::file_size(file, error);
        if (error || bytes > INT32_MAX) { fail(kIoError); return out; }
        out.value = static_cast<int32_t>(bytes);
        out.entry.size = static_cast<uint32_t>(bytes);
        out.entry.attributes = 0x8497;
        // Same deterministic directory dates as the existing host save store.
        out.entry.created = out.entry.modified = {{0, 0, 0, 12, 15, 8, 0xea, 7}};
        std::copy(request.file.begin(), request.file.end(), out.entry.name.begin());
        out.entry_present = true;
        return out;
    }
    if (request.operation == Operation::Read) {
        if (!exists) { fail(kMissing); return out; }
        std::ifstream stream(file, std::ios::binary);
        if (!stream) { fail(kIoError); return out; }
        out.bytes.resize(request.size);
        stream.read(reinterpret_cast<char*>(out.bytes.data()), request.size);
        const auto read = stream.gcount();
        if (stream.bad() || (stream.fail() && !stream.eof())) { fail(kIoError); out.bytes.clear(); return out; }
        out.bytes.resize(static_cast<size_t>(read));
        return out; // short reads retain the game's own format/checksum checks
    }
    if (!exists && fs::exists(file, error)) { fail(kDenied); return out; }
    if (!exists && files.size() >= 18) { fail(kNoSpace); return out; }
    uint64_t old_size = 0;
    if (exists) {
        old_size = fs::file_size(file, error);
        if (error) { fail(kIoError); return out; }
    }
    const uint64_t free = static_cast<uint32_t>(StorageInfo(request.port, request.slot).free_kilobytes);
    const uint64_t entry_cost = !exists && files.size() % 2 == 0 ? 1 : 0;
    if (FileKilobytes(request.size) + entry_cost > free + FileKilobytes(old_size)) {
        fail(kNoSpace); return out;
    }
    const int32_t result = AtomicWrite(file, request.bytes, out.committed);
    if (result) fail(result);
    return out;
}
int32_t SaveStorage::AtomicWrite(const fs::path& file, const std::vector<uint8_t>& bytes, bool& committed) {
    const auto injected = [&](WritePoint point) { return config_.write_failure ? config_.write_failure(point) : 0; };
    if (const int32_t error = injected(WritePoint::BeforeWrite)) return error;
    fs::path temporary;
    FILE* stream = nullptr;
#ifdef _WIN32
    wchar_t name[MAX_PATH];
    if (!GetTempFileNameW(file.parent_path().c_str(), L"sv", 0, name)) return kIoError;
    temporary = name;
    stream = _wfopen(temporary.c_str(), L"wb");
#else
    std::string pattern = (file.parent_path() / ".save-pending-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) return IoError();
    temporary = name.data();
    stream = fdopen(descriptor, "wb");
    if (!stream) close(descriptor);
#endif
    const auto remove_temporary = [&] { std::error_code ignored; fs::remove(temporary, ignored); };
    if (!stream) { remove_temporary(); return kIoError; }
    int32_t result = 0;
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stream) != bytes.size()) result = IoError();
    if (!result) result = injected(WritePoint::BeforeSync);
    if (!result && std::fflush(stream) != 0) result = IoError();
#ifdef _WIN32
    if (!result && _commit(_fileno(stream)) != 0) result = IoError();
#else
    if (!result && fsync(fileno(stream)) != 0) result = IoError();
#endif
    if (std::fclose(stream) != 0 && !result) result = IoError();
    if (!result) result = injected(WritePoint::BeforeCommit);
    if (result) { remove_temporary(); return result; }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove_temporary(); return kIoError;
    }
    committed = true;
#else
    if (std::rename(temporary.c_str(), file.c_str()) != 0) { result = IoError(); remove_temporary(); return result; }
    committed = true;
#if defined(__vita__)
    // The Vita: no directory to sync. VitaSDK's open() of a directory is an sceIoDopen descriptor and its fsync() passes
    // that to sceIoSyncByFd, which takes a FILE descriptor -- an error there would report this committed write as an I/O
    // error to the game (the emulator, which does not implement sceIoSyncByFd, never shows it). The kernel's rename (its
    // _rename_r: sceIoRemove of the old file, then sceIoRename) is the commit, as engine/platform/host's DirectorySync
    // says for the Vita.
#else
    const int directory = open(file.parent_path().c_str(), O_RDONLY);
    if (directory < 0) return kIoError;
    const int synced = fsync(directory);
    close(directory);
    // The complete file is already committed even if durability confirmation
    // fails. Report that fact; never replay or pretend the preceding file won.
    if (synced != 0) return kIoError;
#endif
#endif
    return 0;
}
void SaveStorage::SetPresent(unsigned port, unsigned slot, bool present) {
    if (port < 2 && slot < 2) config_.present[port * 2 + slot] = present;
    inspected_.fill(false);
}
void SaveStorage::Reset() {
    const uint64_t generation = state_.generation == UINT64_MAX ? UINT64_MAX : state_.generation + 1;
    state_ = Snapshot{};
    state_.generation = generation;
    last_submit_error_ = 0;
    inspected_.fill(false);
}
bool SaveStorage::CanCheckpoint() const { return !Busy() || !Mutation(state_.pending.operation); }
std::optional<SaveStorage::Snapshot> SaveStorage::Capture() const {
    return CanCheckpoint() ? std::optional<Snapshot>(state_) : std::nullopt;
}
bool SaveStorage::ValidateSnapshot(const Snapshot& snapshot, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (snapshot.version != 1 || snapshot.generation == 0 || snapshot.generation == UINT64_MAX ||
        snapshot.sequence == UINT64_MAX ||
        (!snapshot.initialised && (snapshot.active != Operation::None || snapshot.completion ||
         !snapshot.region.empty() || !snapshot.product.empty())) ||
        (snapshot.initialised && (!Name(snapshot.region, 7) || !Name(snapshot.product, 15))))
        return fail("SaveStorage: invalid snapshot identity or generation");
    if (snapshot.active != Operation::None &&
        (!Supported(snapshot.pending.operation) || Mutation(snapshot.pending.operation) ||
         snapshot.pending.port >= 2 || snapshot.pending.slot >= 2 || snapshot.pending.size > kMaxBytes ||
         snapshot.sequence == 0 || !snapshot.pending.bytes.empty() ||
         !Name(snapshot.pending.save, 31, true) ||
         !Name(snapshot.pending.file, 31, snapshot.pending.operation == Operation::Measure) ||
         snapshot.pending.file.rfind(".save-pending-", 0) == 0))
        return fail("SaveStorage: invalid pending request or uncheckpointable mutation");
    if (snapshot.active != Operation::None && snapshot.active != Operation::Checking &&
        snapshot.active != snapshot.pending.operation) return fail("SaveStorage: inconsistent active operation");
    if (snapshot.active != Operation::None &&
        (snapshot.progress > LastStep(snapshot.pending.operation) ||
         (snapshot.active == Operation::Checking && snapshot.progress != 0)))
        return fail("SaveStorage: invalid operation step");
    if (snapshot.completion &&
        (!Supported(snapshot.completion->operation) || snapshot.completion->bytes.size() > kMaxBytes ||
         snapshot.active != Operation::None || snapshot.sequence == 0 ||
         snapshot.completion->generation != snapshot.generation || snapshot.completion->sequence != snapshot.sequence ||
         (snapshot.completion->result.succeeded != 0 && snapshot.completion->result.succeeded != 1) ||
         (snapshot.completion->operation != Operation::Read && !snapshot.completion->bytes.empty()) ||
         (snapshot.completion->result.succeeded == 0 && !snapshot.completion->bytes.empty()) ||
         (snapshot.completion->committed && !Mutation(snapshot.completion->operation)) ||
         (snapshot.completion->entry_present &&
          (snapshot.completion->operation != Operation::Find || snapshot.completion->result.succeeded != 1 ||
           std::find(snapshot.completion->entry.name.begin(), snapshot.completion->entry.name.end(), '\0') ==
               snapshot.completion->entry.name.end()))))
        return fail("SaveStorage: invalid completion");
    if (snapshot.active == Operation::None && (snapshot.pending.operation != Operation::None ||
        !snapshot.pending.bytes.empty() || snapshot.progress != 0))
        return fail("SaveStorage: idle snapshot retains a request");
    for (const auto& result : snapshot.results)
        if (result.succeeded < -1 || result.succeeded > 1 || result.step < -1 || result.step > 5)
            return fail("SaveStorage: invalid result");
    for (const auto& info : snapshot.info)
        if ((info.type != -1 && info.type != 0 && info.type != 2) || info.free_kilobytes < -1 ||
            info.formatted < -1 || info.formatted > 1) return fail("SaveStorage: invalid storage information");
    if (snapshot.completion) {
        const auto& result = snapshot.results[static_cast<unsigned>(snapshot.completion->operation)];
        const auto& completion = snapshot.completion->result;
        if (result.succeeded != completion.succeeded || result.step != completion.step || result.error != completion.error)
            return fail("SaveStorage: completion disagrees with retained result");
    }
    if (error) error->clear();
    return true;
}
bool SaveStorage::Restore(const Snapshot& snapshot, uint64_t new_boundary) {
    const uint64_t generation = std::max(state_.generation, snapshot.generation);
    if (generation == UINT64_MAX || !ValidateSnapshot(snapshot)) return false;
    state_ = snapshot;
    state_.generation = generation + 1;
    state_.last_boundary = new_boundary;
    if (state_.completion) state_.completion->generation = state_.generation;
    inspected_.fill(false);
    return true;
}

}  // namespace title
