// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "app/host/build_id.h"
#include "app/host/display_sequence.h"
#include "engine/ee/timing/tick_profile.h"
#include "engine/core/verify/frame_hasher.h"
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"
#include "app/host/live_restore_gate.h"
#include "app/host/savestate_slots.h"
#include "games/slus_20909_crash_twinsanity/composition/savestate_trigger.h"
#include "app/host/session_relaunch.h"
#include "engine/platform/sdl/input/input_mapper.h"

namespace {
// The close's guest-join bound (ms): the guest workers normally finish in
// < 20 ms (shutdown census guest_join=6..16 across the diag runs); past this a
// worker is parked somewhere the cancel does not wake and is abandoned, named.
constexpr uint64_t kGuestJoinDeadlineMs = 500;

// --set harness.inject_key=<KEY>@N[-M][,...]: a synthetic PRESS through the
// real key route (SDL's queue -> the main pump -> SessionGui -> the pad's
// KeyboardMouseBackend / SessionControls), one per item, at the first
// boundary >= N; with "-M" the key is HELD through boundary M (the KEYUP is
// pushed there; meanwhile the kbm backend's injection overlay reads the
// scancode as down -- SDL's keyboard-state array ignores pushed events). The
// validator pinned the format; an unknown key name is refused loudly here.
struct HarnessKeyPress { int number; int scancode; uint64_t frame, until; bool pressed = false, released = false; std::string name; };

bool ParseHarnessKeyPresses(const std::string& spec, std::vector<HarnessKeyPress>& out) {
    for (std::string rest = spec; !rest.empty();) {
        const size_t comma = rest.find(',');
        const std::string item = rest.substr(0, comma);
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        HarnessKeyPress press;
        press.name = item.substr(0, at);
        press.frame = std::strtoull(item.c_str() + at + 1, nullptr, 10);
        const size_t dash = item.find('-', at + 1);
        press.until = dash == std::string::npos ? 0 : std::strtoull(item.c_str() + dash + 1, nullptr, 10);
        press.number = (press.name.size() >= 2 && press.name.size() <= 3 && press.name[0] == 'F' &&
                        std::atoi(press.name.c_str() + 1) >= 1 && std::atoi(press.name.c_str() + 1) <= 12)
                           ? std::atoi(press.name.c_str() + 1) : 0;
        press.scancode = ps2r::platform::sdl::ScancodeFromName(press.name);
        if (press.scancode <= 0) {
            std::fprintf(stderr, "nativeee: harness.inject_key: unknown key name '%s'\n", press.name.c_str());
            return false;
        }
        out.push_back(press);
    }
    return true;
}

// One boundary's step of a harness key press: the press at the first boundary
// >= frame (released at once unless held), the release at the boundary >= until.
void StepHarnessKeyPress(HarnessKeyPress& press, ps2r::app::SessionRuntime& session, PlatformInput* live_input,
                         uint64_t boundary) {
    if (press.released || boundary < press.frame) return;
    if (!press.pressed) {
        press.pressed = true;
        const bool pushed = press.number ? session.InjectFunctionKey(press.number)
                                         : session.InjectKey(press.scancode, true);
        if (press.until > press.frame && live_input) live_input->HoldKey(press.scancode, true);
        std::fprintf(stderr, "nativeee: harness.inject_key %s at boundary %llu: %s%s\n", press.name.c_str(),
                     static_cast<unsigned long long>(boundary),
                     pushed ? "pushed into SDL's queue" : "SDL_PushEvent refused",
                     press.until > press.frame ? " (held)" : "");
        if (press.number || press.until <= press.frame) {
            press.released = true;
            if (!press.number) session.InjectKey(press.scancode, false);
        }
    } else if (boundary >= press.until) {
        press.released = true;
        if (live_input) live_input->HoldKey(press.scancode, false);
        session.InjectKey(press.scancode, false);
        std::fprintf(stderr, "nativeee: harness.inject_key %s released at boundary %llu\n", press.name.c_str(),
                     static_cast<unsigned long long>(boundary));
    }
}

// --set harness.virtual_pad=attach@N[,press@K-L][,detach@M]: the hot-plugged
// SDL virtual game controller (RootPadHotplug), each verb at its boundary on
// the input owner (PlatformInput's work queue). The validator pinned the format.
struct HarnessPadVerb { PlatformInput::VirtualPad action; uint64_t frame; bool done = false; const char* name; };

std::vector<HarnessPadVerb> ParseHarnessPadVerbs(const std::string& spec) {
    std::vector<HarnessPadVerb> verbs;
    for (std::string rest = spec; !rest.empty();) {
        const size_t comma = rest.find(',');
        const std::string item = rest.substr(0, comma);
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        const std::string verb = item.substr(0, at);
        const uint64_t from = std::strtoull(item.c_str() + at + 1, nullptr, 10);
        if (verb == "attach") verbs.push_back({PlatformInput::VirtualPad::Attach, from, false, "attach"});
        else if (verb == "detach") verbs.push_back({PlatformInput::VirtualPad::Detach, from, false, "detach"});
        else if (verb == "press") {
            const size_t dash = item.find('-', at + 1);
            verbs.push_back({PlatformInput::VirtualPad::Press, from, false, "press"});
            verbs.push_back({PlatformInput::VirtualPad::Release, std::strtoull(item.c_str() + dash + 1, nullptr, 10), false, "release"});
        }
    }
    return verbs;
}
}  // namespace

int EeBootstrap::ArmParkSink(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    const bool& play_mode = *s.play_mode;
    ps2r::app::SessionFiles& session_files = *s.session_files;
    EmuDomain& domain = *s.domain;
    std::atomic<bool>& running = *s.running;
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>& gs_queue = *s.gs_queue;
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer>& consumer = *s.consumer;
    ps2r::app::EffectsApplication& effects_application = *s.effects_application;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
    const bool& iop_booted = *s.iop_booted;
    ps2r::app::StartupOwnership& startup_ownership = *s.startup_ownership;
    GameBoundary& game_boundary = *s.game_boundary;
    DeliveryMeter& delivery_meter = *s.delivery_meter;
    RpcRouter& rpc_router = *s.rpc_router;
    std::unique_ptr<PlatformInput>& live_input = *s.live_input;
    ps2r::app::InputSave& input_save = *s.input_save;
    ps2r::platform::sdl::AudioPullRing& audio_ring = *s.audio_ring;
    ps2r::platform::sdl::SdlAudioSink& audio_sink = *s.audio_sink;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    BoundaryProtocol& protocol = *s.protocol;
    uint64_t& audio_pumped_tick = *s.audio_pumped_tick;
    ps2r::app::GraphicsApplication& graphics_application = *s.graphics_application;
    StateRestorer& restorer = *s.restorer;
    ps2r::app::SessionClock& session_clock = *s.session_clock;
    ps2r::app::ServiceRestore& service_restore = *s.service_restore;
    uint32_t& restored_vsync_frame = *s.restored_vsync_frame;
    ps2r::app::MovieCheckpoint& movie_checkpoint = *s.movie_checkpoint;
    ps2r::app::SessionCapture& checkpoint = *s.checkpoint;
    TickThread& tick_thread = *s.tick_thread;
    LegCloser& closer = *s.closer;
    std::function<void()>& write_census = *s.write_census;
    ps2r::gs::DrawlistRing& drawlist_ring = *s.drawlist_ring;
    // --- the crash path: the triage bundle (ALL-ON, NO EXCEPTIONS (1)) ---------
    // Any park but "none" -- an uncaught exception the leg caught at its edge, a
    // GuestPark, a std::terminate on any thread -- writes
    // <harness.out_dir | paths.log_dir>/triage/<UTC>/ {census.txt,
    // drawlist_last200.txt, fault.txt, backtrace.txt, run.meta} before the
    // process ends, from LegCloser::Close (run.meta already carries the park
    // keys and is finalized). The backtrace is the throw site noted on the
    // throwing thread (TriageBundle::NoteThrowSite at the catch / the park).
    using ps2r::triage::TriageBundle;
    closer.SetFaultSink([&](const char* park, uint32_t pc, int tid, const char* detail) {
        TriageBundle::Files files;
        auto* meta = host.Meta();
        if (meta) files.census = files.run_meta = meta->Render();
        files.drawlist = drawlist_ring.Dump();
        TriageBundle::Fault fault;
        fault.park = park;
        fault.pc = pc;
        fault.tid = tid;
        fault.detail = detail;
        ps2r::gs::DrawlistRing::DrawRecord last;
        fault.last_draw = drawlist_ring.Last(&last) ? drawlist_ring.Describe(last) : "none";
        // The producer: the Graphics ledgers' rows of the census (refusal /
        // fault sites carry frame, source frame, producer entry address and
        // detail code) and the owned-output / service rows.
        {
            std::string producer;
            size_t start = 0;
            while (start < files.census.size()) {
                size_t end = files.census.find('\n', start);
                if (end == std::string::npos) end = files.census.size();
                const std::string line = files.census.substr(start, end - start);
                start = end + 1;
                if (line.rfind("graphics_refusals_", 0) == 0 || line.rfind("graphics_output_faulted", 0) == 0 ||
                    line.rfind("graphics_service", 0) == 0 || line.rfind("graphics_owned_output", 0) == 0 ||
                    line.rfind("graphics_movie_transport", 0) == 0)
                    producer += (producer.empty() ? "" : "\n  ") + line;
            }
            fault.producer = producer.empty() ? "none (no Graphics ledger rows in the census)" : producer;
        }
        files.fault = TriageBundle::RenderFault(fault);
        const std::string root = cfg.harness.out_dir.empty() ? cfg.paths.log_dir : cfg.harness.out_dir;
        std::string err;
        const std::string dir = TriageBundle::Write(root, files, &err);
        if (dir.empty()) {
            std::fprintf(stderr, "nativeee: triage bundle NOT written: %s\n", err.c_str());
            return;
        }
        std::fprintf(stderr, "nativeee: triage bundle written: %s\n", dir.c_str());
        if (meta) {
            meta->Set("triage_bundle", dir);
            meta->Flush();
        }
    });
    // An exception no handler catches (another thread's) still reaches the
    // bundle: the terminate hook closes the leg as park=exception on the
    // throwing thread, then aborts.
    TriageBundle::InstallTerminateHook([&](const char* what) {
        write_census();
        closer.Close("exception", 0, ThreadManager::Instance().CurrentTid(), what);
    });
    // The hook captures this frame's locals: cleared when the leg returns.
    static int terminate_hook_token = 0;
    const std::unique_ptr<int, void (*)(int*)> terminate_hook_reset(&terminate_hook_token,
                                                                    [](int*) { TriageBundle::ClearTerminateHook(); });
    // --- the park sink: a park is the leg's result ----------------------------
    GuestPark::SetSink([&](const ParkRecord& r) {
        TriageBundle::NoteThrowSite(r.detail ? r.detail : r.reason);
        write_census();
        closer.Close(r.reason, r.pc, ThreadManager::Instance().CurrentTid(), r.detail);
        if (session) {
            session->RequestClose();
            throw ThreadExitUnwind{};
        }
        std::_Exit(0);
    });
    ps2r::app::SessionWorkers session_workers(ThreadManager::Instance(), running,
        [&] { tick_thread.Stop(); }, [&] { protocol.NotifyTick(); });
    std::unique_ptr<ps2r::app::SessionRestore> session_restore;
    std::unique_ptr<ps2r::app::SessionRelaunch> session_relaunch;
    std::unique_ptr<ps2r::app::SessionAudio> session_audio;
    // The live gate's probes/census outlive the block: the lifecycle lambda the
    // transaction holds refers to them for the leg (this frame nests the later
    // phases through the tail call below).
    ps2r::app::LiveRestoreGate::Census live_refusals;
    ps2r::app::LiveRestoreGate::Probes live_probes;
    if (session) {
        ps2r::app::SessionRuntime::Hooks hooks;
        hooks.stop = [&] { session_workers.Cancel(); };
        hooks.pause = [&](bool paused) {
            input_save.Pause(paused);
            if (paused) tick_thread.Stop(); else tick_thread.Start();
            if (!session->OnMain([&, paused] { audio_sink.SetPaused(paused); }) && !session->Closing())
                throw std::runtime_error("Audio pause owner unavailable.");
        };
        checkpoint.Bind(hooks);
        ps2r::app::SessionRestore::Lifecycle restore_lifecycle;
        // The live F8's admission (LiveRestoreGate
        // 2.5): each asynchronous owner that holds state no section carries
        // refuses BY NAME with its count (census `live_restore_refusals=`);
        // nothing is dropped to admit a boundary -- F8 again at the next one.
        if (mtvu) live_probes.mtvu_quiesce = [&](std::string* why) { return mtvu->Quiesce(why); };
        live_probes.events_pending = [] { return EeEventQueue::Instance().HasPending(); };
        live_probes.rpc_pending = [&] { return rpc_router.PendingCount(); };
        live_probes.deliveries_pending = [&] { return delivery_meter.Pending(); };
        live_probes.owners = [&](std::string& error) {
            return input_save.CanCheckpoint(&error) && movie_checkpoint.CanCheckpoint(&error) &&
                service_restore.CanCheckpoint(&error);
        };
        restore_lifecycle.inspect = [&](std::string& error) {
            if (iop_booted || !startup_ownership.Owned()) {
                // The named list of retained modules and their consumers (the
                // startup contract); STREAM's running state is one of them.
                error = startup_ownership.Refusal();
                if (error.empty()) error = "Live restore requires service-only startup; STREAM still owns running state.";
                return false;
            }
            if (ps2r::app::LiveRestoreGate::Inspect(live_probes, live_refusals, error)) return true;
            if (auto* meta = host.Meta()) { meta->Set("live_restore_refusals", live_refusals.Render()); meta->Flush(); }
            return false;
        };
        restore_lifecycle.prepare = [&](std::string& error) {
            return graphics_application.CanRestore(&error);
        };
        session_audio = std::make_unique<ps2r::app::SessionAudio>(audio_sink, audio_ring,
            [&](auto operation) { return session->OnMain(std::move(operation)); });
        restore_lifecycle.joined = [&](std::string& error) {
            checkpoint.Join();
            // The display's memory of the OLD session:
            // the pipelined consumer's published grab and the present
            // policy's held image are the previous session's last pictures; a
            // boundary of the restored session must never present them. Drain the
            // queue (the records of the final pre-restore boundary), forget the grab
            // ON the consumer (stream order) and the held image here (the boundary
            // thread is joined): the first restored boundary presents nothing until
            // its own first image completes -- what a fresh process shows.
            if (gs_queue) {
                gs_queue->Sync();
                if (consumer) gs_queue->RunOrdered([&] { consumer->ForgetLastFrame(); return true; });
            } else if (consumer) {
                consumer->ForgetLastFrame();
            }
            s.held_rgba->clear();
            *s.held_w = 0;
            *s.held_h = 0;
            s.present_policy->ForgetImage();
            return session_audio->StopForRestore(error);
        };
        restore_lifecycle.resumed = [&](uint32_t frame, std::string& error) {
            restored_vsync_frame = frame;
            audio_pumped_tick = domain.DomainTick();
            // The restored segment's provenance (its run.meta is current now):
            // the image's RAM hash as --load-state reports it (restore.mhash /
            // restore_mhash) -- the identity between a live restore and a
            // process restore of the same slot -- and the restored frame.
            if (auto* meta = host.Meta()) {
                meta->Set("restore_mhash", Hex(restorer.RestoredMemoryHash()));
                meta->Set("restored_vsync_frame", std::to_string(frame));
                meta->Set("restore_route", "live");
                meta->Flush();
            }
            if (live_input) live_input->ResetPending();
            if (s.menu_extension) s.menu_extension->NoteRestored();
            input_save.Pause(session->Paused());
            if (!session_audio->ResumeAfterRestore(session->Paused(), error)) return false;
            running.store(true);
            if (play_mode && !session->Paused()) tick_thread.Start();
            return true;
        };
        session_restore = std::make_unique<ps2r::app::SessionRestore>(*session, session_workers,
            restorer, session_clock, game_boundary, host, session_files, std::move(restore_lifecycle));
        // The F8 route. LIVE (save.restore_mode=live, the default, or
        // the root opt-in harness.live_restore): the newest slot restores IN this
        // process -- the boundary thread quiesced at the admitted boundary,
        // Graphics settled, every owner's restore on the live composition, the
        // presenter/audio re-armed, the window and the GL device kept. RELAUNCH
        // (save.restore_mode=relaunch, or a composition that still retains IOP
        // state -- the list `StartupOwnership` names, where a live rewind
        // is refused by contract): the former path, this binary re-executed
        // from the latest slot through its own --load-state after the slot is
        // preflighted against the live registry AND the live composition. The
        // route and, for a fallback, its reason are the census `f8=` row.
        using LiveGate = ps2r::app::LiveRestoreGate;
        const LiveGate::Route f8_route = LiveGate::Choose(cfg.save.restore_mode, cfg.harness.live_restore,
                                                          startup_ownership.Owned(), iop_booted);
        if (auto* meta = host.Meta()) meta->Set("f8", LiveGate::RouteCensus(f8_route));
        if (LiveGate::Live(f8_route)) {
            if (!session_restore->Bind(hooks, true)) return 2;
            // The slot's sidecar against the live composition, with the session
            // still open (the relaunch route's preflight, kept on the live one): a
            // slot of another profile / hle.* composition is refused HERE by name;
            // a differing build_id= is a warning. Then the transaction's own
            // preflight (the registry, the clock/providers, every owner).
            const auto live_restore = hooks.restore;
            hooks.restore = [live_restore, live_composition = ps2r::app::SavestateSlots::Composition(cfg)](
                                const ps2r::app::SessionControls::Request& request, uint64_t frame) {
                std::string error, warning;
                if (!request.path.empty() &&
                    !ps2r::app::SavestateSlots::PreflightComposition(request.path, live_composition, g_build_id,
                                                                     &error, &warning))
                    return ps2r::app::SessionRuntime::CaptureResult{ps2r::app::SessionControls::Phase::Refused,
                                                                    request.path + ": " + error};
                if (!warning.empty()) ps2r::app::SavestateSlots::Report("F8 restore", "Warning", request.path + ": " + warning);
                return live_restore(request, frame);
            };
        } else {
            if (f8_route == LiveGate::Route::RelaunchFallback) {
                std::string why = startup_ownership.Refusal();
                if (why.empty()) why = "the IOP booted (irx entry ran); its running state has no live rewind";
                std::fprintf(stderr, "nativeee: F8 restore route: relaunch (live refused: %s)\n", why.c_str());
            }
            session_relaunch = std::make_unique<ps2r::app::SessionRelaunch>(session_workers,
                restorer.Registry(), ps2r::app::SavestateSlots::Directory(cfg),
                ps2r::app::SavestateSlots::Composition(cfg), g_build_id);
            session_relaunch->Bind(hooks);
        }
        // F8 with nothing selected restores the newest slot<N>.ps2st.
        session->Controls().SetRestoreFallback([slots = ps2r::app::SavestateSlots::Directory(cfg)](std::string* reason) {
            return ps2r::app::SavestateSlots::Latest(slots, reason);
        });
        // Every F5/F8 result is LOUD: the key press is acknowledged on stderr the
        // moment it is queued, and every terminal result (written / refused by
        // a named owner / failed) reaches stderr + the SAVE log, never only a
        // line in the menu (U4/U5; the all-on play session's silent F5).
        session->Controls().SetObserver([](const ps2r::app::SessionControls::Result& result) {
            using Action = ps2r::app::SessionControls::Action;
            if (result.request.action != Action::SaveReproduction && result.request.action != Action::RestoreReproduction)
                return;
            ps2r::app::SavestateSlots::Report(
                result.request.action == Action::SaveReproduction ? "F5 save" : "F8 restore",
                ps2r::app::SessionControls::PhaseLabel(result.phase), result.message);
        });
        hooks.alive = [&] { return session_workers.BoundaryAlive(); };
        // The shutdown ladder, in dependency order, each step timed into the
        // session's ShutdownCensus (run.meta shutdown=; the watchdog names the
        // step in progress): the guest workers first (they produce for every
        // consumer below), the savestate writer, the MTVU worker, Graphics'
        // quiescence, the GS consumer, the recorder's flush.
        hooks.stopped = [&] {
            using Step = ps2r::app::ShutdownCensus::Step;
            ps2r::app::ShutdownCensus& census = session->Shutdown();
            {
                // Bounded: a guest worker parked in a wait the cancel cannot
                // reach (the tester's frozen Quit tick) is abandoned after the
                // deadline and NAMED -- the close completes without it; the leg
                // then leaves through _Exit (no static teardown around a live
                // thread) once the census is on disk.
                Step step(census, "guest_join");
                std::string abandoned;
                if (session_workers.Join(kGuestJoinDeadlineMs, &abandoned) > 0) {
                    census.Note("guest_join_abandoned", abandoned);
                    std::fprintf(stderr, "nativeee: shutdown: guest workers abandoned after %u ms: %s\n",
                                 static_cast<unsigned>(kGuestJoinDeadlineMs), abandoned.c_str());
                }
            }
            { Step step(census, "savestate_writer"); checkpoint.Join(); }
            if (mtvu) { Step step(census, "mtvu_worker"); mtvu->Stop(); }
            { Step step(census, "graphics_quiesce"); graphics_application.StopAfterQuiescence(); }
            {
                Step step(census, "effects_quiesce");
                if (!effects_application.StopAfterQuiescence())
                    throw std::runtime_error("Effect resource shutdown lost the render owner");
            }
            if (gs_queue) { Step step(census, "gs_consumer"); gs_queue->Stop(); }
            { Step step(census, "recorder_flush"); host.FlushInput(); }
        };
        hooks.shutdown_report = [&](const std::string& body) {
            if (auto* meta = host.Meta()) { meta->Set("shutdown", body); meta->Flush(); }
        };
        session->SetHooks(std::move(hooks));
    }
    // --set harness.save_at_frame=N: the F5 capture without a key press, on the
    // completed game owner (the same callback F5's capture runs in), at the
    // first admissible boundary from N on; the writer result is polled at the
    // following boundaries and the slot + .meta land where F5's would.
    ps2r::app::SavestateTrigger save_trigger(checkpoint, cfg,
        [&](const std::string& key, const std::string& value) {
            if (auto* meta = host.Meta()) { meta->Set(key, value); meta->Flush(); }
        });
    // --set harness.restore_at_frame=N: the F8 press without a key (play
    // sessions only: a window-less leg has nothing to press it in).
    bool restore_pressed = false;
    // --set harness.inject_key=<KEY>@N[-M][,...] (HarnessKeyPress above).
    std::vector<HarnessKeyPress> key_presses;
    if (!ParseHarnessKeyPresses(cfg.harness.inject_key, key_presses)) return 2;
    // --set harness.close_at_frame=N: the window's close (a real SDL_QUIT into
    // SDL's queue) at the first boundary >= N -- the user's red X without the finger.
    bool close_pushed = false;
    // --set harness.virtual_pad=attach@N[,press@K-L][,detach@M] (HarnessPadVerb above).
    std::vector<HarnessPadVerb> pad_verbs = ParseHarnessPadVerbs(cfg.harness.virtual_pad);
    // --set harness.display_sequence=<frame>:<mode>[,...]: the Display page's
    // request submitted at the first boundary >= frame, through the same
    // SessionControls route the menu widget takes; the presenter's thread runs
    // the swap between two presented frames (RootScreenModeSwap). The validator
    // pinned the format (Skipped() = 0).
    ps2r::app::DisplaySequence display_sequence(cfg.harness.display_sequence);
    game_boundary.SetConsumer([&](uint64_t frame) {
        save_trigger.OnBoundary(frame, domain.BoundaryCount());
        if (session && !display_sequence.Empty()) {
            display_sequence.OnBoundary(domain.BoundaryCount(), audio_ring.padded(), [&](const ps2r::app::DisplaySequence::Step& step) {
                if (!protocol.WorkWindowMarked()) protocol.MarkWorkWindow();   // the game thread's work from the first change on
                const uint64_t id = session->Controls().Submit(step.action, step.value);
                std::fprintf(stderr, "nativeee: harness.display_sequence %s at boundary %llu: %s\n", step.mode.c_str(),
                             static_cast<unsigned long long>(domain.BoundaryCount()),
                             id ? "submitted to the presenter" : "refused by SessionControls");
                return id;
            }, [&](uint64_t id) {
                // The presenter's terminal report (SessionControls::Finish on main)
                // read from the recent results: the swap window's end.
                const auto snapshot = session->Controls().Read();
                for (const auto& result : snapshot.recent)
                    if (result.request.id == id) return true;
                return false;
            });
        }
        if (session)
            for (HarnessKeyPress& press : key_presses)
                StepHarnessKeyPress(press, *session, live_input.get(), domain.BoundaryCount());
        for (HarnessPadVerb& verb : pad_verbs) {
            if (verb.done || !live_input || domain.BoundaryCount() < verb.frame) continue;
            verb.done = true;
            live_input->VirtualPadAction(verb.action);
            std::fprintf(stderr, "nativeee: harness.virtual_pad %s at boundary %llu\n", verb.name,
                         static_cast<unsigned long long>(domain.BoundaryCount()));
        }
        if (session && cfg.harness.close_at_frame > 0 && !close_pushed &&
            domain.BoundaryCount() >= static_cast<uint64_t>(cfg.harness.close_at_frame)) {
            close_pushed = true;
            if (cfg.harness.close_after_ms > 0) {
                session->ArmCloseAfter(uint64_t(cfg.harness.close_after_ms) * 1000000ull);
                std::fprintf(stderr, "nativeee: harness.close_at_frame at boundary %llu: SDL_QUIT armed on main in %d ms\n",
                             static_cast<unsigned long long>(domain.BoundaryCount()), cfg.harness.close_after_ms);
            } else {
                const bool pushed = session->InjectClose();
                std::fprintf(stderr, "nativeee: harness.close_at_frame at boundary %llu: %s\n",
                             static_cast<unsigned long long>(domain.BoundaryCount()),
                             pushed ? "SDL_QUIT pushed into SDL's queue" : "SDL_PushEvent refused");
            }
        }
        if (session && cfg.harness.restore_at_frame > 0 && !restore_pressed &&
            domain.BoundaryCount() >= static_cast<uint64_t>(cfg.harness.restore_at_frame)) {
            restore_pressed = true;
            session->Controls().Submit(ps2r::app::SessionControls::Action::RestoreReproduction);
        }
        if (session && !session->Boundary(frame)) throw ThreadExitUnwind{};
    });
    s.session_workers = &session_workers;
    s.save_trigger = &save_trigger;
    s.display_sequence = &display_sequence;
    return ArmVerifySlot(s);
}

int EeBootstrap::ArmVerifySlot(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    const bool& play_mode = *s.play_mode;
    MainMemory& ram = *s.ram;
    ps2r::app::SessionFiles& session_files = *s.session_files;
    const std::string& dump_frames_dir = *s.dump_frames_dir;
    uint64_t& dump_frames_written = *s.dump_frames_written;
#if PS2R_DIAGNOSTICS
    ps2r::diag::WordWatch& word_watch = *s.word_watch;
    Vu0Trace& vu0_trace = *s.vu0_trace;
#endif
    EmuDomain& domain = *s.domain;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    ps2r::app::VideoStack& video = *s.video;
#if PS2R_DIAGNOSTICS
    const TexresTraceRequest& texres_req = *s.texres_req;
#endif
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer>& consumer = *s.consumer;
    ps2r::gs::CountingDrawIrSink& draw_sink = *s.draw_sink;
    RegisterOpStream& gs_ops = *s.gs_ops;
    GsStateSink& gs_state_sink = *s.gs_state_sink;
    ps2r::sound::Mixer& mixer = *s.mixer;
    BoundarySequencer& sequencer = *s.sequencer;
    BoundaryProtocol& protocol = *s.protocol;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    const std::vector<ps2r::verify::MemorySpan>& verify_spans = *s.verify_spans;
    int64_t& audio_first_nonsilent_window = *s.audio_first_nonsilent_window;
    FootprintCensus& footprint_census = *s.footprint_census;
    std::function<void(const float*)>& push_audio_ring = *s.push_audio_ring;
    const bool& audio_field_pump_on = *s.audio_field_pump_on;
    uint64_t& audio_pumped_tick = *s.audio_pumped_tick;
    std::function<void(uint64_t)>& audio_field_pump = *s.audio_field_pump;
    ps2r::ipu::MpegApi& mpeg = *s.mpeg;
    ps2r::ipu::LibmpegBindings& libmpeg = *s.libmpeg;
    StateRestorer& restorer = *s.restorer;
    uint32_t& restored_vsync_frame = *s.restored_vsync_frame;
    uint64_t& fmv_active_frames = *s.fmv_active_frames;
    uint64_t& fmv_window_first = *s.fmv_window_first;
    uint64_t& fmv_window_last = *s.fmv_window_last;
    uint64_t& fmv_served_at_prev_boundary = *s.fmv_served_at_prev_boundary;
    bool& fmv_window_open = *s.fmv_window_open;
    ps2r::savestate::Quiescence& quiesce = *s.quiesce;
    ps2r::gs::PresentPolicy& present_policy = *s.present_policy;
    std::vector<uint8_t>& held_rgba = *s.held_rgba;
    uint64_t& held_hash = *s.held_hash;
    int& held_w = *s.held_w;
    int& held_h = *s.held_h;
    LegCloser& closer = *s.closer;
    std::function<void()>& write_census = *s.write_census;
    ps2r::app::ClockCensus& clock_census = *s.clock_census;
    TickThread& tick_thread = *s.tick_thread;
    // Rule 5's window attribution: a boundary belongs to the window of its first
    // field's wall edge (the pacer's), not of its completion.
    clock_census.SetEdgeLookup([&tick_thread](uint64_t tick, std::chrono::steady_clock::time_point* edge) {
        int64_t ns;
        if (!tick_thread.EdgeAt(tick, &ns)) return false;
        *edge = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
        return true;
    });
    // Slot 8 (Verify): the audio pump LAST -- every command the boundary's duties
    // and the IOP dispatch issued lands on the voices before the frames are mixed;
    // the 800-frame output feeds the audio lane, the RAM spans the mem lane. The
    // present is the Phase-3 stub's black frame (the GS owns slot 0 later): it
    // counts `frames=` and grows frames.fhash, the bounded runner's progress
    // file. The --frames cap ends the leg here with park=none.
    std::vector<uint8_t> black_frame(static_cast<size_t>(kPresentW) * kPresentH * 4, 0);
    // --dump-frames also keeps the presenter's OUTPUT of each present (the window
    // image after the display quad, before the swap) as <dir>/p<key>.ppm: what a
    // player sees, scaled/filtered by the presenter, beside the target grab f<key>.
    if (!dump_frames_dir.empty() && video.Active()) video.Pipe()->Backend().ArmPresentCapture(true);
    const auto write_ppm = [](const std::string& path, int w, int h, const uint8_t* rgba_bottom_up) {
        std::FILE* pf = std::fopen(path.c_str(), "wb");
        if (!pf) return false;
        std::fprintf(pf, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y)
            for (int x = 0; x < w; ++x)
                std::fwrite(rgba_bottom_up + (static_cast<size_t>(y) * w + x) * 4, 1, 3, pf);
        std::fclose(pf);
        return true;
    };
    uint64_t grab_local_memory = 0, grab_fallback = 0;   // grabs by source over the leg (census gs_grab_sources=)
    sequencer.SetStep(BoundarySequencer::Step::Verify, [&](CpuContext&) {
        session_files.Select(host.LegDir(), host.OutputSegment());
        // Play mode: fields the domain advanced since the last pump without a
        // display tick are mixed to the device first; this boundary's own tick is
        // the hashed quantum below (the lane's 800 frames per boundary).
        audio_field_pump(1);
        mixer.Pump();
        if (audio_field_pump_on && audio_pumped_tick < domain.DomainTick()) audio_pumped_tick = domain.DomainTick();
        const uint64_t boundary = host.OnBoundary(mixer.Output(), ps2r::sound::Mixer::kFramesPerVsync, verify_spans);
        if (play_mode) {
            // Rule 5: this boundary in its 1 s wall window; the latest window to the F1
            // panel. The fast-forward fields never were pacer ticks: subtracted so the
            // boundary is attributed to its wall edge (review C2 loop 1); the fields
            // the bounded catch-up dropped never became domain ticks: added back so
            // the attribution stays by the true wall edge (rule 3, single-field form).
            clock_census.NoteBoundary(domain.DomainTick(), sequencer.FastForwardFields(), protocol.PaceDroppedFields());
            if (session) session->SetClockReadout(clock_census.LatestGameHz(), clock_census.LatestRenderFps());
        }
        // harness.inject_fault_frame=N (--set, harness legs only): the synthetic
        // fault that proves the crash path -- the triage bundle, run.meta
        // park=exception, the census at exit -- on a real process (RootTriageBundleOnFault).
        if (cfg.harness.inject_fault_frame > 0 && boundary == static_cast<uint64_t>(cfg.harness.inject_fault_frame))
            throw std::runtime_error("injected fault at frame boundary " + std::to_string(boundary) +
                                     " (harness.inject_fault_frame)");
#if PS2R_DIAGNOSTICS
        word_watch.SetBoundary(boundary);
        WatchFlush().flush_on = WatchFlush().flush_from > 0 && boundary >= WatchFlush().flush_from;
        WatchFlush().record_on = WatchFlush().record_from == 0 || boundary >= WatchFlush().record_from;
        vu0_trace.SetBoundary(boundary);
#endif
        push_audio_ring(mixer.Output());
        if (audio_first_nonsilent_window < 0) {
            const float* out = mixer.Output();
            for (int i = 0; i < ps2r::sound::Mixer::kFramesPerVsync * 2; ++i)
                if (out[i] != 0.0f) { audio_first_nonsilent_window = static_cast<int64_t>((boundary * ps2r::sound::Mixer::kFramesPerVsync) / ps2r::verify::AudioHasher::kWindowFrames); break; }
        }
        // The frames lane: the display grab of this boundary's present (bottom-up
        // RGBA8 at the physical size, hashed at the native scale inside the host).
        // With a live stack and no grabbable target the line is the hasher's "no
        // readable scanout" value 0 (the original's lines before its first target
        // read 0 too); the stub's black frame only when no video stack exists.
        // The line's key is the original's vsync-frame convention: a
        // cold boot keys its presents 1, 2, 3, ...; a restored leg keeps key 1 for
        // its first present and then continues from the anchor's vsync frame --
        // `restored_vsync_frame + 1, + 2, ...` -- the frame counter the original's
        // graphics thread reseeded from the file at the load (its first present,
        // the boot-era one before the load, stays key 1). The lanes are paired by
        // key. Interactive restore segments instead continue saved frame + 1.
        const uint32_t present_key = host.PresentKey(restored_vsync_frame, boundary);
        // The grab this boundary reads: the consumer's reference when the boundary
        // waited for it (deterministic), a published COPY under the wall lock (the
        // consumer runs concurrently; rule 2).
        // Under the wall lock the boundary needs the frame's header and its lane
        // hash (computed by the consumer at the grab, FrameHasher::HashNativeScale
        // at video.Scale()), never its pixels: no copy crosses the threads.
        ps2r::gs::DrawPipeline::Frame wall_locked_grab;
        const ps2r::gs::DrawPipeline::Frame* last_frame = nullptr;
        if (consumer) {
            if (sequencer.WallLocked()) {
                wall_locked_grab = consumer->FrameHashScale() > 0 ? consumer->LastFrameHeaderCopy() : consumer->LastFrameCopy();
                last_frame = &wall_locked_grab;
            } else {
                last_frame = &consumer->LastFrame();
            }
        }
        if (consumer) {
            // Census: what the grab read -- `gs_key1_grab=<target|local_memory|none>@<fbp>[,fallback]`
            // for boundary 0 (the restore point's scanout), and the leg-wide counts of
            // local-memory decodes and most-recently-drawn fallbacks.
            const ps2r::gs::DrawPipeline::Frame& g = *last_frame;
            if (g.source == ps2r::gs::ScanoutComposer::Source::Mirror) ++grab_local_memory;
            if (g.source_fallback) ++grab_fallback;
            if (auto* meta = host.Meta()) {
                if (boundary == 0) {
                    const char* src = g.source == ps2r::gs::ScanoutComposer::Source::Target ? "target"
                                      : g.source == ps2r::gs::ScanoutComposer::Source::Mirror ? "local_memory" : "none";
                    meta->Set("gs_key1_grab", std::string(src) + "@" + Hex(g.source_fbp) + (g.source_fallback ? ",fallback" : ""));
                }
                meta->Set("gs_grab_sources", "local_memory=" + std::to_string(grab_local_memory) +
                                                 ",fallback=" + std::to_string(grab_fallback));
            }
        }
        if (play_mode && consumer) {
            // Row 6.7 (play mode only): live / hold / none per the present policy.
            // The lane line is the grab's hash: the consumer's when it hashed the
            // grab (the header form), else taken here over the pixels; Hold repeats
            // the last live hash (the same bytes hash the same), None writes 0.
            const ps2r::gs::DrawPipeline::Frame& fr = *last_frame;
            switch (present_policy.Decide(fr.grabbed, mpeg.FmvActive())) {
                case ps2r::gs::PresentPolicy::Decision::Live:
                    if (fr.hashed) {
                        host.OnPresentHashed(present_key, fr.hash, video.Renderer());
                        held_hash = fr.hash;
                        held_rgba.clear();
                    } else {
                        host.OnPresent(present_key, fr.rgba.data(), fr.w, fr.h, video.Scale(), video.Renderer());
                        held_hash = ps2r::verify::FrameHasher::HashNativeScale(fr.rgba.data(), fr.w, fr.h, video.Scale());
                        held_rgba = fr.rgba;
                    }
                    held_w = fr.w;
                    held_h = fr.h;
                    break;
                case ps2r::gs::PresentPolicy::Decision::Hold:
                    host.OnPresentHashed(present_key, held_hash, video.Renderer());
                    break;
                case ps2r::gs::PresentPolicy::Decision::None:
                    host.OnPresentHashed(present_key, 0, video.Renderer());
                    break;
            }
        } else if (consumer && last_frame->grabbed) {
            const ps2r::gs::DrawPipeline::Frame& fr = *last_frame;
            host.OnPresent(present_key, fr.rgba.data(), fr.w, fr.h, video.Scale(), video.Renderer());
            // --dump-frames <dir>: the very image the line above hashed, as <dir>/f<key>.ppm
            // (P6, top-down -- the grab's rows are bottom-up), for the pixel diff against
            // the original's frame dump of the same key.
            if (!dump_frames_dir.empty()) {
                const std::string path = dump_frames_dir + "/f" + std::to_string(present_key) + ".ppm";
                if (std::FILE* pf = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(pf, "P6\n%d %d\n255\n", fr.w, fr.h);
                    for (int y = fr.h - 1; y >= 0; --y)
                        for (int x = 0; x < fr.w; ++x)
                            std::fwrite(fr.rgba.data() + (static_cast<size_t>(y) * fr.w + x) * 4, 1, 3, pf);
                    std::fclose(pf);
                    ++dump_frames_written;
                }
                // Alongside the display: the most-recently-drawn target's colour, as
                // <dir>/f<key>_drawn_<fbp>.ppm, when that target is not the one the
                // display shows. A title that composes its frame off-screen and then
                // copies it into the DISPFB base shows the copy on the display; this
                // readback is the frame BEFORE that copy, so the two links (the
                // composition, the display copy) can be measured apart. A plain
                // colour readback of the cache's target (no present, no flush, no
                // trace reset); the scanout base is restored right after.
#if PS2R_DIAGNOSTICS
                if (texres_req.enabled && present_key >= texres_req.from && video.Active())
                    WriteTexresTrace(video.Pipe()->Resolver().Trace(), dump_frames_dir, present_key);
#endif
                if (video.Active()) {
                    // Both readbacks are GL calls on the pipeline's backend. With the
                    // pipelined queue the GL context is current on the CONSUMER thread,
                    // so they run there through RunOrdered (the ForgetLastFrame
                    // pattern of the live restore); inline when the leg has no queue.
                    // Calling them from the EE thread SIGSEGVed in GlStateCache::BindFb
                    // on every --load-state + --dump-frames leg (the oracle's cavern legs).
                    const auto dump_extras = [&]() -> bool {
                        std::vector<uint8_t> presented;
                        int pw = 0, ph = 0;
                        if (video.Pipe()->Backend().ReadPresented(&presented, &pw, &ph) && pw > 0 && ph > 0)
                            write_ppm(dump_frames_dir + "/p" + std::to_string(present_key) + ".ppm", pw, ph, presented.data());
                        ps2r::gs::ScanoutComposer& sc = video.Pipe()->Scanout();
                        const uint32_t display_fbp = sc.ScanoutFbp();
                        sc.SetScanoutFbp(ps2r::gs::DrawPipeline::kNoFbp);
                        std::vector<uint8_t> drawn;
                        int dw = 0, dh = 0;
                        const bool got = sc.GrabDisplay(&drawn, &dw, &dh, nullptr);
                        const uint32_t drawn_fbp = sc.LastGrabFbp();
                        sc.SetScanoutFbp(display_fbp);
                        if (got && dw > 0 && dh > 0 && drawn_fbp != display_fbp) {
                            const std::string dpath = dump_frames_dir + "/f" + std::to_string(present_key) + "_drawn_" + Hex(drawn_fbp) + ".ppm";
                            if (std::FILE* pf = std::fopen(dpath.c_str(), "wb")) {
                                std::fprintf(pf, "P6\n%d %d\n255\n", dw, dh);
                                for (int y = dh - 1; y >= 0; --y)
                                    for (int x = 0; x < dw; ++x)
                                        std::fwrite(drawn.data() + (static_cast<size_t>(y) * dw + x) * 4, 1, 3, pf);
                                std::fclose(pf);
                            }
                        }
                        return true;
                    };
                    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>* gs_queue = s.gs_queue;
                    if (gs_queue && *gs_queue) (*gs_queue)->RunOrdered(dump_extras);
                    else dump_extras();
                }
            }
        } else if (consumer) {
            host.OnPresent(present_key, nullptr, 0, 0, video.Scale(), video.Renderer());
        } else {
            host.OnPresent(present_key, black_frame.data(), kPresentW, kPresentH, 1, "none");
        }
        {
            const uint64_t fp_now = HostPhysFootprintBytes();
            footprint_census.last_bytes = fp_now;
            if (fp_now > footprint_census.peak_bytes) footprint_census.peak_bytes = fp_now;
            if (boundary == 60) footprint_census.b60_bytes = fp_now;
            if (boundary % kFootprintSeriesEvery == 0 && footprint_census.series_n < kFootprintSeriesMax) {
                footprint_census.series += (footprint_census.series_n ? "," : "") + std::string("b") + std::to_string(boundary) +
                                           ":" + std::to_string(fp_now / (1024u * 1024u));
                footprint_census.series_n++;
            }
        }
        // Process-wide diagnostic hashes follow their recording epoch's files.
        session_files.WriteHashes(boundary, gs_ops.Hash(), draw_sink.DrawHash());
        // Draws and jobs pushed after this boundary are stamped with the next frame.
        gs_state.SetSrcFrame(static_cast<uint32_t>(boundary + 1));
        gs_state_sink.SetFrameInstance(static_cast<uint32_t>(boundary + 1));
        // Row 7.6: the FMV-active sample of this boundary (census; the quiesce
        // predicate below reads the same signal).
        if (mpeg.FmvActive()) ++fmv_active_frames;
        if (libmpeg.ServedTotal() > fmv_served_at_prev_boundary) {
            if (!fmv_window_open) fmv_window_first = boundary;
            fmv_window_open = true;
            fmv_window_last = boundary;
            fmv_served_at_prev_boundary = libmpeg.ServedTotal();
        }
        // Boundary memory inspection is independent of resumable device state.
        // Capture the hashed bytes even when pending service work refuses a save.
        if (cfg.harness.state_every > 0 && (boundary % static_cast<uint64_t>(cfg.harness.state_every)) == 0 &&
            !host.LegDir().empty()) {
            ps2r::app::CheckpointCapture::Write(host.LegDir(), boundary, domain.BoundaryCount(), ram, restorer, quiesce);
        }
        if (cfg.harness.frames > 0 && boundary + 1 >= static_cast<uint64_t>(cfg.harness.frames)) {
            if (s.save_trigger) s.save_trigger->Finish();   // the slot lands before the cap's _Exit
            write_census();
            closer.Close("none", 0, ThreadManager::Instance().CurrentTid(), "frames_cap");
            if (session) {
                session->RequestClose();
                throw ThreadExitUnwind{};
            }
            // No video teardown here: _Exit skips every destructor and the window
            // may only be destroyed on the process's main thread (SDL/Cocoa), which
            // this boundary thread is not under a restore leg.
            std::_Exit(0);
        }
        return true;
    });

    return RunLeg(s);
}

int EeBootstrap::RunLeg(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    const TitleProfile& title = *s.title;
    const EeBootstrap::Paths& paths = *s.paths;
    const bool& play_mode = *s.play_mode;
    MetaArena& arena = *s.arena;
    EmuDomain& domain = *s.domain;
    std::atomic<bool>& running = *s.running;
    ps2r::gs::GsState& gs_state = *s.gs_state;
#if PS2R_IOP_ORACLE
    ps2r::iop::IopMemory& iop_mem = *s.iop_mem;
    std::function<std::string()>& iop_threads_census = *s.iop_threads_census;
    std::function<std::string()>& iop_semas_census = *s.iop_semas_census;
    std::string& iop_threads_restored = *s.iop_threads_restored;
#endif
    GameBoundary& game_boundary = *s.game_boundary;
    McCommandModel& mc_model = *s.mc_model;
    std::unique_ptr<PlatformInput>& live_input = *s.live_input;
    const uint32_t& input_save_selection = *s.input_save_selection;
    ps2r::app::InputSave& input_save = *s.input_save;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    uint64_t& audio_pumped_tick = *s.audio_pumped_tick;
    StateRestorer& restorer = *s.restorer;
    ps2r::app::SessionClock& session_clock = *s.session_clock;
    HeapSection& heap_section = *s.heap_section;
    VuMemorySection& vu_memory_section = *s.vu_memory_section;
    ThreadSection& thread_section = *s.thread_section;
    VifsSection& vifs_section = *s.vifs_section;
    ps2r::gs::GsSections& gs_sections = *s.gs_sections;
    GifsSection& gifs_section = *s.gifs_section;
    uint64_t& gifs_restore_tail_bytes = *s.gifs_restore_tail_bytes;
    Vu1RegisterSection& vu1_register_section = *s.vu1_register_section;
    uint32_t& restored_vsync_frame = *s.restored_vsync_frame;
    ps2r::app::MovieCheckpoint& movie_checkpoint = *s.movie_checkpoint;
    TickThread& tick_thread = *s.tick_thread;
    LegCloser& closer = *s.closer;
    std::function<void()>& write_census = *s.write_census;
    ps2r::app::SessionWorkers& session_workers = *s.session_workers;
    using InputSave = ps2r::app::InputSave;
    // --- --load-state: restore, restore.mhash, then (later stage) relaunch ----
    if (!cfg.harness.load_state.empty()) {
        std::string err;
        ps2r::app::MovieBoot movie_boot(movie_checkpoint, input_save, session_clock);
        InputSave::Checkpoint::LegacyPolicy legacy_input_save;
        // Selecting either service requires explicit OLD boot import.
        legacy_input_save.approved = input_save_selection == 0 || cfg.harness.legacy_input_save;
        legacy_input_save.clock = InputSave::Checkpoint::Clock::OldSyncV;
        const bool prepared = movie_boot.PrepareFile(cfg.harness.load_state, legacy_input_save, err);
        if (prepared) {
            restored_vsync_frame = movie_boot.Meta().vsync_frame;
            tick_thread.Stop();
        }
        if (!prepared || !movie_boot.Restore(restorer, err)) {
            std::fprintf(stderr, "nativeee: --load-state %s: %s\n", cfg.harness.load_state.c_str(), err.c_str());
            return 2;
        }
        // Preserve restored MOVI clocks; reset only the completed game boundary.
        game_boundary.ResetAfterRestore();
        if (s.menu_extension) s.menu_extension->NoteRestored();
        for (const auto& section : restorer.LastSections())
            if (section.tag == ps2r::app::SessionClock::kTag) audio_pumped_tick = domain.DomainTick();
        if (play_mode) tick_thread.Start();
        // libmc's readiness is EE data the image carries: the bound-server word
        // of its sceSifClientData (title.mc.client_addr + 0x24), written by
        // sceMcInit's sceSifBindRpc before the anchor was taken. The model's
        // Bound is derived from it (the mcserv init command never re-runs).
        if (!input_save.SavesSelected() && title.mc.client_addr != 0)
            mc_model.DeriveBound(memory::read<uint32_t>(title.mc.client_addr + SceSif::kClientServer));
#if PS2R_IOP_ORACLE
        const std::string iop_ram_hash_restored = Hex(ps2r::hash::Fnv1a64(iop_mem.Data(), ps2r::iop::IopMemory::kSize));
        iop_threads_restored = iop_threads_census() + ";" + iop_semas_census();
#else
        const std::string iop_ram_hash_restored = "none";   // no IOP RAM in the play binary (an anchor's IOPM section is skipped: not required)
#endif
        const uint64_t mhash = restorer.RestoredMemoryHash();
        if (std::FILE* f = std::fopen(host.LegPath("restore.mhash").c_str(), "wb")) {
            std::fprintf(f, "%016llx\n", static_cast<unsigned long long>(mhash));
            std::fclose(f);
        }
        uint64_t expected = 0;
        const bool known = ExpectedHash(paths.title_dir + "/" + title.save.expected_hashes,
                                        Basename(cfg.harness.load_state), &expected);
        if (auto* meta = host.Meta()) {
            meta->Set("restore_mhash", Hex(mhash));
            meta->Set("restore_mhash_ok", known ? (expected == mhash ? "1" : "0") : "unknown_anchor");
            meta->Set("restore_thrd_records", std::to_string(thread_section.RestoredCount()));
            meta->Set("heap_section_restored", std::to_string(heap_section.RestoredCount()));
            meta->Set("meta_arena_bump_restored", std::to_string(arena.GetStats().bump_used));
            meta->Set("vu_sections_restored", std::to_string(vu_memory_section.RestoredCount()) + "/" +
                                                  std::to_string(vu1_register_section.RestoredCount()) + "/" +
                                                  std::to_string(vu1_register_section.RestoredRunningCount()));
            meta->Set("vif_gif_sections_restored", std::to_string(vifs_section.RestoredCount()) + "/" +
                                                       std::to_string(gifs_section.RestoredCount()) + "/" +
                                                       std::to_string(gifs_restore_tail_bytes));
            meta->Set("iop_ram_hash_restored", iop_ram_hash_restored);
            meta->Set("restored_vsync_frame", std::to_string(restored_vsync_frame));
            meta->Set("gs_sections_restored", std::to_string(gs_sections.RestoredVramCount()) + "/" +
                                                  std::to_string(gs_sections.RestoredGsprCount()) + "/" +
                                                  std::to_string(gs_sections.RestoredGsstCount()));
            meta->Set("gs_restored_dispfb1", Hex(gs_state.ReadPrivileged(ps2r::gs::GsPrivRegs::kDispfb1)));
        }
        PS2R_LOG(HOST, Info, "restore %s: mhash=%s expected=%s thrd=%zu", cfg.harness.load_state.c_str(),
                 Hex(mhash).c_str(), known ? Hex(expected).c_str() : "?", thread_section.RestoredCount());
        // Relaunch the parked guest threads (THRD records with a context) and
        // let them run to the leg's park; the park sink ends the process, so
        // JoinAll returns only when every relaunched thread exited on its own.
        ThreadRelauncher relauncher(ThreadManager::Instance(), [](uint32_t fn, uint32_t enter, CpuContext& c) {
            return DispatchTable::InvokeAt(fn, enter, c);
        });
        auto relaunch = [&] {
            const int count = relauncher.Relaunch(thread_section.Restored(), &err);
            if (auto* meta = host.Meta()) meta->Set("relaunch", std::to_string(count));
            if (count < 0) {
                std::fprintf(stderr, "nativeee: relaunch: %s\n", err.c_str());
                closer.Close("relaunch_failed", 0, 0, err.c_str());
            }
            return count;
        };
        if (session) {
            // Joining a live table holds ThreadManager's host map lock. Keep
            // it free for guest-created threads, and keep SDL main pumping.
            if (live_input) live_input->ResetPending();
            const auto restored = thread_section.Restored();
            for (const auto& record : restored) if (record.is_boundary) { session_workers.SetBoundary(record.tid); break; }
            const int result = session->Run([&] { return relaunch() < 0 ? 2 : 0; },
                static_cast<unsigned>(cfg.engine.shutdown_wait_s));
            write_census();
            closer.Close("none", 0, 0, "session_closed");
            return result;
        }
        if (relaunch() < 0) return 2;
        ThreadManager::Instance().JoinAll();
        running.store(false);
        closer.Close("none", 0, 0, "relaunched_threads_exited");
        return 0;
    }

    // The guest pc at a fault caught inside cold_boot, for the session path's
    // close on the main thread (the harness path closes with ctx directly).
    uint32_t fault_pc = 0;
    auto cold_boot = [&]() {
        CpuContext ctx;
        CpuContextInit::Reset(ctx, title.entry, title.main_stack_top);
        std::shared_ptr<ThreadInfo> main_info = ThreadManager::Instance().AdoptCurrentThread(1, kMainThreadPriority);
        main_info->ctx = &ctx;
        EeRunToken::Instance().GuestEnter(1, kMainThreadPriority);
        PS2R_LOG(HOST, Info, "cold boot: entry 0x%08x sp 0x%08x", title.entry, title.main_stack_top);
        std::exception_ptr failure;
        try {
            Ps2Float::SetGuestThreadRounding();
            ps2r::ee::TickProfile::BindGameThread();   // this thread's hook work is the tick's (TickProfile)
            DispatchTable::Invoke(title.entry, ctx);
        } catch (const ThreadExitUnwind&) {
        } catch (...) {
            failure = std::current_exception();
            fault_pc = ctx.cpuRegs.pc;
            // The throw site for the triage bundle: this thread, before the
            // fault travels to the session's main thread.
            std::string what = "guest fault";
            try { std::rethrow_exception(failure); } catch (const std::exception& e) { what = e.what(); } catch (...) {}
            ps2r::triage::TriageBundle::NoteThrowSite(what.c_str());
        }
        std::string release_error;
        if (!ThreadManager::Instance().ReleaseCurrentThread(&release_error) && !failure)
            failure = std::make_exception_ptr(std::runtime_error(release_error));
        if (session) {
            // The same supervisor handles an accepted restore after this
            // adopted incarnation releases its token/context. Returning does
            // not close the SDL loop while restart work is pending.
            if (failure) std::rethrow_exception(failure);
            return 0;
        }
        session_workers.Join();
        write_census();
        if (failure) {
            std::string what = "guest fault";
            try { std::rethrow_exception(failure); } catch (const std::exception& e) { what = e.what(); } catch (...) {}
            closer.Close("exception", ctx.cpuRegs.pc, 1, what.c_str());
            std::rethrow_exception(failure);
        }
        closer.Close("none", ctx.cpuRegs.pc, 1, "guest_returned");
        return 0;
    };
    if (session) {
        int result = 0;
        try {
            result = session->Run(cold_boot, static_cast<unsigned>(cfg.engine.shutdown_wait_s));
        } catch (const std::exception& e) {
            // A guest fault the session worker rethrew: the census (every
            // family's refusal/fault ledger) is the attribution of that fault.
            // Publish and close the leg before reporting it; the runner's
            // non-zero exit still fails the leg.
            write_census();
            closer.Close("exception", fault_pc, fault_pc ? 1 : 0, e.what());
            throw;
        }
        write_census();
        closer.Close("none", 0, 0, "session_closed");
        if (session->Shutdown().HasNote("guest_join_abandoned")) {
            // An abandoned guest worker is still parked inside objects this
            // frame owns: leave now, census written, without their destructors.
            std::fprintf(stderr, "nativeee: exiting around abandoned guest workers (census written)\n");
            std::fflush(stderr);
            std::fflush(stdout);
            std::_Exit(result);
        }
        return result;
    }
    return cold_boot();
}
