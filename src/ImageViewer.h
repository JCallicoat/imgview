#pragma once

#include "Resample.h"

#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>

class ScaledImageLabel;

class ImageViewer : public QMainWindow {
  Q_OBJECT
public:
  // `path` may be an image file, a folder, or empty (start with nothing
  // open; see showOpenDialog()).
  explicit ImageViewer(const QString &path = QString(),
                       QWidget *parent = nullptr);
  ~ImageViewer() override = default;

  void openPath(const QString &path); // an image file, or a folder
  // Open several paths; ←/→ then step through just these, in the order
  // given (folders contribute their images). One path: same as openPath().
  void openPaths(const QStringList &paths);
  void showOpenDialog();              // pick an image file to open
  // Save the visible image area as shown; ask for the location or not.
  void saveView(bool chooseLocation);
  bool hasOpenedSomething() const { return !m_folder.isEmpty(); }

protected:
  void keyPressEvent(QKeyEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  bool handleShortcutKey(QKeyEvent *event);
  void createInfoPanel();
  void toggleInfoPanel();
  void toggleInterpolation();
  void toggleFullScreen();
  void toggleInverted();
  void stepGamma(int delta);
  void stepSaturation(int delta);
  void resetTone();
  void toggleHelpPanel();
  void updateHelpPanel();
  void updateWindowTitle();

  // Image loading and folder navigation.
  void loadImage(const QString &path);
  void showAdjacentImage(int delta); // -1 = previous, +1 = next
  void refreshInfoPanel();
  QStringList imageFilesInFolder() const;
  static QStringList imageFilesIn(const QString &folder);
  void showAdjacentInList(int delta);

  QString m_imagePath; // empty when a folder with no images was opened
  QString m_folder;    // absolute path of the folder being browsed; empty
                       // until something has been opened
  // Set when several paths were given: ←/→ step through these (absolute
  // paths) instead of the folder. Empty means folder browsing.
  QStringList m_list;
  int m_folderIndex = -1; // position of m_imagePath in its folder or the
  int m_folderCount = 0;  // list, 0-based, and the number of images there
  ScaledImageLabel *m_label = nullptr;
  QWidget *m_infoPanel = nullptr;
  // Key reference on the left, created on first use. m_helpLabel holds its
  // text, which shows the current view settings.
  QScrollArea *m_helpPanel = nullptr;
  QLabel *m_helpLabel = nullptr;
  // Status bar copy of the title's file/view info, shown only in fullscreen
  // (where there's no title bar).
  QLabel *m_viewInfoLabel = nullptr;
};

class ScaledImageLabel : public QLabel {
  Q_OBJECT
public:
  // How the image is resampled when zoomed in past 100%. Zooming out always
  // uses a smooth (area-averaging) downscale.
  enum class Interpolation {
    Nearest,  // stored pixel values, unmodified (default)
    Bicubic,  // smoother, interpolated
    Lanczos3, // sharper interpolation than bicubic, slight edge ringing
  };
  // "nearest", "bicubic" or "lanczos3".
  static QString interpolationName(Interpolation mode);
  // The resampling actually applied at the current zoom, as shown in the
  // title and saved file names: "area" when zoomed out (any mode), else the
  // selected mode's name. At exactly 1:1 every mode shows the stored pixels.
  QString resamplingName() const;

  explicit ScaledImageLabel(QWidget *parent = nullptr);
  void setPixmap(const QPixmap &pixmap);

  void setPixmap(const QImage &image);

  QSize originalSize() const { return m_original.size(); }
  // Whether the image at the current zoom is wider or taller than the view.
  bool canPan() const;

  // Drop the current image (e.g. before showing a "Failed to load" text).
  void clearImage();

  // What the image area shows right now, as an image at display resolution:
  // the visible part of the image with resampling and tone adjustments
  // applied, without the surrounding background. Null if nothing is shown.
  QImage renderVisible();
  // The part of the original image that is visible, in image pixels.
  QRectF visibleSourceRect() const;

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

  // Display tone adjustments, applied after resampling: a saturation
  // change, optional colour inversion, then a gamma curve.
  bool isInverted() const { return m_inverted; }
  void setInverted(bool inverted);

  // Gamma moves in equal multiplicative steps: gamma = 2^(step / 4), with
  // step in [-kMaxGammaStep, kMaxGammaStep]. Positive steps darken (and
  // stretch contrast among bright values), negative steps brighten.
  static constexpr int kMaxGammaStep = 12;
  int gammaStep() const { return m_gammaStep; }
  void setGammaStep(int step);
  double gamma() const;

  // Saturation factor (1 = unchanged, the default) set in steps, step in
  // [kMinSaturationStep, kMaxSaturationStep]. Negative steps reduce it
  // linearly by 0.1 each, down to 0 (grey). Positive steps multiply it by
  // 2^(1/4) each, like gamma, so it doubles every 4 steps up to 16x, enough
  // to bring out the faint colour differences in e.g. lunar images.
  static constexpr int kMinSaturationStep = -10;
  static constexpr int kMaxSaturationStep = 16;
  int saturationStep() const { return m_saturationStep; }
  void setSaturationStep(int step);
  double saturation() const;
  int saturationPercent() const { return qRound(saturation() * 100); }

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
  // Whether the view is drawn from the area-averaged downscale rather than
  // the selected resampler: at or below one image pixel per device pixel.
  bool isDownscaled() const;
  double fitScale() const;
  QSizeF scaledSize() const;
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

  // Tone adjustments (saturation, inversion + gamma) are applied to the
  // resampled image just before drawing, through applyTone() with
  // m_toneLut. m_adjustedCache holds the adjusted copy of the render whose
  // cacheKey() is m_adjustedSourceKey; it's cleared whenever the tone
  // settings change.
  void rebuildToneLut();
  bool m_inverted = false;
  int m_gammaStep = 0;
  int m_saturationStep = 0;
  bool m_toneIsIdentity = true;
  ToneLut m_toneLut{};
  QImage m_adjustedCache;
  qint64 m_adjustedSourceKey = 0;
  QTimer m_refineTimer;
  bool m_interacting = false;
  qint64 m_lastRenderMs = 0;
};
