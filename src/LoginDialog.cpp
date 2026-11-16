#include "LoginDialog.h"
#include "BridgeClient.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

LoginDialog::LoginDialog(BridgeClient *bridge, QWidget *parent)
    : QDialog(parent), m_bridge(bridge)
{
    setWindowTitle(QStringLiteral("登录 B站（扫码 / 手动 Cookie）"));
    setModal(true);
    resize(560, 420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto *hint = new QLabel(QStringLiteral(
        "用哔哩哔哩 APP 扫码登录（方案同 bilibiliDownloader：passport qrcode generate/poll）。"
        "登录态写入与 DSH 插件共享的凭据文件，抓取评论更稳、风控更少。"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#666"));
    root->addWidget(hint);

    // ---- 二维码区 ----
    auto *qrRow = new QHBoxLayout;
    m_qrLabel = new QLabel(QStringLiteral("二维码加载中…"), this);
    m_qrLabel->setFixedSize(260, 260);
    m_qrLabel->setAlignment(Qt::AlignCenter);
    m_qrLabel->setStyleSheet(QStringLiteral(
        "QLabel { background:#f5f5f5; border:1px solid #ddd; border-radius:6px; color:#999 }"));
    m_qrLabel->setWordWrap(true);

    auto *qrSide = new QVBoxLayout;
    m_stageLabel = new QLabel(QStringLiteral("正在获取二维码…"), this);
    m_stageLabel->setWordWrap(true);
    m_stageLabel->setStyleSheet(QStringLiteral("font-size:14px; color:#555"));
    m_refreshBtn = new QPushButton(QStringLiteral("🔄 刷新二维码"), this);
    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setReadOnly(true);
    m_urlEdit->setPlaceholderText(QStringLiteral("二维码链接（可用手机浏览器打开）"));
    auto *copyBtn = new QPushButton(QStringLiteral("复制链接"), this);
    connect(copyBtn, &QPushButton::clicked, this, &LoginDialog::onCopyUrl);
    connect(m_refreshBtn, &QPushButton::clicked, this, &LoginDialog::startQr);

    qrSide->addWidget(m_stageLabel, 1);
    qrSide->addWidget(m_refreshBtn);
    qrSide->addWidget(m_urlEdit);
    qrSide->addWidget(copyBtn);
    qrSide->addStretch(1);

    qrRow->addWidget(m_qrLabel);
    qrRow->addLayout(qrSide, 1);
    root->addLayout(qrRow);

    // ---- 手动粘贴 Cookie 兜底 ----
    root->addWidget(new QLabel(QStringLiteral("或手动粘贴 Cookie（SESSDATA 或整串，nav 接口验证后保存）："), this));
    auto *manualRow = new QHBoxLayout;
    m_cookieEdit = new QLineEdit(this);
    m_cookieEdit->setPlaceholderText(QStringLiteral("SESSDATA=xxxx; bili_jct=xxxx; DedeUserID=xxxx …"));
    m_manualBtn = new QPushButton(QStringLiteral("验证并保存"), this);
    manualRow->addWidget(m_cookieEdit, 1);
    manualRow->addWidget(m_manualBtn);
    root->addLayout(manualRow);

    auto *closeRow = new QHBoxLayout;
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    connect(closeBtn, &QPushButton::clicked, this, &LoginDialog::reject);
    closeRow->addStretch(1);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // ---- bridge 信号 ----
    connect(m_bridge, &BridgeClient::replyReceived, this, &LoginDialog::onReply);

    // ---- 轮询定时器 ----
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1500);
    connect(m_pollTimer, &QTimer::timeout, this, &LoginDialog::onPollTick);

    connect(m_manualBtn, &QPushButton::clicked, this, &LoginDialog::onManualSave);
    connect(m_cookieEdit, &QLineEdit::returnPressed, this, &LoginDialog::onManualSave);

    startQr();
}

void LoginDialog::setStageText(const QString &text, const QString &color)
{
    m_stageLabel->setText(text);
    m_stageLabel->setStyleSheet(
        QStringLiteral("font-size:14px; color:%1; font-weight:bold").arg(color));
}

void LoginDialog::showQrImage(const QString &pngBase64)
{
    if (pngBase64.isEmpty()) {
        m_qrLabel->setText(QStringLiteral("无法生成二维码图片\n请复制下方链接到手机打开"));
        return;
    }
    const QByteArray raw = QByteArray::fromBase64(pngBase64.toLatin1());
    const QImage img = QImage::fromData(raw, "PNG");
    if (img.isNull()) {
        m_qrLabel->setText(QStringLiteral("二维码图片解码失败\n请复制下方链接到手机打开"));
        return;
    }
    const QPixmap pm = QPixmap::fromImage(img);
    m_qrLabel->setPixmap(pm.scaled(m_qrLabel->size(),
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void LoginDialog::startQr()
{
    if (m_loginDone)
        return;
    stopPoll();
    m_busy = true;
    m_refreshBtn->setEnabled(false);
    setStageText(QStringLiteral("正在获取二维码…"));
    m_qrLabel->setText(QStringLiteral("二维码加载中…"));
    m_reqId = m_bridge->call(QStringLiteral("qr_start"), QJsonObject{});
    if (m_reqId < 0) {
        m_busy = false;
        m_refreshBtn->setEnabled(true);
        setStageText(QStringLiteral("bridge 忙碌或未就绪，请稍后点「刷新二维码」"),
                     QStringLiteral("#c0392b"));
    }
}

void LoginDialog::onPollTick()
{
    if (m_loginDone || m_busy || m_reqId >= 0 || m_qrKey.isEmpty())
        return;
    m_busy = true;
    m_reqId = m_bridge->call(QStringLiteral("qr_poll"),
                             QJsonObject{{"qr_key", m_qrKey}});
    if (m_reqId < 0)
        m_busy = false;   // 主通道被占（异常情况），下个 tick 再试
}

void LoginDialog::onReply(qint64 id, bool ok, const QJsonObject &data, const QString &error)
{
    if (id != m_reqId)
        return;
    m_reqId = -1;
    m_busy = false;
    m_refreshBtn->setEnabled(true);

    if (!ok) {
        setStageText(QStringLiteral("请求失败: %1").arg(error.left(80)),
                     QStringLiteral("#c0392b"));
        m_pollTimer->stop();
        return;
    }

    // ---- qr_start ----
    if (data.contains(QStringLiteral("qr_key"))) {
        m_qrKey = data.value(QStringLiteral("qr_key")).toString();
        m_urlEdit->setText(data.value(QStringLiteral("url")).toString());
        showQrImage(data.value(QStringLiteral("png_base64")).toString());
        setStageText(QStringLiteral("请用哔哩哔哩 APP 扫码…"));
        m_pollTimer->start();
        return;
    }

    // ---- qr_poll ----
    if (data.contains(QStringLiteral("status"))) {
        const QString st = data.value(QStringLiteral("status")).toString();
        const QString msg = data.value(QStringLiteral("message")).toString();
        if (st == QLatin1String("waiting")) {
            setStageText(QStringLiteral("请用哔哩哔哩 APP 扫码…（%1）").arg(msg));
        } else if (st == QLatin1String("scanned")) {
            setStageText(QStringLiteral("✅ 已扫码，请在手机上点「确认登录」"),
                         QStringLiteral("#e67e22"));
        } else if (st == QLatin1String("expired")) {
            setStageText(QStringLiteral("二维码已过期，正在自动刷新…"),
                         QStringLiteral("#e67e22"));
            startQr();
        } else if (st == QLatin1String("ok")) {
            // 登录成功：保存 Cookie（nav 验证）
            stopPoll();
            setStageText(QStringLiteral("✅ 扫码成功，正在验证并保存登录态…"),
                         QStringLiteral("#2e7d32"));
            m_busy = true;
            m_refreshBtn->setEnabled(false);
            m_reqId = m_bridge->call(QStringLiteral("login_save"),
                                     QJsonObject{{"cookie", data.value(QStringLiteral("cookie")).toString()}});
            if (m_reqId < 0) {
                m_busy = false;
                setStageText(QStringLiteral("保存请求发出失败，请重试"), QStringLiteral("#c0392b"));
            }
        } else {
            setStageText(QStringLiteral("轮询异常: %1 (%2)").arg(msg).arg(st),
                         QStringLiteral("#c0392b"));
        }
        return;
    }

    // ---- login_save ----
    if (data.contains(QStringLiteral("saved"))) {
        const bool saved = data.value(QStringLiteral("saved")).toBool();
        const QString uname = data.value(QStringLiteral("uname")).toString();
        if (saved && data.value(QStringLiteral("is_login")).toBool()) {
            m_loginDone = true;
            m_manualBtn->setEnabled(false);
            m_cookieEdit->setEnabled(false);
            setStageText(QStringLiteral("🎉 登录成功：%1\n登录态已写入共享凭据文件（%2）")
                             .arg(uname,
                                  QDateTime::currentDateTime().toString("hh:mm:ss")),
                         QStringLiteral("#2e7d32"));
            QTimer::singleShot(1100, this, &LoginDialog::accept);
        } else {
            setStageText(QStringLiteral("❌ 保存失败: %1")
                             .arg(data.value(QStringLiteral("error")).toString().left(120)),
                         QStringLiteral("#c0392b"));
        }
    }
}

void LoginDialog::onManualSave()
{
    const QString cookie = m_cookieEdit->text().trimmed();
    if (cookie.isEmpty()) {
        setStageText(QStringLiteral("请先粘贴 SESSDATA 或完整 Cookie"), QStringLiteral("#c0392b"));
        return;
    }
    if (m_reqId >= 0)
        return;   // 通道在途
    m_busy = true;
    m_manualBtn->setEnabled(false);
    setStageText(QStringLiteral("正在验证粘贴的 Cookie…"));
    m_reqId = m_bridge->call(QStringLiteral("login_save"),
                             QJsonObject{{"cookie", cookie}});
    if (m_reqId < 0) {
        m_busy = false;
        m_manualBtn->setEnabled(true);
        setStageText(QStringLiteral("bridge 忙碌，请稍后重试"), QStringLiteral("#c0392b"));
    }
}

void LoginDialog::onCopyUrl()
{
    if (m_urlEdit->text().isEmpty())
        return;
    QApplication::clipboard()->setText(m_urlEdit->text());
    setStageText(QStringLiteral("链接已复制到剪贴板"), QStringLiteral("#2e7d32"));
}

void LoginDialog::stopPoll()
{
    m_pollTimer->stop();
}
