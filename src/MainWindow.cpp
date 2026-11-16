#include "MainWindow.h"
#include "BridgeClient.h"
#include "LoginDialog.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QUrl>
#include <QVariant>

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

QString MainWindow::formatCount(qint64 n)
{
    if (n >= 100000000)
        return QString::number(n / 100000000.0, 'f', 1) + QStringLiteral("亿");
    if (n >= 10000)
        return QString::number(n / 10000.0, 'f', 1) + QStringLiteral("万");
    return QString::number(n);
}

QString MainWindow::formatTime(qint64 epoch)
{
    if (epoch <= 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(epoch).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

/** 极简 Markdown -> HTML（正文预览用：转义 + 换行/加粗/标题） */
QString MainWindow::mdToHtml(const QString &md, int maxChars)
{
    QString text = md;
    if (text.size() > maxChars)
        text = text.left(maxChars) + QStringLiteral("\n\n…（正文过长，仅预览前 %1 字）").arg(maxChars);

    text = text.toHtmlEscaped();
    text.replace(QRegularExpression(QStringLiteral("^#{3,} (.*)$"),
                                    QRegularExpression::MultilineOption),
                 QStringLiteral("<h3>\\1</h3>"));
    text.replace(QRegularExpression(QStringLiteral("^## (.*)$"),
                                    QRegularExpression::MultilineOption),
                 QStringLiteral("<h2>\\1</h2>"));
    text.replace(QRegularExpression(QStringLiteral("^# (.*)$"),
                                    QRegularExpression::MultilineOption),
                 QStringLiteral("<h1>\\1</h1>"));
    text.replace(QRegularExpression(QStringLiteral("\\*\\*(.+?)\\*\\*")),
                 QStringLiteral("<b>\\1</b>"));
    text.replace(QRegularExpression(QStringLiteral("^[-*] (.*)$"),
                                    QRegularExpression::MultilineOption),
                 QStringLiteral("• \\1"));
    text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return text;
}

/** UP 清单树填充（带复选框，项本身可勾选） */
void MainWindow::fillListTree(QTreeWidget *tree, const QJsonArray &items,
                              const QString &kind)
{
    tree->clear();
    tree->setUpdatesEnabled(false);
    for (const QJsonValue &v : items) {
        const auto o = v.toObject();
        auto *item = new QTreeWidgetItem(tree);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);

        const qint64 pub = o.value(QStringLiteral("pubdate")).toVariant().toLongLong();
        item->setText(1, formatTime(pub));
        item->setText(2, o.value(QStringLiteral("title")).toString());

        if (kind == QLatin1String("video")) {
            item->setText(3, o.value(QStringLiteral("duration")).toString());
            item->setText(4, formatCount(o.value(QStringLiteral("play")).toVariant().toLongLong()));
            item->setText(5, o.value(QStringLiteral("bvid")).toString());
            item->setToolTip(2, o.value(QStringLiteral("description")).toString());
        } else {
            const int words = o.value(QStringLiteral("words")).toInt();
            item->setText(3, QStringLiteral("%1 字").arg(formatCount(words)));
            item->setText(4, formatCount(o.value(QStringLiteral("view")).toVariant().toLongLong()));
            item->setText(5, QStringLiteral("cv%1").arg(o.value(QStringLiteral("cv_id")).toString()));
            item->setToolTip(2, o.value(QStringLiteral("summary")).toString());
        }
        item->setData(0, Qt::UserRole,
                      kind == QLatin1String("video")
                          ? o.value(QStringLiteral("bvid")).toString()
                          : QStringLiteral("https://www.bilibili.com/read/cv%1")
                                .arg(o.value(QStringLiteral("cv_id")).toString()));
        // 未勾选项的选中态字色
        item->setForeground(2, QColor(QStringLiteral("#222")));
    }
    tree->setUpdatesEnabled(true);
    tree->viewport()->update();
}

// ---------------------------------------------------------------------------
// 构造 / 设置
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("BiliFetch —— B站内容抓取（bili-digest 驱动）"));
    resize(1220, 820);

    loadSettings();
    buildUi();
    buildMenus();
    updateOutputDirLabel();

    m_bridge = new BridgeClient(this);
    connect(m_bridge, &BridgeClient::replyReceived, this, &MainWindow::onReply);
    connect(m_bridge, &BridgeClient::progressReceived, this, &MainWindow::onProgress);
    connect(m_bridge, &BridgeClient::bridgeExited, this, &MainWindow::onBridgeExited);
    connect(m_bridge, &BridgeClient::bridgeReady, this, [this] {
        if (m_requestId < 0)
            m_status->setText(QStringLiteral("桥接就绪"));
    });

    // 启动后查询登录态（nav 接口），状态栏显示账号
    QTimer::singleShot(700, this, [this] {
        if (m_requestId >= 0)
            return;
        m_pendingKind = QStringLiteral("check");
        m_requestId = m_bridge->call(QStringLiteral("login_check"), QJsonObject{});
        if (m_requestId < 0)
            m_pendingKind.clear();
    });
}

void MainWindow::loadSettings()
{
    QSettings s;
    m_outputDir = s.value(QStringLiteral("paths/outputDir")).toString();
}

void MainWindow::applyOutputDirToParams(QJsonObject &params) const
{
    if (!m_outputDir.isEmpty())
        params.insert(QStringLiteral("output_dir"), m_outputDir);
}

void MainWindow::updateOutputDirLabel()
{
    QString disp = m_outputDir.isEmpty()
        ? QStringLiteral("D:\\软件\\AI工具\\输出\\B站（默认）")
        : m_outputDir;
    m_outDirLabel->setText(QStringLiteral("📁 输出: %1").arg(disp));
    m_outDirLabel->setToolTip(m_outputDir.isEmpty()
        ? QStringLiteral("使用插件默认输出目录，点击「设置 → 输出目录…」更改")
        : disp);
}

void MainWindow::buildMenus()
{
    QMenu *menuAccount = menuBar()->addMenu(QStringLiteral("账号(&A)"));

    QAction *actLogin = menuAccount->addAction(QStringLiteral("登录（扫码 / 粘贴 Cookie）(&L)"));
    actLogin->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(actLogin, &QAction::triggered, this, &MainWindow::onLogin);

    QAction *actLogout = menuAccount->addAction(QStringLiteral("退出登录(&O)"));
    connect(actLogout, &QAction::triggered, this, &MainWindow::onLogout);

    QMenu *menuSettings = menuBar()->addMenu(QStringLiteral("设置(&S)"));

    QAction *actPick = menuSettings->addAction(QStringLiteral("输出目录(&O)…"));
    connect(actPick, &QAction::triggered, this, &MainWindow::onPickOutputDir);

    QAction *actReset = menuSettings->addAction(QStringLiteral("恢复默认输出目录(&R)"));
    connect(actReset, &QAction::triggered, this, &MainWindow::onResetOutputDir);

    menuSettings->addSeparator();
    QAction *actQuit = menuSettings->addAction(QStringLiteral("退出(&Q)"));
    connect(actQuit, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::onPickOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        m_outputDir.isEmpty() ? QStringLiteral("选择输出目录（当前：插件默认）")
                              : QStringLiteral("选择输出目录"),
        m_outputDir.isEmpty() ? QStringLiteral("D:/软件/AI工具/输出/B站") : m_outputDir);
    if (dir.isEmpty())
        return;
    m_outputDir = dir;
    QSettings s;
    s.setValue(QStringLiteral("paths/outputDir"), dir);
    updateOutputDirLabel();
    m_status->setText(QStringLiteral("输出目录已改为: %1").arg(dir));
}

void MainWindow::onResetOutputDir()
{
    m_outputDir.clear();
    QSettings s;
    s.remove(QStringLiteral("paths/outputDir"));
    updateOutputDirLabel();
    m_status->setText(QStringLiteral("已恢复插件默认输出目录"));
}

// ---------------------------------------------------------------------------
// 账号：扫码登录 / 手动 Cookie / 退出
// ---------------------------------------------------------------------------

void MainWindow::updateLoginLabel(const QJsonObject &data)
{
    const bool isLogin = data.value(QStringLiteral("is_login")).toBool();
    const QString uname = data.value(QStringLiteral("uname")).toString();
    const qint64 mid = data.value(QStringLiteral("mid")).toVariant().toLongLong();
    if (isLogin && !uname.isEmpty()) {
        m_loginLabel->setText(QStringLiteral("👤 %1").arg(uname));
        m_loginLabel->setStyleSheet(QStringLiteral("color:#2e7d32; padding-right:8px"));
        m_loginLabel->setToolTip(
            QStringLiteral("已登录（UID %1）｜登录态与 DSH bili-digest 插件共享").arg(mid));
    } else {
        m_loginLabel->setText(QStringLiteral("👤 未登录"));
        m_loginLabel->setStyleSheet(QStringLiteral("color:#888; padding-right:8px"));
        m_loginLabel->setToolTip(
            QStringLiteral("未登录：菜单「账号 → 登录」扫码即可（登录后评论抓取更稳）"));
    }
}

void MainWindow::onLogin()
{
    if (m_requestId >= 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先等待当前抓取完成或点「取消」。"));
        return;
    }
    LoginDialog dlg(m_bridge, this);
    const int r = dlg.exec();
    if (r == QDialog::Accepted) {
        // 刷新状态栏登录态
        m_pendingKind = QStringLiteral("check");
        m_requestId = m_bridge->call(QStringLiteral("login_check"), QJsonObject{});
        if (m_requestId < 0)
            m_pendingKind.clear();
        m_status->setText(QStringLiteral("✅ 登录成功，登录态已与 DSH 插件共享"));
    }
}

void MainWindow::onLogout()
{
    if (m_requestId >= 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先等待当前任务完成或点「取消」。"));
        return;
    }
    const auto ret = QMessageBox::question(
        this, QStringLiteral("退出登录"),
        QStringLiteral("确定清除本机共享凭据中的 B站 Cookie 吗？\n"
                       "（同时影响 DSH 的 bili-digest 插件登录态）"));
    if (ret != QMessageBox::Yes)
        return;
    m_pendingKind = QStringLiteral("logout");
    m_requestId = m_bridge->call(QStringLiteral("logout"), QJsonObject{});
    if (m_requestId < 0)
        m_pendingKind.clear();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // ---- 顶部输入行 ----
    auto *top = new QHBoxLayout;
    m_input = new QLineEdit(central);
    m_input->setPlaceholderText(QStringLiteral(
        "粘贴任意 B站 输入：BV 号 / 视频链接 / 专栏链接(read/cv) 或 cv 号 / 动态 t.bilibili.com / "
        "UP主 space 主页（列全部视频+专栏）/ 关键词 …（自动识别）"));
    m_input->setClearButtonEnabled(true);

    m_mode = new QComboBox(central);
    m_mode->addItem(QStringLiteral("🔥 热门精选 hot_60"), QStringLiteral("hot_60"));
    m_mode->addItem(QStringLiteral("🌐 全量抓取 all_100"), QStringLiteral("all_100"));
    m_mode->setToolTip(QStringLiteral("评论抓取模式（复用 bili-digest 双模式）"));

    m_maxComments = new QSpinBox(central);
    m_maxComments->setRange(1, 1500);
    m_maxComments->setValue(50);
    m_maxComments->setPrefix(QStringLiteral("上限 "));
    m_maxComments->setToolTip(QStringLiteral("最大评论条数"));

    m_fetchBtn = new QPushButton(QStringLiteral("抓取"), central);
    m_fetchBtn->setDefault(true);
    m_cancelBtn = new QPushButton(QStringLiteral("取消"), central);
    m_cancelBtn->setEnabled(false);
    m_sendBatchBtn = new QPushButton(QStringLiteral("→ 批量"), central);
    m_sendBatchBtn->setToolTip(QStringLiteral("把关键词搜索结果的全部 BV 加入批量队列"));
    m_sendBatchBtn->setEnabled(false);
    m_openMdBtn = new QPushButton(QStringLiteral("打开 Markdown"), central);
    m_openMdBtn->setEnabled(false);

    top->addWidget(m_input, 1);
    top->addWidget(m_mode);
    top->addWidget(m_maxComments);
    top->addWidget(m_fetchBtn);
    top->addWidget(m_cancelBtn);
    top->addWidget(m_sendBatchBtn);
    top->addWidget(m_openMdBtn);
    root->addLayout(top);

    // ---- 页签区 ----
    m_tabs = new QTabWidget(central);

    // 📺 内容页
    m_infoPane = new QTextBrowser(m_tabs);
    m_infoPane->setOpenExternalLinks(true);

    // 💬 评论页（树 + 导出按钮）
    auto *commentPage = new QWidget(m_tabs);
    auto *commentLayout = new QVBoxLayout(commentPage);
    commentLayout->setContentsMargins(0, 0, 0, 0);
    commentLayout->setSpacing(4);
    auto *commentBar = new QHBoxLayout;
    m_exportCsvBtn = new QPushButton(QStringLiteral("📥 导出 CSV"), commentPage);
    m_exportCsvBtn->setEnabled(false);
    m_exportCsvBtn->setToolTip(QStringLiteral("导出当前评论树（含楼中楼）为 CSV，Excel 可直接打开"));
    m_exportMdBtn = new QPushButton(QStringLiteral("📤 导出 MD"), commentPage);
    m_exportMdBtn->setEnabled(false);
    m_exportMdBtn->setToolTip(QStringLiteral("把当前评论树（含楼中楼）导出为独立 Markdown 文件"));
    auto *commentHint = new QLabel(QStringLiteral("评论数据仅存在于当前会话，导出请点左侧按钮"), commentPage);
    commentHint->setStyleSheet(QStringLiteral("color:#888"));
    commentBar->addWidget(m_exportCsvBtn);
    commentBar->addWidget(m_exportMdBtn);
    commentBar->addWidget(commentHint, 1);
    commentLayout->addLayout(commentBar);

    m_commentTree = new QTreeWidget(commentPage);
    m_commentTree->setColumnCount(4);
    m_commentTree->setHeaderLabels({
        QStringLiteral("内容"), QStringLiteral("用户"),
        QStringLiteral("点赞"), QStringLiteral("时间")});
    m_commentTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_commentTree->header()->resizeSection(1, 160);
    m_commentTree->header()->resizeSection(2, 80);
    m_commentTree->header()->resizeSection(3, 130);
    m_commentTree->setUniformRowHeights(false);
    m_commentTree->setWordWrap(true);
    commentLayout->addWidget(m_commentTree, 1);

    // 🤖 AI / 📝 字幕
    m_aiPane = new QTextBrowser(m_tabs);
    m_aiPane->setOpenExternalLinks(true);
    m_subPane = new QTextBrowser(m_tabs);
    m_subPane->setOpenExternalLinks(true);

    // 📂 UP主内容页（视频 / 专栏 双清单，按发布时间倒序，可勾选）
    auto *upPage = new QWidget(m_tabs);
    auto *upLayout = new QVBoxLayout(upPage);
    upLayout->setContentsMargins(0, 0, 0, 0);
    upLayout->setSpacing(4);

    auto *upBar = new QHBoxLayout;
    auto *upAllBtn = new QPushButton(QStringLiteral("☑ 全选当前页"), upPage);
    auto *upNoneBtn = new QPushButton(QStringLiteral("☐ 全不选"), upPage);
    auto *upToBatchBtn = new QPushButton(QStringLiteral("🚀 抓取选中 → 批量队列"), upPage);
    upToBatchBtn->setToolTip(QStringLiteral("把勾选的视频(BV)与专栏(cv)送入批量队列，串行节流抓取"));
    m_upSummary = new QLabel(QStringLiteral("先抓取一个 UP 主主页以加载清单"), upPage);
    m_upSummary->setStyleSheet(QStringLiteral("color:#888"));
    upBar->addWidget(upAllBtn);
    upBar->addWidget(upNoneBtn);
    upBar->addWidget(upToBatchBtn);
    upBar->addWidget(m_upSummary, 1);
    upLayout->addLayout(upBar);

    m_upTabs = new QTabWidget(upPage);

    m_upVideoTree = new QTreeWidget(m_upTabs);
    m_upVideoTree->setColumnCount(6);
    m_upVideoTree->setHeaderLabels({
        QStringLiteral("✓"), QStringLiteral("发布时间"), QStringLiteral("标题"),
        QStringLiteral("时长"), QStringLiteral("播放"), QStringLiteral("BV号")});
    m_upVideoTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_upVideoTree->header()->resizeSection(0, 24);
    m_upVideoTree->header()->resizeSection(1, 130);
    m_upVideoTree->header()->resizeSection(3, 70);
    m_upVideoTree->header()->resizeSection(4, 80);
    m_upVideoTree->header()->resizeSection(5, 120);
    m_upVideoTree->setRootIsDecorated(false);
    m_upVideoTree->setSortingEnabled(true);
    m_upVideoTree->sortByColumn(1, Qt::DescendingOrder);

    m_upArticleTree = new QTreeWidget(m_upTabs);
    m_upArticleTree->setColumnCount(6);
    m_upArticleTree->setHeaderLabels({
        QStringLiteral("✓"), QStringLiteral("发布时间"), QStringLiteral("标题"),
        QStringLiteral("字数"), QStringLiteral("阅读"), QStringLiteral("专栏号")});
    m_upArticleTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_upArticleTree->header()->resizeSection(0, 24);
    m_upArticleTree->header()->resizeSection(1, 130);
    m_upArticleTree->header()->resizeSection(3, 80);
    m_upArticleTree->header()->resizeSection(4, 80);
    m_upArticleTree->header()->resizeSection(5, 100);
    m_upArticleTree->setRootIsDecorated(false);
    m_upArticleTree->setSortingEnabled(true);
    m_upArticleTree->sortByColumn(1, Qt::DescendingOrder);

    m_upTabs->addTab(m_upVideoTree, QStringLiteral("🎬 视频"));
    m_upTabs->addTab(m_upArticleTree, QStringLiteral("📖 专栏"));
    upLayout->addWidget(m_upTabs, 1);

    connect(upAllBtn, &QPushButton::clicked, this, &MainWindow::onUpSelectAll);
    connect(upNoneBtn, &QPushButton::clicked, this, &MainWindow::onUpSelectNone);
    connect(upToBatchBtn, &QPushButton::clicked, this, &MainWindow::onUpSendToBatch);

    // 📦 批量页
    auto *batchPage = new QWidget(m_tabs);
    auto *batchLayout = new QVBoxLayout(batchPage);
    batchLayout->setContentsMargins(4, 4, 4, 4);
    batchLayout->setSpacing(6);

    auto *batchHint = new QLabel(QStringLiteral(
        "每行一个目标（BV / 链接 / 专栏 cv / 动态 / 任意可路由输入）。串行抓取，"
        "操作间强制休眠；遇 412/-352 风控自动退避重试（复用 bili-batch 逻辑）。"), batchPage);
    batchHint->setWordWrap(true);
    batchHint->setStyleSheet(QStringLiteral("color:#666"));
    batchLayout->addWidget(batchHint);

    m_batchInput = new QPlainTextEdit(batchPage);
    m_batchInput->setPlaceholderText(QStringLiteral(
        "BV1GJ411x7h7\nhttps://www.bilibili.com/read/cv24381303\n"
        "https://www.bilibili.com/video/BV1xxxx\n…（每行一个）"));
    m_batchInput->setMaximumHeight(110);
    batchLayout->addWidget(m_batchInput);

    auto *batchCtrl = new QHBoxLayout;
    m_batchSleep = new QSpinBox(batchPage);
    m_batchSleep->setRange(0, 3600);
    m_batchSleep->setValue(30);
    m_batchSleep->setPrefix(QStringLiteral("间隔 "));
    m_batchSleep->setSuffix(QStringLiteral("s"));
    m_batchSleep->setToolTip(QStringLiteral("两次抓取之间的强制休眠秒数"));

    m_batchBackoff = new QSpinBox(batchPage);
    m_batchBackoff->setRange(10, 3600);
    m_batchBackoff->setValue(420);
    m_batchBackoff->setPrefix(QStringLiteral("风控退避 "));
    m_batchBackoff->setSuffix(QStringLiteral("s"));
    m_batchBackoff->setToolTip(QStringLiteral("遇 412/-352 风控时的退避秒数（bili-batch 默认 420s）"));

    m_batchRetries = new QSpinBox(batchPage);
    m_batchRetries->setRange(1, 5);
    m_batchRetries->setValue(3);
    m_batchRetries->setPrefix(QStringLiteral("重试 "));
    m_batchRetries->setSuffix(QStringLiteral("次"));

    m_batchStartBtn = new QPushButton(QStringLiteral("▶ 开始批量"), batchPage);
    m_batchCancelBtn = new QPushButton(QStringLiteral("■ 停止"), batchPage);
    m_batchCancelBtn->setEnabled(false);

    batchCtrl->addWidget(m_batchSleep);
    batchCtrl->addWidget(m_batchBackoff);
    batchCtrl->addWidget(m_batchRetries);
    batchCtrl->addStretch(1);
    batchCtrl->addWidget(m_batchStartBtn);
    batchCtrl->addWidget(m_batchCancelBtn);
    batchLayout->addLayout(batchCtrl);

    m_batchProgress = new QProgressBar(batchPage);
    m_batchProgress->setRange(0, 1);
    m_batchProgress->setValue(0);
    m_batchProgress->setFormat(QStringLiteral("等待开始…"));
    batchLayout->addWidget(m_batchProgress);

    m_batchResult = new QTreeWidget(batchPage);
    m_batchResult->setColumnCount(5);
    m_batchResult->setHeaderLabels({
        QStringLiteral("#"), QStringLiteral("目标"),
        QStringLiteral("状态"), QStringLiteral("标题"), QStringLiteral("评论")});
    m_batchResult->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_batchResult->header()->resizeSection(0, 40);
    m_batchResult->header()->resizeSection(2, 70);
    m_batchResult->header()->resizeSection(4, 60);
    batchLayout->addWidget(m_batchResult, 1);

    m_tabs->addTab(m_infoPane, QStringLiteral("📺 内容"));
    m_tabs->addTab(commentPage, QStringLiteral("💬 评论"));
    m_tabs->addTab(m_aiPane, QStringLiteral("🤖 AI 总结"));
    m_tabs->addTab(m_subPane, QStringLiteral("📝 字幕"));
    m_tabs->addTab(upPage, QStringLiteral("📂 UP主内容"));
    m_tabs->addTab(batchPage, QStringLiteral("📦 批量队列"));
    root->addWidget(m_tabs, 1);

    // ---- 状态栏 ----
    m_status = new QLabel(QStringLiteral("就绪 —— 粘贴任意 B站 输入开始"), this);
    statusBar()->addWidget(m_status, 1);
    m_loginLabel = new QLabel(QStringLiteral("👤 未登录"), this);
    m_loginLabel->setStyleSheet(QStringLiteral("color:#888; padding-right:8px"));
    m_loginLabel->setToolTip(QStringLiteral("菜单「账号」可扫码登录（登录态与 DSH 插件共享）"));
    statusBar()->addPermanentWidget(m_loginLabel);
    m_outDirLabel = new QLabel(this);
    m_outDirLabel->setStyleSheet(QStringLiteral("color:#555; padding-right:8px"));
    statusBar()->addPermanentWidget(m_outDirLabel);

    setCentralWidget(central);

    // 启动即聚焦输入框，回车即可抓取
    m_input->setFocus();

    // 信号
    connect(m_fetchBtn, &QPushButton::clicked, this, &MainWindow::onFetch);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_openMdBtn, &QPushButton::clicked, this, &MainWindow::onOpenMarkdown);
    connect(m_sendBatchBtn, &QPushButton::clicked, this, &MainWindow::onSendToBatch);
    connect(m_exportCsvBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);
    connect(m_exportMdBtn, &QPushButton::clicked, this, &MainWindow::onExportMd);
    connect(m_batchStartBtn, &QPushButton::clicked, this, &MainWindow::onBatchStart);
    connect(m_batchCancelBtn, &QPushButton::clicked, this, &MainWindow::onBatchCancel);
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::onFetch);
}

// ---------------------------------------------------------------------------
// busy 状态
// ---------------------------------------------------------------------------

void MainWindow::setBusy(bool busy)
{
    m_fetchBtn->setEnabled(!busy);
    m_cancelBtn->setEnabled(busy);
    m_input->setEnabled(!busy);
    m_batchStartBtn->setEnabled(!busy);
    if (busy)
        m_openMdBtn->setEnabled(false);
}

void MainWindow::setBatchBusy(bool busy)
{
    m_batchStartBtn->setEnabled(!busy);
    m_batchCancelBtn->setEnabled(busy);
    m_batchInput->setEnabled(!busy);
    m_fetchBtn->setEnabled(!busy);
    m_cancelBtn->setEnabled(!busy);
    m_input->setEnabled(!busy);
    if (busy)
        m_openMdBtn->setEnabled(false);
}

// ---------------------------------------------------------------------------
// 单目标抓取（auto 模糊路由）
// ---------------------------------------------------------------------------

void MainWindow::openTarget(const QString &target)
{
    if (target.isEmpty())
        return;
    m_input->setText(target);
    QTimer::singleShot(300, this, &MainWindow::onFetch);
}

void MainWindow::onFetch()
{
    const QString target = m_input->text().trimmed();
    if (target.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先输入 BV 号、链接、专栏/动态/UP主 或关键词。"));
        return;
    }

    QJsonObject params{
        {"target", target},
        {"comment_mode", m_mode->currentData().toString()},
        {"max_comments", m_maxComments->value()},
        {"count", 6},
        {"include_replies", true},
        {"include_subtitles", true},
        {"include_ai", true},
        {"include_comments", true},
        {"save_markdown", true},
    };
    applyOutputDirToParams(params);

    m_pendingKind = QStringLiteral("auto");
    m_requestId = m_bridge->call(QStringLiteral("auto"), params);
    if (m_requestId < 0) {
        m_status->setText(QStringLiteral("启动 bridge 失败，请检查 Python 环境"));
        return;
    }

    m_markdownFile.clear();
    m_lastComments = QJsonArray{};
    m_lastListItems = QJsonArray{};
    m_lastListKind.clear();
    m_openMdBtn->setEnabled(false);
    m_sendBatchBtn->setEnabled(false);
    m_exportCsvBtn->setEnabled(false);
    m_exportMdBtn->setEnabled(false);
    m_commentTree->clear();
    m_infoPane->clear();
    m_aiPane->clear();
    m_subPane->clear();
    setBusy(true);
    m_status->setText(QStringLiteral("已发送请求，等待 bridge …"));
}

void MainWindow::onCancel()
{
    m_bridge->cancel();
    setBusy(false);
    setBatchBusy(false);
    m_status->setText(QStringLiteral("已取消"));
}

void MainWindow::onOpenMarkdown()
{
    if (m_markdownFile.isEmpty() || !QFileInfo::exists(m_markdownFile))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_markdownFile));
}

/** keyword 清单 → 加入批量队列输入框 */
void MainWindow::onSendToBatch()
{
    if (m_lastListItems.isEmpty())
        return;

    QStringList lines;
    for (const QJsonValue &v : m_lastListItems) {
        const auto o = v.toObject();
        QString id = o.value(QStringLiteral("bvid")).toString();
        if (id.isEmpty())
            id = o.value(QStringLiteral("dynamic_id")).toString();
        if (!id.isEmpty())
            lines << id;
    }
    if (lines.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("清单中没有可加入批量的目标。"));
        return;
    }

    QStringList existing = m_batchInput->toPlainText().split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &l : lines)
        if (!existing.contains(l))
            existing << l;
    m_batchInput->setPlainText(existing.join(QLatin1Char('\n')));

    m_tabs->setCurrentIndex(5);   // 批量队列页
    m_status->setText(QStringLiteral("已加入 %1 个目标到批量队列").arg(lines.size()));
}

// ---------------------------------------------------------------------------
// UP 主清单：全选 / 取消 / 抓取选中
// ---------------------------------------------------------------------------

static void setAllChecked(QTreeWidget *tree, bool checked)
{
    tree->setUpdatesEnabled(false);
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        tree->topLevelItem(i)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    tree->setUpdatesEnabled(true);
    tree->viewport()->update();
}

void MainWindow::onUpSelectAll()
{
    if (auto *t = qobject_cast<QTreeWidget *>(m_upTabs->currentWidget()))
        setAllChecked(t, true);
}

void MainWindow::onUpSelectNone()
{
    if (auto *t = qobject_cast<QTreeWidget *>(m_upTabs->currentWidget()))
        setAllChecked(t, false);
}

static void collectChecked(QTreeWidget *tree, QStringList &out)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto *item = tree->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked) {
            const QString id = item->data(0, Qt::UserRole).toString();
            if (!id.isEmpty() && !out.contains(id))
                out << id;
        }
    }
}

void MainWindow::onUpSendToBatch()
{
    QStringList lines;
    collectChecked(m_upVideoTree, lines);
    collectChecked(m_upArticleTree, lines);
    if (lines.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先勾选要抓取的视频或专栏（点行首复选框）。"));
        return;
    }

    QStringList existing = m_batchInput->toPlainText().split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &l : lines)
        if (!existing.contains(l))
            existing << l;
    m_batchInput->setPlainText(existing.join(QLatin1Char('\n')));

    m_tabs->setCurrentIndex(5);   // 批量队列页
    m_status->setText(QStringLiteral("已把 %1 个选中项加入批量队列，点「▶ 开始批量」执行")
                          .arg(lines.size()));
    QMessageBox::information(this, QStringLiteral("已加入批量队列"),
                             QStringLiteral("共 %1 项已加入批量队列。\n"
                                            "可调整「间隔 / 风控退避 / 重试」参数后点「▶ 开始批量」。")
                                 .arg(lines.size()));
}

// ---------------------------------------------------------------------------
// 批量队列
// ---------------------------------------------------------------------------

void MainWindow::onBatchStart()
{
    QStringList targets = m_batchInput->toPlainText().split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    targets.removeAll(QString());
    if (targets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先在批量输入框中每行填一个目标。"));
        return;
    }

    QJsonObject params{
        {"targets", QJsonArray::fromStringList(targets)},
        {"comment_mode", m_mode->currentData().toString()},
        {"max_comments", m_maxComments->value()},
        {"count", 6},
        {"include_replies", true},
        {"include_subtitles", false},   // 批量默认跳过字幕/AI，降速防风控
        {"include_ai", false},
        {"include_comments", true},
        {"sleep", m_batchSleep->value()},
        {"backoff", m_batchBackoff->value()},
        {"retries", m_batchRetries->value()},
    };
    applyOutputDirToParams(params);

    m_pendingKind = QStringLiteral("batch");
    m_requestId = m_bridge->call(QStringLiteral("batch"), params);
    if (m_requestId < 0) {
        m_status->setText(QStringLiteral("启动 bridge 失败，请检查 Python 环境"));
        return;
    }

    m_batchTotal = targets.size();
    m_batchResult->clear();
    m_batchProgress->setRange(0, m_batchTotal);
    m_batchProgress->setValue(0);
    m_batchProgress->setFormat(QStringLiteral("0 / %1 …").arg(m_batchTotal));
    setBatchBusy(true);
    m_status->setText(QStringLiteral("批量队列启动：%1 个目标").arg(m_batchTotal));
}

void MainWindow::onBatchCancel()
{
    m_bridge->cancel();
    setBatchBusy(false);
    m_batchProgress->setFormat(QStringLiteral("已停止"));
    m_status->setText(QStringLiteral("批量已停止（已完成项的产物已落盘）"));
}

// ---------------------------------------------------------------------------
// 进度 / 退出
// ---------------------------------------------------------------------------

void MainWindow::onProgress(qint64 id, const QString &stage, const QString &detail)
{
    if (id != m_requestId)
        return;

    m_status->setText(QStringLiteral("[%1] %2").arg(stage, detail));

    // 批量进度条：解析 "[i/n]" 前缀
    if (m_pendingKind == QLatin1String("batch")) {
        static const QRegularExpression re(QStringLiteral("^\\[(\\d+)/(\\d+)\\]"));
        const auto m = re.match(detail);
        if (m.hasMatch()) {
            const int cur = m.captured(1).toInt();
            m_batchProgress->setValue(cur);
            m_batchProgress->setFormat(QStringLiteral("%1 / %2 — %3")
                                           .arg(cur).arg(m_batchTotal)
                                           .arg(detail.mid(m.capturedLength() + 1).left(48)));
        }
        if (stage == QLatin1String("batch_done"))
            m_batchProgress->setFormat(detail);
    }
}

void MainWindow::onBridgeExited(int exitCode)
{
    if (m_requestId >= 0)
        m_status->setText(QStringLiteral("bridge 退出 (code=%1)").arg(exitCode));
}

// ---------------------------------------------------------------------------
// 结果分发
// ---------------------------------------------------------------------------

void MainWindow::onReply(qint64 id, bool ok, const QJsonObject &data, const QString &error)
{
    if (id != m_requestId)
        return;

    const QString kind = m_pendingKind;
    m_requestId = -1;
    m_pendingKind.clear();

    // ---- 账号状态查询 / 退出登录 ----
    if (kind == QLatin1String("check")) {
        if (ok)
            updateLoginLabel(data);
        else
            m_loginLabel->setText(QStringLiteral("👤 未登录（状态查询失败）"));
        return;
    }
    if (kind == QLatin1String("logout")) {
        if (ok) {
            m_loginLabel->setText(QStringLiteral("👤 未登录"));
            m_status->setText(QStringLiteral("已退出登录（共享凭据中的 Cookie 已清除）"));
            // 复核一次真实状态
            m_pendingKind = QStringLiteral("check");
            m_requestId = m_bridge->call(QStringLiteral("login_check"), QJsonObject{});
            if (m_requestId < 0)
                m_pendingKind.clear();
        } else {
            m_status->setText(QStringLiteral("退出登录失败: %1").arg(error));
        }
        return;
    }

    if (!ok) {
        setBusy(false);
        setBatchBusy(false);
        m_status->setText(QStringLiteral("失败: %1").arg(error));
        if (kind == QLatin1String("batch")) {
            m_batchProgress->setFormat(QStringLiteral("失败: %1").arg(error.left(60)));
        } else {
            m_tabs->setCurrentIndex(0);
            m_infoPane->setHtml(
                QStringLiteral("<h3 style='color:#c0392b'>抓取失败</h3><p>%1</p>")
                    .arg(error.toHtmlEscaped()));
        }
        return;
    }

    if (kind == QLatin1String("batch")) {
        setBatchBusy(false);
        renderBatchResult(data);
        return;
    }

    setBusy(false);
    renderAutoResult(data);
}

void MainWindow::renderAutoResult(const QJsonObject &data)
{
    const QString kind = data.value(QStringLiteral("kind")).toString();
    m_markdownFile = data.value(QStringLiteral("markdown_file")).toString();
    m_openMdBtn->setEnabled(!m_markdownFile.isEmpty());

    if (kind == QLatin1String("video")) {
        renderVideo(data);
    } else if (kind == QLatin1String("article")) {
        renderArticle(data);
    } else if (kind == QLatin1String("dynamic")) {
        renderDynamic(data);
    } else if (kind == QLatin1String("up_content")) {
        renderUpContent(data);
    } else if (kind == QLatin1String("keyword")) {
        renderKeyword(data);
    } else {
        m_tabs->setCurrentIndex(0);
        m_infoPane->setHtml(QStringLiteral("<p>未知结果类型: %1</p>").arg(kind));
        return;
    }

    // 评论（video/article/dynamic 有）
    if (data.contains(QStringLiteral("comments"))) {
        const auto comments = data.value(QStringLiteral("comments")).toObject();
        m_lastCommentsObj = comments;
        m_lastComments = comments.value(QStringLiteral("items")).toArray();
        renderComments(comments);
        m_exportCsvBtn->setEnabled(!m_lastComments.isEmpty());
        m_exportMdBtn->setEnabled(!m_lastComments.isEmpty());
        m_status->setText(QStringLiteral("✅ 评论 %1 条（%2） | %3")
                              .arg(QString::number(m_lastComments.size()),
                                   comments.value(QStringLiteral("mode")).toString(),
                                   m_markdownFile.isEmpty()
                                       ? QStringLiteral("未保存")
                                       : QFileInfo(m_markdownFile).fileName()));
        m_tabs->setCurrentIndex(1);   // 评论页
    } else {
        m_commentTree->clear();
        m_exportCsvBtn->setEnabled(false);
        m_exportMdBtn->setEnabled(false);
        // 清单类结果（UP主/关键词）：renderX 已在状态栏写了统计，这里补归档信息
        const QString base = m_status->text();
        const QString archived = m_markdownFile.isEmpty()
            ? QStringLiteral("无归档文件")
            : QStringLiteral("📄 %1").arg(QFileInfo(m_markdownFile).fileName());
        if (base.startsWith(QStringLiteral("✅")))
            m_status->setText(base + QStringLiteral(" | ") + archived);
        else
            m_status->setText(QStringLiteral("✅ 完成 | %1").arg(archived));
    }
}

// ---------------------------------------------------------------------------
// 各类型渲染
// ---------------------------------------------------------------------------

void MainWindow::renderVideo(const QJsonObject &data)
{
    const auto v = data.value(QStringLiteral("video")).toObject();
    const auto stat = v.value(QStringLiteral("stat")).toObject();
    m_lastTitle = v.value(QStringLiteral("title")).toString();
    m_lastListItems = QJsonArray{};

    const QString html = QStringLiteral(
        "<div style='font-size:16px'><b>%1</b></div>"
        "<p style='color:#666'>%2 · UP主：<a href='%3'>%4</a> · 发布于 %5</p>"
        "<p><a href='%6'>%6</a></p>"
        "<table cellspacing='6'>"
        "<tr><td>▶ 播放 %7</td><td>💬 弹幕 %8</td><td>💬 评论 %9</td>"
        "<td>👍 点赞 %10</td><td>🪙 硬币 %11</td><td>⭐ 收藏 %12</td>"
        "<td>🔁 转发 %13</td></tr></table>"
        "<hr><p style='color:#444'>%14</p>")
        .arg(m_lastTitle.toHtmlEscaped(),
             v.value(QStringLiteral("pubdate_text")).toString(),
             v.value(QStringLiteral("url")).toString(),
             v.value(QStringLiteral("up_name")).toString().toHtmlEscaped(),
             formatTime(v.value(QStringLiteral("pubdate")).toVariant().toLongLong()),
             v.value(QStringLiteral("url")).toString(),
             formatCount(stat.value(QStringLiteral("view")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("danmaku")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("reply")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("like")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("coin")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("favorite")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("share")).toVariant().toLongLong()),
             v.value(QStringLiteral("desc")).toString().toHtmlEscaped().left(800));
    m_infoPane->setHtml(html);
    m_tabs->setCurrentIndex(0);

    // AI / 字幕
    const auto ai = data.value(QStringLiteral("ai")).toObject();
    if (!ai.value(QStringLiteral("has")).toBool()) {
        m_aiPane->setHtml(QStringLiteral(
            "<p style='color:#888'>该视频没有官方 AI 总结（可能未开启或需登录态）。</p>"));
    } else {
        QString h = QStringLiteral("<h2>官方 AI 总结</h2>");
        for (const QJsonValue &sv : ai.value(QStringLiteral("segments")).toArray()) {
            const auto seg = sv.toObject();
            const qint64 from = seg.value(QStringLiteral("timestamp_from")).toVariant().toLongLong();
            const qint64 to = seg.value(QStringLiteral("timestamp_to")).toVariant().toLongLong();
            h += QStringLiteral("<h3><span style='color:#0a84ff'>[%1 ~ %2]</span> %3</h3><p>%4</p>")
                     .arg(from > 0 ? QString::number(from) : QStringLiteral("-"),
                          to > 0 ? QString::number(to) : QStringLiteral("-"),
                          seg.value(QStringLiteral("title")).toString().toHtmlEscaped(),
                          seg.value(QStringLiteral("summary")).toString().toHtmlEscaped());
        }
        m_aiPane->setHtml(h);
    }

    const auto sub = data.value(QStringLiteral("subtitles")).toObject();
    if (!sub.value(QStringLiteral("has")).toBool()) {
        m_subPane->setHtml(QStringLiteral("<p style='color:#888'>该视频无可抓取字幕。</p>"));
    } else {
        QString h = QStringLiteral("<h2>字幕（%1%2）</h2><table>")
                        .arg(sub.value(QStringLiteral("lan_doc")).toString(),
                             sub.value(QStringLiteral("is_ai")).toBool()
                                 ? QStringLiteral(" · AI 字幕") : QString());
        for (const QJsonValue &lv : sub.value(QStringLiteral("lines")).toArray()) {
            const auto line = lv.toObject();
            const double from = line.value(QStringLiteral("from")).toDouble();
            h += QStringLiteral("<tr><td style='color:#888;white-space:nowrap'>%1:%2</td><td>%3</td></tr>")
                     .arg(int(from) / 60, 2, 10, QChar('0'))
                     .arg(int(from) % 60, 2, 10, QChar('0'))
                     .arg(line.value(QStringLiteral("text")).toString().toHtmlEscaped());
        }
        m_subPane->setHtml(h + QStringLiteral("</table>"));
    }
}

void MainWindow::renderArticle(const QJsonObject &data)
{
    const auto a = data.value(QStringLiteral("article")).toObject();
    m_lastTitle = a.value(QStringLiteral("title")).toString();
    m_lastListItems = QJsonArray{};
    const auto stat = a.value(QStringLiteral("stat")).toObject();

    const QString html = QStringLiteral(
        "<div style='font-size:16px'><b>📖 %1</b></div>"
        "<p style='color:#666'>作者：%2 · 发布于 %3 · 👍 %4 · 💬 %5 · ↗ %6</p>"
        "<p><a href='%7'>%7</a></p><hr>%8")
        .arg(m_lastTitle.toHtmlEscaped(),
             a.value(QStringLiteral("author")).toString().toHtmlEscaped(),
             a.value(QStringLiteral("pubdate_text")).toString(),
             formatCount(stat.value(QStringLiteral("like")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("comment")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("forward")).toVariant().toLongLong()),
             a.value(QStringLiteral("url")).toString(),
             mdToHtml(a.value(QStringLiteral("body_md")).toString(), 4000));
    m_infoPane->setHtml(html);
    m_tabs->setCurrentIndex(0);

    m_aiPane->setHtml(QStringLiteral("<p style='color:#888'>AI 总结仅视频可用。</p>"));
    m_subPane->setHtml(QStringLiteral("<p style='color:#888'>字幕仅视频可用。</p>"));
}

void MainWindow::renderDynamic(const QJsonObject &data)
{
    const auto d = data.value(QStringLiteral("dynamic")).toObject();
    m_lastTitle = d.value(QStringLiteral("title")).toString();
    m_lastListItems = QJsonArray{};
    const auto stat = d.value(QStringLiteral("stat")).toObject();

    QString imgs;
    const auto images = d.value(QStringLiteral("images")).toArray();
    if (!images.isEmpty()) {
        imgs = QStringLiteral("<p>🖼 配图 %1 张：</p>").arg(images.size());
        for (const QJsonValue &iv : images) {
            const QString u = iv.toString();
            if (!u.isEmpty())
                imgs += QStringLiteral("<p><a href='%1'>%1</a></p>").arg(u.toHtmlEscaped());
        }
    }

    const QString html = QStringLiteral(
        "<div style='font-size:16px'><b>💭 %1</b></div>"
        "<p style='color:#666'>%2 · %3 · 👍 %4 · 💬 %5 · ↗ %6</p>"
        "<p><a href='%7'>%7</a></p><hr>"
        "<p style='color:#444'>%8</p>%9")
        .arg(m_lastTitle.toHtmlEscaped(),
             d.value(QStringLiteral("author")).toString().toHtmlEscaped(),
             d.value(QStringLiteral("pubdate_text")).toString(),
             formatCount(stat.value(QStringLiteral("like")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("comment")).toVariant().toLongLong()),
             formatCount(stat.value(QStringLiteral("forward")).toVariant().toLongLong()),
             d.value(QStringLiteral("url")).toString(),
             d.value(QStringLiteral("text")).toString().toHtmlEscaped().left(3000),
             imgs);
    m_infoPane->setHtml(html);
    m_tabs->setCurrentIndex(0);

    m_aiPane->setHtml(QStringLiteral("<p style='color:#888'>AI 总结仅视频可用。</p>"));
    m_subPane->setHtml(QStringLiteral("<p style='color:#888'>字幕仅视频可用。</p>"));
}

void MainWindow::renderUpContent(const QJsonObject &data)
{
    const auto user = data.value(QStringLiteral("user")).toObject();
    const auto videos = data.value(QStringLiteral("videos")).toArray();
    const auto articles = data.value(QStringLiteral("articles")).toArray();
    const int vTotal = data.value(QStringLiteral("video_total")).toInt();
    const int aTotal = data.value(QStringLiteral("article_total")).toInt();

    m_lastTitle = QStringLiteral("UP主：%1").arg(user.value(QStringLiteral("name")).toString());
    m_lastListItems = QJsonArray{};

    fillListTree(m_upVideoTree, videos, QStringLiteral("video"));
    fillListTree(m_upArticleTree, articles, QStringLiteral("article"));

    m_upTabs->setTabText(0, QStringLiteral("🎬 规频（%1/%2）")
                                .arg(videos.size()).arg(vTotal));
    m_upTabs->setTabText(1, QStringLiteral("📖 专栏（%1/%2）")
                                .arg(articles.size()).arg(aTotal));
    m_upSummary->setText(
        QStringLiteral("👤 %1 · 按发布时间倒序 · 勾选后点「抓取选中」")
            .arg(user.value(QStringLiteral("name")).toString()));

    m_aiPane->setHtml(QStringLiteral("<p style='color:#888'>AI 总结仅视频详情可用。</p>"));
    m_subPane->setHtml(QStringLiteral("<p style='color:#888'>字幕仅视频详情可用。</p>"));

    m_status->setText(QStringLiteral("✅ %1：视频 %2/%3 个、专栏 %4/%5 篇（📂 UP主内容页可勾选）")
                          .arg(user.value(QStringLiteral("name")).toString(),
                               QString::number(videos.size()), QString::number(vTotal),
                               QString::number(articles.size()), QString::number(aTotal)));
    m_tabs->setCurrentIndex(4);   // UP主内容页
}

void MainWindow::renderKeyword(const QJsonObject &data)
{
    const auto videos = data.value(QStringLiteral("videos")).toArray();
    m_lastTitle = QStringLiteral("关键词：%1").arg(data.value(QStringLiteral("keyword")).toString());

    QString rows;
    for (const QJsonValue &vv : videos) {
        const auto v = vv.toObject();
        rows += QStringLiteral(
            "<tr><td style='white-space:nowrap'>%1</td>"
            "<td><a href='%2'>%3</a><br><span style='color:#888'>%4 · %5</span></td>"
            "<td style='white-space:nowrap'>▶ %6</td>"
            "<td><code>%7</code></td></tr>")
            .arg(formatTime(v.value(QStringLiteral("pubdate")).toVariant().toLongLong()),
                 v.value(QStringLiteral("url")).toString(),
                 v.value(QStringLiteral("title")).toString().toHtmlEscaped(),
                 v.value(QStringLiteral("author")).toString().toHtmlEscaped(),
                 v.value(QStringLiteral("bvid")).toString(),
                 formatCount(v.value(QStringLiteral("play")).toVariant().toLongLong()),
                 v.value(QStringLiteral("bvid")).toString());
    }

    const QString html = QStringLiteral(
        "<div style='font-size:16px'><b>🔍 %1</b></div>"
        "<p style='color:#666'>找到 %2 个视频（点击标题打开；"
        "「→ 批量」可把 BV 全部加入队列逐个抓评论）</p>"
        "<table width='100%' cellspacing='6'>"
        "<tr><th>发布</th><th>视频</th><th>播放</th><th>BV号</th></tr>%3</table>")
        .arg(data.value(QStringLiteral("keyword")).toString().toHtmlEscaped(),
             QString::number(videos.size()),
             rows);
    m_infoPane->setHtml(html);
    m_tabs->setCurrentIndex(0);

    m_aiPane->setHtml(QStringLiteral("<p style='color:#888'>AI 总结仅视频详情可用。</p>"));
    m_subPane->setHtml(QStringLiteral("<p style='color:#888'>字幕仅视频详情可用。</p>"));

    m_lastListItems = videos;
    m_lastListKind = QStringLiteral("keyword");
    if (m_sendBatchBtn)
        m_sendBatchBtn->setEnabled(!videos.isEmpty());
    m_status->setText(QStringLiteral("✅ 关键词「%1」：找到 %2 个视频结果")
                          .arg(data.value(QStringLiteral("keyword")).toString(),
                               QString::number(videos.size())));
}

// ---------------------------------------------------------------------------
// 评论树
// ---------------------------------------------------------------------------

void MainWindow::renderComments(const QJsonObject &comments, const QString &scope)
{
    m_commentTree->clear();

    const QString modeDesc = comments.value(QStringLiteral("mode_desc")).toString();
    const auto items = comments.value(QStringLiteral("items")).toArray();
    if (items.isEmpty())
        return;

    QTreeWidgetItem *parent = nullptr;   // nullptr = 根级（挂在 m_commentTree 上）
    QTreeWidgetItem *groupHeader = nullptr;
    if (!scope.isEmpty()) {
        groupHeader = new QTreeWidgetItem(m_commentTree,
                                          {scope, QString(), QString(), QString()});
        parent = groupHeader;
    } else if (!modeDesc.isEmpty()) {
        groupHeader = new QTreeWidgetItem(m_commentTree,
                                          {modeDesc, QString(), QString(), QString()});
        parent = groupHeader;
    }
    if (groupHeader) {
        groupHeader->setFirstColumnSpanned(true);
        QFont f = groupHeader->font(0);
        f.setBold(true);
        groupHeader->setFont(0, f);
    }

    for (const QJsonValue &cv : items) {
        const auto c = cv.toObject();
        auto *item = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(m_commentTree);
        item->setText(0, c.value(QStringLiteral("message")).toString());
        item->setText(1, QStringLiteral("%1  Lv%2")
                             .arg(c.value(QStringLiteral("uname")).toString(),
                                  QString::number(c.value(QStringLiteral("level")).toInt())));
        item->setText(2, formatCount(c.value(QStringLiteral("like")).toVariant().toLongLong()));
        item->setText(3, formatTime(c.value(QStringLiteral("ctime")).toVariant().toLongLong()));
        item->setToolTip(0, c.value(QStringLiteral("message")).toString());

        for (const QJsonValue &sv : c.value(QStringLiteral("sub_replies")).toArray()) {
            const auto s = sv.toObject();
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, QStringLiteral("└ %1")
                                  .arg(s.value(QStringLiteral("message")).toString()));
            child->setText(1, s.value(QStringLiteral("uname")).toString());
            child->setText(2, formatCount(s.value(QStringLiteral("like")).toVariant().toLongLong()));
            child->setText(3, formatTime(s.value(QStringLiteral("ctime")).toVariant().toLongLong()));
            child->setForeground(0, QColor(QStringLiteral("#555")));
        }
    }
    if (groupHeader)
        m_commentTree->expandItem(groupHeader);
}

// ---------------------------------------------------------------------------
// 批量结果
// ---------------------------------------------------------------------------

void MainWindow::renderBatchResult(const QJsonObject &data)
{
    const int total = data.value(QStringLiteral("total")).toInt();
    const int ok = data.value(QStringLiteral("ok")).toInt();
    const int failed = data.value(QStringLiteral("failed")).toInt();

    m_batchProgress->setRange(0, qMax(total, 1));
    m_batchProgress->setValue(total);
    m_batchProgress->setFormat(QStringLiteral("完成：成功 %1 / 失败 %2").arg(ok).arg(failed));

    m_batchResult->clear();
    const auto items = data.value(QStringLiteral("items")).toArray();
    int i = 0;
    for (const QJsonValue &iv : items) {
        const auto it = iv.toObject();
        ++i;
        auto *row = new QTreeWidgetItem(m_batchResult);
        row->setText(0, QString::number(i));
        row->setText(1, it.value(QStringLiteral("target")).toString());
        const bool good = it.value(QStringLiteral("ok")).toBool();
        row->setText(2, good ? QStringLiteral("✅ 成功") : QStringLiteral("❌ 失败"));
        row->setForeground(2, good ? QColor(QStringLiteral("#2e7d32"))
                                   : QColor(QStringLiteral("#c62828")));
        row->setText(3, good ? it.value(QStringLiteral("title")).toString()
                             : it.value(QStringLiteral("error")).toString().left(120));
        row->setText(4, good ? QString::number(it.value(QStringLiteral("comment_count")).toInt())
                             : QString());
        const QString md = it.value(QStringLiteral("markdown_file")).toString();
        if (!md.isEmpty())
            row->setToolTip(3, md);
        if (good && !md.isEmpty())
            row->setData(3, Qt::UserRole, md);
    }

    // 双击打开对应 Markdown（一次性连接，避免重复累积）
    connect(m_batchResult, &QTreeWidget::itemDoubleClicked, this,
            [](QTreeWidgetItem *item, int) {
                const QString md = item->data(3, Qt::UserRole).toString();
                if (!md.isEmpty() && QFileInfo::exists(md))
                    QDesktopServices::openUrl(QUrl::fromLocalFile(md));
            }, Qt::SingleShotConnection);

    m_status->setText(QStringLiteral("✅ 批量完成：成功 %1 / 失败 %2（共 %3）")
                          .arg(ok).arg(failed).arg(total));
    if (failed > 0)
        QMessageBox::warning(this, QStringLiteral("批量完成"),
                             QStringLiteral("成功 %1 个，失败 %2 个。\n失败项可在批量框中保留后重试。")
                                 .arg(ok).arg(failed));
}

// ---------------------------------------------------------------------------
// CSV 导出
// ---------------------------------------------------------------------------

static QString csvEscape(const QString &s)
{
    QString v = s;
    v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    if (v.contains(QLatin1Char(',')) || v.contains(QLatin1Char('"'))
        || v.contains(QLatin1Char('\n')) || v.contains(QLatin1Char('\r')))
        v = QLatin1Char('"') + v + QLatin1Char('"');
    return v;
}

void MainWindow::onExportCsv()
{
    if (m_lastComments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("当前没有可导出的评论数据。"));
        return;
    }

    const QString suggested = QStringLiteral("%1_评论.csv")
                                  .arg(m_lastTitle.isEmpty()
                                           ? QStringLiteral("bilibili")
                                           : m_lastTitle.left(30));
    const QString startDir = m_outputDir.isEmpty()
        ? QStringLiteral("D:/软件/AI工具/输出/B站") : m_outputDir;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出评论 CSV"),
        startDir + QLatin1Char('/') + suggested,
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入文件: %1").arg(f.errorString()));
        return;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    // UTF-8 BOM，保证 Excel 正确识别中文
    ts << QChar(0xFEFF);
    ts << QStringLiteral("楼层ID,父楼层ID,用户,UID,等级,点赞,时间,内容\n");

    int written = 0;
    for (const QJsonValue &cv : m_lastComments) {
        const auto c = cv.toObject();
        const QString rpid = c.value(QStringLiteral("rpid")).toVariant().toString();
        const QString time = formatTime(c.value(QStringLiteral("ctime")).toVariant().toLongLong());
        ts << csvEscape(rpid) << ','
           << ','                                    // 父楼层（主评论为空）
           << csvEscape(c.value(QStringLiteral("uname")).toString()) << ','
           << csvEscape(c.value(QStringLiteral("mid")).toVariant().toString()) << ','
           << c.value(QStringLiteral("level")).toInt() << ','
           << c.value(QStringLiteral("like")).toVariant().toLongLong() << ','
           << csvEscape(time) << ','
           << csvEscape(c.value(QStringLiteral("message")).toString()) << '\n';
        ++written;

        for (const QJsonValue &sv : c.value(QStringLiteral("sub_replies")).toArray()) {
            const auto s = sv.toObject();
            const QString stime = formatTime(s.value(QStringLiteral("ctime")).toVariant().toLongLong());
            ts << csvEscape(s.value(QStringLiteral("rpid")).toVariant().toString()) << ','
               << csvEscape(rpid) << ','
               << csvEscape(s.value(QStringLiteral("uname")).toString()) << ','
               << csvEscape(s.value(QStringLiteral("mid")).toVariant().toString()) << ','
               << ','                                  // 楼中楼无等级字段
               << s.value(QStringLiteral("like")).toVariant().toLongLong() << ','
               << csvEscape(stime) << ','
               << csvEscape(s.value(QStringLiteral("message")).toString()) << '\n';
            ++written;
        }
    }
    f.close();

    m_status->setText(QStringLiteral("📥 已导出 %1 条评论 → %2").arg(written).arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// ---------------------------------------------------------------------------
// 评论导出 MD（独立评论 Markdown，含楼中楼）
// ---------------------------------------------------------------------------

void MainWindow::onExportMd()
{
    if (m_lastComments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("当前没有可导出的评论数据。"));
        return;
    }

    const QString suggested = QStringLiteral("%1_评论.md")
                                  .arg(m_lastTitle.isEmpty()
                                           ? QStringLiteral("bilibili")
                                           : m_lastTitle.left(30));
    const QString startDir = m_outputDir.isEmpty()
        ? QStringLiteral("D:/软件/AI工具/输出/B站") : m_outputDir;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出评论 Markdown"),
        startDir + QLatin1Char('/') + suggested,
        QStringLiteral("Markdown 文件 (*.md)"));
    if (path.isEmpty())
        return;

    // ---- 组装 Markdown ----
    const auto comments = m_lastCommentsObj;
    const QString modeDesc = comments.value(QStringLiteral("mode_desc")).toString();
    const int total = comments.value(QStringLiteral("total")).toInt();

    QString md;
    md += QStringLiteral("# %1 · 评论导出\n\n")
              .arg(m_lastTitle.isEmpty() ? QStringLiteral("B站评论") : m_lastTitle);
    md += QStringLiteral("> 导出时间：`%1`  \n")
              .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    if (!modeDesc.isEmpty())
        md += QStringLiteral("> 抓取模式：%1  \n").arg(modeDesc);
    if (total > 0)
        md += QStringLiteral("> 评论区总量：%1 条  \n").arg(formatCount(total));
    md += QStringLiteral("> 本文件：%1 条楼层（含楼中楼 %2 条）\n\n---\n\n")
              .arg(m_lastComments.size())
              .arg([&] {
                  int n = 0;
                  for (const QJsonValue &cv : std::as_const(m_lastComments))
                      n += cv.toObject().value(QStringLiteral("sub_replies")).toArray().size();
                  return n;
              }());

    int floor = 0;
    for (const QJsonValue &cv : std::as_const(m_lastComments)) {
        const auto c = cv.toObject();
        ++floor;
        const int lv = c.value(QStringLiteral("level")).toInt();
        md += QStringLiteral("#### #%1 [%2](https://space.bilibili.com/%3) %4 ｜ 👍 %5 ｜ 🕒 %6\n\n%7\n\n")
                  .arg(floor)
                  .arg(c.value(QStringLiteral("uname")).toString(),
                       c.value(QStringLiteral("mid")).toVariant().toString(),
                       lv > 0 ? QStringLiteral("`Lv.%1`").arg(lv) : QString(),
                       formatCount(c.value(QStringLiteral("like")).toVariant().toLongLong()),
                       formatTime(c.value(QStringLiteral("ctime")).toVariant().toLongLong()),
                       c.value(QStringLiteral("message")).toString());

        const auto subs = c.value(QStringLiteral("sub_replies")).toArray();
        if (!subs.isEmpty()) {
            md += QStringLiteral("> **楼中楼回复 (%1/%1)**：\n").arg(subs.size());
            for (const QJsonValue &sv : subs) {
                const auto s = sv.toObject();
                QString text = s.value(QStringLiteral("message")).toString();
                text.replace(QLatin1Char('\n'), QStringLiteral(" "));   // 引用块内保持单行
                const qint64 sl = s.value(QStringLiteral("like")).toVariant().toLongLong();
                md += QStringLiteral("> - **[%1](https://space.bilibili.com/%2)**%3 %4\n")
                          .arg(s.value(QStringLiteral("uname")).toString(),
                               s.value(QStringLiteral("mid")).toVariant().toString(),
                               sl > 0 ? QStringLiteral(" 👍 %1:").arg(formatCount(sl))
                                      : QStringLiteral("："),
                               text);
            }
            md += QLatin1Char('\n');
        }
        md += QStringLiteral("---\n\n");
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入文件: %1").arg(f.errorString()));
        return;
    }
    f.write(md.toUtf8());
    f.close();

    m_status->setText(QStringLiteral("📤 已导出 %1 条评论（含楼中楼）→ %2")
                          .arg(m_lastComments.size()).arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
