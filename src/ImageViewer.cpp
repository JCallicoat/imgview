

#include "ImageViewer.h"
#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QResizeEvent>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <exiv2/exiv2.hpp>

//-------------------- ScaledImageLabel --------------------

ScaledImageLabel::ScaledImageLabel(QWidget *parent) : QLabel(parent) {
  setAlignment(Qt::AlignCenter);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  setFocusPolicy(Qt::StrongFocus);
}

void ScaledImageLabel::setPixmap(const QPixmap &pixmap) {
  m_original = pixmap;
  m_zoomFactor = 1.0;
  m_manualZoom = false;
  updateScaledPixmap();
}

void ScaledImageLabel::setPixmap(const QImage &image) {
  m_original = QPixmap::fromImage(image);
  m_zoomFactor = 1.0;
  m_manualZoom = false;
  updateScaledPixmap();
}

void ScaledImageLabel::resizeEvent(QResizeEvent *event) {
  QLabel::resizeEvent(event);
  updateScaledPixmap();
}

void ScaledImageLabel::wheelEvent(QWheelEvent *event) {
  if (m_original.isNull()) {
    QLabel::wheelEvent(event);
    return;
  }

  if (!m_manualZoom) {
    // Capture the current fit-to-window scale as the starting zoom factor
    double wRatio = double(width()) / m_original.width();
    double hRatio = double(height()) / m_original.height();
    m_zoomFactor = qMin(wRatio, hRatio);
    m_manualZoom = true;
  }

  double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9;
  m_zoomFactor *= factor;
  m_zoomFactor = qBound(0.1, m_zoomFactor, 10.0);
  updateScaledPixmap();
  event->accept();
}

void ScaledImageLabel::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_Plus:
  case Qt::Key_Equal:
  case Qt::Key_Minus: {
    if (!m_manualZoom) {
      double wRatio = double(width()) / m_original.width();
      double hRatio = double(height()) / m_original.height();
      m_zoomFactor = qMin(wRatio, hRatio);
      m_manualZoom = true;
    }

    if (event->key() == Qt::Key_Minus)
      m_zoomFactor /= 1.1;
    else
      m_zoomFactor *= 1.1;

    m_zoomFactor = qBound(0.1, m_zoomFactor, 10.0);
    updateScaledPixmap();
    event->accept();
    break;
  }
  case Qt::Key_0:
    m_manualZoom = false;
    m_zoomFactor = 1.0;
    updateScaledPixmap();
    event->accept();
    break;
  default:
    QLabel::keyPressEvent(event);
    break;
  }
}

void ScaledImageLabel::updateScaledPixmap() {
  if (m_original.isNull() || size().isEmpty())
    return;

  QSize target;
  if (m_manualZoom) {
    int w = static_cast<int>(m_original.width() * m_zoomFactor);
    int h = static_cast<int>(m_original.height() * m_zoomFactor);
    target = QSize(w, h);
  } else {
    double wRatio = double(width()) / m_original.width();
    double hRatio = double(height()) / m_original.height();
    double scale = qMin(wRatio, hRatio);
    int w = int(m_original.width() * scale);
    int h = int(m_original.height() * scale);
    target = QSize(w, h);
  }
  QLabel::setPixmap(
      m_original.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
  if (!m_label->pixmap().isNull()) {
    int w = m_label->pixmap().width();
    int h = m_label->pixmap().height();
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
