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
// command-line arguments.
class Application : public QApplication {
public:
  using QApplication::QApplication;
  std::function<void(const QString &)> openFile;

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::FileOpen) {
      auto *openEvent = static_cast<QFileOpenEvent *>(event);
      QString path = openEvent->file();
      if (path.isEmpty())
        path = openEvent->url().toLocalFile();
      if (!path.isEmpty() && openFile)
        openFile(path);
      return true;
    }
    return QApplication::event(event);
  }
};

void setUpParser(QCommandLineParser &parser) {
  parser.setApplicationDescription(
      "Image viewer for close inspection of pixels and EXIF metadata.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(
      "path", "Image file or folder to open. Without one, a file picker opens.",
      "[path]");
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

  ImageViewer viewer(args.isEmpty() ? QString() : args.first());
  app.openFile = [&viewer](const QString &path) { viewer.openPath(path); };
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
