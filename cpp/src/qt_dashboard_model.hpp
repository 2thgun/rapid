#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;

class DashboardModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QString networkMode READ networkMode NOTIFY changed)
  Q_PROPERTY(bool networkAvailable READ networkAvailable NOTIFY changed)
  Q_PROPERTY(QString logNotice READ logNotice NOTIFY changed)
  Q_PROPERTY(QString setupNotice READ setupNotice NOTIFY changed)
  Q_PROPERTY(bool pairingPending READ pairingPending NOTIFY changed)
  Q_PROPERTY(QString pairingLabel READ pairingLabel NOTIFY changed)
  Q_PROPERTY(QString pairingCode READ pairingCode NOTIFY changed)
  Q_PROPERTY(QVariantList graphSamples READ graphSamples NOTIFY changed)

public:
  explicit DashboardModel(QUrl endpoint, QObject *parent = nullptr);
  int revision() const { return revision_; }
  QString status() const { return status_; }
  QString networkMode() const { return network_mode_; }
  bool networkAvailable() const { return network_available_; }
  QString logNotice() const { return log_notice_; }
  QString setupNotice() const { return setup_notice_; }
  bool pairingPending() const { return pairing_pending_; }
  QString pairingLabel() const { return pairing_label_; }
  QString pairingCode() const { return pairing_code_; }
  QVariantList graphSamples() const { return graph_samples_; }

  Q_INVOKABLE QVariant value(const QString &key) const;
  Q_INVOKABLE QString timeValue(const QString &key) const;
  Q_INVOKABLE QString percentValue(const QString &key) const;
  Q_INVOKABLE void setNetworkMode(const QString &mode);

signals:
  void changed();

private:
  void pollLive();
  void pollNetworkMode();
  void pollLogStatus();
  void pollSetupStatus();
  void pollPairingPanel();
  void consumeLive(QNetworkReply *reply);
  void consumeNetworkMode(QNetworkReply *reply);
  void consumeLogStatus(QNetworkReply *reply);
  void updateStatus();
  void updateGraphHistory();
  void bump();

  QUrl endpoint_;
  QNetworkAccessManager *network_;
  QVariantMap state_;
  QString status_ = "Connecting to dashboard…";
  QString network_mode_;
  QString log_notice_;
  QString setup_notice_;
  bool pairing_pending_ = false;
  QString pairing_label_;
  QString pairing_code_;
  bool network_available_ = false;
  bool live_request_pending_ = false;
  qint64 last_log_sequence_ = -1;
  QString last_graph_sample_;
  qint64 last_graph_gap_ms_ = 0;
  QVariantList graph_samples_;
  int revision_ = 0;
};
