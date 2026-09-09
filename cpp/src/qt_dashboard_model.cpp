#include "qt_dashboard_model.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QtMath>

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
}  // namespace

DashboardModel::DashboardModel(QUrl endpoint, QObject *parent)
    : QObject(parent), endpoint_(std::move(endpoint)), network_(new QNetworkAccessManager(this)) {
  network_->setTransferTimeout(1800);
  QTimer::singleShot(0, this, &DashboardModel::pollLive);
  QTimer::singleShot(0, this, &DashboardModel::pollNetworkMode);
  QTimer::singleShot(0, this, &DashboardModel::pollLogStatus);
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
