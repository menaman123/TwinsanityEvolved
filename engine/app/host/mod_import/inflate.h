#pragma once
// Inflate: a DEFLATE (RFC 1951) decoder -- stored, fixed-Huffman and dynamic-Huffman blocks -- for the zip reader. Raw
// deflate (no zlib or gzip wrapper), streaming: compressed bytes are pulled from a source callback in chunks and the
// decoded bytes pushed to a sink callback in chunks, so an entry of any size decodes in a fixed amount of memory (a
// 32 KB history plus a 1 MB output buffer). The port's own: no lane of this repository has zlib.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace te::modimport {

// Pulls up to n compressed bytes into dst; returns how many (0 = no more input).
using InflateSource = std::function<size_t(uint8_t* dst, size_t n)>;
// Takes n decoded bytes; false stops the decode (Inflate then returns false with *error "stopped").
using InflateSink = std::function<bool(const uint8_t* data, size_t n)>;

// Decodes one raw DEFLATE stream (to its final block). False with *error on malformed input, a truncated stream or a
// sink that stopped. *consumed_out (optional) = compressed bytes the stream used; *produced_out = bytes decoded.
bool Inflate(const InflateSource& source, const InflateSink& sink, std::string* error, uint64_t* consumed_out = nullptr,
             uint64_t* produced_out = nullptr);

}  // namespace te::modimport
