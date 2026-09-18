#pragma once

#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWebSocket;

class DashboardModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  // #13: physical display rotation in degrees (0 or 180), read from the same
  // state file rapid-display-recovery writes. The panel rotates its scene by
  // this value; rotation is no longer an X/xrandr transform.
  Q_PROPERTY(int displayRotation READ displayRotation NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  // True once the live WebSocket push (#17) is delivering snapshots; false
  // while the model relies on its 200 ms HTTP poll fallback (socket never
  // connected yet, or lost and reconnecting).
  Q_PROPERTY(bool livePushActive READ livePushActive NOTIFY changed)
  Q_PROPERTY(QString networkMode READ networkMode NOTIFY changed)
  Q_PROPERTY(bool networkAvailable READ networkAvailable NOTIFY changed)
  Q_PROPERTY(QString logNotice READ logNotice NOTIFY changed)
  Q_PROPERTY(QString setupNotice READ setupNotice NOTIFY changed)
  Q_PROPERTY(bool pairingPending READ pairingPending NOTIFY changed)
  Q_PROPERTY(QString pairingLabel READ pairingLabel NOTIFY changed)
  Q_PROPERTY(QString pairingCode READ pairingCode NOTIFY changed)
  Q_PROPERTY(QString pairingTransaction READ pairingTransaction NOTIFY changed)
  Q_PROPERTY(bool pairingApprovalSent READ pairingApprovalSent NOTIFY changed)
  Q_PROPERTY(QVariantList graphSamples READ graphSamples NOTIFY changed)
  Q_PROPERTY(bool displayConfirmPending READ displayConfirmPending NOTIFY changed)
  Q_PROPERTY(bool displayConfirmSent READ displayConfirmSent NOTIFY changed)
  Q_PROPERTY(QString calibrationStage READ calibrationStage NOTIFY changed)
  Q_PROPERTY(QString calibrationMessage READ calibrationMessage NOTIFY changed)
  Q_PROPERTY(QPointF calibrationTarget READ calibrationTarget NOTIFY changed)

public:
  explicit DashboardModel(QUrl endpoint, QObject *parent = nullptr);
  int revision() const { return revision_; }
  int displayRotation() const { return display_rotation_; }
  QString status() const { return status_; }
  bool livePushActive() const { return live_push_active_; }
  QString networkMode() const { return network_mode_; }
  bool networkAvailable() const { return network_available_; }
  QString logNotice() const { return log_notice_; }
  QString setupNotice() const { return setup_notice_; }
  bool pairingPending() const { return pairing_pending_; }
  QString pairingLabel() const { return pairing_label_; }
  QString pairingCode() const { return pairing_code_; }
  QString pairingTransaction() const { return pairing_transaction_; }
  bool pairingApprovalSent() const { return pairing_approval_sent_; }
  QVariantList graphSamples() const { return graph_samples_; }
  bool displayConfirmPending() const { return display_confirm_pending_; }
  bool displayConfirmSent() const { return display_confirm_sent_; }
  // Empty when idle; otherwise capture, applying, verify, done or failed.
  QString calibrationStage() const { return calibration_stage_; }
  QString calibrationMessage() const { return calibration_message_; }
  // Normalized screen position of the target to tap, or (-1, -1) when none.
  QPointF calibrationTarget() const;

  Q_INVOKABLE QVariant value(const QString &key) const;
  Q_INVOKABLE QString timeValue(const QString &key) const;
  Q_INVOKABLE QString percentValue(const QString &key) const;
  Q_INVOKABLE void setNetworkMode(const QString &mode);
  Q_INVOKABLE bool approvePairing();
  Q_INVOKABLE bool confirmDisplay();
  Q_INVOKABLE void startCalibration();
  Q_INVOKABLE void calibrationTap(double x, double y);

signals:
  void changed();

private:
  void pollLive();
  void connectLiveSocket();
  void liveSocketConnected();
  void liveSocketDisconnected();
  void consumeLiveMessage(const QString &message);
  void pollNetworkMode();
  void pollLogStatus();
  void pollSetupStatus();
  void pollPairingPanel();
  void pollCalibrationFile();
  void pollDisplayConfirmation();
  void pollDisplayRotation();
  // Maps a normalized tap from the rotated scene's local coordinates to the
  // unrotated screen frame the calibration helper works in (#13).
  QPointF screenPoint(double x, double y) const;
  void runDisplayRecovery(const QStringList &arguments,
                          std::function<void(bool, const QString &)> done);
  void calibrationTimedOut();
  void finishCalibration(const QString &stage, const QString &message);
  void consumeLive(QNetworkReply *reply);
  void applyLiveState(QVariantMap state);
  void applyLiveFailure(const QString &message);
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
  QString pairing_transaction_;
  bool pairing_approval_sent_ = false;
  bool network_available_ = false;
  bool live_request_pending_ = false;
  QWebSocket *live_socket_ = nullptr;
  QUrl live_socket_url_;
  bool live_push_active_ = false;
  qint64 last_log_sequence_ = -1;
  QString last_graph_sample_;
  qint64 last_graph_gap_ms_ = 0;
  QVariantList graph_samples_;
  int display_rotation_ = 0;
  bool display_confirm_pending_ = false;
  bool display_confirm_sent_ = false;
  qint64 display_confirm_revision_ = -1;
  QString calibration_stage_;
  QString calibration_message_;
  QList<QPointF> calibration_taps_;
  QTimer *calibration_timeout_;
  QString calibration_signature_;
  bool calibration_signature_known_ = false;
  int calibration_generation_ = 0;
  int revision_ = 0;
};
