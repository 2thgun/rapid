#include "../qt_dashboard_model.hpp"
#include <QCoreApplication>
#include <QDir>
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
#include <algorithm>
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
  // #18: the owner-set steering lock is display state, read/written by the panel.
  const auto steering_lock_path = helper_directory.filePath("steering-lock.json");
  qputenv("RAPID_STEERING_LOCK", steering_lock_path.toUtf8());
  qputenv("RAPID_TOUCH_CALIBRATION", calibration_path.toUtf8());
  const auto calibration_request = helper_directory.filePath("calibration-request.json");
  qputenv("RAPID_CALIBRATION_REQUEST", calibration_request.toUtf8());
  const auto apply_result = helper_directory.filePath("apply-result.json");
  const auto display_confirm = helper_directory.filePath("display-confirm.json");
  qputenv("RAPID_APPLY_RESULT", apply_result.toUtf8());
  qputenv("RAPID_DISPLAY_CONFIRM", display_confirm.toUtf8());
  const auto network_control = helper_directory.filePath("network-control");
  QDir().mkpath(network_control);
  qputenv("RAPID_NETWORK_CONTROL", network_control.toUtf8());
  QJsonObject state{{"companion_connected", true}, {"companion_daemon_state", "driving"},
                    {"telemetry_fresh", true}, {"session_id", "qt-test"},
                    {"samples_received", 1}, {"throttle", 0.75}, {"brake", 0.25},
                    {"g_x", 0.5}, {"g_z", -0.5}, {"telemetry_age_ms", 0},
                    {"steering_angle", 0.25}};
  bool fail = false;
  QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
    auto *socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
      auto buffer = socket->property("request").toByteArray() + socket->readAll();
      if (!buffer.contains("\r\n\r\n")) { socket->setProperty("request", buffer); return; }
      const bool live = buffer.startsWith("GET /api/live ");
      const bool network_mode = buffer.startsWith("GET /api/v1/network/mode ");
      const QJsonObject reply = live ? state
          : network_mode ? QJsonObject{{"available", true}, {"mode", "ap"}}
                         : QJsonObject{{"available", false}};
      const auto body = QJsonDocument(reply).toJson(QJsonDocument::Compact);
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
  // steering-display-motion: deterministic checks of the exact presentation
  // smoother the model runs, measuring the introduced delay from the recorded
  // ACC motion rather than asserting source shape.
  {
    constexpr double tick_ms = 1000.0 / 60.0;
    SteeringSmoother smoother;
    require(!smoother.valid() && smoother.value() == 0.0, "Smoother starts invalid");
    require(smoother.set_target(0.4) && smoother.value() == 0.4 && !smoother.moving(),
            "The first steering sample snaps to position");
    // 0.4 -> 0.9 is a 0.5 jump, beyond the 0.25 snap threshold.
    require(smoother.set_target(0.9) && smoother.value() == 0.9 && !smoother.moving(),
            "A jump beyond the snap threshold snaps immediately");
    require(smoother.set_target(0.4), "An opposite discontinuity snaps back");
    require(!smoother.set_target(0.6) && smoother.moving(),
            "A step within the threshold animates instead of snapping");
    double settle_ms = 0.0, ninety_five_ms = -1.0;
    for (int i = 0; i < 600 && smoother.moving(); ++i) {
      smoother.advance(tick_ms / 1000.0);
      settle_ms += tick_ms;
      require(smoother.value() >= 0.4 - 1e-9 && smoother.value() <= 0.6 + 1e-9,
              "Smoothing never overshoots the target");
      if (ninety_five_ms < 0 && smoother.value() >= 0.4 + 0.95 * 0.2) ninety_five_ms = settle_ms;
    }
    require(!smoother.moving() && settle_ms <= 150.0,
            "A steering step settles within the documented budget");
    require(ninety_five_ms > 0 && ninety_five_ms <= 80.0,
            "95% of a step is reached within ~54 ms");
    const double before_reversal = smoother.value();
    smoother.set_target(0.5);
    for (int i = 0; i < 600 && smoother.moving(); ++i) {
      smoother.advance(tick_ms / 1000.0);
      require(smoother.value() >= 0.5 - 1e-9 && smoother.value() <= before_reversal + 1e-9,
              "A rapid reversal does not overshoot");
    }
    // A sustained 30 Hz ramp must not accumulate lag.
    SteeringSmoother ramp;
    ramp.set_target(0.0);
    double target = 0.0, early_lag = 0.0, late_lag = 0.0;
    for (int tick = 0; tick < 600; ++tick) {
      if (tick % 2 == 0) { target += 0.004; ramp.set_target(target); }
      ramp.advance(tick_ms / 1000.0);
      const double lag = target - ramp.value();
      if (tick >= 60 && tick < 120) early_lag = std::max(early_lag, lag);
      if (tick >= 540) late_lag = std::max(late_lag, lag);
    }
    require(early_lag < 0.02 && late_lag < 0.02,
            "Steady-state steering lag stays under 0.02 norm (~9 deg)");
    require(late_lag <= early_lag + 0.005, "Steering lag does not grow under sustained updates");
    ramp.set_target(ramp.value() + 0.2);
    require(ramp.moving() && ramp.settle() && !ramp.moving(),
            "Stale telemetry settles the wheel instead of freezing mid-sweep");
  }
  spin(500);
  // steering-display-motion: the smoothed copy tracks the raw channel, and the
  // raw channel itself is preserved for value()/the recorder.
  require(model.value("steering_angle").toDouble() == 0.25 &&
              std::abs(model.steeringDisplay() - 0.25) < 1e-6,
          "Raw steering is preserved and the display settles to it");
  state["steering_angle"] = -0.5;
  spin(400);
  require(model.value("steering_angle").toDouble() == -0.5 &&
              std::abs(model.steeringDisplay() - (-0.5)) < 1e-6,
          "A discontinuous steering change snaps through the model");
  state["steering_angle"] = 0.25;
  spin(400);
  // #18: the lock-to-lock is user-settable and only a display fallback. The
  // wire value (iRacing's own lock) always wins; the owner value is used when
  // the sim exposes none; the "*" marker only when neither exists.
  {
    const auto read_lock_file = [&] {
      QFile file(steering_lock_path);
      return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    };
    require(model.userSteeringLockDeg() == 0 && !model.steeringLockKnown() &&
                model.effectiveSteeringLockDeg() == 900,
            "#18: with no sim lock and no owner value the panel uses the 900 default (unknown)");
    require(model.setUserSteeringLockDeg(540) && model.userSteeringLockDeg() == 540 &&
                model.steeringLockKnown() && model.effectiveSteeringLockDeg() == 540,
            "#18: an owner-set lock is used and marks the lock known");
    require(read_lock_file().contains("\"lock_to_lock_deg\":540"),
            "#18: the owner-set lock is persisted for the next boot");
    // iRacing supplies its own lock on the wire and must win over the owner value.
    state["steering_lock_deg"] = 480;
    spin(400);
    require(model.effectiveSteeringLockDeg() == 480 && model.userSteeringLockDeg() == 540,
            "#18: the sim's own lock wins over the owner value");
    require(model.setUserSteeringLockDeg(0) && model.effectiveSteeringLockDeg() == 480,
            "#18: AUTO clears the owner value while the sim value still applies");
    state.remove("steering_lock_deg");
    spin(400);
    require(!model.steeringLockKnown() && model.effectiveSteeringLockDeg() == 900,
            "#18: clearing the owner value with no sim lock returns to the unknown default");
    require(model.setUserSteeringLockDeg(5000) && model.userSteeringLockDeg() == 1440 &&
                model.effectiveSteeringLockDeg() == 1440,
            "#18: an out-of-range owner value is clamped to the maximum");
    {
      QFile file(steering_lock_path);
      require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  file.write("{\"lock_to_lock_deg\":360}") == 24,
              "write an external steering-lock value");
    }
    spin(2200);
    require(model.userSteeringLockDeg() == 360 && model.effectiveSteeringLockDeg() == 360,
            "#18: an externally written lock (setup page) is picked up without a restart");
    require(model.setUserSteeringLockDeg(0), "reset the owner steering lock");
  }
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
              model.setupNotice().contains("FINGERPRINT (first 16)  fedc ba98 7654 3210") &&
              !model.setupNotice().contains("fedcba9876543210") &&
              model.setupNotice().contains("0123456789abcdef 0123456789abcdef") &&
              !model.setupNotice().contains("PASSWORD"),
          "#54: the panel prints the same first-16-hex fingerprint form as the browser "
          "and still shows the full activation token, with no passphrase");
  // The activation token must be visible even before the privileged
  // provisioner has published the resolved SSID: hiding the whole card until
  // /run/rapid/network-ssid exists left the owner with no on-screen token.
  network_ssid.resize(0);
  network_ssid.flush();
  spin(1200);
  require(model.setupNotice().contains("SETUP AP  rapid  (open network)") &&
              model.setupNotice().contains("0123456789abcdef 0123456789abcdef"),
          "Setup card still shows the activation token before the provisioner publishes the SSID");
  network_ssid.seek(0);
  network_ssid.write("rapid\n");
  network_ssid.flush();
  spin(1200);
  require(model.setupNotice().contains("SETUP AP  rapid  (open network)"),
          "Setup card returns to the published SSID once it exists");
  // #59: an enrolled device publishes the address/fingerprint but no token.
  // The card still appears while Access Point mode is up, with no TOKEN line.
  {
    const QByteArray enrolled =
        "{\"bootstrap\":{\"setup_address\":\"192.168.1.64\",\"setup_port\":8002,"
        "\"setup_url\":\"http://192.168.1.64:8002/setup\","
        "\"certificate_fingerprint\":\"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\"}}";
    require(setup_status.resize(0) && setup_status.seek(0) &&
                setup_status.write(enrolled) == enrolled.size(),
            "rewrite the first-boot status without an activation token");
    setup_status.flush();
    spin(2200);
    require(model.setupNotice().contains("SETUP AP  rapid  (open network)") &&
                model.setupNotice().contains("fedcba9876543210 fedcba9876543210") &&
                !model.setupNotice().contains("TOKEN"),
            "#59: an enrolled device in AP mode shows the SSID/address/fingerprint without a token");
  }
  // setup-page-ux: the settings page shows the setup page URL and can ask the
  // root Wi-Fi mode worker to start/restart the setup service.
  require(model.setupUrl() == "http://192.168.1.64:8002/setup",
          "the panel settings page shows the published setup page URL");
  model.restartSetupService();
  {
    QFile request(QDir(network_control).filePath("request"));
    require(request.open(QIODevice::ReadOnly), "the setup-service restart request is written");
    require(request.readAll().contains("\"action\":\"restart-setup\""),
            "the panel asks the root worker to restart the setup service");
  }
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
