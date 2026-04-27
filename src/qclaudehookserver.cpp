#include "qclaudehookserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QUuid>

namespace QClaude::Internal {

HookServer::HookServer(QObject *parent)
    : QObject(parent)
{
}

HookServer::~HookServer()
{
    stop();
}

bool HookServer::isRunning() const
{
    return m_server && m_server->isListening();
}

QString HookServer::permissionPromptToolName()
{
    return QStringLiteral("mcp__qclaude_perm__permission_prompt");
}

bool HookServer::start(const QString &mcpHelperExecutable)
{
    if (isRunning())
        return true;
    if (mcpHelperExecutable.isEmpty() || !QFile::exists(mcpHelperExecutable))
        return false;

    if (!m_server)
        m_server = new QLocalServer(this);

    // Pick a fresh, process-unique socket name. The name is the bare token
    // (Qt prepends the platform-specific prefix, e.g. /tmp/<name> on Linux,
    // \\.\pipe\<name> on Windows).
    if (m_serverName.isEmpty()) {
        m_serverName = QStringLiteral("qclaude-hook-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    }
    // Clean up any stale endpoint from a previous launch.
    QLocalServer::removeServer(m_serverName);

    if (!m_server->listen(m_serverName)) {
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection,
            this, &HookServer::onNewConnection);

    if (!writeMcpConfig(mcpHelperExecutable)) {
        stop();
        return false;
    }
    return true;
}

void HookServer::stop()
{
    // Surface any in-flight permission requests as abandoned so the panel
    // can clean up its cards. We do this *before* dropping the sockets,
    // because the disconnect-driven path is suppressed (we explicitly
    // s->disconnect(this) below to avoid double-emits during teardown).
    const QStringList abandonedIds = m_pending.keys();
    for (const QString &id : abandonedIds)
        emit requestAbandoned(id);

    // Drop pending sockets so we don't dangle bridge subprocesses. The
    // bridge will see EOF on its socket, fall through its timeout/error
    // path, and emit allow — which is what we want when the user toggles
    // the gate off.
    for (QLocalSocket *s : m_socketIds.keys()) {
        if (s) {
            s->disconnect(this);
            s->disconnectFromServer();
            s->deleteLater();
        }
    }
    m_pending.clear();
    m_socketIds.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (!m_mcpConfigFilePath.isEmpty()) {
        QFile::remove(m_mcpConfigFilePath);
        m_mcpConfigFilePath.clear();
    }
}

void HookServer::respond(const QString &id, const QString &decision,
                         const QString &reason)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    QPointer<QLocalSocket> sockPtr = it.value();
    m_pending.erase(it);
    if (!sockPtr) {
        // Bridge already disconnected (e.g. timed out). Nothing to send.
        return;
    }
    QLocalSocket *sock = sockPtr.data();
    m_socketIds.remove(sock);

    QJsonObject reply;
    reply.insert(QStringLiteral("id"), id);
    reply.insert(QStringLiteral("decision"), decision);
    if (!reason.isEmpty())
        reply.insert(QStringLiteral("reason"), reason);
    QByteArray frame = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    frame.append('\n');
    sock->write(frame);
    sock->flush();
    sock->disconnectFromServer();
    // The socket cleans itself up via onSocketDisconnected → deleteLater.
}

void HookServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *sock = m_server->nextPendingConnection();
        if (!sock)
            continue;
        connect(sock, &QLocalSocket::readyRead,
                this, &HookServer::onSocketReadyRead);
        connect(sock, &QLocalSocket::disconnected,
                this, &HookServer::onSocketDisconnected);
        // We don't register the socket in m_socketIds yet — that happens
        // when we first see a request id on it.
    }
}

void HookServer::onSocketReadyRead()
{
    auto *sock = qobject_cast<QLocalSocket *>(sender());
    if (!sock)
        return;
    // Each socket only ever sends one frame, but tolerate fragmentation.
    static constexpr char kBufProp[] = "qclaude_buf";
    QByteArray buf = sock->property(kBufProp).toByteArray();
    buf += sock->readAll();

    while (true) {
        const int nl = buf.indexOf('\n');
        if (nl < 0)
            break;
        const QByteArray line = buf.left(nl).trimmed();
        buf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        const QJsonObject obj = doc.object();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QString tool = obj.value(QStringLiteral("tool_name")).toString();
        const QJsonObject input = obj.value(QStringLiteral("tool_input")).toObject();
        const QString cwd = obj.value(QStringLiteral("cwd")).toString();

        m_pending.insert(id, sock);
        m_socketIds.insert(sock, id);

        emit permissionRequested(id, tool, input, cwd);
    }

    sock->setProperty(kBufProp, buf);
}

void HookServer::onSocketDisconnected()
{
    auto *sock = qobject_cast<QLocalSocket *>(sender());
    if (!sock)
        return;
    const QString id = m_socketIds.take(sock);
    if (!id.isEmpty()) {
        m_pending.remove(id);
        emit requestAbandoned(id);
    }
    sock->deleteLater();
}

bool HookServer::writeMcpConfig(const QString &mcpHelperExecutable)
{
    // Build an MCP config registering qclaude_mcp_permission as a stdio
    // server named `qclaude_perm`. Claude is launched with
    // `--permission-prompt-tool mcp__qclaude_perm__permission_prompt`, so
    // every tool that needs permission (Edit/Write/MultiEdit/MCP/…) is
    // routed through the helper and the panel's Allow/Deny UI.
    QJsonArray helperArgs;
    helperArgs.append(QStringLiteral("--socket"));
    helperArgs.append(m_serverName);

    QJsonObject server;
    server.insert(QStringLiteral("type"), QStringLiteral("stdio"));
    server.insert(QStringLiteral("command"), mcpHelperExecutable);
    server.insert(QStringLiteral("args"), helperArgs);

    QJsonObject mcpServers;
    mcpServers.insert(QStringLiteral("qclaude_perm"), server);

    QJsonObject root;
    root.insert(QStringLiteral("mcpServers"), mcpServers);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/qclaude-mcp-config-%2.json")
        .arg(dir, QString::number(QCoreApplication::applicationPid()));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();

    m_mcpConfigFilePath = path;
    return true;
}

} // namespace QClaude::Internal
