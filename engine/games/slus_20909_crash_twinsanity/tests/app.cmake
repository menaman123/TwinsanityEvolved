# The title-bound rows of tests/app: the Root* process pins -- each RUNS the built nativeee /
# nativeee_oracle binary the way the harness does (verify/run_leg.py), bounded through
# verify/run_bounded.sh, against this title's frozen anchors, savestates and pad logs
# (PS2R_OG_ROOT / PS2R_CONSOLE_IMAGES_DIR; SKIP 77 when absent) -- and the VideoStack resize pin
# over the composition's video stack. Registered only when both binaries exist (a disc image /
# STREAM.IRX gives the oracle arm; cmake/TitleInputs.cmake).
include_guard(GLOBAL)
set(_app "${PS2R_TITLE_TESTS}/app")
set(_app_engine "${CMAKE_SOURCE_DIR}/tests/app")   # the engine's shared helpers (origin/main)
if(TARGET nativeee AND TARGET nativeee_oracle)
# RootInstallsKernelClocks: a 2-frame deterministic coldboot leg must report
# kernl_timer_clock=1, finalized=1 and a park other than tick_clock_absent
# (DerivedClocks::InstallKernelClocks wired in ee_bootstrap.cpp).
add_test(NAME RootInstallsKernelClocks
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_kernel_clocks.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootInstallsKernelClocks PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootRegistersVuVifGifSections: a 2-frame deterministic anchor leg (the artdet
# anchor, OLD save_states read-only; SKIP 77 when absent) must report
# vu_sections_restored=4/1/1, vif_gif_sections_restored=1/1/0,
# dmac_save_unquiesced=0, restore_mhash_ok=1 and finalized=1 (VuMemorySection,
# Vu1RegisterSection, VifsSection, GifsSection and Dmac::SavestateSection
# registered with the StateRestorer in ee_bootstrap.cpp).
add_test(NAME RootRegistersVuVifGifSections
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_vu_vif_gif_sections.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootRegistersVuVifGifSections PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootBindsDmacSyncSeams: a 2-frame deterministic anchor leg (the artdet anchor,
# OLD save_states read-only; SKIP 77 when absent) must report
# dmac_tail_chases=2 (GsSync::BindDmac: Dmac::ChaseVif1Tail at every boundary,
# before RunBoundary), gs_syncpath_flushes >= 1, libdma_rows=6 and the
# sync=/gifsync= fields of libdma_calls (LibdmaBindings::Bind registers the two
# sync rows) -- the item 5.5 wiring in ee_bootstrap.cpp.
add_test(NAME RootBindsDmacSyncSeams
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_dmac_sync_seams.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootBindsDmacSyncSeams PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootRegistersIopmSection: a 2-frame deterministic anchor leg (the artdet
# anchor, OLD save_states read-only; SKIP 77 when absent) with --state-every 1
# must report iopm_restored=1, iop_ram_hash_restored equal to the anchor's own
# IOPM RAM bytes, iop_unrouted=0, mc_bound=1 with
# mc_bound_source=restored_client_record, and its boundary-0 image must carry
# the restored STREAM.IRX module's reply header word (RAM0[0x3b3004] ==
# 0x7fff0020, not the cold module's counter) -- IopmSection::MakeSpec
# registered after MCHL and McCommandModel::DeriveBound at the --load-state
# restore in ee_bootstrap.cpp.
add_test(NAME RootRegistersIopmSection
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_iopm_mc_restore.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --frames 2 --timeout 50)
set_tests_properties(RootRegistersIopmSection PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# After an anchor restore the STREAM.IRX streaming worker (tid 2) the image records
# as started is re-created by the plugin's boot-symmetric re-park and parked on its
# semaphore (iop_threads_restored from the LIVE kernel table), and the stream
# position word RAM0[0x3b3008] advances across the boundaries as OLD's paired
# images do. Without the re-park the worker is absent, sema 0 banks an unconsumed
# token and the position is frozen (I5.G3's INT-I5G3-STREAM-WORKER-DORMANT).
add_test(NAME RootRestoresStreamWorker
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_stream_worker_restore.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --frames 4 --timeout 60)
set_tests_properties(RootRestoresStreamWorker PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

# RootRestoresStreamHeartbeat: the same 4-frame artdet anchor leg must report
# iop_timer_running listing hard timer 1 -- the module's heartbeat, armed by the
# title's init RPC on the saver and recorded in the image's descriptor at IOP 0x14530;
# the plugin's re-park re-arms it from those words. Without the re-arm the loader
# (which restores before any title frame) reports `none` (I5.G7's
# INT-I5G7-HEARTBEAT-TIMER-NOT-REARMED).
add_test(NAME RootRestoresStreamHeartbeat
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_stream_heartbeat_restore.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 4 --timeout 60)
set_tests_properties(RootRestoresStreamHeartbeat PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

# RootRoutesGsPageAndDrawIr: a 2-frame deterministic anchor leg (the artdet anchor,
# OLD save_states read-only; SKIP 77 when absent) must report no 0x12000000 /
# 0x12001000 page in unhandled_mmio_pages (GsMmioHandler registered over GsState,
# item 6.2), gs_priv_writes > 0 with gs_priv_mmio w > 0 (the ONE GsPrivRegs shadow
# fed through the page), gs_sink regs/kicks/eops > 0 and drawir=draws>0,jobs>0
# (GsStateSink + GsSinkFanout feed GsState, the counting DrawIrSink sees the draws,
# item 6.4), gsops_ops > 0 (the recorder stays attached), a drawir.ohash with one
# `<boundary>,0x<hash>` line per frame beside gsops.ohash, drawir identity_stamped > 0
# with identity_underflow = 0 (the invariant-28 stamp, 6.4-B), a `classes=` census
# (video=gl) routing stamped draws into the title's stamps classes (the consumer's
# handler-name hook bound) and the per-cause `ticks=` census with boundary = N.
add_test(NAME RootRoutesGsPageAndDrawIr
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_gs_page_drawir.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootRoutesGsPageAndDrawIr PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootVideoStackPresents: a 2-frame deterministic anchor leg (the artdet anchor, OLD
# save_states read-only; SKIP 77 when absent) must report video=gl, a gl_renderer,
# presents > 0, gs_frame_boundaries=2 / drawir frames=2 (the VSync job per boundary),
# gfx_consumer draws > 0 with uploads_dropped=0, and a frames.fhash whose lines are
# NOT the Phase-3 black-frame hash (VideoStack + DrawIrQueueConsumer + the slot-0 /
# Verify present steps in ee_bootstrap.cpp, items 6.3 / 6.5). SKIP 77 honestly when
# the leg reports video=stub (no GL context on the machine).
add_test(NAME RootVideoStackPresents
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_video_stack_presents.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootVideoStackPresents PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootBindsFmvRows: a 200-frame deterministic coldboot leg must report
# libmpeg_rows=15, libipu_rows=4, avcodec_version set, fmv= inits=1, libipu_calls
# init=1, unhandled_mmio=0, finalized=1 and a park_detail that is neither the
# former `libipu.a sceIpuInit` park (frame 135) nor a `libcdvd.a sceCdSt*` park
# (IpuDevice + MpegApi + the two binding families + the sceCdSt wrapper rows
# wired in ee_bootstrap.cpp / LibcdvdBindings). The served-picture pin follows.
# ALL-ON build (2026-10-09): every hle.* default is on except
# graphics_frame / graphics_strips=plain / graphics_sprites / stream_files. The
# retained-arm pins below therefore state EVERY dependent key of the retired arm
# explicitly (the validator refuses movie_step/start/wait/stop without
# graphics_movie, music_buffers/audio_mix without the lifecycle); nothing is weakened.
set(PS2R_RETAINED_MOVIE_ARM
    --set hle.movie_lifecycle=off --set hle.graphics_movie=off
    --set hle.movie_step=off --set hle.movie_start=off --set hle.movie_wait=off --set hle.movie_stop=off
    --set hle.music_buffers=off --set hle.audio_mix=off)
# ALL-ON build (2026-10-09): under the owned Input/Save services and the libmc t0
# format read (hle.input_service / hle.save_storage / hle.libmc_format_in_t0) the title
# performs its memory-card check at boot ("Checking memory card (PS2) in memory card slot
# 1", frames 151-366 of the deterministic coldboot: +216 frames before the first movie) and
# the owned pad service changes frames 120-130 -- the retained arm (BUG-089's stack read
# of *format, under which the frozen baselines and every fixed-frame pin below were
# recorded) skipped that check. Every test whose premise is the OLD boot timing (a movie
# inside N boundaries, a fixture of frames 1-130, a judged frame key, cutscene windows)
# states that retained arm EXPLICITLY; nothing is weakened. The owned arms are censused on
# the all-on acceptance leg.
set(PS2R_RETAINED_BOOT_TIMING_ARM
    --set hle.input_service=off --set hle.save_storage=off --set hle.libmc_format_in_t0=off)
# ALL-ON, NO EXCEPTIONS (2026-10-09 ~11:30): the five Graphics keys the first
# all-on build kept off (graphics_frame / graphics_strips=all / graphics_sprites /
# graphics_shadows / graphics_text) are now the compiled default, and the deterministic
# coldboot ABORTS under them (frame 63 `Graphics Movie presentation release refused`
# with the movie picture on; frame 113 `Shadow model load could not establish owned
# geometry` with it off) -- the KNOWN CRASH the user plays into (the triage bundle
# attributes it). Every Root test whose premise is a coldboot running PAST frame 60
# was recorded under the retained Graphics arms and states them EXPLICITLY below;
# nothing is weakened. RootPlayModeSmoke does NOT carry the arm: it is the all-on
# smoke (its red at all-on IS known-crash #1, recorded with its bundle), and
# RootTriageBundleOnFault pins the crash path itself.
# hle.graphics_textures (textures at load, 2026-10-11) joins the arm: it converts every
# texture record at its read and the title's heap no longer holds the model's texture
# transfers (RootMetaArenaOff's console descriptor count is the model's: 385 vs 270 under it).
# hle.graphics_meshes (2026-10-12) requires graphics_textures: stated off with it
# (the loader would degrade it with a note otherwise; the arm names every key it turns off).
set(PS2R_RETAINED_GRAPHICS_ARM
    --set hle.graphics_frame=off --set hle.graphics_strips=plain --set hle.graphics_sprites=off
    --set hle.graphics_shadows=off --set hle.graphics_text=off --set hle.graphics_textures=off
    --set hle.graphics_meshes=off)
# Cutover flip 4 (2026-10-08): hle.movie_lifecycle defaults ON. The four tests below
# measure the RETAINED IPU/libmpeg lifecycle arm (sceIpuInit tail, FmvAbortHold,
# BVOL register gains, retained PCM input), so each passes the old value EXPLICITLY
# (--set hle.movie_lifecycle=off); nothing is weakened. RootMovieLifecyclePlay is
# the selected arm (explicit on). Cutover flip 7 (2026-10-08, REVERTED) flipped
# hle.graphics_movie, which requires the lifecycle: the same four retained-arm tests
# state the retired picture arm explicitly too (--set hle.graphics_movie=off),
# default-independent.
add_test(NAME RootBindsFmvRows
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_fmv_rows.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 200 --timeout 120 --require-subsystem-census
            ${PS2R_RETAINED_MOVIE_ARM} ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootBindsFmvRows PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)

# RootHoldsFmvAbort: a 1500-frame deterministic coldboot leg must report
# fmv_served_per_playback starting 201,346 (the golden picture windows of both intro
# movies under the live binary), fmv_pictures_served=547, fmv_abort_holds >= 1 without
# `,nohook`, finalized=1 and frames=1500 (FmvAbortHold installed on the title's scene
# updater in ee_bootstrap.cpp, INTEGRATION_PLAN row 7.7). Without the hold the leg
# reports 2,2+ (the two priming serves per movie).
add_test(NAME RootHoldsFmvAbort
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_fmv_abort_hold.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1500 --timeout 600 ${PS2R_RETAINED_MOVIE_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootHoldsFmvAbort PROPERTIES TIMEOUT 700 SKIP_RETURN_CODE 77)

# RootMixesFmvPcm: a 400-frame deterministic coldboot leg must report
# fmv_pcm sink=mixer with pushed > 0, mixed > 0, dropped=0, gate_opens >= 1 and
# mixed + queued <= pushed, snd_frames=320000, finalized=1, frames=400 (the mixer's
# FmvPcmInput bound as MpegApi's FmvPcmSink in ee_bootstrap.cpp, INTEGRATION_PLAN
# row 7.2). Without the binding the leg reports sink=tally and no mixed= field.
add_test(NAME RootMixesFmvPcm
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_fmv_pcm.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 400 --timeout 300 ${PS2R_RETAINED_MOVIE_ARM} ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootMixesFmvPcm PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)

# RootGatesPlayModeDevices: a 4-frame deterministic coldboot leg must report
# audio_sink=none, audio_ring pushed=3200 / pulled=0 (the mixer output is pushed into
# the AudioPullRing every boundary; the SdlAudioSink opens in play mode only),
# pad_input neutral|replay (no live InputSystem headless), avcodec_version=<x.y.z>
# and env_hash == the FNV fold of env_ps2's entries + avcodec_version (env_ps2 itself
# unchanged) -- INTEGRATION_PLAN rows 7.1 / 7.5 / 7.8 in ee_bootstrap.cpp + main.cpp.
add_test(NAME RootGatesPlayModeDevices
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_mode_gates.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 4 --timeout 50)
set_tests_properties(RootGatesPlayModeDevices PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootTriageBundleOnFault (ALL-ON, NO EXCEPTIONS): the crash path on a real process.
# A deterministic headless coldboot with the harness-only `--set harness.inject_fault_frame=1300`
# throws at boundary 1300 (the title screen: the model still draws there -- the drawlist ring records
# the GS model's DrawIR stream, and since tracks 2b/2c the boot screens' sprites, the screen
# filter and the frame clears are all OWNED deliveries, so the ring is EMPTY for the first 400+ frames;
# a fault at 100 read `faulting_draw: none`); the leg must end non-zero with run.meta park=exception /
# finalized=1 / triage_bundle=<dir>, write <harness-out>/triage/<UTC>/ {census.txt,
# drawlist_last200.txt, fault.txt, backtrace.txt, run.meta} (all non-empty), print the
# `census:` block at exit, and `--set hle.movie_wait=off` must degrade hle.movie_stop with the
# one logged `[config] ...` line (hle.graphics_movie=off cascades to hle.movie_start, a
# native-startup consumer the service-only binary REFUSES -- RootPlayBinaryServiceOnly pins
# that). The knob is refused outside a harness leg.
add_test(NAME RootTriageBundleOnFault
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_triage_bundle_on_fault.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1360 --fault-frame 1300 --timeout 300)
set_tests_properties(RootTriageBundleOnFault PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)

# RootGraphicsBridgeAllOn (shared-cause attribution, 2026-10-09): the five all-on/flip
# Graphics failures -- frame 63 (Movie presentation release), 113 (shadow model load),
# 119 (first owned text group), 118 (first owned strip group), 60 (sprite textures) --
# as five 300-frame deterministic coldboots at the compiled all-on defaults, each
# completing (park=none, graphics_output_faulted=0) with the family's owned work
# positive and the ordered-group lifecycle rows (owned failed=0, failed_groups=0,
# every graphics_refusals_* faults=0) at zero. No key is turned off.
add_test(NAME RootGraphicsBridgeAllOn
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_graphics_bridge_all_on.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 300 --timeout 50)
set_tests_properties(RootGraphicsBridgeAllOn PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootClockContract: the play-profile wall-paced replay of the s3 session
# (tests/app/fixtures/play_beach_session.padlog, 1500 frames) under the clock contract --
# domain tick == pacer ticks (every wall field reaches the title's clock once, none
# absorbed: rules 1/3), field_hz 58..62 in every in-loop 1 s window (the game's clock
# at wall speed), gs_queue frame_waits=0 frame_skip=1 (rule 2), audio_ring dropped=0
# underruns=0 (rule 4), the game_hz / field_hz / render_fps series published (rule 5);
# the in-loop game_hz min/max and the ordered waits are REPORTED. SKIP 77 without the
# title files or a display. Selftest judges canned run.meta values.
add_test(NAME RootClockContract
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_clock_contract.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_beach_session.padlog
            --frames 1500 --timeout 420)
set_tests_properties(RootClockContract PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootClockContractSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_clock_contract.py --selftest)
set_tests_properties(RootClockContractSelftest PROPERTIES TIMEOUT 60)
# RootMechaBandicootClock (2026-10-13): a play-profile wall-paced
# RESUME of the Mecha-Bandicoot state (OLD save_states/mecha_bandicoot_f37165.ps2st, saved on
# baf15a38 with aspect 21:9 / aniso 16 / point / vsync on -- passed as --set), 1200 frames:
# game_hz 59.5..60.5 in EVERY window (no in-loop filter), gs_queue frame_waits=0, the MTVU
# backpressure never parked the game thread (mtvu bp=0, kick_drop=on, tick_profile vu1wait=0).
# baf15a38 read game_hz 25 (a 560 ms bytes park every ~25 ticks: the red-before). SKIP 77
# without the title files, the state or a display. Selftest judges canned run.meta values.
add_test(NAME RootMechaBandicootClock
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mecha_bandicoot_clock.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --state ${PS2R_OG_ROOT}/save_states/mecha_bandicoot_f37165.ps2st
            --frames 1200 --timeout 240)
set_tests_properties(RootMechaBandicootClock PROPERTIES TIMEOUT 420 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootMechaBandicootClockSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mecha_bandicoot_clock.py --selftest)
set_tests_properties(RootMechaBandicootClockSelftest PROPERTIES TIMEOUT 60)
# RootClockContractDisplayRate: the same contract with the presenter at a simulated
# 120 Hz display (video.render_rate=display, harness.display_hz=120) -- the render rate never
# touches the game's speed (game_hz 59.5..60.5, frame_waits=0, audio dropped=0 underruns=0).
add_test(NAME RootClockContractDisplayRate
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_clock_contract.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_beach_session.padlog
            --frames 1500 --timeout 420
            --extra-set video.render_rate=display --extra-set harness.display_hz=120
            --extra-set video.vsync=off)   # the simulated display: the real swap interval would clamp it
set_tests_properties(RootClockContractDisplayRate PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootScreenModeSwap: the same wall-paced s3 replay (+ harness.live_pad_probe: the live
# pad read at every boundary beside the replay, the product path) with a scripted sequence
# of Display-page changes (harness.display_sequence: borderless / windowed / fullscreen, the
# internal scale, vsync) submitted through the menu widget's SessionControls route -- the
# presenter's thread runs each swap between two presented frames (display_swap=count,max_ms)
# while the game thread never carries one: its longest frame work from the first change on
# < 100 ms, no pace_late_trace stall, game_hz >= 50 in every window from the first change to
# the end, audio padded_after=0 dropped=0 underruns=0, frame_waits=0, the clock contract's
# judge green. The red-before (the s8 play session): the live pad's per-boundary round trip
# to main blocked the game thread for the whole macOS fullscreen transition (560-920 ms).
# Takes the display over for ~20 s (the Spaces transitions). ~1 min Release.
add_test(NAME RootScreenModeSwap
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_screen_mode_swap.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_beach_session.padlog
            --frames 1800 --timeout 480)
set_tests_properties(RootScreenModeSwap PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootScreenModeSwapSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_screen_mode_swap.py --selftest)
set_tests_properties(RootScreenModeSwapSelftest PROPERTIES TIMEOUT 60)
# RootNoLongTick (FRAME SKIP): the play-profile wall-paced replay of a session
# (s3 = play_beach_session 3068, s5 = play_s5_session 4968, s8 = play_s8_level_entry 8592 frames:
# the level-entry cut whose 45 ms tick dropped a frame) publishes every tick longer than one field
# period with its host-site breakdown (pace_long_trace, TickProfile) and the loader's load windows
# (load_windows, in boundary ordinals: the loader's file windows AND the title's chunk state
# machine -- loading/unloading/releasing chunk steps and the CUT, a chunk's first Shown update,
# ChunkStateCensus -- so load_tail_ticks=0 and chunk_cuts lists the cuts); judged: no long tick
# outside a load window or the FMV window, NO tail (the first tick after a window is a play tick),
# every cut inside a window. The frozen all-on 91dfdbe6 publishes none of the rows; baf15a38
# publishes a 15-tick tail and no chunk_cuts (s9 = the tester's level-entry session, the 32.5 ms
# cut 1178 three ticks past its window: the red-before). Needs an IDLE machine (a loaded host
# stretches ticks by scheduling alone). SKIP 77 without the title files or a display.
foreach(_nlt_arm "S3;play_beach_session.padlog;3068;300" "S5;play_s5_session.padlog;4968;330"
                 "S8;play_s8_level_entry.padlog;8592;420" "S9;play_s9_level_entry.padlog;4161;330")
    list(GET _nlt_arm 0 _nlt_name)
    list(GET _nlt_arm 1 _nlt_padlog)
    list(GET _nlt_arm 2 _nlt_frames)
    list(GET _nlt_arm 3 _nlt_timeout)
    add_test(NAME RootNoLongTick${_nlt_name}
        COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_no_long_tick.py
                --binary $<TARGET_FILE:nativeee>
                --game-files ${PS2R_OG_ROOT}/game_files
                --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
                --padlog ${PS2R_TITLE_TESTS}/app/fixtures/${_nlt_padlog}
                --frames ${_nlt_frames} --timeout ${_nlt_timeout})
    math(EXPR _nlt_ctest_timeout "${_nlt_timeout} + 180")
    set_tests_properties(RootNoLongTick${_nlt_name} PROPERTIES TIMEOUT ${_nlt_ctest_timeout} SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endforeach()
add_test(NAME RootNoLongTickSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_no_long_tick.py --selftest)
set_tests_properties(RootNoLongTickSelftest PROPERTIES TIMEOUT 60)
# RootHighFrameRate (video.render_rate=display): a 120 Hz display simulated by the
# presenter clock on the beach WALK route (tests/app/fixtures/play_beach_walk.padlog: the s3
# session + 8 s of UP once the autosave dialog is dismissed, so the follow camera pans): the
# game's logic stays at the 60 Hz tick (game_hz 59.5..60.5 in every in-loop window, EE-own-time
# windows bounded and printed) while the presenter shows 120 frames a second (render_fps >= 110,
# presents >= 1.8 x frames, frame_waits=0, audio dropped=0 underruns=0); the camera interpolated
# between the two latest ticks per presented frame is traced (<leg>/camera.trace) and the busiest
# continuous pan's per-present deltas have CV <= 0.5 and no backward step, where the tick-rate
# columns of the same presents (the red-before: duplicates at the display rate) read CV >= 2x.
# HONEST LIMITATION: the interpolated camera drives the census and the trace; the presented
# pixels at the display rate are the latest tick's frame until the owned producers re-render at
# the interpolated transform (the next slice). ~3.5 min Release wall-paced. Needs an IDLE machine
# (the EE-own-time bound, as RootClockContract).
add_test(NAME RootHighFrameRate
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_high_frame_rate.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_beach_walk.padlog
            --frames 7100 --timeout 900)
set_tests_properties(RootHighFrameRate PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootHighFrameRateSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_high_frame_rate.py --selftest)
set_tests_properties(RootHighFrameRateSelftest PROPERTIES TIMEOUT 60)

# RootPlayModeSmoke: a play-mode coldboot (a window, wall-paced boundaries:
# neither --headless nor time.tick_sync) must run PAST frame 60 -- the first
# kernel timer wait completes only when the play-mode ticker (TickThread) notes
# ticks the waiter fires -- and finish its --frames cap: frames=N, finalized=1,
# tick_thread=ticks=<k>, audio_sink=sdl, pad_input=live, video=gl,
# fmv_active_frames present. SKIP 77 without the title files or a display
# (video != gl: no window could be created).
add_test(NAME RootPlayModeSmoke
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_mode_smoke.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 120 --timeout 90)
set_tests_properties(RootPlayModeSmoke PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# The test of the smoke (cutover 2026-10-08): a crashed or killed game process (runner exit 134/124/125/137)
# or an unfinalized run.meta is a FAIL, never a SKIP; SKIP only for absent title files or a COMPLETED
# run that reported video=stub. Judged against a fake runner (tests/app/smoke_fake_runner.py), no game process.
add_test(NAME RootPlayModeSmokeSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_mode_smoke.py --selftest)
set_tests_properties(RootPlayModeSmokeSelftest PROPERTIES TIMEOUT 60)

# RootPlayPolicyInert: a deterministic headless coldboot
# `--frames 130` reproduces tests/app/fixtures/coldboot_130_lanes.txt line for line
# on audio.ahash / frames.fhash; mem.mhash stays advisory (the fixture was recorded from the binary
# BEFORE the Play-policy changes: wall-locked boundary, pacer release, present
# policy). SKIP 77 only without the title files.
add_test(NAME RootPlayPolicyInert
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_policy_inert.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --fixture ${CMAKE_SOURCE_DIR}/tests/app/fixtures/coldboot_130_lanes.txt
            --frames 130 --timeout 240 --advisory-mem ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootPlayPolicyInert PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootFootprintBounded: a deterministic headless coldboot `--frames 400`
# must keep the host footprint bounded by a frame's work, not the frame count: run.meta
# `footprint=` gsops_retained_bytes == 0 (the register-op recorder runs hash-only in a
# leg), peak_mb <= 1024, and the last two `footprint_series` samples (b300 -> b360) grow
# by <= 120 MiB. Before the fix the recorder retained 24 bytes per sink call for the whole
# leg (~1.2 GB by frame 400, ~8-11 MB per title frame, OS kill at ~4096 frames).
add_test(NAME RootFootprintBounded
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_footprint_bounded.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 400 --timeout 480 --peak-mb 1024 --tail-growth-mb 120 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootFootprintBounded PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
add_test(NAME RootFootprintBoundedSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_footprint_bounded.py --selftest)
set_tests_properties(RootFootprintBoundedSelftest PROPERTIES TIMEOUT 60)

# RootTitleScreenEffects: the live title-screen gate. A
# deterministic headless coldboot `--frames 2520 --dump-frames` on the RELEASE binary (the
# dev binary needs ~7.5 min for the same leg; Release ~90 s) must present frame 2519's three
# effect regions as the console does: LOGO letters' yellow within +-8/channel of (238,216,16)
# with >= 15000 yellow pixels, ORB bright share >= 0.35, OCEAN bright share >= 0.55 (the
# thresholds of Render_TitleFrameReplay, between the pre-fix live frame (213,177,65) /
# 0.098 / 0.472 and the console (238,216,16) / 0.417 / 0.659). Before the TEX1.K sign fix
# the live frame failed all three. SKIP 77 without build-release/ or the title files.
add_test(NAME RootTitleScreenEffects
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_title_screen_effects.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee_oracle
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2520 --key 2519 --timeout 480 ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootTitleScreenEffects PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootTitleScreenEffectsSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_title_screen_effects.py --selftest)
set_tests_properties(RootTitleScreenEffectsSelftest PROPERTIES TIMEOUT 60)

# RootTitleMusicPlays (all-on bug #1 of 2026-10-09): the title music STARTS under the
# compiled all-on audio composition (hle.music + hle.music_buffers + audio_mix + movie_*).
# A deterministic headless coldboot `--frames 1600` on the RELEASE binary through the boot
# movie to the title must report `music_library starts>0, refused_window/refused_buffers/
# refused_voice/errors=0`, `music_buffers allocations>0` with `music_buffers_binding=on`, and
# a numeric `audio_first_nonsilent_window` (the first all-on build: `starts=0 errors=2
# refused_buffers=2 audio_first_nonsilent_window=none` -- the paired child, which the title
# opens WITHOUT a buffer of its own, was refused by the buffer owner; the --selftest judges
# that run.meta as FAIL). 1600 frames because the owned boot (memory-card check) reaches the
# first audible window at 237 x 6 frames ~ 1422 (a 1300-frame leg has starts=0 either way).
# Play session s2 (2026-10-09, bug A): the music started in the MIDDLE of the attract cutscene
# -- the owned read charged the complete 4 MiB track window before readiness. TR
# multistream.h MsAllowKeyOn: the stream keys on once its BUFFERS are full (0x50000 bytes,
# 160 sectors); the rest streams in as refills. The pin is tightened: `fills>0`,
# `first_start_tick - first_play_tick <= 30` boundary ticks (fixed leg 14; the whole-window
# charge >= 120) and `audio_first_nonsilent_window <= 220` (fixed leg 210; before 237).
# The retained Graphics arm and hle.stream_files=off are stated EXPLICITLY (the known
# frames-60-220 Graphics crash class / proven coldboot abort, as every Root coldboot above);
# nothing about the audio keys is weakened. SKIP 77 without build-release/ or the title files.
add_test(NAME RootTitleMusicPlays
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_title_music_plays.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee_oracle
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1600 --timeout 540 --set hle.stream_files=off ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootTitleMusicPlays PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootTitleMusicPlaysSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_title_music_plays.py --selftest)
set_tests_properties(RootTitleMusicPlaysSelftest PROPERTIES TIMEOUT 60)

# RootShadowLevelEntry (play sessions of
# 2026-10-09 and 2026-10-10): the first level's default shadow meshes are ACCEPTED and DRAWN by the owned
# shadow pass; no model body runs inside an owned bracket; a pass with no mesh publishes
# nothing; and the footprint is in the PRESENTED frame under the PLAYER'S composition.
# Two arms: RootShadowLevelEntry at the compiled play default video.scale=1
# (2026-10-11) and RootShadowLevelEntryScale2 at `--video-scale 2`, the s4 shape (s4 showed the
# target dump at scale 1 green while the player saw no shadow at scale 2 -- the pass's private
# images were scaled while the backend's strip-count pass is native); the judge's boxes are
# stated on the 512x448 logical frame and scaled to each image, so both arms share one rule.
# A headless coldboot `--frames 7600` on the RELEASE binary with the deterministic profile's
# fields set one by one plus the arm's `video.scale` replays the tester's s3 recording
# (tests/app/root_shadow_level_entry.padlog: title -> New Game -> the first level -> the
# autosave dialog dismissed at tick 6477 -> Crash idle on the beach) with --dump-frames (only
# f7000/f7500 and the presented p7000/p7500 kept). run.meta must report `graphics_shadow_mesh calls>0 model_calls=0
# replaced==calls`, `graphics_shadow instances>0 blocks>0 groups>0 faults=0 refused_meshes=0`
# (a remaining refusal FAILS naming its check/result), `graphics_shadow_scene published>0` with
# published+empty == completed and pass empty == scene empty (an empty pass publishes nothing),
# `graphics_refusals_shadows total=0`, `park=none`, `graphics_output_faulted=0`, `video_scale=<arm>`;
# and the dumped frames -- the target grab f<key> (512x448 / 1024x896) AND the presenter's output p<key>
# (the window image) -- must show a dark footprint between Crash's feet (sand luminance >= 22
# below the ring beside him; -36 with shadows, -10 without) and no tall white vertical streak (s3:
# `model_calls=18728 replaced=0`, `refused_meshes=18728 check:4/result:2`, `instances=0`, the
# streak frame; the --selftest judges that census and such frames as FAIL). Compiled all-on
# defaults, nothing weakened. SKIP 77 without build-release/ or the title files. 7600 frames
# with the frame dump ~ 11-13 min Release (the dump doubles the leg's wall time).
add_test(NAME RootShadowLevelEntry
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_shadow_level_entry.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400)
set_tests_properties(RootShadowLevelEntry PROPERTIES TIMEOUT 2500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootShadowLevelEntryScale2
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_shadow_level_entry.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400 --video-scale 2)
set_tests_properties(RootShadowLevelEntryScale2 PROPERTIES TIMEOUT 2500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# The scale-3 arm (every owned pass scale-aware at the Display page's 3x; the judge's
# boxes scale to the 1536x1344 image as they do to the scale-2 one).
add_test(NAME RootShadowLevelEntryScale3
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_shadow_level_entry.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400 --video-scale 3)
set_tests_properties(RootShadowLevelEntryScale3 PROPERTIES TIMEOUT 2500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootWidescreenBeach (video.aspect): two arms of the s3 route (4:3 and
# 16:9, `--frames 7000`, headless Release, the deterministic fields stated one by one),
# judged on the PRESENTED frames p7000 (the beach) and p2900 (the save dialog) of both: the 16:9 frame is the 4:3 frame's scene
# squeezed by 3/4 about the centre (the projection's term) with world filling the outer
# bands (no pillarbox/box, no stretch), run.meta video_aspect= ratio=16:9 with every
# projection and culling frustum of the leg owned (passed_*=0), the widget fits owned
# (fit_place/fit_size>0) and SEEN: the dialog disc 0.75x as wide, centred (its arithmetic is ScreenAspect's bit-exact
# pin), and the 4:3 arm `ratio=4:3,model=off` (4:3 composes nothing: the lanes' identity).
# Red-before: without the model both frames are identical (the squeeze ratio fails).
# ~20-25 min Release (two 7000-frame legs with the frame dump).
add_test(NAME RootWidescreenBeach
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_widescreen_beach.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7000 --timeout 2400)
set_tests_properties(RootWidescreenBeach PROPERTIES TIMEOUT 5000 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootWidescreenBeachSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_widescreen_beach.py --selftest)
set_tests_properties(RootWidescreenBeachSelftest PROPERTIES TIMEOUT 120)
add_test(NAME RootShadowLevelEntrySelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_shadow_level_entry.py --selftest)
set_tests_properties(RootShadowLevelEntrySelftest PROPERTIES TIMEOUT 60)

# RootSpriteTitleLogo: the title screen's logo -- a Sprite,
# OLEG's titleLogo SpriteWidget over sprites[SpriteCrashTitle] (the Language\Titles picture)
# -- is drawn by the OWNED sprite path with the widened UI texture selection (TexturePairs +
# TextureConsumers Owned) and is VISIBLE in the PRESENTED frame. Before this stage every
# non-startup Sprite ran the model (`graphics_sprites replaced=0,model_calls=16508` on the
# 1300-frame all-on coldboot; the --selftest judges that census as FAIL), and the s4 lesson
# applies: a census that says "owned" over a presented frame without the logo FAILS. A
# deterministic headless coldboot `--frames 2520 --dump-frames` on the RELEASE binary (the
# frame of RootTitleScreenEffects, the logo fully faded in) must report `graphics_sprites
# replaced>0 model_calls=0 failed=0`, `graphics_sprites_model_by_texture total=0`,
# `graphics_refusals_sprites total=0 faults=0` (no sprite dropped at its ordered slot),
# `graphics_textures loaded>43`, and both f2519 (the lane's grab) and p2519 (the presenter's
# output) must carry >= 15000 yellow LOGO pixels whose mean is within +-8 (grab) / +-16
# (presented) per channel of the console's (238,216,16). SKIP 77 without build-release/ or
# the title files. ~2 min Release with the frame dump.
add_test(NAME RootSpriteTitleLogo
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_sprite_title_logo.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2520 --key 2519 --timeout 900)
set_tests_properties(RootSpriteTitleLogo PROPERTIES TIMEOUT 1000 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootSpriteTitleLogoSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_sprite_title_logo.py --selftest)
set_tests_properties(RootSpriteTitleLogoSelftest PROPERTIES TIMEOUT 120)

# RootScreenFilterLevel (2026-10-11): the owned bucket-4 screen
# effects (hle.graphics_screen) draw the first level's colour filter and the result is VISIBLE
# in the PRESENTED frame: on the RootShadowLevelEntry route at the composition the thresholds
# were measured on (video.scale=2; stated explicitly: the play default is 1 since),
# frames 7000/7500 on the beach, the FAR band's mean luminance (the scenery
# above the horizon, the frame's only Z < 0x10000 pixels) is >= 59.1 -- a threshold set
# between the two arms (owned 59.3-59.6, model 58.4-58.6), judged on the target grab AND the
# presenter's output; the census reads graphics_screen_selection=owned, model_calls=0,
# refused=0, faults=0, passes + filter_off == replaced. Red before on the same binary with
# --set hle.graphics_screen=off (the model arm: model_calls>0, the far band below 59.1).
# TRANSITIONAL: luminance only, ~1 unit of margin -- the direction of the shift toward the
# palette colour is NOT judged because no sky (the 30 % pull) is in view on these frames; a
# sky-in-view frame with a colour-direction judgement is the recorded follow-up.
add_test(NAME RootScreenFilterLevel
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_screen_filter_level.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400)
set_tests_properties(RootScreenFilterLevel PROPERTIES TIMEOUT 2500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootScreenFilterLevelSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_screen_filter_level.py --selftest)
set_tests_properties(RootScreenFilterLevelSelftest PROPERTIES TIMEOUT 60)

# RootSkyAttractFrame (2026-10-11): the owned bucket-0 sky pass
# (hle.graphics_sky) draws the sky the player sees. Before this stage every all-on sky start refused
# `missing_resource` (producer 0x1b9cf8 detail 4 ModelMissing): the sky MODEL's load-boundary
# conversion read GetDiskManager's block descriptors, which live in the META arena under
# hle.meta_arena, through a reader that covered main RAM and the scratchpad only. The deterministic
# 1300-frame coldboot reaches the attract cutscene's full-frame sunset sky (frame 1290): the census
# reads graphics_sky_start/model/finish replaced>0 model_calls=0, graphics_refusals_sky total=0,
# graphics_sky_assets models_loaded>0; the SKY band of the target grab AND the presented frame is a
# drawn sky (luminance >= 100, blue >= 110, blue >= 0.9 green) within 12/channel of the retail sky
# the player saw on that frame -- the frozen all-on main 1bb648b5's (182.5, 135.4, 129.0): the model
# sky under the attract shot's colour filter (this binary: owned sky + owned filter (182.8, 135.7,
# 128.6), owned sky alone byte-identical to the model sky). Red before: the frozen main fails the
# census (replaced=0, model_calls=54, refusals 54); the model arm of this binary (--set
# hle.graphics_sky=off) fails selection/model_calls (its frames pass: the owned sky IS the retail sky).
add_test(NAME RootSkyAttractFrame
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_sky_attract_frame.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1300 --timeout 600)
set_tests_properties(RootSkyAttractFrame PROPERTIES TIMEOUT 700 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootTexturePackTitle (2026-10-12): a texture pack reaches the PRESENTED frame.
# Two deterministic 1000-frame coldboots to the title screen: without a pack the census reads
# graphics_texture_pack pack=none replaced=0 and the presented logo band holds no pack colour (the
# identity arm, red-before for the pack judge); with a synthetic pack (`<paths.user_data>/textures/
# root_pin/<TextureContentHash>.png`, the six title-screen pictures as solid magenta at TWICE the
# authored resolution) the census reads files=hits=replaced=6 and the presented logo band is >= 20 %
# magenta (measured 0.35..0.39). Resolution-independent: the surface is 2x, the title's tuple 1x.
add_test(NAME RootTexturePackTitle
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_pack_title.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1000 --timeout 600)
set_tests_properties(RootTexturePackTitle PROPERTIES TIMEOUT 1300 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootTextureFilterTitle (2026-10-12): video.texture_filter reaches the PRESENTED frame.
# Three headless 1000-frame coldboots to the title screen (the deterministic fields stated one by one,
# --profile default, so the player's video.* keys apply): `title` (the identity arm: census
# video_texture_filter enhanced_draws=0 hw_lod_draws=0), `point` and `bilinear` (enhanced_draws>0).
# The presented p900 logo band differs between point and bilinear (>= 5 % of its pixels, mean |delta|
# >= 0.5; measured 0.19 / 1.79) and point's edges are steeper (steep-step share ratio >= 1.10; measured
# 1.28). Red-before: with the selection ignored the two frames are identical and both judges fail.
add_test(NAME RootTextureFilterTitle
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_filter_title.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1000 --timeout 600)
set_tests_properties(RootTextureFilterTitle PROPERTIES TIMEOUT 1900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootTextureFilterTitleSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_filter_title.py --selftest)
set_tests_properties(RootTextureFilterTitleSelftest PROPERTIES TIMEOUT 60)

# RootTextureFilterBeach (2026-10-12): trilinear + 16x anisotropy over the owned mip chains
# reach the PRESENTED frame of the first level. Two 7600-frame runs of RootShadowLevelEntry's route (title /
# trilinear+16x): the trilinear arm's census reads enhanced_draws>0 hw_lod_draws>0 (14.2 M of 14.6 M owned
# draws carry a chain), the presented p7000 far-ground band differs (>= 10 % of its pixels, mean |delta|
# >= 0.4; measured 0.246 / 1.10) while Crash's body box stays the same scene (<= 10 % differing; measured
# 0.008). Both arms: graphics_kick_lowering raw=0 unsupported=0, and graphics_cloth_producer
# installed=1 type17_packet>0 type1a_packet>0 -- the route flushes cloth materials every frame; their VU1
# programs are the library's vs_indexed_displace. Red-before: identical frames fail the band.
# ~25-30 min Release (two 7600-frame legs with the frame dump; the per-leg bound and the ctest TIMEOUT are
# RootWidescreenBeach's -- the same two-leg shape; the first registration's 1500/3200 timed out under a
# concurrent component-suite run). ONE game process at a time: run it alone (RUN_SERIAL).
add_test(NAME RootTextureFilterBeach
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_filter_beach.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400)
set_tests_properties(RootTextureFilterBeach PROPERTIES TIMEOUT 5000 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootTextureFilterBeachSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_filter_beach.py --selftest)
set_tests_properties(RootTextureFilterBeachSelftest PROPERTIES TIMEOUT 60)

# RootMeshLevelEntry (2026-10-12): the owned mesh bucket (hle.graphics_meshes) draws the
# first level's certified type-01 placed meshes in the PRESENTED frame and the PATH1 geometry those
# materials sent through VU1 no longer reaches the GS state machine. Two 7600-frame runs of
# RootShadowLevelEntry's route (owned = the compiled default / model = --set hle.graphics_meshes=off):
# the owned arm's census reads graphics_meshes placed_replaced>0 flush_replaced>0 draws>0 faults=0,
# graphics_mesh_materials certified>0 unknown_record=0 unhashed=0 and no reservation refusal; the
# model arm's reads selected=0 draws=0 placed_model_calls>0. Retirement: the owned arm's
# gs_sink_path1_by_bucket buckets 2+3 kicks <= 0.95 of the model arm's (measured 0.925 on
# 2026-10-12: 368.4 M vs 398.1 M; the residue is the certificate's -- 0 rigid IDs certified under
# the HUD rule, 546 k shared-consumer placed refusals) and bucket 2 strictly fewer (268.3 M vs
# 297.9 M). Presented p7000: the totem/crate boxes match the model arm within the 5-bit floor
# (>= 97 % of pixels within 8 levels per channel; measured IDENTICAL -- the two arms' frames.fhash
# and audio.ahash agree line for line) and Crash's body box stays the same scene. Red-before (both
# seen): the first composition overwrote FlushMaterial's wrapper (instances collected, never
# delivered: the totem gone from the frame) and TextureAssets::Upload's off-by-one mip finish drew
# every owned mesh BLACK (totem box share within the floor 0.130) -- the match judge fails both; a
# model arm with draws>0 fails the census judge. ~25-30 min Release; ONE game process at a time
# (RUN_SERIAL).
add_test(NAME RootMeshLevelEntry
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mesh_level_entry.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 7600 --timeout 2400)
set_tests_properties(RootMeshLevelEntry PROPERTIES TIMEOUT 5000 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootMeshLevelEntrySelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mesh_level_entry.py --selftest)
set_tests_properties(RootMeshLevelEntrySelftest PROPERTIES TIMEOUT 60)

# RootMechaBandicootMeshes (s11, 2026-10-13): the owned mesh bucket after a savestate RESTORE. The user's
# slot0 (the Mecha-Bandicoot arena, saved in the pause menu at frame 37165) is resumed under a window with
# the state's own video.* selection and the s11 fight padlog (1200 frames). The restored image carries
# no MaterialResourceRead receipt for its loaded materials (an older capture: no MSHR section), so
# MeshProducer::FrameAdmits refuses every frame WHOLE to the model path: graphics_mesh_materials
# mixed_frames=0 no_receipt=0 receipt_stale=0 table_unreadable=0, frames_admitted+frames_whole_model =
# every decided frame, no mesh fault, and the presented arena is drawn. Red-before: baf15a38's leg4
# (no_receipt=144000 next to replaced=0: every certifiable material fell to the writer INSIDE the
# owned bracket). `--model-binary` compares the presented arena against a frozen model-path binary
# (reported; windowed restores are not frame-deterministic across binaries). SKIP without the state /
# title files / a display. ~90 s Release; ONE game process (RUN_SERIAL).
add_test(NAME RootMechaBandicootMeshes
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mecha_bandicoot_meshes.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 1200 --timeout 900)
set_tests_properties(RootMechaBandicootMeshes PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootMechaBandicootMeshesSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mecha_bandicoot_meshes.py --selftest)
set_tests_properties(RootMechaBandicootMeshesSelftest PROPERTIES TIMEOUT 60)

add_test(NAME RootTexturePackTitleSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_texture_pack_title.py --selftest)
set_tests_properties(RootTexturePackTitleSelftest PROPERTIES TIMEOUT 60)

add_test(NAME RootSkyAttractFrameSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_sky_attract_frame.py --selftest)
set_tests_properties(RootSkyAttractFrameSelftest PROPERTIES TIMEOUT 60)

# RootMenuSfxAudible (play-session s2 bug B of 2026-10-09): the menu SFX are AUDIBLE under
# the compiled all-on audio composition (hle.sound_samples + audio_mix). A deterministic
# headless coldboot `--frames 2000` on the RELEASE binary replays a pad script (START at the
# title, then DOWN/UP x2 in the main menu) and must report `sound_controls plays>=2 errors=0`,
# every `sound_plays_head` record `started=1`, and `sound_output sample_nonzero_samples>0`
# (s2: `plays=39 errors=0`, `sample_nonzero_samples=0` -- the title pauses the sound service
# on the title->menu transition and keys the menu samples WHILE paused; the module's pause
# holds only the channels it finds keyed, the owned Audio muted every voice until resume; the
# --selftest judges the pre-fix leg's run.meta as FAIL). The retained Graphics arm and
# hle.stream_files=off are stated EXPLICITLY (as RootTitleMusicPlays); nothing about the audio
# keys is weakened. SKIP 77 without build-release/ or the title files.
add_test(NAME RootMenuSfxAudible
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_menu_sfx_audible.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee_oracle
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2000 --timeout 540 --set hle.stream_files=off ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootMenuSfxAudible PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootMenuSfxAudibleSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_menu_sfx_audible.py --selftest)
set_tests_properties(RootMenuSfxAudibleSelftest PROPERTIES TIMEOUT 60)

# RootServiceOnlyStartup: a coldboot to the title and its main menu with
# the IOP NEVER started -- `hle.startup=native`, the compiled all-on default. The eight
# sceSifLoadModule requests, the IOP heap and the STREAM RPC batches (server 0x12345) are
# answered by the native owners (NativeStartup / StreamTransport / SoundEffects / the
# libraries): run.meta must report `startup=native`, `iop_boot=0`, `iop_modules=` empty,
# `iop_hb_beats=0`, `stream_rpc_calls=0/0`, `libsd_calls=` empty, `service_startup=stream=
# native,sound=native,retained=none,consumers=0`, `startup_loads=8/0`, `sif_rpc_calls=<n>/0`
# (n > 0: the title still calls, none unbound), `stream_transport ... unowned=0 malformed=0
# server_calls==batches`, `iop_heap_native refused=0`, the title music started and audible
# (`music_library starts>0 fills>0`, `sound_output music_nonzero_samples>0`) and the menu SFX
# audible by RootMenuSfxAudible's own judge (the same pad script). The deterministic preset
# pins `hle.startup=retained` (the IOP oracle arm of the acceptance lanes), so the leg sets
# the preset's fields one by one instead. SKIP 77 without build-release/ or the title files.
add_test(NAME RootServiceOnlyStartup
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_service_only_startup.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2000 --timeout 540)
set_tests_properties(RootServiceOnlyStartup PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootPlayBinaryServiceOnly: the PLAY binary carries no IOP arm
# `--profile deterministic` runs native (iop_linked=0, iop_boot=0, the [config] note names the
# profile's retained pin as the oracle binary's), `--set hle.startup=retained` REFUSES by name,
# and the degrade edges (`--set hle.music=off`, `--set iop.dispatch=realtime`) REFUSE with the
# key named instead of booting an IOP. Four short headless legs on THIS build's play binary.
add_test(NAME RootPlayBinaryServiceOnly
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_binary_service_only.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --frames 4 --timeout 120)
set_tests_properties(RootPlayBinaryServiceOnly PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
add_test(NAME RootPlayBinaryServiceOnlySelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_binary_service_only.py --selftest)
set_tests_properties(RootPlayBinaryServiceOnlySelftest PROPERTIES TIMEOUT 60)

add_test(NAME RootServiceOnlyStartupSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_service_only_startup.py --selftest)
set_tests_properties(RootServiceOnlyStartupSelftest PROPERTIES TIMEOUT 60)

# RootAttractCutsceneFrames: the ground-truth frame gate of the in-engine "THREE
# YEARS AGO..." cutscene against the console (the PCSX2 GS dumps' screenshots under
# games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/gsdumps/). A deterministic headless
# coldboot `--frames 2350 --dump-frames` on the RELEASE binary (~85 s; the PPMs outside the
# windows are pruned as the leg runs) must show, for each of the four cutscene shots
# (attract_start / mid1 / mid2 / end), a frame within +-25 of the shot's NEW frame
# (1148 / 1580 / 1866 / 2214 under the loader's ring rule, edge new_b1048; 1233 / 1665 / 1951 / 2299
# at edge new_b1133 -- the centres follow the cold boot's scene edge) whose six region means are each within an ABSOLUTE 12/channel of
# the console's (measured residual <= 5.2; the near-black CAPTION makes a relative bound
# meaningless) and whose CAPTION bright share is >= 0.02 (console 0.046, NEW 0.034, a skipped
# caption draw 0). RED measured on windows shifted +-100 / +432 frames and on frames with the
# caption blanked. title_return is the title screen = RootTitleScreenEffects' pin. SKIP 77
# without build-release/, the title files or the shots. RUN_SERIAL: a 2350-frame Release leg.
# The console shots are private fixtures (game imagery): {PRIVATE_FIXTURES} = $NATIVEEE_PRIVATE_FIXTURES.
ps2r_add_private_fixture_test(NAME RootAttractCutsceneFrames
    FIXTURES games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/gsdumps/attract_start_f2845_shot.ppm
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_attract_cutscene_frames.py
            --binary ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee_oracle
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --shots {PRIVATE_FIXTURES}/games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/gsdumps
            --frames 2350 --radius 25 --tol 12 --caption-min 0.02 --timeout 600 ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootAttractCutsceneFrames PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
add_test(NAME RootAttractCutsceneFramesSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_attract_cutscene_frames.py --selftest)
set_tests_properties(RootAttractCutsceneFramesSelftest PROPERTIES TIMEOUT 60)

# RootPlayWallLock: a play-mode coldboot `--frames 240` with a
# GL window: boundaries == frames, the boundary consumed the pacer's ticks
# (pace_parks + pace_late == frames; pace_timeouts == 0) and present_policy census
# live+hold+none == frames with hold > 0 only after a first image. Pacing is
# MEASURED, not met (the gl frame can cost more than a field on this machine:
# 750 ms/frame measured): wall_ms is printed, pace_late > 0 is reported, and the
# wall bound is asserted only when pace_late == 0. SKIP 77 without the title
# files or a GL window. RUN_SERIAL: a wall-paced leg.
add_test(NAME RootPlayWallLock
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_wall_lock.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 240 --timeout 540 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootPlayWallLock PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# The test of its judge (cutover 2026-10-08): a crashed/killed game process or an unfinalized run.meta
# is a FAIL, never a SKIP; SKIP only for a COMPLETED video=stub run. Fake runner, no game process.
add_test(NAME RootPlayWallLockSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_wall_lock.py --selftest)
set_tests_properties(RootPlayWallLockSelftest PROPERTIES TIMEOUT 60)

# RootPlayFmvPresents (2026-10-02, row 6.7 FMV phase): a play-mode coldboot through
# both movies must present every served picture -- the composition root's display
# tick (a VSync job on every field the FMV pace park waits through and on every
# late serve) and the pacer's play-mode bounded re-anchor (slow motion, no blur).
# frames= is untouched by display ticks. SKIP 77 without the title files or a GL
# window. RUN_SERIAL: a wall-paced leg.
add_test(NAME RootPlayFmvPresents
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_fmv_presents.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 200 --timeout 840 ${PS2R_RETAINED_MOVIE_ARM} ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootPlayFmvPresents PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# The selected lifecycle must retain real play-mode movie sound and
# presentation while every IPU command and the old abort wrapper stay idle.
add_test(NAME RootMovieLifecyclePlay
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_fmv_presents.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 200 --timeout 840 --set hle.movie_lifecycle=on ${PS2R_RETAINED_BOOT_TIMING_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootMovieLifecyclePlay PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# The test of both movie smokes' judge (RootPlayFmvPresents and RootMovieLifecyclePlay share the script):
# runner exit 134 with an unfinalized run.meta -> FAIL (the flip-2/3 shape), no run.meta -> FAIL, finalized=0
# -> FAIL, a completed video=stub run -> SKIP, a good retained/owned run -> PASS. Fake runner, no game process.
add_test(NAME RootPlayFmvPresentsSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_fmv_presents.py --selftest)
set_tests_properties(RootPlayFmvPresentsSelftest PROPERTIES TIMEOUT 60)

# RootMtvuOffDeterministic: a 2-frame deterministic headless coldboot
# leg must report mtvu=off, savestate_refused_mtvu=0, vu1_library=title and
# finalized=1 -- the MTVU stack (app/host/mtvu_stack.*) is constructed ONLY
# under engine.mtvu, which the deterministic profile pins false, so the harness
# lanes are the inline path's.
add_test(NAME RootMtvuOffDeterministic
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mtvu_stack.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --mode off --frames 2 --timeout 50)
set_tests_properties(RootMtvuOffDeterministic PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# RootMtvuPlaySmoke: a play-mode coldboot with `--set engine.mtvu=on`
# (`--harness-window --frames 120`) must reach its cap with the MTVU worker
# running (mtvu=on,worker=1,...), every boundary through the ladder
# (deferred + terminal == 120, declared == presented == 120) and VIF1 walks
# staged to the worker (batches > 0). SKIP 77 without the title files or a
# display (video != gl).
add_test(NAME RootMtvuPlaySmoke
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_mtvu_stack.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --mode play --frames 120 --timeout 120 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootMtvuPlaySmoke PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootHeadroomQueryRecompiled: a 130-frame deterministic
# coldboot leg must report headroom_query=recompiled (HeadroomQueryGlue installed the
# title's generated headroom predicate FUN_00191bd0 over the generated TU's
# hle_TimerWaitMicroseconds hook BEFORE LoaderSpinLift wrapped the address), a
# ticks= census with demand=0 (the query fires no Demand tick; the wait seam fired
# 638 on this leg) and boundary=130, loader_spin_lifts without ,nohook, finalized=1.
# Cutover flip 6 (hle.stream_files): this pin measures the RETAINED load-wait arm
# (the STREAM module's read record under LoaderSpinLift's headroom query: ticks
# demand == fmv park_ticks + cdvd parked frames), so it passes the old value
# EXPLICITLY (--set hle.stream_files=off); nothing is weakened. The owned
# FileSchedule's own waits are pinned by FileScheduleSelfOracle / StreamFileSelfOracle.
# ALL-ON build (2026-10-09): hle.sound_samples composes the same owner and the owned
# file delivery (stream_files || sound_samples || music), hle.service_time requires one
# of them, and audio_mix / music_buffers / movie_start / movie_wait / movie_stop require
# music -- so the retained load-wait / CDVD-poll arm these two pins measure is stated
# with every dependent key EXPLICITLY; nothing is weakened.
set(PS2R_RETAINED_FILE_ARM
    --set hle.stream_files=off --set hle.music=off --set hle.sound_samples=off --set hle.service_time=off
    --set hle.music_buffers=off --set hle.audio_mix=off
    --set hle.movie_start=off --set hle.movie_wait=off --set hle.movie_stop=off)
# Cutover flip 9b (hle.music): the music key composes the sample/bank/control
# owner and the owned file delivery (SoundOutput(samples, music, output); the root's
# stream_files || sound_samples || music), which would retire the very load-wait arm
# this pin measures (ticks demand == fmv park_ticks + cdvd parked frames), so the
# retained arm is stated EXPLICITLY (--set hle.music=off) as well; nothing is weakened.
add_test(NAME RootHeadroomQueryRecompiled
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_headroom_query.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 130 --timeout 120 ${PS2R_RETAINED_FILE_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootHeadroomQueryRecompiled PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)

# RootVu1LibraryNone: a 2-frame deterministic artdet anchor
# leg with `--vu1-library none` (a harness CLI knob, not a Config key) must report
# vu1_library=none, vu1_hle=0/0/0 (the Vu1HleRuntime hook is not set; every image
# runs on the fallback tier), vu1_tier=cached, finalized=1; the same leg without the
# knob reports vu1_library=title with vu1_hle runs > 0 (the lanes are unchanged).
# SKIP 77 only when the title files / anchor are absent, never for a leg that ran.
add_test(NAME RootVu1LibraryNone
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_vu1_library_none.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2 --timeout 50)
set_tests_properties(RootVu1LibraryNone PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)

# RootFramesKeyConvention / RootDiffLegsCoalesced: a
# 3-frame restored artdet leg keys frames.fhash `1, restored_vsync_frame+1, +2` = the
# frozen baseline's first keys (the original's vsync-frame convention); a 70-frame
# coldboot leg compared with games/slus_20909_crash_twinsanity/verify/baseline/coldboot through diff_legs lists the
# coalesced presents 64,65 and carries `coalesced 2` on the verdict line (the verdict
# word itself is the gates' business). SKIP 77 only when the title files are absent.
add_test(NAME RootFramesKeyConvention
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_frames_pairing.py
            --mode anchor_keys
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --repo ${CMAKE_SOURCE_DIR}
            --frames 3 --timeout 60)
set_tests_properties(RootFramesKeyConvention PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
add_test(NAME RootDiffLegsCoalesced
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_frames_pairing.py
            --mode coalesced
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --repo ${CMAKE_SOURCE_DIR}
            --frames 70 --timeout 90 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootDiffLegsCoalesced PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)

# RootAnchorGsRestored: the root registers
# GsSections (VRAM + GSPR + GSST). A 3-frame restored artdet leg reports
# run.meta gs_sections_restored=1/1/1 and gs_restored_dispfb1=0x11000 (the anchor's
# DISPFB1), and its key-1 present -- the restore point's scanout -- is not the
# 512x448 black frame (0xa741b33a5c037124) a root restoring no GS section
# presents (S6: key 1 divergent on all 11 anchors). The exact pin, key 1 == the
# anchor's DISPFB decode, is BUG-076's OPEN item. SKIP 77 when the title files or
# the anchor are absent.
add_test(NAME RootAnchorGsRestored
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_frames_pairing.py
            --mode gs_restored
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --repo ${CMAKE_SOURCE_DIR}
            --frames 3 --timeout 60)
set_tests_properties(RootAnchorGsRestored PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)

# RootAnchorPadReady: a 4-frame restored artdet leg (soft backend) keeps
# port 0's READY slot image at boundary 0 (state 0x06 STABLE, len 0x20) and the
# title's controller SM in its gameplay case 12 at boundaries 2/3 -- PadProtocol
# holds a READY port across the restore instead of re-staging it from the
# restarted boundary clock (the controller-disconnected hold, case 15, of
# the pad-restore class). SKIP 77 when the title files or
# the anchor are absent.
add_test(NAME RootAnchorPadReady
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_anchor_pad_ready.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --frames 4 --timeout 240)
set_tests_properties(RootAnchorPadReady PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootAnchorF57292 / RootAnchorF59991: the two recompiler anchors run to
# --frames 600 and finalize with park=none on the regenerated shards. Before the recompiler's
# delay-slot rule the f57292 leg stopped at 15 frames and f59991 at about 126 (FUN_0027edf0
# spinning on a stack frame below the scratchpad after FUN_00178470 leaked 0x10 of sp per
# call). Measured about 70 s each at --frames 600 on the soft backend; the harness stall
# detector (RB_STALL_S=120) kills a re-stalled walk well inside the bound. SKIP 77 when the
# title files or the anchor are absent.
foreach(anchor f57292 f59991)
    string(TOUPPER ${anchor} anchor_uc)
    add_test(NAME RootAnchor${anchor_uc}
        COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_anchor_${anchor}.py
                --binary $<TARGET_FILE:nativeee_oracle>
                --game-files ${PS2R_OG_ROOT}/game_files
                --anchor ${PS2R_OG_ROOT}/save_states/ps2state_slot0_${anchor}.ps2st
                --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
                --frames 600 --timeout 540)
    set_tests_properties(RootAnchor${anchor_uc} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()

# RootAnchorHeartbeat: a 6-frame restored slowspot_f40298 leg (soft
# backend) must report iop_hb_beats >= 5 and iop_clock frames = the anchor's vsync
# frame + 6 (IopClock::RebaseToFrame in IopmSection::ReconcileKernel: the restored
# frame count IS the FRAME clock's frame base, so the re-armed STREAM.IRX heartbeat's
# due is reached by the boundary pumps), its state_b1 IOP voice-record type bytes
# (0x12f60..) carry no 4 with v29's reading 1 (the heartbeat-gated release pass ran),
# and its state_b2 SNDC voice model has v29 Off (the sweep's KOFF landed) -- the
# anchors' audio W0 class. SKIP 77 when the title files or the anchor are
# absent.
add_test(NAME RootAnchorHeartbeat
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_anchor_heartbeat.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/slowspot_f40298_s1789693059.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --voice 29 --frames 6 --timeout 240)
set_tests_properties(RootAnchorHeartbeat PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootAnchorHeartbeatAtRestore ((c) behind Config iop.restored_timer_due):
# the same slowspot_f40298 leg under `--set iop.restored_timer_due=at_restore` -- the
# restored RUNNING hard timer is due AT the restore (TimrmanModule::RestoreHardTimer arms
# IopKernel::StartPeriodicTimerAtUs(now)), so the heartbeat's first beat precedes the
# EE's RPC-woken sweep of boundary 0 and the sweep keys the probed voice v29 off: the
# script asserts iop_hb_beats >= frames and state_b1 v29 phase Off (the dropped
# check, re-enabled under the toggle). Default (one_period) behaviour stays pinned by
# RootAnchorHeartbeat above. SKIP 77 when the title files or the anchor are absent.
add_test(NAME RootAnchorHeartbeatAtRestore
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_anchor_heartbeat.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --anchor ${PS2R_OG_ROOT}/save_states/slowspot_f40298_s1789693059.ps2st
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --voice 29 --frames 6 --timeout 240 --restored-timer-due at_restore)
set_tests_properties(RootAnchorHeartbeatAtRestore PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootCdvdPollVisibility (attempt 3): tests/app/root_cdvd_poll_visibility.py runs a
# deterministic coldboot `--frames 300` under `--set cdvd.delivery_visibility=poll` and the default
# leg and asserts the poll model's properties: both legs finalize; cdvd_forced=0 and
# cdvd_wait_overruns=0 under poll (a blocking wait parks whole frames to the drive's end, never
# crossing a frame edge); cdvd_wait_parks > 0; wait + poll + boundary deliveries == async_issued;
# the boot case at the final image >= 6 and not below the boundary leg's. The read-count ratio is
# measured and asserted at --ratio 0.9: boundary mode is not one read per boundary (it force-
# completes every blocking wait: cdvd_forced=903 of 918 at 300 frames), so the two legs issue
# nearly the same reads (907 vs 918) -- the "4x" premise never held. RUN_SERIAL: two
# bounded legs. 500 frames: the poll leg once parked `fatal_no_jump pc=0` at frame 309
# (the title's STREAM client aged a host-served read's slot to done before the drive delivered);
# the leg must run past it; BUG-070 (the frame-624 load-wait spin: the lap waited on the STREAM
# module's service of a record, visible only at a boundary the spin never reached) is fixed by the
# lift's poll-visibility branch (the IOP's in-frame work at the poll, the lift when nothing is due),
# so the pin runs 1500 frames (~140 s poll, ~300 s boundary) and asserts both legs finalize with no
# park and the poll leg's idle lifts > 0. Both legs pass `--ring-cache off` (= `--set
# loader.ring_cache=off`, the per-record seam every figure above was measured on; the Config default
# is `on`): the boundary control leg is the pre-decision visibility
# model and does not finalize under the ring rule.
# Cutover flip 6 (hle.stream_files): both legs measure the RETAINED CDVD-poll arm (the
# module's reads landing at the title's poll: cdvd_poll_deliveries, wait + poll + boundary
# deliveries == async_issued), so both pass the old value EXPLICITLY (--set
# hle.stream_files=off); nothing is weakened. The owned FileSchedule delivers at its own
# poll/wait sites (file_schedule poll_delivered/wait_delivered in run.meta).
# Cutover flip 9b (hle.music): the music key composes the sample/bank/control
# owner and the owned file delivery, which would retire the CDVD-poll arm both legs
# measure (cdvd_poll_deliveries, wait + poll + boundary == async_issued, snd_kon > 0
# once boot case 7 keyed the title music), so the retained arm is stated EXPLICITLY
# (--set hle.music=off) as well; nothing is weakened. The owned music path is pinned by
# MusicSelfOracle / StreamFileSelfOracle and censused on the acceptance leg.
add_test(NAME RootCdvdPollVisibility
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_cdvd_poll_visibility.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --verify-dir ${CMAKE_SOURCE_DIR}/verify
            --frames 1500 --state-every 60 --ratio 0.9 --min-case 6 --timeout 700 --ring-cache off
            ${PS2R_RETAINED_FILE_ARM} ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootCdvdPollVisibility PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootReleaseLanesIdentical: the Release-vs-Debug identity
# guard that precedes every Release battery. A deterministic headless coldboot `--frames 200`
# and the artdet anchor `--frames 60` run on BOTH binaries (Debug = this build's nativeee,
# Release = build-release/app/host/nativeee) and the lanes are byte-compared: frames.fhash,
# mem.mhash, drawir.ohash, gsops.ohash whole, audio.ahash over the first N-1 windows (the
# last is the partial tail). Prints `guard=IDENTICAL` or the first differing lane/line.
# SKIP 77 when build-release/ is absent or the title files are missing. RUN_SERIAL: four
# bounded legs (the Debug coldboot 200 alone is ~40 s).
set(PS2R_LANES_DEBUG_ORACLE_BINARY "${CMAKE_SOURCE_DIR}/build/app/host/nativeee_oracle"
    CACHE FILEPATH "Matching Debug ORACLE executable for the Release lane guard (M12: the lanes are the oracle arm's)")
set(ps2r_lane_debug "$<IF:$<CONFIG:Debug>,$<TARGET_FILE:nativeee_oracle>,${PS2R_LANES_DEBUG_ORACLE_BINARY}>")
add_test(NAME RootReleaseLanesIdentical
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_release_lanes_identical.py
            --debug ${ps2r_lane_debug}
            --release ${CMAKE_SOURCE_DIR}/build-release/app/host/nativeee_oracle
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --repo "${PS2R_OG_ROOT}"
            --coldboot-frames 200 --anchor-frames 60 --timeout 540 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootReleaseLanesIdentical PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootGsQueueThreadIdentical: gs.queue=thread (the DrawIR
# consumer on its own thread, lockstep) reproduces gs.queue=inline byte for byte. A
# deterministic headless coldboot `--frames 130` and the artdet anchor `--frames 60` run
# in BOTH modes on this build's nativeee; frames.fhash, mem.mhash, drawir.ohash,
# gsops.ohash whole and audio.ahash over the first N-1 windows are byte-compared. Prints
# `pin=IDENTICAL` or the first differing lane/line, plus each leg's wall_ms (measured,
# not asserted). SKIP 77 without the title files or the anchor. RUN_SERIAL: four bounded
# gl legs (~4 min Debug).
add_test(NAME RootGsQueueThreadIdentical
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_gs_queue_thread_identical.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st
            --repo "${PS2R_OG_ROOT}"
            --coldboot-frames 130 --anchor-frames 60 --timeout 540 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootGsQueueThreadIdentical PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootGsQueuePipelinedIdentical (the split VRAM image): gs.queue=pipelined
# (the pipeline on its own thread AND its own VramImage, the EE's calls carried in the stream)
# reproduces gs.queue=inline byte for byte on the ALL-ON composition: the deterministic
# headless coldboot `--frames 200` in both modes, the five lanes byte-compared as above.
# No anchor leg (the pin stands without the frozen anchors). The s4 play-session replay
# (`--inputs play_sessions/s4/session.padlog --inputs-frames 4000`) is the stage's evidence
# run of the same script, not a ctest (~2 x 70 s). SKIP 77 without the title files.
add_test(NAME RootGsQueuePipelinedIdentical
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_gs_queue_thread_identical.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --anchor ${PS2R_OG_ROOT}/save_states/artdet_f37152.ps2st --anchor-frames 0
            --repo "${PS2R_OG_ROOT}"
            --coldboot-frames 200 --modes inline,pipelined --timeout 540)
set_tests_properties(RootGsQueuePipelinedIdentical PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# RootColdbootProbeResumes: the coldboot leg's frozen boundary image
# `old_b120.ps2st` (written by the original tree from a cold boot; the main thread's
# chain is crt0-rooted) restores on this build's nativeee AND resumes: `--frames 61`
# finalizes, run.meta heap_section_restored=1 (the HEAP section's META-arena bookkeeping
# follows the arena bytes -- before the fix the first post-restore metadata allocation
# re-vended the title's live compaction job and no boundary ever followed), restore_mhash
# equals the lane's mem line 120, and the resumed leg's first presented frame hashes equal
# to the frozen cold-boot lane's frame 121 (gl headless; the original tree's own next
# frame). Selftest pins the judge. SKIP 77 without the title files / the probe. ~3 s Debug.
# The frozen probe is a private fixture (a RAM image: game bytes); the lane's hash files stay in-tree.
ps2r_add_private_fixture_test(NAME RootColdbootProbeResumes
    FIXTURES games/slus_20909_crash_twinsanity/verify/baseline/coldboot/probe/old_b120.ps2st
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_coldboot_probe_resumes.py
            --binary $<TARGET_FILE:nativeee_oracle>
            --game-files ${PS2R_OG_ROOT}/game_files
            --probe {PRIVATE_FIXTURES}/games/slus_20909_crash_twinsanity/verify/baseline/coldboot/probe/old_b120.ps2st
            --lane ${CMAKE_SOURCE_DIR}/games/slus_20909_crash_twinsanity/verify/baseline/coldboot
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 61 --timeout 240)
set_tests_properties(RootColdbootProbeResumes PROPERTIES TIMEOUT 300)
add_test(NAME RootColdbootProbeResumesSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_coldboot_probe_resumes.py --selftest)
set_tests_properties(RootColdbootProbeResumesSelftest PROPERTIES TIMEOUT 60)

# RootRecordInputRoundTrip (product obligation): the play-session recorder
# command end to end on the actual binary -- a deterministic headless cold-boot leg replays a small
# pad script (`--inputs`) into an EXPLICIT recording (`--record-input <file>`); run.meta names that
# file (pad_record_path/start/last/lines), the recording begins with the header line, replays
# nothing the script does not hold and carries every scripted range; a second leg naming the same
# file exits 2 with "an existing file is never overwritten" and leaves the first recording's bytes.
# Parent-run (game process); the selftest pins the judge. SKIP 77 without the title files.
add_test(NAME RootRecordInputRoundTrip
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_record_input_round_trip.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 61 --timeout 240)
set_tests_properties(RootRecordInputRoundTrip PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
add_test(NAME RootRecordInputRoundTripSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_record_input_round_trip.py --selftest)
set_tests_properties(RootRecordInputRoundTripSelftest PROPERTIES TIMEOUT 60)

# RootPlayRecordReplay (the s4 recording hazard, 2026-10-11): a session recorded in PLAY replays
# deterministically. The recorder's clock is the GAME FRAME (the frames-lane key of the served
# boundary; header `clock=frame`, run.meta pad_record_clock=frame), never the logical input tick
# (wall-paced in play: s4 tick=8823 for 3993 frames -- the s4 padlog replayed onto the load-file
# menu). Leg A: the play profile (--harness-window, wall-paced) replays a frame-clock script for
# 600 frames while recording it; leg B: deterministic headless `--inputs <recording>`; leg C:
# deterministic headless `--inputs <script>`. Pins: A finalized with pad_record_clock=frame; the
# recording's per-frame held state == the script's EXACTLY over 1..600 (red on dc1a8a3a: the tick
# clock lost the three ranges whose ticks passed inside parks -- 150-170:LX, 500-560:RY,
# 590:SQUARE -- `differs at 83 frames`); B and C finalized and IDENTICAL on frames+audio
# (diff_legs). Parent-run (game process, a window); SKIP 77 without the title files or a display.
add_test(NAME RootPlayRecordReplay
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_record_replay.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 600 --timeout 240)
set_tests_properties(RootPlayRecordReplay PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootPlayRecordReplaySelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_record_replay.py --selftest)
set_tests_properties(RootPlayRecordReplaySelftest PROPERTIES TIMEOUT 60)

# RootPlaySaveAtTitle (Graphics capture-time settle, INTERFACES section 13; the s3 play bug
# "F5 at the title refuses `graphics: Graphics owned pending=N`"): the all-on binary CAPTURES a
# savestate at a TITLE boundary (1300) and the capture RESTORES. Leg A: a deterministic headless
# cold boot with the capture trigger -- `--set harness.save_at_frame=1300` (the product's F5 path)
# when the binary's --help names it, else `--state-every 1300` (the harness checkpoint writer) --
# writes the image (no refusal) with run.meta `graphics_capture_settle=...,unlinked=U,...,items=I`
# U > 0, I > 0: the settle admitted the frame's OPEN recording (its owned groups recorded before
# Present, Linked after SyncV: TR renderer.cpp:436-440) and published the GXPD digest. Leg B:
# `--load-state <image> --frames 120`: finalized=1, frames=120, restored_vsync_frame=1300, park=none,
# `graphics_restore_unlinked` items = I and exactly I ordering refusals detail 5 (kRestoredUnlinked)
# at the Link producer -- the one frame drawn without those groups is attributed, never silent.
# Red before the settle: `state_b1300.refused` = "graphics: Graphics owned pending=4,failed=0".
# Leg B depends on the SessionClock::Validate fix:
# without it `--load-state` of a deterministic image refuses "Saved clock/provider selection
# differs from the running session" (recorded, not weakened); with 16a-f applied the pin is green
# on the harness.save_at_frame trigger (int_work/m_series/graphics/settle/root_play_save_at_title_b16.log).
# SKIP 77 without the title files; selftest pins the judge (no game process). ~40 s Release.
add_test(NAME RootPlaySaveAtTitle
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_save_at_title.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --boundary 1300 --tail 120 --timeout 400)
set_tests_properties(RootPlaySaveAtTitle PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootPlaySaveAtTitleSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_save_at_title.py --selftest)
set_tests_properties(RootPlaySaveAtTitleSelftest PROPERTIES TIMEOUT 60)

# RootPlaySaveRestore (product F5/F8/--load-state, the s3 play-session bug "F5 writes nothing,
# no refusal"): the savestate SLOT round trip on the actual all-on binary at COMPILED DEFAULTS.
# Leg A: a deterministic headless cold boot `--frames 180 --set harness.save_at_frame=60` -- the F5
# capture path without a key press (SessionCapture::Capture/Poll, the same owners, writer, refusal
# text and .meta sidecar) -- must report run.meta `savestate=written,path=<save_dir>/savestates/
# slot0.ps2st,frame=60,boundary=60`, the slot + its `.meta` (frame, build_id = --version,
# composition = every hle.* key + profile, section tags) present. Leg B: `--load-state <slot>
# --frames 120` on the same binary: finalized=1, restored_vsync_frame=60, and every restored frame
# key (61..179) hashes equal to the uninterrupted run's key k+1 (the restored boundary presents the
# picture the uninterrupted run keyed one later; the shift is constant and reported). Boundary 60 is
# the pre-recording window (before the renderer's first owned recording); RootPlaySaveAtTitle above
# covers a title boundary (1300) through the Graphics capture-time settle (INTERFACES section 13).
# SKIP 77 without the title files; selftest pins the judge (no game process).
add_test(NAME RootPlaySaveRestore
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_save_restore.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --save-at 60 --tail 120 --timeout 300)
set_tests_properties(RootPlaySaveRestore PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootPlaySaveRefusal -- RETIRED at the savestate integration (2026-10-10): it pinned the all-on
# refusal `graphics: Graphics owned pending=N` at boundary 120 (every SyncV boundary from the
# renderer's first owned recording on), the gap the Graphics capture-time settle closes -- on the
# combined binary that trigger WRITES (`savestate=written,...,frame=120,boundary=120`,
# int_work/m_series/cutover/allon_saves/ctest-save-refusal-combined.log), exactly as the owner's
# note here foresaw ("turns RED on purpose ... or retire it"). The LOUD refusal path stays pinned:
# the judge (`root_play_save_restore.py --expect-refusal`, RootPlaySaveRestoreSelftest), the text
# (SessionControls / SavestateSlots units) and a real-process refusal with the session kept open
# (RootPlayRelaunchRestore leg D: `F8 restore Refused: ... composition differs`).
# RootPlayRelaunchRestore: F8's RELAUNCH route in a PLAY session end to end on the actual binary (a
# window, wall-paced) -- the key press without a key. Now the compiled default route is
# the LIVE in-process restore (RootPlayLiveRestore); legs B/D select the relaunch by setting
# (`--set save.restore_mode=relaunch`), the fallback a composition that retains IOP state takes.
# Leg A (play mode, `--set harness.save_at_frame=60`,
# the F5 path) writes the slot; leg B (`--set harness.restore_at_frame=100`) submits
# RestoreReproduction at boundary 100 exactly as F8 does: the newest slot, the live-registry
# preflight on the game owner, the session closes (`park_detail=session_closed`) and main execv's
# THIS binary (same pid) with `--load-state <slot>` and `--harness-out <out>/restores/<stamp>` (the
# one-shot trigger dropped). stderr carries `nativeee: savestate F8 restore Queued`, `... Completed:
# Restarting from <slot>`, `... Relaunch: <argv>`; the restored play session's run.meta under
# restores/<stamp>/ is finalized with frames=400, restored_vsync_frame=60, video=gl, park=none.
# Legs C/D (the composition preflight): a DETERMINISTIC-profile slot (leg C, headless) pressed in a
# play session (leg D, `--frames 160 --set harness.restore_at_frame=100`) is refused in the LIVE
# session by name -- `F8 restore Refused: <slot>: composition differs: profile(saved=deterministic,
# live=default)` -- with no Relaunch, no restores/, and leg D's own run.meta finalized=1 frames=160
# park=none park_detail=frames_cap (the window stayed open). SKIP 77 without the title files or a
# display. ~60 s (three windowed sessions + one headless). RUN_SERIAL.
add_test(NAME RootPlayRelaunchRestore
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_relaunch_restore.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 400 --timeout 240)
set_tests_properties(RootPlayRelaunchRestore PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootPlayRelaunchRestoreSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_relaunch_restore.py --selftest)
set_tests_properties(RootPlayRelaunchRestoreSelftest PROPERTIES TIMEOUT 60)
add_test(NAME RootPlaySaveRestoreSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_save_restore.py --selftest)
set_tests_properties(RootPlaySaveRestoreSelftest PROPERTIES TIMEOUT 60)
# RootPlayLiveRestore: F8 restores LIVE in
# the running play session on the actual all-on binary -- no relaunch, the window and the GL device
# kept, one game process. Leg A (play, `--frames 600 --set harness.save_at_frame=300`) writes the slot
# at an in-play boundary through the Graphics capture settle and runs on (the uninterrupted frames
# lane); leg B (play, `--frames 900 --set harness.restore_at_frame=600`, the compiled default
# save.restore_mode=live) submits RestoreReproduction at 600 as F8 does: stderr `F8 restore Queued`
# then `F8 restore Completed: Reproduction restored; approved continuations relaunched.`, NO
# `Relaunch:` line, no restores/; the pre-restore run.meta closes at frames=601 with f8=live and
# output_next=<out>/output-000001; the restored segment's run.meta is finalized with
# restore_route=live restored_vsync_frame=300 frames=299 park=none park_detail=frames_cap video=gl,
# graphics_live_restore resets=1, graphics_restore_unlinked == leg A's capture-settle digest, gs_queue
# frame_waits=0, audio_ring dropped=0 underruns=0 (the clock contract across the restore), and its
# frames lane starts at 301 with the uninterrupted run's frame 301. Leg C (`--load-state <slot>`): its
# restore_mhash equals the live segment's (the same image applied by both routes). The agreement of
# the restored frames with the uninterrupted run's is REPORTED (play frames are run-to-run variant:
# frame-skip, the FMV's wall-clocked decode); the byte identity is RootPlaySaveRestore's
# (deterministic profile). Red before the live route: `F8 restore Refused: A remaining asynchronous
# owner has no approved live-restore contract.` (harness.live_restore=on) or the relaunch's
# `Relaunch:` line + restores/ (the default). SKIP 77 without the title files or a display. ~70 s
# Release (three windowed sessions). RUN_SERIAL (one game process at a time).
add_test(NAME RootPlayLiveRestore
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_live_restore.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --save-at 300 --restore-at 600 --frames-a 600 --frames-b 900 --timeout 240)
set_tests_properties(RootPlayLiveRestore PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootPlayLiveRestoreSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_live_restore.py --selftest)
set_tests_properties(RootPlayLiveRestoreSelftest PROPERTIES TIMEOUT 60)
# RootPlayHotkeyF5 (the s5 play bug "F5 does nothing": savestates/ empty, no F5 stderr line): F5 in a
# PLAY session through the REAL key route on the actual binary. `--set harness.inject_key=F5@300`
# pushes a real SDL_KEYDOWN/KEYUP pair into SDL's own queue at boundary 300, stamped as SDL stamps a
# physical press (the focus window's id, 0 when SDL recorded none -- the s5 session's shape), drained
# by SessionRuntime's main pump: SDL_PollEvent -> SessionWindow -> SessionGui::ProcessEvent ->
# SessionControls::HandleFunctionKey -> Submit. (RootPlayRelaunchRestore's restore_at_frame calls
# Submit directly and never pinned this route.) Pins: stderr `harness.inject_key F5 at boundary 300:
# pushed into SDL's queue`, `nativeee: savestate F5 save Queued` then `... Completed`, no
# `nativeee: F5 discarded:`; <saves>/savestates/slot0.ps2st + .meta (frame >= 300); run.meta
# finalized=1 frames=420 video=gl park=none hotkeys=key_events=2,function_keys=1,discarded=0.
# Red before (the 8bd76e91 router + this knob) ONLY when macOS does not grant the process keyboard
# focus -- SDL then stamps the press windowID=0 and the old strict test discards it: `nativeee: F5
# discarded: SDL stamped window id 0 (session window 2)`, no slot, hotkeys discarded=1 (2/2 runs,
# int_work/.../root_pins/f5/red_arm_old_router). When macOS does grant focus (FOCUS_GAINED, press
# stamped with the window's id) the old router passes this leg too, so the Root red is reproducible,
# not deterministic; the deterministic red is the unit pin SessionHotkeyRoute case 1 (windowID 0).
# SKIP 77 without the title files or a display. ~25 s Release. RUN_SERIAL (one game process).
# RootMenuPerfOverlay: the owned menu
# extension's first option end to end on the actual play binary. Two play legs on the default
# profile replay tests/app/root_menu_perf_overlay.padlog (title -> START -> options -> the ADDED
# 'display' link -> the added Display page); leg A (3300 frames) stops on the page, leg B (3600)
# presses CROSS on 'fps overlay' at 3330. Pins: run.meta menu_ext state=registered,pages=2,items=7,
# texts=24,banks=1 (the title's OptionsPage::Construct built the pages through the title's own
# constructors), the Display page DRAWN with the title's menu font in the presented window image
# (--dump-frames -> w_final.ppm), leg A changes=0 / no overlay, leg B changes=1 / ui.perf_overlay=1 /
# the game Hz / render fps overlay VISIBLE top-right / <user data>/ps2r.ini `ui.perf_overlay = on`.
add_test(NAME RootMenuPerfOverlay
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_menu_perf_overlay.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames-a 3300 --frames-b 3600 --timeout 300)
set_tests_properties(RootMenuPerfOverlay PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootMenuPerfOverlaySelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_menu_perf_overlay.py --selftest)
set_tests_properties(RootMenuPerfOverlaySelftest PROPERTIES TIMEOUT 60)
# RootDisplayRowsLive: two Display-page rows changed from the title's own menu
# take effect LIVE and persist -- one play leg (tests/app/root_display_rows_live.padlog: the
# perf-overlay route to the Display page, then DOWN x2, RIGHT on 'internal scale' at 3420
# (1x -> 2x), DOWN, CROSS on 'vsync' at 3540; --frames 3700, --dump-frames -> w_final.ppm).
# Judged: menu_ext changes=2, menu_ext_pages video.scale=2 video.vsync=1; display_rows
# scale_setting=2 scale=2 scale_requests=1 scale_applied=1 scale_async=1 scale_inline=0 (ONE
# ordered job on the GS consumer, no wait on the game thread) grab=1024x896 (the consumer's
# latest display grab -- the presented frame -- at scale 2) vsync=1; gfx_consumer resizes>=1;
# the Display page still drawn in w_final; <user data>/ps2r.ini `video.scale = 2` +
# `video.vsync = on`. Red-before: no DisplayRows -> scale_requests=0, grab=512x448, no key.
add_test(NAME RootDisplayRowsLive
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_display_rows_live.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 3700 --timeout 300)
set_tests_properties(RootDisplayRowsLive PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootDisplayRowsLiveSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_display_rows_live.py --selftest)
set_tests_properties(RootDisplayRowsLiveSelftest PROPERTIES TIMEOUT 60)
add_test(NAME RootPlayHotkeyF5
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_hotkey_f5.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 420 --press 300 --timeout 200)
set_tests_properties(RootPlayHotkeyF5 PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootQuitGame / RootExitClean: the pause menu's QUIT
# GAME natively RETURNS TO THE TITLE SCREEN (QuitToTitle -> StateRestartingTitle -> LoadingTitle ->
# StateTitle -> the intro movie): the quit route (s9 beach route + START, UP, CROSS, UP, CROSS@2340)
# then the window closed at 2560 -- run.meta shows the title reload window right after the YES,
# movie_start starts=3, and the close completes: shutdown_ms < 1000 with every ladder step named
# (guest_join, savestate_writer, mtvu_worker, graphics_quiesce, effects_quiesce, gs_consumer,
# recorder_flush), none abandoned, no watchdog. RootExitClean: the beach replay closed mid-play at
# 900, the same close contract. Red-before: the s10 session's `Session shutdown exceeded 10 seconds`
# (finalized=0, no shutdown_ms). ~100 s / ~35 s Release.
add_test(NAME RootQuitGame
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_quit_exit.py --arm quit
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_quit_route.padlog
            --frames 2700 --close 2560 --confirm 2340 --timeout 400)
set_tests_properties(RootQuitGame PROPERTIES TIMEOUT 500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootExitClean
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_quit_exit.py --arm exit
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --padlog ${PS2R_TITLE_TESTS}/app/fixtures/play_beach_session.padlog
            --frames 1200 --close 900 --timeout 240)
set_tests_properties(RootExitClean PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# RootKbmPadOnly / RootPadHotplug (the 2026-10-13 kbm BUG contract): a LIVE play session
# (no --inputs, the pad recorded). kbm: NO gamepad (SKIP when one is connected) -- a scripted key
# sequence through the real SDL key route (Space, Space, W held, Tab) drives the title menu into the
# beach (the level load window), moves Crash (LY=0 rows; frames.fhash 2099 != 2300) and selects
# (SELECT: select=Tab bound by default); input_sources active=kbm. hotplug: an SDL virtual game
# controller attached at 2000 (real JOYDEVICEADDED), its cross held 2050-2080, detached at 2150; the
# keyboard's W after -- hotplug_added/removed >= 1, switches >= 2, no input lost either way.
# Red-before: with no controller the keyboard produced nothing. ~85 s each Release.
add_test(NAME RootKbmPadOnly
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_kbm_pad.py --arm kbm
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2400 --timeout 400)
set_tests_properties(RootKbmPadOnly PROPERTIES TIMEOUT 500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootPadHotplug
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_kbm_pad.py --arm hotplug
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 2400 --timeout 400)
set_tests_properties(RootPadHotplug PROPERTIES TIMEOUT 500 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME RootKbmPadSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_kbm_pad.py --selftest)
set_tests_properties(RootKbmPadSelftest PROPERTIES TIMEOUT 60)
add_test(NAME RootQuitExitSelftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_quit_exit.py --selftest)
set_tests_properties(RootQuitExitSelftest PROPERTIES TIMEOUT 60)
add_test(NAME RootPlayHotkeyF5Selftest
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_play_hotkey_f5.py --selftest)
set_tests_properties(RootPlayHotkeyF5Selftest PROPERTIES TIMEOUT 60)

# RootLoaderRingCache: a 121-frame deterministic coldboot leg with
# `--set loader.ring_cache=on` (explicit, although `on` is the Config
# default) holds the console's Default.rm2 stream ring at boundary 120
# (the EOF fill over the previous window's tail = pcsx2_rec159_ee.bin, the PCSX2 state at the
# same record count), the console's loader-object words (fs+0/+0xc/+0x10/+0x18, the cursor at
# base) and reports loader_ring=on,hits=887,straddles=12,misses=12,fills=24,direct=1 (19 fills
# for the 900 Default.rm2 records instead of 900). SKIP 77 without the title files or the
# saved console image (PS2R_CONSOLE_IMAGES_DIR, tests/CMakeLists.txt).
add_test(NAME RootLoaderRingCache
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_loader_ring_cache.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --console-dir ${PS2R_CONSOLE_IMAGES_DIR}
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 121 --timeout 240 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootLoaderRingCache PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootMetaArenaOff: a 121-frame deterministic coldboot leg with
# `--set hle.meta_arena=off` keeps the title's handle-heap descriptors on the title's page
# pool as the console does (pcsx2_rec159_ee.bin: 385 descriptors, every one a USED chunk of
# slab class 7/32 B, 12 class-7 pages, the console's first class-7 pages at NEW's indices);
# run.meta meta_arena=off,hooks=1,allocs=0,frees=0 (the retained free body never fires on a
# fresh boot). The red arm (`--arm on`) fails the same assertions (desc in META 385, 9 pages).
# SKIP 77 without the title files or the saved console image.
add_test(NAME RootMetaArenaOff
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_meta_arena_off.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --console-dir ${PS2R_CONSOLE_IMAGES_DIR}
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 121 --timeout 240 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootMetaArenaOff PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# RootTypeRegistryComplete: a 121-frame deterministic coldboot leg builds the
# title's type registry as the console does -- the MC manager's slot-count nibble 4, the 28-slot
# registry table at 0xba8a60 filled with objects of the console's class signatures, 126 USED
# class-22 (92 B) page-pool chunks (pcsx2_rec159_ee.bin). RED before the libmc sceMcGetInfo
# fifth-argument fix (nibble 1, 8 slots, 114 chunks). SKIP 77 without the title files or the
# saved console image.
add_test(NAME RootTypeRegistryComplete
    COMMAND ${Python3_EXECUTABLE} ${PS2R_TITLE_TESTS}/app/root_type_registry_complete.py
            --binary $<TARGET_FILE:nativeee>
            --game-files ${PS2R_OG_ROOT}/game_files
            --console-dir ${PS2R_CONSOLE_IMAGES_DIR}
            --runner ${CMAKE_SOURCE_DIR}/verify/run_bounded.sh
            --frames 121 --timeout 240 ${PS2R_RETAINED_GRAPHICS_ARM})
set_tests_properties(RootTypeRegistryComplete PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
else()
    message(STATUS "title tests/app: the Root* process pins are SKIPPED (they run nativeee / nativeee_oracle; give -DNATIVEEE_DISC)")
endif()   # TARGET nativeee AND TARGET nativeee_oracle
add_executable(test_video_stack_resize ${_app}/test_video_stack_resize.cpp
    "${PS2R_TITLE_SRC}/composition/video_stack.cpp")
target_include_directories(test_video_stack_resize PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_video_stack_resize PRIVATE ps2r_title_hle ps2r_gs_hle_queue
    ps2r_title_renderer_profile ps2r_render_gl ps2r_render_review ps2r_platform_sdl)
target_compile_options(test_video_stack_resize PRIVATE -Wall -Wextra -Werror)
add_test(NAME VideoStackDefersOffMainResize COMMAND test_video_stack_resize)
set_tests_properties(VideoStackDefersOffMainResize PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
