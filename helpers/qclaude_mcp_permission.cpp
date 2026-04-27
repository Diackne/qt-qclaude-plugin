// qclaude_mcp_permission — minimal MCP stdio server exposing a single
// `permission_prompt` tool, used as Claude Code's
// `--permission-prompt-tool mcp__qclaude_perm__permission_prompt`.
//
// This is the "true" pre-flight gate — unlike the older PreToolUse hook,
// `--permission-prompt-tool` is invoked for *every* tool that needs
// permission, including MCP tool calls (`mcp__server__tool`), so the
// panel's Allow/Deny UI now covers the MCP surface as well.
//
// We still only show the UI for file-modification tools and MCP calls;
// everything else (Bash, Read, Glob, …) auto-allows. Gated requests are
// forwarded to the plugin via QLocalSocket using the same wire format as
// qclaude_hook_bridge so the plugin-side HookServer doesn't have to know
// which helper is calling it.
//
// Wire format on the plugin socket (newline-delimited JSON):
//   helper → plugin   {"id":"…","tool_name":"…","tool_input":{…},"cwd":"…"}
//   plugin → helper   {"id":"…","decision":"allow"|"deny","reason":"…"}
//
// MCP transport: line-delimited JSON-RPC 2.0 on stdio (one message per
// line, no Content-Length framing — per the 2024-11-05 stdio transport).
// On any error talking to the plugin we fall through to "allow" so an
// unreachable plugin never strands Claude mid-turn — the post-hoc
// Edit/Write card is the safety net in that case.

#include <QByteArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QPair>
#include <QString>
#include <QUuid>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

QString g_socketPath;

void writeStdoutLine(const QJsonObject &message)
{
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

QJsonObject makeResponse(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject root;
    root.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("result"), result);
    return root;
}

QJsonObject makeError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject err;
    err.insert(QStringLiteral("code"), code);
    err.insert(QStringLiteral("message"), message);
    QJsonObject root;
    root.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    root.insert(QStringLiteral("id"), id);
    root.insert(QStringLiteral("error"), err);
    return root;
}

// Tools the panel actually wants to gate. Read/Glob/Grep/etc. fall through
// silently so the user doesn't have to click Allow on every directory
// listing during a typical turn.
bool shouldGate(const QString &toolName)
{
    return toolName == QStringLiteral("Edit")
        || toolName == QStringLiteral("Write")
        || toolName == QStringLiteral("MultiEdit")
        || toolName == QStringLiteral("NotebookEdit")
        || toolName.startsWith(QStringLiteral("mcp__"));
}

// Synchronous round-trip to the plugin's QLocalServer. Returns
// (decision, reason); falls open ("allow") on any error.
QPair<QString, QString> askPlugin(const QString &toolName,
                                  const QJsonObject &toolInput)
{
    if (g_socketPath.isEmpty())
        return { QStringLiteral("allow"),
                 QStringLiteral("permission helper: no socket configured") };

    QLocalSocket sock;
    sock.connectToServer(g_socketPath);
    if (!sock.waitForConnected(2000))
        return { QStringLiteral("allow"),
                 QStringLiteral("permission helper: plugin not reachable") };

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject req;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("tool_name"), toolName);
    req.insert(QStringLiteral("tool_input"), toolInput);
    req.insert(QStringLiteral("cwd"), QDir::currentPath());
    QByteArray frame = QJsonDocument(req).toJson(QJsonDocument::Compact);
    frame.append('\n');
    sock.write(frame);
    sock.flush();

    // Hard cap so we don't hang forever if the user never answers. 10
    // minutes mirrors qclaude_hook_bridge; past that, allow and let the
    // post-hoc card take over.
    const int timeoutMs = 10 * 60 * 1000;
    QElapsedTimer t;
    t.start();

    QByteArray inbuf;
    QString decision;
    QString reason;
    bool got = false;
    while (!got && t.elapsed() < timeoutMs) {
        if (!sock.waitForReadyRead(500)) {
            if (sock.state() != QLocalSocket::ConnectedState)
                break;
            continue;
        }
        inbuf += sock.readAll();
        while (true) {
            const int nl = inbuf.indexOf('\n');
            if (nl < 0)
                break;
            const QByteArray line = inbuf.left(nl).trimmed();
            inbuf.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject())
                continue;
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("id")).toString() != id)
                continue;
            decision = obj.value(QStringLiteral("decision")).toString();
            reason = obj.value(QStringLiteral("reason")).toString();
            got = true;
            break;
        }
    }
    sock.disconnectFromServer();

    if (!got)
        return { QStringLiteral("allow"),
                 QStringLiteral("permission helper: timeout / no response") };
    if (decision != QStringLiteral("allow") && decision != QStringLiteral("deny"))
        decision = QStringLiteral("allow");
    return { decision, reason };
}

QJsonObject buildToolsListResult()
{
    QJsonObject inputSchema;
    inputSchema.insert(QStringLiteral("type"), QStringLiteral("object"));

    QJsonObject properties;
    QJsonObject toolNameProp;
    toolNameProp.insert(QStringLiteral("type"), QStringLiteral("string"));
    properties.insert(QStringLiteral("tool_name"), toolNameProp);
    QJsonObject inputProp;
    inputProp.insert(QStringLiteral("type"), QStringLiteral("object"));
    properties.insert(QStringLiteral("input"), inputProp);
    QJsonObject toolUseIdProp;
    toolUseIdProp.insert(QStringLiteral("type"), QStringLiteral("string"));
    properties.insert(QStringLiteral("tool_use_id"), toolUseIdProp);
    inputSchema.insert(QStringLiteral("properties"), properties);

    QJsonArray required;
    required.append(QStringLiteral("tool_name"));
    required.append(QStringLiteral("input"));
    inputSchema.insert(QStringLiteral("required"), required);

    QJsonObject tool;
    tool.insert(QStringLiteral("name"), QStringLiteral("permission_prompt"));
    tool.insert(QStringLiteral("description"),
                QStringLiteral("QClaude permission gate. Returns Claude Code "
                               "permission decisions sourced from the plugin's "
                               "Allow / Deny UI."));
    tool.insert(QStringLiteral("inputSchema"), inputSchema);

    QJsonArray tools;
    tools.append(tool);
    QJsonObject result;
    result.insert(QStringLiteral("tools"), tools);
    return result;
}

QJsonObject handlePermissionPrompt(const QJsonObject &arguments)
{
    const QString toolName = arguments.value(QStringLiteral("tool_name")).toString();
    const QJsonObject input = arguments.value(QStringLiteral("input")).toObject();

    QString behavior;
    QString message;
    if (shouldGate(toolName)) {
        const auto [decision, reason] = askPlugin(toolName, input);
        behavior = (decision == QStringLiteral("deny"))
                       ? QStringLiteral("deny")
                       : QStringLiteral("allow");
        message = reason;
    } else {
        behavior = QStringLiteral("allow");
    }

    QJsonObject decisionObj;
    decisionObj.insert(QStringLiteral("behavior"), behavior);
    if (behavior == QStringLiteral("allow")) {
        // Echo the original input back as `updatedInput` — the gate is
        // pure approve/reject, we never rewrite arguments.
        decisionObj.insert(QStringLiteral("updatedInput"), input);
    } else {
        decisionObj.insert(QStringLiteral("message"),
                           message.isEmpty() ? QStringLiteral("denied by user")
                                             : message);
    }

    QJsonObject content;
    content.insert(QStringLiteral("type"), QStringLiteral("text"));
    content.insert(QStringLiteral("text"),
                   QString::fromUtf8(QJsonDocument(decisionObj)
                                         .toJson(QJsonDocument::Compact)));

    QJsonArray contents;
    contents.append(content);
    QJsonObject result;
    result.insert(QStringLiteral("content"), contents);
    return result;
}

void handleMessage(const QByteArray &line)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        // No id to respond to — just drop. Logging to stderr is fine
        // because Claude only consumes our stdout.
        std::fprintf(stderr, "qclaude_mcp_permission: malformed JSON-RPC line\n");
        return;
    }
    const QJsonObject req = doc.object();
    const QString method = req.value(QStringLiteral("method")).toString();
    const QJsonValue id = req.value(QStringLiteral("id"));
    const bool isNotification = id.isUndefined() || id.isNull();

    if (method == QStringLiteral("initialize")) {
        QJsonObject capabilities;
        capabilities.insert(QStringLiteral("tools"), QJsonObject{});
        QJsonObject serverInfo;
        serverInfo.insert(QStringLiteral("name"), QStringLiteral("qclaude_perm"));
        serverInfo.insert(QStringLiteral("version"), QStringLiteral("0.1.0"));
        QJsonObject result;
        result.insert(QStringLiteral("protocolVersion"),
                      QStringLiteral("2024-11-05"));
        result.insert(QStringLiteral("capabilities"), capabilities);
        result.insert(QStringLiteral("serverInfo"), serverInfo);
        if (!isNotification)
            writeStdoutLine(makeResponse(id, result));
        return;
    }

    if (method.startsWith(QStringLiteral("notifications/"))) {
        // Notifications never expect a response.
        return;
    }

    if (method == QStringLiteral("ping")) {
        if (!isNotification)
            writeStdoutLine(makeResponse(id, QJsonObject{}));
        return;
    }

    if (method == QStringLiteral("tools/list")) {
        if (!isNotification)
            writeStdoutLine(makeResponse(id, buildToolsListResult()));
        return;
    }

    if (method == QStringLiteral("tools/call")) {
        const QJsonObject params = req.value(QStringLiteral("params")).toObject();
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject arguments
            = params.value(QStringLiteral("arguments")).toObject();
        if (name != QStringLiteral("permission_prompt")) {
            if (!isNotification)
                writeStdoutLine(makeError(id, -32601,
                                          QStringLiteral("unknown tool: ") + name));
            return;
        }
        const QJsonObject result = handlePermissionPrompt(arguments);
        if (!isNotification)
            writeStdoutLine(makeResponse(id, result));
        return;
    }

    if (!isNotification)
        writeStdoutLine(
            makeError(id, -32601, QStringLiteral("method not found: ") + method));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qclaude_mcp_permission"));

    QCommandLineParser parser;
    QCommandLineOption socketOpt(
        QStringLiteral("socket"),
        QStringLiteral("Path/name of the QLocalServer the plugin is listening on."),
        QStringLiteral("path"));
    parser.addOption(socketOpt);
    parser.process(app);
    g_socketPath = parser.value(socketOpt);

    // Synchronous read loop — every operation we handle is synchronous
    // (the plugin socket round-trip blocks to wait for the user click), so
    // we don't need an event loop, just stdin → handle → stdout.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        handleMessage(QByteArray::fromStdString(line));
    }
    return 0;
}
