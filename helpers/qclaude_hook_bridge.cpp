// qclaude_hook_bridge — tiny stdio↔socket relay invoked by Claude Code as a
// PreToolUse hook. Reads the hook payload from stdin, asks the plugin for a
// permission decision over a QLocalSocket, and writes Claude's hook-output
// JSON to stdout.
//
// Why a separate executable: Claude Code spawns the hook as a subprocess for
// every tool call, which means we can't just be a Qt slot. A 150-line
// QtCore-only binary keeps this self-contained and ships alongside the
// plugin.
//
// Wire format on the socket (newline-delimited JSON):
//   bridge → plugin   {"id":"<uuid>","tool_name":"...","tool_input":{...},"cwd":"..."}
//   plugin → bridge   {"id":"<uuid>","decision":"allow"|"deny","reason":"..."}
//
// The bridge writes Claude's hook-output schema to stdout:
//   {"hookSpecificOutput": {
//      "hookEventName":"PreToolUse",
//      "permissionDecision":"allow"|"deny",
//      "permissionDecisionReason":"..."}}
// On any error talking to the plugin we fall through to "allow" so a missing
// or crashed plugin never strands Claude mid-turn — the post-hoc
// Edit/Write card is the safety net in that case.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QString>
#include <QTextStream>
#include <QUuid>

#include <cstdio>

namespace {

// Emit Claude's hook-output JSON for an "allow" decision. Used both for the
// success path and for any local error (we'd rather let Claude continue and
// surface the failure in the post-hoc card than block silently).
void emitDecision(const QString &decision, const QString &reason)
{
    QJsonObject hookOut;
    hookOut.insert(QStringLiteral("hookEventName"), QStringLiteral("PreToolUse"));
    hookOut.insert(QStringLiteral("permissionDecision"), decision);
    if (!reason.isEmpty())
        hookOut.insert(QStringLiteral("permissionDecisionReason"), reason);
    QJsonObject root;
    root.insert(QStringLiteral("hookSpecificOutput"), hookOut);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

QByteArray readAllStdin()
{
    QByteArray buf;
    char chunk[4096];
    while (true) {
        const std::size_t n = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (n == 0)
            break;
        buf.append(chunk, int(n));
    }
    return buf;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qclaude_hook_bridge"));

    QCommandLineParser parser;
    QCommandLineOption socketOpt(
        QStringLiteral("socket"),
        QStringLiteral("Path/name of the QLocalServer the plugin is listening on."),
        QStringLiteral("path"));
    parser.addOption(socketOpt);
    parser.process(app);

    const QByteArray stdinPayload = readAllStdin();
    QJsonParseError perr{};
    const QJsonDocument inDoc = QJsonDocument::fromJson(stdinPayload, &perr);
    if (perr.error != QJsonParseError::NoError || !inDoc.isObject()) {
        // Malformed hook input — let Claude proceed; we have no useful gate.
        emitDecision(QStringLiteral("allow"),
                     QStringLiteral("hook bridge: malformed stdin"));
        return 0;
    }

    const QJsonObject hookIn = inDoc.object();
    const QString toolName = hookIn.value(QStringLiteral("tool_name")).toString();
    const QJsonObject toolInput = hookIn.value(QStringLiteral("tool_input")).toObject();
    const QString cwd = hookIn.value(QStringLiteral("cwd")).toString();

    const QString socketPath = parser.value(socketOpt);
    if (socketPath.isEmpty()) {
        emitDecision(QStringLiteral("allow"),
                     QStringLiteral("hook bridge: no socket configured"));
        return 0;
    }

    QLocalSocket sock;
    sock.connectToServer(socketPath);
    if (!sock.waitForConnected(2000)) {
        emitDecision(QStringLiteral("allow"),
                     QStringLiteral("hook bridge: plugin not reachable"));
        return 0;
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QJsonObject req;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("tool_name"), toolName);
    req.insert(QStringLiteral("tool_input"), toolInput);
    req.insert(QStringLiteral("cwd"), cwd);

    QByteArray frame = QJsonDocument(req).toJson(QJsonDocument::Compact);
    frame.append('\n');
    sock.write(frame);
    sock.flush();

    // Read until we see a complete line whose JSON id matches ours. The
    // plugin only ever writes one frame per request, so this is just a
    // single readyRead in practice — but loop so we tolerate fragmentation.
    QByteArray inbuf;
    QString decision;
    QString reason;
    bool got = false;
    // Hard cap so we don't hang forever if the user never answers. 10
    // minutes is generous for an interactive prompt; past that, allow and
    // let the post-hoc card take over.
    const int timeoutMs = 10 * 60 * 1000;
    QElapsedTimer t;
    t.start();
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
            QJsonParseError lerr{};
            const QJsonDocument ldoc = QJsonDocument::fromJson(line, &lerr);
            if (lerr.error != QJsonParseError::NoError || !ldoc.isObject())
                continue;
            const QJsonObject obj = ldoc.object();
            if (obj.value(QStringLiteral("id")).toString() != id)
                continue;
            decision = obj.value(QStringLiteral("decision")).toString();
            reason   = obj.value(QStringLiteral("reason")).toString();
            got = true;
            break;
        }
    }

    sock.disconnectFromServer();

    if (!got) {
        emitDecision(QStringLiteral("allow"),
                     QStringLiteral("hook bridge: timeout / no response"));
        return 0;
    }

    if (decision != QStringLiteral("allow")
        && decision != QStringLiteral("deny")
        && decision != QStringLiteral("ask"))
        decision = QStringLiteral("allow");
    emitDecision(decision, reason);
    return 0;
}
