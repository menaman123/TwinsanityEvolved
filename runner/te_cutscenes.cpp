// te_cutscenes.cpp -- the in-game cutscenes' diagnostics (te_cutscenes.h): the census of the behaviour scripts the game read
// and the state lines a watched and a skipped run are compared by. Read-only over guest memory through the retail globals of
// the PAL image (game/symbol_addrs.txt) and the layouts of game/include/game/agentlab.h, behaviours.h, objects.h,
// chunkloading.h (their CHECK_OFFSETs); nothing here is reachable from the game. The commands carry no ID of their own once
// read (game/src/game/agentlab.cpp ReadCommand: the builder's object keeps NoId): a command is named by its vtable's retail
// address, which tools/cutscenes/census.py maps to the factory's IDs (game/src/game/commands.cpp BuildScriptCommand).
#include "te_cutscenes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "te_harness.h"
#include "te_host.h"
#include "te_services.h"

#if defined(TE_NATIVE) && defined(__GNUC__)
#pragma GCC diagnostic ignored "-Warray-bounds"   // as te_probe.cpp: every read below is behind InRange
#endif

namespace te::cutscenes {

namespace {
// Retail globals (PAL SLES-52568, game/symbol_addrs.txt)
constexpr uint32_t kScriptTable = 0x0030A0C0;     // G_ScriptTable: ResourceTable*
constexpr uint32_t kChunkManager = 0x0030A150;    // G_ChunkManager_: ChunkManager*
constexpr uint32_t kGraphVTable = 0x002FCE38;     // MajorScript_methods (ScriptGraph)
constexpr uint32_t kStarterVTable = 0x002FCE80;   // HeaderScript_methods (ScriptStarter)
constexpr uint32_t kQueueObjectVideoVTable = 0x002FF318;   // vt_Cmd180 (QueueObjectVideoCommand: the cutscene file's number at +0xC)

bool InRange(uint32_t address, uint32_t size) { return address != 0 && GuestPointer(address, size) != nullptr; }
uint32_t Rd32(uint32_t address) {
    if (!InRange(address, 4)) return 0;
    uint32_t v;
    std::memcpy(&v, GuestPointer(address, 4), 4);
    return v;
}
uint16_t Rd16(uint32_t address) {
    if (!InRange(address, 2)) return 0;
    uint16_t v;
    std::memcpy(&v, GuestPointer(address, 2), 2);
    return v;
}
uint8_t Rd8(uint32_t address) { return InRange(address, 1) ? *GuestPointer(address, 1) : 0; }
float RdF(uint32_t address) {
    const uint32_t w = Rd32(address);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
// game/string.h String { char* string; s32 length; s32 capacity; }, JSON-escaped (names only)
std::string RdString(uint32_t string) {
    const uint32_t text = Rd32(string), length = Rd32(string + 4);
    if (!text || length == 0 || length > 200 || !InRange(text, length)) return "";
    std::string out;
    const char* p = reinterpret_cast<const char*>(GuestPointer(text, length));
    for (uint32_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20 || c > 0x7E) out += '?';
        else out += char(c);
    }
    return out;
}
std::string Num(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", f);
    return b;
}

// A control packet's value (ControlPacket::SlotOf + Floats): the float of its slot, or nothing when the slot is none or an
// instance property's
bool PacketValue(uint32_t packet, uint32_t index, float* out) {
    const uint32_t counts = Rd32(packet), data = Rd32(packet + 8);
    const uint32_t slots = counts & 0xFF, floats = (counts >> 8) & 0xFF;
    if (index >= slots || !data) return false;
    const uint8_t slot = Rd8(data + floats * 4 + index);
    if (slot == 0xFF || (slot & 0x80) != 0) return false;
    *out = RdF(data + (slot & 0x7F) * 4u);
    return true;
}

// The states of a graph in their read order (a body's jump is named by its target's index there)
std::vector<uint32_t> States(uint32_t graph_data) {
    std::vector<uint32_t> states;
    for (uint32_t s = Rd32(graph_data + 0x14); InRange(s, 0x10) && states.size() < 512; s = Rd32(s + 0xC)) {
        states.push_back(s);
        if ((Rd32(s) & (1u << 15)) == 0) break;   // GraphStateBits::hasNext
    }
    return states;
}
int IndexOf(const std::vector<uint32_t>& states, uint32_t state) {
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i] == state) return int(i);
    return -1;
}

// One graph: {"g":id,"name":..,"start":i,"s":[{"f":bits,"child":id,"pk":{..},"b":[{"c":cond,"inv":..,"p":..,"t":..,"j":i,
// "x":[vtables],"cs":seconds?}]}]}
std::string GraphJson(uint32_t graph_data) {
    const std::vector<uint32_t> states = States(graph_data);
    std::string j = "{\"g\":" + std::to_string(Rd32(graph_data) & 0xFFFF) + ",\"name\":\"" + RdString(graph_data + 8) +
                    "\",\"start\":" + std::to_string(IndexOf(states, Rd32(graph_data + 4))) + ",\"s\":[";
    for (size_t si = 0; si < states.size(); ++si) {
        const uint32_t s = states[si], bits = Rd32(s);
        if (si) j += ",";
        j += "{\"f\":" + std::to_string(bits & 0xFFFF) + ",\"child\":" + std::to_string(bits >> 16);
        const uint32_t packet = Rd32(s + 4);
        if (InRange(packet, 0xC)) {
            const uint32_t settings = Rd32(packet + 4);
            j += ",\"pk\":{\"m\":" + std::to_string((settings >> 3) & 0xF) + ",\"sp\":" + std::to_string(settings & 7) +
                 ",\"tr\":" + std::to_string((settings >> 13) & 1) + ",\"ro\":" + std::to_string((settings >> 14) & 1) +
                 ",\"st\":" + std::to_string(settings >> 31);
            j += ",\"set\":" + std::to_string(settings);
            // which values are given, and how: a float of the packet ('f') or an instance property ('p'), by the tool's index
            {
                const uint32_t counts = Rd32(packet), data = Rd32(packet + 8);
                const uint32_t slots = counts & 0xFF, floats = (counts >> 8) & 0xFF;
                std::string given;
                for (uint32_t k = 0; k < slots && k < 32 && data; ++k) {
                    const uint8_t slot = Rd8(data + floats * 4 + k);
                    if (slot != 0xFF) given += std::to_string(k) + ((slot & 0x80) ? "p" : "f") + " ";
                }
                j += ",\"given\":\"" + given + "\"";
            }
            float v;
            if (PacketValue(packet, 10, &v)) j += ",\"delay\":" + Num(v);
            if (PacketValue(packet, 11, &v)) j += ",\"dur\":" + Num(v);
            if (PacketValue(packet, 2, &v)) j += ",\"speed\":" + Num(v);
            j += "}";
        }
        j += ",\"b\":[";
        unsigned bi = 0;
        for (uint32_t b = Rd32(s + 8); InRange(b, 0x14) && bi < 64; b = Rd32(b + 0x10), ++bi) {
            const uint32_t bb = Rd32(b);
            if (bi) j += ",";
            j += "{";
            const uint32_t cond = Rd32(b + 4);
            if ((bb & (1u << 9)) && InRange(cond, 0x14)) {
                const uint32_t cw = Rd32(cond);
                j += "\"c\":" + std::to_string(cw & 0xFFFF) + ",\"inv\":" + std::to_string((cw >> 16) & 1) + ",\"p\":" +
                     std::to_string(cw >> 17) + ",\"t\":" + Num(RdF(cond + 8)) + ",\"w\":" + Num(RdF(cond + 0xC)) + ",";
            }
            j += "\"j\":" + std::to_string((bb & (1u << 10)) ? IndexOf(states, Rd32(b + 8)) : -1) + ",\"r\":" +
                 std::to_string((bb >> 8) & 1) + ",\"x\":[";
            unsigned ci = 0;
            std::string videos, args;
            for (uint32_t c = Rd32(b + 0xC); InRange(c, 0xC) && ci < 255; ++ci) {
                if (ci) j += ",";
                const uint32_t vt = Rd32(c + 8) - RetailAddress(0);   // the vtable's retail address
                j += std::to_string(vt);
                if (vt == kQueueObjectVideoVTable) videos += (videos.empty() ? "" : ",") + std::to_string(int32_t(Rd32(c + 0xC)));
                // the command's first two words after the base's (SendUserMessage: its target -- the message and the recipient --
                // and its flags -- the linked object's id and how linked objects are picked; numbers only)
                args += std::string(ci ? "," : "") + "[" + std::to_string(Rd32(c + 0xC)) + "," + std::to_string(Rd32(c + 0x10)) + "]";
                const uint32_t next = Rd32(c + 4);
                if ((Rd32(c) & (1u << 24)) == 0) break;   // ScriptCommandBits::hasNext
                c = next;
            }
            j += "],\"xa\":[" + args + "]";
            if (!videos.empty()) j += ",\"v\":[" + videos + "]";
            j += "}";
            if ((bb & (1u << 11)) == 0) break;   // StateBodyBits::hasNext
        }
        j += "]}";
        if (si + 1 < states.size() && (bits & (1u << 15)) == 0) break;
    }
    return j + "]}";
}

// One starter: its priority and its assigners (who, the argument, the graph's id)
std::string StarterJson(uint32_t starter) {
    std::string j = "{\"st\":" + std::to_string(Rd32(starter + 8) & 0xFFFF) + ",\"prio\":" + std::to_string((Rd32(starter + 8) >> 16) & 0xFF) +
                    ",\"a\":[";
    const unsigned count = Rd8(starter + 0x1C);
    for (unsigned a = 0; a < count && a < 7; ++a) {
        const uint32_t assigner = Rd32(starter + 0x20 + 4 * a);
        const uint32_t convention = Rd32(assigner), graph = Rd32(assigner + 4);
        const uint32_t cw = Rd32(convention);
        if (a) j += ",";
        j += "[" + std::to_string(cw & 0xF) + "," + std::to_string(cw >> 16) + "," +
             std::to_string(InRange(graph, 0x18) ? int(Rd32(graph) & 0xFFFF) : -1) + "]";
    }
    return j + "]}";
}

std::set<std::pair<uint32_t, uint32_t>> g_printed;   // (resource address, id) already logged this run
uint32_t g_signature = 0;
uint64_t g_settled_at = 0;
bool g_dirty = false;

uint32_t TableSignature(uint32_t* count) {
    const uint32_t table = Rd32(RetailAddress(kScriptTable));
    const uint32_t capacity = Rd32(table) & 0x3FFF, items = Rd32(table + 4);
    uint32_t h = 2166136261u, n = 0;
    if (!InRange(items, capacity * 4)) return 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t item = Rd32(items + 4 * i);
        if (!item) continue;
        h = (h ^ item ^ (i << 18)) * 16777619u;
        ++n;
    }
    *count = n;
    return h;
}

// The loaded chunks: each one's path and its object instances (the object's id and name, its behaviour starters by slot and
// its trigger behaviours' starters) -- which chunk's instances run which starters
void LogChunks(uint64_t boundary) {
    const uint32_t manager = Rd32(RetailAddress(kChunkManager));
    if (!InRange(manager, 0x1FC0)) return;
    const unsigned count = Rd16(manager);
    for (unsigned e = 0; e < count && e < 0x200; ++e) {
        const uint32_t entry = Rd32(manager + 4 + 4 * e);
        if (!InRange(entry, 0x38)) continue;
        const uint32_t data_ref = Rd32(entry + 0x14), chunk = InRange(data_ref, 8) ? Rd32(data_ref) : 0;
        std::string j = "{\"b\":" + std::to_string(boundary) + ",\"chunk\":\"" + RdString(entry) + "\",\"index\":" +
                        std::to_string(Rd16(entry + 0xC)) + ",\"loaded\":" + std::to_string(InRange(chunk, 0x1F0) ? 1 : 0) + ",\"o\":[";
        // The objects the chunk's RM2 brought (ChunkEntry::objects: a count, then the IDs) -- the object table's entries
        const uint32_t objects = Rd32(entry + 0x18);
        const uint32_t n = InRange(objects, 4) ? Rd32(objects) : 0;
        for (uint32_t k = 0; k < n && k < 4096; ++k) {
            if (k) j += ",";
            j += std::to_string(Rd32(objects + 4 + 4 * k));
        }
        j += "],\"i\":[";
        if (InRange(chunk, 0x1F0)) {
            const uint32_t insts = Rd32(chunk + 0x15C);   // ChunkData::instances -> ChunkInstances::nodes[NodeObject] (0x08 + 4)
            unsigned ni = 0;
            for (uint32_t node = InRange(insts, 0x68) ? Rd32(insts + 0x08 + 4 * 1) : 0; InRange(node, 0x80) && ni < 4096;
                 node = Rd32(node + 0x10), ++ni) {
                const uint32_t owner = Rd32(node), object = Rd32(node + 0x78);
                if (ni) j += ",";
                j += "{\"id\":" + std::to_string(InRange(owner, 0x158) ? Rd16(owner + 0x154) : 0xFFFF) + ",\"ob\":" +
                     std::to_string(InRange(object, 0x60) ? Rd32(object + 4) : 0xFFFFFFFFu) + ",\"n\":\"" +
                     (InRange(object, 0x60) ? RdString(object + 0x14) : std::string()) + "\",\"beh\":[";
                if (InRange(object, 0x60)) {
                    const uint32_t beh = Rd32(object + 0x48), nb = Rd32(object + 0x4C);
                    for (uint32_t k = 0; k < nb && k < 64; ++k) {
                        if (k) j += ",";
                        j += std::to_string(Rd16(beh + 2 * k));
                    }
                    j += "],\"trig\":[";
                    const uint32_t trig = Rd32(object + 0x30), nt = Rd32(object + 0x34);
                    for (uint32_t k = 0; k < nt && k < 64; ++k) {
                        const uint32_t w = Rd32(trig + 4 * k);
                        if (k) j += ",";
                        j += "[" + std::to_string(w & 0x3FF) + "," + std::to_string((w >> 10) & 0x3FFF) + "]";
                    }
                } else {
                    j += "],\"trig\":[";
                }
                j += "]}";
            }
            // the message triggers (ChunkInstances::nodes[NodeMessageTrigger]: MessageTriggerNode): where each is, the messages it
            // sends what enters / stays / leaves it, and the instances it tells (TriggerNode::instances)
            j += "],\"t\":[";
            unsigned nt = 0;
            for (uint32_t node = InRange(insts, 0x68) ? Rd32(insts + 0x08 + 4 * 7) : 0; InRange(node, 0x1A0) && nt < 1024;
                 node = Rd32(node + 0x10), ++nt) {
                const uint32_t owner = Rd32(node);
                const uint32_t place = InRange(owner, 0x160) ? Rd32(owner + 0x08) : 0;
                float xyz[3] = {0, 0, 0};
                if (InRange(place, 0x70)) {
                    const uint32_t at = (Rd32(place + 0x60) & 0x4) ? place + 0x30 : place + 0x40;
                    for (int k = 0; k < 3; ++k) xyz[k] = RdF(at + 4u * k);
                }
                char p[96];
                std::snprintf(p, sizeof p, "[%.2f,%.2f,%.2f]", xyz[0], xyz[1], xyz[2]);
                j += std::string(nt ? "," : "") + "{\"p\":" + p + ",\"m\":[";
                for (unsigned k = 0; k < 4; ++k) j += (k ? "," : "") + std::to_string(Rd16(node + 0x182 + 2 * k));
                j += "],\"to\":[";
                bool first = true;
                for (unsigned k = 0; k < (0x170 - 0xE4) / 4; ++k) {
                    const uint32_t target = Rd32(node + 0xE4 + 4 * k);
                    if (!InRange(target, 0x160)) continue;
                    const uint32_t onode = Rd32(target + 0xD4 + 4 + 4 * 1);   // InstanceContext::nodes (NodeList: mask, nodes[]) -> its object node
                    const uint32_t object = InRange(onode, 0x80) ? Rd32(onode + 0x78) : 0;
                    j += std::string(first ? "" : ",") + "\"" + (InRange(object, 0x60) ? RdString(object + 0x14) : std::string("?")) + "\"";
                    first = false;
                }
                j += "]}";
            }
        }
        std::fprintf(stderr, "[cs.chunk] %s]}\n", j.c_str());
    }
}

void Census(uint64_t boundary) {
    const uint32_t table = Rd32(RetailAddress(kScriptTable));
    const uint32_t capacity = Rd32(table) & 0x3FFF, items = Rd32(table + 4);
    if (!InRange(items, capacity * 4)) return;
    unsigned graphs = 0, starters = 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t item = Rd32(items + 4 * i);
        if (!InRange(item, 0x3C)) continue;
        const uint32_t vt = Rd32(item + 0x18);
        const uint32_t id = Rd32(item + 8) & 0xFFFF;
        if (!g_printed.insert({item, id}).second) continue;
        if (vt == RetailAddress(kGraphVTable) || vt == kGraphVTable) {
            const uint32_t data = Rd32(item + 0x1C);
            if (!InRange(data, 0x18)) continue;
            std::fprintf(stderr, "[cs.graph] %s\n", GraphJson(data).c_str());
            ++graphs;
        } else if (vt == RetailAddress(kStarterVTable) || vt == kStarterVTable) {
            std::fprintf(stderr, "[cs.starter] %s\n", StarterJson(item).c_str());
            ++starters;
        }
    }
    std::fprintf(stderr, "[cs.census] b %llu: %u new graphs, %u new starters\n", (unsigned long long)boundary, graphs, starters);
    LogChunks(boundary);
}

}  // namespace

void InstallCensus() {
    // A load adds its scripts over many boundaries: the census runs once the table has kept its signature for 30 boundaries
    Services::Get().clock().AddBoundaryHook("cutscene census", [](uint64_t boundary) {
        uint32_t count = 0;
        const uint32_t signature = TableSignature(&count);
        if (signature != g_signature) {
            g_signature = signature;
            g_settled_at = boundary;
            g_dirty = count != 0;
            return;
        }
        if (g_dirty && boundary - g_settled_at >= 30) {
            g_dirty = false;
            Census(boundary);
        }
    });
    std::fprintf(stderr, "[cs.census] on: the scripts' table logged whenever it settles (TE_CUTSCENE_CENSUS)\n");
}

namespace {

constexpr uint32_t kGameController = 0x00309888;   // G_GameController
constexpr uint32_t kGlobalClock = 0x003238B0;      // G_GlobalClock_ (TimeClock: flags, time, advance)
constexpr uint32_t kPlayerCharacter = 0x003098FC;  // the played character's agent (te_probe.cpp)
constexpr uint32_t kMusic = 0x00309E58;            // g_Music (game/sound.h MusicSystem*: prepared[4], playing[4], the players)

// game/place.h: the position the place's matrix keeps when bit 2 of its bits says so (SyncPosition), else its own
void PlacePosition(uint32_t place, float* xyz) {
    xyz[0] = xyz[1] = xyz[2] = 0.f;
    if (!InRange(place, 0x70)) return;
    const uint32_t at = (Rd32(place + 0x60) & 0x4) ? place + 0x30 : place + 0x40;
    for (int k = 0; k < 3; ++k) xyz[k] = RdF(at + 4u * k);
}
std::string Vec(const float* v) {
    char b[96];
    std::snprintf(b, sizeof b, "[%.4f,%.4f,%.4f]", v[0], v[1], v[2]);
    return b;
}
// A runner (BehaviourRunner, behaviours.h): whether it runs a behaviour, and per level the graph's id and the state's index
std::string RunnerJson(uint32_t runner) {
    if (!InRange(runner, 0x60) || !Rd32(runner + 0x14)) return "0";
    std::string j = "[";
    const unsigned depth = Rd8(runner + 0x28);
    for (unsigned l = 0; l <= depth && l < 8; ++l) {
        const uint32_t level = Rd32(runner + 0x2C + 4 * l);
        if (!InRange(level, 0x20)) break;
        const uint32_t graph = Rd32(level), state = Rd32(level + 4);
        if (l) j += ",";
        const int gid = InRange(graph, 0x18) ? int(Rd32(graph) & 0xFFFF) : -1;
        j += "[" + std::to_string(gid) + "," + std::to_string(InRange(graph, 0x18) ? IndexOf(States(graph), state) : -1) + "," +
             std::to_string((Rd32(level + 0x18) >> 8) & 1) + "]";
    }
    return j + "]";
}

void StateLine(uint64_t boundary, unsigned game_state) {
    const uint32_t gc = Rd32(RetailAddress(kGameController));
    std::string j = "{\"b\":" + std::to_string(boundary) + ",\"gs\":" + std::to_string(game_state) + ",\"clock\":" +
                    std::to_string(Rd32(RetailAddress(kGlobalClock) + 4));
    if (InRange(gc, 0x5A4)) {
        // GameProgress (GameController + 0x500): counts, play, the levels' gems and crystals
        j += ",\"counts\":" + std::to_string(Rd32(gc + 0x500)) + ",\"play\":" + std::to_string(Rd32(gc + 0x504)) + ",\"levels\":[";
        for (unsigned k = 0; k < 16; ++k) j += (k ? "," : "") + std::to_string(Rd32(gc + 0x534 + 4 * k));
        j += "]";
    }
    const uint32_t agent = Rd32(RetailAddress(kPlayerCharacter));
    const uint32_t player = InRange(agent, 0x110) ? Rd32(agent) : 0;
    if (InRange(player, 0x160)) {
        float xyz[3];
        PlacePosition(Rd32(player + 0x08), xyz);
        const uint32_t chunk = Rd32(player + 0xA0);
        j += ",\"player\":{\"p\":" + Vec(xyz) + ",\"chunk\":\"" + (InRange(chunk, 0x1F0) ? RdString(chunk + 0x138) : std::string()) +
             "\",\"state\":" + std::to_string(Rd32(agent + 0x70)) + "}";
        if (InRange(chunk, 0x1F0)) {
            // the camera: the player's chunk matrix's last row (te_probe.cpp's)
            float cam[3] = {RdF(chunk + 0x10 + 0x30), RdF(chunk + 0x10 + 0x34), RdF(chunk + 0x10 + 0x38)};
            j += ",\"camera\":" + Vec(cam);
        }
    }
    const uint32_t music = Rd32(RetailAddress(kMusic));
    if (InRange(music, 0xE0)) {
        // the music: per slot (game/sound.h MusicSlot: main, context, spare) the track its playing player plays and the one its
        // prepared player prepares (-1 none) -- which music the scripts left, not where in it (its fade and position are time)
        j += ",\"music\":[";
        for (unsigned slot = 0; slot < 4; ++slot) {
            const uint32_t playing = Rd32(music + 0x10 + 4 * slot), prepared = Rd32(music + 4 * slot);
            j += std::string(slot ? "," : "") + "[" + std::to_string(InRange(playing, 0x40) ? int32_t(Rd32(playing + 0x20)) : -1) + "," +
                 std::to_string(InRange(prepared, 0x40) ? int32_t(Rd32(prepared + 0x20)) : -1) + "]";
        }
        j += "]";
    }
    const uint32_t manager = Rd32(RetailAddress(kChunkManager));
    if (InRange(manager, 0x1FC0)) {
        // the game's counters the scripts set (ChunkManager::counters, after the instances' IDs): the non-zero ones
        j += ",\"counters\":{";
        bool first = true;
        const uint32_t counters = manager + 4 + 4 * 0x200 + 4 * 4 + 0x804;   // entries, reader, objects, resources, finder, ids
        for (unsigned k = 0; k < 1000; ++k) {
            const uint32_t v = Rd32(counters + 4 * k);
            if (!v) continue;
            j += std::string(first ? "" : ",") + "\"" + std::to_string(k) + "\":" + std::to_string(int32_t(v));
            first = false;
        }
        j += "},\"chunks\":[";
        const unsigned count = Rd16(manager);
        bool firstc = true;
        for (unsigned e = 0; e < count && e < 0x200; ++e) {
            const uint32_t entry = Rd32(manager + 4 + 4 * e);
            if (!InRange(entry, 0x38)) continue;
            const uint32_t data_ref = Rd32(entry + 0x14), chunk = InRange(data_ref, 8) ? Rd32(data_ref) : 0;
            j += std::string(firstc ? "" : ",") + "{\"c\":\"" + RdString(entry) + "\",\"saved\":[";
            firstc = false;
            // SavedFlagStore / UnsavedFlagStore (game/src/game/gamechunkmanager.cpp): the vtable, then eight words
            for (unsigned which = 0; which < 2; ++which) {
                const uint32_t store = Rd32(entry + 0x1C + 4 * which);
                if (which) j += "],\"unsaved\":[";
                for (unsigned k = 0; k < 8; ++k) j += (k ? "," : "") + std::to_string(InRange(store, 0x24) ? Rd32(store + 4 + 4 * k) : 0);
            }
            j += "],\"i\":[";
            if (InRange(chunk, 0x1F0)) {
                const uint32_t insts = Rd32(chunk + 0x15C);
                unsigned ni = 0;
                for (uint32_t node = InRange(insts, 0x68) ? Rd32(insts + 0x08 + 4 * 1) : 0; InRange(node, 0xD0) && ni < 4096;
                     node = Rd32(node + 0x10), ++ni) {
                    const uint32_t owner = Rd32(node), object = Rd32(node + 0x78);
                    float xyz[3];
                    PlacePosition(InRange(owner, 0x160) ? Rd32(owner + 0x08) : 0, xyz);
                    // the agent's id (ObjectNodeBase::agent -> Agent::id: its persistent flag's slot in the chunk's stores)
                    const uint32_t agent_ptr = Rd32(node + 0x84);
                    const int agent_id = InRange(agent_ptr, 0x18) ? int(Rd16(agent_ptr + 0x16)) : -1;
                    j += std::string(ni ? "," : "") + "{\"id\":" + std::to_string(InRange(owner, 0x158) ? int32_t(Rd32(owner + 0x154)) : -1) +
                         ",\"a\":" + std::to_string(agent_id) +
                         // the frame stamp it was last seen at (InstanceContext::seen): an instance not seen for long is updated
                         // seldom (game/src/game/objectnode.cpp's update rate), its loops frozen where they were
                         ",\"seen\":" + std::to_string(InRange(owner, 0x154) ? (Rd32(owner + 0x150) & 0xFFFFFF) : 0) +
                         ",\"n\":\"" + (InRange(object, 0x60) ? RdString(object + 0x14) : std::string()) + "\",\"p\":" + Vec(xyz) +
                         ",\"f\":" + std::to_string(InRange(owner, 0x8) ? Rd32(owner + 0x04) : 0) + ",\"r\":[" + RunnerJson(Rd32(node + 0xA0)) +
                         "," + RunnerJson(Rd32(node + 0xA4)) + "]}";
                }
            }
            j += "]}";
        }
        j += "]";
    }
    std::fprintf(stderr, "[cs.state] %s}\n", j.c_str());
}

}  // namespace

namespace {

// The instance of a loaded chunk whose object is named `name` (its whole name or the part after the last '|'), its object node
uint32_t FindObjectNode(const std::string& name) {
    const uint32_t manager = Rd32(RetailAddress(kChunkManager));
    if (!InRange(manager, 0x1FC0)) return 0;
    const unsigned count = Rd16(manager);
    for (unsigned e = 0; e < count && e < 0x200; ++e) {
        const uint32_t entry = Rd32(manager + 4 + 4 * e);
        if (!InRange(entry, 0x38)) continue;
        const uint32_t data_ref = Rd32(entry + 0x14), chunk = InRange(data_ref, 8) ? Rd32(data_ref) : 0;
        if (!InRange(chunk, 0x1F0)) continue;
        const uint32_t insts = Rd32(chunk + 0x15C);
        unsigned ni = 0;
        for (uint32_t node = InRange(insts, 0x68) ? Rd32(insts + 0x08 + 4 * 1) : 0; InRange(node, 0xD0) && ni < 4096;
             node = Rd32(node + 0x10), ++ni) {
            const uint32_t object = Rd32(node + 0x78);
            if (!InRange(object, 0x60)) continue;
            const std::string n = RdString(object + 0x14);
            const size_t bar = n.find_last_of('|');
            if (n == name || (bar != std::string::npos && n.substr(bar + 1) == name)) return node;
        }
    }
    return 0;
}

void Write32(uint32_t address, uint32_t value) {
    if (uint8_t* p = GuestPointer(address, 4)) std::memcpy(p, &value, 4);
}

}  // namespace

void InstallStart(const char* spec) {
    std::string s = spec;
    unsigned long long at = 0;
    const size_t at_sign = s.rfind('@');
    if (at_sign != std::string::npos) { at = std::strtoull(s.c_str() + at_sign + 1, nullptr, 10); s = s.substr(0, at_sign); }
    unsigned message = 87;
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) { message = unsigned(std::strtoul(s.c_str() + colon + 1, nullptr, 10)); s = s.substr(0, colon); }
    Services::Get().clock().AddBoundaryHook("cutscene start", [s, message, at](uint64_t boundary) {
        static bool done = false;
        unsigned state = 0, last = 0, next = 0;
        if (done || boundary < at || !harness::ReadGameState(&state, &last, &next) || state != 12) return;
        done = true;
        const uint32_t node = FindObjectNode(s);
        const uint32_t object = node ? Rd32(node + 0x78) : 0;
        if (!InRange(object, 0x60)) { std::fprintf(stderr, "[cs.start] b %llu: no loaded instance of %s\n", (unsigned long long)boundary, s.c_str()); return; }
        // the object's trigger behaviour for the message (game/include/game/objects.h TriggerBehaviour: message, starter, runner)
        const uint32_t trig = Rd32(object + 0x30), nt = Rd32(object + 0x34);
        for (uint32_t k = 0; k < nt && k < 64; ++k) {
            const uint32_t w = Rd32(trig + 4 * k);
            if ((w & 0x3FF) != message) continue;
            const uint32_t id = (w >> 10) & 0x3FFF, slot = (w >> 24) & 1;
            const uint32_t table = Rd32(RetailAddress(kScriptTable));
            const uint32_t starter = Rd32(Rd32(table + 4) + 4 * (id & 0x7FFF));
            const uint32_t runner = Rd32(node + 0xA0 + 4 * slot);
            const uint32_t agent = Rd32(RetailAddress(kPlayerCharacter));
            const uint32_t player = InRange(agent, 0x110) ? Rd32(agent) : 0;
            if (!InRange(runner, 0x60) || !InRange(starter, 0x3C)) {
                std::fprintf(stderr, "[cs.start] b %llu: %s has no runner %u for message %u's starter %u\n", (unsigned long long)boundary,
                             s.c_str(), slot, message, id);
                return;
            }
            Write32(runner + 0x18, starter);   // BehaviourRunner::nextStarter
            Write32(runner + 0x1C, player);    // nextOriginator
            std::fprintf(stderr, "[cs.start] b %llu: %s given starter %u (message %u's) on runner %u\n", (unsigned long long)boundary,
                         s.c_str(), id, message, slot);
            return;
        }
        std::fprintf(stderr, "[cs.start] b %llu: %s has no trigger behaviour for message %u\n", (unsigned long long)boundary, s.c_str(), message);
    });
}

uint32_t FindInstance(const std::string& name) {
    const uint32_t node = FindObjectNode(name);
    return node ? Rd32(node) : 0;   // GameNode::owner
}

bool RunsGraph(const std::string& name, int graph, int state) {
    const uint32_t node = FindObjectNode(name);
    if (!node) return false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        // ObjectNodeBase::runners, a runner's levels (runner/te_cutscenes.cpp RunnerJson's reading)
        const uint32_t runner = Rd32(node + 0xA0 + 4 * slot);
        if (!InRange(runner, 0x60) || !Rd32(runner + 0x14)) continue;
        const unsigned depth = Rd8(runner + 0x28);
        for (unsigned l = 0; l <= depth && l < 8; ++l) {
            const uint32_t level = Rd32(runner + 0x2C + 4 * l);
            if (!InRange(level, 0x20)) break;
            const uint32_t data = Rd32(level);
            if (!InRange(data, 0x18) || int(Rd32(data) & 0xFFFF) != graph) continue;
            if (state < 0 || IndexOf(States(data), Rd32(level + 4)) == state) return true;
        }
    }
    return false;
}

namespace {

// An instance's object name (its object node's object), "" when it has none (a trigger's own instance)
std::string InstanceName(uint32_t instance) {
    if (!InRange(instance, 0x160)) return "";
    const uint32_t onode = Rd32(instance + 0xD4 + 4 + 4 * 1);   // InstanceContext::nodes (NodeList: mask, nodes[]) -> NodeObject
    const uint32_t object = InRange(onode, 0x80) ? Rd32(onode + 0x78) : 0;
    return InRange(object, 0x60) ? RdString(object + 0x14) : std::string();
}

// One loaded chunk's links: every object instance (its name, place, its properties' state bits -- InstanceState, the scripts'
// soft flags -- and the instances its attachments link, which SendUserMessage and RunSlotBehaviourOnLinked reach) and every
// message trigger (its place, its bits -- kind, how many it tells, what it tells: first entry, every entry, entry or stay, exit
// -- its four messages and the instances it tells): who can start a cutscene's director (game/include/game/instances.h
// TriggerNode / MessageTriggerNode, attachments.h AttachmentsNode)
void LogLinks(uint64_t boundary) {
    const uint32_t manager = Rd32(RetailAddress(kChunkManager));
    if (!InRange(manager, 0x1FC0)) return;
    const unsigned count = Rd16(manager);
    for (unsigned e = 0; e < count && e < 0x200; ++e) {
        const uint32_t entry = Rd32(manager + 4 + 4 * e);
        if (!InRange(entry, 0x38)) continue;
        const uint32_t data_ref = Rd32(entry + 0x14), chunk = InRange(data_ref, 8) ? Rd32(data_ref) : 0;
        if (!InRange(chunk, 0x1F0)) continue;
        const uint32_t insts = Rd32(chunk + 0x15C);
        if (!InRange(insts, 0x68)) continue;
        std::string j = "{\"b\":" + std::to_string(boundary) + ",\"chunk\":\"" + RdString(entry) + "\",\"i\":[";
        // the awake instances' object nodes (the kind's list), then the sleeping instances' (ChunkInstances::sleeping, linked by
        // InstanceContext::next): a far trigger or director sleeps until the player comes near
        std::vector<std::pair<uint32_t, bool>> objects, triggers;
        for (uint32_t node = Rd32(insts + 0x08 + 4 * 1), n = 0; InRange(node, 0xD0) && n < 4096; node = Rd32(node + 0x10), ++n)
            objects.push_back({node, false});
        for (uint32_t node = Rd32(insts + 0x08 + 4 * 7), n = 0; InRange(node, 0x1A0) && n < 1024; node = Rd32(node + 0x10), ++n)
            triggers.push_back({node, false});
        for (uint32_t inst = Rd32(insts), n = 0; InRange(inst, 0x160) && n < 4096; inst = Rd32(inst + 0x144), ++n) {
            const uint32_t onode = Rd32(inst + 0xD4 + 4 + 4 * 1), tnode = Rd32(inst + 0xD4 + 4 + 4 * 7);
            if (InRange(onode, 0xD0)) objects.push_back({onode, true});
            if (InRange(tnode, 0x1A0)) triggers.push_back({tnode, true});
        }
        // the triggers the characters check (never polled: not in the kind's list) are found in the chunk's instance cells
        // (ChunkData::instanceCells, 0x301 lists linked by InstanceContext::cellNext)
        {
            std::set<uint32_t> known;
            for (const auto& t : triggers) known.insert(t.first);
            const uint32_t cells = Rd32(chunk + 0x160);
            for (uint32_t c = 0; InRange(cells, 0x301 * 4) && c < 0x301; ++c) {
                for (uint32_t inst = Rd32(cells + 4 * c), n = 0; InRange(inst, 0x160) && n < 4096; inst = Rd32(inst + 0x14C), ++n) {
                    const uint32_t tnode = Rd32(inst + 0xD4 + 4 + 4 * 7);
                    if (InRange(tnode, 0x1A0) && known.insert(tnode).second) triggers.push_back({tnode, false});
                }
            }
        }
        unsigned ni = 0;
        for (const auto& [node, asleep] : objects) {
            const uint32_t owner = Rd32(node), object = Rd32(node + 0x78), props = Rd32(node + 0x80);
            float xyz[3];
            PlacePosition(InRange(owner, 0x160) ? Rd32(owner + 0x08) : 0, xyz);
            j += std::string(ni++ ? "," : "") + "{\"n\":\"" + (InRange(object, 0x60) ? RdString(object + 0x14) : std::string()) +
                 "\",\"p\":" + Vec(xyz) + ",\"st\":" + std::to_string(InRange(props, 4) ? Rd32(props) : 0) + (asleep ? ",\"sl\":1" : "") +
                 ",\"ln\":[";
            const uint32_t att = InRange(owner, 0x160) ? Rd32(owner + 0xD4 + 4 + 4 * 6) : 0;   // NodeAttachments
            if (InRange(att, 0x64)) {
                const unsigned linked = Rd32(att + 0x18) & 0x1F;
                for (unsigned k = 0; k < linked && k < 16; ++k)
                    j += std::string(k ? "," : "") + "\"" + InstanceName(Rd32(att + 0x20 + 4 * k)) + "\"";
            }
            j += "]}";
        }
        j += "],\"t\":[";
        unsigned nt = 0;
        for (const auto& [node, asleep] : triggers) {
            const uint32_t owner = Rd32(node), bits = Rd32(node + 0x18);
            float xyz[3];
            PlacePosition(InRange(owner, 0x160) ? Rd32(owner + 0x08) : 0, xyz);
            // its box's corner (InstanceContext::box: the trigger's scale, the box -scale..scale in its instance's space)
            float box[3] = {RdF(owner + 0xC0), RdF(owner + 0xC4), RdF(owner + 0xC8)};
            j += std::string(nt++ ? "," : "") + "{\"p\":" + Vec(xyz) + ",\"box\":" + Vec(box) + ",\"bits\":" + std::to_string(bits) +
                 (asleep ? ",\"sl\":1" : "") + ",\"m\":[";
            for (unsigned k = 0; k < 4; ++k) j += (k ? "," : "") + std::to_string(Rd16(node + 0x182 + 2 * k));
            j += "],\"to\":[";
            const unsigned told = (bits >> 8) & 0xFF;
            for (unsigned k = 0; k < told && k < (0x170 - 0xE4) / 4; ++k)
                j += std::string(k ? "," : "") + "\"" + InstanceName(Rd32(node + 0xE4 + 4 * k)) + "\"";
            j += "]}";
        }
        std::fprintf(stderr, "[cs.links] %s]}\n", j.c_str());
    }
}

}  // namespace

void InstallLinks(const char* spec) {
    // B[,B...]: the links of the loaded chunks at each boundary given; "play": 60 boundaries after each start of play
    const bool on_play = std::strncmp(spec, "play", 4) == 0;
    std::vector<uint64_t> at;
    for (const char* p = spec; *p && !on_play;) {
        char* end = nullptr;
        const unsigned long long b = std::strtoull(p, &end, 10);
        if (end == p) break;
        at.push_back(b);
        p = *end == ',' ? end + 1 : end;
    }
    Services::Get().clock().AddBoundaryHook("cutscene links", [at, on_play](uint64_t boundary) {
        static unsigned last = ~0u;
        static uint64_t due = 0;
        if (on_play) {
            unsigned state = 0, last_state = 0, next_state = 0;
            if (!harness::ReadGameState(&state, &last_state, &next_state)) return;
            if (state == 12 && last == 11) due = boundary + 60;
            last = state;
            if (due && boundary == due) { due = 0; LogLinks(boundary); }
            return;
        }
        for (uint64_t b : at)
            if (b == boundary) LogLinks(boundary);
    });
    std::fprintf(stderr, "[cs.links] on: the loaded chunks' links %s (TE_CUTSCENE_LINKS)\n", on_play ? "60 boundaries into each play" : "at the boundaries given");
}

void InstallStarts() {
    // Every boundary, each loaded object instance's runners: when a runner's first level runs another graph than the boundary
    // before (a behaviour started: a trigger behaviour, a slot behaviour, a restart), one line with the instance, the slot, the
    // graph and the runner's originator (BehaviourRunner::originator: what sent the message or started it) -- who starts what
    Services::Get().clock().AddBoundaryHook("cutscene starts", [](uint64_t boundary) {
        static std::map<uint32_t, int> last;   // runner address -> graph id of its first level (-1 none)
        std::map<uint32_t, int> now;
        const uint32_t manager = Rd32(RetailAddress(kChunkManager));
        if (!InRange(manager, 0x1FC0)) return;
        const unsigned count = Rd16(manager);
        for (unsigned e = 0; e < count && e < 0x200; ++e) {
            const uint32_t entry = Rd32(manager + 4 + 4 * e);
            if (!InRange(entry, 0x38)) continue;
            const uint32_t data_ref = Rd32(entry + 0x14), chunk = InRange(data_ref, 8) ? Rd32(data_ref) : 0;
            if (!InRange(chunk, 0x1F0)) continue;
            const uint32_t insts = Rd32(chunk + 0x15C);
            unsigned ni = 0;
            for (uint32_t node = InRange(insts, 0x68) ? Rd32(insts + 0x08 + 4 * 1) : 0; InRange(node, 0xD0) && ni < 4096;
                 node = Rd32(node + 0x10), ++ni) {
                for (unsigned slot = 0; slot < 2; ++slot) {
                    const uint32_t runner = Rd32(node + 0xA0 + 4 * slot);
                    if (!InRange(runner, 0x60)) continue;
                    const uint32_t level = Rd32(runner + 0x2C), graph = InRange(level, 0x20) ? Rd32(level) : 0;
                    const int gid = Rd32(runner + 0x14) && InRange(graph, 0x18) ? int(Rd32(graph) & 0xFFFF) : -1;
                    now[runner] = gid;
                    const auto was = last.find(runner);
                    if (gid < 0 || (was != last.end() && was->second == gid)) continue;
                    const uint32_t object = Rd32(node + 0x78), originator = Rd32(runner + 0x20);
                    std::fprintf(stderr, "[cs.starts] b %llu: %s %s r%u runs %d <- %s\n", (unsigned long long)boundary,
                                 RdString(entry).c_str(), InRange(object, 0x60) ? RdString(object + 0x14).c_str() : "?", slot, gid,
                                 originator ? InstanceName(originator).c_str() : "-");
                }
            }
        }
        last.swap(now);
    });
    std::fprintf(stderr, "[cs.starts] on: every behaviour started, with its originator (TE_CUTSCENE_STARTS)\n");
}

void InstallState(const char* spec) {
    // N, or N@FROM-TO: lines only within [FROM, TO] (a diagnosis window at every boundary)
    unsigned every = unsigned(std::strtoul(spec, nullptr, 10));
    unsigned long long from = 0, to = ~0ull;
    if (const char* at = std::strchr(spec, '@')) std::sscanf(at + 1, "%llu-%llu", &from, &to);
    if (every == 0) every = 60;
    // A line at every change of the game's state among Watching (13), StartingPlay (11) and Playing (12), and every `every`
    // boundaries while the game plays or watches
    Services::Get().clock().AddBoundaryHook("cutscene state", [every, from, to](uint64_t boundary) {
        static unsigned last = ~0u;
        static uint64_t next = 0;
        unsigned state = 0, last_state = 0, next_state = 0;
        if (boundary < from || boundary > to) return;
        if (!harness::ReadGameState(&state, &last_state, &next_state)) return;
        const bool watched = state == 11 || state == 12 || state == 13;
        if (state != last && (watched || last == 11 || last == 12 || last == 13)) {
            last = state;
            next = boundary + every;
            StateLine(boundary, state);
            return;
        }
        last = state;
        if (watched && boundary >= next) {
            next = boundary + every;
            StateLine(boundary, state);
        }
    });
    std::fprintf(stderr, "[cs.state] on: a state line every %u boundaries of play and at each Watching / StartingPlay / Playing change\n", every);
}

}  // namespace te::cutscenes
