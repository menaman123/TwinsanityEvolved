// te_vita.h -- the PlayStation Vita runner's start-up: the device has no command line, no terminal and no
// home directory, so the arguments are baked (or read from ux0:data/twinsanity/args.txt), stderr goes to a log file under
// the same folder and the kernel's memory budget is reported once. Compiled on the Vita lane only (runner/te_vita.cpp).
#pragma once

namespace te::vita {

// Replaces argc/argv with the device's arguments (the storage they point at lives for the process) and redirects stderr
// / stdout to ux0:data/twinsanity/twinsanityevolved.log. Called first in main().
void Startup(int* argc, char*** argv);
// One start-up step line in the log (synced): the device run that dies without a word is located by the last one.
void Step(const char* what);
// The game runs (its first picture is on the display: graphics.cpp NotePresented): from here every thread's lines go to
// the card through the logger thread -- no thread waits on the memory card any more (the start-up's lines before this
// are written and committed one by one by the thread that logs them). Any thread; idempotent.
void LogRunning();

// Closes the log file after the run's exit census (main's last lines): Vita3K does not implement sceIoSyncByFd, so its
// host-side buffer reaches the disk per 4 KB and at close only -- an exit-time emulator fault lost the census before.
// Later lines go to the kernel's TTY only.
void CloseLog();

// One start-up line on the game module's code generation (CMake TE_VITA_UNCHECKED_MEMORY / TE_VITA_W2C_LEAN): which
// guards are compiled out, where the 32 MB guest memory sits, and the check behind TE_W2C_VFP_ABS -- VABS must clear
// the sign bit and nothing else (NaN payloads, signalling NaNs, denormals kept) under the game thread's FPSCR (RZ | FZ |
// DN on the device) and the default one. Returns false (and says so) when VABS is not the plain sign clear.
bool ReportModuleCode(const void* guest_memory);

}  // namespace te::vita
