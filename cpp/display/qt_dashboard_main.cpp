#include "qt_dashboard_model.hpp"

#include <QCommandLineParser>
#include <QCursor>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTransform>

namespace {
// #13: the Qt panel rotates its scene in software, but the X cursor is a
// separate overlay that X keeps upright, so at 180 degrees it appears upside
// down over the rotated picture. Rotating the X server itself on this fbdev
// panel needs an X restart and a rotation-dependent input matrix (see the
// handoff), so instead the panel replaces the cursor with an arrow drawn at the
// same angle while it is rotated, and restores the normal X cursor at 0.
QCursor rotation_cursor(int rotation) {
  if (rotation != 180) return QCursor(Qt::ArrowCursor);
  // A themed cursor cannot be read back as a pixmap, so draw a standard arrow.
  QPixmap arrow(24, 24);
  arrow.fill(Qt::transparent);
  QPainter painter(&arrow);
  QPolygonF shape;
  shape << QPointF(1, 1) << QPointF(1, 16) << QPointF(4.5, 12.5) << QPointF(7.5, 19)
        << QPointF(10, 18) << QPointF(7, 11.5) << QPointF(12, 11.5);
  painter.setPen(QPen(Qt::black, 1.5));
  painter.setBrush(Qt::white);
  painter.drawPolygon(shape);
  painter.end();
  const QPixmap turned = arrow.transformed(QTransform().rotate(180), Qt::FastTransformation);
  // The tip must stay under the pointer, so mirror the hotspot with the image.
  const QPoint hotspot(0, 0);
  return QCursor(turned, arrow.width() - 1 - hotspot.x(), arrow.height() - 1 - hotspot.y());
}
}  // namespace

int main(int argc, char *argv[]) {
  QGuiApplication application(argc, argv);
  QCoreApplication::setApplicationName("rapid-qt-display");
  QCommandLineParser parser;
  parser.addHelpOption();
  QCommandLineOption endpoint_option("endpoint", "Local rapid-pi HTTP endpoint.", "url",
                                     qEnvironmentVariable("RAPID_DASHBOARD_URL", "http://127.0.0.1:8000"));
  parser.addOption(endpoint_option);
  parser.process(application);
  const QUrl endpoint(parser.value(endpoint_option));
  if (!endpoint.isValid() || endpoint.scheme() != "http" || endpoint.host().isEmpty()) {
    qCritical("--endpoint must be an HTTP URL with a host");
    return 2;
  }

  DashboardModel dashboard(endpoint);
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("dashboard", &dashboard);
  engine.load(QUrl(QStringLiteral("qrc:/display/Main.qml")));
  if (engine.rootObjects().isEmpty()) return 1;

  // Keep the pointer consistent with the rotated scene (#13). The rotation is
  // polled, so track the last value applied and only touch the cursor when it
  // actually changes; the model's `changed` notification covers that poll.
  int applied_rotation = dashboard.displayRotation();
  bool cursor_overridden = false;
  const auto apply_rotation_cursor = [&](int rotation) {
    if (rotation == applied_rotation) return;
    applied_rotation = rotation;
    if (rotation == 180) {
      if (cursor_overridden) {
        QGuiApplication::changeOverrideCursor(rotation_cursor(rotation));
      } else {
        QGuiApplication::setOverrideCursor(rotation_cursor(rotation));
        cursor_overridden = true;
      }
    } else if (cursor_overridden) {
      QGuiApplication::restoreOverrideCursor();
      cursor_overridden = false;
    }
  };
  QObject::connect(&dashboard, &DashboardModel::changed, &application,
                   [&] { apply_rotation_cursor(dashboard.displayRotation()); });
  apply_rotation_cursor(dashboard.displayRotation());
  return application.exec();
}
