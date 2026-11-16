#pragma once

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QPushButton;
class QLabel;
class QTabWidget;
class QTreeWidget;
class QTextBrowser;
class QPlainTextEdit;
class QProgressBar;
class BridgeClient;

/**
 * MainWindow —— BiliFetch 主窗口
 * 单输入框 auto 模糊路由（视频/专栏/动态/UP主/关键词）
 * + UP主全部视频/专栏双清单勾选抓取
 * + 批量任务队列（串行节流抓取）+ 评论导出 CSV
 * + 自定义输出目录（QSettings 持久化）
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    /** 命令行启动直接抓取：填入 target 并立即执行一次 auto 抓取 */
    void openTarget(const QString &target);

private slots:
    void onFetch();
    void onReply(qint64 id, bool ok, const QJsonObject &data, const QString &error);
    void onProgress(qint64 id, const QString &stage, const QString &detail);
    void onBridgeExited(int exitCode);
    void onCancel();
    void onOpenMarkdown();
    void onSendToBatch();
    void onExportCsv();
    void onExportMd();

    // 批量队列
    void onBatchStart();
    void onBatchCancel();

    // UP 主清单
    void onUpSelectAll();
    void onUpSelectNone();
    void onUpSendToBatch();

    // 设置
    void onPickOutputDir();
    void onResetOutputDir();

    // 账号（扫码登录 / 退出）
    void onLogin();
    void onLogout();

private:
    void updateLoginLabel(const QJsonObject &data);
    void buildUi();
    void buildMenus();
    void loadSettings();
    void applyOutputDirToParams(QJsonObject &params) const;
    void updateOutputDirLabel();

    void setBusy(bool busy);
    void setBatchBusy(bool busy);

    // auto 结果渲染（按 kind 分发）
    void renderAutoResult(const QJsonObject &data);
    void renderVideo(const QJsonObject &data);
    void renderArticle(const QJsonObject &data);
    void renderDynamic(const QJsonObject &data);
    void renderUpContent(const QJsonObject &data);
    void renderKeyword(const QJsonObject &data);

    void renderComments(const QJsonObject &comments, const QString &scope = {});
    void renderBatchResult(const QJsonObject &data);

    static QString formatCount(qint64 n);
    static QString formatTime(qint64 epoch);
    static QString mdToHtml(const QString &md, int maxChars = 3000);
    static void fillListTree(QTreeWidget *tree, const QJsonArray &items,
                             const QString &kind);

    BridgeClient *m_bridge = nullptr;
    qint64 m_requestId = -1;
    QString m_pendingKind;          // "auto" | "batch"
    QString m_markdownFile;
    QString m_outputDir;            // 空 = 插件默认目录

    // 最近一次评论数据（CSV/MD 导出用）
    QJsonArray m_lastComments;
    QJsonObject m_lastCommentsObj;   // 含 items/mode_desc/total
    QString m_lastTitle;

    // keyword 清单（「→ 批量」用）
    QJsonArray m_lastListItems;
    QString m_lastListKind;

    // ---- 控件：抓取行 ----
    QLineEdit *m_input = nullptr;
    QComboBox *m_mode = nullptr;
    QSpinBox *m_maxComments = nullptr;
    QPushButton *m_fetchBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_sendBatchBtn = nullptr;
    QPushButton *m_openMdBtn = nullptr;
    QLabel *m_status = nullptr;

    // ---- 控件：页签 ----
    QTabWidget *m_tabs = nullptr;
    QTextBrowser *m_infoPane = nullptr;
    QTreeWidget *m_commentTree = nullptr;
    QPushButton *m_exportCsvBtn = nullptr;
    QPushButton *m_exportMdBtn = nullptr;
    QTextBrowser *m_aiPane = nullptr;
    QTextBrowser *m_subPane = nullptr;

    // ---- UP 主内容页签 ----
    QTabWidget *m_upTabs = nullptr;
    QTreeWidget *m_upVideoTree = nullptr;
    QTreeWidget *m_upArticleTree = nullptr;
    QLabel *m_upSummary = nullptr;

    // ---- 批量页签 ----
    QPlainTextEdit *m_batchInput = nullptr;
    QSpinBox *m_batchSleep = nullptr;
    QSpinBox *m_batchBackoff = nullptr;
    QSpinBox *m_batchRetries = nullptr;
    QPushButton *m_batchStartBtn = nullptr;
    QPushButton *m_batchCancelBtn = nullptr;
    QProgressBar *m_batchProgress = nullptr;
    QTreeWidget *m_batchResult = nullptr;
    int m_batchTotal = 0;

    // ---- 输出目录标签 ----
    QLabel *m_outDirLabel = nullptr;
    QLabel *m_loginLabel = nullptr;
};
