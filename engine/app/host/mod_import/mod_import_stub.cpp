// app/host/mod_import/mod_import_stub.cpp -- the import library's stand-in: compiled into the
// launcher instead of the library while the library's sources are not in the tree (the launcher's CMake option
// PS2R_LAUNCHER_IMPORT: auto takes the library when it is there, stub forces this file). Every artefact is refused with one
// sentence a player reads; nothing is written.
#include "app/host/mod_import/mod_import.h"

namespace te::modimport {

namespace {
const char kNotAvailable[] = "Importing is not available in this version of the launcher.";
}  // namespace

Kind Detect(const std::string&, std::string* why) {
    if (why) *why = kNotAvailable;
    return Kind::Unknown;
}

std::string ModIdFor(const std::string&, std::string* name) {
    if (name) name->clear();
    return "";
}

bool Import(const std::string&, const std::string&, const std::string&, const Progress& progress, Report* report, std::string* error) {
    if (progress.report) progress.report("checking", 1.0);
    if (report) *report = Report{};
    if (error) *error = kNotAvailable;
    return false;
}

}  // namespace te::modimport
