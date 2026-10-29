// test_disc_languages.cpp -- engine/app/host/disc_languages (the launcher's Settings page: the text and speech languages it
// offers for a disc) against the game's own lists (te_language.h TextLanguagesOf / SpeechLanguagesOf over the same disc):
// the two sides offer the same languages in the same order, the English text's keys a language pack must name, the
// pack check and the legacy pairing. A generated disc always (the development image's layout: Portuguese text in the `English` slot, the
// English text in the `Spanish` slot, speech banks named for their slots; an American-only disc); the TE_TEST_ISO disc (and the
// North American one) when TE_TEST_ISO / TE_TEST_NTSC_ISO name them.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/host/disc_languages.h"
#include "engine/core/hash/sha256.h"
#include "te_language.h"
#include "te_language_mod.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {

void Bank(const std::string& file, const std::string& slot, const std::string& text, std::vector<uint8_t>& bh, std::vector<uint8_t>& bd) {
    const std::string path = "Language\\" + file + "\\" + slot + ".txt";
    auto put32 = [&bh](uint32_t v) { for (int i = 0; i < 4; ++i) bh.push_back(uint8_t(v >> (8 * i))); };
    put32(uint32_t(path.size()));
    bh.insert(bh.end(), path.begin(), path.end());
    put32(uint32_t(bd.size()));
    put32(uint32_t(text.size()));
    bd.insert(bd.end(), text.begin(), text.end());
    bd.resize((bd.size() + 2047) & ~size_t(2047));
}

std::vector<std::string> Names(const std::vector<te::language::TextLanguage>& v) {
    std::vector<std::string> out;
    for (const auto& l : v) out.push_back(l.name);
    return out;
}
std::vector<std::string> Names(const std::vector<te::language::SpeechLanguage>& v) {
    std::vector<std::string> out;
    for (const auto& l : v) out.push_back(l.name);
    return out;
}

// The game's lists and the launcher's for one disc image: equal (the game's slot names are the PAL ones unless `slots`)
void SameAsGame(const std::string& iso, const std::vector<std::string>& slots, const char* what) {
    std::vector<te::language::Bank> banks;
    std::string error;
    CHECK(te::language::EnumerateFromDisc(iso, slots, banks, &error));
    ps2r::app::DiscLanguages dl;
    CHECK(ps2r::app::ReadDiscLanguages(iso, &dl));
    const std::vector<std::string> gt = Names(te::language::TextLanguagesOf(banks)), gs = Names(te::language::SpeechLanguagesOf(banks));
    CHECK(dl.text == gt);
    CHECK(dl.speech == gs);
    std::string t, s;
    for (const std::string& x : dl.text) t += x + " ";
    for (const std::string& x : dl.speech) s += x + " ";
    std::fprintf(stderr, "test_disc_languages: %s: text [ %s] speech [ %s] english keys %s / %s\n", what, t.c_str(), s.c_str(),
                 dl.english_code_key.substr(0, 12).c_str(), dl.english_agentlab_key.substr(0, 12).c_str());
}

}  // namespace

int main() {
    using namespace te_test;
    // --- the development image's layout, generated
    const std::string pt = "desativar\nativar\nn\xe3o\nsim\nvoltar\ncontinuar\nmono\nst\xe9" "reo\ndolby\n\nnovo jogo\ncarregar jogo\nop\xe7\xf5" "es\nidioma\n";
    const std::string fr = "off\non\nnon\noui\nretour\ncontinuer\nmono\nst\xe9" "r\xe9" "o\ndolby\n\nnouvelle partie\ncharger\noptions\nlangue\n";
    const std::string en = "off\r\non\r\nno\r\nyes\r\nback\r\ncontinue\r\nmono\r\nstereo\r\ndolby\r\n\r\nnew game\r\nload game\r\noptions\r\nlanguage\r\ngraphic options\r\n";
    const std::string en_agent = "thanks for playing\r\n\r\nzzz\r\nyou gotta help me crash\r\n";
    std::vector<uint8_t> bh = {4, 0, 0, 0}, bd;
    Bank("Code", "English", pt, bh, bd);
    Bank("Code", "French", fr, bh, bd);
    Bank("Code", "Spanish", en, bh, bd);
    Bank("AgentLab", "Spanish", en_agent, bh, bd);
    const std::string disc = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"CRASH6", "CRASH.BD", bd}, {"", "SLES_525.68", Pattern(100, 12)},
                                           {"CRASH6", "ENGLISH.MB", Pattern(4096, 3)}, {"CRASH6", "SPANISH.MB", Pattern(4096, 4)}});
    CHECK(!disc.empty());
    ps2r::app::DiscLanguages dl;
    CHECK(ps2r::app::ReadDiscLanguages(disc, &dl) && dl.ok);
    CHECK((dl.text == std::vector<std::string>{"english", "portuguese", "french"}));
    CHECK((dl.speech == std::vector<std::string>{"english", "spanish"}));
    SameAsGame(disc, {}, "generated PAL layout");
    // the English text's keys: the sha256 a language pack's bank names (the game's own EnglishKey)
    CHECK(dl.english_code_key == te::language::mod::EnglishKey(en));
    CHECK(dl.english_agentlab_key == te::language::mod::EnglishKey(en_agent));
    // a pack whose bank names them fits; another disc's does not
    {
        const std::string bank = "@@ Code 3 " + dl.english_code_key + "\r\na\r\nb\r\nc\r\n@@ AgentLab 1 " + dl.english_agentlab_key + "\r\nd\r\n"
                                 "@@ Port 2 " + std::string(64, '0') + "\r\ne\r\nf\r\n";
        std::string code, agent;
        ps2r::app::BankKeys(bank, &code, &agent);
        CHECK(code == dl.english_code_key && agent == dl.english_agentlab_key);
        CHECK(ps2r::app::PackFits(dl, code, agent));
        CHECK(!ps2r::app::PackFits(dl, std::string(64, 'a'), agent));
        ps2r::app::BankKeys("not a bank", &code, &agent);
        CHECK(code.empty() && agent.empty());
    }
    // the legacy pairing, by names
    {
        std::string t, s;
        ps2r::app::LegacyPair(dl.text, dl.speech, "spanish", &t, &s);
        CHECK(t == "english" && s == "spanish");
        ps2r::app::LegacyPair(dl.text, dl.speech, "Portuguese", &t, &s);
        CHECK(t == "portuguese" && s == "english");
        ps2r::app::LegacyPair(dl.text, dl.speech, "arabic+french", &t, &s);
        CHECK(t == "arabic" && s == "french");
    }
    std::remove(disc.c_str());

    // --- an American-only disc (the North American layout): english text and speech, the slot named American
    {
        std::vector<uint8_t> bh2 = {4, 0, 0, 0}, bd2;
        Bank("Code", "American", en, bh2, bd2);
        Bank("AgentLab", "American", en_agent, bh2, bd2);
        const std::string us = WriteTestIso({{"CRASH6", "CRASH.BH", bh2}, {"CRASH6", "CRASH.BD", bd2}, {"", "SLUS_209.09", Pattern(100, 12)},
                                             {"CRASH6", "AMERICAN.MB", Pattern(4096, 3)}});
        ps2r::app::DiscLanguages us_dl;
        CHECK(ps2r::app::ReadDiscLanguages(us, &us_dl));
        CHECK((us_dl.text == std::vector<std::string>{"english"}) && (us_dl.speech == std::vector<std::string>{"english"}));
        CHECK(!us_dl.english_code_key.empty());
        SameAsGame(us, {"American"}, "generated American layout");
        std::remove(us.c_str());
    }
    // --- no archive: nothing offered, said why
    {
        const std::string none = WriteTestIso({{"", "SLES_525.68", Pattern(100, 12)}});
        ps2r::app::DiscLanguages n;
        CHECK(!ps2r::app::ReadDiscLanguages(none, &n) && !n.ok && !n.error.empty());
        std::remove(none.c_str());
    }

    // --- the real discs, when named
    if (const char* iso = std::getenv("TE_TEST_ISO")) {
        SameAsGame(iso, {}, "TE_TEST_ISO");
        ps2r::app::DiscLanguages real;
        CHECK(ps2r::app::ReadDiscLanguages(iso, &real));
        // the repository's Arabic pack was made from this disc's English (mods/arabic/data/arabic.bank)
        if (const char* src = std::getenv("TE_SOURCE_DIR")) {
            const std::string path = std::string(src) + "/mods/arabic/data/arabic.bank";
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                std::string bank;
                char buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bank.append(buf, n);
                std::fclose(f);
                std::string code, agent;
                ps2r::app::BankKeys(bank, &code, &agent);
                std::fprintf(stderr, "test_disc_languages: TE_TEST_ISO: the Arabic pack %s this disc\n", ps2r::app::PackFits(real, code, agent) ? "fits" : "does not fit");
            }
        }
    }
    if (const char* iso = std::getenv("TE_TEST_NTSC_ISO")) SameAsGame(iso, {"American"}, "TE_TEST_NTSC_ISO");
    return Finish("test_disc_languages");
}
