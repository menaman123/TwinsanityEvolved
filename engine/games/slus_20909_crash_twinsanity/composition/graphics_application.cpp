#include "games/slus_20909_crash_twinsanity/composition/graphics_application.h"

#include <cstdio>
#include <string>

#include "app/host/texture_pack_path.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/savestate/quiescence.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ipu/hle/guest_memory.h"
#include "engine/ipu/hle/libmpeg_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/screen_aspect.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace ps2r::app {
gs::AspectSelection::Shape ShapeOf(AspectSel selection) {
    static_assert(static_cast<int>(gs::AspectSelection::Shape::Auto) == static_cast<int>(AspectSel::Auto) &&
                  static_cast<int>(gs::AspectSelection::Shape::Narrow) == static_cast<int>(AspectSel::R4_3) &&
                  static_cast<int>(gs::AspectSelection::Shape::Wide16_9) == static_cast<int>(AspectSel::R16_9) &&
                  static_cast<int>(gs::AspectSelection::Shape::Wide16_10) == static_cast<int>(AspectSel::R16_10) &&
                  static_cast<int>(gs::AspectSelection::Shape::Wide21_9) == static_cast<int>(AspectSel::R21_9) &&
                  static_cast<int>(gs::AspectSelection::Shape::Wide32_9) == static_cast<int>(AspectSel::R32_9),
                  "video.aspect and AspectSelection::Shape share one order");
    return static_cast<gs::AspectSelection::Shape>(selection);
}

render::TextureFilter FilterOf(TextureFilterSel selection) {
    static_assert(static_cast<int>(render::TextureFilter::Title) == static_cast<int>(TextureFilterSel::Title) &&
                  static_cast<int>(render::TextureFilter::Point) == static_cast<int>(TextureFilterSel::Point) &&
                  static_cast<int>(render::TextureFilter::Bilinear) == static_cast<int>(TextureFilterSel::Bilinear) &&
                  static_cast<int>(render::TextureFilter::Trilinear) == static_cast<int>(TextureFilterSel::Trilinear),
                  "video.texture_filter and render::TextureFilter share one order");
    return static_cast<render::TextureFilter>(selection);
}

GraphicsApplication::GraphicsApplication(MainMemory& memory, ipu::GuestMemory& guest,
    ::title::MovieDecoder& decoder, ipu::LibmpegBindings& mpeg, GraphicsOutput& output,
    VideoStack& video, const Config& config, std::function<void()> stop_producers,
    std::shared_ptr<gs::AspectSelection> aspect, bool has_window)
    : memory_(memory), guest_(guest), decoder_(decoder), mpeg_(mpeg), output_(output),
      video_(video), selection_(config.hle), pack_dir_(TexturePackPath::Resolve(config)),
      dump_dir_(ConfigLoader::Request().dump_textures_dir), stop_producers_(std::move(stop_producers)),
      aspect_(std::move(aspect)),
      aspect_live_(aspect_ && gs::AspectSelection::Live(aspect_->Selection(), has_window)) {}
GraphicsApplication::~GraphicsApplication() { StopAfterQuiescence(); }

bool GraphicsApplication::Install() {
    if (movie_) return false;
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, DispatchTable::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    const auto& selected = selection_;
    if (selected.graphics_sprites || selected.graphics_text || selected.graphics_sky || selected.graphics_textures ||
        selected.graphics_meshes || selected.graphics_movie || selected.graphics_frame ||
        selected.graphics_strips == GraphicsStrips::All) {
        if (!video_.Active()) {
            std::fprintf(stderr, "nativeee: selected Graphics producers require a live shared scene\n");
            return false;
        }
        // The title's main thread runs on the scratchpad (TR entry.cpp:83): the
        // Stream a TextureRead receives and placed-sprite matrices are stack
        // objects there, read through the root's guest translation.
        const GraphicsRuntime::Window scratchpad{guest_.Translate(Scratchpad::kBase, Scratchpad::kSize),
                                                 Scratchpad::kSize};
        // The META arena (hle.meta_arena): GetDiskManager's 0x20-byte block
        // descriptors live there, and the sky owner's load-boundary conversion
        // reads a model's bytes through them (SkyAssets::ModelLoaded). Without
        // this window every sky load was declined and every selected sky pass
        // refused `missing_resource` (producer 0x1b9cf8 detail 4).
        static_assert(MetaArena::kBase == GraphicsProducers::kArenaBase, "the producers' arena window is the META arena");
        const GraphicsRuntime::Window arena{guest_.Translate(MetaArena::kBase, MetaArena::kSize), MetaArena::kSize};
        runtime_ = std::make_unique<GraphicsRuntime>(memory_, output_, video_, GraphicsRuntime::Options{
            selected.graphics_sprites, selected.graphics_text, selected.graphics_sky,
            selected.graphics_movie, true, // existing authored-chain enhancement policy
            selected.graphics_textures, pack_dir_, dump_dir_, selected.graphics_meshes},
            scratchpad.data ? scratchpad : GraphicsRuntime::Window{},
            arena.data ? arena : GraphicsRuntime::Window{});
        if (!runtime_->Install(capture, install, [](CpuContext& context, uint32_t object) {
                context.cpuRegs.GPR.r[4].UD[0] = object;
                DispatchTable::Invoke(0x181590, context);
            })) {
            std::fprintf(stderr, "nativeee: selected Graphics producers refused installation\n");
            return false;
        }
        if (frame_clock_) runtime_->SetFrameClock(frame_clock_);
    }
    GraphicsMovie::Hooks hooks;
    hooks.selected = selected.graphics_movie;
    hooks.step = selected.movie_step;
    hooks.start = selected.movie_start;
    hooks.wait = selected.movie_wait;
    hooks.stop = selected.movie_stop;
    hooks.call = [](CpuContext& context, uint32_t address) { DispatchTable::Invoke(address, context); };
    hooks.picture = [this](CpuContext& context, auto& picture) { mpeg_.ReadPlaybackPicture(context,picture); };
    hooks.submitted = [this](auto& frame) { return output_.CaptureSubmittedScene(frame); };
    hooks.select = [this](const auto& frame) {
        if (selection_.graphics_frame)
            return output_.EnqueueOwned([frame](VideoStack& video) { return video.DisplayMatches(frame); });
        return output_.EnqueueOwned([frame](VideoStack& video) { return video.SelectMovieFrame(frame); });
    };
    hooks.clear = [this] {
        return output_.EnqueueOwned([](VideoStack& video) { video.ClearMovieFrame(); return true; });
    };
    movie_ = std::make_unique<GraphicsMovie>(guest_, decoder_, std::move(hooks),
        runtime_ ? runtime_->MovieOutput() : ::title::MovieGraphics::OutputServices{});
    if (aspect_live_ && aspect_) {
        // The movie picture follows the selected shape (TR movie.cpp:224's rule
        // at the ratio); 4:3 leaves the source rule in force.
        movie_->SetAspectSource([aspect = aspect_] { return aspect->Ratio(); });
        screen_aspect_ = std::make_unique<::title::ScreenAspect>(guest_, [aspect = aspect_] { return aspect->Ratio(); });
        if (!screen_aspect_->Install(capture, [&](uint32_t address, DispatchTable::Body body) {
                // Interior entries (a restored continuation inside Update /
                // SetViewFrustum) run the original; the aspect argument of a
                // full entry is the model's.
                return install(address, DispatchTable::DelegateInterior(address, DispatchTable::Capture(address), std::move(body)));
            })) {
            std::fprintf(stderr, "nativeee: the screen aspect model refused installation\n");
            return false;
        }
    }
    std::string reason;
    if (!movie_->Install(capture, [&](uint32_t address, DispatchTable::Body body) {
        // The ordinary Present (0x1a0150) is LIVE on the boundary thread's
        // chain at every SyncV (TR graphics.cpp:548-559: WaitIdle -> SyncV ->
        // SendToGif -> SetUpDisplay), so a live capture needs its post-SyncV
        // continuation proven. The selected transport runs the ORIGINAL at
        // every entry (interior entries pass straight through; the owned
        // picture select/release follows the full entry's return), so it
        // keeps the original's exact continuation proof (TraceBody) -- an
        // opaque body refused every in-play boundary ("may contain omitted
        // frames (depth=5, innermost frame fn=0x001a0150)"). A deselected
        // transport delegates its interior entries as before.
        const auto original = DispatchTable::Capture(address);
        body = selected.graphics_movie ? DispatchTable::TraceBody(address, original, std::move(body))
                                       : DispatchTable::DelegateInterior(address, original, std::move(body));
        return install(address, std::move(body));
    }, &reason)) {
        std::fprintf(stderr, "nativeee: %s\n", reason.c_str());
        return false;
    }
    frame_ = std::make_unique<GraphicsFrame>(memory_,output_,video_,selected.graphics_frame);
    if (frame_clock_) frame_->SetFrameClock(frame_clock_);
    if (frame_->Install(capture,[&](uint32_t address,DispatchTable::Body body) {
        if (!selected.graphics_frame)
            body = DispatchTable::DelegateInterior(address,DispatchTable::Capture(address),std::move(body));
        return install(address,std::move(body));
    })) return true;
    std::fprintf(stderr, "nativeee: Graphics frame owner refused installation\n");
    return false;
}

bool GraphicsApplication::InstallMoviePlayer() {
    if (!movie_) {
        std::fprintf(stderr, "nativeee: Movie player owners require the installed Graphics application\n");
        return false;
    }
    std::string reason;
    if (movie_->InstallPlayer([](uint32_t address) { return DispatchTable::Capture(address); },
                              [&](uint32_t address, DispatchTable::Body body) {
                                  // The same installer as the pictures: a deselected
                                  // Movie keeps its interior continuations delegated.
                                  if (!selection_.graphics_movie)
                                      body = DispatchTable::DelegateInterior(address, DispatchTable::Capture(address), std::move(body));
                                  return DispatchTable::Install(address, std::move(body));
                              }, &reason)) return true;
    std::fprintf(stderr, "nativeee: %s\n", reason.c_str());
    return false;
}

void GraphicsApplication::SetFrameClock(std::function<uint64_t()> host_frame) {
    frame_clock_ = std::move(host_frame);
    output_.SetFrameClock(frame_clock_);
    if (runtime_) runtime_->SetFrameClock(frame_clock_);
    if (frame_) frame_->SetFrameClock(frame_clock_);
}

void GraphicsApplication::SetCameraObserver(std::function<void(const float* to_camera16, const float* camera_to_screen16)> observer) {
    if (screen_aspect_) screen_aspect_->SetCameraObserver(std::move(observer));
}

void GraphicsApplication::ApproveLegacyImport(const std::string& inventory, const std::string& load_file) {
    if (!runtime_ || import_) return;
    const auto& selected = selection_;
    import_ = std::make_unique<GraphicsImport>(GraphicsImport::Options{
        selected.graphics_sprites, selected.graphics_text, selected.graphics_sky, selected.graphics_movie, true,
        selected.graphics_textures, pack_dir_, dump_dir_, selected.graphics_meshes});
    import_->Approve(inventory, load_file);
    runtime_->SetImport(import_.get());
    const auto prepared = [this] { return import_ && import_->Prepared(); };
    output_.SetLegacyImport(prepared);
    frame_->SetLegacyImport(prepared);
}

bool GraphicsApplication::RegisterState(StateRestorer& restorer) {
    return movie_ && frame_ && (!runtime_ ||
        (runtime_->RegisterState(restorer) && output_.RegisterSourceState(restorer))) &&
        frame_->RegisterState(restorer);
}
void GraphicsApplication::RegisterConditions(savestate::Quiescence& conditions) {
    conditions.Register("graphics_movie", [this](auto* reason) { return movie_->CanCheckpoint(reason); });
    conditions.Register("graphics_frame", [this](auto* reason) { return frame_->CanCheckpoint(reason); });
    if (runtime_) conditions.Register("graphics_resources", [this](auto* reason) {
        return runtime_->CanCheckpoint(reason);
    });
}
bool GraphicsApplication::CanRestore(std::string* reason) {
    return output_.CanCheckpoint(reason) && movie_->CanCheckpoint(reason) && frame_->CanCheckpoint(reason) &&
        (!runtime_ || runtime_->CanCheckpoint(reason));
}
void GraphicsApplication::Record(verify::RunMeta& meta) const {
    // video_aspect: the ratio in force at the end of the leg and the model's
    // census (its four entries' owned/passed calls); `4:3,model=off` when the
    // selection composes nothing (the identity lanes).
    meta.Set("video_aspect", screen_aspect_ ? screen_aspect_->Census() :
        std::string("ratio=") + gs::AspectSelection::Name(aspect_ ? aspect_->Ratio() : gs::AspectSelection::kNarrow) + ",model=off");
    output_.Record(meta);
    movie_->Record(meta);
    frame_->Record(meta);
    if (runtime_) runtime_->Record(meta);
    if (import_) import_->Record(meta);
}
void GraphicsApplication::StopAfterQuiescence() {
    if (stopped_) return;
    if (stop_producers_) stop_producers_();
    output_.StopAfterQuiescence();
    stopped_ = true;
}
}  // namespace ps2r::app
