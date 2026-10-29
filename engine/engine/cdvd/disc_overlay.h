#pragma once
// DiscOverlay: an ISO9660 image with LAYERS of replacement files composed over it -- one virtual volume every reader of
// the disc sees through the IsoFs interface (Find / List / ReadBytes / ReadSectors / ImageBytes).
//
// A layer is a folder whose disc/ tree is laid out like the disc:
//   <layer>/disc/<DISC PATH>           the whole file (a replacement, or an added file when the disc has none there);
//                                      paths case-insensitive, no ";1": disc/CRASH6/ENGLISH.MH
//   <layer>/disc/<DISC PATH>.ranges    a RANGE RECIPE instead: UTF-8 text, one segment per line, in order --
//                                        base <offset> <length>             bytes of the LAYER BELOW's version of the file
//                                        file <relpath> <offset> <length>   bytes of a file of the layer (relpath from <layer>/)
//                                        zero <length>
//                                      the composed size is the sum; '#' starts a comment; numbers decimal or 0x hex.
// Layers apply in order, later wins; a recipe's "base" is the volume below it (the image, or the layers before), so two
// layers touching one file stack. A layer with any error (a malformed recipe, a range past its source, a file where the
// disc has a directory, a recipe over another version of its file than the one it was made against -- Layer::checks ...)
// is refused WHOLE and the composition goes on without it (Report::refused says why).
//
// Placement: a composed file no bigger than its original sector allocation keeps its LBA (its allocation is then served
// from the composed bytes, zero past its end); a grown or added file moves to a VIRTUAL sector range past the image (from
// max(VolumeSectors, image sectors) on, in path order), and ImageBytes() grows to cover it. Find / List answer the new
// LBA / size (an added file in a directory the disc lacks gets a synthesized directory); ReadBytes / ReadSectors route
// each byte: inside a composed file's sector range -> the composed bytes, else -> the image.
//
// Cost: the composition is done once, at Open; reads are const and thread-safe (positioned reads, an atomic hit count).
// A read binary-searches a sorted interval table and allocates nothing. With no composed file the table is empty and every
// call forwards to IsoFs (a plain IsoFs is what the port uses when no mod is on: no overlay object at all).
// Generic: nothing here knows any title's archives.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/cdvd/iso_fs.h"

class DiscOverlay : public IsoFs {
public:
    // What a layer's recipe was made against (mod.json files[].base_*, written by the importer): the file below it must be
    // that file, or the layer is refused whole -- a recipe's `base` ranges over another version of the file (another
    // disc's archive, or another mod's rebuilt one) would compose wrong bytes. Checked when the layer is composed: the
    // file below must exist with `bytes` bytes and, when recorded, the sampled SHA-256 (SampledSha256) must match --
    // or, with no sample recorded, the whole file's SHA-256. Only recipes are checked (a whole replacement needs no base).
    struct BaseCheck {
        std::string path;                  // the disc path the recipe composes (any case and separators)
        uint64_t bytes = 0;
        std::string sha256;                // the whole file's, lower-case hex ("" = not recorded)
        std::string sample_sha256;         // SampledSha256 of it ("" = not recorded)
        uint32_t sample_windows = 0;
        uint64_t sample_window_bytes = 0;
    };
    // A file a layer COMPUTES from the file below it (the image's, or an earlier layer's): `make` gets the whole file below
    // (empty when there is none) and writes the new contents, or fails with *why (the layer is then refused whole, like a
    // malformed recipe). Kept in memory. The overlay knows nothing of the format: the caller's function does (the region
    // loader renames an archive index entry this way).
    struct Computed {
        std::string path;   // the disc path (any case and separators)
        std::function<bool(const std::vector<uint8_t>& below, std::vector<uint8_t>* out, std::string* why)> make;
    };
    struct Layer {
        Layer() = default;
        Layer(std::string layer_id, std::string layer_dir, std::vector<BaseCheck> base_checks = {})
            : id(std::move(layer_id)), dir(std::move(layer_dir)), checks(std::move(base_checks)) {}
        std::string id;    // for the report
        std::string dir;   // the layer's folder: its disc/ tree and the files its recipes name ("" = none)
        std::vector<BaseCheck> checks;   // what its recipes were made against (none: not checked)
        std::vector<Computed> computed;  // files computed from the volume below, after the disc/ tree's
    };
    struct Report {
        std::vector<std::string> applied;   // the layers composed, in order
        std::vector<std::string> refused;   // "<id>: why" for each layer left out
        std::vector<std::string> files;     // one line per composed disc file (path, form, size, placement)
        uint32_t overrides = 0;             // disc files replaced or added (distinct paths)
        uint32_t relocated = 0;             // ... of them in the virtual region (grown or added)
        uint32_t base_checks = 0;           // the recipes' base checks made (BaseCheck: the size, then the sampled / whole SHA-256)
        uint64_t base_check_ns = 0;         // ... and their time (the reads of the file below and the hashing: the device's cost at boot)
    };

    explicit DiscOverlay(std::vector<Layer> layers);
    ~DiscOverlay() override;

    // The image, then the layers composed over it (a refused layer does not fail the open)
    bool Open(const std::string& path, std::string* error = nullptr) override;
    void Close() override;

    bool Find(const std::string& path, Entry& out) const override;
    bool List(const Entry& dir, std::vector<Entry>& out) const override;
    size_t ReadBytes(uint64_t offset, size_t size, uint8_t* dst) const override;
    uint64_t ImageBytes() const override { return image_end_; }

    const Report& GetReport() const { return report_; }
    // Reads that touched a composed file's bytes (ReadBytes / ReadSectors calls)
    uint64_t Hits() const { return hits_.load(std::memory_order_relaxed); }
    uint32_t Overrides() const { return uint32_t(files_.size()); }
    bool Empty() const { return intervals_.empty() && files_.empty(); }

    // A path in the overlay's form: no device prefix, no version, '/' separators, upper case ("CRASH6/CRASH.BD")
    static std::string NormalizePath(const std::string& path);

    // The sampled SHA-256 the base check compares (the importer records it with the same function): `windows` windows of
    // `window_bytes` bytes spread evenly over the file -- window i starts at (size - window_bytes) * i / (windows - 1), so
    // the first starts at 0 and the last ends at the file's end -- hashed in order into one SHA-256; the whole file when
    // it is no larger than windows * window_bytes. `read(offset, n, dst)` reads n bytes of the file. False when a read
    // fails or the parameters are out of range (windows 1..4096, window_bytes 1..16 MiB).
    using ReadFn = std::function<bool(uint64_t offset, size_t n, uint8_t* dst)>;
    static bool SampledSha256(const ReadFn& read, uint64_t size, uint32_t windows, uint64_t window_bytes, std::string* hex);
    static bool WholeSha256(const ReadFn& read, uint64_t size, std::string* hex);

private:
    // One piece of a composed file: bytes of the image (absolute image offset), of a layer's file (an open descriptor), or zeros
    struct Segment {
        enum class Kind : uint8_t { Image, Payload, Zero, Memory };
        Kind kind = Kind::Zero;
        int fd = -1;
        uint64_t at = 0;       // Image: image byte offset; Payload: offset in the file; Memory: offset in the buffer
        uint64_t length = 0;
        uint64_t start = 0;    // the segment's offset in the composed file (prefix sum)
        const uint8_t* mem = nullptr;   // Memory: a buffer of buffers_ (alive until Close)
    };
    struct File {
        std::string path;          // normalized
        std::string name;          // the identifier List answers ("CRASH.BD;1")
        uint32_t parent = 0;       // the LBA of the directory that lists it (a real or a synthesized directory)
        uint8_t date[7] = {};
        bool on_image = false;     // the image has this path (else it is added)
        uint32_t image_lba = 0, image_size = 0;
        uint64_t size = 0;
        std::vector<Segment> segments;
        uint32_t lba = 0;          // where it is now
        uint32_t sectors = 0;      // the sector range it serves (in place: the original allocation)
        std::string form;          // "replace" | "add" | "ranges" (the last layer's)
        std::vector<std::string> layers;   // the layers that touched it
    };
    struct Dir {                   // a synthesized directory (an added file's folder the image lacks)
        std::string path, name;
        uint32_t parent = 0;
        uint32_t lba = 0;
    };
    struct Interval {
        uint64_t begin = 0, end = 0;   // image bytes [begin, end)
        uint32_t file = 0;             // index into files_
    };

    bool ComposeLayer(const Layer& layer, std::string* why);
    bool CheckBase(const BaseCheck& check, const std::vector<Segment>& base, uint64_t base_size, std::string* why) const;
    bool ParentOf(const std::string& path, std::vector<Dir>* new_dirs, std::string* why) const;
    void Layout();
    size_t ReadComposed(const File& f, uint64_t pos, size_t n, uint8_t* dst) const;
    size_t ReadSegments(const std::vector<Segment>& segments, uint64_t size, uint64_t pos, size_t n, uint8_t* dst) const;
    int PayloadFd(const std::string& host_path, std::string* why);
    void Release();

    std::vector<Layer> layers_;
    Report report_;
    std::vector<File> files_;                     // sorted by path after Layout
    std::map<std::string, uint32_t> by_path_;     // normalized path -> files_ index
    std::vector<Dir> dirs_;                       // synthesized directories
    std::map<std::string, uint32_t> dir_by_path_;
    std::vector<Interval> intervals_;             // sorted, disjoint
    std::map<std::string, int> fds_;              // host path -> descriptor (the layers' files, open until Close)
    std::vector<std::unique_ptr<std::vector<uint8_t>>> buffers_;   // the computed files' bytes (Segment::Kind::Memory)
    uint64_t image_end_ = 0;                      // ImageBytes(): the image's, or the virtual region's end
    mutable std::atomic<uint64_t> hits_{0};
};
