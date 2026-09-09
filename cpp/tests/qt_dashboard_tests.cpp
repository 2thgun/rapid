#include "../src/qt_dashboard_model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cmath>
#include <iostream>

// Loopback-only fixture: never sends UDP or writes a production recording.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const bool serve = app.arguments().contains("--serve");
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, serve ? 18080 : 0)) return 2;
  QJsonObject state{{"companion_connected", true}, {"companion_daemon_state", "driving"},
                    {"telemetry_fresh", true}, {"session_id", "qt-test"},
                    {"samples_received", 1}, {"throttle", 0.75}, {"brake", 0.25},
                    {"g_x", 0.5}, {"g_z", -0.5}, {"telemetry_age_ms", 0}};
  bool fail = false;
  QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
    auto *socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
      auto buffer = socket->property("request").toByteArray() + socket->readAll();
      if (!buffer.contains("\r\n\r\n")) { socket->setProperty("request", buffer); return; }
      const bool live = buffer.startsWith("GET /api/live ");
      const auto body = QJsonDocument(live ? state : QJsonObject{{"available", false}}).toJson(QJsonDocument::Compact);
      socket->write(QByteArray(fail && live ? "HTTP/1.1 503 Unavailable\r\n" : "HTTP/1.1 200 OK\r\n") +
                    "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
      socket->disconnectFromHost();
    });
  });
  QTimer fixture;
  QElapsedTimer elapsed; elapsed.start();
  int sequence = 1;
  QObject::connect(&fixture, &QTimer::timeout, &app, [&] {
    const double t = elapsed.elapsed()/1000.0;
    const bool driving = std::fmod(t, 16.0) < 11.0;
    state["companion_daemon_state"] = driving ? "driving" : "waiting";
    state["telemetry_fresh"] = driving;
    if (driving) {
      state["samples_received"] = ++sequence;
      state["throttle"] = (std::sin(t) + 1)/2;
      state["brake"] = (std::cos(t*1.3) + 1)/4;
      state["g_x"] = std::sin(t*0.7)*1.5;
      state["g_z"] = std::cos(t)*1.2;
    }
  });
  if (serve) {
    fixture.start(100);
    QTimer::singleShot(120000, &app, &QCoreApplication::quit);
    std::cout << "Qt fixture: http://127.0.0.1:18080\n";
    return app.exec();
  }
  DashboardModel model(QUrl("http://127.0.0.1:" + QString::number(server.serverPort())));
  const auto spin = [&](int ms) {
    QElapsedTimer wait; wait.start();
    while (wait.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  };
  const auto require = [](bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
  };
  spin(500);
  require(model.graphSamples().size() == 1, "Repeated polling duplicated a source sample");
  require(model.graphSamples().first().toMap()["throttle"].toDouble() == 75, "Pedal scale");
  state["samples_received"] = 2;
  state["g_x"] = QJsonValue::Null;
  spin(300);
  require(model.graphSamples().last().toMap()["lateral"].isNull(), "Missing channel became zero");
  state["companion_daemon_state"] = "waiting";
  spin(400);
  require(model.graphSamples().last().toMap()["throttle"].isNull(), "Idle heartbeat became telemetry");
  state["companion_daemon_state"] = "driving";
  state["samples_received"] = 3;
  spin(300);
  fail = true;
  spin(400);
  require(model.graphSamples().last().toMap()["throttle"].isNull(), "HTTP failure did not create a gap");
  fail = false;
  state["session_id"] = "qt-test-next-session";
  state["samples_received"] = 3;
  spin(300);
  require(!model.graphSamples().last().toMap()["throttle"].isNull(), "New session sample was deduplicated");
  std::cout << "Qt model polling, deduplication, null and gap checks passed\n";
}
