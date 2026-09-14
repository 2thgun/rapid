#include "../src/qt_dashboard_model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTimer>
#include <cmath>
#include <iostream>

// Loopback-only fixture: never sends UDP or writes a production recording.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const bool serve = app.arguments().contains("--serve");
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, serve ? 18080 : 0)) return 2;
  QTemporaryFile setup_status;
  if (!setup_status.open()) return 2;
  setup_status.write("{\"bootstrap\":{\"setup_address\":\"192.168.1.64\",\"setup_port\":8002,\"setup_url\":\"http://192.168.1.64:8002/setup\",\"ssid\":\"rapid-123abc\",\"access_point_password\":\"1234567890abcdef\",\"activation_token\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}");
  setup_status.flush();
  qputenv("RAPID_FIRSTBOOT_STATUS", setup_status.fileName().toUtf8());
  QTemporaryFile pairing_panel;
  if (!pairing_panel.open()) return 2;
  const auto pairing_approval = pairing_panel.fileName() + ".approval";
  pairing_panel.resize(0);
  pairing_panel.write("{\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"label\":\"Test PC\",\"code\":\"12345678\",\"expires_at\":120}\n");
  pairing_panel.flush();
  qputenv("RAPID_PAIRING_PANEL", pairing_panel.fileName().toUtf8());
  qputenv("RAPID_PAIRING_APPROVAL", pairing_approval.toUtf8());
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
  require(model.setupNotice().contains("rapid-123abc") &&
              model.setupNotice().contains("0123456789abcdef 0123456789abcdef"),
          "First-boot credentials are rendered from the local status file");
  require(model.pairingPending() && model.pairingLabel() == "Test PC" &&
              model.pairingCode() == "12345678" && model.approvePairing(),
          "Pairing panel metadata and approval action are exposed");
  require(QFile::exists(pairing_approval), "Pairing panel approval handoff is written");
  require(!model.approvePairing(), "Pairing panel approval is one-shot");
  const auto approval_permissions = QFileInfo(pairing_approval).permissions();
  require(!(approval_permissions & QFileDevice::ReadOther) &&
              !(approval_permissions & QFileDevice::WriteOther),
          "Pairing panel approval handoff is not world-readable");
  pairing_panel.resize(0);
  pairing_panel.write("{\"transaction_id\":\"0123456789abcdef0123456789abcdeg\",\"label\":\"Bad PC\",\"code\":\"12345678\"}");
  pairing_panel.flush();
  spin(600);
  require(!model.pairingPending(), "Malformed pairing transaction is ignored");
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
