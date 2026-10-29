#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace title {

// One game save operation at a time, advanced on the logical game boundary.
// Submit owns its inputs; Update publishes a complete result on the same owner.
// It contains no memory-card RPC, firmware, descriptor or semaphore model.
class SaveStorage {
public:
    enum class Operation : int32_t {
        NoStorage = -1, None = 0, Checking = 1, Format = 2, Create = 4,
        Measure = 7, Find = 8, Read = 10, Write = 11
    };
    static constexpr uint32_t kMaxBytes = 2 * 1024 * 1024;
    static constexpr int32_t kNoSpace = -3, kMissing = -4, kDenied = -5,
                             kIoError = -8, kInvalid = -22, kNoStorage = -100;
    struct Result { int32_t succeeded = -1, step = -1, error = 0; };
    struct Info { int32_t type = -1, free_kilobytes = -1, formatted = -1; };
    struct FileEntry {
        std::array<uint8_t, 8> created{}, modified{};
        uint32_t size = 0;
        uint16_t attributes = 0, reserved0 = 0;
        std::array<uint32_t, 2> reserved1{};
        std::array<char, 32> name{};
    };
    static_assert(sizeof(FileEntry) == 64, "game directory entry ABI");
    struct Request {
        Operation operation = Operation::None;
        unsigned port = 0, slot = 0;
        std::string save, file;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;
    };
    struct Completion {
        // Declared here, defaulted after the class: State holds an
        // std::optional<Completion> while SaveStorage is still incomplete, and
        // clang + libstdc++ evaluate (and cache) is_constructible<Completion>
        // there, before the default member initializers below are parsed.
        Completion();
        uint64_t generation = 0, sequence = 0;
        Operation operation = Operation::None;
        Result result;
        int32_t value = -1; // measure in KiB, find in bytes
        FileEntry entry;
        bool entry_present = false, committed = false;
        std::vector<uint8_t> bytes;
    };
    enum class WritePoint { BeforeWrite, BeforeSync, BeforeCommit };
    struct Config {
        std::filesystem::path directory; // existing Config paths.mc_dir
        uint32_t capacity_kilobytes = 0x1f00;
        std::array<bool, 4> present{{true, true, true, true}};
        // Testing / host I/O failure injection: return an error before commit.
        std::function<int32_t(WritePoint)> write_failure = {};
    };
    struct Snapshot {
        uint32_t version = 1;
        bool initialised = false;
        std::string region, product;
        uint64_t generation = 1, sequence = 0, last_boundary = 0;
        Operation active = Operation::None;
        uint32_t progress = 0;
        Request pending;
        std::array<Result, 12> results{};
        std::array<Info, 4> info{};
        std::optional<Completion> completion;
    };

    explicit SaveStorage(Config config);
    // Prepare configured host save roots without selecting a game format,
    // resetting logical requests or rewriting existing progress. Used by the
    // title's startup service request before its save-manager Initialise.
    bool Start();
    bool Initialise(const std::string& region, const std::string& product);
    bool Submit(Request request, uint64_t boundary);
    Operation Update(unsigned port, unsigned slot, uint64_t boundary);
    Info StorageInfo(unsigned port, unsigned slot) const;
    Result GetResult(Operation operation) const;
    std::optional<Completion> TakeCompletion();
    bool Busy() const { return state_.active != Operation::None; }
    Operation Active() const { return state_.active; }
    int32_t LastSubmitError() const { return last_submit_error_; }
    uint64_t Completed() const { return completed_; }
    uint64_t Committed() const { return committed_; }
    void SetPresent(unsigned port, unsigned slot, bool present);
    void Reset();
    // Mutating operations must finish before a runtime checkpoint. Capturing a
    // pending mutation could replay a completed disk write after a later load.
    // Completed durable operations ARE captured, without executing them again.
    bool CanCheckpoint() const;
    std::optional<Snapshot> Capture() const;
    // Pure preflight; no filesystem, generation changes or completion delivery.
    static bool ValidateSnapshot(const Snapshot& snapshot, std::string* error = nullptr);
    bool Restore(const Snapshot& snapshot, uint64_t new_boundary);
    static uint64_t FileKilobytes(uint64_t bytes);
    static uint64_t SaveBytes(uint64_t kilobytes, uint64_t files);

private:
    static bool Name(const std::string& name, size_t max, bool empty = false);
    static bool Supported(Operation operation);
    std::filesystem::path Root(unsigned port, unsigned slot) const;
    std::filesystem::path SavePath(const Request& request) const;
    Info Inspect(unsigned port, unsigned slot) const;
    Completion Execute(const Request& request);
    int32_t AtomicWrite(const std::filesystem::path& file, const std::vector<uint8_t>& bytes, bool& committed);
    void Finish(Completion completion);
    Config config_;
    Snapshot state_;
    // A present slot's walk of its directory (Inspect) is redone once a second of boundaries while idle, and after anything
    // this storage changed: the menus poll the slot every frame, and the walk is file-system work on the game thread. An
    // absent slot is asked again at every poll (that answer costs no walk).
#if defined(__vita__)
    // The Vita: nothing but this process changes its card while it runs (one application at a time), so the walk is redone
    // only after this storage's own changes -- in the emulator the walk's file-system calls took the game thread ~30 ms,
    // one hitch a second (TE_TICK_LOG_MS: a ~45 ms tick every 60 boundaries, all of it "game")
    static constexpr uint64_t kInspectBoundaries = UINT64_MAX;
#else
    static constexpr uint64_t kInspectBoundaries = 60;
#endif
    std::array<uint64_t, 4> inspected_at_{};
    std::array<bool, 4> inspected_{};
    int32_t last_submit_error_ = 0;
    uint64_t completed_ = 0, committed_ = 0;
};

inline SaveStorage::Completion::Completion() = default;

}  // namespace title
