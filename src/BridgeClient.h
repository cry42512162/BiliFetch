#pragma once

#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

/**
 * BridgeClient —— 管理 bili_bridge.py 子进程，按「一行一个 JSON」协议通信。
 *
 * 协议：
 *   发送:  {"id": N, "cmd": "...", "params": {...}}
 *   接收:  {"id": N, "ok": true, "data": {...}} / {"id": N, "ok": false, "error": "..."}
 *          {"id": N, "event": "progress", "stage": "...", "detail": "..."}
 *          {"event": "ready", ...}   （子进程启动横幅）
 *
 * 用法：
 *   auto *c = new BridgeClient(this);
 *   connect(c, &BridgeClient::replyReceived, ...);
 *   c->call("digest", QJsonObject{{"target", bv}});
 */
class BridgeClient : public QObject
{
    Q_OBJECT
public:
    explicit BridgeClient(QObject *parent = nullptr);
    ~BridgeClient() override;

    /** 发起一次调用；返回请求 id（-1 表示启动失败/未就绪） */
    qint64 call(const QString &cmd, const QJsonObject &params = {});

    /** 取消正在执行的调用（kill 子进程并重启） */
    void cancel();

    bool isRunning() const;
    bool isBusy() const { return m_pendingId >= 0; }

    /** 显式启动（call 时也会懒启动） */
    void start();
    void stop();

signals:
    /** 收到某次调用的最终结果 */
    void replyReceived(qint64 id, bool ok, const QJsonObject &data, const QString &error);
    /** 调用过程中的进度事件 */
    void progressReceived(qint64 id, const QString &stage, const QString &detail);
    /** 子进程就绪（横幅到达） */
    void bridgeReady();
    /** 子进程意外退出（用于 UI 提示） */
    void bridgeExited(int exitCode);

private slots:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    void dispatch(const QJsonObject &msg);
    void restart();

    QProcess m_proc;
    QByteArray m_buffer;
    qint64 m_nextId = 1;
    qint64 m_pendingId = -1;   // 当前在途请求，-1 = 空闲
    bool m_ready = false;
    bool m_starting = false;
    QStringList m_startupQueue; // ready 之前排队的请求行

    QString pythonExe() const;
    QString bridgeScript() const;
};
