// runner/te_startup.h -- starting without a terminal: the disc image named in a file beside the executable, a message
// box for a start that cannot go on, and (Windows) the parent terminal's console for the windowed executable.
#pragma once
#include <string>

#include "app/host/game_folder.h"

namespace te::startup {

// The file beside the executable that names the disc image (one line, the image's path). The launcher writes it into
// the folder it installs the game to, so the game starts from a shortcut with no arguments.
inline constexpr const char* kDiscImageFile = "disc-image.txt";

// The disc image named by kDiscImageFile beside the running executable, or empty.
std::string DiscImageBesideExecutable();

// The first start of a downloaded game: the player picks their disc image in the system's file dialog (Windows' Open
// dialog, macOS' chooser, zenity / kdialog on Linux). Empty when cancelled or no dialog exists.
std::string ChooseDiscImage();
// Writes `path` into kDiscImageFile beside the executable, so later starts need no dialog. False when the folder is
// read-only (the game then asks again next time).
bool RememberDiscImage(const std::string& path);

// Where the memory cards and ps2r.ini live: `userdata` beside the executable when it exists, or when the folder is
// writable and there is no earlier <home>/.twinsanityevolved (a downloaded game keeps everything in its own folder,
// on whatever drive it was unzipped to); else <home>/.twinsanityevolved. Empty when neither can be named.
// ("Beside the executable" is the GAME FOLDER: the one that holds the .app bundle on macOS.)
std::string DataDirectory();

// The game folder (engine/app/host/game_folder.h) of the running executable: its own folder, or -- macOS -- the folder that
// holds its .app bundle. Empty when the platform cannot name the executable (the Vita).
std::string GameFolderPath();
// Where the settings file, the memory cards and the mods folder are for this run (GameFolder::Locate over this executable's
// folder and DataDirectory()): ps2r.ini beside the game wins when present (portable); --config / --mc-dir / --mods-dir
// (empty = not given) override.
ps2r::app::GameFolder::Resolved LocateFolders(const std::string& config_arg, const std::string& mc_dir_arg, const std::string& mods_dir_arg,
                                              ps2r::app::GameFolder::Inputs* inputs = nullptr);

// The ps2r.ini keys the port reads itself (the game folder's folders, the mods' order, the PS Vita's tick lock) and the
// engine's schema may not know: the engine's strict parse refuses a whole file for one unknown key, so the lines of
// these keys are blanked in the returned text (the line numbers stay the file's) and the folders' values handed back.
// A key the engine's schema knows (a later engine may add them) stays in the text.
std::string TakePortKeys(const std::string& text, std::string* memcards, std::string* mods);

// Windows: the executable is a windowed (GUI-subsystem) program; started from a terminal it writes to that terminal.
// Output redirected to a file or a pipe is left alone. Elsewhere a no-op.
void AttachParentConsole();

// The reason the game cannot start: on stderr, and (Windows, no console) in a message box.
void ShowError(const char* title, const std::string& text);

}  // namespace te::startup
