

#include "ImageViewer.h"
#include "Resample.h"
#include <QApplication>
#include <QCursor>
#include <QDebug>
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
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidgetItem>
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
  QLabel::setText(QString()); // drop any earlier "Failed to load" message
  m_original = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  m_viewportCache = QImage();
  resetView();
}

void ScaledImageLabel::clearImage() {
  m_original = QImage();
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

QRectF ScaledImageLabel::visibleSourceRect() const {
  if (m_original.isNull())
    return {};
  const QRectF target(m_offset, scaledSize());
  const QRectF visible = target.intersected(QRectF(rect()));
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
  if (scale * dpr <= 1.0) {
    // Zoomed out: the cache holds the whole downscaled image; cut out the
    // part that is on screen.
    if (m_scaledCache.isNull())
      return {};
    const QImage &full = forDisplay(m_scaledCache);
    const QRectF target(m_offset, scaledSize());
    const QRectF visible = target.intersected(QRectF(rect()));
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
} // namespace

ImageViewer::ImageViewer(const QString &path, QWidget *parent)
    : QMainWindow(parent) {
  resize(800, 600);

  // Key hints. Keys are space-separated (',' and '-' are keys themselves).
  // A minimum width of 1 lets the window shrink below the text width; the
  // text is then clipped, with the full list still in the tooltip.
  const QString hints = QStringLiteral(
      "zoom: - + 0 r  |  pan: ← ↑ → ↓ (shift: 1 px)  |  prev/next image: ← →  |  "
      "interp: i  |  "
      "invert: n  |  gamma: , . /  |  info: m  |  open: o  |  "
      "save view: s (shift: save as)  |  fullscreen: f  |  "
      "quit: q");
  QLabel *hintLabel = new QLabel(hints, this);
  hintLabel->setMinimumWidth(1);
  hintLabel->setContentsMargins(6, 0, 0, 0);
  hintLabel->setToolTip(hints);
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

  if (path.isEmpty())
    m_label->setText("No image open.\nPress O to open an image.");
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
  m_label->clearImage();
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

// S saves what the image area shows (resampling, inversion and gamma
// applied, at display resolution) as a lossless PNG next to the image, or in
// the Pictures folder if the image's folder isn't writable. Shift+S asks
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
  const bool nearest =
      m_label->interpolation() == ScaledImageLabel::Interpolation::Nearest;
  const QString zoom = QString::number(m_label->currentScale(), 'f', 3);
  QString tag = "x" + zoom + (nearest ? "_nearest" : "_bicubic");
  QString settings =
      QString("%1, %2x%3").arg(nearest ? "nearest" : "bicubic", zoom,
                               m_label->isFitToWindow() ? " (fit)" : "");
  if (m_label->isInverted()) {
    tag += "_inv";
    settings += ", inverted";
  }
  if (m_label->gammaStep() != 0) {
    const QString g = QString::number(m_label->gamma(), 'f', 2);
    tag += "_g" + g;
    settings += ", gamma " + g;
  }
  const QRect region = m_label->visibleSourceRect().toAlignedRect();

  const QFileInfo source(m_imagePath);
  QString dir = source.absolutePath();
  if (!QFileInfo(dir).isWritable()) {
    dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir))
      dir = QDir::homePath();
  }
  QString path = uniquePath(dir, source.completeBaseName() + "_view_" + tag);

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
  shot.setText("Source", m_imagePath);
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

void ImageViewer::loadImage(const QString &path) {
  m_imagePath = QFileInfo(path).absoluteFilePath();
  m_folder = QFileInfo(m_imagePath).absolutePath();
  QPixmap pixmap(m_imagePath);
  if (pixmap.isNull()) {
    m_label->clearImage();
    m_label->setText(QString("Failed to load image:\n%1").arg(m_imagePath));
  } else {
    m_label->setPixmap(pixmap); // keeps interpolation/invert/gamma settings
  }

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

  QFileInfo fi(m_imagePath);
  addRow("File size:", formatFileSize(fi.size()));

  if (!m_label->originalSize().isEmpty()) {
    int w = m_label->originalSize().width();
    int h = m_label->originalSize().height();
    addRow("Dimensions:", QString("%1 × %2 px").arg(w).arg(h));
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
  // ("1") is its tooltip when the two differ.
  auto addTag = [table](const QString &tag, const QString &value,
                        const QString &raw = QString()) {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *valueItem = new QTableWidgetItem(value);
    if (!raw.isEmpty() && raw != value)
      valueItem->setToolTip(QString("Raw value: %1").arg(raw));
    table->setItem(row, 0, new QTableWidgetItem(tag));
    table->setItem(row, 1, valueItem);
  };
  // exif gives print() context for values that depend on other EXIF tags.
  auto addData = [&addTag](const auto &data, const Exiv2::ExifData *exif) {
    for (const auto &kv : data) {
      std::string label = kv.tagLabel();
      if (label.empty())
        label = kv.tagName();
      addTag(QString::fromStdString(label),
             QString::fromStdString(kv.print(exif)),
             QString::fromStdString(kv.value().toString()));
    }
  };

  if (!m_imagePath.isEmpty()) try { // nothing to read for an empty folder
    auto image = Exiv2::ImageFactory::open(m_imagePath.toUtf8().constData());
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

void ImageViewer::toggleInterpolation() {
  using Mode = ScaledImageLabel::Interpolation;
  m_label->setInterpolation(m_label->interpolation() == Mode::Nearest
                                ? Mode::Bicubic
                                : Mode::Nearest);
  updateWindowTitle();
}

// e.g. "frame_0012.png (12/48) – Image Viewer [nearest, 3.138×]" or
// "... [nearest, inverted, γ 1.68, fit 0.421×]". The
// mode is shown because it changes what the pixels mean; three decimals so a
// near-1 wheel zoom can't be mistaken for exactly 1:1.
void ImageViewer::updateWindowTitle() {
  if (m_folder.isEmpty()) { // nothing opened yet
    setWindowTitle("Image Viewer");
    if (m_viewInfoLabel)
      m_viewInfoLabel->setText("No image open");
    return;
  }
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
  QString file = m_imagePath.isEmpty() ? QDir(m_folder).dirName() + "/"
                                       : QFileInfo(m_imagePath).fileName();
  if (m_folderIndex >= 0)
    file += QString(" (%1/%2)").arg(m_folderIndex + 1).arg(m_folderCount);
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
  if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) &&
      !m_label->canPan()) {
    if (event->key() == Qt::Key_Left) {
      showAdjacentImage(-1);
      return;
    }
    if (event->key() == Qt::Key_Right) {
      showAdjacentImage(1);
      return;
    }
  }
  QMainWindow::keyPressEvent(event);
}
