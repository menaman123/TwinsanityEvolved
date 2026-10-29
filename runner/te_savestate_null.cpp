// te_savestate_null.cpp -- the save-state runner's interface (te_savestate.h) on a target without the mechanism: the
// PlayStation Vita lane (TE_NO_SAVESTATES, CMakeLists.txt). A save state there would need the image at a fixed address, a
// second process to relaunch into and the host stack walk (te_savestate.cpp's dyld / dl / spawn rows); none exists on the
// device, so every entry refuses with a message and the sections are not registered (F5 / F8 / --save-state / --load-state
// say so instead of writing a file no process could load). The build id is still real: the engine's executable path hashed.
#include "te_savestate.h"

#include <fstream>
#include <iterator>

#include "engine/core/savestate/savestate_format.h"
#include "engine/platform/host/platform.h"

namespace te::savestate_runner {

namespace {
uint32_t g_build_id = 0;
const char* const kRefusal = "save states are not available on this target (no fixed-address image / relaunch)";
}  // namespace

uint32_t BuildId() {
    if (g_build_id) return g_build_id;
    std::string err;
    const std::string path = ps2r::platform::host::ExecutablePath(&err);
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    g_build_id = bytes.empty() ? 1u : ps2r::savestate::Crc32(bytes.data(), bytes.size());
    return g_build_id;
}

ImageInfo Image() { return ImageInfo{}; }

void EnsureNoAslr(int argc, char** argv, std::string* warning) {
    (void)argc; (void)argv;
    if (warning) *warning = kRefusal;
}

void Register(w2c_env& env, w2c_game& instance) { (void)env; (void)instance; }

bool Save(const std::string& path, uint64_t boundary, std::string* err) {
    (void)path; (void)boundary;
    if (err) *err = kRefusal;
    return false;
}

bool WriteSidecar(const std::string& path, uint64_t boundary, const std::string& detail, std::string* err) {
    (void)path; (void)boundary; (void)detail;
    if (err) *err = kRefusal;
    return false;
}

bool Load(const std::string& path, std::string* err) {
    (void)path;
    if (err) *err = kRefusal;
    return false;
}

uint64_t LoadedBoundary() { return 0; }

}  // namespace te::savestate_runner
