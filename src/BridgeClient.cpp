#include "BridgeClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcBridge, "bili.bridge")

// ---------------------------------------------------------------------------
// 环境探测：Python 解释器与 bridge 脚本位置
// ---------------------------------------------------------------------------

static bool fileExists(const QString &p) { return QFile::exists(p); }

QString BridgeClient::pythonExe() const
{
    // 1) 环境变量显式指定（优先）
    const QString envPy = qEnvironmentVariable("BILI_BRIDGE_PYTHON");
    if (!envPy.isEmpty() && fileExists(envPy))
        return envPy;

    // 2) 常见本机 Python（bili-digest 仅用标准库，3.10+ 均可）
    static const char *candidates[] = {
        "C:/Users/cyr/AppData/Local/Programs/Python/Python310/python.exe",
        "C:/Python312/python.exe",
        "C:/Python311/python.exe",
        "C:/Python310/python.exe",
    };
    for (const char *c : candidates) {
        if (fileExists(QString::fromLatin1(c)))
            return QString::fromLatin1(c);
    }

    // 3) PATH 兜底
    return QStringLiteral("python");
}

QString BridgeClient::bridgeScript() const
{
    // 优先与 exe 同目录（构建时已拷贝），其次回退到源码树
    const QString beside = QCoreApplication::applicationDirPath() + "/bili_bridge.py";
    if (fileExists(beside))
        return beside;
    return QStringLiteral(BILI_BRIDGE_SOURCE_DIR "/bridge/bili_bridge.py");
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &BridgeClient::onReadyRead);
    connect(&m_proc, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray err = m_proc.readAllStandardError();
        if (!err.isEmpty())
            qCWarning(lcBridge) << "[bridge stderr]" << err.trimmed();
    });
    connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BridgeClient::onFinished);
}

BridgeClient::~BridgeClient()
{
    stop();
}

bool BridgeClient::isRunning() const
{
    return m_proc.state() == QProcess::Running;
}

void BridgeClient::start()
{
    if (m_proc.state() != QProcess::NotRunning)
        return;

    const QString py = pythonExe();
    const QString script = bridgeScript();
    if (!fileExists(script)) {
        qCWarning(lcBridge) << "bridge script not found:" << script;
        return;
    }

    m_ready = false;
    m_starting = true;
    m_buffer.clear();

    m_proc.setProgram(py);
    m_proc.setArguments({script});
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    // 显式 UTF-8，避免 Windows 控制台编码问题（bridge 内部也 reconfigure 过）
    m_proc.setReadChannel(QProcess::StandardOutput);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUTF8", "1");
    m_proc.setProcessEnvironment(env);

    qCInfo(lcBridge) << "starting bridge:" << py << script;
    m_proc.start();
    if (!m_proc.waitForStarted(8000)) {
        qCWarning(lcBridge) << "failed to start bridge:" << m_proc.errorString();
        m_starting = false;
        emit bridgeExited(-1);
    }
}

void BridgeClient::stop()
{
    if (m_proc.state() == QProcess::NotRunning)
        return;
    m_proc.disconnect(this);
    m_proc.kill();
    m_proc.waitForFinished(3000);
    m_ready = false;
    m_starting = false;
}

void BridgeClient::restart()
{
    stop();
    start();
}

void BridgeClient::cancel()
{
    if (m_proc.state() != QProcess::NotRunning) {
        qCInfo(lcBridge) << "cancel: killing bridge pid" << m_proc.processId();
        const qint64 pending = m_pendingId;
        restart();
        if (pending >= 0)
            emit replyReceived(pending, false, {}, QStringLiteral("已取消"));
    }
    m_pendingId = -1;
}

// ---------------------------------------------------------------------------
// 调用
// ---------------------------------------------------------------------------

qint64 BridgeClient::call(const QString &cmd, const QJsonObject &params)
{
    if (m_pendingId >= 0) {
        qCWarning(lcBridge) << "bridge busy, rejecting call:" << cmd;
        return -1;
    }

    if (m_proc.state() == QProcess::NotRunning)
        start();
    if (m_proc.state() != QProcess::Running)
        return -1;

    const qint64 id = m_nextId++;
    QJsonObject req{{"id", id}, {"cmd", cmd}, {"params", params}};
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n";

    if (m_ready) {
        m_proc.write(line);
        m_proc.waitForBytesWritten(2000);
    } else {
        m_startupQueue.append(QString::fromUtf8(line));
    }

    m_pendingId = id;
    return id;
}

// ---------------------------------------------------------------------------
// 收包分发
// ---------------------------------------------------------------------------

void BridgeClient::onReadyRead()
{
    m_buffer += m_proc.readAllStandardOutput();

    int idx;
    while ((idx = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray raw = m_buffer.left(idx).trimmed();
        m_buffer.remove(0, idx + 1);
        if (raw.isEmpty())
            continue;

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            qCWarning(lcBridge) << "bad json line:" << raw.left(200) << perr.errorString();
            continue;
        }
        dispatch(doc.object());
    }
}

void BridgeClient::dispatch(const QJsonObject &msg)
{
    // 启动横幅
    if (msg.contains("event") && !msg.contains("id")) {
        const QString ev = msg.value("event").toString();
        if (ev == "ready") {
            m_ready = true;
            m_starting = false;
            // 冲刷启动前排队的请求
            for (const QString &line : std::as_const(m_startupQueue)) {
                m_proc.write(line.toUtf8() + "\n");
            }
            m_startupQueue.clear();
            m_proc.waitForBytesWritten(2000);
            qCInfo(lcBridge) << "bridge ready, pid" << msg.value("pid").toInt();
            emit bridgeReady();
        }
        return;
    }

    // 进度事件
    if (msg.value("event").toString() == "progress") {
        emit progressReceived(msg.value("id").toVariant().toLongLong(),
                              msg.value("stage").toString(),
                              msg.value("detail").toString());
        return;
    }

    // 最终结果
    const qint64 id = msg.value("id").toVariant().toLongLong();
    const bool ok = msg.value("ok").toBool();
    if (id == m_pendingId)
        m_pendingId = -1;

    if (ok)
        emit replyReceived(id, true, msg.value("data").toObject(), {});
    else
        emit replyReceived(id, false, {}, msg.value("error").toString());
}

void BridgeClient::onFinished(int exitCode, QProcess::ExitStatus)
{
    m_ready = false;
    m_starting = false;
    m_startupQueue.clear();

    if (m_pendingId >= 0) {
        const qint64 pending = m_pendingId;
        m_pendingId = -1;
        emit replyReceived(pending, false, {},
                           QStringLiteral("bridge 进程退出 (code=%1)").arg(exitCode));
    }
    emit bridgeExited(exitCode);
}
