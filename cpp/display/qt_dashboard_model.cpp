#include "qt_dashboard_model.hpp"

#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QUrlQuery>
#include <QAbstractSocket>
#include <QWebSocket>
#include <QtMath>
#include <cmath>
#include <utility>

namespace {
QString format_time(const QVariant &value) {
  if (!value.isValid() || value.isNull()) return "—";
  const auto milliseconds = value.toLongLong();
  return QStringLiteral("%1:%2.%3")
      .arg(milliseconds / 60000)
      .arg((milliseconds / 1000) % 60, 2, 10, QLatin1Char('0'))
      .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}

QUrl endpoint_path(QUrl endpoint, const QString &path, int port = -1) {
  endpoint.setPath(path);
  endpoint.setQuery(QUrlQuery());
  if (port > 0) endpoint.setPort(port);
  return endpoint;
}

// The live push socket (#17) reuses the same host/port as the HTTP endpoint,
// asking the native server for the coalesced display state rather than the
// engineering telemetry view's raw sample-event history.
QUrl live_socket_endpoint(QUrl endpoint) {
  endpoint.setScheme(endpoint.scheme() == "https" ? "wss" : "ws");
  endpoint.setPath("/api/v1/live");
  endpoint.setQuery(QUrlQuery{{"mode", "state"}});
  return endpoint;
}

// #54/#61: the printed short form of the certificate fingerprint is the first
// 16 hex (64 bits, the evil-twin floor) grouped in fours. The panel, the setup
// page and the companion all use it; the full 64-hex value is still what the
// wire carries and is still what the stored pairing identity pins.
QString short_fingerprint(const QString &value) {
  const QString hex = value.left(16).toLower();
  QStringList groups;
  for (qsizetype i = 0; i < hex.size(); i += 4) groups << hex.mid(i, 4);
  return groups.join(QLatin1Char(' '));
}

// #61: the address a first-run companion is given. Pairing is served by the
// runtime's own TLS listener ([pairing] in packaging/config.toml, port 8003),
// which is up whenever the Pi is - in AP mode and on Home Wi-Fi alike - and
// the panel's window/approval handoffs address that same coordinator. The
// setup page's address (192.168.1.64:8002) exists only while the setup AP is
// up, so the pairing entry publishes the device's current address instead.
// RAPID_PAIRING_ADDRESS overrides the whole host:port for tests and unusual
// deployments.
constexpr int kPairingPort = 8003;

QString pairing_address() {
  const QString override = qEnvironmentVariable("RAPID_PAIRING_ADDRESS");
  if (!override.isEmpty()) return override;
  const auto first_ipv4 = [](const QNetworkInterface &interface) -> QString {
    if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
        !interface.flags().testFlag(QNetworkInterface::IsRunning) ||
        interface.flags().testFlag(QNetworkInterface::IsLoopBack))
      return {};
    for (const auto &entry : interface.addressEntries()) {
      const auto address = entry.ip();
      if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() &&
          !address.isLinkLocal())
        return address.toString();
    }
    return {};
  };
  const auto interfaces = QNetworkInterface::allInterfaces();
  for (const auto &interface : interfaces) {
    if (interface.name() == QLatin1String("wlan0")) {
      const auto address = first_ipv4(interface);
      if (!address.isEmpty()) return address + ':' + QString::number(kPairingPort);
    }
  }
  for (const auto &interface : interfaces) {
    const auto address = first_ipv4(interface);
    if (!address.isEmpty()) return address + ':' + QString::number(kPairingPort);
  }
  return {};
}

// #22: the setup AP is the open network "rapid" (or a disambiguated
// "rapid-NNNN" when another one is already in range, chosen once by the
// provisioner for this boot). It carries no passphrase, so the card/panel no
// longer shows one. The activation token is what lets the owner enroll without
// reading a file over SSH, so it must never be hidden behind another file:
// when the provisioner has not yet published the resolved SSID the card still
// shows the token (and the address/fingerprint), with the name falling back to
// "rapid". Two Pis can broadcast the same SSID, so the panel also shows the
// address and the TLS certificate fingerprint the client's browser should
// match.
//
// #59 / setup-page-ux: first boot now publishes the address and fingerprint
// even on an enrolled device, so the panel can show the setup card whenever
// Access Point mode is up (the owner can still choose AP mode after setup).
// On an enrolled device there is no activation token, so the card shows no
// TOKEN line, and it stays hidden outside AP mode so it cannot cover the
// dashboard during normal driving.
QString setup_notice(const QByteArray &status_contents, const QByteArray &ssid_contents,
                     bool ap_mode) {
  const auto document = QJsonDocument::fromJson(status_contents);
  if (!document.isObject()) return {};
  const auto bootstrap = document.object().value("bootstrap");
  if (!bootstrap.isObject()) return {};
  const auto values = bootstrap.toObject();
  const auto url = values.value("setup_url").toString();
  const auto fingerprint = values.value("certificate_fingerprint").toString();
  const auto token = values.value("activation_token").toString();
  if (url.isEmpty() || fingerprint.size() != 64) return {};
  if (!token.isEmpty() && token.size() != 64) return {};
  // An enrolled device (no token) only shows the card in Access Point mode.
  if (token.isEmpty() && !ap_mode) return {};
  const auto resolved = QString::fromUtf8(ssid_contents).trimmed();
  const auto ssid = resolved.isEmpty() ? QStringLiteral("rapid") : resolved;
  // #54: the browser's setup page shows the first 16 hex (64 bits, the
  // evil-twin floor) grouped in fours, so the panel shows the same short form
  // and labels it. The full 64-hex value is still what the wire carries and is
  // still validated above; only the printed form is shortened.
  auto grouped = [](const QString &value, const QString &indent) {
    return QStringLiteral("%1 %2\n%3%4 %5")
        .arg(value.sliced(0, 16), value.sliced(16, 16), indent, value.sliced(32, 16), value.sliced(48, 16));
  };
  QString notice = QStringLiteral("SETUP AP  %1  (open network)\n%2\nFINGERPRINT (first 16)  %3")
      .arg(ssid, url, short_fingerprint(fingerprint));
  if (token.size() == 64)
    notice += QStringLiteral("\nTOKEN  %1").arg(grouped(token, QStringLiteral("       ")));
  return notice;
}

// Inset targets avoid the bezel, where resistive panels are least linear.
const QPointF calibration_points[] = {{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.1, 0.9}, {0.5, 0.5}};
constexpr qsizetype calibration_point_count = 5;
const QPointF verification_point{0.3, 0.7};
constexpr double verification_tolerance = 0.06;

QString calibration_file() {
  return qEnvironmentVariable("RAPID_TOUCH_CALIBRATION", "/var/lib/rapid-setup/touch-calibration.conf");
}

// The one rotation state file rapid-display-recovery writes; the panel reads
// the same file and path (do not invent a second one).
QString display_state_file() {
  return qEnvironmentVariable("RAPID_DISPLAY_STATE", "/var/lib/rapid/display-recovery.json");
}

QStringList display_recovery_files() {
  return {"--state-file", display_state_file(), "--calibration-file", calibration_file()};
}

// #18: the owner-set steering lock-to-lock. Display-only state (never sent on
// the wire, which keeps the sim's own reading truthful), so it lives with the
// other panel state. A setup-page writer can use the same path/schema.
QString steering_lock_file() {
  return qEnvironmentVariable("RAPID_STEERING_LOCK", "/var/lib/rapid/steering-lock.json");
}

QString file_signature(const QString &path) {
  const QFileInfo info(path);
  return info.exists() ? QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' +
                             QString::number(info.size())
                       : QString{};
}
}  // namespace

DashboardModel::DashboardModel(QUrl endpoint, QObject *parent)
    : QObject(parent), endpoint_(std::move(endpoint)), network_(new QNetworkAccessManager(this)),
      calibration_timeout_(new QTimer(this)) {
  live_socket_url_ = live_socket_endpoint(endpoint_);
  network_->setTransferTimeout(1800);
  calibration_timeout_->setSingleShot(true);
  connect(calibration_timeout_, &QTimer::timeout, this, &DashboardModel::calibrationTimedOut);
  // ~60 Hz presentation tick for the smoothed steering wheel. It only runs
  // while the wheel is moving (updateSteeringDisplay/advanceSteeringDisplay),
  // so an idle or straight-line car costs nothing.
  steering_timer_ = new QTimer(this);
  steering_timer_->setInterval(16);
  connect(steering_timer_, &QTimer::timeout, this, &DashboardModel::advanceSteeringDisplay);
  QTimer::singleShot(0, this, &DashboardModel::pollCalibrationFile);
  QTimer::singleShot(0, this, &DashboardModel::pollDisplayConfirmation);
  QTimer::singleShot(0, this, &DashboardModel::pollDisplayRotation);
  QTimer::singleShot(0, this, &DashboardModel::pollSteeringLock);
  // The HTTP poll loop keeps running underneath the socket (it is a no-op
  // fetch while the push is active, see pollLive()) so that losing the
  // socket at any moment falls straight back to it without a gap.
  QTimer::singleShot(0, this, &DashboardModel::pollLive);
  QTimer::singleShot(0, this, &DashboardModel::connectLiveSocket);
  QTimer::singleShot(0, this, &DashboardModel::pollNetworkMode);
  QTimer::singleShot(0, this, &DashboardModel::pollLogStatus);
  QTimer::singleShot(0, this, &DashboardModel::pollSetupStatus);
  QTimer::singleShot(0, this, &DashboardModel::pollPairingPanel);
}

void DashboardModel::connectLiveSocket() {
  if (!live_socket_) {
    live_socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(live_socket_, &QWebSocket::connected, this, &DashboardModel::liveSocketConnected);
    connect(live_socket_, &QWebSocket::disconnected, this, &DashboardModel::liveSocketDisconnected);
    connect(live_socket_, &QWebSocket::textMessageReceived, this, &DashboardModel::consumeLiveMessage);
    // QWebSocket::error is deprecated in favour of errorOccurred since Qt
    // 6.5; the Pi build (Debian 13, Qt 6.8) compiles with -Werror and would
    // fail on -Wdeprecated-declarations, while WSL (Qt 6.4.2) predates
    // errorOccurred's deprecation of the old signal. Pick whichever compiles
    // warning-free on both.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(live_socket_, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { liveSocketDisconnected(); });
#else
    connect(live_socket_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) { liveSocketDisconnected(); });
#endif
  }
  if (live_socket_->state() == QAbstractSocket::UnconnectedState)
    live_socket_->open(live_socket_url_);
}

void DashboardModel::liveSocketConnected() {
  live_push_active_ = true;
  bump();
}

void DashboardModel::liveSocketDisconnected() {
  const bool was_active = live_push_active_;
  live_push_active_ = false;
  if (was_active) bump();
  // Retry with a fixed backoff; a display should not need a restart to
  // recover a socket the server dropped or that never came up.
  QTimer::singleShot(2000, this, &DashboardModel::connectLiveSocket);
}

void DashboardModel::consumeLiveMessage(const QString &message) {
  const auto document = QJsonDocument::fromJson(message.toUtf8());
  if (!document.isObject()) return;
  live_push_active_ = true;
  applyLiveState(document.object().toVariantMap());
}

QVariant DashboardModel::value(const QString &key) const { return state_.value(key); }

QString DashboardModel::timeValue(const QString &key) const { return format_time(value(key)); }

QString DashboardModel::percentValue(const QString &key) const {
  const auto raw = value(key);
  if (!raw.isValid() || raw.isNull()) return "—";
  return QString::number(qRound(raw.toDouble() * 100)) + '%';
}

void DashboardModel::bump() {
  ++revision_;
  emit changed();
}

void DashboardModel::pollLive() {
  // The socket is the primary source once connected; this loop keeps ticking
  // regardless so a lost or never-established socket has a fallback already
  // running instead of one that must be started from scratch.
  if (!live_push_active_ && !live_request_pending_) {
    live_request_pending_ = true;
    auto *reply = network_->get(QNetworkRequest(endpoint_path(endpoint_, "/api/live")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { consumeLive(reply); });
  }
  QTimer::singleShot(200, this, &DashboardModel::pollLive);
}

void DashboardModel::pollSetupStatus() {
  QFile file(qEnvironmentVariable("RAPID_FIRSTBOOT_STATUS",
                                  "/run/rapid/firstboot.json"));
  QFile ssid_file(qEnvironmentVariable("RAPID_NETWORK_SSID",
                                       "/run/rapid/network-ssid"));
  const auto status_contents = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
  const auto ssid_contents = ssid_file.open(QIODevice::ReadOnly) ? ssid_file.readAll() : QByteArray{};
  // #59: the setup page URL is shown on the panel's settings page. It is the
  // same first-boot bootstrap the setup card reads; empty until first boot has
  // published it.
  QString next_url;
  QString next_fingerprint;
  {
    const auto document = QJsonDocument::fromJson(status_contents);
    if (document.isObject()) {
      const auto bootstrap = document.object().value("bootstrap");
      if (bootstrap.isObject()) {
        const auto values = bootstrap.toObject();
        next_url = values.value("setup_url").toString();
        // #61: the same validation the setup card applies; only a full 64-hex
        // digest becomes the short printed form the companion accepts.
        const auto fingerprint = values.value("certificate_fingerprint").toString();
        if (!next_url.isEmpty() && fingerprint.size() == 64)
          next_fingerprint = short_fingerprint(fingerprint);
      }
    }
  }
  const auto next_address = pairing_address();
  bool changed = false;
  if (setup_url_ != next_url) { setup_url_ = next_url; changed = true; }
  if (pairing_address_ != next_address) { pairing_address_ = next_address; changed = true; }
  if (pairing_fingerprint_ != next_fingerprint) { pairing_fingerprint_ = next_fingerprint; changed = true; }
  const QString next = setup_notice(status_contents, ssid_contents, network_mode_ == "ap");
  if (setup_notice_ != next) {
    setup_notice_ = next;
    changed = true;
  }
  if (changed) bump();
  QTimer::singleShot(1000, this, &DashboardModel::pollSetupStatus);
}

void DashboardModel::pollPairingPanel() {
  QFile file(qEnvironmentVariable("RAPID_PAIRING_PANEL", "/run/rapid/pairing.json"));
  bool pending = false;
  QString label, code;
  QString transaction;
  if (file.open(QIODevice::ReadOnly)) {
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject()) {
      const auto object = document.object();
      label = object.value("label").toString();
      code = object.value("code").toString();
      transaction = object.value("transaction_id").toString();
      const bool numeric = code.size() == 8 && std::all_of(code.cbegin(), code.cend(),
          [](const QChar character) { return character >= QLatin1Char('0') &&
              character <= QLatin1Char('9'); });
      const bool transaction_hex = transaction.size() == 32 &&
          std::all_of(transaction.cbegin(), transaction.cend(), [](const QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
                (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
          });
      pending = !label.isEmpty() && transaction_hex &&
          code.size() == 8 && numeric;
    }
  }
  // #61: the pairing service publishes the window state and owns the window;
  // the panel's open/cancel handoff is a private control file that the service
  // consumes on the next pairing interaction, so its presence means "armed,
  // waiting for the companion".
  bool window_active = false;
  QFile state_file(qEnvironmentVariable("RAPID_PAIRING_STATE", "/run/rapid/pairing-state.json"));
  if (state_file.open(QIODevice::ReadOnly)) {
    const auto document = QJsonDocument::fromJson(state_file.readAll());
    if (document.isObject()) window_active = document.object().value("active").toBool();
  }
  const bool window_requested = [&] {
    QFile control(qEnvironmentVariable("RAPID_PAIRING_CONTROL", "/run/rapid/pairing-control.json"));
    if (!control.open(QIODevice::ReadOnly)) return false;
    const auto document = QJsonDocument::fromJson(control.readAll());
    return document.isObject() && document.object().value("action").toString() == "open";
  }();
  if (pairing_pending_ != pending || pairing_label_ != label || pairing_code_ != code ||
      pairing_transaction_ != transaction || pairing_window_active_ != window_active ||
      pairing_window_requested_ != window_requested) {
    if (pairing_transaction_ != transaction) pairing_approval_sent_ = false;
    pairing_pending_ = pending;
    pairing_label_ = std::move(label);
    pairing_code_ = std::move(code);
    pairing_transaction_ = std::move(transaction);
    pairing_window_active_ = window_active;
    pairing_window_requested_ = window_requested;
    if (!pairing_pending_) pairing_approval_sent_ = false;
    bump();
  }
  QTimer::singleShot(500, this, &DashboardModel::pollPairingPanel);
}

bool DashboardModel::approvePairing() {
  if (!pairing_pending_ || pairing_approval_sent_) return false;
  const QString path = qEnvironmentVariable("RAPID_PAIRING_APPROVAL",
                                            "/run/rapid/pairing-approval.json");
  const QFileInfo info(path);
  QDir().mkpath(info.absolutePath());
  const auto temporary = path + ".tmp";
  QFile file(temporary);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const QJsonObject object{{"transaction_id", pairing_transaction_}, {"code", pairing_code_}};
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.flush()) {
    file.close(); QFile::remove(temporary); return false;
  }
  file.close();
  if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                           QFileDevice::ReadGroup)) {
    QFile::remove(temporary);
    return false;
  }
  QFile::remove(path);
  if (!QFile::rename(temporary, path)) return false;
  pairing_approval_sent_ = true;
  bump();
  return true;
}

void DashboardModel::pollDisplayConfirmation() {
  QFile file(qEnvironmentVariable("RAPID_APPLY_RESULT", "/run/rapid-apply/result.json"));
  bool pending = false;
  qint64 revision = -1;
  if (file.open(QIODevice::ReadOnly)) {
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto value = object.value("revision");
    if (object.value("rotation").toString() == "awaiting_confirmation" && value.isDouble()) {
      pending = true;
      revision = value.toInteger();
    }
  }
  if (pending != display_confirm_pending_ || revision != display_confirm_revision_) {
    if (revision != display_confirm_revision_ || !pending) display_confirm_sent_ = false;
    display_confirm_pending_ = pending;
    display_confirm_revision_ = revision;
    bump();
  }
  QTimer::singleShot(500, this, &DashboardModel::pollDisplayConfirmation);
}

void DashboardModel::pollDisplayRotation() {
  int rotation = 0;
  QFile file(display_state_file());
  if (file.open(QIODevice::ReadOnly)) {
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto value = object.value("rotation");
    if (value.isDouble()) {
      const int candidate = value.toInt();
      if (candidate == 0 || candidate == 180) rotation = candidate;
    }
  }
  if (rotation != display_rotation_) {
    display_rotation_ = rotation;
    bump();
  }
  QTimer::singleShot(500, this, &DashboardModel::pollDisplayRotation);
}

int DashboardModel::simSteeringLockDeg() const {
  bool ok = false;
  const double value = state_.value("steering_lock_deg").toDouble(&ok);
  return ok && std::isfinite(value) && value > 0 ? qRound(value) : 0;
}

int DashboardModel::effectiveSteeringLockDeg() const {
  const int sim = simSteeringLockDeg();
  if (sim > 0) return sim;
  return user_steering_lock_deg_ > 0 ? user_steering_lock_deg_ : kDefaultSteeringLockDeg;
}

bool DashboardModel::setUserSteeringLockDeg(int degrees) {
  int next = degrees;
  if (next != 0) next = std::clamp(next, kMinSteeringLockDeg, kMaxSteeringLockDeg);
  const QString path = steering_lock_file();
  const QFileInfo info(path);
  QDir().mkpath(info.absolutePath());
  const auto temporary = path + ".tmp";
  QFile file(temporary);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const QJsonObject object{{"lock_to_lock_deg", next}};
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.flush()) {
    file.close();
    QFile::remove(temporary);
    return false;
  }
  file.close();
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                      QFileDevice::ReadGroup);
  QFile::remove(path);
  if (!QFile::rename(temporary, path)) return false;
  if (next != user_steering_lock_deg_) {
    user_steering_lock_deg_ = next;
    bump();
  }
  return true;
}

void DashboardModel::pollSteeringLock() {
  // Owner-set display fallback (#18). Reading it here (rather than only at
  // startup) means a setup-page writer using the same file takes effect without
  // a panel restart, and an external clear returns the wheel to the sim/default.
  int value = 0;
  QFile file(steering_lock_file());
  if (file.open(QIODevice::ReadOnly)) {
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto raw = object.value("lock_to_lock_deg");
    if (raw.isDouble()) {
      const int candidate = raw.toInt();
      if (candidate == 0) value = 0;
      else if (candidate >= kMinSteeringLockDeg && candidate <= kMaxSteeringLockDeg)
        value = candidate;
    }
  }
  if (value != user_steering_lock_deg_) {
    user_steering_lock_deg_ = value;
    bump();
  }
  QTimer::singleShot(2000, this, &DashboardModel::pollSteeringLock);
}

QPointF DashboardModel::screenPoint(double x, double y) const {
  // Qt hit-tests a rotated scene back into the item's local frame, so a touch
  // arrives already unwound. The calibration helper speaks screen coordinates
  // and the X matrix carries no rotation (#13), so a 180-degree scene rotates
  // the reported point the same way the picture is rotated.
  return display_rotation_ == 180 ? QPointF(1.0 - x, 1.0 - y) : QPointF(x, y);
}

bool DashboardModel::confirmDisplay() {
  if (!display_confirm_pending_ || display_confirm_sent_) return false;
  // The root applicator accepts only a confirmation naming the previewed revision.
  const QString path = qEnvironmentVariable("RAPID_DISPLAY_CONFIRM", "/run/rapid-apply/display-confirm.json");
  const auto temporary = path + ".tmp";
  QFile file(temporary);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const QJsonObject object{{"revision", display_confirm_revision_}};
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.flush()) {
    file.close();
    QFile::remove(temporary);
    return false;
  }
  file.close();
  QFile::remove(path);
  if (!QFile::rename(temporary, path)) return false;
  display_confirm_sent_ = true;
  bump();
  return true;
}

QPointF DashboardModel::calibrationTarget() const {
  if (calibration_stage_ == "capture" && calibration_taps_.size() < calibration_point_count)
    return calibration_points[calibration_taps_.size()];
  if (calibration_stage_ == "verify") return verification_point;
  return {-1, -1};
}

void DashboardModel::startCalibration() {
  if (calibration_stage_ == "capture" || calibration_stage_ == "applying" ||
      calibration_stage_ == "verify")
    return;
  calibration_taps_.clear();
  calibration_stage_ = "capture";
  calibration_message_ = "Tap the centre of each target (1 of 5)";
  calibration_timeout_->start(60000);
  bump();
}

void DashboardModel::calibrationTap(double x, double y) {
  // The MouseArea lives inside the rotated scene, so this is a local
  // coordinate; the helper wants the unrotated screen coordinate.
  const QPointF tap = screenPoint(x, y);
  if (calibration_stage_ == "done" || calibration_stage_ == "failed") {
    calibration_stage_.clear();
    calibration_message_.clear();
    bump();
  } else if (calibration_stage_ == "capture") {
    calibration_taps_.append(tap);
    if (calibration_taps_.size() < calibration_point_count) {
      calibration_message_ = QStringLiteral("Tap the centre of each target (%1 of %2)")
                                 .arg(calibration_taps_.size() + 1).arg(calibration_point_count);
      calibration_timeout_->start(60000);
      bump();
      return;
    }
    auto arguments = display_recovery_files();
    arguments << "--calibrate";
    for (qsizetype i = 0; i < calibration_point_count; ++i) {
      const QPointF target = screenPoint(calibration_points[i].x(), calibration_points[i].y());
      arguments << "--sample" << QStringLiteral("%1,%2,%3,%4")
                                     .arg(calibration_taps_[i].x(), 0, 'f', 6)
                                     .arg(calibration_taps_[i].y(), 0, 'f', 6)
                                     .arg(target.x(), 0, 'f', 6)
                                     .arg(target.y(), 0, 'f', 6);
    }
    calibration_timeout_->stop();
    calibration_stage_ = "applying";
    calibration_message_ = "Applying calibration…";
    bump();
    runDisplayRecovery(arguments, [this](bool ok, const QString &error) {
      if (!ok) {
        finishCalibration("failed", error.isEmpty() ? QStringLiteral("Calibration rejected; try again") : error);
        return;
      }
      calibration_stage_ = "verify";
      calibration_message_ = "Tap the target to keep the new calibration";
      calibration_timeout_->start(30000);
      bump();
    });
  } else if (calibration_stage_ == "verify") {
    calibration_timeout_->stop();
    const QPointF expected = screenPoint(verification_point.x(), verification_point.y());
    const bool accurate = std::hypot(tap.x() - expected.x(), tap.y() - expected.y()) <=
                          verification_tolerance;
    calibration_stage_ = "applying";
    bump();
    runDisplayRecovery(display_recovery_files() << (accurate ? "--confirm-calibration" : "--rollback-calibration"),
                       [this, accurate](bool ok, const QString &error) {
                         if (accurate && ok) finishCalibration("done", "Touch calibration saved");
                         else if (accurate) finishCalibration("failed", error.isEmpty() ? QStringLiteral("Calibration could not be saved") : error);
                         else finishCalibration("failed", "Tap missed the target; previous calibration restored");
                       });
  }
}

void DashboardModel::calibrationTimedOut() {
  if (calibration_stage_ == "capture") {
    finishCalibration("failed", "Calibration timed out");
  } else if (calibration_stage_ == "verify") {
    // An unconfirmed calibration may be unusable, so it never outlives its preview.
    calibration_stage_ = "applying";
    bump();
    runDisplayRecovery(display_recovery_files() << "--rollback-calibration", [this](bool, const QString &) {
      finishCalibration("failed", "Calibration not confirmed; previous calibration restored");
    });
  }
}

void DashboardModel::finishCalibration(const QString &stage, const QString &message) {
  calibration_taps_.clear();
  calibration_stage_ = stage;
  calibration_message_ = message;
  // The panel changed the file itself; do not re-apply it as an external change.
  calibration_signature_ = file_signature(calibration_file());
  const int generation = ++calibration_generation_;
  bump();
  QTimer::singleShot(4000, this, [this, generation] {
    if (generation != calibration_generation_ ||
        (calibration_stage_ != "done" && calibration_stage_ != "failed"))
      return;
    calibration_stage_.clear();
    calibration_message_.clear();
    bump();
  });
}

void DashboardModel::runDisplayRecovery(const QStringList &arguments,
                                        std::function<void(bool, const QString &)> done) {
  auto *process = new QProcess(this);
  connect(process, &QProcess::finished, this,
          [process, done](int code, QProcess::ExitStatus status) {
            const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(120);
            process->deleteLater();
            done(status == QProcess::NormalExit && code == 0, error);
          });
  connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    process->deleteLater();
    done(false, QStringLiteral("Display recovery helper is unavailable"));
  });
  process->start(qEnvironmentVariable("RAPID_DISPLAY_RECOVERY", "/usr/lib/rapid/rapid-display-recovery"),
                 arguments);
}

void DashboardModel::pollCalibrationFile() {
  // The authenticated setup page starts calibration through a private runtime
  // file. Removing it before starting makes each request start at most once.
  const auto request = qEnvironmentVariable("RAPID_CALIBRATION_REQUEST", "/run/rapid/calibration-request.json");
  if (QFile::exists(request) && QFile::remove(request)) startCalibration();
  const auto signature = file_signature(calibration_file());
  if (!calibration_signature_known_) {
    calibration_signature_ = signature;
    calibration_signature_known_ = true;
  } else if (signature != calibration_signature_ && calibration_stage_.isEmpty()) {
    // For example a browser calibration reset: apply it without restarting the panel.
    calibration_signature_ = signature;
    runDisplayRecovery(display_recovery_files() << "--apply-input", [](bool, const QString &) {});
  }
  QTimer::singleShot(1000, this, &DashboardModel::pollCalibrationFile);
}

void DashboardModel::consumeLive(QNetworkReply *reply) {
  live_request_pending_ = false;
  // A push already delivered a newer state while this fallback request was
  // in flight; do not let a slower, now-stale HTTP reply overwrite it.
  if (live_push_active_) {
    reply->deleteLater();
    return;
  }
  if (reply->error() == QNetworkReply::NoError) {
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (document.isObject()) {
      applyLiveState(document.object().toVariantMap());
    } else {
      applyLiveFailure("Dashboard sent invalid telemetry");
    }
  } else {
    applyLiveFailure("Dashboard connection lost");
  }
  reply->deleteLater();
}

void DashboardModel::applyLiveState(QVariantMap state) {
  state_ = std::move(state);
  updateStatus();
  updateGraphHistory();
  updateSteeringDisplay();
  bump();
}

void DashboardModel::applyLiveFailure(const QString &message) {
  state_["telemetry_fresh"] = false;
  status_ = message;
  updateGraphHistory();
  bump();
}

void DashboardModel::updateGraphHistory() {
  constexpr qint64 kWindowMilliseconds = 30'000;
  constexpr qint64 kGapIntervalMilliseconds = 200;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool driving = state_.value("companion_connected").toBool() &&
      state_.value("companion_daemon_state").toString() == "driving" &&
      state_.value("telemetry_fresh").toBool();
  const QString sample_id = state_.value("session_id").toString() + ':' +
      QString::number(state_.value("samples_received").toLongLong());
  const bool fresh_sample = driving && state_.value("samples_received").toLongLong() > 0 &&
      sample_id != last_graph_sample_;

  if (fresh_sample || (!driving && now - last_graph_gap_ms_ >= kGapIntervalMilliseconds)) {
    QVariantMap sample;
    sample.insert("time", now - (fresh_sample ? state_.value("telemetry_age_ms").toLongLong() : 0));
    if (fresh_sample) {
      const auto channel = [this](const char *key, bool pedal = false) -> QVariant {
        const auto raw = state_.value(key);
        bool valid = false;
        const double value = raw.toDouble(&valid);
        if (!raw.isValid() || raw.isNull() || !valid || !qIsFinite(value)) return {};
        return pedal ? qBound(0.0, value * 100.0, 100.0) : value;
      };
      sample.insert("throttle", channel("throttle", true));
      sample.insert("brake", channel("brake", true));
      sample.insert("lateral", channel("g_x"));
      sample.insert("longitudinal", channel("g_z"));
      last_graph_sample_ = sample_id;
    } else {
      // Preserve an explicit gap instead of redrawing retained idle telemetry.
      sample.insert("throttle", QVariant());
      sample.insert("brake", QVariant());
      sample.insert("lateral", QVariant());
      sample.insert("longitudinal", QVariant());
      last_graph_gap_ms_ = now;
    }
    graph_samples_.append(sample);
  }

  while (!graph_samples_.isEmpty() &&
         graph_samples_.front().toMap().value("time").toLongLong() < now - kWindowMilliseconds) {
    graph_samples_.removeFirst();
  }
}

void DashboardModel::updateSteeringDisplay() {
  // Presentation-only: state_ keeps the raw steering_angle for value() and the
  // recorder; only the wheel reads the smoothed copy. Feed the newest target
  // and let the ~60 Hz timer interpolate, so a 30 Hz push becomes continuous
  // motion without queueing anything.
  const auto raw = state_.value("steering_angle");
  bool ok = false;
  const double target = raw.toDouble(&ok);
  const bool present = raw.isValid() && !raw.isNull() && ok && qIsFinite(target);
  if (!present) {
    // Telemetry gone (disconnect/stale): finish any in-flight move so a frozen
    // wheel never sits mid-sweep, then hold the last position.
    const bool changed = steering_smoother_.settle();
    steering_timer_->stop();
    if (changed) emit steeringDisplayChanged();
    return;
  }
  if (steering_smoother_.set_target(target)) emit steeringDisplayChanged();
  if (steering_smoother_.moving()) {
    if (!steering_timer_->isActive()) {
      steering_tick_.start();
      steering_timer_->start();
    }
  } else {
    steering_timer_->stop();
  }
}

void DashboardModel::advanceSteeringDisplay() {
  const bool changed = steering_smoother_.advance(steering_tick_.restart() / 1000.0);
  if (changed) emit steeringDisplayChanged();
  if (!steering_smoother_.moving()) steering_timer_->stop();
}

void DashboardModel::updateStatus() {
  if (!state_.value("companion_connected").toBool()) {
    status_ = "Waiting for daemon connection…";
    return;
  }
  const auto simulator = state_.value("simulator").toString();
  const auto daemon_state = state_.value("companion_daemon_state").toString();
  if (daemon_state == "driving") {
    status_ = state_.value("telemetry_fresh").toBool()
        ? (simulator.isEmpty() ? "Simulator connected" : simulator + " connected")
        : "Waiting for fresh telemetry";
    return;
  }
  if (daemon_state == "paused") {
    // #15: the companion still reports the recording open during a
    // pause/menu/alt-tab (native_runtime.cpp keeps "recording" true), so this
    // must read as a distinct paused state rather than falling into the idle
    // "waiting for driving" text below. "REC" is driven separately by the
    // "recording" state value and is unaffected by this status text.
    status_ = simulator.isEmpty() ? "Paused" : simulator + " paused";
    return;
  }
  status_ = simulator.isEmpty() ? "Daemon connected — waiting for simulator"
                                : simulator + " detected — waiting for driving";
}

void DashboardModel::pollNetworkMode() {
  auto *reply = network_->get(QNetworkRequest(endpoint_path(endpoint_, "/api/v1/network/mode")));
  connect(reply, &QNetworkReply::finished, this, [this, reply] { consumeNetworkMode(reply); });
  QTimer::singleShot(2000, this, &DashboardModel::pollNetworkMode);
}

void DashboardModel::consumeNetworkMode(QNetworkReply *reply) {
  if (reply->error() == QNetworkReply::NoError) {
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (document.isObject()) {
      const auto object = document.object();
      network_available_ = object.value("available").toBool();
      network_mode_ = object.value("mode").toString();
      bump();
    }
  }
  reply->deleteLater();
}

void DashboardModel::setNetworkMode(const QString &mode) {
  if (!network_available_ || (mode != "off" && mode != "ap" && mode != "home")) return;
  QNetworkRequest request(endpoint_path(endpoint_, "/api/v1/network/mode"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  auto *reply = network_->post(request, QJsonDocument(QJsonObject{{"mode", mode}}).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void DashboardModel::restartSetupService() {
  // The panel runs as the rapid user, which may write the mode controller's
  // request directory; the root rapid-network-mode worker owns the actual
  // restart of rapid-setup.service. This is the same file hand-off the Wi-Fi
  // mode buttons use, so the panel needs no privilege of its own.
  const auto directory = qEnvironmentVariable("RAPID_NETWORK_CONTROL", "/run/rapid-network");
  QFile file(QDir(directory).filePath("request"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(QJsonDocument(QJsonObject{{"action", "restart-setup"}}).toJson(QJsonDocument::Compact));
  file.close();
  log_notice_ = "Restarting setup…";
  bump();
  QTimer::singleShot(3000, this, [this] { log_notice_.clear(); bump(); });
}

bool DashboardModel::write_pairing_control(const QString &action) {
  // #61: the pairing window belongs to the runtime; the panel writes the same
  // private control handoff the setup server proxies ({"action":"open"} or
  // {"action":"cancel"}) and never touches a credential. The service consumes
  // it on the next pairing interaction, and the state file it publishes is
  // what pollPairingPanel() reflects.
  const QString path = qEnvironmentVariable("RAPID_PAIRING_CONTROL",
                                            "/run/rapid/pairing-control.json");
  const QFileInfo info(path);
  QDir().mkpath(info.absolutePath());
  const auto temporary = path + ".tmp";
  QFile file(temporary);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const QJsonObject object{{"action", action}};
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.flush()) {
    file.close();
    QFile::remove(temporary);
    return false;
  }
  file.close();
  if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                           QFileDevice::ReadGroup)) {
    QFile::remove(temporary);
    return false;
  }
  QFile::remove(path);
  if (!QFile::rename(temporary, path)) return false;
  pairing_window_requested_ = action == "open";
  bump();
  return true;
}

bool DashboardModel::openPairingWindow() { return write_pairing_control("open"); }

bool DashboardModel::cancelPairingWindow() { return write_pairing_control("cancel"); }

void DashboardModel::pollLogStatus() {
  auto *reply = network_->get(QNetworkRequest(endpoint_path(endpoint_, "/api/log-status", 8001)));
  connect(reply, &QNetworkReply::finished, this, [this, reply] { consumeLogStatus(reply); });
  QTimer::singleShot(1000, this, &DashboardModel::pollLogStatus);
}

void DashboardModel::consumeLogStatus(QNetworkReply *reply) {
  if (reply->error() == QNetworkReply::NoError) {
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (document.isObject()) {
      const auto sequence = document.object().value("log_sequence").toInteger();
      if (last_log_sequence_ >= 0 && sequence > last_log_sequence_) {
        log_notice_ = "Log updated";
        QTimer::singleShot(3000, this, [this] { log_notice_.clear(); bump(); });
      }
      last_log_sequence_ = sequence;
      bump();
    }
  }
  reply->deleteLater();
}
