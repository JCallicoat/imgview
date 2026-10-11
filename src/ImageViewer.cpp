

#include "ImageViewer.h"
#include "Resample.h"
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>

#include <exiv2/exiv2.hpp>

//-------------------- ScaledImageLabel --------------------

namespace {
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 10.0;
constexpr double kZoomStep = 1.1;
// Renders slower than this use a quick QPainter preview while interacting.
constexpr qint64 kInteractiveBudgetMs = 16;
// How long the view must be still before the bicubic render.
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

// Called whenever a tone setting changes. A future curve type (levels,
// S-curve) would be built here too.
void ScaledImageLabel::rebuildToneLut() {
  m_toneIsIdentity = !m_inverted && m_gammaStep == 0;
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
    // applyLut() then detaches, so `rendered` itself is untouched.
    m_adjustedCache =
        rendered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    applyLut(m_adjustedCache, m_toneLut);
    m_adjustedSourceKey = rendered.cacheKey();
  }
  return m_adjustedCache;
}

void ScaledImageLabel::markInteracting() {
  m_interacting = true;
  m_refineTimer.start(); // restarts if already running
}

void ScaledImageLabel::setPixmap(const QPixmap &pixmap) {
  setPixmap(pixmap.toImage());
}

void ScaledImageLabel::setPixmap(const QImage &image) {
  m_original = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  m_viewportCache = QImage();
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

  if (scale * dpr <= 1.0) {
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
    const QRectF target(m_offset, scaledSize());
    const QRectF visible = target.intersected(QRectF(rect()));
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
      // Only smooth it in bicubic mode, so nearest never shows blending.
      const bool smooth = m_interpolation == Interpolation::Bicubic;
      const QRectF source((visible.topLeft() - m_offset) / scale,
                          visible.size() / scale);
      if (m_toneIsIdentity) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        painter.drawImage(visible, m_original, source);
      } else {
        // Draw the preview off-screen so the tone curve can be applied.
        QImage preview(outSize, QImage::Format_ARGB32_Premultiplied);
        preview.setDevicePixelRatio(dpr);
        preview.fill(Qt::transparent);
        QPainter p(&preview);
        p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        p.translate(-pos);
        p.drawImage(visible, m_original, source);
        p.end();
        applyLut(preview, m_toneLut);
        painter.drawImage(pos, preview);
      }
      return;
    }

    if (!cacheValid) {
      QElapsedTimer timer;
      timer.start();
      m_viewportCache =
          m_interpolation == Interpolation::Nearest
              ? resampleNearest(m_original, outSize, origin, step)
              : resampleBicubic(m_original, outSize, origin, step);
      m_viewportCache.setDevicePixelRatio(dpr);
      m_viewportCachePos = pos;
      m_viewportCacheOrigin = origin;
      m_viewportCacheStep = step;
      m_lastRenderMs = timer.elapsed();
    }
    painter.drawImage(pos, forDisplay(m_viewportCache));
  }
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
// view. They move the view like scrollbars do (Right shows more of the right
// side): 10% of the view per press, or exactly one image pixel with Shift
// for stepping across fine detail. Other keys go up to the window.
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
  if (!canPan()) {
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
} // namespace

ImageViewer::ImageViewer(const QString &imagePath, QWidget *parent)
    : QMainWindow(parent), m_imagePath(imagePath) {
  resize(800, 600);

  QWidget *central = new QWidget(this);
  setCentralWidget(central);
  QHBoxLayout *mainLayout = new QHBoxLayout(central);

  m_label = new ScaledImageLabel(this);
  connect(m_label, &ScaledImageLabel::viewChanged, this,
          &ImageViewer::updateWindowTitle);
  updateWindowTitle();
  mainLayout->addWidget(m_label, 1);

  QPixmap pixmap(imagePath);
  if (pixmap.isNull()) {
    m_label->setText(QString("Failed to load image:\n%1").arg(imagePath));
  } else {
    m_label->setPixmap(pixmap);
  }
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

  QFileInfo fi(m_imagePath);
  addRow("File size:", formatFileSize(fi.size()));

  if (!m_label->originalSize().isEmpty()) {
    int w = m_label->originalSize().width();
    int h = m_label->originalSize().height();
    addRow("Dimensions:", QString("%1 × %2 px").arg(w).arg(h));
  }
  layout->addLayout(form);

  // EXIF data table
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

  try {
    auto image = Exiv2::ImageFactory::open(m_imagePath.toUtf8().constData());
    if (image) {
      image->readMetadata();
      const auto &exifData = image->exifData();
      if (!exifData.empty()) {
        table->setRowCount(static_cast<int>(exifData.count()));
        int row = 0;
        for (const auto &kv : exifData) {
          QString tag =
              QString::fromStdString(kv.tagLabel()); // kv.getTagName()
          QString value = QString::fromStdString(kv.value().toString());
          table->setItem(row, 0, new QTableWidgetItem(tag));
          table->setItem(row, 1, new QTableWidgetItem(value));
          ++row;
        }
      }
    }
  } catch (const Exiv2::Error &e) {
    qWarning("Exiv2 error: %s", e.what());
  } catch (const std::exception &e) {
    qWarning("Exception: %s", e.what());
  }

  layout->addWidget(new QLabel("<b>EXIF Data:</b>"));
  layout->addWidget(table);

  table->resizeRowsToContents();
  table->resizeColumnsToContents();

  m_infoPanel = panel;
  QHBoxLayout *mainLayout =
      qobject_cast<QHBoxLayout *>(centralWidget()->layout());
  if (mainLayout)
    mainLayout->addWidget(panel, 0);
}

void ImageViewer::toggleInterpolation() {
  using Mode = ScaledImageLabel::Interpolation;
  m_label->setInterpolation(m_label->interpolation() == Mode::Nearest
                                ? Mode::Bicubic
                                : Mode::Nearest);
  updateWindowTitle();
}

// e.g. "Image Viewer [nearest, 3.138×]" or
// "[nearest, inverted, γ 1.68, fit 0.421×]". The
// mode is shown because it changes what the pixels mean; three decimals so a
// near-1 wheel zoom can't be mistaken for exactly 1:1.
void ImageViewer::updateWindowTitle() {
  const bool nearest =
      m_label->interpolation() == ScaledImageLabel::Interpolation::Nearest;
  QString info = nearest ? "nearest" : "bicubic";
  if (m_label->isInverted())
    info += ", inverted";
  if (m_label->gammaStep() != 0)
    info += QString(", γ %1").arg(m_label->gamma(), 0, 'f', 2);
  if (!m_label->originalSize().isEmpty()) {
    info += QString(", %1%2×")
                .arg(m_label->isFitToWindow() ? "fit " : "")
                .arg(m_label->currentScale(), 0, 'f', 3);
  }
  setWindowTitle(QString("Image Viewer [%1]").arg(info));
}

void ImageViewer::toggleInverted() {
  m_label->setInverted(!m_label->isInverted());
}

void ImageViewer::stepGamma(int delta) {
  m_label->setGammaStep(m_label->gammaStep() + delta);
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
  switch (event->key()) {
  case Qt::Key_Escape:
    // Leave fullscreen first rather than quitting straight out of it.
    if (isFullScreen())
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

// The EXIF table would otherwise eat the shortcut keys (type-to-edit and
// keyboard search). While a cell is being edited the editor widget has
// focus instead of the table, so typing in a cell is unaffected.
bool ImageViewer::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::KeyPress &&
      handleShortcutKey(static_cast<QKeyEvent *>(event)))
    return true;
  return QMainWindow::eventFilter(watched, event);
}

void ImageViewer::keyPressEvent(QKeyEvent *event) {
  if (!handleShortcutKey(event))
    QMainWindow::keyPressEvent(event);
}
