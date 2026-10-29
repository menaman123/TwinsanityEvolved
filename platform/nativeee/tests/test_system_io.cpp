// test_system_io.cpp -- system.h (Initialise / StartServices construction order and idempotence, Language, LocalTime,
// kputs) and io.h (no-op restart, positive driver ids, the opaque IOP heap budget) and the stream boot façade over a
// generated ISO (synchronous reads into guest memory, FileSize, the buffer budget).
#include <cstdio>

#include "ps2e_seam.h"
#include "te_language.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
constexpr uint32_t kPath = 0x1000, kBuf = 0x100000;
}

int main() {
    using namespace te_test;
    const std::vector<uint8_t> bh = Pattern(70000, 11);
    const std::string iso = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"", "SLES_525.68", Pattern(100, 12)}});
    CHECK(!iso.empty());

    // --- system: the boot order
    te::Settings s = DefaultSettings();
    s.iso_path = iso;
    s.language = 4;   // German
    s.stream_instant = true;   // this test's stream rows assert the instant model (the drive model: test_stream)
    Reset(s);
    te::Services& services = te::Services::Get();
    CHECK(!services.Initialised());
    ps2e_system_initialise();
    CHECK(services.Initialised());
    CHECK(!services.Started());
    CHECK(services.disc().IsOpen());
    CHECK_EQ(ps2e_disc_wait_ready(), 2);
    ps2e_system_start_services();
    CHECK(services.Started());
    ps2e_system_initialise();      // idempotent
    ps2e_system_start_services();
    CHECK_EQ(ps2e_system_language(), 4);
    // The pads are up after StartServices: the setup frames then find a DualShock 2
    CHECK_EQ(ps2e_pads_open(0, 0, 0x5000), 1);
    ps2e_graphics_wait_v_sync();   // the boot façade's boundary (one field)
    CHECK_EQ(ps2e_pads_get_state(0, 0), 6);
    CHECK_EQ(services.clock().BoundaryCount(), 1u);
    ps2e_graphics_present(0);
    ps2e_graphics_present_from_interrupt(0);
    CHECK_EQ(services.clock().BoundaryCount(), 3u);
    ps2e_time_initialise();
    CHECK_EQ(ps2e_time_ticks(), 0u);
    ps2e_graphics_present(0);
    CHECK_EQ(ps2e_time_ticks(), 9600u);
    // Language out of range reads English
    {
        te::Settings bad = s; bad.language = 42;
        te::SetSettings(bad);
        CHECK_EQ(ps2e_system_language(), 1);
        te::SetSettings(s);
    }
    // kputs: a guest string to stderr (no crash on an unmapped address)
    WriteString(kPath, "[game] hello from kputs\n");
    ps2e_system_kputs(kPath);
    ps2e_system_kputs(0x7FFFFFF0u);

    // --- io
    ps2e_io_initialise();
    WriteString(kPath, "cdrom0:\\CRASH6\\SYS\\IOPRP300.IMG");
    ps2e_io_restart(kPath);
    WriteString(kPath, "cdrom0:\\CRASH6\\SYS\\STREAM.IRX");
    const int32_t id1 = ps2e_io_load_driver(kPath);
    const int32_t id2 = ps2e_io_load_driver(kPath);
    CHECK(id1 > 0 && id2 > id1);
    ps2e_io_initialise_heap();
    const uint32_t a = ps2e_io_allocate_heap(1000);
    const uint32_t b = ps2e_io_allocate_heap(0x10000);
    CHECK(a != 0 && b != 0 && b > a);
    CHECK_EQ(a & 15u, 0u);
    CHECK(a < 0x02000000u || a >= 0x00100000u);   // an IOP address, not a guest pointer the game could dereference
    CHECK_EQ(ps2e_io_allocate_heap(0), 0u);
    CHECK_EQ(ps2e_io_allocate_heap(0x7FFFFFFF), 0u);
    CHECK_EQ(ps2e_io_free_heap(b), 0);
    CHECK_EQ(ps2e_io_free_heap(b), -1);
    const uint32_t c = ps2e_io_allocate_heap(16);
    CHECK_EQ(c, b);   // the top block's bytes came back
    CHECK_EQ(ps2e_io_free_heap(a), 0);

    // --- stream boot façade
    CHECK_EQ(ps2e_stream_initialise(9, 0x8000), 0);
    CHECK_EQ(ps2e_stream_get_state(), 1);
    CHECK_EQ(ps2e_stream_update(), 0);
    const int32_t free0 = ps2e_stream_free_buffer_memory();
    ps2e_stream_attach_buffer(0, 0x20000, 0, 0);
    CHECK_EQ(ps2e_stream_free_buffer_memory(), free0 - 0x20000);
    ps2e_stream_detach_buffer(0);
    CHECK_EQ(ps2e_stream_free_buffer_memory(), free0);
    WriteString(kPath, "Crash6/crash.bh");
    const int32_t f = ps2e_stream_open_file(kPath);
    CHECK(f >= 0);
    CHECK_EQ(ps2e_stream_file_size(), 70000);
    WriteString(kPath, "Crash6/none.bh");
    const int32_t g = ps2e_stream_open_file(kPath);
    CHECK(g == f + 1);
    CHECK_EQ(ps2e_stream_file_size(), 0);
    // Reads complete before they return: the bytes are there, nothing is reading, Wait is 0
    ps2e_stream_read(2, f, 2048, 4096, kBuf);
    CHECK_EQ(ps2e_stream_is_reading(2), 0);
    CHECK_EQ(ps2e_stream_wait(2), 0);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 4096), bh.data() + 2048, 4096) == 0);
    ps2e_stream_read(2, f, 69000, 4096, kBuf);   // clipped at the end
    CHECK(std::memcmp(te::GuestPointer(kBuf, 1000), bh.data() + 69000, 1000) == 0);
    ps2e_stream_read(2, g, 0, 16, kBuf);          // no such file: nothing
    ps2e_stream_read(2, f, 0, 16, 0x7FFFFFF0u);   // bad destination: nothing, no crash
    ps2e_stream_read_sound_bank(3, 1, f, 0, 100); // dropped
    ps2e_stream_close_file(f);
    ps2e_stream_read(2, f, 0, 16, kBuf);          // closed: nothing

    // --- stream without a disc: Initialise fails
    {
        te::Settings none = DefaultSettings(); none.iso_path = "";
        Reset(none);
        CHECK_EQ(ps2e_stream_initialise(9, 0x8000), -1);
        CHECK_EQ(ps2e_stream_get_state(), 0);
    }
    te::Services::ResetForTest();
    std::remove(iso.c_str());
    // --- the language setting -- content recognition and the disc's banks
    {
        std::string probe;
        CHECK(te::language::RecognizeContent("off\r\non\r\nno\r\nyes\r\nback\r\ncontinue\r\nmono\r\nstereo\r\ndolby pro logic ii\r\n\r\nnew game\r\nload game\r\noptions\r\nlanguage\r\ngraphic options\r\n", &probe) == "english");
        CHECK(probe == "new game");
        CHECK(te::language::RecognizeContent("desativar\nativar\nn\xe3o\nsim\nvoltar\ncontinuar\nmono\nst\xe9" "reo\ndolby pro logic ii\n\nnovo jogo\ncarregar jogo\nop\xe7\xf5" "es\nidioma\n", &probe) == "portuguese");
        CHECK(probe == "novo jogo");
        CHECK(te::language::RecognizeContent("aus\nein\nnein\nja\nzur\xfc\x63k\nweiter\nmono\nstereo\ndolby pro logic ii\n\nneues spiel\nspiel laden\noptionen\nsprache\n", nullptr) == "german");
        CHECK(te::language::RecognizeContent("off\non\nno\nyes\n", nullptr) == "");   // too little to tell (English's and Spanish's first lines coincide)
        // A disc whose `English` slot carries Portuguese and whose `Spanish` slot carries the English text (the development image):
        // the speech banks Crash6\<slot>.mb are the retail ones (English.mb English, Spanish.mb Spanish; none for French here)
        auto bank = [](const std::string& name, const std::string& text, std::vector<uint8_t>& bh, std::vector<uint8_t>& bd) {
            const std::string path = "Language\\Code\\" + name + ".txt";
            auto put32 = [&bh](uint32_t v) { for (int i = 0; i < 4; ++i) bh.push_back(uint8_t(v >> (8 * i))); };
            put32(uint32_t(path.size()));
            bh.insert(bh.end(), path.begin(), path.end());
            put32(uint32_t(bd.size()));
            put32(uint32_t(text.size()));
            bd.insert(bd.end(), text.begin(), text.end());
            bd.resize((bd.size() + 2047) & ~size_t(2047));
        };
        std::vector<uint8_t> bh = {4, 0, 0, 0}, bd;
        bank("English", "desativar\nativar\nn\xe3o\nsim\nvoltar\ncontinuar\nmono\nst\xe9" "reo\ndolby\n\nnovo jogo\ncarregar jogo\nop\xe7\xf5" "es\nidioma\n", bh, bd);
        bank("French", "off\non\nnon\noui\nretour\ncontinuer\nmono\nst\xe9" "r\xe9" "o\ndolby\n\nnouvelle partie\ncharger\noptions\nlangue\n", bh, bd);
        bank("Spanish", "off\non\nno\nyes\nback\ncontinue\nmono\nstereo\ndolby\n\nnew game\nload game\noptions\nlanguage\ngraphic options\n", bh, bd);
        bank("Startup\\Fonts\\x", "not a bank", bh, bd);
        const std::string disc = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"CRASH6", "CRASH.BD", bd}, {"", "SLES_525.68", Pattern(100, 12)},
                                               {"CRASH6", "ENGLISH.MB", Pattern(4096, 3)}, {"CRASH6", "SPANISH.MB", Pattern(4096, 4)}});
        std::vector<te::language::Bank> banks;
        std::string error;
        CHECK(te::language::EnumerateFromDisc(disc, {}, banks, &error));
        CHECK(banks.size() == 3);   // the PAL slots the disc has Code banks for: English 0, French 1, Spanish 3
        if (banks.size() == 3) {
            CHECK(banks[0].slot == 0 && banks[0].name == "English" && banks[0].content == "portuguese" && banks[0].recognised && banks[0].code == 1);
            CHECK(banks[0].speech && banks[0].speech_language == "english");
            CHECK(banks[1].slot == 1 && banks[1].content == "french" && banks[1].code == 2 && !banks[1].speech);
            CHECK(banks[2].slot == 3 && banks[2].name == "Spanish" && banks[2].content == "english" && banks[2].code == 3 && banks[2].speech);
        }
        // the two lists -- TEXT languages: one per text content, english first, then slot order; SPEECH languages: one
        // per speech bank by the slot's name, english first, then slot order (French has no speech bank here)
        const std::vector<te::language::TextLanguage> texts = te::language::TextLanguagesOf(banks);
        CHECK(texts.size() == 3);
        if (texts.size() == 3) {
            CHECK(texts[0].name == "english" && texts[0].slot == 3 && texts[0].pack.empty());
            CHECK(texts[1].name == "portuguese" && texts[1].slot == 0);
            CHECK(texts[2].name == "french" && texts[2].slot == 1);
        }
        const std::vector<te::language::SpeechLanguage> speeches = te::language::SpeechLanguagesOf(banks);
        CHECK(speeches.size() == 2);
        if (speeches.size() == 2) {
            CHECK(speeches[0].name == "english" && speeches[0].slot == 0 && speeches[0].bank == "Crash6\\English.mb");
            CHECK(speeches[1].name == "spanish" && speeches[1].slot == 3 && speeches[1].bank == "Crash6\\Spanish.mb");
        }
        // the legacy one name (game.language / --language) through the single setting's pairing
        {
            std::string t, sp;
            te::language::LegacyPair(texts, speeches, "english", &t, &sp);
            CHECK(t == "english" && sp == "english");
            te::language::LegacyPair(texts, speeches, "portuguese", &t, &sp);   // text without speech: English's speech
            CHECK(t == "portuguese" && sp == "english");
            te::language::LegacyPair(texts, speeches, "spanish", &t, &sp);      // speech without text: the English text
            CHECK(t == "english" && sp == "spanish");
            te::language::LegacyPair(texts, speeches, "arabic+french", &t, &sp);   // the retired "<text>+<speech>" form
            CHECK(t == "arabic" && sp == "french");
            te::language::LegacyPair(texts, speeches, "italian", &t, &sp);      // neither: the names stand (the rows fall back)
            CHECK(t == "italian" && sp == "italian");
        }
        // A speech language on a disc with no English text: the legacy pairing reads its own slot's text
        {
            std::vector<te::language::Bank> two;
            te::language::Bank fr;
            fr.slot = 1; fr.name = "French"; fr.content = "french"; fr.recognised = true; fr.code = 2; fr.speech = true; fr.speech_language = "french";
            te::language::Bank es;
            es.slot = 3; es.name = "Spanish"; es.content = "german"; es.recognised = true; es.code = 3; es.speech = true; es.speech_language = "spanish";
            two.push_back(fr);
            two.push_back(es);
            const std::vector<te::language::TextLanguage> t2 = te::language::TextLanguagesOf(two);
            const std::vector<te::language::SpeechLanguage> s2 = te::language::SpeechLanguagesOf(two);
            CHECK(t2.size() == 2 && s2.size() == 2);
            std::string t, sp;
            te::language::LegacyPair(t2, s2, "spanish", &t, &sp);
            CHECK(t == "german" && sp == "spanish");
        }
        // The resolution through the settings: english -> text slot 3 (the Spanish slot's English text), Language() 1 (the
        // English slot: its speech bank, FMV track, voices, pictures); the rows are independent; a raw slot / code stands;
        // the Options menu's choices
        te::Settings lang = s;
        lang.iso_path = disc;
        lang.language = -1;
        lang.language_name = "english";
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_text_language_count(), 3);
        CHECK_EQ(ps2e_system_text_language_selected(), 0);
        CHECK_EQ(ps2e_system_speech_language_count(), 2);
        CHECK_EQ(ps2e_system_speech_language_selected(), 0);
        CHECK(te::language::TextLabel(1) == "portuguese");
        CHECK(te::language::SpeechLabel(1) == "spanish");
        CHECK(te::language::RowTitle(0) == "text language" && te::language::RowTitle(1) == "speech language");
        lang.language_name = "portuguese";   // legacy: portuguese text, English speech
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 0);
        CHECK(te::language::RowTitle(0) == "idioma do texto");   // the port's rows in the text's language
        lang.language_name = "italian";   // no such language on this disc: english, english
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        lang.language_name = "spanish";   // legacy: no Spanish TEXT on this disc, but Spanish.mb: the Spanish voice, English's text
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_text_language_selected(), 0);
        CHECK_EQ(ps2e_system_speech_language_selected(), 1);
        // the two rows apart: french text with the Spanish speech; the rows' own names win over the legacy one
        lang.language_name = "english";
        lang.text_language_name = "french";
        lang.speech_language_name = "spanish";
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 1);
        CHECK(te::language::RowTitle(1) == "langue des voix");
        lang.speech_language_name = "french";   // no French speech bank here: the speech row falls back to english
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 1);
        lang.text_language_name.clear();
        lang.speech_language_name.clear();
        lang.language_name.clear();
        lang.language = 3;   // a raw console code: the Spanish slot for everything, as a console set to Spanish ran retail
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_text_language_count(), 3);   // the raw slot is not a row
        CHECK_EQ(ps2e_system_text_language_selected(), 0);   // its text (english) and its speech (spanish) are the rows'
        CHECK_EQ(ps2e_system_speech_language_selected(), 1);
        lang.language = -1;
        lang.language_bank = 0;   // a raw slot (the pad logs' pin): slot 0 for everything
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 0);
        ps2e_system_set_text_language(2);   // the Options menu picked french text: the text bank follows, the speech stays slot 0's (english)
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 1);
        CHECK(te::GetSettings().text_language_name == "french" && te::GetSettings().speech_language_name == "english");
        CHECK(te::GetSettings().language_bank == -1);
        ps2e_system_set_speech_language(1);   // and the Spanish speech: Language() follows, the text stays french
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 1);
        CHECK(te::GetSettings().speech_language_name == "spanish");
        // the choice persisted: ps2r.ini names both rows (game.text_language, game.speech_language), the legacy line goes, every
        // other line stays
        {
            const std::string ini = disc + ".ps2r.ini";
            {
                std::FILE* f = std::fopen(ini.c_str(), "wb");
                CHECK(f != nullptr);
                if (f) {
                    std::fputs("# the player's file\ngame.language = arabic+french\nvideo.scale = 2\n", f);
                    std::fclose(f);
                }
            }
            te::Settings with_file = te::GetSettings();
            with_file.settings_path = ini;
            te::SetSettings(with_file);
            ps2e_system_set_text_language(0);   // english text, the speech stays spanish
            std::string text;
            if (std::FILE* f = std::fopen(ini.c_str(), "rb")) {
                char buf[512];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                std::fclose(f);
            }
            CHECK(text.find("game.text_language = english\n") != std::string::npos);
            CHECK(text.find("game.speech_language = spanish\n") != std::string::npos);
            CHECK(text.find("game.language") == std::string::npos);
            CHECK(text.find("# the player's file\n") != std::string::npos && text.find("video.scale = 2\n") != std::string::npos);
            std::remove(ini.c_str());
        }
        te::language::Reset();
        te::SetSettings(s);
    }

    return Finish("test_system_io");
}
