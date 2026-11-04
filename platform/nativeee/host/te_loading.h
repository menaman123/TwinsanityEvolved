// te_loading.h -- THE LOADS (disc.speed): the game's loads at the speed of the device it runs on, with its loading picture.
//
// A LOAD. The game loads a level behind its loading picture in two states of its controller: LoadingTitle (6: the hub the
// title stands on, once per start) and LoadingLevel (9: LOAD GAME, a level from the level select, the reload after a game
// over, a restart whose checkpoint chunk is no longer loaded -- GameController::Restarting sends every reload there). The
// picture is read and fades in over 0.5 s; on the first frame it is fully shown every chunk is unloaded and the level's chunk
// queued; the streaming loader (ChunkLoaderBase::StepStates) reads it, then the chunks linked to it; StartingPlay follows and
// the picture fades out as the level fades in. At the console's pace a load is 9-14 s: the modelled DVD reading the level's
// 13-21 MB (2.2 MB/s and the seeks: stream_service.h) and the loader's two waits of a second (a chunk is loaded once it has
// been wanted a second -- the streaming's hysteresis at a link, made for play -- once for the level's chunk, once for its links).
//
// AT THE DEVICE'S SPEED (disc.speed = device, play mode's default): while the game is in a loading state
//   * the disc reads at this device's speed (StreamService::SetLoading, from the hook here): a read's bytes land as soon as
//     they are read -- in the call on a computer, once the PS Vita's reader thread has them off the card (te::io);
//   * the loader does not wait its seconds: Platform::System::LoadReadsDone() answers 1 while no read the game asked for is
//     in flight (ReadsDone), and StepStates then starts a wanted chunk's load at once (game/src/game/chunkloading.cpp: the
//     game's loader change; the PS2 waits its seconds);
//   * every boundary is paced as any other -- the game's clock is the wall's: the picture fades in over its 0.5 s and stays
//     until the level fades in. What is left of a load is that fade-in and the device's own work: the reads, the parse into
//     the game's memory, the textures and the meshes.
// Play keeps the console's pace (a chunk streamed in during play lands block by block, as the drive model delivers it: read
// whole in one tick it was a 12.7 ms spike of the game thread).
//
// AT THE CONSOLE'S PACE (disc.speed = console, --disc-speed console; the deterministic profile's default): the drive model and
// the loader's waits in the loads too -- the console's load, the timeline every fixture was recorded on. A pad log replayed
// keeps the disc speed of its recording (its `# disc-speed=` line, none = the console's: padlog.cpp).
//
// THE CENSUS, a line per load at its end (nothing else is read of the game's memory than the controller's state word,
// te::harness::ReadGameState):
//   [te.load] LoadingLevel @2152-2196: 44 boundaries in 0.738 s at the device's speed: 228 reads, 19.0 MB (12.3 ms reading
//   in the calls); LoadReadsDone 1 to the loader 9 times
//   [te.load] LoadingLevel @2152-2882: 730 boundaries in 12.163 s at the console's pace
#pragma once

namespace te::loading {

// The boundary hook on the services' clock (the game's state read at every boundary): the stream service told as a load at
// the device's speed begins and ends, the census line at a load's end. Installed once, before the game starts (the runner;
// a test after each Services reset); says the run's disc speed.
void Install();
// The game is in a loading state (as of the last boundary) and the disc reads at the device's speed in it
bool DeviceSpeed();
// Platform::System::LoadReadsDone's answer: DeviceSpeed() and no read the game asked for in flight
bool ReadsDone();

}  // namespace te::loading
