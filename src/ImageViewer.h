#pragma once

#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QPixmap>
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

protected:
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  void updateScaledPixmap();

  QPixmap m_original;
  double m_zoomFactor = 1.0;
  bool m_manualZoom = false;
};
