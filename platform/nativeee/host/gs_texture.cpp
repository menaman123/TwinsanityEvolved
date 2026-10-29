// gs_texture.cpp -- see gs_texture.h. The chain walker follows what the PS2 renderer's texture upload produced: the
// slot's BITBLTBUF (written here as the upload packet wrote it: base 0, the buffer width 64 pixels unless the texels are
// 32-bit, the file's upload format) then a CALL into the file's chain, which is CNT/RET/END tags whose VIF codes are
// DIRECT (GIF data follows) with GIF A+D writes of TRXPOS/TRXREG/TRXDIR and IMAGE data. REF/NEXT/CALL tags would carry
// absolute addresses the file cannot hold, so they end the walk with an error.
#include "gs_texture.h"

#include <algorithm>

#include <cstdio>
#include <cstring>
#include <memory>

#include "engine/gs/hle/ir/tex_read_view.h"
#include "engine/gs/hle/texsrc/texture_decoder.h"
#include "engine/gs/vram/vram_image.h"
#include "engine/gs/xfer/transfer_engine.h"

namespace te::gfx {
namespace {

constexpr uint32_t kDmaCnt = 1, kDmaNext = 2, kDmaRef = 3, kDmaRefs = 4, kDmaCall = 5, kDmaRet = 6, kDmaEnd = 7,
                   kDmaRefe = 0;
constexpr uint32_t kVifDirect = 0x50, kVifDirectHl = 0x51, kVifNop = 0x00, kVifFlushA = 0x13, kVifFlush = 0x11,
                   kVifFlushE = 0x10, kVifMark = 0x07, kVifStcycl = 0x01, kVifOffset = 0x02, kVifBase = 0x03,
                   kVifItop = 0x04, kVifStmod = 0x05, kVifMskpath3 = 0x06;
constexpr uint8_t kRegBitBltBuf = 0x50, kRegTrxPos = 0x51, kRegTrxReg = 0x52, kRegTrxDir = 0x53, kRegHwReg = 0x54;

struct GifState {
    uint32_t nloop = 0;      // loops left in the current tag
    uint32_t flg = 0;        // 0 PACKED, 1 REGLIST, 2/3 IMAGE
    uint32_t nreg = 0;
    uint64_t regs = 0;
    uint32_t reg_index = 0;
    bool in_tag = false;
};

uint64_t Load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

struct Walker {
    ps2r::gs::TransferEngine& xfer;
    GifState gif;
    uint32_t direct_left = 0;    // GIF quadwords the current DIRECT still owes
    uint32_t transfers = 0, image_bytes = 0;
    std::string* error;

    bool Fail(const char* what) {
        if (error) *error = what;
        return false;
    }

    // One GIF quadword
    bool GifQuad(const uint8_t* q) {
        if (!gif.in_tag) {
            const uint64_t lo = Load64(q);
            gif.nloop = uint32_t(lo & 0x7FFF);
            gif.flg = uint32_t((lo >> 58) & 3);
            gif.nreg = uint32_t((lo >> 60) & 0xF);
            if (gif.nreg == 0) gif.nreg = 16;
            gif.regs = Load64(q + 8);
            gif.reg_index = 0;
            gif.in_tag = gif.nloop != 0;
            return true;
        }
        if (gif.flg >= 2) {
            // IMAGE: this quadword is pixel data
            xfer.WriteImage(q, 16);
            image_bytes += 16;
            if (--gif.nloop == 0) gif.in_tag = false;
            return true;
        }
        if (gif.flg == 0) {
            // PACKED: one register per quadword, the descriptor from the regs list
            const uint32_t desc = uint32_t((gif.regs >> (gif.reg_index * 4)) & 0xF);
            if (desc == 0xE) {
                const uint64_t data = Load64(q);
                const uint8_t addr = uint8_t(Load64(q + 8) & 0xFF);
                if (addr == kRegTrxDir) transfers++;
                if (!xfer.WriteRegister(addr, data) && addr != 0x3F /* TEXFLUSH */) {
                    // other GS registers in a texture chain are not expected; tolerated
                }
            }
            if (++gif.reg_index == gif.nreg) {
                gif.reg_index = 0;
                if (--gif.nloop == 0) gif.in_tag = false;
            }
            return true;
        }
        // REGLIST: two registers per quadword (not used by texture uploads)
        for (int half = 0; half < 2; ++half) {
            const uint32_t desc = uint32_t((gif.regs >> (gif.reg_index * 4)) & 0xF);
            (void)desc;
            if (++gif.reg_index == gif.nreg) {
                gif.reg_index = 0;
                if (--gif.nloop == 0) {
                    gif.in_tag = false;
                    break;
                }
            }
        }
        return true;
    }

    // One VIF code word (the DMA tag's upper two words, or a data word outside DIRECT)
    void VifCode(uint32_t code) {
        const uint32_t cmd = (code >> 24) & 0x7F;
        switch (cmd) {
            case kVifDirect:
            case kVifDirectHl: {
                uint32_t count = code & 0xFFFF;
                if (count == 0) count = 0x10000;
                direct_left += count;
                gif.in_tag = false;   // a DIRECT starts with a GIF tag
                break;
            }
            default:
                break;   // NOP / FLUSH* / MARK / STCYCL ...: no effect on the GS
        }
    }

    bool Walk(const uint8_t* packet, size_t size) {
        size_t at = 0;
        while (at + 16 <= size) {
            const uint32_t w0 = Load32(packet + at), w2 = Load32(packet + at + 8), w3 = Load32(packet + at + 12);
            const uint32_t qwc = w0 & 0xFFFF;
            const uint32_t id = (w0 >> 28) & 7;
            at += 16;
            if (id == kDmaNext || id == kDmaRef || id == kDmaRefs || id == kDmaCall || id == kDmaRefe)
                return Fail("DMA tag with an absolute address (NEXT/REF/REFS/CALL/REFE) inside a texture chain");
            VifCode(w2);
            VifCode(w3);
            const size_t data_end = at + size_t(qwc) * 16;
            if (data_end > size) return Fail("DMA tag's data runs past the packet");
            while (at < data_end) {
                if (direct_left) {
                    if (!GifQuad(packet + at)) return false;
                    direct_left--;
                } else {
                    VifCode(Load32(packet + at));
                    VifCode(Load32(packet + at + 4));
                    VifCode(Load32(packet + at + 8));
                    VifCode(Load32(packet + at + 12));
                }
                at += 16;
            }
            if (id == kDmaRet || id == kDmaEnd) return true;
            if (id != kDmaCnt) return Fail("unknown DMA tag id");
        }
        return true;   // ran off the end without RET/END: what was executed stands
    }
};

}  // namespace

bool DecodeTextureResource(const te_gfx_texture_header& h, const uint8_t* packet, size_t size, DecodedTexture& out,
                           std::string* error) {
    if (h.widthPower > 10 || h.heightPower > 10) {
        if (error) *error = "texture size power out of range";
        return false;
    }
    // one scratch GS memory per decoding thread, Reset between decodes (VramImage::Reset: the pages the last
    // decode touched zeroed, every record as new) instead of a new 4 MiB image -- allocated and zero-filled per texture,
    // the largest part of a texture's load on the device's CPU (the title loads ~320 of them, LoadingTitle and the boot's
    // Starting state, the game thread waiting: device-run8.log "ps2e_gfx2_texture_loaded 26.6 %" of 601-900). The bytes
    // and the records a decode reads are those of a new image.
    static thread_local std::unique_ptr<ps2r::gs::VramImage> scratch;
    if (scratch) scratch->Reset();
    else scratch = std::make_unique<ps2r::gs::VramImage>();
    ps2r::gs::VramImage& vram = *scratch;
    ps2r::gs::TransferEngine xfer(&vram);
    // The slot upload's BITBLTBUF (textures.cpp WriteTextureUpload): base 0 here, the buffer 64 pixels wide unless the
    // texels are 32-bit, the upload format.
    const uint64_t width = h.pixelFormat != 0 ? 1u : uint64_t(h.levelWidths[0]);
    const uint64_t bitblt = (uint64_t(0) << 32) | (width << 48) | (uint64_t(h.uploadFormat) << 56);
    xfer.WriteRegister(kRegBitBltBuf, bitblt);
    Walker walker{xfer, {}, 0, 0, 0, error};
    if (!walker.Walk(packet, size)) return false;
    vram.SettleWrites();

    // TEX0 as WriteTexturePairs builds it (the slot base 0): the first level's address and width, the texel format, the
    // size powers, TCC, TFX, the palette at its offset (CPSM CT32, CSM1, CSA 0).
    ps2r::gs::TexReadView v;
    v.tbp = h.levelOffsets[0] * 256u;
    v.tbw = h.levelWidths[0] * 64u;
    v.psm = h.pixelFormat;
    v.tw = h.widthPower;
    v.th = h.heightPower;
    v.tcc = h.hasAlpha ? 1u : 0u;
    v.tfx = h.function & 3u;
    v.cbp = h.paletteOffset * 256u;
    v.cpsm = 0;
    v.csm = 0;
    v.csa = 0;
    v.cld = 1;
    v.fst = false;
    v.mxl = h.levels > 1 ? uint8_t(h.levels - 1) : 0;
    v.texa_ta0 = 0;
    v.texa_ta1 = 0x80;
    v.texa_aem = false;
    out.width = 1 << h.widthPower;
    out.height = 1 << h.heightPower;
    out.transfers = walker.transfers;
    out.image_bytes = walker.image_bytes;
    ps2r::gs::TextureDecoder::Decode(vram.Bytes(), v, nullptr, nullptr, &out.rgba);
    if (out.rgba.size() != size_t(out.width) * out.height) {
        if (error) *error = "decoder produced the wrong texel count";
        return false;
    }
    // The levels below: the same view at each level's address and buffer width, its size halved per level (the GS reads
    // level i at MIPTBPi with TW / TH less i, never under 1). A level the decoder cannot read ends the chain there.
    out.mips.clear();
    const unsigned levels = std::min<unsigned>(h.levels, 7);
    for (unsigned i = 1; i < levels; ++i) {
        ps2r::gs::TexReadView lv = v;
        lv.tbp = h.levelOffsets[i] * 256u;
        lv.tbw = std::max<uint32_t>(h.levelWidths[i], 1u) * 64u;
        lv.tw = h.widthPower > i ? h.widthPower - i : 0;
        lv.th = h.heightPower > i ? h.heightPower - i : 0;
        lv.mxl = 0;
        std::vector<uint32_t> level;
        ps2r::gs::TextureDecoder::Decode(vram.Bytes(), lv, nullptr, nullptr, &level);
        if (level.size() != (size_t(1) << lv.tw) * (size_t(1) << lv.th)) break;
        out.mips.push_back(std::move(level));
    }
    return true;
}

}  // namespace te::gfx
