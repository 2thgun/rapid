#include "../qt_dashboard_model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QPointF>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
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
  // #22: SSID is no longer part of the first-boot bootstrap document (it is
  // resolved and published separately by the privileged provisioner, since
  // only it can scan for a same-name AP already in range); no AP passphrase
  // is generated at all for the now-open network.
  setup_status.write("{\"bootstrap\":{\"setup_address\":\"192.168.1.64\",\"setup_port\":8002,\"setup_url\":\"http://192.168.1.64:8002/setup\",\"certificate_fingerprint\":\"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\",\"activation_token\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}");
  setup_status.flush();
  qputenv("RAPID_FIRSTBOOT_STATUS", setup_status.fileName().toUtf8());
  QTemporaryFile network_ssid;
  if (!network_ssid.open()) return 2;
  network_ssid.write("rapid\n");
  network_ssid.flush();
  qputenv("RAPID_NETWORK_SSID", network_ssid.fileName().toUtf8());
  QTemporaryFile pairing_panel;
  if (!pairing_panel.open()) return 2;
  const auto pairing_approval = pairing_panel.fileName() + ".approval";
  pairing_panel.resize(0);
  pairing_panel.write("{\"transaction_id\":\"0123456789abcdef0123456789abcdef\",\"label\":\"Test PC\",\"code\":\"12345678\",\"expires_at\":120}\n");
  pairing_panel.flush();
  qputenv("RAPID_PAIRING_PANEL", pairing_panel.fileName().toUtf8());
  qputenv("RAPID_PAIRING_APPROVAL", pairing_approval.toUtf8());
  // Fake constrained display helper: records arguments, fails --calibrate on request.
  QTemporaryDir helper_directory;
  if (!helper_directory.isValid()) return 2;
  const auto helper = helper_directory.filePath("display-recovery");
  const auto helper_log = helper_directory.filePath("calls");
  const auto helper_fail = helper_directory.filePath("fail");
  const auto calibration_path = helper_directory.filePath("touch-calibration.conf");
  {
    QFile script(helper);
    if (!script.open(QIODevice::WriteOnly)) return 2;
    script.write(QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$*\" >> '%1'\n"
                                "case \"$*\" in *--calibrate*) [ -f '%2' ] && { echo 'calibration taps are inconsistent' >&2; exit 1; };; esac\n"
                                "exit 0\n").arg(helper_log, helper_fail).toUtf8());
    script.close();
    script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }
  qputenv("RAPID_DISPLAY_RECOVERY", helper.toUtf8());
  qputenv("RAPID_DISPLAY_STATE", helper_directory.filePath("display-state.json").toUtf8());
  qputenv("RAPID_TOUCH_CALIBRATION", calibration_path.toUtf8());
  const auto calibration_request = helper_directory.filePath("calibration-request.json");
  qputenv("RAPID_CALIBRATION_REQUEST", calibration_request.toUtf8());
  const auto apply_result = helper_directory.filePath("apply-result.json");
  const auto display_confirm = helper_directory.filePath("display-confirm.json");
  qputenv("RAPID_APPLY_RESULT", apply_result.toUtf8());
  qputenv("RAPID_DISPLAY_CONFIRM", display_confirm.toUtf8());
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
  // #13: the panel renders the orientation rapid-display-recovery persisted,
  // read from the existing state file, rather than an X/xrandr transform.
  const auto display_state = helper_directory.filePath("display-state.json");
  const auto write_display = [&](int rotation) {
    QFile file(display_state);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write display state");
    const QJsonObject object{{"rotation", rotation}, {"pending", false}};
    require(file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0, "write display state");
    file.close();
  };
  require(model.displayRotation() == 0,
          "The panel defaults to 0 degrees when no display state file exists");
  write_display(180);
  spin(700);
  require(model.displayRotation() == 180,
          "#13: the panel reads the persisted 180-degree rotation from the state file");
  write_display(0);
  spin(700);
  require(model.displayRotation() == 0,
          "#13: the panel follows the persisted rotation back to 0 degrees");
  require(model.setupNotice().contains("SETUP AP  rapid  (open network)") &&
              model.setupNotice().contains("fedcba9876543210 fedcba9876543210") &&
              model.setupNotice().contains("0123456789abcdef 0123456789abcdef") &&
              !model.setupNotice().contains("PASSWORD"),
          "Setup card/panel shows the open network's SSID, address, TLS fingerprint and "
          "activation token, with no passphrase");
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

  // #15: a paused daemon heartbeat keeps the recording open (the Pi side
  // already reports "paused" distinctly from "driving"/"waiting"); the panel
  // must show a distinct paused status rather than reading as idle or
  // "waiting for driving".
  state["companion_daemon_state"] = "paused";
  state["simulator"] = "AC1";
  spin(400);
  const auto paused_status = model.status();
  require(paused_status.contains("paused", Qt::CaseInsensitive),
          "A paused daemon heartbeat shows a status naming 'paused'");
  require(!paused_status.contains("waiting", Qt::CaseInsensitive) &&
              !paused_status.contains("idle", Qt::CaseInsensitive),
          "The paused status must not read as idle/waiting for driving (#15)");
  state["companion_daemon_state"] = "waiting";
  spin(400);
  require(model.status().contains("waiting for driving", Qt::CaseInsensitive),
          "A genuinely idle daemon (not paused) still shows waiting for driving");
  state["companion_daemon_state"] = "driving";
  state.remove("simulator");
  spin(300);

  const auto calls = [&] {
    QFile file(helper_log);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
  };
  const QPointF taps[] = {{0.12, 0.09}, {0.88, 0.11}, {0.91, 0.92}, {0.1, 0.88}, {0.5, 0.52}};
  const auto capture = [&] {
    model.startCalibration();
    for (const auto &tap : taps) model.calibrationTap(tap.x(), tap.y());
    spin(600);
  };
  model.startCalibration();
  require(model.calibrationStage() == "capture" && model.calibrationTarget() == QPointF(0.1, 0.1),
          "Calibration starts at the first inset target");
  for (const auto &tap : taps) model.calibrationTap(tap.x(), tap.y());
  spin(600);
  require(model.calibrationStage() == "verify" && calls().contains("--calibrate") &&
              calls().contains("--calibration-file " + calibration_path) &&
              calls().contains("--sample 0.120000,0.090000,0.100000,0.100000") &&
              calls().contains("--sample 0.500000,0.520000,0.500000,0.500000") &&
              model.calibrationTarget() == QPointF(0.3, 0.7),
          "Five taps are submitted as normalized samples before verification");
  model.calibrationTap(0.31, 0.69);
  spin(600);
  require(model.calibrationStage() == "done" && calls().contains("--confirm-calibration") &&
              !calls().contains("--rollback-calibration"),
          "An accurate verification tap confirms the calibration");
  model.calibrationTap(0.5, 0.5);
  require(model.calibrationStage().isEmpty(), "A calibration result can be dismissed");
  capture();
  model.calibrationTap(0.8, 0.2);
  spin(600);
  require(model.calibrationStage() == "failed" && calls().contains("--rollback-calibration"),
          "A missed verification tap rolls back the new calibration");
  model.calibrationTap(0.5, 0.5);
  // #13 touch mapping: at 180 degrees the rotated scene delivers local
  // coordinates, so the panel maps both the tap and the target into the
  // unrotated screen frame before calibrating; the stored matrix then stays
  // orientation-independent and the X matrix needs no rotation.
  write_display(180);
  spin(700);
  require(model.displayRotation() == 180, "rotation is 180 for the touch mapping check");
  model.startCalibration();
  model.calibrationTap(0.12, 0.09);
  model.calibrationTap(0.88, 0.11);
  model.calibrationTap(0.91, 0.92);
  model.calibrationTap(0.10, 0.88);
  model.calibrationTap(0.50, 0.52);
  spin(600);
  require(calls().contains("--sample 0.880000,0.910000,0.900000,0.900000") &&
              calls().contains("--sample 0.500000,0.480000,0.500000,0.500000"),
          "#13: at 180 degrees local taps and targets are mapped to the unrotated screen frame");
  model.calibrationTap(0.3, 0.7);
  spin(600);
  require(model.calibrationStage() == "done", "The mapped 180-degree calibration still verifies");
  model.calibrationTap(0.5, 0.5);
  write_display(0);
  spin(700);
  { QFile flag(helper_fail); require(flag.open(QIODevice::WriteOnly), "create helper failure flag"); }
  capture();
  require(model.calibrationStage() == "failed" && model.calibrationMessage().contains("inconsistent"),
          "Rejected calibration samples report the helper's reason");
  model.calibrationTap(0.5, 0.5);
  require(!calls().contains("--apply-input"), "Panel-owned calibration changes are not re-applied");
  {
    QFile file(calibration_path);
    require(file.open(QIODevice::WriteOnly) && file.write("{}") == 2, "write external calibration change");
  }
  spin(1600);
  require(calls().contains("--apply-input"), "An external calibration change re-applies touch input");
  {
    QFile file(calibration_request);
    require(file.open(QIODevice::WriteOnly) && file.write("{\"requested\":true}") > 0, "write calibration request");
  }
  spin(1600);
  require(model.calibrationStage() == "capture" && !QFile::exists(calibration_request),
          "A setup-page request starts panel calibration once");

  const auto write_result = [&](const QByteArray &contents) {
    QFile file(apply_result);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size(),
            "write apply result fixture");
  };
  write_result("{\"revision\":9,\"hostname_applied\":true,\"rotation\":\"awaiting_confirmation\"}");
  spin(700);
  require(model.displayConfirmPending() && model.confirmDisplay() && !model.confirmDisplay(),
          "The panel can keep a previewed orientation exactly once");
  {
    QFile confirmation(display_confirm);
    require(confirmation.open(QIODevice::ReadOnly) &&
                QJsonDocument::fromJson(confirmation.readAll()).object().value("revision").toInteger() == 9,
            "Panel confirmation names the previewed settings revision");
  }
  write_result("{\"revision\":9,\"hostname_applied\":true,\"rotation\":\"confirmed\"}");
  spin(700);
  require(!model.displayConfirmPending() && !model.displayConfirmSent(),
          "The orientation prompt clears once the applicator keeps it");
  std::cout << "Qt model polling, deduplication, null, gap and calibration checks passed\n";
}
