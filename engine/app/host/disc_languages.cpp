// app/host/disc_languages.cpp -- see disc_languages.h.
#include "app/host/disc_languages.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "app/host/language_probes.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/core/hash/sha256.h"

namespace ps2r::app {

namespace {

namespace probes = language_probes;

struct IndexEntry {
    std::string name;
    uint32_t offset = 0, size = 0;
};

// CRASH6/CRASH.BH: one 32-bit word, then {u32 name length, name, u32 offset, u32 size} to the end
std::vector<IndexEntry> ReadIndex(const IsoFs& iso, IsoFs::Entry* bd, std::string* error) {
    std::vector<IndexEntry> out;
    IsoFs::Entry bh;
    if (!iso.Find("CRASH6/CRASH.BH", bh) || !iso.Find("CRASH6/CRASH.BD", *bd) || bh.size < 8 || bh.size > (1u << 20)) {
        if (error) *error = "CRASH6/CRASH.BH or CRASH.BD is not on the disc";
        return out;
    }
    std::vector<uint8_t> index(bh.size);
    if (iso.ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, index.data()) != bh.size) {
        if (error) *error = "CRASH.BH did not read whole";
        return out;
    }
    size_t at = 4;
    while (at + 4 <= index.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &index[at], 4);
        at += 4;
        if (len > 512 || at + len + 8 > index.size()) break;
        IndexEntry e;
        e.name.assign(reinterpret_cast<const char*>(&index[at]), len);
        at += len;
        std::memcpy(&e.offset, &index[at], 4);
        std::memcpy(&e.size, &index[at + 4], 4);
        at += 8;
        out.push_back(e);
    }
    return out;
}

std::string ReadEntry(const IsoFs& iso, const IsoFs::Entry& bd, const IndexEntry& e, size_t at_most) {
    const size_t want = std::min<size_t>(at_most, e.size);
    std::string text(want, '\0');
    const size_t got = want ? iso.ReadBytes(uint64_t(bd.lba) * IsoFs::kSectorSize + e.offset, want, reinterpret_cast<uint8_t*>(&text[0])) : 0;
    text.resize(got);
    return text;
}

// The sha256 a bank section names for an English file: its non-blank lines joined by '\n' (te_language_mod.h EnglishKey)
std::string EnglishKey(const std::string& file) {
    std::string joined, cur;
    bool any = false;
    auto flush = [&] {
        if (cur.empty()) return;
        if (any) joined.push_back('\n');
        joined += cur;
        any = true;
        cur.clear();
    };
    for (char c : file) {
        if (c == '\r' || c == '\n') flush();
        else cur.push_back(c);
    }
    flush();
    return ps2r::hash::Sha256::HexOf(joined.data(), joined.size());
}

template <typename T> void EnglishFirst(std::vector<T>& v) {
    std::stable_partition(v.begin(), v.end(), [](const std::string& s) { return s == "english"; });
}

}  // namespace

bool ReadDiscLanguages(const IsoFs& iso, DiscLanguages* out) {
    *out = DiscLanguages{};
    IsoFs::Entry bd;
    const std::vector<IndexEntry> index = ReadIndex(iso, &bd, &out->error);
    if (index.empty()) {
        if (out->error.empty()) out->error = "the archive index has no entries";
        return false;
    }
    // the slots the archive has a Code bank for: the PAL order first, then any other (American)
    const std::string prefix = "Language\\Code\\";
    std::vector<std::string> slots;
    std::vector<std::string> others;
    for (const IndexEntry& e : index) {
        if (e.name.size() <= prefix.size() + 4 || e.name.compare(0, prefix.size(), prefix) != 0 || e.name.compare(e.name.size() - 4, 4, ".txt") != 0)
            continue;
        const std::string slot = e.name.substr(prefix.size(), e.name.size() - prefix.size() - 4);
        const auto& pal = probes::PalSlots();
        if (std::none_of(pal.begin(), pal.end(), [&](const std::string& p) { return probes::Lower(p) == probes::Lower(slot); })) others.push_back(slot);
    }
    for (const std::string& p : probes::PalSlots()) slots.push_back(p);
    slots.insert(slots.end(), others.begin(), others.end());
    std::string english_slot;
    for (const std::string& slot : slots) {
        const auto code = std::find_if(index.begin(), index.end(), [&](const IndexEntry& e) { return probes::Lower(e.name) == probes::Lower(prefix + slot + ".txt"); });
        if (code == index.end()) continue;
        std::string content = probes::Recognize(ReadEntry(iso, bd, *code, probes::kProbeBytes), nullptr);
        if (content.empty()) content = probes::Lower(slot);
        if (std::find(out->text.begin(), out->text.end(), content) == out->text.end()) out->text.push_back(content);
        if (content == "english" && english_slot.empty()) english_slot = code->name.substr(prefix.size(), code->name.size() - prefix.size() - 4);
        IsoFs::Entry mb;
        std::string upper = slot;
        for (char& c : upper) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (iso.Find("CRASH6/" + upper + ".MB", mb) && !mb.directory && mb.size > 0) {
            const std::string speech = probes::LanguageOfSlotName(slot);
            if (std::find(out->speech.begin(), out->speech.end(), speech) == out->speech.end()) out->speech.push_back(speech);
        }
    }
    EnglishFirst(out->text);
    EnglishFirst(out->speech);
    // the English text's keys (the whole two files: a few tens of kilobytes)
    if (!english_slot.empty()) {
        for (int f = 0; f < 2; ++f) {
            const std::string name = std::string("Language\\") + (f == 0 ? "Code" : "AgentLab") + "\\" + english_slot + ".txt";
            const auto it = std::find_if(index.begin(), index.end(), [&](const IndexEntry& e) { return probes::Lower(e.name) == probes::Lower(name); });
            if (it == index.end()) continue;
            (f == 0 ? out->english_code_key : out->english_agentlab_key) = EnglishKey(ReadEntry(iso, bd, *it, it->size));
        }
    }
    out->ok = !out->text.empty();
    if (!out->ok) out->error = "no Language\\Code\\<slot>.txt bank in the archive";
    return out->ok;
}

bool ReadDiscLanguages(const std::string& iso_path, DiscLanguages* out) {
    IsoFs iso;
    std::string error;
    if (!iso.Open(iso_path, &error)) {
        *out = DiscLanguages{};
        out->error = error;
        return false;
    }
    return ReadDiscLanguages(iso, out);
}

void BankKeys(const std::string& bank, std::string* code_key, std::string* agentlab_key) {
    code_key->clear();
    agentlab_key->clear();
    size_t at = 0;
    while (at < bank.size()) {
        size_t end = bank.find_first_of("\r\n", at);
        if (end == std::string::npos) end = bank.size();
        const std::string line = bank.substr(at, end - at);
        at = end + 1;
        if (line.rfind("@@ ", 0) != 0) continue;
        // "@@ <file> <texts> <sha256>"
        const size_t a = line.find(' ', 3), b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (b == std::string::npos) continue;
        const std::string file = line.substr(3, a - 3), sha = line.substr(b + 1);
        if (sha.size() != 64) continue;
        if (probes::Lower(file) == "code") *code_key = sha;
        if (probes::Lower(file) == "agentlab") *agentlab_key = sha;
    }
}

bool PackFits(const DiscLanguages& disc, const std::string& code_key, const std::string& agentlab_key) {
    return !disc.english_code_key.empty() && !disc.english_agentlab_key.empty() && code_key == disc.english_code_key &&
           agentlab_key == disc.english_agentlab_key;
}

void LegacyPair(const std::vector<std::string>& text, const std::vector<std::string>& speech, const std::string& name_in,
                std::string* text_name, std::string* speech_name) {
    const std::string name = probes::Lower(name_in);
    const size_t plus = name.find('+');
    if (plus != std::string::npos) {
        *text_name = name.substr(0, plus);
        *speech_name = name.substr(plus + 1);
        return;
    }
    *text_name = *speech_name = name;
    const bool is_text = std::find(text.begin(), text.end(), name) != text.end();
    const bool is_speech = std::find(speech.begin(), speech.end(), name) != speech.end();
    const bool english_text = std::find(text.begin(), text.end(), "english") != text.end();
    const bool english_speech = std::find(speech.begin(), speech.end(), "english") != speech.end();
    if (!is_text && is_speech && english_text) *text_name = "english";
    if (is_text && !is_speech && english_speech) *speech_name = "english";
}

}  // namespace ps2r::app
