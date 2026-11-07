#include "Resample.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Keys cubic convolution kernel with a = -0.5 (Catmull-Rom spline).
float cubicWeight(double t) {
  t = std::fabs(t);
  if (t < 1.0)
    return float((1.5 * t - 2.5) * t * t + 1.0);
  if (t < 2.0)
    return float(((-0.5 * t + 2.5) * t - 4.0) * t + 2.0);
  return 0.0f;
}

double sinc(double x) {
  if (x == 0.0)
    return 1.0;
  constexpr double kPi = 3.14159265358979323846;
  const double px = kPi * x;
  return std::sin(px) / px;
}

// Lanczos kernel with three lobes: sinc(t) * sinc(t / 3) for |t| < 3.
float lanczos3Weight(double t) {
  return std::fabs(t) < 3.0 ? float(sinc(t) * sinc(t / 3.0)) : 0.0f;
}

// The source indices and weights contributing to one output position: 2 *
// radius of them, for a kernel that is zero from `radius` on.
constexpr int kMaxTaps = 6;
struct Taps {
  int idx[kMaxTaps];
  float w[kMaxTaps];
};

// Precompute taps for every output column (or row). Indices past the image
// edge are clamped, which extends the border pixels outward. The weights are
// normalized to sum to 1 (Lanczos weights don't quite), so flat areas stay
// exactly flat.
std::vector<Taps> makeTaps(int count, double origin, double step, int srcLen,
                           int radius, float (*kernel)(double)) {
  const int n = 2 * radius;
  std::vector<Taps> taps(count);
  for (int i = 0; i < count; ++i) {
    const double u = origin + (i + 0.5) * step - 0.5;
    const double base = std::floor(u);
    const double f = u - base;
    float sum = 0.0f;
    for (int k = 0; k < n; ++k) {
      const int offset = k - (radius - 1); // source index relative to base
      taps[i].idx[k] = std::clamp(int(base) + offset, 0, srcLen - 1);
      taps[i].w[k] = kernel(f - offset);
      sum += taps[i].w[k];
    }
    for (int k = 0; k < n; ++k)
      taps[i].w[k] /= sum;
  }
  return taps;
}

// Run fn(begin, end) over [0, count) split across hardware threads.
template <typename Fn> void parallelRows(int count, Fn fn) {
  const int threads = std::clamp(
      int(std::thread::hardware_concurrency()), 1, std::max(1, count / 32));
  if (threads <= 1) {
    fn(0, count);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (int t = 0; t < threads; ++t) {
    const int b = count * t / threads, e = count * (t + 1) / threads;
    pool.emplace_back([=] { fn(b, e); });
  }
  for (auto &th : pool)
    th.join();
}

// Source index under the center of each output pixel.
std::vector<int> nearestIndices(int count, double origin, double step,
                                int srcLen) {
  std::vector<int> idx(count);
  for (int i = 0; i < count; ++i)
    idx[i] = std::clamp(int(std::floor(origin + (i + 0.5) * step)), 0,
                        srcLen - 1);
  return idx;
}

} // namespace

QImage resampleNearest(const QImage &src, const QSize &outSize,
                       const QPointF &origin, double step) {
  if (src.isNull() || outSize.isEmpty())
    return {};
  Q_ASSERT(src.format() == QImage::Format_ARGB32_Premultiplied);

  const int outW = outSize.width(), outH = outSize.height();
  const std::vector<int> xs =
      nearestIndices(outW, origin.x(), step, src.width());
  const std::vector<int> ys =
      nearestIndices(outH, origin.y(), step, src.height());

  QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
  // Take the pixel pointer once: QImage::scanLine() detaches, which isn't
  // safe to call from several threads.
  uchar *const bits = out.bits();
  const qsizetype bpl = out.bytesPerLine();
  parallelRows(outH, [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      // Consecutive output rows often map to the same source row.
      if (y > b && ys[y] == ys[y - 1]) {
        std::copy_n(reinterpret_cast<const QRgb *>(bits + (y - 1) * bpl),
                    outW, line);
        continue;
      }
      const QRgb *srcLine =
          reinterpret_cast<const QRgb *>(src.constScanLine(ys[y]));
      for (int x = 0; x < outW; ++x)
        line[x] = srcLine[xs[x]];
    }
  });
  return out;
}

namespace {

// Separable convolution with `kernel` (zero from `radius` on), shared by the
// bicubic and Lanczos resamplers.
QImage resampleSeparable(const QImage &src, const QSize &outSize,
                         const QPointF &origin, double step, int radius,
                         float (*kernel)(double)) {
  if (src.isNull() || outSize.isEmpty())
    return {};
  Q_ASSERT(src.format() == QImage::Format_ARGB32_Premultiplied);
  Q_ASSERT(2 * radius <= kMaxTaps);

  const int n = 2 * radius;
  const int outW = outSize.width(), outH = outSize.height();
  const std::vector<Taps> xt =
      makeTaps(outW, origin.x(), step, src.width(), radius, kernel);
  const std::vector<Taps> yt =
      makeTaps(outH, origin.y(), step, src.height(), radius, kernel);

  // Only the source rows some output row actually reads.
  int rowMin = src.height(), rowMax = -1;
  for (const Taps &t : yt) {
    rowMin = std::min(rowMin, t.idx[0]);
    rowMax = std::max(rowMax, t.idx[n - 1]);
  }
  const int rows = rowMax - rowMin + 1;

  // Pass 1: horizontal filter of each needed source row -> float rows of
  // width outW (4 channels: b, g, r, a in QRgb byte order).
  std::vector<float> tmp(size_t(rows) * outW * 4);
  parallelRows(rows, [&](int b, int e) {
    for (int r = b; r < e; ++r) {
      const QRgb *line =
          reinterpret_cast<const QRgb *>(src.constScanLine(rowMin + r));
      float *dst = tmp.data() + size_t(r) * outW * 4;
      for (int x = 0; x < outW; ++x) {
        const Taps &t = xt[x];
        float c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        for (int k = 0; k < n; ++k) {
          const QRgb p = line[t.idx[k]];
          const float w = t.w[k];
          c0 += w * float(p & 0xff);
          c1 += w * float((p >> 8) & 0xff);
          c2 += w * float((p >> 16) & 0xff);
          c3 += w * float(p >> 24);
        }
        dst[0] = c0, dst[1] = c1, dst[2] = c2, dst[3] = c3;
        dst += 4;
      }
    }
  });

  // Pass 2: vertical filter into the output. The weights go negative, so
  // results can overshoot; clamp to [0, 255] and keep colour <= alpha so the
  // premultiplied data stays valid.
  QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
  uchar *const bits = out.bits(); // see resampleNearest()
  const qsizetype bpl = out.bytesPerLine();
  parallelRows(outH, [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      const Taps &t = yt[y];
      const float *r[kMaxTaps];
      for (int k = 0; k < n; ++k)
        r[k] = tmp.data() + size_t(t.idx[k] - rowMin) * outW * 4;
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      for (int x = 0; x < outW; ++x) {
        const size_t o = size_t(x) * 4;
        float c[4] = {0, 0, 0, 0};
        for (int k = 0; k < n; ++k)
          for (int ch = 0; ch < 4; ++ch)
            c[ch] += t.w[k] * r[k][o + ch];
        const int a = std::clamp(int(c[3] + 0.5f), 0, 255);
        const int bl = std::clamp(int(c[0] + 0.5f), 0, a);
        const int g = std::clamp(int(c[1] + 0.5f), 0, a);
        const int rd = std::clamp(int(c[2] + 0.5f), 0, a);
        line[x] = (uint(a) << 24) | (uint(rd) << 16) | (uint(g) << 8) |
                  uint(bl);
      }
    }
  });
  return out;
}

} // namespace

QImage resampleBicubic(const QImage &src, const QSize &outSize,
                       const QPointF &origin, double step) {
  return resampleSeparable(src, outSize, origin, step, 2, cubicWeight);
}

QImage resampleLanczos3(const QImage &src, const QSize &outSize,
                        const QPointF &origin, double step) {
  return resampleSeparable(src, outSize, origin, step, 3, lanczos3Weight);
}

ToneLut buildToneLut(bool invert, double gamma) {
  ToneLut lut;
  for (int v = 0; v < 256; ++v) {
    const double x = (invert ? 255 - v : v) / 255.0;
    lut[v] = quint8(std::lround(255.0 * std::pow(x, gamma)));
  }
  return lut;
}

void applyTone(QImage &img, const ToneLut &lut, double saturation,
               SaturationMode mode, const ColorMap *colorMap) {
  if (img.isNull())
    return;
  Q_ASSERT(img.format() == QImage::Format_ARGB32_Premultiplied);
  // Saturation and the Rec. 709 luma weights (0.2126, 0.7152, 0.0722) in
  // 1/256ths; the weights add up to 256.
  const int s = int(std::lround(std::max(saturation, 0.0) * 256));
  constexpr uint kR = 54, kG = 183, kB = 19;
  uchar *const bits = img.bits(); // see resampleNearest()
  const qsizetype bpl = img.bytesPerLine();
  const int w = img.width();
  parallelRows(img.height(), [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      for (int x = 0; x < w; ++x) {
        const QRgb p = line[x];
        const uint a = p >> 24;
        if (a == 0) // fully transparent pixels stay as they are
          continue;
        uint r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
        if (a != 255) { // translucent: adjust the straight colour
          auto straight = [a](uint c) {
            return std::min((c * 255 + a / 2) / a, 255u);
          };
          r = straight(r), g = straight(g), bl = straight(bl);
        }
        if (s != 256 && !colorMap) {
          int ps = s; // this pixel's factor, in 1/256ths
          if (mode == SaturationMode::Vibrance) {
            // Weight the change by 1 - S, S = HSV saturation in 1/256ths.
            const int mx = int(std::max({r, g, bl}));
            const int mn = int(std::min({r, g, bl}));
            const int sat = mx == 0 ? 0 : (mx - mn) * 256 / mx;
            ps = 256 + (s - 256) * (256 - sat) / 256;
          }
          // L + ps * (c - L), rounded. It stays within [0, 255] for ps <= 1;
          // above that it can overshoot and is clamped.
          const int l = int((kR * r + kG * g + kB * bl + 128) >> 8);
          auto saturate = [l, ps](uint c) {
            const int v = l * 256 + (int(c) - l) * ps + 128;
            return v <= 0 ? 0u : std::min(uint(v) >> 8, 255u);
          };
          r = saturate(r), g = saturate(g), bl = saturate(bl);
        }
        r = lut[r], g = lut[g], bl = lut[bl];
        if (colorMap) {
          const QRgb c =
              (*colorMap)[(kR * r + kG * g + kB * bl + 128) >> 8];
          r = qRed(c), g = qGreen(c), bl = qBlue(c);
        }
        if (a != 255) {
          auto premultiply = [a](uint c) { return (c * a + 127) / 255; };
          r = premultiply(r), g = premultiply(g), bl = premultiply(bl);
        }
        line[x] = (a << 24) | (r << 16) | (g << 8) | bl;
      }
    }
  });
}

bool isDeepFormat(QImage::Format format) {
  switch (format) {
  case QImage::Format_BGR30:
  case QImage::Format_A2BGR30_Premultiplied:
  case QImage::Format_RGB30:
  case QImage::Format_A2RGB30_Premultiplied:
  case QImage::Format_RGBX64:
  case QImage::Format_RGBA64:
  case QImage::Format_RGBA64_Premultiplied:
  case QImage::Format_Grayscale16:
  case QImage::Format_RGBX16FPx4:
  case QImage::Format_RGBA16FPx4:
  case QImage::Format_RGBA16FPx4_Premultiplied:
  case QImage::Format_RGBX32FPx4:
  case QImage::Format_RGBA32FPx4:
  case QImage::Format_RGBA32FPx4_Premultiplied:
    return true;
  default:
    return false;
  }
}

namespace {
// `img` in a format the stretch code reads directly, without losing
// precision: RGBA64 for a deep image, else (A)RGB32, possibly premultiplied.
QImage stretchInput(const QImage &img) {
  if (isDeepFormat(img.format()))
    return img.convertToFormat(QImage::Format_RGBA64);
  switch (img.format()) {
  case QImage::Format_RGB32:
  case QImage::Format_ARGB32:
  case QImage::Format_ARGB32_Premultiplied:
    return img;
  default:
    return img.convertToFormat(QImage::Format_ARGB32);
  }
}

// Calls fn(r, g, b, a) for each pixel of rows [b, e) of `src` (from
// stretchInput()), with straight (not premultiplied) values: 16-bit for
// RGBA64, else 8-bit.
template <typename Fn>
void forEachPixel(const QImage &src, int b, int e, Fn fn) {
  const int w = src.width();
  if (src.format() == QImage::Format_RGBA64) {
    for (int y = b; y < e; ++y) {
      const auto *line = reinterpret_cast<const QRgba64 *>(src.constScanLine(y));
      for (int x = 0; x < w; ++x)
        fn(line[x].red(), line[x].green(), line[x].blue(), line[x].alpha());
    }
    return;
  }
  const bool premultiplied =
      src.format() == QImage::Format_ARGB32_Premultiplied;
  for (int y = b; y < e; ++y) {
    const auto *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
    for (int x = 0; x < w; ++x) {
      const QRgb p = premultiplied ? qUnpremultiply(line[x]) : line[x];
      fn(uint(qRed(p)), uint(qGreen(p)), uint(qBlue(p)), uint(qAlpha(p)));
    }
  }
}
} // namespace

StretchHistogram stretchHistogram(const QImage &img) {
  StretchHistogram hist(65536, 0);
  const QImage src = stretchInput(img);
  if (src.isNull())
    return hist;
  const uint scale = src.format() == QImage::Format_RGBA64 ? 1 : 257;
  std::mutex mutex;
  parallelRows(src.height(), [&](int b, int e) {
    StretchHistogram local(65536, 0);
    forEachPixel(src, b, e, [&](uint r, uint g, uint bl, uint a) {
      if (a == 0)
        return;
      ++local[r * scale];
      ++local[g * scale];
      ++local[bl * scale];
    });
    const std::lock_guard<std::mutex> lock(mutex);
    for (int v = 0; v < 65536; ++v)
      hist[v] += local[v];
  });
  return hist;
}

StretchRange findStretchRange(const StretchHistogram &hist,
                              double clipPercent) {
  quint64 total = 0;
  for (quint64 n : hist)
    total += n;
  if (total == 0)
    return {};
  // The first value from each end at which more than `clip` values have
  // been passed.
  const double clip = double(total) * clipPercent / 100.0;
  StretchRange range;
  quint64 count = 0;
  for (int v = 0; v < 65536; ++v) {
    count += hist[v];
    if (double(count) > clip) {
      range.black = v;
      break;
    }
  }
  count = 0;
  for (int v = 65535; v >= 0; --v) {
    count += hist[v];
    if (double(count) > clip) {
      range.white = v;
      break;
    }
  }
  if (range.white <= range.black)
    return {};
  return range;
}

QImage applyStretch(const QImage &img, const StretchRange &range) {
  const QImage src = stretchInput(img);
  if (src.isNull())
    return {};
  // Output value for each input value, on the input's scale.
  const bool deep = src.format() == QImage::Format_RGBA64;
  std::vector<quint8> lut(deep ? 65536 : 256);
  const double span = range.white - range.black;
  for (size_t v = 0; v < lut.size(); ++v) {
    const double v16 = deep ? double(v) : v * 257.0;
    lut[v] = quint8(std::clamp(
        std::lround((v16 - range.black) * 255.0 / span), 0L, 255L));
  }
  QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
  if (out.isNull())
    return {};
  uchar *const bits = out.bits();
  const qsizetype bpl = out.bytesPerLine();
  const int w = src.width();
  parallelRows(src.height(), [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      int x = 0;
      forEachPixel(src, y, y + 1, [&](uint r, uint g, uint bl, uint a) {
        const uint a8 = deep ? (a + 128) / 257 : a;
        line[x++] = qPremultiply(qRgba(lut[r], lut[g], lut[bl], int(a8)));
      });
    }
  });
  return out;
}
