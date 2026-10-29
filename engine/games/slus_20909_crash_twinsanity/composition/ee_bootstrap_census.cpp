// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"

int EeBootstrap::CensusServices(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    MetaArena& arena = *s.arena;
    MmioRouter& mmio = *s.mmio;
    SifState& sif_state = *s.sif_state;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>& gs_queue = *s.gs_queue;
    RegisterOpStream& gs_ops = *s.gs_ops;
    KickLowering& kick_lowering = *s.kick_lowering;
    Vu1HleRuntime& vu1_hle = *s.vu1_hle;
    Dmac& dmac = *s.dmac;
    ps2r::app::EffectsApplication& effects_application = *s.effects_application;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
    ps2r::gs::GsMmioHandler& gs_mmio = *s.gs_mmio;
#if PS2R_IOP_ORACLE
    ps2r::ipu::IpuDevice& ipu = *s.ipu;
    ps2r::ipu::DmacChannelFaceAdapter& ipu_dmac_face = *s.ipu_dmac_face;
    ps2r::ipu::LibipuApi& libipu = *s.libipu;
#endif
    VifMmioGate& vif_gate = *s.vif_gate;
    uint64_t& dmac_int1_raises = *s.dmac_int1_raises;
    uint64_t& dmac_sif_kicks = *s.dmac_sif_kicks;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::iop::IopMemory& iop_mem = *s.iop_mem;
    MainMemoryEeRamSink& ee_ram_sink = *s.ee_ram_sink;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
    ps2r::title::stream_irx::StreamIrxPlugin::BootReport& irx_boot = *s.irx_boot;
    uint64_t& sif_dma_clamped = *s.sif_dma_clamped;
    ps2r::title::stream_irx::StreamRpcServer& stream_server = *s.stream_server;
    uint64_t& iopm_restored = *s.iopm_restored;
    std::function<std::string()>& iop_threads_census = *s.iop_threads_census;
    std::function<std::string()>& iop_semas_census = *s.iop_semas_census;
    std::string& iop_threads_restored = *s.iop_threads_restored;
#endif
    const bool& iop_booted = *s.iop_booted;
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::app::SoundOutput& sound_output = *s.sound_output;
    ps2r::sound::Mixer& mixer = *s.mixer;
    BoundarySequencer& sequencer = *s.sequencer;
    DeliveryMeter& delivery_meter = *s.delivery_meter;
    LoaderModel& loader_model = *s.loader_model;
    ps2r::app::ServiceTimeline& service_timeline = *s.service_timeline;
    RpcRouter& rpc_router = *s.rpc_router;
    McCommandModel& mc_model = *s.mc_model;
    McservModule& mcserv = *s.mcserv;
    PadProtocol& pad_protocol = *s.pad_protocol;
    PadmanModule& padman = *s.padman;
    ps2r::app::InputSave& input_save = *s.input_save;
    ps2r::app::FrameCensus& frame_census = *s.frame_census;
    FootprintCensus& footprint_census = *s.footprint_census;
    uint64_t& snd_pumps_fields = *s.snd_pumps_fields;
    uint64_t& cdvd_park_waits = *s.cdvd_park_waits;
    std::string& first_syscalls = *s.first_syscalls;
    uint64_t& syscall_count = *s.syscall_count;
    ps2r::ipu::GuestMemoryAdapter& guest_mem = *s.guest_mem;
    title::MovieStream& movie_stream = *s.movie_stream;
    ps2r::ipu::FmvSectorSourceAdapter& fmv_sectors = *s.fmv_sectors;
    ps2r::sound::FmvPcmInput& fmv_pcm = *s.fmv_pcm;
    ps2r::app::MovieSound& movie_sound = *s.movie_sound;
    ps2r::ipu::LibmpegBindings& libmpeg = *s.libmpeg;
#if PS2R_IOP_ORACLE
    ps2r::ipu::LibipuBindings& libipu_bind = *s.libipu_bind;
#endif
    title::GameLibraries& game_libraries = *s.game_libraries;
    ps2r::app::GraphicsApplication& graphics_application = *s.graphics_application;
    ps2r::app::FileServices& file_services = *s.file_services;
    ps2r::app::ServiceImport& service_import = *s.service_import;
    ps2r::app::MovieStorage& movie_storage = *s.movie_storage;
    const int& meta_arena_hooks = *s.meta_arena_hooks;
    FmvAbortHold& fmv_abort_hold = *s.fmv_abort_hold;
    LoaderSpinLift& loader_spin_lift = *s.loader_spin_lift;
    const bool& headroom_query_recompiled = *s.headroom_query_recompiled;
    ps2r::app::ServiceTraceLog& service_trace = *s.service_trace;
    ps2r::app::StartupOwnership& startup_ownership = *s.startup_ownership;
    ps2r::app::NativeStartup& native_startup = *s.native_startup;
    uint64_t& dmac_save_unquiesced = *s.dmac_save_unquiesced;
    PadsSection& pads_section = *s.pads_section;
    ps2r::app::MovieCheckpoint& movie_checkpoint = *s.movie_checkpoint;
    ps2r::savestate::Quiescence& quiesce = *s.quiesce;
    // --- the leg census: written by the park sink and by the --frames cap -------
    LegCloser closer(host, mmio);
    auto census_services = [&]() {
        // The consumer thread's counters are read after it drained (lockstep leaves
        // nothing in flight; the sync is the pipelined policy's barrier).
        if (gs_queue) gs_queue->Quiesce();   // the leg's end, not an EE wait in play (uncounted)
        if (auto* meta = host.Meta()) {
            meta->Set("syscalls_total", std::to_string(syscall_count));
            meta->Set("dmac_save_unquiesced", std::to_string(dmac_save_unquiesced));
            meta->Set("syscalls_first", first_syscalls);
            meta->Set("sdk_rows_hit", std::to_string(SdkBindingManifest::Instance().HitTotal()));
            meta->Set("sdk_rows_hit_list", SdkBindingManifest::Instance().HitList());
            meta->Set("sif_smflag", Hex(sif_state.Smflag()));
            meta->Set("sif_msflag", Hex(sif_state.Msflag()));
            meta->Set("sif_reg_access", std::to_string(sif_state.ReadCount()) + "/" + std::to_string(sif_state.WriteCount()));
            // Whether the retained IOP arm is LINKED into this binary (1 = the
            // oracle binary nativeee_oracle; 0 = the service-only play binary, where
            // every IOP / STREAM.IRX / libsd / SIF-DMA row below is the empty value by
            // construction, not by census).
            meta->Set("iop_linked", ps2r::app::kIopLinked ? "1" : "0");
#if PS2R_IOP_ORACLE
            meta->Set("iop_modules", iop_table.ModulesCensus());
            meta->Set("iop_unrouted", std::to_string(iop_mem.UnroutedCount()));
#else
            meta->Set("iop_modules", "");
            meta->Set("iop_unrouted", "0");
#endif
            meta->Set("sif_bound", rpc_router.BoundCensus());
            meta->Set("sif_rpc_calls", std::to_string(rpc_router.Calls()) + "/" + std::to_string(rpc_router.UnboundCalls()));
            meta->Set("sif_init_rpc_calls", std::to_string(SceSif::InitRpcCalls()));
#if PS2R_IOP_ORACLE
            meta->Set("sif_dma_bytes", std::to_string(SyscallTable::Instance().SifDmaBytes()) + "/" + std::to_string(ee_ram_sink.Bytes()) + "/" + std::to_string(sif_dma_clamped));
            meta->Set("stream_rpc_calls", std::to_string(stream_server.Calls()) + "/" + std::to_string(stream_server.Stalls()));
            meta->Set("stream_rpc_cmds", stream_server.CommandIdsCsv());
            meta->Set("stream_rpc_cmds_head", stream_server.CommandHeadCsv());
            meta->Set("stream_rpc_cmds_tail", stream_server.CommandTailCsv());
#else
            meta->Set("sif_dma_bytes", std::to_string(SyscallTable::Instance().SifDmaBytes()) + "/0/0");
            meta->Set("stream_rpc_calls", "0/0");
            meta->Set("stream_rpc_cmds", "");
            meta->Set("stream_rpc_cmds_head", "");
            meta->Set("stream_rpc_cmds_tail", "");
#endif
            meta->Set("cdvd_deliveries", std::to_string(delivery_meter.Delivered()) + "/" + std::to_string(delivery_meter.Pending()));
            // The deliveries census is the model's split by the site that landed the
            // bytes (a meter pop whose closure found the read already delivered is
            // not a delivery): poll = at a Poll, boundary = slot 4, wait = inside the
            // title's blocking wait at the drive's end (poll mode; overruns = waits
            // whose drive end lay past the frame's edge), forced = boundary mode's
            // pull-forward (0 under poll by the model).
            meta->Set("cdvd_poll_deliveries", std::to_string(loader_model.PollDelivered()));
            meta->Set("cdvd_boundary_deliveries", std::to_string(loader_model.BoundaryDelivered()));
            meta->Set("cdvd_wait_deliveries", std::to_string(loader_model.WaitDelivered()));
            meta->Set("cdvd_wait_overruns", std::to_string(loader_model.WaitOverruns()));
            // Reads issued before a waited one that the wait made visible first (issue order at the wait).
            meta->Set("cdvd_wait_order_deliveries", std::to_string(loader_model.WaitOrderDelivered()));
            // Whole frames blocking waits parked through under poll (the EE blocked
            // while the drive worked past the frame's edge) / the protocol waits
            // those parks issued (absorbed demands included).
            meta->Set("cdvd_wait_parks", std::to_string(loader_model.WaitParks()) + "/" + std::to_string(cdvd_park_waits));
            meta->Set("cdvd_forced", std::to_string(loader_model.Forced()));
            meta->Set("cdvd_seeks", "contig=" + std::to_string(delivery_meter.SeeksContiguous()) + ",short=" + std::to_string(delivery_meter.SeeksShort()) +
                                        ",long=" + std::to_string(delivery_meter.SeeksLong()));
            meta->Set("cdvd_meter_pops", std::to_string(delivery_meter.PollDeliveries()) + "/" + std::to_string(delivery_meter.BoundaryDeliveries()));
            meta->Set("cdvd_visibility", cfg.cdvd.delivery_visibility == ps2r::CdvdDeliveryVisibility::Poll ? "poll" : "boundary");
            {
                char scale[32];
                std::snprintf(scale, sizeof scale, "%g", cfg.cdvd.poll_latency_scale);
                meta->Set("cdvd_poll_scale", scale);
            }
            {
                // The loader file-IO census: opens that missed the disc answer -1
                // (archive-relative names until the archive resolver exists), and
                // the waits that completed a read inside the call.
                const LoaderFileIo::Census& lc = LoaderFileIo::Stats();
                meta->Set("loader_opens", std::to_string(lc.opens));
                meta->Set("loader_misses", std::to_string(lc.misses));
                meta->Set("loader_archive_misses", std::to_string(lc.archive_misses));
                meta->Set("loader_forced", std::to_string(loader_model.Forced()) + "/" + std::to_string(lc.waits));
                meta->Set("loader_reads", std::to_string(lc.sync_reads) + "/" + std::to_string(lc.async_issued) + "/" + std::to_string(lc.bytes_read));
                meta->Set("loader_polls", "pending=" + std::to_string(lc.async_polls_pending) + ",done=" + std::to_string(lc.async_polls_done));
                meta->Set("loader_slot_status", std::to_string(lc.slot_issued) + "/" + std::to_string(lc.slot_done));
                meta->Set("loader_waits_module", std::to_string(lc.waits_module) + "/" + std::to_string(lc.module_laps));
                meta->Set("loader_wait_completion_copies", std::to_string(lc.wait_completion_copies));
                {
                    const MetaArena::Stats ma = arena.GetStats();
                    meta->Set("meta_arena", std::string(cfg.hle.meta_arena ? "on" : "off") + ",hooks=" + std::to_string(meta_arena_hooks) +
                                                ",allocs=" + std::to_string(ma.allocs) + ",frees=" + std::to_string(ma.frees) +
                                                ",live=" + std::to_string(ma.live) + ",bump=" + std::to_string(ma.bump_used));
                }
                meta->Set("loader_ring", std::string(cfg.loader.ring_cache ? "on" : "off") + ",hits=" + std::to_string(lc.ring_hits) +
                                             ",straddles=" + std::to_string(lc.ring_straddles) + ",misses=" + std::to_string(lc.ring_misses) +
                                             ",fills=" + std::to_string(lc.ring_fills) + ",direct=" + std::to_string(lc.async_direct));
                meta->Set("loader_spin_lifts", "lap1=" + std::to_string(loader_spin_lift.LiftsLap1()) + ",lap2=" + std::to_string(loader_spin_lift.LiftsLap2()) +
                                                   ",declines=" + std::to_string(loader_spin_lift.Declines()) + ",idle=" + std::to_string(loader_spin_lift.LiftsIdle()) +
                                                   ",iop_pumps=" + std::to_string(loader_spin_lift.IopPumps()) + ",rpc=" + std::to_string(loader_spin_lift.RpcLaps()) +
                                                   ",edge=" + std::to_string(loader_spin_lift.LiftsEdge()) + (loader_spin_lift.Installed() ? "" : ",nohook"));
                meta->Set("headroom_query", headroom_query_recompiled ? "recompiled" : "wait_seam");
#if PS2R_IOP_ORACLE
                frame_census.Record(*meta, iop_table.Rpc().ServiceUs(), iop_table.Rpc().ChargedUs());
#else
                frame_census.Record(*meta, 0, 0);
#endif
            }
#if PS2R_IOP_ORACLE
            meta->Set("iop_rpc_tid", std::to_string(irx_boot.rpc_tid));
#else
            meta->Set("iop_rpc_tid", "0");
#endif
            // Whether the IOP kernel booted STREAM.IRX this leg (0 under
            // hle.startup=native: no module rows, no threads, no heartbeat).
            meta->Set("iop_boot", iop_booted ? "1" : "0");
            native_startup.Record(*meta);
            {
                const SceSif::HeapCensus hc = SceSif::Heap();
                meta->Set("iop_heap_calls", "allocs=" + std::to_string(hc.allocs) + ",frees=" + std::to_string(hc.frees) +
                                                ",refused=" + std::to_string(hc.refused) + ",bytes=" + std::to_string(hc.bytes));
            }
            meta->Set("mc_bound", mc_model.Bound() ? "1" : "0");
            meta->Set("mc_bound_source", mc_model.BoundDerived() ? "restored_client_record" : (mc_model.Bound() ? "mcserv_init" : "none"));
            meta->Set("mcserv_client", Hex(mcserv.InitClientAddr()));
#if PS2R_IOP_ORACLE
            meta->Set("iopm_restored", std::to_string(iopm_restored));
            meta->Set("iop_threads", iop_threads_census());
            meta->Set("iop_semas", iop_semas_census());
            meta->Set("iop_threads_restored", iop_threads_restored);
#else
            meta->Set("iopm_restored", "0");
            meta->Set("iop_threads", "none");
            meta->Set("iop_semas", "none");
            meta->Set("iop_threads_restored", "none");
#endif
            meta->Set("mcserv_calls", std::to_string(mcserv.InitCalls()) + "/" + std::to_string(mcserv.UnexpectedCalls()));
            meta->Set("mc_calls", "sync=" + std::to_string(LibmcBindings::SyncCalls()) + ",getinfo=" + std::to_string(LibmcBindings::GetInfoCalls()) +
                                      ",getdir=" + std::to_string(LibmcBindings::GetDirCalls()) + ",open=" + std::to_string(LibmcBindings::OpenCalls()) +
                                      ",getinfo_dropped=" + std::to_string(mc_model.DroppedGetInfoKicks()));
            meta->Set("padman_calls", std::to_string(padman.Calls()) + "/" + std::to_string(padman.InitCalls()) + "/" +
                                          std::to_string(padman.OpenCalls()) + "/" + std::to_string(padman.UnknownCalls()));
            meta->Set("pad_calls", "open=" + std::to_string(LibpadBindings::OpenCalls()) + ",read=" + std::to_string(LibpadBindings::ReadCalls()) +
                                       ",getstate=" + std::to_string(LibpadBindings::GetStateCalls()) + ",served=" + std::to_string(LibpadBindings::ReadsServed()));
            meta->Set("pad_stamps", std::to_string(pad_protocol.Stamps()));
            // The control arm's availability keys: the boundary of each port's
            // first READY stamp and the title readiness event it keyed on (0 =
            // none) -- read against the owned arm's input_service stable=/setup=.
            meta->Set("pad_ready", std::to_string(pad_protocol.ReadyAt(0)) + "," + std::to_string(pad_protocol.ReadyAt(1)) +
                                       ",event=" + std::to_string(pad_protocol.ReadinessEvent()));
            // The slot sequence counter: its live value and the value a restore
            // derived from the image (0 = none derived).
            meta->Set("pad_seq", std::to_string(pad_protocol.Seq()) + "," + std::to_string(pads_section.DerivedSeq()));
            meta->Set("snd_pumps", std::to_string(mixer.Pumps()));
            sound_output.Record(*meta, spu_core);
            movie_storage.Record(*meta);
            service_timeline.Record(*meta);
            file_services.Record(*meta);
            service_import.Record(*meta);
            service_trace.Record(*meta);
            startup_ownership.Record(*meta);
            meta->Set("movie_stream", movie_stream.Census());
            movie_checkpoint.Record(*meta);
            meta->Set("boundary_return_rejections", std::to_string(sequencer.RejectedServiceReturns()));
            meta->Set("snd_pumps_fields", std::to_string(snd_pumps_fields));   // play-mode field pumps (device only)
            meta->Set("vu0_calls", std::to_string(VuMacroBridge::Stats().vu0_calls));
            meta->Set("vu0_park_resumes", std::to_string(VuMacroBridge::Stats().vu0_park_resumes));
            meta->Set("vu1_kicks", std::to_string(VuMacroBridge::Stats().vu1_kicks));
            meta->Set("vu1_hle", std::to_string(vu1_hle.census().runs) + "/" + std::to_string(vu1_hle.census().misses) + "/" +
                                     std::to_string(vu1_hle.census().declines));
            // The missed images by (variant hash, entry pc): which VU1 program
            // family the title library lacks on this route, or which residency the
            // image carried that the corpus never saw (the 2a sky, allon_w2).
            const auto miss_kinds = [](const Vu1HleRuntime::Census& c) {
                std::string out;
                for (const auto& kind : c.miss_kinds) {
                    if (!out.empty()) out += '/';
                    out += Hex(kind.variant_hash) + ":" + Hex(kind.entry_pc) + ":" + std::to_string(kind.count);
                }
                return out.empty() ? std::string("none") : out;
            };
            meta->Set("vu1_hle_misses_by_variant", miss_kinds(vu1_hle.census()));
            // The --vu1-miss-cases capture: files written (inline unit / MTVU worker unit).
            if (!ps2r::ConfigLoader::Request().vu1_miss_cases_dir.empty())
                meta->Set("vu1_hle_miss_cases", std::to_string(vu1_hle.miss_captures()) + "/" +
                                                    std::to_string(s.worker_vu1_hle ? s.worker_vu1_hle->miss_captures() : 0));
            if (s.worker_vu1_hle) {
                const Vu1HleRuntime::Census& wc = s.worker_vu1_hle->census();
                meta->Set("vu1_hle_worker", std::to_string(wc.runs) + "/" + std::to_string(wc.misses) + "/" +
                                                std::to_string(wc.declines));
                meta->Set("vu1_hle_worker_misses_by_variant", miss_kinds(wc));
            }
            // The library's declines by program (both runtimes): the entry that
            // declined, the image it saw, the entry pc, how often -- the programs
            // the VU1 interpreter still runs (their kicks are the `identity_raw`
            // PATH1 packets the GIF parser executes).
            {
                const auto rows = [](const Vu1HleRuntime& rt) {
                    std::string text;
                    for (const Vu1HleRuntime::Decline& d : rt.declines()) {
                        if (!text.empty()) text += ",";
                        text += std::string(d.name ? d.name : "?") + ":" + Hex(d.image_hash) + ":" + Hex(d.variant_hash) +
                                ":" + Hex(d.entry_pc) + ":" + std::to_string(d.count);
                    }
                    return text.empty() ? std::string("none") : text;
                };
                meta->Set("vu1_hle_declines_by_program", rows(vu1_hle));
                if (s.worker_vu1_hle) meta->Set("vu1_hle_worker_declines_by_program", rows(*s.worker_vu1_hle));
            }
            meta->Set("gsops_recorder", s.gsops_recorder ? "on" : "off");
            meta->Set("gsops_hash", s.gsops_recorder ? Hex(gs_ops.Hash()) : std::string("off"));
            meta->Set("gsops_ops", std::to_string(gs_ops.Count()));
            {
                const uint64_t now = HostPhysFootprintBytes();
                const FootprintCensus& fp = footprint_census;
                const auto mb = [](uint64_t b) { return std::to_string(b / (1024u * 1024u)); };
                meta->Set("footprint", "b60_mb=" + mb(fp.b60_bytes) + ",last_mb=" + mb(fp.last_bytes) +
                                           ",peak_mb=" + mb(std::max(fp.peak_bytes, now)) + ",final_mb=" + mb(now) +
                                           ",gsops_retained_bytes=" + std::to_string(gs_ops.RetainedBytes()));
                meta->Set("footprint_series", fp.series.empty() ? std::string("none") : fp.series);
            }
            // The CPU's SIMD tiers the runtime dispatch saw (never a global /arch flag).
            meta->Set("cpu_features", HostCpuFeaturesCensus());
            meta->Set("dmac_int1_raises", std::to_string(dmac_int1_raises));
            meta->Set("fmv", libmpeg.Census());
            game_libraries.Record(*meta);
            meta->Set("fmv_served_per_playback", libmpeg.ServedPerPlayback());
            meta->Set("fmv_pictures_served", std::to_string(libmpeg.ServedTotal()));
            meta->Set("fmv_abort_holds", std::to_string(fmv_abort_hold.Holds()) + (fmv_abort_hold.Installed() ? "" : ",nohook"));
            meta->Set("fmv_abort_hold_ticks", std::to_string(fmv_abort_hold.Ticks()));
            if (s.menu_extension) s.menu_extension->Record(*meta);
            else meta->Set("menu_ext", std::string("state=off,reason=") + (cfg.ui.menu_pages ? "not_play_mode" : "ui.menu_pages=off"));
            if (s.retail_fixes) meta->Set("retail_fixes", s.retail_fixes->Census());
            movie_sound.Record(*meta);
            if (cfg.hle.audio_mix)
                meta->Set("fmv_pcm", "pushed=" + std::to_string(fmv_pcm.Pushed()) + ",pushed_nonzero=" + std::to_string(fmv_pcm.PushedNonZero()) +
                                         ",mixed=" + std::to_string(sound_output.MovieFrames(spu_core)) +
                                         ",dropped=" + std::to_string(fmv_pcm.Dropped()) + ",queued=" + std::to_string(fmv_pcm.Queued()) +
                                         ",gate=" + (fmv_pcm.GateIsOpen() ? "open" : "closed") + ",gate_opens=" + std::to_string(fmv_pcm.GateOpens()) +
                                         ",flushes=" + std::to_string(fmv_pcm.Flushes()) + ",bvol=" + Hex(spu_core.CoreAttr(0, ps2r::sound::SpuCore::kBvoll)) + "/" +
                                         Hex(spu_core.CoreAttr(0, ps2r::sound::SpuCore::kBvolr)) +
                                         ",bvol_sets=" + std::to_string(spu_core.BvolSets()) + ",bvol_nonzero_sets=" + std::to_string(spu_core.BvolSetsNonZero()) +
                                         ",bvol_peak=" + Hex(spu_core.BvolPeak()) + ",mixed_audible=" + std::to_string(sound_output.AudibleMovieFrames(spu_core)) +
                                         (cfg.hle.audio_mix ? ",sink=audio_mixer" : ",sink=mixer"));
            meta->Set("fmv_sectors", std::to_string(fmv_sectors.Pumps()) + "/" + std::to_string(fmv_sectors.SectorsPumped()));
            meta->Set("fmv_guest_unbacked", std::to_string(guest_mem.UnbackedWords()));
#if PS2R_IOP_ORACLE
            meta->Set("libipu_calls", "init=" + std::to_string(libipu_bind.InitCalls()) + ",sync=" + std::to_string(libipu_bind.SyncCalls()) +
                                          ",stop=" + std::to_string(libipu_bind.StopCalls()) + ",restart=" + std::to_string(libipu_bind.RestartCalls()));
            SubsystemCensus::Record(*meta, dmac, SubsystemCensus::Snapshot(stream_irx.Thbase()));
            SubsystemCensus::RecordIpu(*meta, ipu, libipu, ipu_dmac_face.OutOfRange());
#else
            // L7: the IPU model is not linked; the rows keep their never-executed text.
            meta->Set("libipu_calls", "init=0,sync=0,stop=0,restart=0");
            SubsystemCensus::Record(*meta, dmac, SubsystemCensus::IopThreadOps{});
            SubsystemCensus::RecordIpuNotLinked(*meta);
#endif
            SubsystemCensus::RecordGraphics(*meta, kick_lowering);
            if (s.worker_kick_lowering)
                SubsystemCensus::RecordKickLowering(*meta, "graphics_kick_lowering_worker", *s.worker_kick_lowering);
            graphics_application.Record(*meta);
            effects_application.Record(*meta);
            input_save.Record(*meta);
            meta->Set("dmac_sif_kicks", std::to_string(dmac_sif_kicks));
            meta->Set("vif_mmio_gated", "r" + std::to_string(vif_gate.ReadsZeroed()) + ",w" + std::to_string(vif_gate.WritesDropped()));
            meta->Set("kernl_timer_clock", KernlTimer::HasClock() ? "1" : "0");
            meta->Set("mtvu", mtvu ? mtvu->Census() : "off");
            meta->Set("savestate_refused_mtvu", std::to_string(quiesce.RefusalsBy("mtvu")));
            meta->Set("libgraph_calls", "resetgraph=" + std::to_string(ps2r::gs::LibgraphBindings::ResetGraphCalls()) +
                                            ",syncvcb=" + std::to_string(ps2r::gs::LibgraphBindings::SyncVCallbackCalls()) +
                                            ",syncv=" + std::to_string(GsSync::SyncVCount()));
            meta->Set("gs_priv_writes", std::to_string(gs_state.Priv().Writes()));
            meta->Set("gs_priv_reads", std::to_string(gs_mmio.Reads()));
        }
    };
    s.closer = &closer;
    std::function<void()> census_services_fn = census_services;
    s.census_services = &census_services_fn;
    return CensusPresentation(s);
}

int EeBootstrap::CensusPresentation(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    const bool& play_mode = *s.play_mode;
    const std::string& dump_frames_dir = *s.dump_frames_dir;
    uint64_t& dump_frames_written = *s.dump_frames_written;
#if PS2R_DIAGNOSTICS
    ps2r::diag::WordWatch& word_watch = *s.word_watch;
    Vu0Trace& vu0_trace = *s.vu0_trace;
#endif
    EmuDomain& domain = *s.domain;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    ps2r::app::VideoStack& video = *s.video;
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer>& consumer = *s.consumer;
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>& gs_queue = *s.gs_queue;
#if PS2R_DIAGNOSTICS
    ps2r::diag::DrawlistRecorder& drawlist_recorder = *s.drawlist_recorder;
#endif
    ps2r::gs::CountingDrawIrSink& draw_sink = *s.draw_sink;
    GsStateSink& gs_state_sink = *s.gs_state_sink;
    GifParser& gif_parser = *s.gif_parser;
    ps2r::gs::GsMmioHandler& gs_mmio = *s.gs_mmio;
    uint64_t& dmac_tail_chases = *s.dmac_tail_chases;
    uint64_t& dmac_tail_chases_parked = *s.dmac_tail_chases_parked;
    uint64_t& gs_syncpath_flushes = *s.gs_syncpath_flushes;
    uint64_t& gs_syncpath_parked = *s.gs_syncpath_parked;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
#endif
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::sound::Mixer& mixer = *s.mixer;
    BoundarySequencer& sequencer = *s.sequencer;
    uint64_t& gs_frame_boundaries = *s.gs_frame_boundaries;
    uint64_t& gs_irq_raises = *s.gs_irq_raises;
    RpcRouter& rpc_router = *s.rpc_router;
    PadProtocol& pad_protocol = *s.pad_protocol;
    ps2r::sound::hle::SdrRemote& sdr_remote = *s.sdr_remote;
    std::string& pad_input_source = *s.pad_input_source;
    std::unique_ptr<PlatformInput>& live_input = *s.live_input;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    ps2r::platform::sdl::AudioPullRing& audio_ring = *s.audio_ring;
    ps2r::app::ClockCensus& clock_census = *s.clock_census;
    ps2r::platform::sdl::SdlAudioSink& audio_sink = *s.audio_sink;
    int64_t& audio_first_nonsilent_window = *s.audio_first_nonsilent_window;
    BoundaryProtocol& protocol = *s.protocol;
    uint64_t& display_ticks = *s.display_ticks;
    uint64_t& display_ticks_late = *s.display_ticks_late;
    uint64_t& audio_device_quanta = *s.audio_device_quanta;
    uint64_t& audio_device_nonsilent = *s.audio_device_nonsilent;
    uint64_t& wait_display_ticks = *s.wait_display_ticks;
    uint64_t& wait_hook_calls = *s.wait_hook_calls;
    LibdmaBindings& libdma = *s.libdma;
    ps2r::ipu::MpegApi& mpeg = *s.mpeg;
    PadReadinessHook& pad_readiness = *s.pad_readiness;
    const bool& pad_hook_installed = *s.pad_hook_installed;
    uint64_t& fmv_active_frames = *s.fmv_active_frames;
    uint64_t& fmv_window_first = *s.fmv_window_first;
    uint64_t& fmv_window_last = *s.fmv_window_last;
    bool& fmv_window_open = *s.fmv_window_open;
    ps2r::savestate::Quiescence& quiesce = *s.quiesce;
    TickThread& tick_thread = *s.tick_thread;
    ps2r::gs::PresentPolicy& present_policy = *s.present_policy;
    std::function<void()>& census_services = *s.census_services;
    // The presentation half of the census: the GS page, the sinks, the clocks,
    // the pads and the device; written after the services half above.
    auto write_census = [&]() {
        census_services();
        if (auto* meta = host.Meta()) {
            // MMIO accesses of the page; gs_priv_writes minus the w count = the
            // libgraph rows' direct shadow writes (each row store reaches the one
            // shadow twice: through the page and directly).
            meta->Set("gs_priv_mmio", "r" + std::to_string(gs_mmio.Reads()) + ",w" + std::to_string(gs_mmio.Writes()));
            meta->Set("gs_sink", "tags=" + std::to_string(gs_state_sink.Tags()) + ",regs=" + std::to_string(gs_state_sink.RegisterWrites()) +
                                     ",packed=" + std::to_string(gs_state_sink.PackedWrites()) + ",kicks=" + std::to_string(gs_state_sink.Kicks()) +
                                     ",image_qw=" + std::to_string(gs_state_sink.ImageQwords()) + ",eops=" + std::to_string(gs_state_sink.Eops()) +
                                     ",path3_fin=" + std::to_string(gs_state_sink.Path3Finishes()));
            SubsystemCensus::RecordGifParse(*meta, gif_parser, gs_state_sink);
            SubsystemCensus::RecordGifAttribution(*meta, gs_state_sink,
                                                  s.graphics_output ? s.graphics_output->BucketHintExact() : false);
#if PS2R_DIAGNOSTICS
            // Where the diag build's drawlist recording went (`off` when
            // the harness was off or the file could not be created).
            if (word_watch.Armed() && !cfg.harness.out_dir.empty()) word_watch.Flush(cfg.harness.out_dir + "/watch.log");
            meta->Set("watch_words", word_watch.Armed() ? word_watch.Summary() : std::string("none"));
            if (vu0_trace.Armed() && !cfg.harness.out_dir.empty()) vu0_trace.Flush(cfg.harness.out_dir + "/vu0_trace.log");
            meta->Set("vu0_trace", vu0_trace.Armed() ? vu0_trace.Summary() : std::string("none"));
            meta->Set("drawlist_recorded", drawlist_recorder.IsOpen()
                                               ? drawlist_recorder.Path() + ",draws=" + std::to_string(drawlist_recorder.Draws()) +
                                                     ",frames=" + std::to_string(drawlist_recorder.Frames())
                                               : std::string("off"));
#endif
            meta->Set("dump_frames", dump_frames_dir.empty() ? std::string("off")
                                                              : dump_frames_dir + ",written=" + std::to_string(dump_frames_written));
            meta->Set("drawir", "draws=" + std::to_string(draw_sink.Draws()) + ",jobs=" + std::to_string(draw_sink.Jobs()) +
                                    ",frames=" + std::to_string(draw_sink.Frames()) + ",hash=" + Hex(draw_sink.DrawHash()) +
                                    ",jobhash=" + Hex(draw_sink.JobHash()) + ",identity_stamped=" + std::to_string(gs_state_sink.IdentityStamped()) +
                                    ",identity_raw=" + std::to_string(gs_state_sink.IdentityRaw()) +
                                    ",identity_underflow=" + std::to_string(gs_state_sink.IdentityUnderflows()));
            {
                // Per-cause tick census: how many tick bodies each TickSync cause fired.
                std::string ticks;
                for (int c = 0; c < static_cast<int>(TickSync::Cause::Count); ++c) {
                    const TickSync::Cause cause = static_cast<TickSync::Cause>(c);
                    if (!ticks.empty()) ticks += ",";
                    ticks += std::string(TickSync::CauseName(cause)) + "=" + std::to_string(sequencer.Ticks().FiredByCause(cause));
                }
                meta->Set("ticks", ticks);
            }
            meta->Set("gs_vram_epoch", std::to_string(gs_state.Vram().UploadEpoch()));
            {
                const ps2r::gs::TransferEngine::Counters& x = gs_state.Xfer().Stats();
                meta->Set("gs_xfer", "trxdir=" + std::to_string(x.trxdir) + ",completed=" + std::to_string(x.completed) +
                                         ",local_local=" + std::to_string(x.local_local) +
                                         ",local_local_over_gpu=" + std::to_string(x.local_local_over_gpu) +
                                         ",local_host=" + std::to_string(x.local_host) +
                                         ",owned=" + std::to_string(x.owned));
            }
            // The pipeline reads the GS's image: equal to gs_vram_epoch when live.
            meta->Set("gs_frame_boundaries", std::to_string(gs_frame_boundaries));
            meta->Set("gs_irq_raises", std::to_string(gs_irq_raises));
            ps2r::app::VideoCensus::Record(*meta, video, gs_queue.get(), consumer.get(), cfg.harness.out_dir);
            meta->Set("libdma_calls", "reset=" + std::to_string(LibdmaBindings::ResetCalls()) +
                                          ",putenv=" + std::to_string(LibdmaBindings::PutEnvCalls()) +
                                          ",getchan=" + std::to_string(LibdmaBindings::GetChanCalls()) +
                                          ",sync=" + std::to_string(LibdmaBindings::SyncCalls()) +
                                          ",gifsync=" + std::to_string(LibdmaBindings::GifSyncCalls()));
            meta->Set("dmac_tail_chases", std::to_string(dmac_tail_chases));
            meta->Set("dmac_tail_chases_parked", std::to_string(dmac_tail_chases_parked));
            meta->Set("gs_syncpath_flushes", std::to_string(gs_syncpath_flushes));
            meta->Set("gs_syncpath_parked", std::to_string(gs_syncpath_parked));
            meta->Set("libdma_dropped_stores", std::to_string(libdma.DroppedStores()));
            meta->Set("snd_frames", std::to_string(mixer.FramesMixed()));
            meta->Set("snd_kon", std::to_string(spu_core.KonCount()));
            meta->Set("snd_active", std::to_string(spu_core.ActiveVoices()));
            meta->Set("audio_first_nonsilent_window", audio_first_nonsilent_window < 0 ? "none" : std::to_string(audio_first_nonsilent_window));
#if PS2R_IOP_ORACLE
            meta->Set("libsd_calls", stream_irx.Libsd().CallsCensus());
            meta->Set("libsd_trans", std::to_string(stream_irx.Libsd().VoiceTransBytes()) + "/" + std::to_string(stream_irx.Libsd().HandlersFired()) +
                                         "/" + std::to_string(stream_irx.Libsd().BatchRecords()));
            {
                // The IOP-side CD drive as the module drove it (sceCdRead issues, sectors
                // moved, completion callbacks fired, the registered callback) and the
                // hard timers whose periodic beat is running -- the two title-armed
                // states a restore must carry besides the thread table.
                const ps2r::iop::hle::CdvdmanModule& cd = stream_irx.Cdvdman();
                meta->Set("iop_cdvd", "reads=" + std::to_string(cd.ReadsIssued()) + ",sectors=" + std::to_string(cd.SectorsRead()) +
                                          ",callbacks=" + std::to_string(cd.CallbacksFired()) + ",cb=" + Hex(cd.CallbackAddr()));
                {
                    // What the IOP side still owes the EE at this instant: queued drive
                    // completions (deferred kernel events), RPC end-callbacks not yet run,
                    // and whether the kernel's next due (thread delay, event, timer beat)
                    // lies inside the current frame (at or before the frame ceiling).
                    const ps2r::iop::IopKernel& k = iop_table.Kernel();
                    const std::optional<uint64_t> due = k.NextDueUs();
                    const uint64_t ceiling = k.Clock().FrameCeilingUs();
                    meta->Set("iop_pending", "events=" + std::to_string(k.PendingEventCount()) + ",rpc=" + std::to_string(rpc_router.PendingCount()) +
                                                 ",due_in_frame=" + std::string(due && *due <= ceiling ? "1" : "0") +
                                                 ",next_due_us=" + (due ? std::to_string(*due) : std::string("none")) + ",now_us=" + std::to_string(k.Clock().NowUs()) +
                                                 ",ceiling_us=" + std::to_string(ceiling));
                }
                std::string timers;
                for (int id = 1; id <= ps2r::iop::hle::TimrmanModule::kMaxTimers; id++) {
                    if (!stream_irx.Timrman().Running(id)) continue;
                    if (!timers.empty()) timers += ',';
                    timers += std::to_string(id);
                }
                meta->Set("iop_timer_running", timers.empty() ? std::string("none") : timers);
                // ... and whether those timers BEAT: the kernel's beat count and the FRAME
                // clock's base (after a restore the base is the anchor's frame count).
                const ps2r::iop::IopKernel& iop_kernel = iop_table.Kernel();
                meta->Set("iop_hb_beats", std::to_string(iop_kernel.TimerBeats()));
                meta->Set("iop_restored_timer_due", cfg.iop.restored_timer_due == ps2r::IopRestoredTimerDue::AtRestore ? "at_restore" : "one_period");
                meta->Set("iop_clock", "frames=" + std::to_string(iop_kernel.Clock().Frames()) +
                                           ",us=" + std::to_string(iop_kernel.Clock().NowUs()));
            }
#else
            // The play binary: the libsd model, the IOP-side drive and the IOP
            // kernel are not linked -- the rows keep their never-booted values.
            meta->Set("libsd_calls", "");
            meta->Set("libsd_trans", "0/0/0");
            meta->Set("iop_cdvd", "reads=0,sectors=0,callbacks=0,cb=0x0");
            meta->Set("iop_pending", "events=0,rpc=" + std::to_string(rpc_router.PendingCount()) +
                                         ",due_in_frame=0,next_due_us=none,now_us=0,ceiling_us=" + std::to_string(ps2r::app::ServiceTimeline::kFrameUs));
            meta->Set("iop_timer_running", "none");
            meta->Set("iop_hb_beats", "0");
            meta->Set("iop_restored_timer_due", cfg.iop.restored_timer_due == ps2r::IopRestoredTimerDue::AtRestore ? "at_restore" : "one_period");
            meta->Set("iop_clock", "frames=0,us=0");
#endif
            meta->Set("libsdr_calls", ps2r::sound::hle::LibsdrBindings::CallsCensus());
            meta->Set("sdrdrv_calls", std::to_string(sdr_remote.Calls()));
            meta->Set("pad_areas", Hex(pad_protocol.Area(0)) + "," + Hex(pad_protocol.Area(1)));
            meta->Set("pad_stage", std::to_string(static_cast<int>(pad_protocol.StageOf(0))) + "," + std::to_string(static_cast<int>(pad_protocol.StageOf(1))));
            meta->Set("pad_input", pad_input_source);
            if (PlatformInput* pad = live_input ? live_input.get() : s.pad_probe ? s.pad_probe->get() : nullptr) {
                meta->Set("pad_live", pad->Census());
                // The source contract's census (kbm always live, the gamepad when
                // connected, active = the last event's source, hot-plug counts).
                meta->Set("input_sources", pad->Sources());
            }
            // The key route's census: key events the main pump drained, F-key
            // presses seen, and F-key presses the GUI discarded (another
            // process's window id -- a silent hotkey is a FAIL, never a mystery).
            if (session) {
                // The shutdown's step census (the close request -> the worker's
                // join; each stop/join named with its ms): the tester's "shutdown
                // exceeded 10 seconds" reads which step was in progress.
                if (session->Shutdown().Begun()) {
                    meta->Set("shutdown", session->Shutdown().Render());
                    meta->Set("shutdown_ms", std::to_string(session->Shutdown().TotalMs()));
                }
                const auto keys = session->Keys();
                meta->Set("hotkeys", "key_events=" + std::to_string(keys.key_events) + ",function_keys=" +
                                         std::to_string(keys.function_keys) + ",discarded=" + std::to_string(keys.discarded));
                // The display swaps the presenter ran on its own thread:
                // count and the longest in ms -- a swap is never on the
                // game thread, so pace_work_max_ms must not carry it.
                const auto swaps = session->DisplaySwaps();
                meta->Set("display_swap", std::to_string(swaps.count) + "," + std::to_string(swaps.max_us / 1000) +
                                              ",total_ms=" + std::to_string(swaps.total_us / 1000));
                // The scripted changes (harness.display_sequence): the audio pads
                // and the game thread's longest frame work SINCE the first change.
                meta->Set("display_sequence", s.display_sequence ? s.display_sequence->Census(audio_ring.padded(), protocol.WorkMaxSinceMarkUs())
                                                                 : std::string("off"));
            }
            meta->Set("audio_sink", audio_sink.Running() ? "sdl" : "none");
            meta->Set("present_policy", play_mode ? present_policy.Census() : std::string("off"));
            meta->Set("pace_parks", std::to_string(protocol.PaceParks()));
            meta->Set("pace_late", std::to_string(protocol.PaceLate()));
            meta->Set("pace_timeouts", std::to_string(protocol.PaceTimeouts()));
            meta->Set("pace_late_fields", std::to_string(sequencer.PaceLateFields()));
            // Rule 3, single-field form: the backlog boundaries (one field each, no
            // park) and the backlog fields the catch-up cap discarded.
            meta->Set("pace_catchup", std::to_string(protocol.PaceCatchUp()));
            meta->Set("pace_dropped_fields", std::to_string(protocol.PaceDroppedFields()));
            meta->Set("fast_forward", s.fast_forward ? s.fast_forward->Census() : std::string("off"));
            meta->Set("fast_forward_fields", std::to_string(sequencer.FastForwardFields()));
            meta->Set("pace_slept_ms", std::to_string(protocol.PaceSleptUs() / 1000));
            meta->Set("pace_work_max_ms", std::to_string(protocol.WorkMaxUs() / 1000));
            meta->Set("pace_late_trace", std::to_string(protocol.LateTraceCount()) + ":" + protocol.LateTrace());
            // Frame skip: every tick longer than one field period
            // with its host-site breakdown (TickProfile: ordinal:work_us:pending:
            // loader=us/calls,tex=,mesh=,shadow=,sky=,save=,alloc=,gs=,boundary=,other=),
            // the longest tick's breakdown, the per-site totals (ms/calls) and the
            // loader census's load windows in boundary ordinals -- RootNoLongTick
            // judges the ticks outside those windows against the period. The windows
            // come from the loader's file windows AND the title's chunk state machine
            // (ChunkStateCensus -> TickProfile::NoteChunkEvent: loading/unloading/
            // releasing chunk steps and the CUT, a chunk's first Shown update), so
            // there is no tail (load_tail_ticks=0); chunk_cuts lists the cut ticks.
            meta->Set("pace_long", std::to_string(protocol.LongTraceCount()));
            meta->Set("pace_long_trace", protocol.LongTrace().empty() ? std::string("none") : protocol.LongTrace());
            meta->Set("pace_work_max_breakdown", protocol.WorkMaxBreakdown().empty() ? std::string("none") : protocol.WorkMaxBreakdown());
            meta->Set("tick_profile", protocol.SiteTotals());
            meta->Set("load_windows", protocol.LoadWindows());
            meta->Set("load_tail_ticks", std::to_string(BoundaryProtocol::kLoadTailTicks));
            meta->Set("load_ticks", std::to_string(protocol.LoadTicks()));
            meta->Set("chunk_ticks", std::to_string(protocol.ChunkTicks()));
            if (s.chunk_state_census)
                meta->Set("chunk_state_census", std::string(s.chunk_state_census->Installed() ? "on" : "nohook") +
                          ",loader_steps=" + std::to_string(s.chunk_state_census->LoaderSteps()) +
                          ",chunk_steps=" + std::to_string(s.chunk_state_census->ChunkSteps()) +
                          ",cuts=" + std::to_string(s.chunk_state_census->Cuts()));
            meta->Set("chunk_cuts", std::to_string(protocol.ChunkCutCount()) + ":" + protocol.ChunkCuts());
            // The clock contract (rule 5): per 1 s wall window -- boundaries (game_hz),
            // fields delivered (field_hz), frames presented (render_fps).
            meta->Set("game_hz", play_mode ? clock_census.GameHzSeries() : std::string("off"));
            meta->Set("field_hz", play_mode ? clock_census.FieldHzSeries() : std::string("off"));
            meta->Set("render_fps", play_mode ? clock_census.RenderFpsSeries() : std::string("off"));
            // Rule 3 (single-field form): the pacer edges the bounded catch-up left
            // unanswered, per window -- a stall is visible where it happened.
            meta->Set("pace_dropped_hz", play_mode ? clock_census.DroppedSeries() : std::string("off"));
            // The Display page's rows and the display-rate camera track.
            if (s.display_rows && *s.display_rows) (*s.display_rows)->Record(*meta);
            // The game thread's own CPU per window (ms; with game_hz = CPU ms per
            // frame, the EE's cost on a loaded machine) and over the run.
            meta->Set("ee_cpu_ms", play_mode ? clock_census.EeCpuMsSeries() : std::string("off"));
            meta->Set("ee_cpu_total_ms", std::to_string(clock_census.EeCpuTotalMs()));
            meta->Set("domain", "tick=" + std::to_string(domain.DomainTick()) + ",absorbed=" + std::to_string(domain.Absorbed()) +
                                    ",regime=" + std::to_string(domain.Regime()));
            meta->Set("tick_thread", tick_thread.Running() || tick_thread.Ticks() > 0
                                         ? "ticks=" + std::to_string(tick_thread.Ticks()) + ",resyncs=" + std::to_string(tick_thread.Resyncs())
                                         : "none");
            meta->Set("fmv_active_frames", std::to_string(fmv_active_frames));
            meta->Set("display_ticks", std::to_string(display_ticks) + "," + std::to_string(display_ticks_late));
            // The timer-wait side: every protocol wait (the FMV park's included),
            // demands deferred off-guest, the kernel timer-wait hook's calls and
            // the display ticks a play-mode kernel wait presented.
            meta->Set("timer_waits", std::to_string(protocol.Waits()) + "," + std::to_string(protocol.DeferredDemands()) +
                                         ",hook=" + std::to_string(wait_hook_calls) +
                                         ",display_ticks=" + std::to_string(wait_display_ticks));
            meta->Set("fmv_pacer_reanchors", std::to_string(mpeg.PacerReanchors()));
            meta->Set("fmv_window", fmv_window_open ? std::to_string(fmv_window_first) + "-" + std::to_string(fmv_window_last)
                                                    : std::string("none"));
            meta->Set("savestate_refused_fmv", std::to_string(quiesce.RefusalsBy("fmv_active")));
            meta->Set("audio_ring", "pushed=" + std::to_string(audio_ring.pushed()) + ",dropped=" + std::to_string(audio_ring.dropped()) +
                                        ",pulled=" + std::to_string(audio_ring.pulled()) + ",underruns=" + std::to_string(audio_ring.underruns()) +
                                        ",padded=" + std::to_string(audio_ring.padded()) + ",relatches=" + std::to_string(audio_ring.relatches()) +
                                        ",trimmed=" + std::to_string(audio_ring.trimmed()) +
                                        ",pad_with_tail=" + (audio_ring.PadWithTail() ? "1" : "0"));
            // The device side: quanta pushed to the ring (boundary + field pumps) and how many carried sound.
            meta->Set("audio_device", "quanta=" + std::to_string(audio_device_quanta) + ",nonsilent=" + std::to_string(audio_device_nonsilent));
            meta->Set("pad_readiness", std::string(pad_hook_installed ? "hook" : "nohook") + "," + (pad_readiness.EventRaised() ? "raised" : "none") +
                                           "," + std::to_string(pad_readiness.Entries()));
            host.FlushInput();
        }
    };
#if PS2R_DIAGNOSTICS
    WatchFlush().census = [&]() {
        write_census();
        auto* meta = host.Meta();
        if (!meta || cfg.harness.out_dir.empty()) return;
        if (std::FILE* f = std::fopen((cfg.harness.out_dir + "/watch_census.meta").c_str(), "w")) {
            const std::string text = meta->Render();
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    };
    // The census closure captures this frame's locals: unbound when Run returns.
    static int watch_census_token = 0;
    const std::unique_ptr<int, void (*)(int*)> watch_census_reset(&watch_census_token, [](int*) { WatchFlush().census = nullptr; });
#endif
    std::function<void()> write_census_fn = write_census;
    s.write_census = &write_census_fn;
    return ArmParkSink(s);
}
