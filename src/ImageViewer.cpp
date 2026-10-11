

#include "ImageViewer.h"
#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
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
} // namespace

ScaledImageLabel::ScaledImageLabel(QWidget *parent) : QLabel(parent) {
  setAlignment(Qt::AlignCenter);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  setFocusPolicy(Qt::StrongFocus);
}

void ScaledImageLabel::setPixmap(const QPixmap &pixmap) {
  m_original = pixmap;
  resetView();
}

void ScaledImageLabel::setPixmap(const QImage &image) {
  m_original = QPixmap::fromImage(image);
  resetView();
}

void ScaledImageLabel::resetView() {
  m_zoomFactor = 1.0;
  m_manualZoom = false;
  m_dragging = false;
  m_scaledCache = QPixmap();
  clampOffset();
  updateCursor();
  update();
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

  clampOffset();
  updateCursor();
  update();
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
    // original). QPixmap::scaled() filters better than the painter's
    // bilinear sampling when shrinking a lot.
    const QSize px = (QSizeF(m_original.size()) * scale * dpr).toSize();
    if (m_scaledCache.isNull() || m_scaledCache.size() != px) {
      m_scaledCache = m_original.scaled(px, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);
      m_scaledCache.setDevicePixelRatio(dpr);
    }
    painter.drawPixmap(m_offset, m_scaledCache);
  } else {
    // Zoomed in: draw only the visible part of the original, scaled up by
    // the painter. Memory use no longer grows with the zoom level.
    m_scaledCache = QPixmap();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF target(m_offset, scaledSize());
    const QRectF visible = target.intersected(QRectF(rect()));
    if (visible.isEmpty())
      return;
    const QRectF source((visible.topLeft() - m_offset) / scale,
                        visible.size() / scale);
    painter.drawPixmap(visible, m_original, source);
  }
}

void ScaledImageLabel::resizeEvent(QResizeEvent *event) {
  QLabel::resizeEvent(event);
  // Keep the same part of the image at the center of the view.
  if (event->oldSize().isValid()) {
    const QSizeF d = QSizeF(event->size() - event->oldSize()) / 2.0;
    m_offset += QPointF(d.width(), d.height());
  }
  clampOffset();
  updateCursor();
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

void ScaledImageLabel::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_Plus:
  case Qt::Key_Equal:
  case Qt::Key_Minus: {
    const double factor =
        (event->key() == Qt::Key_Minus) ? 1.0 / kZoomStep : kZoomStep;
    zoomAt(currentScale() * factor, QRectF(rect()).center());
    event->accept();
    break;
  }
  case Qt::Key_0:
    resetView();
    event->accept();
    break;
  default:
    QLabel::keyPressEvent(event);
    break;
  }
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

void ScaledImageLabel::mouseMoveEvent(QMouseEvent *event) {
  if (m_dragging) {
    const QPointF pos = event->position();
    m_offset += pos - m_lastMousePos;
    m_lastMousePos = pos;
    clampOffset();
    update();
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

ImageViewer::ImageViewer(const QString &imagePath, QWidget *parent)
    : QMainWindow(parent), m_imagePath(imagePath) {
  setWindowTitle("Image Viewer");
  resize(800, 600);

  QWidget *central = new QWidget(this);
  setCentralWidget(central);
  QHBoxLayout *mainLayout = new QHBoxLayout(central);

  m_label = new ScaledImageLabel(this);
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

  // File size
  QFileInfo fi(m_imagePath);
  QString sizeStr =
      fi.size() > 0 ? QString("%1 bytes").arg(fi.size()) : QString("N/A");
  layout->addWidget(new QLabel(QString("<b>File size:</b> %1").arg(sizeStr)));

  // Dimensions
  if (!m_label->originalSize().isEmpty()) {
    int w = m_label->originalSize().width();
    int h = m_label->originalSize().height();
    layout->addWidget(
        new QLabel(QString("<b>Dimensions:</b> %1 × %2 px").arg(w).arg(h)));
  }

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

void ImageViewer::toggleInfoPanel() {
  if (!m_infoPanel) {
    createInfoPanel();
  } else {
    m_infoPanel->setVisible(!m_infoPanel->isVisible());
  }
}

void ImageViewer::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_Escape:
  case Qt::Key_Q:
    close();
    break;
  case Qt::Key_M:
    toggleInfoPanel();
    break;
  default:
    QMainWindow::keyPressEvent(event);
    break;
  }
}
