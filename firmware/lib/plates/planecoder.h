// The coder for a plate's two planes, as `tools/planecoder.py` writes them: an
// adaptive context model driving LZMA's range coder. The Python module's
// docstring has the why; this is the same arithmetic, step for step, and has
// to stay that way - a stream is only readable by the model that wrote it.
//
// Luma: a code a pixel (0-14 a level, 15 outside), each coded in the context
// of the pixels above and to its left, the models starting from kLumaPrior.
// Chroma: a code a block, only the blocks with anything painted in them, each
// in the context of the blocks above and to its left and its own luma - so
// the luma plane comes first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace birdposter {
namespace planecoder {

// w * h codes, row-major, into `out`. False when the stream is not exactly
// one plane of that size - the decoder reads what the encoder wrote, byte
// for byte, so a short or corrupt stream shows as a length that does not add up.
bool decodeLuma(const uint8_t *data, size_t len, int w, int h, uint8_t *out);

// The chroma codes of a `w` x `h` sprite with this luma plane, a code a
// `block` x `block` block, row-major into `out`; an unpainted block comes back 0.
bool decodeChroma(const uint8_t *data, size_t len, const uint8_t *luma, int w, int h, int block,
                  uint8_t *out);

// The other direction, for tests and host tools; the bake itself is Python.
std::vector<uint8_t> encodeLuma(const uint8_t *codes, int w, int h);
std::vector<uint8_t> encodeChroma(const uint8_t *chroma, const uint8_t *luma, int w, int h,
                                  int block);

}  // namespace planecoder
}  // namespace birdposter
