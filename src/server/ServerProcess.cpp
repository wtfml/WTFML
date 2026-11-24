/**
 * @file ServerProcess.cpp
 * @brief 服务端那一条 `QProcess` 的实现（形状与取舍见头文件）。
 */

#include "ServerProcess.hpp"
#include "util/WriteDirectoryGuard.hpp"

#include <QDateTime>
#include <QDir>
#include <QTimer>

#include "launcher/ProcessPathLimit.hpp"
#include "server/ServerRun.hpp"

namespace ServerRun {

namespace {

/// 日志缓冲区留多少行。**留个上限**：一个跑了几天的服务端能吐出几十万行，全攒在内存里
/// 迟早把启动器拖死；控制台那一页只画看得见的那一段。
constexpr int kMaxLogLines = 5000;

/// `stopAndWait()` 在宽限时间之外**额外**给的余量：宽限到了那只说明"可以强杀了"，
/// 不是"它一定还没退"。多等这一会儿，能少一批"刚好在退的最后一步被杀掉"的收场。
constexpr int kStopWaitSlackMs = 5000;

}  // namespace

int ServerProcess::maxLogLines() {
    return kMaxLogLines;
}

ServerProcess::ServerProcess(QObject *parent) : QObject(parent) {}

ServerProcess::~ServerProcess() {
    if (m_gracefulTimer != nullptr) {
        m_gracefulTimer->stop();
    }
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
        // 这里是**收尾**，不是"正常停"：能问用户的那一发（关窗时"要不要一起停掉"）在
        // `MainWindow` 那边先问过了，走到这儿说明已经决定不要它了。所以直接杀、只给三秒
        // 兜底 —— 不能让 `QProcess` 的析构自己去等（那一等就是几十秒界面不动）。
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

bool ServerProcess::isRunning() const {
    return m_process != nullptr && m_process->state() != QProcess::NotRunning;
}

void ServerProcess::start(const QString &serverId, const QString &dir, const QString &program,
                          const QStringList &arguments, int gracefulMs) {
    if (isRunning()) {
        return;  // 同一个服务端已经在跑了 —— 调用方该先拦住（"重复运行提醒"）
    }
    QString problem;
    if (!WriteDirectoryGuard::acquire(dir, &problem)) {
        m_serverId = serverId;
        setStatus(ServerModel::Status::Crashed);
        emit message(problem);
        emit exited(serverId, -1);
        return;
    }
    if (m_process == nullptr) {
        m_process = new QProcess(this);
        // 两路合并：服务端把一部分话写在 stderr（崩栈、mod 的报错），分开收的话控制台里
        // 那一行的上下文会缺一半。合并之后顺序就是它真实吐出来的顺序。
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_process, &QProcess::readyReadStandardOutput, this, &ServerProcess::drainOutput);
        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus) {
                    drainOutput();
                    m_gracefulTimer->stop();
                    m_forceKillAllowed = false;
                    m_startedAtMs = 0;
                    // 原版端口绑定失败也可能返回 0；从未就绪且不是用户停止，不能伪装正常结束。
                    const bool startupFailed = !m_started && !m_killed
                        && m_status != ServerModel::Status::Stopping;
                    setStatus(startupFailed ? ServerModel::Status::Crashed
                                           : statusAfterExit(exitCode, m_killed));
                    emit exited(m_serverId, exitCode);
                });
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError code) {
            // `FailedToStart`（java 路径写错、权限不够…）那一路**不会有** `finished` ——
            // 它是另一条收尾，必须自己接。退出码按 −1 报（"没起得来"不是任何一种退出码）。
            if (code != QProcess::FailedToStart) {
                return;
            }
            m_gracefulTimer->stop();
            m_forceKillAllowed = false;
            m_startedAtMs = 0;
            setStatus(ServerModel::Status::Crashed);
            emit message(tr("服务端进程未启动（%1）。请检查 Java 设置。")
                             .arg(QDir::toNativeSeparators(m_program)) +
                         ProcessPathLimit::tooLongHint({m_program, m_dir}));
            emit exited(m_serverId, -1);
        });
    }
    if (m_gracefulTimer == nullptr) {
        m_gracefulTimer = new QTimer(this);
        m_gracefulTimer->setSingleShot(true);
        connect(m_gracefulTimer, &QTimer::timeout, this, [this] {
            if (!isRunning()) {
                return;
            }
            // 到点了：进程还没退 ⇒ 界面可以把「强制结束」放出来（见头文件那段）。
            m_forceKillAllowed = true;
            emit forceKillAvailable(m_serverId);
        });
    }

    m_serverId = serverId;
    m_dir = dir;
    m_program = program;
    m_gracefulMs = gracefulMs;
    m_pending.clear();
    m_log.clear();
    m_forceKillAllowed = false;
    m_started = false;
    m_killed = false;
    m_startedAtMs = QDateTime::currentMSecsSinceEpoch();

    m_process->setProgram(program);
    m_process->setArguments(arguments);
    m_process->setWorkingDirectory(dir);
    setStatus(ServerModel::Status::Starting);
    emit message(tr("起服务端 %1：%2 %3")
                     .arg(serverId, QDir::toNativeSeparators(program),
                          arguments.join(QLatin1Char(' '))));
    m_process->start();
}

bool ServerProcess::requestStop() {
    if (!isRunning() || m_process == nullptr) {
        return false;
    }
    if (m_status == ServerModel::Status::Stopping) {
        return false;  // 已经发过一句了，别催
    }
    if (m_process->write(stopCommandBytes()) <= 0) {
        return false;
    }
    setStatus(ServerModel::Status::Stopping);
    // 宽限时间到点才允许强杀 —— 这一段里界面那颗按钮是灰的（见头文件那段）。
    m_gracefulTimer->start(qMax(0, m_gracefulMs));
    emit message(tr("%1：已经让它正常停下来（给它 %2 秒存盘）。")
                     .arg(m_serverId)
                     .arg(qMax(0, m_gracefulMs) / 1000));
    return true;
}

void ServerProcess::forceKill() {
    if (m_process == nullptr || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_killed = true;
    emit message(tr("%1：强制结束。").arg(m_serverId));
    m_process->kill();
}

bool ServerProcess::stopAndWait() {
    if (!isRunning() || m_process == nullptr) {
        return true;  // 已经不在跑了 —— "停干净了"是事实
    }
    // 已经在停止中（用户先按过「停止服务端」）就不必再写一句 —— 那一句已经在路上了，
    // 这一趟只是接着等它。
    const bool asked = (m_status == ServerModel::Status::Stopping) ? true : requestStop();
    if (!asked) {
        // 那一句写不进去（stdin 关了 / 它根本不在读）⇒ 只剩强杀一条路。这一步**在这一条
        // 路上是允许的**：调用方（关窗）已经决定不要它了。
        forceKill();
    }
    const int waitMs = qMax(0, m_gracefulMs) + kStopWaitSlackMs;
    const bool finished = m_process->waitForFinished(waitMs);
    if (!finished) {
        forceKill();
        m_process->waitForFinished(kStopWaitSlackMs);
    }
    return asked && finished;
}

void ServerProcess::detach() {
    if (m_gracefulTimer != nullptr) {
        m_gracefulTimer->stop();  // 交出去之后不该再发 `forceKillAvailable` 了
    }
    m_forceKillAllowed = false;
    if (m_process == nullptr) {
        return;
    }
    // 先断信号：交出去之后 `exited` 之类的槽不该再被调到（那时界面已经在拆了）。
    m_process->disconnect();
    // 关键那一步：脱离 `this` 的所有权。析构链（MainWindow 是 main() 里的栈对象）走到
    // 这里时，它已经不是我们的子对象，下面析构里那套"进程还在跑就 kill()"跑不到它，
    // java.exe 自己继续活在系统里。
    m_process->setParent(nullptr);
    // NOTE: 这一条 `QProcess` **故意不释放**（同 `LauncherManager::detachRunningGame()` 那段）：
    // 释放它就等于执行析构、等于把服务端强杀。本对象也不再认得它（`m_process = nullptr`
    // —— 析构那一句正因为如此才够不着它）；泄漏的就是一个 QProcess 壳子，进程退出时由
    // 系统回收。指针不置空会更危险，留一个活着的壳子在这儿没有任何用处。
    m_process = nullptr;
    m_status = ServerModel::Status::Stopped;
    m_startedAtMs = 0;
}

bool ServerProcess::sendCommand(const QString &command) {
    if (!isRunning() || m_process == nullptr) {
        return false;
    }
    // 换行**必须补**（见头文件那段）。和 `stop` 那句走的是同一条通道，顺带也是同一个
    // 理由：服务端那边是一行一行读的。
    return m_process->write(command.toLocal8Bit() + '\n') > 0;
}

void ServerProcess::setStatus(ServerModel::Status status) {
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(m_serverId, status);
}

void ServerProcess::appendLine(const QString &line) {
    m_log.append(line);
    while (m_log.size() > kMaxLogLines) {
        m_log.removeFirst();
    }
    emit lineAppended(m_serverId, line);

    // Fabric can leave a Java error dialog/thread alive after this fatal startup
    // exception. It cannot recover by waiting; stop only our own failed process.
    if (!m_started && m_status == ServerModel::Status::Starting &&
        (line.startsWith(QStringLiteral("java.lang.UnsupportedClassVersionError:")) ||
         line.startsWith(QStringLiteral("Caused by: java.lang.UnsupportedClassVersionError:")))) {
        setStatus(ServerModel::Status::Crashed);
        emit message(tr("服务端使用的 Java 版本过旧，已停止失败的启动；请选择匹配的 Java 后重试。"));
        if (m_process) m_process->kill();
        return;
    }

    if (!m_started && m_status == ServerModel::Status::Starting && isReadyLine(line)) {
        // 服务端没有"窗口出现了"那种信号 —— 这一行才是"真的在跑了"。读到之后启动就
        // 结束了，宽限那个计时器也就不用等了（它只在"停止中"才有意义）。
        m_started = true;
        m_gracefulTimer->stop();
        setStatus(ServerModel::Status::Running);
    }
}

void ServerProcess::drainOutput() {
    if (m_process == nullptr) {
        return;
    }
    // 先按**字节**切行、切出整行再解码 —— 不是拿到手就整块 `fromLocal8Bit`。
    // 判据和那个"为什么"都在 `ServerRun::takeLines()` 的注释里（ 修：管道
    // 把一个多字节字符从中间劈成两截时，逐块解码会解出两个乱码，T41.3 那条断言就时红时绿）。
    const QStringList lines = takeLines(m_pending, m_process->readAllStandardOutput());
    for (const QString &line : lines) {
        appendLine(line);
    }
}

}  // namespace ServerRun
