#include "ScaledImageLabel.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 10.0;
constexpr double kZoomStep = 1.1;
// Renders slower than this use a quick QPainter preview while interacting.
constexpr qint64 kInteractiveBudgetMs = 16;
// How long the view must be still before the full (e.g. bicubic) render.
constexpr int kRefineDelayMs = 120;
} // namespace

ScaledImageLabel::ScaledImageLabel(QWidget *parent) : QLabel(parent) {
  setAlignment(Qt::AlignCenter);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  setFocusPolicy(Qt::StrongFocus);

  m_refineTimer.setSingleShot(true);
  m_refineTimer.setInterval(kRefineDelayMs);
  connect(&m_refineTimer, &QTimer::timeout, this, [this] {
    m_interacting = false;
    update();
  });
}

QString ScaledImageLabel::interpolationName(Interpolation mode) {
  switch (mode) {
  case Interpolation::Nearest:
    return "nearest";
  case Interpolation::Bicubic:
    return "bicubic";
  case Interpolation::Lanczos3:
    return "lanczos3";
  }
  return {};
}

QString ScaledImageLabel::resamplingName() const {
  if (!m_original.isNull() && currentScale() * devicePixelRatioF() < 1.0)
    return "area";
  return interpolationName(m_interpolation);
}

bool ScaledImageLabel::isDownscaled() const {
  return currentScale() * devicePixelRatioF() <= 1.0;
}

void ScaledImageLabel::setInterpolation(Interpolation mode) {
  if (mode == m_interpolation)
    return;
  m_interpolation = mode;
  m_viewportCache = QImage();
  m_lastRenderMs = 0; // re-measure for the new mode
  update();
  emit viewChanged();
}

void ScaledImageLabel::setInverted(bool inverted) {
  if (inverted == m_inverted)
    return;
  m_inverted = inverted;
  rebuildToneLut();
}

void ScaledImageLabel::setGammaStep(int step) {
  step = qBound(-kMaxGammaStep, step, kMaxGammaStep);
  if (step == m_gammaStep)
    return;
  m_gammaStep = step;
  rebuildToneLut();
}

double ScaledImageLabel::gamma() const {
  return std::pow(2.0, m_gammaStep / 4.0);
}

void ScaledImageLabel::setSaturationStep(int step) {
  step = qBound(kMinSaturationStep, step, kMaxSaturationStep);
  if (step == m_saturationStep)
    return;
  m_saturationStep = step;
  rebuildToneLut();
}

void ScaledImageLabel::setSaturationMode(SaturationMode mode) {
  if (mode == m_saturationMode)
    return;
  m_saturationMode = mode;
  rebuildToneLut();
}

QString ScaledImageLabel::saturationModeName() const {
  return m_saturationMode == SaturationMode::Vibrance ? "vibrance"
                                                      : "saturation";
}

void ScaledImageLabel::setFalseColor(FalseColor map) {
  if (map == m_falseColor)
    return;
  m_falseColor = map;
  rebuildToneLut();
}

QString ScaledImageLabel::falseColorName(FalseColor map) {
  switch (map) {
  case FalseColor::Inferno: return "inferno";
  case FalseColor::Viridis: return "viridis";
  case FalseColor::Off:     break;
  }
  return "off";
}

const ColorMap *ScaledImageLabel::colorMap() const {
  switch (m_falseColor) {
  case FalseColor::Inferno: return &kInfernoMap;
  case FalseColor::Viridis: return &kViridisMap;
  case FalseColor::Off:     break;
  }
  return nullptr;
}

double ScaledImageLabel::saturation() const {
  return m_saturationStep <= 0 ? 1.0 + 0.1 * m_saturationStep
                               : std::pow(2.0, m_saturationStep / 4.0);
}

// Called whenever a tone setting changes. A future curve type (levels,
// S-curve) would be built here too.
void ScaledImageLabel::rebuildToneLut() {
  m_toneIsIdentity = !m_inverted && m_gammaStep == 0 &&
                     m_saturationStep == 0 && m_falseColor == FalseColor::Off;
  m_toneLut = buildToneLut(m_inverted, gamma());
  m_adjustedCache = QImage();
  update();
  emit viewChanged();
}

// Returns `rendered` untouched when no tone adjustment is active (so the
// true-pixel guarantee holds exactly), otherwise a tone-mapped copy.
const QImage &ScaledImageLabel::forDisplay(const QImage &rendered) {
  if (m_toneIsIdentity)
    return rendered;
  if (m_adjustedCache.isNull() ||
      m_adjustedSourceKey != rendered.cacheKey()) {
    // convertToFormat() is a no-op for the formats used here, and
    // applyTone() then detaches, so `rendered` itself is untouched.
    m_adjustedCache =
        rendered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    applyTone(m_adjustedCache, m_toneLut, saturation(), m_saturationMode,
              colorMap());
    m_adjustedSourceKey = rendered.cacheKey();
  }
  return m_adjustedCache;
}

void ScaledImageLabel::markInteracting() {
  m_interacting = true;
  m_refineTimer.start(); // restarts if already running
}

void ScaledImageLabel::setFrames(QList<QImage> frames) {
  QLabel::setText(QString()); // drop any earlier "Failed to load" message
  // Stored as decoded if deeper than 8 bits or with alpha (see m_source),
  // otherwise as ARGB32_Premultiplied, which keeps the values. Converted in
  // place, so with the frames moved in each decoded frame is freed as soon
  // as it's converted, rather than all of them being held twice.
  for (QImage &image : frames)
    if (!isDeepFormat(image.format()) && !image.hasAlphaChannel())
      image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  m_frames = std::move(frames);
  m_stretchHistogram.clear();
  m_sharedHighPassLimit = 0.0;
  showFrame(0);
  updateProcessed();
  m_viewportCache = QImage();
  resetView();
}

void ScaledImageLabel::showFrame(int index) {
  m_frameIndex = index;
  m_source = m_frames.value(index);
  m_deep = isDeepFormat(m_source.format());
  m_hasAlpha = m_source.hasAlphaChannel();
  m_gray = m_source.isGrayscale(); // scans 32-bit images, stopping at a colour
}

void ScaledImageLabel::setFrame(int index) {
  if (index < 0 || index >= frameCount() || index == m_frameIndex)
    return;
  const QSize oldSize = m_source.size();
  showFrame(index);
  reprocess();
  if (m_source.size() != oldSize) // pages of a TIFF can differ in size
    resetView();
}

// After a change to the frame shown or the stretch or high-pass settings:
// rebuild m_original and drop the renders made from the old one.
void ScaledImageLabel::reprocess() {
  updateProcessed();
  m_scaledCache = QImage();
  m_viewportCache = QImage();
  update();
  emit viewChanged();
}

// Rebuild m_original from m_source for the current stretch and high-pass
// settings.
void ScaledImageLabel::updateProcessed() {
  m_stretchRange = StretchRange();
  m_highPassLimit = 0.0;
  if (m_source.isNull()) {
    m_original = QImage();
    return;
  }
  if (m_stretchStep < 0) {
    m_stretchRange = squashRange(squashPercent() / 100.0);
  } else if (m_stretchStep > 0) {
    if (m_stretchHistogram.empty()) {
      // All frames pooled, so every frame gets the same mapping.
      QGuiApplication::setOverrideCursor(Qt::WaitCursor);
      for (const QImage &frame : std::as_const(m_frames)) {
        const StretchHistogram hist = stretchHistogram(frame);
        if (m_stretchHistogram.empty())
          m_stretchHistogram = hist;
        else
          for (size_t v = 0; v < hist.size(); ++v)
            m_stretchHistogram[v] += hist[v];
      }
      QGuiApplication::restoreOverrideCursor();
    }
    m_stretchRange = findStretchRange(m_stretchHistogram, stretchClipPercent());
  }
  if (m_highPassStep != 0) {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor); // can take a second
    m_original = applyHighPass(m_source, m_stretchRange, highPassRadius(),
                               &m_highPassLimit, m_sharedHighPassLimit);
    if (frameCount() > 1)
      m_sharedHighPassLimit = m_highPassLimit;
    QGuiApplication::restoreOverrideCursor();
  } else if (m_stretchStep != 0) {
    m_original = applyStretch(m_source, m_stretchRange);
  } else {
    m_original = m_source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  }
}

double ScaledImageLabel::highPassRadius() const {
  return m_highPassStep > 0 ? double(1 << (m_highPassStep - 1)) : 0.0;
}

void ScaledImageLabel::setHighPassStep(int step) {
  step = qBound(0, step, kMaxHighPassStep);
  if (step == m_highPassStep)
    return;
  m_highPassStep = step;
  m_sharedHighPassLimit = 0.0; // a new gain for the new values
  reprocess();
}

double ScaledImageLabel::stretchClipPercent() const {
  static constexpr double kClipPercent[kMaxStretchStep] = {
      0, 0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0};
  return m_stretchStep > 0 ? kClipPercent[m_stretchStep - 1] : 0.0;
}

int ScaledImageLabel::squashPercent() const {
  return m_stretchStep < 0 ? 100 + 20 * m_stretchStep : 100; // 80 % to 20 %
}

QString ScaledImageLabel::stretchName() const {
  if (m_stretchStep > 0)
    return QString("stretch %1%").arg(stretchClipPercent());
  if (m_stretchStep < 0)
    return QString("squash %1%").arg(squashPercent());
  return "off";
}

void ScaledImageLabel::setStretchStep(int step) {
  step = qBound(kMinStretchStep, step, kMaxStretchStep);
  if (step == m_stretchStep)
    return;
  m_stretchStep = step;
  m_sharedHighPassLimit = 0.0; // a new gain for the new values
  reprocess();
}

void ScaledImageLabel::clearImage() {
  m_original = QImage();
  m_source = QImage();
  m_frames.clear();
  m_frameIndex = 0;
  m_stretchHistogram.clear();
  m_sharedHighPassLimit = 0.0;
  m_stretchRange = StretchRange();
  m_deep = m_gray = m_hasAlpha = false;
  m_viewportCache = QImage();
  m_adjustedCache = QImage();
  resetView();
}

void ScaledImageLabel::resetView() {
  m_zoomFactor = 1.0;
  m_manualZoom = false;
  m_dragging = false;
  m_scaledCache = QImage();
  clampOffset();
  updateCursor();
  update();
  emit viewChanged();
}

double ScaledImageLabel::fitScale() const {
  if (m_original.isNull() || width() <= 0 || height() <= 0)
    return 1.0;
  return qMin(double(width()) / m_original.width(),
              double(height()) / m_original.height());
}

double ScaledImageLabel::currentScale() const {
  return m_manualZoom ? m_zoomFactor : fitScale();
}

QSizeF ScaledImageLabel::scaledSize() const {
  return QSizeF(m_original.size()) * currentScale();
}

bool ScaledImageLabel::canPan() const {
  if (m_original.isNull())
    return false;
  const QSizeF s = scaledSize();
  return s.width() > width() || s.height() > height();
}

// Keep the viewport inside the image: center along any axis where the image
// is smaller than the widget, otherwise don't allow scrolling past an edge.
void ScaledImageLabel::clampOffset() {
  if (m_original.isNull())
    return;
  const QSizeF s = scaledSize();
  const double w = width(), h = height();

  if (s.width() <= w)
    m_offset.setX((w - s.width()) / 2.0);
  else
    m_offset.setX(qBound(w - s.width(), m_offset.x(), 0.0));

  if (s.height() <= h)
    m_offset.setY((h - s.height()) / 2.0);
  else
    m_offset.setY(qBound(h - s.height(), m_offset.y(), 0.0));
}

// Zoom so the image point under `anchor` (widget coordinates) stays under it.
void ScaledImageLabel::zoomAt(double newScale, const QPointF &anchor) {
  if (m_original.isNull())
    return;

  const double oldScale = currentScale();
  // Always allow zooming out at least as far as fit-to-window.
  newScale = qBound(qMin(kMinZoom, fitScale()), newScale, kMaxZoom);

  const QPointF imagePoint = (anchor - m_offset) / oldScale;
  m_zoomFactor = newScale;
  m_manualZoom = true;
  m_offset = anchor - imagePoint * newScale;

  markInteracting();
  clampOffset();
  updateCursor();
  update();
  emit viewChanged();
}

void ScaledImageLabel::updateCursor() {
  if (m_dragging)
    setCursor(Qt::ClosedHandCursor);
  else if (canPan())
    setCursor(Qt::OpenHandCursor);
  else
    unsetCursor();
}

void ScaledImageLabel::paintEvent(QPaintEvent *event) {
  if (m_original.isNull()) {
    QLabel::paintEvent(event); // e.g. the "Failed to load" text
    return;
  }

  QPainter painter(this);
  const double scale = currentScale();
  const qreal dpr = devicePixelRatioF();

  if (isDownscaled()) {
    // Zoomed out: cache a smoothly downscaled copy (never larger than the
    // original). QImage::scaled() filters better than the painter's
    // bilinear sampling when shrinking a lot.
    m_viewportCache = QImage();
    const QSize px = (QSizeF(m_original.size()) * scale * dpr).toSize();
    if (m_scaledCache.isNull() || m_scaledCache.size() != px) {
      m_scaledCache = m_original.scaled(px, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);
      m_scaledCache.setDevicePixelRatio(dpr);
    }
    painter.drawImage(m_offset, forDisplay(m_scaledCache));
  } else {
    // Zoomed in: resample only the visible part of the original, at
    // device-pixel resolution. Memory use doesn't grow with zoom.
    m_scaledCache = QImage();
    const QRectF visible = visibleArea();
    if (visible.isEmpty())
      return;

    // Visible area in whole device pixels.
    const int dx0 = int(std::floor(visible.left() * dpr));
    const int dy0 = int(std::floor(visible.top() * dpr));
    const int dx1 = int(std::ceil(visible.right() * dpr));
    const int dy1 = int(std::ceil(visible.bottom() * dpr));
    const QPointF pos(dx0 / dpr, dy0 / dpr); // logical draw position
    const QPointF origin = (pos - m_offset) / scale; // in source pixels
    const double step = 1.0 / (scale * dpr);
    const QSize outSize(dx1 - dx0, dy1 - dy0);

    const bool cacheValid =
        !m_viewportCache.isNull() && m_viewportCache.size() == outSize &&
        m_viewportCachePos == pos && m_viewportCacheOrigin == origin &&
        m_viewportCacheStep == step;

    if (!cacheValid && m_interacting &&
        m_lastRenderMs > kInteractiveBudgetMs) {
      // Quick preview; m_refineTimer repaints with the full render shortly.
      // Only smooth it in the interpolating modes, so nearest never shows
      // blending.
      const bool smooth = m_interpolation != Interpolation::Nearest;
      const QRectF source((visible.topLeft() - m_offset) / scale,
                          visible.size() / scale);
      if (m_toneIsIdentity) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        painter.drawImage(visible, m_original, source);
      } else {
        // Draw the preview off-screen so the tone adjustments can be applied.
        QImage preview(outSize, QImage::Format_ARGB32_Premultiplied);
        preview.setDevicePixelRatio(dpr);
        preview.fill(Qt::transparent);
        QPainter p(&preview);
        p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        p.translate(-pos);
        p.drawImage(visible, m_original, source);
        p.end();
        applyTone(preview, m_toneLut, saturation(), m_saturationMode,
                  colorMap());
        painter.drawImage(pos, preview);
      }
      return;
    }

    if (!cacheValid) {
      QElapsedTimer timer;
      timer.start();
      switch (m_interpolation) {
      case Interpolation::Nearest:
        m_viewportCache = resampleNearest(m_original, outSize, origin, step);
        break;
      case Interpolation::Bicubic:
        m_viewportCache = resampleBicubic(m_original, outSize, origin, step);
        break;
      case Interpolation::Lanczos3:
        m_viewportCache = resampleLanczos3(m_original, outSize, origin, step);
        break;
      }
      m_viewportCache.setDevicePixelRatio(dpr);
      m_viewportCachePos = pos;
      m_viewportCacheOrigin = origin;
      m_viewportCacheStep = step;
      m_lastRenderMs = timer.elapsed();
    }
    painter.drawImage(pos, forDisplay(m_viewportCache));
  }
}

// The part of the widget the image covers, in logical pixels.
QRectF ScaledImageLabel::visibleArea() const {
  return QRectF(m_offset, scaledSize()).intersected(QRectF(rect()));
}

QRectF ScaledImageLabel::visibleSourceRect() const {
  if (m_original.isNull())
    return {};
  const QRectF visible = visibleArea();
  const double scale = currentScale();
  return QRectF((visible.topLeft() - m_offset) / scale, visible.size() / scale);
}

QImage ScaledImageLabel::renderVisible() {
  if (m_original.isNull())
    return {};
  // Use the full-quality render, not a quick preview from a recent drag, and
  // let paintEvent() bring the render caches up to date for the current view.
  m_refineTimer.stop();
  m_interacting = false;
  repaint();

  const double scale = currentScale();
  const qreal dpr = devicePixelRatioF();
  QImage shot;
  if (isDownscaled()) {
    // Zoomed out: the cache holds the whole downscaled image; cut out the
    // part that is on screen.
    if (m_scaledCache.isNull())
      return {};
    const QImage &full = forDisplay(m_scaledCache);
    const QRectF visible = visibleArea();
    const QRect src = QRectF((visible.topLeft() - m_offset) * dpr,
                             visible.size() * dpr)
                          .toAlignedRect()
                          .intersected(full.rect());
    shot = full.copy(src);
  } else {
    // Zoomed in: the cache is exactly the visible area in device pixels.
    if (m_viewportCache.isNull())
      return {};
    shot = forDisplay(m_viewportCache).copy();
  }
  shot.setDevicePixelRatio(1.0);
  return shot;
}

void ScaledImageLabel::setPixelTips(bool on) {
  m_pixelTips = on;
  if (!on)
    QToolTip::hideText();
}

// e.g. "x 512, y 300" over "RGB 12, 34, 56", "value 87" for a grey image,
// with "A 77" added for an image with alpha. The values are those decoded
// from the file, before the contrast stretch, resampling and tone
// adjustments; a deeper image
// shows 16-bit values.
QString ScaledImageLabel::pixelTipText(const QPoint &pixel) const {
  const QRgba64 c = m_source.pixelColor(pixel).rgba64();
  auto value = [this](quint16 v16, quint8 v8) {
    return QString::number(m_deep ? v16 : v8);
  };
  QString text = QString("x %1, y %2\n").arg(pixel.x()).arg(pixel.y());
  if (m_gray)
    text += "value " + value(c.red(), c.red8());
  else
    text += QString("RGB %1, %2, %3")
                .arg(value(c.red(), c.red8()), value(c.green(), c.green8()),
                     value(c.blue(), c.blue8()));
  if (m_hasAlpha)
    text += "  A " + value(c.alpha(), c.alpha8());
  if (m_deep)
    text += " (16-bit)";
  return text;
}

// Qt sends a ToolTip event once the pointer has rested for the platform's
// hover delay. The tip is tied to the screen area of the pixel it describes,
// so it closes when the pointer moves off that pixel; while a tip is up, Qt
// shows the next one with almost no delay, so it then follows the pointer.
// A key press closes it, so panning or zooming can't leave a stale value.
bool ScaledImageLabel::event(QEvent *event) {
  if (event->type() != QEvent::ToolTip || !m_pixelTips)
    return QLabel::event(event);
  const auto *help = static_cast<QHelpEvent *>(event);
  const double scale = currentScale();
  const QPointF p = (QPointF(help->pos()) - m_offset) / scale;
  const QPoint pixel(int(std::floor(p.x())), int(std::floor(p.y())));
  if (m_original.isNull() || m_dragging || !m_original.rect().contains(pixel)) {
    QToolTip::hideText();
    return true;
  }
  const QRect area = QRectF(m_offset + QPointF(pixel) * scale,
                            QSizeF(scale, scale))
                         .toAlignedRect()
                         .intersected(rect());
  QToolTip::showText(help->globalPos(), pixelTipText(pixel), this, area);
  return true;
}

void ScaledImageLabel::resizeEvent(QResizeEvent *event) {
  QLabel::resizeEvent(event);
  // Keep the same part of the image at the center of the view.
  if (event->oldSize().isValid()) {
    const QSizeF d = QSizeF(event->size() - event->oldSize()) / 2.0;
    m_offset += QPointF(d.width(), d.height());
  }
  markInteracting();
  clampOffset();
  updateCursor();
  if (!m_manualZoom)
    emit viewChanged(); // fit-to-window scale follows the widget size
}

void ScaledImageLabel::wheelEvent(QWheelEvent *event) {
  const int dy = event->angleDelta().y();
  if (m_original.isNull() || dy == 0) {
    QLabel::wheelEvent(event);
    return;
  }
  // 120 units = one notch; fractional deltas from smooth wheels/touchpads
  // give proportionally smaller steps.
  const double factor = std::pow(kZoomStep, dy / 120.0);
  zoomAt(currentScale() * factor, event->position());
  event->accept();
}

void ScaledImageLabel::zoomIn() {
  zoomAt(currentScale() * kZoomStep, QRectF(rect()).center());
}

void ScaledImageLabel::zoomOut() {
  zoomAt(currentScale() / kZoomStep, QRectF(rect()).center());
}

void ScaledImageLabel::resetZoom() { resetView(); }

void ScaledImageLabel::setScale(double scale, const QPointF &anchor) {
  zoomAt(scale, anchor);
}

void ScaledImageLabel::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && canPan()) {
    m_dragging = true;
    m_lastMousePos = event->position();
    updateCursor();
    event->accept();
    return;
  }
  QLabel::mousePressEvent(event);
}

// Move the image by `delta` logical pixels (positive = image moves
// right/down), clamped to the image edges.
void ScaledImageLabel::panBy(const QPointF &delta) {
  m_offset += delta;
  markInteracting();
  clampOffset();
  update();
}

// Arrow keys pan while this view has focus and the image is larger than the
// view along that axis. They move the view like scrollbars do (Right shows
// more of the right side): 10% of the view per press, or exactly one image
// pixel with Shift for stepping across fine detail. An arrow with nothing to
// pan goes up to the window, where Left/Right switch images if the image fits
// the view both ways.
void ScaledImageLabel::keyPressEvent(QKeyEvent *event) {
  int dx = 0, dy = 0;
  switch (event->key()) {
  case Qt::Key_Left:  dx = -1; break;
  case Qt::Key_Right: dx = 1;  break;
  case Qt::Key_Up:    dy = -1; break;
  case Qt::Key_Down:  dy = 1;  break;
  default:
    QLabel::keyPressEvent(event);
    return;
  }
  const QSizeF s = m_original.isNull() ? QSizeF() : scaledSize();
  const bool canPanAxis = dx != 0 ? s.width() > width() : s.height() > height();
  if (!canPanAxis) {
    QLabel::keyPressEvent(event);
    return;
  }
  QPointF step(width() * 0.1, height() * 0.1);
  if (event->modifiers() & Qt::ShiftModifier)
    step = QPointF(1.0, 1.0) * qMax(currentScale(), 1.0);
  panBy(QPointF(-dx * step.x(), -dy * step.y()));
  event->accept();
}

void ScaledImageLabel::mouseMoveEvent(QMouseEvent *event) {
  if (m_dragging) {
    const QPointF pos = event->position();
    panBy(pos - m_lastMousePos);
    m_lastMousePos = pos;
    event->accept();
    return;
  }
  QLabel::mouseMoveEvent(event);
}

void ScaledImageLabel::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && m_dragging) {
    m_dragging = false;
    updateCursor();
    event->accept();
    return;
  }
  QLabel::mouseReleaseEvent(event);
}
