#pragma once

#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMainWindow>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

class ScaledImageLabel;

class ImageViewer : public QMainWindow {
  Q_OBJECT
public:
  // `path` may be an image file, a folder, or empty (start with nothing
  // open; O opens a file picker, Ctrl+V pastes an image).
  explicit ImageViewer(const QString &path = QString(),
                       QWidget *parent = nullptr);
  ~ImageViewer() override = default;

  void openPath(const QString &path); // an image file, or a folder
  // Open several paths; ←/→ then step through just these, in the order
  // given (folders contribute their images). One path: same as openPath().
  void openPaths(const QStringList &paths);
  void showOpenDialog();              // pick an image file to open
  // Show the image on the clipboard (e.g. copied in a web browser) from
  // memory, in place of any open image. ←/→ are off until a file is opened.
  void pasteImage();
  // Show an image read from standard input (`imgview -`, or piped in with
  // no paths), from memory like a pasted one; there's nothing to browse.
  // An animation's frames can be stepped through as usual.
  void openStdinData(const QByteArray &data);
  // Save the visible image area as shown; ask for the location or not.
  void saveView(bool chooseLocation);

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
  void stepStretch(int delta);
  void stepHighPass(int delta);
  void toggleSaturationMode();
  void stepFalseColor(int delta);
  void togglePixelTips();
  void resetTone();
  void toggleHelpPanel();
  void updateHelpPanel();
  void updateWindowTitle();
  // A setting that's on, as shown in the title ("γ 1.68"), described in a
  // saved view's PNG text ("gamma 1.68") and tagged in its name ("g1.68").
  struct ActiveSetting {
    QString title, description, tag;
  };
  QList<ActiveSetting> activeSettings() const;
  void updateHints(); // the status bar's key hints, for the image shown

  // Image loading and folder navigation.
  void loadImage(const QString &path);
  void showAdjacentImage(int delta); // -1 = previous, +1 = next
  void refreshInfoPanel();
  QStringList imageFilesInFolder() const;
  static QStringList imageFilesIn(const QString &folder);
  void showAdjacentInList(int delta);

  // Frames of an animation or multi-page file: ←/→ step through them,
  // Home/End go to the first and last, Space plays and pauses.
  // readFrames() decodes all of a file's frames (asking first if they'd
  // take a lot of memory); showFrames() shows them.
  struct LoadedFrames {
    QList<QImage> images; // empty if nothing could be read
    QList<int> delays;    // ms per frame, 0 when not given
    bool animated = false; // an animation, not pages of a document
    int total = 0;         // frames in the source; more than read if only
                           // the first was
    QByteArray format;     // the reader's format, e.g. "png"
  };
  LoadedFrames readFrames(const QString &path);
  LoadedFrames readFramesFromData(const QByteArray &data,
                                  const QByteArray &format, const QString &name);
  // The same from any source: makeReader() returns a new reader positioned
  // at the start of the image. `name` is used in messages.
  LoadedFrames readFrames(
      const std::function<std::unique_ptr<QImageReader>()> &makeReader,
      const QString &name);
  void showFrames(LoadedFrames frames); // sets the frame fields below
  void showMemoryImage(LoadedFrames frames, bool fromStdin, const QString &type,
                       const QByteArray &data);
  void stepFrame(int delta);
  void goToFrame(int index); // pauses playback
  void togglePlayback();
  void stopPlayback();
  void playNextFrame();
  int frameDelayMs(int index) const;
  QString frameDelaysText() const; // for the info panel

  bool m_animated = false; // an animation, not pages of a document
  int m_frameTotal = 0;    // frames in the file; more than loaded if only
                           // the first was
  QList<int> m_frameDelays; // ms each frame is shown; 0 when not given
  bool m_playing = false;
  QTimer m_playTimer;       // single shot, for the next frame

  QString m_imagePath; // empty when a folder with no images was opened,
                       // or for an image held in memory
  QString m_folder;    // absolute path of the folder being browsed; empty
                       // until something has been opened
  // Set when several paths were given: ←/→ step through these (absolute
  // paths) instead of the folder. Empty means folder browsing.
  QStringList m_list;
  int m_folderIndex = -1; // position of m_imagePath in its folder or the
  int m_folderCount = 0;  // list, 0-based, and the number of images there
  // While an image held in memory is shown (pasted, or read from standard
  // input): where it came from, "Clipboard" or "stdin"; the type it was
  // read as (e.g. "image/png" from the clipboard, "png" from stdin); and its
  // encoded bytes, kept for the info panel's data size and metadata. The
  // type and data are empty if the clipboard only offered an already
  // decoded image.
  bool m_inMemory = false;
  bool m_fromStdin = false;
  QString memorySourceName() const {
    return m_fromStdin ? "stdin" : "Clipboard";
  }
  QString m_memoryType;
  QByteArray m_memoryData;
  ScaledImageLabel *m_label = nullptr;
  QWidget *m_infoPanel = nullptr;
  // Key reference on the left, created on first use. m_helpLabel holds its
  // text, which shows the current view settings.
  QScrollArea *m_helpPanel = nullptr;
  QLabel *m_helpLabel = nullptr;
  // Status bar copy of the title's file/view info, shown only in fullscreen
  // (where there's no title bar).
  QLabel *m_viewInfoLabel = nullptr;
  QLabel *m_hintLabel = nullptr; // key hints on the left of the status bar
};
