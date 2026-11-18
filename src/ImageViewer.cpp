#include "ImageViewer.h"
#include "Resample.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <exiv2/exiv2.hpp>

//-------------------- ScaledImageLabel --------------------

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

//-------------------- ImageViewer --------------------

namespace {
// Natural, locale-independent filename order: runs of digits compare by
// numeric value ("frame2" < "frame10"), everything else case-insensitively.
// (QCollator's numeric mode is silently ignored under the C locale.)
bool naturalLess(const QString &a, const QString &b) {
  qsizetype i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].isDigit() && b[j].isDigit()) {
      qsizetype i2 = i, j2 = j;
      while (i2 < a.size() && a[i2].isDigit()) ++i2;
      while (j2 < b.size() && b[j2].isDigit()) ++j2;
      // Compare the digit runs as numbers: ignore leading zeros, then the
      // longer run is larger, then compare digit by digit.
      qsizetype ia = i, jb = j;
      while (ia < i2 - 1 && a[ia] == u'0') ++ia;
      while (jb < j2 - 1 && b[jb] == u'0') ++jb;
      if (i2 - ia != j2 - jb)
        return i2 - ia < j2 - jb;
      for (; ia < i2; ++ia, ++jb)
        if (a[ia] != b[jb])
          return a[ia] < b[jb];
      if (i2 - i != j2 - j) // same value: fewer leading zeros first
        return i2 - i < j2 - j;
      i = i2;
      j = j2;
    } else {
      const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
      if (ca != cb)
        return ca < cb;
      ++i;
      ++j;
    }
  }
  if (a.size() - i != b.size() - j)
    return a.size() - i < b.size() - j; // shorter remainder first
  return a < b; // differ only by case: keep a stable, total order
}

// "184.9 KiB (189345 bytes)"; plain "N bytes" below 1 KiB.
QString formatFileSize(qint64 bytes) {
  if (bytes <= 0)
    return QStringLiteral("N/A");
  if (bytes < 1024)
    return QString("%1 bytes").arg(bytes);
  // IEC format = powers of 1024 with KiB/MiB/GiB/... suffixes.
  const QString human =
      QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
  return QString("%1 (%2 bytes)").arg(human).arg(bytes);
}

// Metadata table labels: Exiv2's label ("Color Space"), or the tag name
// when it has none.
QString metadataLabel(const Exiv2::Metadatum &kv) {
  const std::string label = kv.tagLabel();
  return QString::fromStdString(label.empty() ? kv.tagName() : label);
}

// "CameraProfiles" -> "Camera Profiles", "RadialDistortParam1" -> "Radial
// Distort Param 1", "LensID" -> "Lens ID", "action" -> "Action": the style of
// Exiv2's own labels, for property names it has no label for.
QString wordsFromName(QString name) {
  static const QRegularExpression boundary(
      "(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])|(?<=[A-Za-z])(?=[0-9])");
  name.replace(boundary, " ");
  if (!name.isEmpty())
    name[0] = name[0].toUpper();
  return name;
}

// Label for one XMP property name: Exiv2's label if it has a real one
// (crs:ToneCurvePV2012 -> "Tone Curve PV 2012"), else the name split into
// words. Exiv2 returns the bare name for properties it doesn't list, and
// throws for namespaces it doesn't know (such as stCamera).
QString xmpPropertyLabel(const std::string &prefix, const QString &name) {
  try {
    const std::string label =
        Exiv2::XmpKey("Xmp." + prefix + "." + name.toStdString()).tagLabel();
    if (!label.empty() && QString::fromStdString(label) != name)
      return QString::fromStdString(label);
  } catch (const Exiv2::Error &) {
    // unknown namespace
  }
  return wordsFromName(name);
}

// Top-level XMP properties get a readable label ("Creator Tool"). Properties
// nested in structs and arrays are labelled by their own name with its
// namespace prefix, e.g. "stCamera:Author" for photoshop:CameraProfiles[1]/
// stCamera:Author; the Tag tooltip shows the full path.
QString metadataLabel(const Exiv2::Xmpdatum &kv) {
  const QString name = QString::fromStdString(kv.tagName());
  const int slash = name.lastIndexOf('/');
  if (slash >= 0)
    return name.mid(slash + 1);
  return xmpPropertyLabel(kv.groupName(), name);
}

// Exiv2 lists each XMP struct or array that holds nested properties as an
// entry of its own, with a placeholder value such as type="Seq", followed by
// the nested properties. Plain-valued arrays (e.g. a tone curve) have their
// own value types and are shown.
bool isMetadataPlaceholder(const Exiv2::Metadatum &) { return false; }
bool isMetadataPlaceholder(const Exiv2::Xmpdatum &kv) {
  if (kv.typeId() != Exiv2::xmpText)
    return false;
  const auto *v = dynamic_cast<const Exiv2::XmpValue *>(&kv.value());
  return v && (v->xmpStruct() != Exiv2::XmpValue::xsNone ||
               v->xmpArrayType() != Exiv2::XmpValue::xaNone);
}
} // namespace

// View setting changes are confirmed briefly in the status bar, where they
// are easier to notice than in the title.
static constexpr int kSettingMessageMs = 2000;

ImageViewer::ImageViewer(const QString &path, QWidget *parent)
    : QMainWindow(parent) {
  resize(800, 600);

  // Hints for the most used keys; ? shows them all. Keys are space-separated
  // (',' and '-' are keys themselves). A minimum width of 1 lets the window
  // shrink below the text width; the text is then clipped.
  // The text is set in updateHints().
  m_hintLabel = new QLabel(this);
  QLabel *hintLabel = m_hintLabel;
  hintLabel->setMinimumWidth(1);
  hintLabel->setContentsMargins(6, 0, 0, 0);
  // Ignored: the hints only get the space the view info label leaves over,
  // so in fullscreen it's the hints that get clipped, never the view info.
  hintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  statusBar()->addWidget(hintLabel, 1);

  // The title's file/view info, on the right of the status bar. A permanent
  // widget, so transient status messages don't hide it. Shown only in
  // fullscreen; see changeEvent().
  m_viewInfoLabel = new QLabel(this);
  m_viewInfoLabel->setTextFormat(Qt::PlainText);
  m_viewInfoLabel->setContentsMargins(16, 0, 6, 0);
  // Never shrink below the text: it's only shown in fullscreen, so it can't
  // force the normal window wider.
  m_viewInfoLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  m_viewInfoLabel->setVisible(false);
  statusBar()->addPermanentWidget(m_viewInfoLabel);

  QWidget *central = new QWidget(this);
  setCentralWidget(central);
  QHBoxLayout *mainLayout = new QHBoxLayout(central);

  m_label = new ScaledImageLabel(this);
  connect(m_label, &ScaledImageLabel::viewChanged, this,
          &ImageViewer::updateWindowTitle);
  updateWindowTitle();
  mainLayout->addWidget(m_label, 1);

  m_playTimer.setSingleShot(true);
  m_playTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_playTimer, &QTimer::timeout, this, &ImageViewer::playNextFrame);

  if (path.isEmpty())
    m_label->setText("No image open.\nPress O to open an image, or Ctrl+V to "
                     "paste one.");
  else
    openPath(path);
}

// File picker for any format Qt can read. Patterns are listed in both
// cases, since some platforms' dialogs match them case-sensitively.
void ImageViewer::showOpenDialog() {
  QStringList patterns;
  for (const QByteArray &fmt : QImageReader::supportedImageFormats()) {
    const QString ext = QString::fromLatin1(fmt);
    patterns << "*." + ext.toLower() << "*." + ext.toUpper();
  }
  patterns.removeDuplicates();
  QString startDir = m_folder;
  if (startDir.isEmpty()) {
    startDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!QDir(startDir).exists())
      startDir = QDir::homePath();
  }
  const QString file = QFileDialog::getOpenFileName(
      this, "Open Image", startDir,
      QString("Images (%1);;All files (*)").arg(patterns.join(' ')));
  if (!file.isEmpty())
    openPath(file);
}

// Ctrl+V. The clipboard's image types are tried in the order it offers them
// (the source's preference; browsers offer image/png for a copied image),
// each with Qt's reader for that type, or detected from the data if Qt has
// none registered under that name. An animation's frames are all read. If
// none can be read, an image the clipboard offers already decoded is used.
// Without any image on the clipboard, the open image stays.
void ImageViewer::pasteImage() {
  const QMimeData *mime = QApplication::clipboard()->mimeData();
  if (mime) {
    for (const QString &type : mime->formats()) {
      if (!type.startsWith("image/"))
        continue;
      const QByteArray bytes = mime->data(type);
      if (bytes.isEmpty())
        continue;
      LoadedFrames frames = readFramesFromData(
          bytes, QImageReader::imageFormatsForMimeType(type.toLatin1()).value(0),
          "The clipboard image");
      if (!frames.images.isEmpty()) {
        showMemoryImage(std::move(frames), false, type, bytes);
        return;
      }
    }
    if (mime->hasImage()) {
      const QImage image = qvariant_cast<QImage>(mime->imageData());
      if (!image.isNull()) {
        LoadedFrames frames;
        frames.images = {image};
        frames.total = 1;
        showMemoryImage(std::move(frames), false, {}, {});
        return;
      }
    }
  }
  statusBar()->showMessage("No image on the clipboard", 3000);
}

// `imgview -`, or data piped in with no paths, e.g.
// `magick in.png -negate png:- | imgview`. The format is detected from the
// data.
void ImageViewer::openStdinData(const QByteArray &data) {
  LoadedFrames frames;
  if (!data.isEmpty())
    frames = readFramesFromData(data, {}, "Standard input");
  const bool failed = frames.images.isEmpty();
  const QString type = QString::fromLatin1(frames.format);
  showMemoryImage(std::move(frames), true, type, data);
  if (failed) {
    const QString why = data.isEmpty()
                            ? QString("No data on standard input")
                            : QString("No image Qt can read on standard input "
                                      "(%1 received)")
                                  .arg(formatFileSize(data.size()));
    std::fprintf(stderr, "imgview: %s\n", qPrintable(why));
    m_label->setText(why);
  }
}

// An image held in memory, pasted or from standard input, in place of any
// open image. There's nothing to browse; ←/→ only step through its frames.
void ImageViewer::showMemoryImage(LoadedFrames frames, bool fromStdin,
                                  const QString &type, const QByteArray &data) {
  m_list.clear();
  m_folder.clear();
  m_imagePath.clear();
  m_folderIndex = -1;
  m_folderCount = 0;
  m_inMemory = true;
  m_fromStdin = fromStdin;
  m_memoryType = type;
  m_memoryData = data;
  showFrames(std::move(frames));
  refreshInfoPanel();
  updateWindowTitle();
  if (!fromStdin && m_label->frameCount() > 0)
    statusBar()->showMessage(
        QString("Pasted %1 × %2 image%3")
            .arg(m_label->originalSize().width())
            .arg(m_label->originalSize().height())
            .arg(type.isEmpty() ? "" : " (" + type + ")"),
        3000);
}

// Every file in the image's folder that Qt has a reader for (by extension,
// case-insensitively), in natural order so "frame2" sorts before "frame10".
// Rescanned on each use, so files added or removed meanwhile are picked up.
QStringList ImageViewer::imageFilesInFolder() const {
  return imageFilesIn(m_folder);
}

QStringList ImageViewer::imageFilesIn(const QString &folder) {
  QStringList filters;
  for (const QByteArray &fmt : QImageReader::supportedImageFormats())
    filters << "*." + QString::fromLatin1(fmt);
  QDir dir(folder);
  QStringList files = dir.entryList(filters, QDir::Files | QDir::Readable);
  std::sort(files.begin(), files.end(), naturalLess);
  return files;
}

// Open an image file, or a folder at its first image (in the same natural
// order that ←/→ use). An empty folder shows a message; ←/→ then pick up any
// images added to it later.
void ImageViewer::openPath(const QString &path) {
  m_list.clear(); // back to browsing the folder
  m_inMemory = false;
  m_memoryType.clear();
  m_memoryData.clear();
  if (!QFileInfo(path).isDir()) {
    loadImage(path);
    return;
  }
  m_folder = QDir(path).absolutePath();
  const QStringList files = imageFilesInFolder();
  if (!files.isEmpty()) {
    loadImage(QDir(m_folder).filePath(files.first()));
    return;
  }
  m_imagePath.clear();
  m_folderIndex = -1;
  m_folderCount = 0;
  showFrames({});
  m_label->setText(QString("No images found in folder:\n%1").arg(m_folder));
  refreshInfoPanel();
  updateWindowTitle();
}

// Several paths, e.g. `imgview a.png b.png c.png`, a shell glob, or several
// files opened together from a file manager: ←/→ then step through exactly
// these, in the order given, instead of the image's folder. A folder in the
// list contributes its images (natural order). Paths that don't exist or
// aren't readable images are skipped with a note on stderr; duplicates are
// dropped. If only one image is left, it's opened as usual (folder mode).
void ImageViewer::openPaths(const QStringList &paths) {
  if (paths.size() <= 1) {
    if (!paths.isEmpty())
      openPath(paths.first());
    return;
  }
  QStringList list;
  int notImages = 0;
  for (const QString &p : paths) {
    const QFileInfo fi(p);
    if (fi.isDir()) {
      const QString dir = fi.absoluteFilePath();
      for (const QString &f : imageFilesIn(dir))
        list << QDir(dir).filePath(f);
    } else if (!fi.exists()) {
      std::fprintf(stderr, "imgview: skipping %s: no such file\n",
                   QFile::encodeName(p).constData());
    } else if (QImageReader::imageFormat(fi.absoluteFilePath()).isEmpty()) {
      ++notImages; // e.g. a text file matched by a shell glob
    } else {
      list << fi.absoluteFilePath();
    }
  }
  if (notImages > 0)
    std::fprintf(stderr, "imgview: skipped %d file(s) that aren't readable images\n",
                 notImages);
  list.removeDuplicates();

  if (list.size() <= 1) {
    if (list.isEmpty()) {
      openPath(paths.first()); // shows why it can't be opened
    } else {
      openPath(list.first());
    }
    return;
  }
  m_list = list;
  loadImage(m_list.first());
}

namespace {
// True if every pixel is fully opaque (then the file needn't store alpha).
bool isOpaque(const QImage &img) {
  if (!img.hasAlphaChannel())
    return true;
  for (int y = 0; y < img.height(); ++y) {
    const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
    for (int x = 0; x < img.width(); ++x)
      if (qAlpha(line[x]) != 255)
        return false;
  }
  return true;
}

// <dir>/<stem>.png, e.g. "frame_0012_view_x3.000_nearest_inv_g1.68.png", or
// with "-2", "-3", ... appended to the stem if that file already exists.
QString uniquePath(const QString &dir, const QString &stem) {
  QString path = QDir(dir).filePath(stem + ".png");
  for (int n = 2; QFileInfo::exists(path); ++n)
    path = QDir(dir).filePath(QString("%1-%2.png").arg(stem).arg(n));
  return path;
}
} // namespace

// S saves what the image area shows (resampling and tone adjustments
// applied, at display resolution) as a lossless PNG next to the image, or in
// the Pictures folder if the image's folder isn't writable or the image was
// pasted. Shift+S asks
// where to save. The PNG's text fields record the source and view settings.
void ImageViewer::saveView(bool chooseLocation) {
  const QImage rendered = m_label->renderVisible();
  if (rendered.isNull()) {
    statusBar()->showMessage("No image to save", 3000);
    return;
  }
  QImage shot = isOpaque(rendered)
                    ? rendered.convertToFormat(QImage::Format_RGB32)
                    : rendered.convertToFormat(QImage::Format_ARGB32);

  // View settings, for the file name and the PNG's text fields.
  const QString mode = m_label->resamplingName();
  const QString zoom = QString::number(m_label->currentScale(), 'f', 3);
  QString tag = "x" + zoom + "_" + mode;
  QString settings = QString("%1, %2x%3").arg(
      mode, zoom, m_label->isFitToWindow() ? " (fit)" : "");
  if (m_label->frameCount() > 1) {
    // Zero-padded to the width of the count, so the files sort in order.
    const int count = m_label->frameCount();
    const int frame = m_label->frameIndex() + 1;
    tag = QString("f%1_").arg(frame, int(QString::number(count).size()), 10,
                              QLatin1Char('0')) +
          tag;
    settings = QString("frame %1/%2, ").arg(frame).arg(count) + settings;
  }
  for (const ActiveSetting &setting : activeSettings()) {
    tag += "_" + setting.tag;
    settings += ", " + setting.description;
  }
  const QRect region = m_label->visibleSourceRect().toAlignedRect();

  const QFileInfo source(m_imagePath);
  // From standard input: the current directory, where the command was run.
  QString dir = !m_inMemory ? source.absolutePath()
                : m_fromStdin ? QDir::currentPath()
                              : QString();
  if (dir.isEmpty() || !QFileInfo(dir).isWritable()) {
    dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir))
      dir = QDir::homePath();
  }
  const QString stem = !m_inMemory ? source.completeBaseName()
                     : m_fromStdin ? QString("stdin")
                                   : QString("clipboard");
  QString path = uniquePath(dir, stem + "_view_" + tag);

  if (chooseLocation) {
    path = QFileDialog::getSaveFileName(
        this, "Save View", path,
        "PNG image (*.png);;TIFF image (*.tif *.tiff);;All files (*)");
    if (path.isEmpty())
      return;
    if (QFileInfo(path).suffix().isEmpty())
      path += ".png";
  }

  const QString description = QString("%1; source region x=%2 y=%3 w=%4 h=%5")
                                  .arg(settings)
                                  .arg(region.x())
                                  .arg(region.y())
                                  .arg(region.width())
                                  .arg(region.height());
  shot.setText("Source", !m_inMemory ? m_imagePath
                         : m_fromStdin ? QString("standard input")
                                       : QString("clipboard"));
  shot.setText("Description", description);
  shot.setText("Software", "imgview " + QCoreApplication::applicationVersion());

  // Also report on the terminal (stdout / stderr), so saved files show up in
  // a log when imgview is run from a shell or script. Paths are printed in
  // the filesystem's encoding, so they can be pasted into other commands.
  QImageWriter writer(path);
  if (!writer.write(shot)) {
    const QString error = writer.errorString();
    statusBar()->showMessage(QString("Couldn't save %1: %2").arg(path, error),
                             6000);
    std::fprintf(stderr, "imgview: couldn't save view to %s: %s\n",
                 QFile::encodeName(path).constData(), qPrintable(error));
    return;
  }
  statusBar()->showMessage("Saved " + path, 5000);
  std::fprintf(stdout, "imgview: saved view to %s (%s)\n",
               QFile::encodeName(path).constData(), qPrintable(description));
  std::fflush(stdout);
}

// Ask before decoding all frames of a file if they'd take more memory
// than this.
static constexpr qint64 kFrameMemoryWarnBytes = 250LL * 1024 * 1024;

// Shown for this long if the file gives no delay, or one of 10 ms or less,
// as web browsers do for GIFs; also for pages of a document played through.
static constexpr int kDefaultFrameDelayMs = 100;

ImageViewer::LoadedFrames ImageViewer::readFrames(const QString &path) {
  return readFrames([&path] { return std::make_unique<QImageReader>(path); },
                    QFileInfo(path).fileName());
}

// `format` is a reader format to try first (e.g. "png"); empty to detect it
// from the data. Each reader gets its own buffer over the shared bytes.
ImageViewer::LoadedFrames
ImageViewer::readFramesFromData(const QByteArray &data, const QByteArray &format,
                                const QString &name) {
  std::vector<std::unique_ptr<QBuffer>> buffers; // kept until all is read
  return readFrames(
      [&] {
        buffers.push_back(std::make_unique<QBuffer>());
        buffers.back()->setData(data);
        buffers.back()->open(QIODevice::ReadOnly);
        auto reader = std::make_unique<QImageReader>(buffers.back().get());
        if (!format.isEmpty())
          reader->setFormat(format); // also falls back to detection
        return reader;
      },
      name);
}

ImageViewer::LoadedFrames ImageViewer::readFrames(
    const std::function<std::unique_ptr<QImageReader>()> &makeReader,
    const QString &name) {
  LoadedFrames result;
  const std::unique_ptr<QImageReader> counter = makeReader();
  QImageReader &reader = *counter;
  // Before anything else: the TIFF reader counts 1 page after other queries.
  const int count = reader.imageCount();
  result.format = reader.format();
  if (count <= 1) {
    // A single image, read with a new reader as before any queries. A
    // QImage rather than a QPixmap, so a 16-bit image keeps its stored
    // values for the pixel value tooltips.
    const QImage image = makeReader()->read();
    if (!image.isNull()) {
      result.images = {image};
      result.total = 1;
    }
    return result;
  }
  const bool animated = reader.supportsAnimation();
  result.animated = animated;
  result.total = count;

  // Estimated at 4 bytes a pixel as decoded, or 8 for deeper formats.
  int toRead = count;
  const QSize size = reader.size();
  const qint64 bytes = qint64(size.width()) * size.height() *
                       (isDeepFormat(reader.imageFormat()) ? 8 : 4) * count;
  if (size.isValid() && bytes > kFrameMemoryWarnBytes) {
    QMessageBox box(QMessageBox::Warning, "Large image",
                    QString("%1 has %2 %3 of %4 × %5 px. Decoding all of them "
                            "takes about %6 of memory.")
                        .arg(name)
                        .arg(count)
                        .arg(animated ? "frames" : "pages")
                        .arg(size.width())
                        .arg(size.height())
                        .arg(QLocale().formattedDataSize(bytes, 0)),
                    QMessageBox::NoButton, this);
    // The safe choice first, as the dialog focuses its first button (and
    // a focused button is what Enter and Space press); both with one role,
    // so the platform's button order keeps them in this order.
    QPushButton *first = box.addButton(
        QString("First %1 Only").arg(animated ? "Frame" : "Page"),
        QMessageBox::ActionRole);
    QPushButton *all = box.addButton(
        QString("Load All %1").arg(animated ? "Frames" : "Pages"),
        QMessageBox::ActionRole);
    box.setDefaultButton(first);
    box.setEscapeButton(first);
    box.exec();
    if (box.clickedButton() != all) {
      toRead = 1;
      statusBar()->showMessage(
          QString("Showing only the first of %1 %2")
              .arg(count)
              .arg(animated ? "frames" : "pages"),
          5000);
    }
  }

  QGuiApplication::setOverrideCursor(Qt::WaitCursor);
  for (int i = 0; i < toRead; ++i) {
    // Animation readers move on to the next frame by themselves; readers of
    // multi-page files (TIFF) have to be told to.
    if (i > 0 && !animated && !reader.jumpToNextImage())
      break;
    const QImage frame = reader.read();
    if (frame.isNull())
      break;
    result.images << frame;
    const int delay = reader.nextImageDelay(); // valid after read()
    result.delays << (animated && delay <= 10 ? kDefaultFrameDelayMs : delay);
  }
  QGuiApplication::restoreOverrideCursor();
  const int read = int(result.images.size());
  if (read < toRead && read > 0) {
    statusBar()->showMessage(QString("Could read only %1 of %2 frames: %3")
                                 .arg(read)
                                 .arg(toRead)
                                 .arg(reader.errorString()),
                             6000);
    std::fprintf(stderr, "imgview: %s: read only %d of %d frames: %s\n",
                 qPrintable(name), read, toRead,
                 qPrintable(reader.errorString()));
  }
  return result;
}

// Show what readFrames() returned, or nothing for an empty result; stops
// any playback.
void ImageViewer::showFrames(LoadedFrames frames) {
  stopPlayback();
  m_animated = frames.animated;
  m_frameTotal = frames.total;
  m_frameDelays = std::move(frames.delays);
  if (frames.images.isEmpty())
    m_label->clearImage();
  else
    m_label->setFrames(std::move(frames.images)); // keeps the view settings
}

void ImageViewer::loadImage(const QString &path) {
  stopPlayback(); // not behind a "Large image" dialog
  m_inMemory = false;
  m_memoryType.clear();
  m_memoryData.clear();
  m_imagePath = QFileInfo(path).absoluteFilePath();
  m_folder = QFileInfo(m_imagePath).absolutePath();
  LoadedFrames frames = readFrames(m_imagePath);
  const bool failed = frames.images.isEmpty();
  showFrames(std::move(frames));
  if (failed)
    m_label->setText(QString("Failed to load image:\n%1").arg(m_imagePath));

  if (!m_list.isEmpty()) {
    m_folderIndex = int(m_list.indexOf(m_imagePath));
    m_folderCount = int(m_list.size());
  } else {
    const QStringList files = imageFilesInFolder();
    m_folderIndex = int(files.indexOf(QFileInfo(m_imagePath).fileName()));
    m_folderCount = int(files.size());
  }

  refreshInfoPanel();
  updateWindowTitle();
}

void ImageViewer::showAdjacentImage(int delta) {
  if (m_inMemory) {
    statusBar()->showMessage(
        QString("No previous or next image for %1 (O opens a file)")
            .arg(m_fromStdin ? "an image from standard input"
                             : "a pasted image"),
        3000);
    return;
  }
  if (!m_list.isEmpty()) {
    showAdjacentInList(delta);
    return;
  }
  if (m_folder.isEmpty())
    return; // nothing opened yet
  const QStringList files = imageFilesInFolder();
  if (files.isEmpty())
    return;
  const QString name = QFileInfo(m_imagePath).fileName();
  qsizetype index = files.indexOf(name);
  if (m_imagePath.isEmpty()) {
    // Opened an empty folder that has images now: → first, ← last.
    index = delta > 0 ? -1 : files.size();
  } else if (index < 0) {
    // The current file is gone (or isn't a readable type): continue from
    // where it would sort.
    const auto it = std::lower_bound(files.begin(), files.end(), name, naturalLess);
    index = (it - files.begin()) - (delta > 0 ? 1 : 0);
  }
  // Wrap around at either end, with a short note so it's clear the folder
  // has been gone through.
  const qsizetype n = files.size();
  qsizetype target = index + delta;
  QString note;
  if (target >= n) {
    target = 0;
    note = "Wrapped to the first image";
  } else if (target < 0) {
    target = n - 1;
    note = "Wrapped to the last image";
  }
  const QString path = QDir(m_folder).filePath(files[target]);
  if (QFileInfo(path) == QFileInfo(m_imagePath)) {
    statusBar()->showMessage("Only image in this folder", 2000);
    return;
  }
  loadImage(path);
  if (!note.isEmpty())
    statusBar()->showMessage(note, 2000);
}

// ←/→ through the list of paths given: wraps around like folder browsing,
// and skips listed files that have been deleted or moved since.
void ImageViewer::showAdjacentInList(int delta) {
  const qsizetype n = m_list.size();
  qsizetype index = m_list.indexOf(m_imagePath);
  if (index < 0)
    index = delta > 0 ? -1 : n;
  bool wrapped = false;
  for (qsizetype step = 0; step < n; ++step) {
    index += delta;
    if (index >= n || index < 0) {
      index = index >= n ? 0 : n - 1;
      wrapped = true;
    }
    const QString &path = m_list[index];
    if (path == m_imagePath) {
      statusBar()->showMessage(n > 1 ? "No other listed images exist anymore"
                                     : "Only image in the list",
                               3000);
      return;
    }
    if (QFileInfo::exists(path)) {
      loadImage(path);
      if (wrapped)
        statusBar()->showMessage(delta > 0 ? "Wrapped to the first image"
                                           : "Wrapped to the last image",
                                 2000);
      return;
    }
  }
  statusBar()->showMessage("None of the listed images exist anymore", 3000);
}

// ←/→ in an animation or multi-page file: pause, then step one frame,
// wrapping around at either end like image browsing.
void ImageViewer::stepFrame(int delta) {
  const int n = m_label->frameCount();
  int index = m_label->frameIndex() + delta;
  QString note;
  if (index >= n) {
    index = 0;
    note = "Wrapped to the first frame";
  } else if (index < 0) {
    index = n - 1;
    note = "Wrapped to the last frame";
  }
  goToFrame(index);
  if (!note.isEmpty())
    statusBar()->showMessage(note, 2000);
}

void ImageViewer::goToFrame(int index) {
  if (m_playing) {
    stopPlayback();
    updateWindowTitle(); // drop "playing" even if the frame stays the same
    updateHelpPanel();
  }
  m_label->setFrame(index);
}

// Space: play from the frame shown, each frame for its own delay, looping
// (regardless of the file's loop count); again to pause.
void ImageViewer::togglePlayback() {
  if (m_label->frameCount() < 2) {
    statusBar()->showMessage("Not an animation or a multi-page image", 2000);
    return;
  }
  if (m_playing) {
    stopPlayback();
    statusBar()->showMessage(QString("Paused at frame %1")
                                 .arg(m_label->frameIndex() + 1),
                             2000);
  } else {
    m_playing = true;
    m_playTimer.start(frameDelayMs(m_label->frameIndex()));
  }
  updateWindowTitle();
  updateHelpPanel();
}

void ImageViewer::stopPlayback() {
  m_playing = false;
  m_playTimer.stop();
}

// The next frame, timed from when it's ready, so a frame that's slow to
// process (e.g. with the high-pass on) slows playback rather than frames
// being skipped.
void ImageViewer::playNextFrame() {
  const int n = m_label->frameCount();
  if (!m_playing || n < 2)
    return;
  m_label->setFrame((m_label->frameIndex() + 1) % n);
  m_playTimer.start(frameDelayMs(m_label->frameIndex()));
}

int ImageViewer::frameDelayMs(int index) const {
  const int delay = m_frameDelays.value(index);
  return delay > 0 ? delay : kDefaultFrameDelayMs;
}

// E.g. "100 ms each, 4.8 s in all" or "40–500 ms, 5.2 s in all".
QString ImageViewer::frameDelaysText() const {
  if (m_frameDelays.isEmpty())
    return {};
  const auto [lo, hi] =
      std::minmax_element(m_frameDelays.begin(), m_frameDelays.end());
  qint64 total = 0;
  for (int d : m_frameDelays)
    total += d;
  const QString each = *lo == *hi ? QString("%1 ms each").arg(*lo)
                                  : QString("%1–%2 ms").arg(*lo).arg(*hi);
  return QString("%1, %2 s in all").arg(each).arg(total / 1000.0, 0, 'f', 1);
}

// Rebuild the info panel for the current file, if it has been created.
// A hidden panel is just dropped; it's rebuilt when next shown.
void ImageViewer::refreshInfoPanel() {
  if (!m_infoPanel)
    return;
  const bool visible = m_infoPanel->isVisible();
  const bool hadFocus = m_infoPanel->isAncestorOf(QApplication::focusWidget());
  delete m_infoPanel;
  m_infoPanel = nullptr;
  if (visible)
    createInfoPanel();
  if (hadFocus)
    m_label->setFocus();
}

void ImageViewer::createInfoPanel() {
  QWidget *panel = new QWidget(this);
  QVBoxLayout *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(8, 8, 8, 8);

  // File size and dimensions: bold key on the left, mouse-selectable value
  // on the right (so selecting doesn't pick up the "File size:" text).
  QFormLayout *form = new QFormLayout();
  auto addRow = [form](const QString &key, const QString &value) {
    QLabel *valueLabel = new QLabel(value);
    valueLabel->setTextFormat(Qt::PlainText);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setCursor(Qt::IBeamCursor);
    form->addRow(new QLabel(QString("<b>%1</b>").arg(key)), valueLabel);
  };

  if (m_inMemory) {
    addRow("Source:", m_memoryType.isEmpty()
                          ? memorySourceName()
                          : QString("%1 (%2)").arg(memorySourceName(),
                                                   m_memoryType));
    addRow("Data size:", formatFileSize(m_memoryData.size()));
  } else {
    addRow("File size:", formatFileSize(QFileInfo(m_imagePath).size()));
  }

  if (!m_label->originalSize().isEmpty()) {
    int w = m_label->originalSize().width();
    int h = m_label->originalSize().height();
    addRow("Dimensions:", QString("%1 × %2 px").arg(w).arg(h));
  }
  if (m_frameTotal > 1) {
    const int loaded = m_label->frameCount();
    const QString kind = m_animated ? "animated" : "pages";
    addRow("Frames:", loaded < m_frameTotal
                          ? QString("%1 of %2 loaded (%3)")
                                .arg(loaded).arg(m_frameTotal).arg(kind)
                          : QString("%1 (%2)").arg(m_frameTotal).arg(kind));
    if (m_animated && loaded > 1)
      addRow("Frame delays:", frameDelaysText());
  }
  layout->addLayout(form);

  // Metadata table: EXIF, XMP and IPTC sections
  QTableWidget *table = new QTableWidget(this);
  table->setColumnCount(2);
  table->setHorizontalHeaderLabels({"Tag", "Value"});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table->verticalHeader()->setVisible(false);

  table->setWordWrap(true);
  table->setTextElideMode(Qt::ElideNone);
  table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

  table->setSelectionBehavior(QTableWidget::SelectItems);
  table->setSelectionMode(QTableWidget::ExtendedSelection);
  table->setEditTriggers(QTableWidget::AllEditTriggers);
  table->installEventFilter(this);

  // A bold, non-editable row spanning both columns, e.g. "XMP".
  auto addSection = [table](const QString &title) {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *item = new QTableWidgetItem(title);
    item->setFlags(Qt::ItemIsEnabled);
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
    table->setItem(row, 0, item);
    table->setSpan(row, 0, 1, 2);
  };
  // The cell shows the interpreted value ("sRGB"); the raw stored value
  // ("1") is its tooltip when the two differ. The tag's tooltip is its full
  // Exiv2 key (e.g. "Exif.Photo.ColorSpace"), when it has one.
  auto addTag = [table](const QString &tag, const QString &value,
                        const QString &raw = QString(),
                        const QString &key = QString()) {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *tagItem = new QTableWidgetItem(tag);
    if (!key.isEmpty())
      tagItem->setToolTip(key);
    auto *valueItem = new QTableWidgetItem(value);
    if (!raw.isEmpty() && raw != value)
      valueItem->setToolTip(QString("Raw value: %1").arg(raw));
    table->setItem(row, 0, tagItem);
    table->setItem(row, 1, valueItem);
  };
  // exif gives print() context for values that depend on other EXIF tags.
  // Each entry is read on its own, so one Exiv2 can't interpret is shown by
  // its key and raw value instead of ending the table there.
  auto addData = [&addTag](const auto &data, const Exiv2::ExifData *exif) {
    for (const auto &kv : data) {
      if (isMetadataPlaceholder(kv))
        continue;
      const QString key = QString::fromStdString(kv.key());
      const QString raw = QString::fromStdString(kv.value().toString());
      try {
        addTag(metadataLabel(kv), QString::fromStdString(kv.print(exif)), raw,
               key);
      } catch (const Exiv2::Error &e) {
        qWarning("Exiv2 error for %s: %s", kv.key().c_str(), e.what());
        addTag(key, raw);
      }
    }
  };

  // Nothing to read for an empty folder, or a pasted image without its
  // encoded data.
  if (!m_imagePath.isEmpty() || !m_memoryData.isEmpty()) try {
    auto image =
        m_inMemory ? Exiv2::ImageFactory::open(
                       reinterpret_cast<const Exiv2::byte *>(
                           m_memoryData.constData()),
                       m_memoryData.size())
                 : Exiv2::ImageFactory::open(m_imagePath.toUtf8().constData());
    // .get(): open() returns std::auto_ptr in Exiv2 0.27 (no bool
    // conversion) and std::unique_ptr in 0.28.
    if (image.get()) {
      image->readMetadata();

      // Offsets of the Exif, GPS, Interoperability and sub-IFDs: Exiv2 has
      // already read the tags they point to, so they mean nothing on their own.
      Exiv2::ExifData exifData = image->exifData();
      for (const char *key :
           {"Exif.Image.ExifTag", "Exif.Image.GPSTag",
            "Exif.Photo.InteroperabilityTag", "Exif.Image.SubIFDs"}) {
        auto it = exifData.findKey(Exiv2::ExifKey(key));
        if (it != exifData.end())
          exifData.erase(it);
      }
      if (!exifData.empty()) {
        addSection("EXIF");
        addData(exifData, &exifData);
      }

      // The toolkit is an attribute of the packet itself, not a property,
      // so it isn't among the XMP data. writeXmpFromPacket(true) makes
      // xmpPacket() return the packet as read rather than Exiv2's
      // re-encoding of it (toolkit "XMP Core 4.4.0-Exiv2"). For some formats
      // (JPEG XL, HEIF) Exiv2 doesn't keep the packet as read, so there's no
      // toolkit to show.
      image->writeXmpFromPacket(true);
      static const QRegularExpression xmptk(
          R"re(x:xmptk\s*=\s*["']([^"']*)["'])re");
      const QString toolkit =
          xmptk.match(QString::fromStdString(image->xmpPacket())).captured(1);
      if (!image->xmpData().empty() || !toolkit.isEmpty()) {
        addSection("XMP");
        if (!toolkit.isEmpty())
          addTag("XMP Toolkit", toolkit);
        addData(image->xmpData(), &exifData);
      }

      if (!image->iptcData().empty()) {
        addSection("IPTC");
        addData(image->iptcData(), &exifData);
      }
    }
  } catch (const Exiv2::Error &e) {
    qWarning("Exiv2 error: %s", e.what());
  } catch (const std::exception &e) {
    qWarning("Exception: %s", e.what());
  }

  layout->addWidget(new QLabel("<b>Metadata:</b>"));
  layout->addWidget(table);

  table->resizeRowsToContents();
  table->resizeColumnsToContents();

  m_infoPanel = panel;
  QHBoxLayout *mainLayout =
      qobject_cast<QHBoxLayout *>(centralWidget()->layout());
  if (mainLayout)
    mainLayout->addWidget(panel, 0);
}

// I cycles nearest -> bicubic -> lanczos3 -> nearest.
void ImageViewer::toggleInterpolation() {
  using Mode = ScaledImageLabel::Interpolation;
  Mode next = Mode::Nearest;
  switch (m_label->interpolation()) {
  case Mode::Nearest:
    next = Mode::Bicubic;
    break;
  case Mode::Bicubic:
    next = Mode::Lanczos3;
    break;
  case Mode::Lanczos3:
    next = Mode::Nearest;
    break;
  }
  m_label->setInterpolation(next);
  updateWindowTitle();
  QString message = ScaledImageLabel::interpolationName(next);
  message[0] = message[0].toUpper();
  message += " resampling";
  if (next == Mode::Nearest)
    message += " (true pixels)";
  if (m_label->resamplingName() == "area")
    message += ", used when zoomed in";
  statusBar()->showMessage(message, kSettingMessageMs);
}

// The status bar's key hints. For an animation or multi-page image ←/→
// step through frames, so the hints show PgUp/PgDn for switching images
// and Space for playing; otherwise just ←/→.
void ImageViewer::updateHints() {
  const bool frames = m_label && m_label->frameCount() > 1;
  const QString text =
      QStringLiteral("help: ?  |  open: o  |  paste: ctrl+v  |  info: m  |  "
                     "quit: q  |  zoom: - + 0 r  |  pan: ← ↑ → ↓  |  ") +
      (frames ? QStringLiteral("prev/next: pgup pgdn  |  frames: ← →  |  "
                               "play: space  |  ")
              : QStringLiteral("prev/next: ← →  |  ")) +
      QStringLiteral("fullscreen: f");
  if (m_hintLabel->text() != text)
    m_hintLabel->setText(text);
}

// The processing and tone settings that are on, in the order applied.
// The saturation change isn't applied while false color is on.
QList<ImageViewer::ActiveSetting> ImageViewer::activeSettings() const {
  QList<ActiveSetting> list;
  if (m_label->stretchStep() > 0) {
    const QString clip = QString::number(m_label->stretchClipPercent());
    list.append({"stretch " + clip + "%", "stretch " + clip + "%", "st" + clip});
  } else if (m_label->stretchStep() < 0) {
    const QString squash = QString::number(m_label->squashPercent());
    list.append(
        {"squash " + squash + "%", "squash " + squash + "%", "sq" + squash});
  }
  if (m_label->highPassStep() != 0) {
    const QString radius = QString::number(m_label->highPassRadius());
    list.append({"high-pass " + radius + " px", "high-pass " + radius + " px",
                 "hp" + radius});
  }
  if (m_label->isInverted())
    list.append({"inverted", "inverted", "inv"});
  if (m_label->gammaStep() != 0) {
    const QString g = QString::number(m_label->gamma(), 'f', 2);
    list.append({"γ " + g, "gamma " + g, "g" + g});
  }
  const auto falseColor = m_label->falseColor();
  if (m_label->saturationStep() != 0 &&
      falseColor == ScaledImageLabel::FalseColor::Off) {
    const QString name = m_label->saturationModeName(); // or "vibrance"
    const QString sat = QString::number(m_label->saturationPercent());
    list.append({name.left(3) + " " + sat + "%", name + " " + sat + "%",
                 name.left(3) + sat}); // e.g. "sat 60%", "sat60"
  }
  if (falseColor != ScaledImageLabel::FalseColor::Off) {
    const QString map = ScaledImageLabel::falseColorName(falseColor);
    list.append({map, "false color " + map, map});
  }
  return list;
}

// e.g. "frame_0012.png (12/48) – Image Viewer [nearest, 3.138×]" or
// "... [area, inverted, γ 1.68, sat 60%, fit 0.421×]". The
// mode is shown because it changes what the pixels mean; three decimals so a
// near-1 wheel zoom can't be mistaken for exactly 1:1.
void ImageViewer::updateWindowTitle() {
  updateHints();
  if (m_folder.isEmpty() && !m_inMemory) { // nothing opened yet
    setWindowTitle("Image Viewer");
    if (m_viewInfoLabel)
      m_viewInfoLabel->setText("No image open");
    return;
  }
  QString info = m_label->resamplingName();
  for (const ActiveSetting &setting : activeSettings())
    info += ", " + setting.title;
  if (!m_label->originalSize().isEmpty()) {
    info += QString(", %1%2×")
                .arg(m_label->isFitToWindow() ? "fit " : "")
                .arg(m_label->currentScale(), 0, 'f', 3);
  }
  QString file;
  if (m_inMemory)
    file = m_memoryType.isEmpty()
               ? memorySourceName()
               : QString("%1 (%2)").arg(memorySourceName(), m_memoryType);
  else if (m_imagePath.isEmpty())
    file = QDir(m_folder).dirName() + "/";
  else
    file = QFileInfo(m_imagePath).fileName();
  if (m_folderIndex >= 0)
    file += QString(" (%1/%2)").arg(m_folderIndex + 1).arg(m_folderCount);
  if (m_label->frameCount() > 1)
    file += QString(", frame %1/%2%3")
                .arg(m_label->frameIndex() + 1)
                .arg(m_label->frameCount())
                .arg(m_playing ? ", playing" : "");
  setWindowTitle(QString("%1 – Image Viewer [%2]").arg(file, info));
  // Same info for the status bar, used in fullscreen.
  if (m_viewInfoLabel)
    m_viewInfoLabel->setText(QString("%1  [%2]").arg(file, info));
}

// Show the view info in the status bar only while fullscreen. Reacting to the
// window state (rather than in toggleFullScreen()) also covers fullscreen
// changes made by the window manager.
void ImageViewer::changeEvent(QEvent *event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange && m_viewInfoLabel)
    m_viewInfoLabel->setVisible(isFullScreen());
}

void ImageViewer::toggleInverted() {
  m_label->setInverted(!m_label->isInverted());
  statusBar()->showMessage(m_label->isInverted() ? "Inverted" : "Not inverted",
                           kSettingMessageMs);
}

void ImageViewer::stepGamma(int delta) {
  const int before = m_label->gammaStep();
  m_label->setGammaStep(before + delta);
  QString message = QString("Gamma %1").arg(m_label->gamma(), 0, 'f', 2);
  if (m_label->gammaStep() == before)
    message += delta > 0 ? " (maximum)" : " (minimum)";
  statusBar()->showMessage(message, kSettingMessageMs);
}

void ImageViewer::stepSaturation(int delta) {
  const int before = m_label->saturationStep();
  m_label->setSaturationStep(before + delta);
  QString message = m_label->saturationModeName();
  message[0] = message[0].toUpper();
  message += QString(" %1%").arg(m_label->saturationPercent());
  if (m_label->saturationStep() == before)
    message += delta > 0 ? " (maximum)" : " (minimum)";
  statusBar()->showMessage(message, kSettingMessageMs);
}

// Z / X: less / more contrast stretch. The message gives the black and
// white points, in the image's own values (16-bit for a deeper image).
void ImageViewer::stepStretch(int delta) {
  const int before = m_label->stretchStep();
  m_label->setStretchStep(before + delta);
  if (m_label->stretchStep() == 0) {
    statusBar()->showMessage("Contrast stretch off", kSettingMessageMs);
    return;
  }
  const StretchRange range = m_label->stretchRange();
  const bool deep = m_label->isDeep();
  QString message;
  if (m_label->stretchStep() > 0) {
    message = QString("Contrast stretch %1%: %2–%3%4 → 0–255")
                  .arg(m_label->stretchClipPercent())
                  .arg(deep ? range.black : range.black / 257)
                  .arg(deep ? range.white : range.white / 257)
                  .arg(deep ? " (16-bit)" : "");
  } else {
    // Where black and white end up.
    const int low = int(std::lround(-range.black * 255.0 /
                                    (range.white - range.black)));
    message = QString("Contrast squash %1%: %2 → %3–%4")
                  .arg(m_label->squashPercent())
                  .arg(deep ? "0–65535 (16-bit)" : "0–255")
                  .arg(low)
                  .arg(255 - low);
  }
  if (m_label->stretchStep() == before)
    message += delta > 0 ? " (maximum)" : " (minimum)";
  statusBar()->showMessage(message, kSettingMessageMs);
}

// H / Shift+H: cycle the high-pass radius up / down, through off. The
// message gives the difference shown as full white or black, in the image's
// own values (16-bit for a deeper image), before any stretch.
void ImageViewer::stepHighPass(int delta) {
  constexpr int n = ScaledImageLabel::kMaxHighPassStep + 1;
  m_label->setHighPassStep((m_label->highPassStep() + delta + n) % n);
  if (m_label->highPassStep() == 0) {
    statusBar()->showMessage("High-pass off", kSettingMessageMs);
    return;
  }
  const StretchRange range = m_label->stretchRange();
  const bool deep = m_label->isDeep();
  const double levels = m_label->highPassLimit() * (range.white - range.black) /
                        (deep ? 1.0 : 257.0);
  statusBar()->showMessage(
      QString("High-pass %1 px: grey = like its surroundings, ±%2%3 → "
              "white, black")
          .arg(m_label->highPassRadius())
          .arg(levels, 0, 'f', levels < 10 ? 1 : 0)
          .arg(deep ? " (16-bit)" : ""),
      kSettingMessageMs);
}

// B switches how the saturation factor is applied: the same to every pixel,
// or weighted towards muted pixels (vibrance). The amount is kept.
void ImageViewer::toggleSaturationMode() {
  m_label->setSaturationMode(m_label->saturationMode() ==
                                     SaturationMode::Linear
                                 ? SaturationMode::Vibrance
                                 : SaturationMode::Linear);
  statusBar()->showMessage(
      m_label->saturationMode() == SaturationMode::Vibrance
          ? "Vibrance: boosts muted colors more than vivid ones"
          : "Linear saturation: same change for every color",
      kSettingMessageMs);
}

// K / Shift+K: cycle the false colour map forwards / backwards.
void ImageViewer::stepFalseColor(int delta) {
  using FalseColor = ScaledImageLabel::FalseColor;
  constexpr int n = ScaledImageLabel::kFalseColorCount;
  const auto map = FalseColor((int(m_label->falseColor()) + delta + n) % n);
  m_label->setFalseColor(map);
  statusBar()->showMessage(
      map == FalseColor::Off
          ? QString("False color off")
          : QString("False color: %1, dark to bright")
                .arg(ScaledImageLabel::falseColorName(map)),
      kSettingMessageMs);
}

// P: while on, resting the pointer on the image shows the stored value of
// the pixel under it in a tooltip.
void ImageViewer::togglePixelTips() {
  m_label->setPixelTips(!m_label->pixelTips());
  updateHelpPanel();
  statusBar()->showMessage(m_label->pixelTips()
                               ? "Pixel values: rest the pointer on a pixel"
                               : "Pixel values off",
                           kSettingMessageMs);
}

// Backspace: undo every tone adjustment at once, so none is left on by
// accident. Zoom and resampling mode are left as they are.
void ImageViewer::resetTone() {
  m_label->setHighPassStep(0); // first, so the stretch change doesn't redo it
  m_label->setStretchStep(0);
  m_label->setInverted(false);
  m_label->setGammaStep(0);
  m_label->setSaturationStep(0);
  m_label->setFalseColor(ScaledImageLabel::FalseColor::Off);
  statusBar()->showMessage(
      "Stretch, high-pass, inversion, gamma, saturation and false color "
      "reset",
      kSettingMessageMs);
}

// The help panel on the left lists every key, with the current value of
// each view setting. It doesn't take keyboard focus, so all keys keep
// working while it's open and their effect can be watched next to it.
void ImageViewer::toggleHelpPanel() {
  if (!m_helpPanel) {
    m_helpLabel = new QLabel;
    m_helpLabel->setTextFormat(Qt::RichText);
    m_helpLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_helpLabel->setContentsMargins(8, 8, 8, 8);
    m_helpPanel = new QScrollArea(this);
    m_helpPanel->setWidget(m_helpLabel);
    m_helpPanel->setWidgetResizable(true);
    m_helpPanel->setFrameShape(QFrame::NoFrame);
    m_helpPanel->setFocusPolicy(Qt::NoFocus);
    m_helpPanel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_helpPanel->hide();
    connect(m_label, &ScaledImageLabel::viewChanged, this,
            &ImageViewer::updateHelpPanel);
    if (auto *mainLayout =
            qobject_cast<QHBoxLayout *>(centralWidget()->layout()))
      mainLayout->insertWidget(0, m_helpPanel, 0);
  }
  m_helpPanel->setVisible(!m_helpPanel->isVisible());
  updateHelpPanel();
}

void ImageViewer::updateHelpPanel() {
  if (!m_helpPanel || !m_helpPanel->isVisible())
    return;
  struct Row {
    const char *keys; // empty for a section heading
    const char *action;
    QString value;
    bool active = false; // not at its default: the value is shown bold
  };
  // Short labels; the README and man page list each setting's steps.
  // Shift variants are written as capitals (H for Shift+h).
  const bool frames = m_label->frameCount() > 1;
  const auto interp = m_label->interpolation();
  const auto falseColor = m_label->falseColor();
  const bool vibrance = m_label->saturationMode() == SaturationMode::Vibrance;
  const QList<Row> rows = {
      {"", "View", {}},
      {"+ - =", "Zoom (wheel: at pointer)", {}},
      {"0  r", "Fit, 1:1", {}},
      {"arrows", "Pan (Shift: 1 px)", {}},
      {"p", "Pixel values", m_label->pixelTips() ? "on" : "off",
       m_label->pixelTips()},
      {"i", "Resampling", ScaledImageLabel::interpolationName(interp),
       interp != ScaledImageLabel::Interpolation::Nearest},
      {"", "Tone", {}},
      {"z x c", "Contrast −, +, off", m_label->stretchName(),
       m_label->stretchStep() != 0},
      {"h H", "High-pass ±",
       m_label->highPassStep() == 0
           ? QString("off")
           : QString("%1 px").arg(m_label->highPassRadius()),
       m_label->highPassStep() != 0},
      {"n", "Invert", m_label->isInverted() ? "on" : "off",
       m_label->isInverted()},
      {". , /", "Gamma ±, reset", QString::number(m_label->gamma(), 'f', 2),
       m_label->gammaStep() != 0},
      {"; l '", "Saturation ±, reset",
       QString("%1%").arg(m_label->saturationPercent()),
       m_label->saturationStep() != 0},
      {"b", "Saturation mode", vibrance ? "vibrance" : "linear", vibrance},
      {"k K", "False color ±", ScaledImageLabel::falseColorName(falseColor),
       falseColor != ScaledImageLabel::FalseColor::Off},
      {"Backspace", "Reset tone", {}},
      {"", "Images", {}},
  };
  QList<Row> all = rows;
  if (!frames) // ←/→ step frames in an animation
    all += Row{"← →", "Previous, next (if it fits)", {}};
  all += QList<Row>{
      {"PgUp PgDn", "Previous, next", {}},
      {"o  Ctrl+V", "Open, paste", {}},
      {"s S", "Save view, save as…", {}},
  };
  if (frames)
    all += QList<Row>{
        {"", "Frames", {}},
        {"← →", "Previous, next",
         QString("%1/%2").arg(m_label->frameIndex() + 1).arg(m_label->frameCount())},
        {"Home End", "First, last", {}},
        {"Space", "Play, pause", m_playing ? "playing" : "paused", m_playing},
    };
  all += QList<Row>{
      {"", "Window", {}},
      {"m f ?", "Info, fullscreen, help", {}},
      {"Esc q", "Close or exit, quit", {}},
  };
  // Descriptions a little dimmer than the keys and values, so the key
  // column is easy to scan down.
  const QPalette pal = m_helpLabel->palette();
  const QColor fg = pal.color(QPalette::WindowText);
  const QColor bg = pal.color(QPalette::Window);
  const QString dim =
      QColor::fromRgbF(0.72f * fg.redF() + 0.28f * bg.redF(),
                       0.72f * fg.greenF() + 0.28f * bg.greenF(),
                       0.72f * fg.blueF() + 0.28f * bg.blueF())
          .name();
  QString html = "<table cellspacing='0' cellpadding='3'>";
  for (const Row &row : all) {
    if (!*row.keys) {
      html += QString("<tr><td colspan='3' style='padding-top: 10px'>"
                      "<b>%1</b></td></tr>")
                  .arg(row.action);
      continue;
    }
    const QString value = row.value.toHtmlEscaped();
    html += QString("<tr><td style='padding-right: 12px'><b>%1</b></td>"
                    "<td style='color: %2'>%3</td>"
                    "<td width='70' align='right'>%4</td></tr>")
                .arg(QString(row.keys).toHtmlEscaped(), dim,
                     QString(row.action).toHtmlEscaped(),
                     row.active ? "<b>" + value + "</b>" : value);
  }
  html += "</table>";
  if (html != m_helpLabel->text()) {
    m_helpLabel->setText(html);
    // Wide enough for the text, with room for the scroll bar a short
    // window needs. The value column has a fixed width, so changing
    // values don't resize the image view.
    m_helpPanel->setFixedWidth(
        m_helpLabel->sizeHint().width() +
        m_helpPanel->verticalScrollBar()->sizeHint().width());
  }
}

void ImageViewer::toggleFullScreen() {
  if (isFullScreen())
    showNormal();
  else
    showFullScreen();
}

void ImageViewer::toggleInfoPanel() {
  if (!m_infoPanel) {
    createInfoPanel();
  } else {
    m_infoPanel->setVisible(!m_infoPanel->isVisible());
  }
}

// All shortcuts live in the window rather than in child widgets so they work
// no matter which child has focus (e.g. after selecting text in the info
// panel). Children that don't use a key pass it up to keyPressEvent().
bool ImageViewer::handleShortcutKey(QKeyEvent *event) {
  if (event->matches(QKeySequence::Paste)) { // Ctrl+V, also Shift+Insert
    pasteImage();
    return true;
  }
  switch (event->key()) {
  case Qt::Key_Escape:
    // Close help or leave fullscreen first rather than quitting.
    if (m_helpPanel && m_helpPanel->isVisible())
      m_helpPanel->hide();
    else if (isFullScreen())
      showNormal();
    else
      close();
    return true;
  case Qt::Key_Q:
    close();
    return true;
  case Qt::Key_M:
    toggleInfoPanel();
    return true;
  case Qt::Key_Question:
    toggleHelpPanel();
    return true;
  case Qt::Key_Backspace:
    resetTone();
    return true;
  case Qt::Key_O:
    showOpenDialog();
    return true;
  case Qt::Key_S: // Shift+S: choose where
    saveView(event->modifiers() & Qt::ShiftModifier);
    return true;
  case Qt::Key_I:
    toggleInterpolation();
    return true;
  case Qt::Key_F:
    toggleFullScreen();
    return true;
  case Qt::Key_N:
    toggleInverted();
    return true;
  case Qt::Key_Period: // darker, more contrast among bright values
    stepGamma(1);
    return true;
  case Qt::Key_Comma: // brighter, more contrast among dark values
    stepGamma(-1);
    return true;
  case Qt::Key_Slash: // reset gamma (inversion is left as it is)
    m_label->setGammaStep(0);
    statusBar()->showMessage("Gamma reset to 1.00", kSettingMessageMs);
    return true;
  case Qt::Key_Semicolon: // more saturated, for more dramatic colours
    stepSaturation(1);
    return true;
  case Qt::Key_L: // less saturated, down to grey
    stepSaturation(-1);
    return true;
  case Qt::Key_Apostrophe: // reset saturation (the mode is kept)
    m_label->setSaturationStep(0);
    statusBar()->showMessage("Saturation reset to 100%", kSettingMessageMs);
    return true;
  case Qt::Key_B:
    toggleSaturationMode();
    return true;
  case Qt::Key_Space:
    togglePlayback();
    return true;
  // Not with Ctrl, so Ctrl+Z / Ctrl+X / Ctrl+C still undo, cut and copy,
  // e.g. in the metadata table.
  case Qt::Key_Z:
  case Qt::Key_X:
  case Qt::Key_C:
    if (event->modifiers() & Qt::ControlModifier)
      return false;
    if (event->key() == Qt::Key_C) {
      m_label->setStretchStep(0);
      statusBar()->showMessage("Contrast stretch and squash off",
                               kSettingMessageMs);
    } else {
      stepStretch(event->key() == Qt::Key_X ? 1 : -1);
    }
    return true;
  case Qt::Key_H: // Shift+H: the other way round
    stepHighPass(event->modifiers() & Qt::ShiftModifier ? -1 : 1);
    return true;
  case Qt::Key_K: // Shift+K: the other way round
    stepFalseColor(event->modifiers() & Qt::ShiftModifier ? -1 : 1);
    return true;
  case Qt::Key_P:
    togglePixelTips();
    return true;
  case Qt::Key_R: {
    // Actual size, centred on the pointer if it's over the image.
    QPointF anchor = m_label->mapFromGlobal(QCursor::pos());
    if (!m_label->rect().contains(anchor.toPoint()))
      anchor = QRectF(m_label->rect()).center();
    m_label->setScale(1.0, anchor);
    return true;
  }
  case Qt::Key_Plus:
  case Qt::Key_Equal:
    m_label->zoomIn();
    return true;
  case Qt::Key_Minus:
    m_label->zoomOut();
    return true;
  case Qt::Key_0:
    m_label->resetZoom();
    return true;
  default:
    return false;
  }
}

// The metadata table would otherwise eat the shortcut keys (type-to-edit and
// keyboard search). While a cell is being edited the editor widget has
// focus instead of the table, so typing in a cell is unaffected.
bool ImageViewer::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::KeyPress &&
      handleShortcutKey(static_cast<QKeyEvent *>(event)))
    return true;
  return QMainWindow::eventFilter(watched, event);
}

// Left/Right reach here when nothing used them first: the image view passes
// them up when the image isn't wider than the window. They're deliberately
// not in handleShortcutKey(), so they still move between metadata table cells.
// They switch images only when the image fits the view both ways: an image
// that's only taller than the view can still be panned up and down, so
// switching then would jump away from a zoomed-in view by accident.
void ImageViewer::keyPressEvent(QKeyEvent *event) {
  if (handleShortcutKey(event))
    return;
  if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    // In an animation or multi-page file ←/→ step through the frames, and
    // PgUp/PgDn switch images; they switch images at any zoom.
    const bool frames = m_label->frameCount() > 1;
    switch (event->key()) {
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
      showAdjacentImage(event->key() == Qt::Key_PageDown ? 1 : -1);
      return;
    case Qt::Key_Home:
    case Qt::Key_End:
      if (frames) {
        goToFrame(event->key() == Qt::Key_Home ? 0 : m_label->frameCount() - 1);
        return;
      }
      break;
    case Qt::Key_Left:
    case Qt::Key_Right:
      if (!m_label->canPan()) {
        const int delta = event->key() == Qt::Key_Right ? 1 : -1;
        if (frames)
          stepFrame(delta);
        else
          showAdjacentImage(delta);
        return;
      }
      break;
    default:
      break;
    }
  }
  QMainWindow::keyPressEvent(event);
}
