#pragma once

#include <QDialog>
#include <QJsonObject>

class BridgeClient;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

/**
 * LoginDialog —— B站扫码登录（方案参考 bilibiliDownloader 的
 * passport web qrcode generate / poll 流程）+ 手动粘贴 Cookie 兜底。
 * 模态打开：轮询期间独占 bridge 单请求通道，不会与主窗口抓取冲突。
 */
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(BridgeClient *bridge, QWidget *parent = nullptr);

private slots:
    void onReply(qint64 id, bool ok, const QJsonObject &data, const QString &error);
    void onPollTick();
    void onManualSave();
    void onCopyUrl();

private:
    void startQr();
    void stopPoll();
    void setStageText(const QString &text, const QString &color = QStringLiteral("#555"));
    void showQrImage(const QString &pngBase64);

    BridgeClient *m_bridge = nullptr;
    qint64 m_reqId = -1;
    QString m_qrKey;
    bool m_busy = false;       // 有请求在途
    bool m_loginDone = false;  // 登录成功，准备关闭

    QTimer *m_pollTimer = nullptr;
    QLabel *m_qrLabel = nullptr;
    QLabel *m_stageLabel = nullptr;
    QLineEdit *m_urlEdit = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QLineEdit *m_cookieEdit = nullptr;
    QPushButton *m_manualBtn = nullptr;
};
