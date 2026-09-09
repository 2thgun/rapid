#include "qt_dashboard_model.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

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
  return application.exec();
}
