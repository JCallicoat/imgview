#include "ImageViewer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QIcon>
#include <QTimer>

#include <exiv2/exiv2.hpp>

#include <functional>

namespace {

// macOS hands files opened from Finder (double-click, Open With, dropping
// onto the Dock icon) to the application as QFileOpenEvents, not as
// command-line arguments. Opening several files at once sends one event per
// file in quick succession; those are collected and opened together as a
// list, like several paths on the command line.
class Application : public QApplication {
public:
  Application(int &argc, char **argv) : QApplication(argc, argv) {
    m_batch.setSingleShot(true);
    m_batch.setInterval(150);
    QObject::connect(&m_batch, &QTimer::timeout, this, [this] {
      const QStringList paths = m_pending;
      m_pending.clear();
      if (openFiles)
        openFiles(paths);
    });
  }
  std::function<void(const QStringList &)> openFiles;

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::FileOpen) {
      auto *openEvent = static_cast<QFileOpenEvent *>(event);
      QString path = openEvent->file();
      if (path.isEmpty())
        path = openEvent->url().toLocalFile();
      if (!path.isEmpty()) {
        m_pending << path;
        m_batch.start(); // (re)start: wait for the rest of the batch
      }
      return true;
    }
    return QApplication::event(event);
  }

private:
  QStringList m_pending;
  QTimer m_batch;
};

void setUpParser(QCommandLineParser &parser) {
  parser.setApplicationDescription(
      "Image viewer for close inspection of pixels and metadata.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(
      "paths",
      "Image files or folders to open. With several, the left/right arrow "
      "keys step through just those, in the order given; with one, through "
      "its folder. Without any, a file picker opens.",
      "[path...]");
}

// --help and --version only print text, so answer them before starting the
// GUI: that way they also work without a display (e.g. over SSH).
void handleInfoOptions(int &argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--help" || arg == "-h" || arg == "-?" || arg == "--help-all" ||
        arg == "--version" || arg == "-v") {
      QCoreApplication core(argc, argv);
      QCommandLineParser parser;
      setUpParser(parser);
      parser.process(core); // prints and exits
    }
  }
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication::setApplicationName(QStringLiteral("imgview"));
  QCoreApplication::setApplicationVersion(QStringLiteral(IMGVIEW_VERSION));
  handleInfoOptions(argc, argv);

  Exiv2::XmpParser::initialize();
  std::atexit(Exiv2::XmpParser::terminate);

  Application app(argc, argv);
  // On Linux the icon comes from the icon theme. macOS has no such theme;
  // there the app bundle's own icon is used instead.
  const QIcon icon = QIcon::fromTheme(QStringLiteral("image-x-generic"));
  if (!icon.isNull())
    app.setWindowIcon(icon);
  // Matches the installed imgview.desktop, so Wayland compositors can find
  // the app's icon and name from it.
  app.setDesktopFileName(QStringLiteral("imgview"));

  QCommandLineParser parser;
  setUpParser(parser);
  parser.process(app);
  const QStringList args = parser.positionalArguments();

  ImageViewer viewer(args.size() == 1 ? args.first() : QString());
  if (args.size() > 1)
    viewer.openPaths(args);
  app.openFiles = [&viewer](const QStringList &paths) {
    viewer.openPaths(paths);
  };
  viewer.show();

  if (args.isEmpty()) {
    // Nothing given: offer a file picker. On macOS, wait a moment first, since
    // a file opened from Finder arrives as an event just after launch.
#ifdef Q_OS_MACOS
    const int delayMs = 500;
#else
    const int delayMs = 0;
#endif
    QTimer::singleShot(delayMs, &viewer, [&viewer] {
      if (!viewer.hasOpenedSomething())
        viewer.showOpenDialog();
    });
  }
  return app.exec();
}
