#include "Resample.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace {

// Keys cubic convolution kernel with a = -0.5 (Catmull-Rom spline).
inline float cubicWeight(float t) {
  t = std::fabs(t);
  if (t < 1.0f)
    return (1.5f * t - 2.5f) * t * t + 1.0f;
  if (t < 2.0f)
    return ((-0.5f * t + 2.5f) * t - 4.0f) * t + 2.0f;
  return 0.0f;
}

// The four source indices and weights contributing to one output position.
struct Taps {
  int idx[4];
  float w[4];
};

// Precompute taps for every output column (or row). Indices past the image
// edge are clamped, which extends the border pixels outward.
std::vector<Taps> makeTaps(int count, double origin, double step,
                           int srcLen) {
  std::vector<Taps> taps(count);
  for (int i = 0; i < count; ++i) {
    const double u = origin + (i + 0.5) * step - 0.5;
    const double base = std::floor(u);
    const float f = float(u - base);
    for (int k = 0; k < 4; ++k) {
      const int idx = int(base) - 1 + k;
      taps[i].idx[k] = std::clamp(idx, 0, srcLen - 1);
      taps[i].w[k] = cubicWeight(f - float(k - 1));
    }
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

QImage resampleBicubic(const QImage &src, const QSize &outSize,
                       const QPointF &origin, double step) {
  if (src.isNull() || outSize.isEmpty())
    return {};
  Q_ASSERT(src.format() == QImage::Format_ARGB32_Premultiplied);

  const int outW = outSize.width(), outH = outSize.height();
  const std::vector<Taps> xt = makeTaps(outW, origin.x(), step, src.width());
  const std::vector<Taps> yt = makeTaps(outH, origin.y(), step, src.height());

  // Only the source rows some output row actually reads.
  int rowMin = src.height(), rowMax = -1;
  for (const Taps &t : yt) {
    rowMin = std::min(rowMin, t.idx[0]);
    rowMax = std::max(rowMax, t.idx[3]);
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
        for (int k = 0; k < 4; ++k) {
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

  // Pass 2: vertical filter into the output. Cubic weights go negative, so
  // results can overshoot; clamp to [0, 255] and keep colour <= alpha so the
  // premultiplied data stays valid.
  QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
  uchar *const bits = out.bits(); // see resampleNearest()
  const qsizetype bpl = out.bytesPerLine();
  parallelRows(outH, [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      const Taps &t = yt[y];
      const float *r[4];
      for (int k = 0; k < 4; ++k)
        r[k] = tmp.data() + size_t(t.idx[k] - rowMin) * outW * 4;
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      for (int x = 0; x < outW; ++x) {
        const size_t o = size_t(x) * 4;
        float c[4];
        for (int ch = 0; ch < 4; ++ch)
          c[ch] = t.w[0] * r[0][o + ch] + t.w[1] * r[1][o + ch] +
                  t.w[2] * r[2][o + ch] + t.w[3] * r[3][o + ch];
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

void invertColors(QImage &img) {
  if (img.isNull())
    return;
  Q_ASSERT(img.format() == QImage::Format_ARGB32_Premultiplied);
  uchar *const bits = img.bits(); // see resampleNearest()
  const qsizetype bpl = img.bytesPerLine();
  const int w = img.width();
  parallelRows(img.height(), [&](int b, int e) {
    for (int y = b; y < e; ++y) {
      QRgb *line = reinterpret_cast<QRgb *>(bits + y * bpl);
      for (int x = 0; x < w; ++x) {
        const QRgb p = line[x];
        const uint a = p >> 24;
        // Premultiplied channels are <= alpha, so alpha - c stays valid.
        line[x] = (a << 24) | ((a - ((p >> 16) & 0xff)) << 16) |
                  ((a - ((p >> 8) & 0xff)) << 8) | (a - (p & 0xff));
      }
    }
  });
}
