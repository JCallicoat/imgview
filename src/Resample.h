#pragma once

#include <QImage>
#include <array>
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

// Per-channel tone mapping table: output = lut[input] for each of R, G, B.
using ToneLut = std::array<quint8, 256>;

// Build the display tone table: optional inversion (v -> 255 - v), then a
// gamma curve out = 255 * (in / 255)^gamma. gamma > 1 darkens midtones and
// stretches contrast among bright values; gamma < 1 the opposite. Black and
// white map to themselves (or each other when inverted), so nothing clips.
ToneLut buildToneLut(bool invert, double gamma);

// Adjust the colour channels in place, leaving alpha unchanged: first scale
// saturation linearly around the pixel's Rec. 709 luma L,
//   out = L + saturation * (in - L),
// on the stored values (saturation 1 = unchanged, 0 = grey, > 1 more
// saturated), then map each channel through `lut`. Above 1 a channel can
// overshoot [0, 255] and is clamped. Translucent pixels are un-premultiplied,
// adjusted and re-premultiplied. `img` must be Format_ARGB32_Premultiplied.
void applyTone(QImage &img, const ToneLut &lut, double saturation);
