#pragma once

#include "ColorMaps.h"

#include <QImage>
#include <array>
#include <vector>
#include <QPointF>
#include <QSize>

// Resampling of part of an image for display.
//
// Both functions render an `outSize` image where output pixel (i, j) covers
// the source area starting at
//   x = origin.x() + i * step,  y = origin.y() + j * step
// in source pixel coordinates (source pixel k spans [k, k + 1)). So `origin`
// is the source position of the output's top-left corner and `step` is the
// source distance between neighbouring output pixels (1 / scale).
//
// `src` must be Format_ARGB32_Premultiplied; the result is in the same
// format.

// Nearest neighbour: every output pixel is an unmodified copy of the source
// pixel under its center. No values are invented or blended, so this shows
// the stored pixel values as they are.
QImage resampleNearest(const QImage &src, const QSize &outSize,
                       const QPointF &origin, double step);

// Bicubic (Catmull-Rom): smoother, but interpolates between pixels and can
// overshoot slightly at hard edges. Interpolating, so output pixels that land
// exactly on a source pixel center get exactly that pixel's value.
QImage resampleBicubic(const QImage &src, const QSize &outSize,
                       const QPointF &origin, double step);

// Lanczos3 (windowed sinc, three lobes): sharper than bicubic, but with more
// ringing (faint halos) at hard edges and about 1.5x the work. Also
// interpolating, like bicubic.
QImage resampleLanczos3(const QImage &src, const QSize &outSize,
                        const QPointF &origin, double step);

// Per-channel tone mapping table: output = lut[input] for each of R, G, B.
using ToneLut = std::array<quint8, 256>;

// Build the display tone table: optional inversion (v -> 255 - v), then a
// gamma curve out = 255 * (in / 255)^gamma. gamma > 1 darkens midtones and
// stretches contrast among bright values; gamma < 1 the opposite. Black and
// white map to themselves (or each other when inverted), so nothing clips.
ToneLut buildToneLut(bool invert, double gamma);

// How applyTone() changes saturation.
enum class SaturationMode {
  Linear,   // every pixel by the same factor
  Vibrance, // muted pixels by the full factor, vivid ones less
};

// Adjust the colour channels in place, leaving alpha unchanged: first scale
// saturation around the pixel's Rec. 709 luma L,
//   out = L + s * (in - L),
// on the stored values, then map each channel through `lut`. In Linear mode
// s = saturation (1 = unchanged, 0 = grey, > 1 more saturated). In Vibrance
// mode the change is weighted by how muted the pixel is,
//   s = 1 + (saturation - 1) * (1 - S),  S = (max - min) / max
// (its HSV saturation), so grey pixels get the full change and fully
// saturated ones none. Channels overshooting [0, 255] are clamped.
// With a colour map, the saturation change is skipped; instead, after the
// lut, each pixel is replaced by the map's colour for its Rec. 709 luma.
// Translucent pixels are un-premultiplied, adjusted and re-premultiplied.
// `img` must be Format_ARGB32_Premultiplied.
void applyTone(QImage &img, const ToneLut &lut, double saturation,
               SaturationMode mode, const ColorMap *colorMap = nullptr);

// Whether `format` has more than 8 bits per colour channel.
bool isDeepFormat(QImage::Format format);

// Linked contrast stretch: one black point and one white point for all
// colour channels, so hues are kept. Values are handled on a 16-bit scale
// (0-65535; an 8-bit value v counts as v * 257), so the stretch of a deeper
// image uses its full precision.

// Histogram of the R, G and B values of `img` pooled, on the 16-bit scale.
// Fully transparent pixels aren't counted.
using StretchHistogram = std::vector<quint64>;
StretchHistogram stretchHistogram(const QImage &img);

// The black and white points that clip `clipPercent` % of the histogram's
// values at each end; with 0, the lowest and highest values present.
// Always black < white: an image of a single value gets the full range.
struct StretchRange {
  int black = 0;
  int white = 65535;
};
StretchRange findStretchRange(const StretchHistogram &hist,
                              double clipPercent);

// `img` with its colour channels mapped linearly from [black, white] to
// [0, 255], clamped outside it, alpha unchanged. Returns
// Format_ARGB32_Premultiplied.
QImage applyStretch(const QImage &img, const StretchRange &range);

// High-pass view: what's left of `img` after taking away its Gaussian blur
// of standard deviation `sigma` pixels, so only detail smaller than about
// sigma remains. Each colour channel (on a 0-1 scale, after the stretch
// `range`; the default range is no stretch) becomes
//   out = 127.5 + 127.5 * (in - blur) / limit,
// clamped to [0, 255]: mid-grey where a pixel matches its surroundings,
// lighter or darker where it stands out. `limit` is one value for all
// channels, the size of difference that 0.1 % of the values exceed, so the
// gain adapts to the image and only the strongest outliers clip. It's
// returned through `limit` if given. Edges are extended by repeating the
// border pixels. Alpha is unchanged. Returns Format_ARGB32_Premultiplied.
QImage applyHighPass(const QImage &img, const StretchRange &range,
                     double sigma, double *limit = nullptr);
