// core/config/mods_config.h -- the built-in mods' settings (TwinsanityEvolved):
// `mods.<id>.enabled`, one key per mod id (lower case letters, digits, underscores),
// a DYNAMIC key family -- Config::IsKey / Get / Set accept any id, Keys() lists none
// (the schema table stays static), the fingerprint carries every ENABLED mod (a
// modded run is another execution). All OFF by default; the title's in-game Mods
// page persists them through SettingsFile. Held by Config (config.h) as `mods`.
#pragma once

#include <map>
#include <string>

namespace ps2r {

struct ModsConfig {
    std::map<std::string, bool> enabled;
    // `mods.order`: the folder mods' overlay order (comma-separated ids, later wins; "" = sorted by id). A title-side key
    // like the family above: IsKey / Get / Set accept it, Keys() does not list it.
    std::string order;
    // true iff the mod `id` is on.
    bool Enabled(const std::string& id) const;
    // true iff any mod is on.
    bool Any() const;
    // "mods.<id>.enabled" -> id (true), else false.
    static bool ParseKey(const std::string& key, std::string* id);
};

}  // namespace ps2r
