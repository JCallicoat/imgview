#pragma once

#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QTableWidget>

class ScaledImageLabel;

class ImageViewer : public QMainWindow {
  Q_OBJECT
public:
  explicit ImageViewer(const QString &imagePath, QWidget *parent = nullptr);
  ~ImageViewer() override = default;

protected:
  void keyPressEvent(QKeyEvent *event) override;

private:
  void createInfoPanel();
  void toggleInfoPanel();

  QString m_imagePath;
  ScaledImageLabel *m_label;
  QWidget *m_infoPanel = nullptr;
};

class ScaledImageLabel : public QLabel {
  Q_OBJECT
public:
  explicit ScaledImageLabel(QWidget *parent = nullptr);
  void setPixmap(const QPixmap &pixmap);

  void setPixmap(const QImage &image);

  QSize originalSize() const { return m_original.size(); }

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  void resetView();
  double fitScale() const;
  double currentScale() const;
  QSizeF scaledSize() const;
  bool canPan() const;
  void clampOffset();
  void zoomAt(double newScale, const QPointF &anchor);
  void updateCursor();

  QPixmap m_original;
  double m_zoomFactor = 1.0;
  bool m_manualZoom = false;

  // Top-left corner of the scaled image in widget coordinates. When the
  // scaled image is larger than the widget this is <= 0 and acts as the
  // scroll position of the virtual viewport.
  QPointF m_offset;
  bool m_dragging = false;
  QPointF m_lastMousePos;

  // High-quality downscaled copy, only used when zoomed out (scale <= 1).
  QPixmap m_scaledCache;
};
