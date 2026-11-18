#pragma once

#include "Resample.h"

#include <QImage>
#include <QLabel>
#include <QList>
#include <QPointF>
#include <QString>
#include <QTimer>

// The zoomable, pannable image view: resampling, the contrast stretch and
// high-pass, tone adjustments, frames and pixel value tooltips.
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

  // Show an image: its frames (one for a still image; more for an
  // animation or a multi-page file), from the first, fitted to the window.
  // Moved in, each frame is converted in place without a second copy. The
  // contrast stretch is worked out from all frames together, and the
  // high-pass keeps one gain for all frames, so a frame doesn't look
  // brighter or more detailed than another just from the processing.
  void setFrames(QList<QImage> frames);
  int frameCount() const { return int(m_frames.size()); } // 0 if none
  int frameIndex() const { return m_frameIndex; }
  // Show another frame, keeping the zoom and position (unless its size
  // differs, as pages of a TIFF can; then it's fitted to the window).
  void setFrame(int index);

  QSize originalSize() const { return m_original.size(); }

  // Pixel value tooltips: while on, resting the pointer on the image shows
  // the position and stored value of the image pixel under it.
  bool pixelTips() const { return m_pixelTips; }
  void setPixelTips(bool on);
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

  // Linked contrast stretch (see applyStretch()), applied to the image
  // before resampling, so a deeper image is stretched at full precision.
  // Step 0 is off; steps 1 to kMaxStretchStep clip increasing percentages
  // of the values at each end, starting at 0 % (darkest value to black,
  // brightest to white). Negative steps, down to kMinStretchStep, squash
  // contrast instead (see squashRange()).
  static constexpr int kMinStretchStep = -4;
  static constexpr int kMaxStretchStep = 9;
  int stretchStep() const { return m_stretchStep; }
  void setStretchStep(int step);
  double stretchClipPercent() const; // 0 unless stretching
  int squashPercent() const;          // 100 unless squashing
  // "stretch 5%", "squash 60%" or "off".
  QString stretchName() const;
  // The black and white points in use, on the 16-bit scale.
  StretchRange stretchRange() const { return m_stretchRange; }
  bool isDeep() const { return m_deep; } // more than 8 bits per channel

  // High-pass view (see applyHighPass()), applied after the stretch, also
  // before resampling. Step 0 is off; steps 1 to kMaxHighPassStep keep
  // detail up to a blur radius (sigma) of 1, 2, 4, 8 or 16 image pixels.
  static constexpr int kMaxHighPassStep = 5;
  int highPassStep() const { return m_highPassStep; }
  void setHighPassStep(int step);
  double highPassRadius() const; // 0 when off
  // The difference shown as full white or black, on a 0-1 scale of the
  // stretched values.
  double highPassLimit() const { return m_highPassLimit; }

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
  // Linear (the default) or vibrance: how the saturation factor is applied;
  // see applyTone().
  SaturationMode saturationMode() const { return m_saturationMode; }
  void setSaturationMode(SaturationMode mode);
  // "saturation" or "vibrance", for messages and saved view names.
  QString saturationModeName() const;

  // False colour, applied last: each pixel's luma (after inversion and
  // gamma) picks a colour from a perceptually uniform map, so small
  // differences in brightness show as differences in hue. Replaces the
  // saturation change while on.
  enum class FalseColor { Off, Inferno, Viridis };
  static constexpr int kFalseColorCount = 3;
  FalseColor falseColor() const { return m_falseColor; }
  void setFalseColor(FalseColor map);
  // "off", "inferno" or "viridis".
  static QString falseColorName(FalseColor map);

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
  bool event(QEvent *event) override; // pixel value tooltips

private:
  void resetView();
  void showFrame(int index); // sets m_source and its flags, no repaint
  void updateProcessed();
  void reprocess();          // updateProcessed(), then redraw
  QRectF visibleArea() const;
  QString pixelTipText(const QPoint &pixel) const;
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

  // The image as shown before resampling: m_source with the contrast
  // stretch and high-pass applied, if on. Kept as an ARGB32_Premultiplied QImage so the
  // bicubic resampler can read its pixels directly.
  QImage m_original;
  // The image with its stored values, for the stretch and the pixel value
  // tooltips: as decoded if it's deeper than 8 bits per channel or has an
  // alpha channel (translucent pixels lose precision when premultiplied),
  // otherwise converted to ARGB32_Premultiplied, which keeps the values, and
  // then sharing its data with an unstretched m_original.
  QImage m_source;
  // Every frame, in the same form as m_source, which is one of them (a
  // single image is one frame).
  QList<QImage> m_frames;
  int m_frameIndex = 0;
  int m_stretchStep = 0;
  StretchHistogram m_stretchHistogram; // of all frames; empty until needed
  StretchRange m_stretchRange;
  int m_highPassStep = 0;
  double m_highPassLimit = 0.0;
  // With several frames: the high-pass gain found for the first frame
  // processed, used for the others; 0 until then, and whenever the stretch
  // or high-pass radius changes.
  double m_sharedHighPassLimit = 0.0;
  bool m_deep = false;     // more than 8 bits per channel
  bool m_gray = false;     // all pixels grey
  bool m_hasAlpha = false;
  bool m_pixelTips = false;
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
  const ColorMap *colorMap() const; // null when false colour is off
  bool m_inverted = false;
  FalseColor m_falseColor = FalseColor::Off;
  int m_gammaStep = 0;
  int m_saturationStep = 0;
  SaturationMode m_saturationMode = SaturationMode::Linear;
  bool m_toneIsIdentity = true;
  ToneLut m_toneLut{};
  QImage m_adjustedCache;
  qint64 m_adjustedSourceKey = 0;
  QTimer m_refineTimer;
  bool m_interacting = false;
  qint64 m_lastRenderMs = 0;
};
