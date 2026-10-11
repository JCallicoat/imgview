#pragma once

#include "Resample.h"

#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QTableWidget>
#include <QTimer>

class ScaledImageLabel;

class ImageViewer : public QMainWindow {
  Q_OBJECT
public:
  explicit ImageViewer(const QString &imagePath, QWidget *parent = nullptr);
  ~ImageViewer() override = default;

protected:
  void keyPressEvent(QKeyEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  bool handleShortcutKey(QKeyEvent *event);
  void createInfoPanel();
  void toggleInfoPanel();
  void toggleInterpolation();
  void toggleFullScreen();
  void toggleInverted();
  void stepGamma(int delta);
  void updateWindowTitle();

  QString m_imagePath;
  ScaledImageLabel *m_label = nullptr;
  QWidget *m_infoPanel = nullptr;
};

class ScaledImageLabel : public QLabel {
  Q_OBJECT
public:
  // How the image is resampled when zoomed in past 100%. Zooming out always
  // uses a smooth (area-averaging) downscale.
  enum class Interpolation {
    Nearest, // stored pixel values, unmodified (default)
    Bicubic, // smoother, interpolated
  };

  explicit ScaledImageLabel(QWidget *parent = nullptr);
  void setPixmap(const QPixmap &pixmap);

  void setPixmap(const QImage &image);

  QSize originalSize() const { return m_original.size(); }

  // Keyboard zoom, anchored at the center of the view.
  void zoomIn();
  void zoomOut();
  void resetZoom();

  // Zoom to exactly `scale` (1.0 = one image pixel per logical screen
  // pixel), keeping the image point under `anchor` in place.
  void setScale(double scale, const QPointF &anchor);

  // Current image pixels -> logical screen pixels factor.
  double currentScale() const;
  bool isFitToWindow() const { return !m_manualZoom; }

  Interpolation interpolation() const { return m_interpolation; }
  void setInterpolation(Interpolation mode);

  // Display tone adjustments, applied after resampling: optional colour
  // inversion, then a gamma curve.
  bool isInverted() const { return m_inverted; }
  void setInverted(bool inverted);

  // Gamma moves in equal multiplicative steps: gamma = 2^(step / 4), with
  // step in [-kMaxGammaStep, kMaxGammaStep]. Positive steps darken (and
  // stretch contrast among bright values), negative steps brighten.
  static constexpr int kMaxGammaStep = 12;
  int gammaStep() const { return m_gammaStep; }
  void setGammaStep(int step);
  double gamma() const;

signals:
  // Scale, fit state or interpolation mode may have changed.
  void viewChanged();

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
  QSizeF scaledSize() const;
  bool canPan() const;
  void clampOffset();
  void zoomAt(double newScale, const QPointF &anchor);
  void updateCursor();
  void markInteracting();
  void panBy(const QPointF &delta);
  const QImage &forDisplay(const QImage &rendered);

  // Kept as an ARGB32_Premultiplied QImage so the bicubic resampler can read
  // its pixels directly.
  QImage m_original;
  double m_zoomFactor = 1.0;
  bool m_manualZoom = false;

  // Top-left corner of the scaled image in widget coordinates. When the
  // scaled image is larger than the widget this is <= 0 and acts as the
  // scroll position of the virtual viewport.
  QPointF m_offset;
  bool m_dragging = false;
  QPointF m_lastMousePos;

  // High-quality downscaled copy, only used when zoomed out (scale <= 1).
  QImage m_scaledCache;

  // Resampled rendering of the visible part when zoomed in, plus what it was
  // rendered for, so plain repaints don't redo the resampling.
  QImage m_viewportCache;
  QPointF m_viewportCachePos;
  QPointF m_viewportCacheOrigin;
  double m_viewportCacheStep = 0.0;

  // While panning/zooming, if rendering is too slow to keep up (last render
  // took longer than a frame), draw a fast preview with QPainter and do the
  // full render once the view has been still for a moment.
  Interpolation m_interpolation = Interpolation::Nearest;

  // Tone adjustments (inversion + gamma) are applied to the resampled image
  // just before drawing, through m_toneLut. m_adjustedCache holds the
  // adjusted copy of the render whose cacheKey() is m_adjustedSourceKey;
  // it's cleared whenever the tone settings change.
  void rebuildToneLut();
  bool m_inverted = false;
  int m_gammaStep = 0;
  bool m_toneIsIdentity = true;
  ToneLut m_toneLut{};
  QImage m_adjustedCache;
  qint64 m_adjustedSourceKey = 0;
  QTimer m_refineTimer;
  bool m_interacting = false;
  qint64 m_lastRenderMs = 0;
};
