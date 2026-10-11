#include "ImageViewer.h"

#include <QApplication>

#include <exiv2/exiv2.hpp>

int main(int argc, char *argv[]) {

  Exiv2::XmpParser::initialize();
  std::atexit(Exiv2::XmpParser::terminate);

  QApplication app(argc, argv);
  if (argc < 2) {
    qWarning("Usage: %s <image_path>", argv[0]);
    return 1;
  }
  ImageViewer viewer(argv[1]);
  viewer.show();
  return app.exec();
}
