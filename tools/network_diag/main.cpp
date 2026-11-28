#include "NetworkMonitor.hpp"
#include "download/DownloadManager.hpp"
#include "download/DownloadRetryPolicy.hpp"
#include "util/AppPaths.hpp"
#include "util/DownloadDiagnostics.hpp"
#include <QApplication>
#include <QSettings>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdio>

using namespace NetDiag;
static std::shared_ptr<Log> qtLog;
static void output(QJsonObject value) {
    const auto bytes = QJsonDocument(safeValue(value).toObject()).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fflush(stdout);
}
static bool writeJson(const QString &path, const QJsonValue &value) {
    const QByteArray bytes = value.isArray() ? QJsonDocument(safeValue(value).toArray()).toJson()
        : QJsonDocument(safeValue(value).toObject()).toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
static bool validProxy(const QUrl &url) {
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && (url.path().isEmpty() || url.path() == QStringLiteral("/")) && url.port() > 0 && url.port() <= 65535
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("socks5"));
}
static int worker(QApplication &app, QJsonObject config) {
    const QUrl url(config.value("url").toString());
    const QString route = config.value("route").toString();
    const QString directory = config.value("directory").toString();
    const QString scratch = config.value("scratch").toString();
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || !QStringList{QStringLiteral("direct"), QStringLiteral("system"), QStringLiteral("explicit")}.contains(route)
        || (route == QStringLiteral("explicit") && !validProxy(QUrl(config.value("proxy").toString()))))
        return 2;
    if (!QDir::isAbsolutePath(directory) || !QDir::isAbsolutePath(scratch) || !QDir(directory).exists() || !QDir(scratch).exists()) return 2;
    qputenv("WTFML_DOWNLOAD_DEBUG", "1");
    AppPaths::setMinecraftDir(scratch);
    app.setProperty("wtfml.testDefaultMinecraftDir", scratch);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch + "/settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, scratch + "/system-settings");
    auto log = std::make_shared<Log>(QDir(directory).filePath(QStringLiteral("events.jsonl")));
    if (!log->file.isOpen()) return 2;
    log->budget = qBound<qint64>(1024, qint64(config.value("budget").toDouble(128 * 1024 * 1024)), 128LL * 1024 * 1024);
    qtLog = log;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &text) {
        if (qtLog) qtLog->event(QStringLiteral("qt_message"), {{"severity", int(type)}, {"text", text}});
    });
    DownloadDiag::setSink([log](const QString &text) { log->event(QStringLiteral("engine"), {{"text", text}}); });
    log->event(QStringLiteral("case_start"), {{"config", config}, {"qt", QString::fromLatin1(qVersion())},
        {"os", QSysInfo::prettyProductName()}, {"architecture", QSysInfo::currentCpuArchitecture()},
        {"tls_backend", QSslSocket::activeBackend()}, {"tls_library", QSslSocket::sslLibraryVersionString()},
        {"application_proxy", proxyInfo(QNetworkProxy::applicationProxy())},
        {"note", QStringLiteral("HTTP/2 is allowed, not guaranteed. DNS below is an independent post-transfer lookup, not the connection peer or request DNS timing.")}});
    for (const char *key : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY", "http_proxy", "https_proxy", "all_proxy", "no_proxy"})
        log->event(QStringLiteral("environment"), {{"name", QString::fromLatin1(key)}, {"present", qEnvironmentVariableIsSet(key)}});
    DownloadManager download(nullptr, [log, config](QObject *owner) { return new Access(log, config, owner); });
    download.setUseMirror(false); // Explicit URLs isolate routing; never silently substitute a different source.
    download.setSegmentedDownloadEnabled(config.value("segmented").toBool());
    download.setRangeCacheRoot(QDir(scratch).filePath(QStringLiteral("range-cache")));
    download.setMaxConcurrentDownloads(qBound(1, config.value("concurrency").toInt(4), 64));
    int completed = 0, failures = 0;
    bool ended = false, timedOut = false;
    const int count = qBound(1, config.value("requests").toInt(1), 16);
    QTimer sample;
    QObject::connect(&sample, &QTimer::timeout, &app, [log, &ended] {
        log->flush();
        if (!ended) output({{"event", "progress"}, {"elapsed_ms", double(log->clock.elapsed())}, {"bytes", double(log->transferred)}});
    }); sample.start(1000);
    const auto finish = [&](bool success) {
        if (ended) return;
        ended = true;
        QJsonObject protocols; for (auto it = log->protocols.begin(); it != log->protocols.end(); ++it) protocols.insert(it.key(), it.value());
        auto first = log->firstBytes; std::sort(first.begin(), first.end());
        qint64 saved = 0;
        for (int i = 0; i < count; ++i) saved += QFileInfo(QDir(scratch).filePath(QStringLiteral("payload-%1.bin").arg(i))).size();
        const QJsonObject result{{"event", "result"}, {"name", config.value("name")}, {"url", safeUrl(url)},
            {"route", route}, {"http2_allowed", config.value("h2")}, {"segmented", config.value("segmented")},
            {"success", success && failures == 0 && !timedOut && !log->budgetExceeded && completed == count},
            {"timed_out", timedOut}, {"throttled", log->throttled}, {"budget_exceeded", log->budgetExceeded}, {"elapsed_ms", double(log->clock.elapsed())},
            {"reported_bytes", double(log->transferred)}, {"saved_bytes", double(saved)}, {"completed_files", completed},
            {"failed_files", failures}, {"request_count", log->nextRequest}, {"protocols", protocols},
            {"first_byte_median_ms", first.isEmpty() ? -1.0 : double(first[first.size() / 2])},
            {"expected_sha1_supplied", !config.value("sha1").toString().isEmpty()}};
        log->event(QStringLiteral("case_finished"), result); log->flush();
        writeJson(QDir(directory).filePath(QStringLiteral("result.json")), result); output(result);
        // Do not pre-warm DNS before the measured transfer. These are candidates, not actual peer IPs.
        log->event(QStringLiteral("dns_lookup_start"), {{"host", url.host()}, {"independent_post_transfer", true}});
        const auto dnsStart = log->clock.elapsed();
        QHostInfo::lookupHost(url.host(), &app, [log, dnsStart](const QHostInfo &info) {
            QJsonArray addresses; for (const auto &address : info.addresses()) addresses.append(address.toString());
            log->event(QStringLiteral("dns_lookup_result"), {{"host", info.hostName()}, {"addresses", addresses},
                {"error", info.errorString()}, {"lookup_ms", double(log->clock.elapsed() - dnsStart)}, {"independent_post_transfer", true}}); log->flush();
        });
        QTimer::singleShot(1500, &app, &QCoreApplication::quit);
    };
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        if (!ended && log->throttled) {
            finish(false);
            download.cancelBatch();
        }
    });
    QObject::connect(&download, &DownloadManager::downloadFinished, &app, [&](const QString &file, bool success, const QString &error) {
        if (ended) return;
        ++completed; if (!success) ++failures;
        log->event(QStringLiteral("file_finished"), {{"file", QFileInfo(file).fileName()}, {"success", success}, {"error", error}});
    });
    QObject::connect(&download, &DownloadManager::allDownloadsFinished, &app, finish);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { timedOut = true; finish(false); download.cancelBatch(); });
    deadline.start(qBound(1, config.value("timeout_s").toInt(120), 180) * 1000);
    QTimer budget;
    QObject::connect(&budget, &QTimer::timeout, &app, [&] {
        if (!ended && log->budgetExceeded) { finish(false); download.cancelBatch(); }
    }); budget.start(100);
    download.beginBatch();
    const QString sha1 = config.value("sha1").toString();
    const qint64 size = qint64(config.value("size").toDouble(-1));
    for (int i = 0; i < count; ++i)
        download.downloadFile(url.toString(), QDir(scratch).filePath(QStringLiteral("payload-%1.bin").arg(i)), sha1, size, VerifyMode::AlwaysDownload);
    download.endBatch();
    app.exec();
    DownloadDiag::setSink({}); qInstallMessageHandler(nullptr); qtLog.reset();
    return 0;
}

class Window final : public QMainWindow {
    QPlainTextEdit *urls, *messages;
    QLineEdit *proxy, *sha1, *size, *outputRoot;
    QCheckBox *direct, *system, *explicitProxy, *h1, *h2, *segmented;
    QSpinBox *concurrency, *requests, *timeout;
    QPushButton *start, *stop, *open;
    QProgressBar *progress;
    QTableWidget *table;
    QList<QJsonObject> cases;
    QJsonArray results;
    QProcess *process = nullptr;
    std::unique_ptr<QTemporaryDir> scratch;
    QTimer groupDeadline, sessionDeadline;
    QByteArray pending;
    QString session;
    QString caseDirectory;
    QJsonObject result;
    qint64 usedBytes = 0;
    qint64 lastBytes = 0;
    bool cancelled = false, closeAfter = false;
    static constexpr qint64 totalBudget = 512LL * 1024 * 1024;
    void saveResults() {
        writeJson(QDir(session).filePath(QStringLiteral("results.json")), results);
        QString csv = QStringLiteral("case,url,route,http2_allowed,success,elapsed_ms,reported_bytes,first_byte_median_ms,timed_out\n");
        const auto quote = [](QString value) { value.replace('"', QStringLiteral("\"\"")); return '"' + value + '"'; };
        for (const auto &value : results) { const auto row = value.toObject(); QStringList cells;
            for (const QString &key : {QStringLiteral("name"), QStringLiteral("url"), QStringLiteral("route"), QStringLiteral("http2_allowed"),
                QStringLiteral("success"), QStringLiteral("elapsed_ms"), QStringLiteral("reported_bytes"), QStringLiteral("first_byte_median_ms"), QStringLiteral("timed_out")})
                cells.append(quote(row.value(key).toVariant().toString()));
            csv += cells.join(',') + '\n';
        }
        QSaveFile file(QDir(session).filePath(QStringLiteral("summary.csv")));
        if (file.open(QIODevice::WriteOnly)) { file.write(csv.toUtf8()); file.commit(); }
    }
    void readWorker() {
        if (!process) return;
        pending += process->readAllStandardOutput();
        while (pending.contains('\n')) {
            const auto line = pending.left(pending.indexOf('\n')); pending.remove(0, line.size() + 1);
            const auto object = QJsonDocument::fromJson(line).object();
            if (object.value("event") == QJsonValue(QStringLiteral("result"))) result = object;
            else if (object.value("event") == QJsonValue(QStringLiteral("progress"))) {
                lastBytes = qMax(lastBytes, qint64(object.value("bytes").toDouble()));
                statusBarMessage(QStringLiteral("本组 %1 秒 · 已接收 %2 MiB").arg(object.value("elapsed_ms").toDouble() / 1000, 0, 'f', 1)
                    .arg(object.value("bytes").toDouble() / 1048576, 0, 'f', 2));
            }
        }
        const auto error = process->readAllStandardError();
        if (!error.isEmpty()) {
            const QString text = redact(QString::fromUtf8(error));
            messages->appendPlainText(text.left(2000));
            QFile file(QDir(caseDirectory).filePath(QStringLiteral("process.txt")));
            if (file.size() < 1024 * 1024 && file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(text.toUtf8());
        }
    }
    void statusBarMessage(const QString &text) { progress->setFormat(text); }
    void finishSession() {
        groupDeadline.stop(); sessionDeadline.stop(); start->setEnabled(true); stop->setEnabled(false); open->setEnabled(true);
        statusBarMessage(cancelled ? QStringLiteral("已停止，已生成的日志保留") : QStringLiteral("诊断完成"));
        messages->appendPlainText(QStringLiteral("结果目录：") + session);
        saveResults(); if (closeAfter) close();
    }
    void next() {
        if (cancelled || results.size() >= cases.size() || usedBytes >= totalBudget) { finishSession(); return; }
        const int index = int(results.size());
        const QString host = QUrl(cases[index].value("url").toString()).host();
        for (const auto &previous : results) {
            const auto record = previous.toObject();
            if (record.value("throttled").toBool() && QUrl(record.value("url").toString()).host() == host) {
                auto skipped = cases[index];
                skipped.insert("success", false);
                skipped.insert("skipped", true);
                skipped.insert("throttled", true);
                skipped.insert("error", QStringLiteral("该来源已返回429，本轮剩余测试跳过，避免延续限流"));
                results.append(skipped);
                table->insertRow(index);
                table->setItem(index, 0, new QTableWidgetItem(skipped.value("name").toString()));
                table->setItem(index, 1, new QTableWidgetItem(QStringLiteral("限流后跳过")));
                messages->appendPlainText(skipped.value("error").toString());
                progress->setValue(int(results.size()));
                saveResults();
                QTimer::singleShot(0, this, [this] { next(); });
                return;
            }
        }
        const QString directory = QDir(session).filePath(QStringLiteral("case-%1").arg(index + 1, 2, 10, QChar('0')));
        caseDirectory = directory;
        if (!QDir().mkpath(directory)) { messages->appendPlainText(QStringLiteral("无法创建结果目录，已停止。")); cancelled = true; finishSession(); return; }
        scratch = std::make_unique<QTemporaryDir>(QDir(session).filePath(QStringLiteral("scratch-XXXXXX")));
        if (!scratch->isValid()) { cancelled = true; finishSession(); return; }
        auto config = cases[index]; config.insert("directory", directory); config.insert("scratch", scratch->path());
        config.insert("budget", double(qMin(totalBudget - usedBytes, 128LL * 1024 * 1024)));
        result = {}; pending.clear(); lastBytes = 0;
        messages->appendPlainText(QStringLiteral("开始 %1/%2：%3").arg(index + 1).arg(cases.size()).arg(config.value("name").toString()));
        process = new QProcess(this);
        connect(process, &QProcess::readyReadStandardOutput, this, [this] { readWorker(); });
        connect(process, &QProcess::readyReadStandardError, this, [this] { readWorker(); });
        connect(process, &QProcess::started, this, [this, config] {
            process->write(QJsonDocument(config).toJson(QJsonDocument::Compact)); process->closeWriteChannel();
        });
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) { result.insert("error", QStringLiteral("诊断子进程无法启动")); completedCase(-1); }
        });
        connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { completedCase(code); });
        groupDeadline.start((config.value("timeout_s").toInt() + 3) * 1000);
        process->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--worker")});
    }
    void completedCase(int code) {
        if (!process) return;
        readWorker(); groupDeadline.stop();
        const int index = int(results.size()); const auto config = cases[index];
        if (result.isEmpty() || !result.contains("event")) {
            result.insert("name", config.value("name")); result.insert("url", safeUrl(QUrl(config.value("url").toString())));
            result.insert("route", config.value("route")); result.insert("http2_allowed", config.value("h2"));
            result.insert("success", false); result.insert("incomplete", true);
            result.insert("reported_bytes", double(lastBytes));
            result.insert("error", cancelled ? QStringLiteral("用户停止或整轮超时") : QStringLiteral("子进程超时、崩溃或启动失败"));
        }
        result.insert("process_exit_code", code);
        results.append(result); usedBytes += qint64(result.value("reported_bytes").toDouble());
        table->insertRow(index);
        QStringList cells{config.value("name").toString(), result.value("success").toBool() ? QStringLiteral("成功") : QStringLiteral("失败/停止"),
            QString::number(result.value("elapsed_ms").toDouble(-1) / 1000, 'f', 2),
            QString::number(result.value("reported_bytes").toDouble() / 1048576, 'f', 2),
            QString::number(result.value("first_byte_median_ms").toDouble(-1), 'f', 0)};
        for (int column = 0; column < cells.size(); ++column) table->setItem(index, column, new QTableWidgetItem(cells[column]));
        process->deleteLater(); process = nullptr; scratch.reset();
        progress->setValue(int(results.size())); saveResults();
        QTimer::singleShot(0, this, [this] { next(); });
    }
    void begin() {
        const auto lines = urls->toPlainText().split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        if (lines.isEmpty() || lines.size() > 8) { QMessageBox::warning(this, QStringLiteral("地址数量"), QStringLiteral("请填写 1–8 个 HTTP/HTTPS 地址，每行一个。")); return; }
        if (explicitProxy->isChecked() && !validProxy(QUrl(proxy->text().trimmed()))) {
            QMessageBox::warning(this, QStringLiteral("代理地址无效"), QStringLiteral("支持 http://主机:端口 或 socks5://主机:端口；不要填写用户名、密码或查询参数。")); return;
        }
        const QString hash = sha1->text().trimmed(); bool sizeOk = false;
        const qint64 length = size->text().trimmed().isEmpty() ? -1 : size->text().toLongLong(&sizeOk);
        if ((!hash.isEmpty() && !QRegularExpression(QStringLiteral("^[a-fA-F0-9]{40}$")).match(hash).hasMatch())
            || (!size->text().trimmed().isEmpty() && (!sizeOk || length <= 0))) {
            QMessageBox::warning(this, QStringLiteral("校验参数无效"), QStringLiteral("SHA-1 应为 40 位十六进制；大小应为正整数。")); return;
        }
        if (segmented->isChecked() && (hash.isEmpty() || length < 4 * 1024 * 1024)) {
            QMessageBox::warning(this, QStringLiteral("分段条件不足"), QStringLiteral("生产分段流程需要完整 SHA-1 和至少 4 MiB 的已知大小。")); return;
        }
        if (segmented->isChecked() && concurrency->value() < 4) {
            QMessageBox::warning(this, QStringLiteral("分段预算不足"), QStringLiteral("分段模式至少需要 4 个并发额度；额度不足时生产模块会退回普通下载。")); return;
        }
        cases.clear();
        for (const auto &line : lines) {
            const QUrl url(line.trimmed());
            if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
                || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
                QMessageBox::warning(this, QStringLiteral("地址无效"), QStringLiteral("只支持不含用户名/密码的 HTTP/HTTPS 地址。")); return;
            }
            QStringList routes;
            if (direct->isChecked()) routes.append(QStringLiteral("direct"));
            if (system->isChecked()) routes.append(QStringLiteral("system"));
            if (explicitProxy->isChecked()) routes.append(QStringLiteral("explicit"));
            for (const auto &route : routes) for (bool allowH2 : {false, true}) {
                if ((!allowH2 && !h1->isChecked()) || (allowH2 && !h2->isChecked())) continue;
                cases.append({{"url", url.toString()}, {"name", url.host() + QStringLiteral(" · ") + route + (allowH2 ? QStringLiteral(" · H2优先") : QStringLiteral(" · H1") )},
                    {"route", route}, {"proxy", proxy->text().trimmed()}, {"h2", allowH2}, {"h1_connections", 12},
                    {"segmented", segmented->isChecked()}, {"sha1", hash.toLower()}, {"size", double(length)},
                    {"concurrency", concurrency->value()}, {"requests", requests->value()}, {"timeout_s", timeout->value()}});
            }
        }
        if (cases.isEmpty()) { QMessageBox::warning(this, QStringLiteral("没有测试组"), QStringLiteral("至少选择一种线路和一种协议。")); return; }
        if (QMessageBox::question(this, QStringLiteral("开始网络诊断"),
            QStringLiteral("将顺序运行 %1 组真实下载。整轮最长 900 秒，正文流量预算 512 MiB（有在途数据误差）。\n下载内容不执行、不安装，测试数据自动清理。日志含域名、路径及 DNS 地址，分享前请检查。\n明确直连仍可能受 TUN/透明代理影响。继续？").arg(cases.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        session = QDir(outputRoot->text()).filePath(QStringLiteral("network-%1-%2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")), QUuid::createUuid().toString(QUuid::Id128).left(6)));
        if (!QDir().mkpath(session)) { QMessageBox::warning(this, QStringLiteral("无法写入"), QStringLiteral("请更换结果目录。")); return; }
        results = {}; usedBytes = 0; cancelled = false; table->setRowCount(0); messages->clear();
        progress->setRange(0, int(cases.size())); progress->setValue(0);
        start->setEnabled(false); stop->setEnabled(true); open->setEnabled(true);
        sessionDeadline.start(900000); next();
    }
protected:
    void closeEvent(QCloseEvent *event) override {
        if (process) { closeAfter = true; cancelled = true; process->kill(); event->ignore(); }
        else QMainWindow::closeEvent(event);
    }
public:
    Window() {
        setWindowTitle(QStringLiteral("WTFML 网络诊断 · 独立下载模块")); resize(1000, 800);
        auto *body = new QWidget(this); auto *layout = new QVBoxLayout(body); setCentralWidget(body);
        auto *notice = new QLabel(QStringLiteral("固定地址、固定并发、全新下载目录，对比直连/代理和 H1/H2。H2 优先不保证协商成功，请看日志实际协议。"), body);
        notice->setWordWrap(true); layout->addWidget(notice);
        auto *sample = new QComboBox(body);
        sample->addItems({QStringLiteral("小 JSON：官方与镜像"), QStringLiteral("大文件：日志中的 26.3 客户端（41.5 MB）"), QStringLiteral("自定义地址")});
        layout->addWidget(sample);
        urls = new QPlainTextEdit(body); urls->setMaximumHeight(100); urls->setPlaceholderText(QStringLiteral("每行一个 URL；同一校验参数会用于所有地址。")); layout->addWidget(urls);
        auto *form = new QFormLayout;
        sha1 = new QLineEdit(body); size = new QLineEdit(body);
        form->addRow(QStringLiteral("预期 SHA-1（可选）"), sha1); form->addRow(QStringLiteral("文件大小/字节（可选）"), size);
        auto *routing = new QWidget(body); auto *routingLayout = new QHBoxLayout(routing); routingLayout->setContentsMargins(0, 0, 0, 0);
        direct = new QCheckBox(QStringLiteral("明确直连"), body); direct->setChecked(true);
        system = new QCheckBox(QStringLiteral("系统代理"), body); system->setChecked(true);
        explicitProxy = new QCheckBox(QStringLiteral("指定代理"), body);
        proxy = new QLineEdit(QStringLiteral("http://127.0.0.1:7897"), body);
        routingLayout->addWidget(direct); routingLayout->addWidget(system); routingLayout->addWidget(explicitProxy); routingLayout->addWidget(proxy);
        form->addRow(QStringLiteral("线路"), routing);
        auto *controls = new QWidget(body); auto *controlsLayout = new QHBoxLayout(controls); controlsLayout->setContentsMargins(0, 0, 0, 0);
        h1 = new QCheckBox(QStringLiteral("HTTP/1.1"), body); h1->setChecked(true);
        h2 = new QCheckBox(QStringLiteral("HTTP/2 优先"), body); h2->setChecked(true);
        segmented = new QCheckBox(QStringLiteral("生产分段流程"), body);
        controlsLayout->addWidget(h1); controlsLayout->addWidget(h2); controlsLayout->addWidget(segmented); controlsLayout->addStretch();
        form->addRow(QStringLiteral("协议/模式"), controls);
        auto *limits = new QWidget(body); auto *limitsLayout = new QHBoxLayout(limits); limitsLayout->setContentsMargins(0, 0, 0, 0);
        concurrency = new QSpinBox(body); concurrency->setRange(1, 64); concurrency->setValue(4);
        requests = new QSpinBox(body); requests->setRange(1, 16); requests->setValue(1);
        timeout = new QSpinBox(body); timeout->setRange(5, 180); timeout->setValue(120);
        limitsLayout->addWidget(new QLabel(QStringLiteral("并发"))); limitsLayout->addWidget(concurrency);
        limitsLayout->addWidget(new QLabel(QStringLiteral("每组重复请求"))); limitsLayout->addWidget(requests);
        limitsLayout->addWidget(new QLabel(QStringLiteral("每组限时/秒"))); limitsLayout->addWidget(timeout); limitsLayout->addStretch();
        form->addRow(QStringLiteral("负载与限时"), limits);
        limits->setToolTip(QStringLiteral("普通模式下，并发只有在重复请求数足够时才会用满；分段模式每个文件内部最多四路。"));
        auto *folder = new QWidget(body); auto *folderLayout = new QHBoxLayout(folder); folderLayout->setContentsMargins(0, 0, 0, 0);
        outputRoot = new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("WTFML网络诊断")), body);
        auto *browse = new QPushButton(QStringLiteral("选择目录"), body); folderLayout->addWidget(outputRoot); folderLayout->addWidget(browse);
        form->addRow(QStringLiteral("结果目录"), folder); layout->addLayout(form);
        connect(browse, &QPushButton::clicked, this, [this] { const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("结果目录"), outputRoot->text()); if (!path.isEmpty()) outputRoot->setText(path); });
        auto *buttons = new QHBoxLayout;
        start = new QPushButton(QStringLiteral("开始对照测试"), body); stop = new QPushButton(QStringLiteral("停止"), body); open = new QPushButton(QStringLiteral("打开结果目录"), body);
        stop->setEnabled(false); open->setEnabled(false); buttons->addWidget(start); buttons->addWidget(stop); buttons->addStretch(); buttons->addWidget(open); layout->addLayout(buttons);
        progress = new QProgressBar(body); progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
        auto *splitter = new QSplitter(Qt::Vertical, body);
        table = new QTableWidget(0, 5, splitter); table->setHorizontalHeaderLabels({QStringLiteral("测试组"), QStringLiteral("结果"), QStringLiteral("秒"), QStringLiteral("接收 MiB"), QStringLiteral("首字节中位/ms")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        messages = new QPlainTextEdit(splitter); messages->setReadOnly(true); splitter->addWidget(table); splitter->addWidget(messages); layout->addWidget(splitter, 1);
        connect(start, &QPushButton::clicked, this, [this] { begin(); });
        connect(stop, &QPushButton::clicked, this, [this] { cancelled = true; if (process) process->kill(); });
        connect(open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(session)); });
        groupDeadline.setSingleShot(true); sessionDeadline.setSingleShot(true);
        connect(&groupDeadline, &QTimer::timeout, this, [this] { messages->appendPlainText(QStringLiteral("本组硬超时，强制结束子进程。")); if (process) process->kill(); });
        connect(&sessionDeadline, &QTimer::timeout, this, [this] { cancelled = true; messages->appendPlainText(QStringLiteral("整轮达到 900 秒，已停止。")); if (process) process->kill(); });
        const auto fill = [this](int index) {
            if (index == 0) {
                urls->setPlainText(QStringLiteral("https://piston-meta.mojang.com/v1/packages/11f1aefe473eefba0af62e3286c33b692807b2eb/26.3.json\nhttps://bmclapi2.bangbang93.com/version/26.3/json"));
                sha1->setText(QStringLiteral("11f1aefe473eefba0af62e3286c33b692807b2eb")); size->setText(QStringLiteral("44992"));
            } else if (index == 1) {
                urls->setPlainText(QStringLiteral("https://piston-data.mojang.com/v1/objects/e877b6a07acd633fb3bb475002175cec036e7b87/client.jar\nhttps://bmclapi2.bangbang93.com/version/26.3/client"));
                sha1->setText(QStringLiteral("e877b6a07acd633fb3bb475002175cec036e7b87")); size->setText(QStringLiteral("41483720"));
            } else { urls->clear(); sha1->clear(); size->clear(); }
        };
        connect(sample, &QComboBox::currentIndexChanged, this, fill); fill(0);
    }
};

static QString fixtureRequestPath(const QByteArray &request) {
    const auto firstLine = request.left(request.indexOf("\r\n")).split(' ');
    return QUrl::fromEncoded(firstLine.value(1)).path();
}

static int selfTest(QApplication &app) {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) return 1;
    const QByteArray payload(32768, 'x');
    const QByteArray large(4 * 1024 * 1024 + 127, 'z');
    int dispatcherRequests = 0;
    int refreshedRequests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, payload, large, &dispatcherRequests, &refreshedRequests] {
                if (socket->property("responded").toBool()) return;
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) return;
                // Route by the request target, never by a substring of all headers
                // (redirect-related headers may contain the previous URL).
                const auto path = fixtureRequestPath(request);
                socket->setProperty("responded", true);
                if (path == QStringLiteral("/stall")) return;
                QByteArray response;
                if (path == QStringLiteral("/refresh")) {
                    ++refreshedRequests;
                    response = "HTTP/1.1 302 Found\r\nLocation: " + QByteArray(refreshedRequests == 1 ? "/expired" : "/large")
                        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else if (path == QStringLiteral("/expired") && !request.contains("bytes=0-0\r\n")) {
                    response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else if (path == QStringLiteral("/dispatch")) {
                    ++dispatcherRequests;
                    response = "HTTP/1.1 302 Found\r\nLocation: /large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else if (path == QStringLiteral("/throttle")) response = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else if (path == QStringLiteral("/redirect")) response = "HTTP/1.1 302 Found\r\nLocation: /ok?secret=redirect-token\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else {
                    QByteArray data = (path == QStringLiteral("/large") || path == QStringLiteral("/expired")) ? large : payload;
                    const auto range = QRegularExpression(QStringLiteral("Range: bytes=(\\d+)-(\\d+)"), QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(request));
                    if (range.hasMatch()) {
                        const auto total = data.size(); const auto first = range.captured(1).toLongLong();
                        const auto last = qMin<qint64>(range.captured(2).toLongLong(), total - 1);
                        response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' + QByteArray::number(total) + "\r\n";
                        data = data.mid(first, last - first + 1);
                    } else response = "HTTP/1.1 200 OK\r\n";
                    response += "Content-Length: " + QByteArray::number(data.size()) + "\r\nETag: \"diag-fixture\"\r\nSet-Cookie: hidden=COOKIE_SECRET\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n" + data;
                }
                socket->write(response); socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    int failures = 0;
    const auto check = [&](bool condition, const char *text) { std::printf("%s %s\n", condition ? "PASS" : "FAIL", text); if (!condition) ++failures; };
    check(fixtureRequestPath("GET /ok?previous=/refresh HTTP/1.1\r\nReferer: http://localhost/expired\r\n\r\n") == QStringLiteral("/ok") &&
          fixtureRequestPath("GET http://localhost/large HTTP/1.1\r\nReferer: http://localhost/refresh\r\n\r\n") == QStringLiteral("/large"),
          "fixture routes by target path, ignoring query strings and redirect-related headers");
    check(!validProxy(QUrl(QStringLiteral("http://user:pass@127.0.0.1:7897"))) && validProxy(QUrl(QStringLiteral("socks5://127.0.0.1:7897"))), "proxy validation rejects credentials");
    check(!redact(QStringLiteral("https://user:pass@example.test/a?token=SECRET#fragment")).contains(QStringLiteral("SECRET")), "URL parameters are redacted");
    const QUrl cooled(QStringLiteral("https://cooldown.test/a"));
    DownloadRetryPolicy::coolDown(cooled, 1000);
    check(DownloadRetryPolicy::coolingDownMs(QUrl(QStringLiteral("https://cooldown.test/b"))) > 0 &&
          DownloadRetryPolicy::recovering(cooled) &&
          DownloadRetryPolicy::coolingDownMs(QUrl(QStringLiteral("https://other.test/a"))) == 0,
          "origin cooldown covers siblings without blocking unrelated sources");
    for (int scenario = 0; scenario < 10; ++scenario) {
        const auto base = directory.filePath(QString::number(scenario)); QDir().mkpath(base);
        QTemporaryDir scratch(directory.filePath(QStringLiteral("scratch-XXXXXX")));
        const QString suffix = scenario == 1 ? QStringLiteral("/stall") : scenario == 2 ? QStringLiteral("/redirect?secret=initial-token")
            : scenario == 5 ? QStringLiteral("/large") : scenario == 7 ? QStringLiteral("/dispatch")
            : scenario == 8 ? QStringLiteral("/throttle") : scenario == 9 ? QStringLiteral("/refresh") : QStringLiteral("/ok");
        const QByteArray data = (scenario == 5 || scenario == 7 || scenario == 9) ? large : payload;
        const QJsonObject config{{"url", QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()) + suffix},
            {"directory", base}, {"scratch", scratch.path()}, {"route", scenario == 3 ? "explicit" : "direct"},
            {"proxy", QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())}, {"h2", scenario == 2}, {"h1_connections", 12},
            {"concurrency", (scenario == 5 || scenario == 7 || scenario == 9) ? 8 : 2}, {"requests", scenario == 0 ? 2 : 1}, {"timeout_s", scenario == 1 ? 1 : 6},
            {"name", QString::number(scenario)}, {"sha1", scenario == 6 ? QString(40, '0') : QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex())},
            {"size", data.size()}, {"budget", scenario == 4 ? 1024 : 8 * 1024 * 1024}, {"segmented", scenario == 5 || scenario == 7 || scenario == 9}};
        QProcess child; QEventLoop loop; QTimer deadline;
        QByteArray stdoutBytes;
        QString processError;
        bool timedOut = false;
        QElapsedTimer processClock;
        processClock.start();
        QObject::connect(&child, &QProcess::readyReadStandardOutput, &loop, [&] { stdoutBytes += child.readAllStandardOutput(); });
        QObject::connect(&child, &QProcess::started, &loop, [&] { child.write(QJsonDocument(config).toJson()); child.closeWriteChannel(); });
        QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
        QObject::connect(&child, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError error) {
            processError = QStringLiteral("process error %1").arg(int(error));
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { timedOut = true; child.kill(); });
        // CI 机器上分段下载场景比本机慢；10 秒会把正常的等待误判成超时。
        // 非分段场景仍留 15 秒，分段下载留 30 秒；总闸由 CTest 的 TIMEOUT 管。
        const bool segmentedScenario = scenario == 5 || scenario == 7 || scenario == 9;
        deadline.setSingleShot(true);
        deadline.start(segmentedScenario ? 30000 : 15000);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--worker")}); loop.exec();
        stdoutBytes += child.readAllStandardOutput();
        QJsonObject result;
        for (const auto &line : stdoutBytes.split('\n')) {
            const auto object = QJsonDocument::fromJson(line).object();
            if (object.value("event") == QJsonValue(QStringLiteral("result"))) result = object;
        }
        const bool expectedSuccess = scenario != 1 && scenario != 4 && scenario != 6 && scenario != 8;
        const bool workerPassed = !result.isEmpty() && result.value("success").toBool() == expectedSuccess;
        if (!workerPassed) {
            // 失败时把“哪个场景、怎么退出、有没有被杀、worker 最后说了什么”一起留下；
            // 只有一句 FAIL 的话，下一次 CI 偶发失败仍然只能猜。
            std::printf("network-diag scenario %d failed: timedOut=%d exitStatus=%d exitCode=%d elapsed=%lldms processError=%s\n",
                        scenario, timedOut ? 1 : 0, int(child.exitStatus()), child.exitCode(),
                        static_cast<long long>(processClock.elapsed()), processError.toUtf8().constData());
            std::printf("network-diag scenario %d stdout tail: %s\n", scenario,
                        stdoutBytes.right(2000).constData());
        }
        check(workerPassed, "production worker handles direct/proxy/redirect/deadline/budget/checksum");
        if (scenario == 7) check(dispatcherRequests == 1 && result.value("request_count").toInt() >= 3,
                                 "range parts reuse the probed node instead of repeating dispatch");
        // 断言仍然是**严格的两次**（docs/dev/TESTING.md 2026-10-20 那轮明确决定：次数不符但
        // 下载成功时也要暴露出来）。2026-11-03 在 CI 上数到 3 次而下载成功，说明这一档在
        // 8 路并发下是**次数取决于调度**的 —— 那属于产品侧"重探有没有被合并"的问题，
        // 不该靠放宽断言解决（2026-11-05 复核纠正：这个工具**不带参数就是 GUI 窗口**，
        // 在无交互的环境里当然"停住"；本机要用 `--self-test` 跑（offscreen），实测 48 条全过、
        // 18.9 秒、退出码 0，与 CI 的约 19 秒一致。所以这一条原样保留）。
        if (scenario == 9) check(refreshedRequests == 2 && result.value("success").toBool(),
                                 "expired node is reprobed once and missing ranges complete safely");
        if (scenario == 8) check(result.value("throttled").toBool() && result.value("elapsed_ms").toDouble() < 3000,
                                 "diagnostic worker stops promptly on HTTP 429");
        QFile events(QDir(base).filePath(QStringLiteral("events.jsonl"))); const bool opened = events.open(QIODevice::ReadOnly);
        const auto log = opened ? events.readAll() : QByteArray();
        if (scenario == 9 && (refreshedRequests != 2 || !result.value("success").toBool())) {
            std::printf("refresh fixture: dispatcher_requests=%d worker_success=%d requests=%d\n",
                        refreshedRequests, result.value("success").toBool(), result.value("request_count").toInt());
            std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            for (const auto &line : log.split('\n'))
                if (line.contains("\"event\":\"engine\"")) std::puts(line.constData());
        }
        check(opened && log.contains("request_created") && log.contains("case_finished"), "structured events survive completion and timeout");
        check(!log.contains("initial-token") && !log.contains("redirect-token") && !log.contains("COOKIE_SECRET"), "query credentials and cookies do not leak into logs");
        if (scenario != 1) check(result.value("protocols").toObject().contains(QStringLiteral("h1")), "actual protocol records H1 even when H2 was allowed");
        if (scenario == 5) {
            const bool observed = result.value("request_count").toInt() >= 3 && log.contains("bytes=0-0") && log.contains("resumable");
            check(observed, "instrumented network factory propagates through real segmented downloads");
            if (!observed) {
                std::fputs(QJsonDocument(result).toJson(QJsonDocument::Compact).constData(), stdout);
                for (const auto &line : log.split('\n')) if (line.contains("\"event\":\"engine\"")) std::puts(line.constData());
            }
        }
    }
    QProcess blocked; QEventLoop blockedLoop; QTimer kill;
    QObject::connect(&blocked, &QProcess::finished, &blockedLoop, &QEventLoop::quit);
    QObject::connect(&kill, &QTimer::timeout, &blockedLoop, [&] { blocked.kill(); });
    QElapsedTimer elapsed; elapsed.start();
    blocked.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--worker")});
    // Leave stdin open: simulate a worker stuck before it can start its own event-loop timeout.
    kill.setSingleShot(true); kill.start(500); blockedLoop.exec();
    check(elapsed.elapsed() < 3000 && blocked.exitStatus() == QProcess::CrashExit, "parent can kill an unresponsive worker independently");
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("WTFML-Network-Diagnostics"));
    QCoreApplication::setApplicationName(QStringLiteral("WTFMLNetworkDiag"));
    if (app.arguments().contains(QStringLiteral("--worker"))) {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
        const auto bytes = input.read(65537);
        if (bytes.size() > 65536) return 2;
        return worker(app, QJsonDocument::fromJson(bytes).object());
    }
    if (app.arguments().contains(QStringLiteral("--self-test"))) return selfTest(app);
    app.setStyle(QStringLiteral("Fusion"));
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    if (QFileInfo::exists(fontPath)) {
        const int id = QFontDatabase::addApplicationFont(fontPath);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) app.setFont(QFont(families.first(), 9));
    }
    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f3f8f7")));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#e4f1eb")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#19332e")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#19332e")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#19332e")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#087a68")));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    app.setPalette(palette);
    app.setStyleSheet(QStringLiteral("QPushButton { padding: 5px 12px; border: 1px solid #bdd6cd; border-radius: 4px; } QPushButton:disabled { color: #83958d; }"));
    Window window; window.show();
    const int preview = app.arguments().indexOf(QStringLiteral("--preview"));
    if (preview >= 0 && preview + 1 < app.arguments().size()) {
        QTimer::singleShot(200, &app, [&] {
            app.exit(window.grab().save(app.arguments()[preview + 1]) ? 0 : 1);
        });
    }
    return app.exec();
}
