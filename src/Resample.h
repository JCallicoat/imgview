#pragma once

#include <QImage>
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

// Invert the colour channels in place (v -> 255 - v for opaque pixels),
// leaving alpha unchanged. For translucent pixels the inversion is done on
// the un-premultiplied colour, i.e. premultiplied c -> alpha - c.
// `img` must be Format_ARGB32_Premultiplied.
void invertColors(QImage &img);
