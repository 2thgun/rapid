#include "qt_dashboard_model.hpp"

#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QUrlQuery>
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

QString setup_notice(const QByteArray &contents) {
  const auto document = QJsonDocument::fromJson(contents);
  if (!document.isObject()) return {};
  const auto bootstrap = document.object().value("bootstrap");
  if (!bootstrap.isObject()) return {};
  const auto values = bootstrap.toObject();
  const auto ssid = values.value("ssid").toString();
  const auto password = values.value("access_point_password").toString();
  const auto url = values.value("setup_url").toString();
  const auto token = values.value("activation_token").toString();
  if (ssid.isEmpty() || password.isEmpty() || url.isEmpty() || token.size() != 64)
    return {};
  return QStringLiteral("SETUP AP  %1\nPASSWORD  %2\n%3\nTOKEN  %4 %5\n       %6 %7")
      .arg(ssid, password, url, token.sliced(0, 16), token.sliced(16, 16),
           token.sliced(32, 16), token.sliced(48, 16));
}

// Inset targets avoid the bezel, where resistive panels are least linear.
const QPointF calibration_points[] = {{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.1, 0.9}, {0.5, 0.5}};
constexpr qsizetype calibration_point_count = 5;
const QPointF verification_point{0.3, 0.7};
constexpr double verification_tolerance = 0.06;

QString calibration_file() {
  return qEnvironmentVariable("RAPID_TOUCH_CALIBRATION", "/var/lib/rapid-setup/touch-calibration.conf");
}

QStringList display_recovery_files() {
  return {"--state-file", qEnvironmentVariable("RAPID_DISPLAY_STATE", "/var/lib/rapid/display-recovery.json"),
          "--calibration-file", calibration_file()};
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
  network_->setTransferTimeout(1800);
  calibration_timeout_->setSingleShot(true);
  connect(calibration_timeout_, &QTimer::timeout, this, &DashboardModel::calibrationTimedOut);
  QTimer::singleShot(0, this, &DashboardModel::pollCalibrationFile);
  QTimer::singleShot(0, this, &DashboardModel::pollLive);
  QTimer::singleShot(0, this, &DashboardModel::pollNetworkMode);
  QTimer::singleShot(0, this, &DashboardModel::pollLogStatus);
  QTimer::singleShot(0, this, &DashboardModel::pollSetupStatus);
  QTimer::singleShot(0, this, &DashboardModel::pollPairingPanel);
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
  if (!live_request_pending_) {
    live_request_pending_ = true;
    auto *reply = network_->get(QNetworkRequest(endpoint_path(endpoint_, "/api/live")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { consumeLive(reply); });
  }
  QTimer::singleShot(200, this, &DashboardModel::pollLive);
}

void DashboardModel::pollSetupStatus() {
  QFile file(qEnvironmentVariable("RAPID_FIRSTBOOT_STATUS",
                                  "/run/rapid/firstboot.json"));
  const QString next = file.open(QIODevice::ReadOnly) ? setup_notice(file.readAll()) : QString{};
  if (setup_notice_ != next) {
    setup_notice_ = next;
    bump();
  }
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
  if (pairing_pending_ != pending || pairing_label_ != label || pairing_code_ != code ||
      pairing_transaction_ != transaction) {
    if (pairing_transaction_ != transaction) pairing_approval_sent_ = false;
    pairing_pending_ = pending;
    pairing_label_ = std::move(label);
    pairing_code_ = std::move(code);
    pairing_transaction_ = std::move(transaction);
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
  if (calibration_stage_ == "done" || calibration_stage_ == "failed") {
    calibration_stage_.clear();
    calibration_message_.clear();
    bump();
  } else if (calibration_stage_ == "capture") {
    calibration_taps_.append({x, y});
    if (calibration_taps_.size() < calibration_point_count) {
      calibration_message_ = QStringLiteral("Tap the centre of each target (%1 of %2)")
                                 .arg(calibration_taps_.size() + 1).arg(calibration_point_count);
      calibration_timeout_->start(60000);
      bump();
      return;
    }
    auto arguments = display_recovery_files();
    arguments << "--calibrate";
    for (qsizetype i = 0; i < calibration_point_count; ++i)
      arguments << "--sample" << QStringLiteral("%1,%2,%3,%4")
                                     .arg(calibration_taps_[i].x(), 0, 'f', 6)
                                     .arg(calibration_taps_[i].y(), 0, 'f', 6)
                                     .arg(calibration_points[i].x(), 0, 'f', 6)
                                     .arg(calibration_points[i].y(), 0, 'f', 6);
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
    const bool accurate = std::hypot(x - verification_point.x(), y - verification_point.y()) <=
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
  if (reply->error() == QNetworkReply::NoError) {
    const auto document = QJsonDocument::fromJson(reply->readAll());
    if (document.isObject()) {
      state_ = document.object().toVariantMap();
      updateStatus();
    } else {
      state_["telemetry_fresh"] = false;
      status_ = "Dashboard sent invalid telemetry";
    }
  } else {
    state_["telemetry_fresh"] = false;
    status_ = "Dashboard connection lost";
  }
  updateGraphHistory();
  reply->deleteLater();
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
