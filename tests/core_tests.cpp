#include "backup_file.h"
#include "three_x_ui_api.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QUuid>
#include <memory>

using namespace fleet;

namespace {
QJsonObject envelope(const QJsonValue& value = QJsonValue()) {
    return {{QStringLiteral("success"), true}, {QStringLiteral("obj"), value}};
}

class MockPanel : public QObject {
  public:
    struct Request {
        QByteArray method;
        QByteArray target;
        QMap<QByteArray, QByteArray> headers;
        QByteArray body;
    };
    struct Response {
        int status;
        QByteArray body;
        QMap<QByteArray, QByteArray> headers;
        std::function<QByteArray(const Request&)> bodyFactory;
    };
    QTcpServer server;
    QList<Request> requests;
    QList<Response> responses;

    MockPanel() {
        if (!server.listen(QHostAddress::LocalHost, 0))
            qFatal("Cannot start loopback HTTP test server");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled] {
                    buffer->append(socket->readAll());
                    if (*handled)
                        return;
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
                    if (lines.isEmpty())
                        return;
                    const auto start = lines.first().trimmed().split(' ');
                    if (start.size() < 2)
                        return;
                    Request request;
                    request.method = start.at(0);
                    request.target = start.at(1);
                    for (qsizetype i = 1; i < lines.size(); ++i) {
                        const int colon = lines.at(i).indexOf(':');
                        if (colon > 0)
                            request.headers.insert(lines.at(i).left(colon).trimmed().toLower(),
                                                   lines.at(i).mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer->size() < headerEnd + 4 + length)
                        return;
                    request.body = buffer->mid(headerEnd + 4, length);
                    requests.append(request);
                    *handled = true;
                    Response response = responses.isEmpty() ? Response{500, QByteArray("{}"), {}}
                                                            : responses.takeFirst();
                    if (response.status == 0)
                        return; // Deliberately unanswered request.
                    if (response.status < 0) {
                        socket->disconnectFromHost();
                        return;
                    }
                    if (response.bodyFactory)
                        response.body = response.bodyFactory(request);
                    QByteArray message =
                        "HTTP/1.1 " + QByteArray::number(response.status) +
                        " Test\r\nConnection: close\r\nContent-Length: " +
                        response.headers.value("Content-Length",
                                               QByteArray::number(response.body.size())) +
                        "\r\n";
                    message += "Content-Type: " +
                               response.headers.value("Content-Type", "application/json") + "\r\n";
                    for (auto it = response.headers.constBegin(); it != response.headers.constEnd();
                         ++it)
                        if (it.key() != "Content-Length" && it.key() != "Content-Type")
                            message += it.key() + ": " + it.value() + "\r\n";
                    message += "\r\n";
                    message += response.body;
                    socket->write(message);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    void add(int status, const QJsonObject& value, QMap<QByteArray, QByteArray> headers = {}) {
        responses.append(
            {status, QJsonDocument(value).toJson(QJsonDocument::Compact), std::move(headers)});
    }
    void hold() { responses.append({0, {}, {}}); }
    void reflectCreatedClient(bool enabled) {
        responses.append(
            {200, {}, {}, [this, enabled](const Request&) {
                 QJsonObject created;
                 QJsonObject record;
                 for (const auto& previous : requests) {
                     if (previous.method != "POST")
                         continue;
                     const auto body = QJsonDocument::fromJson(previous.body).object();
                     if (previous.target.endsWith("/clients/add")) {
                         created = body;
                         record = body.value(QStringLiteral("client")).toObject();
                     } else if (previous.target.contains("/clients/update/"))
                         record = body;
                 }
                 const QString uuid = record.value(QStringLiteral("id")).toString();
                 record.insert(QStringLiteral("uuid"), uuid);
                 record.insert(QStringLiteral("id"), 42);
                 record.insert(QStringLiteral("enable"), enabled);
                 record.insert(QStringLiteral("inboundIds"),
                               created.value(QStringLiteral("inboundIds")));
                 return QJsonDocument(
                            envelope(QJsonObject{{QStringLiteral("client"), record},
                                                 {QStringLiteral("inboundIds"),
                                                  created.value(QStringLiteral("inboundIds"))}}))
                     .toJson(QJsonDocument::Compact);
             }});
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/secret/base/").arg(server.serverPort()));
    }
};

ServerConfig configuration(MockPanel& mock, ApiFlavor flavor = ApiFlavor::ModernV3) {
    ServerConfig config;
    config.id = QStringLiteral("server-one");
    config.name = QStringLiteral("Test");
    config.panelUrl = mock.url();
    config.allowHttp = true;
    config.auth = AuthKind::Token;
    config.token = QStringLiteral("test-token");
    config.api = flavor;
    return config;
}

QJsonObject inbound(int id = 1, const QString& protocol = QStringLiteral("vless")) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("remark"), QStringLiteral("Inbound")},
            {QStringLiteral("protocol"), protocol},
            {QStringLiteral("port"), 443},
            {QStringLiteral("enable"), true}};
}

QJsonObject modernRecord() {
    return {{QStringLiteral("id"), 42},
            {QStringLiteral("uuid"), QStringLiteral("11111111-2222-4333-8444-555555555555")},
            {QStringLiteral("email"), QStringLiteral("alice+tag@example.com")},
            {QStringLiteral("password"), QStringLiteral("test-password")},
            {QStringLiteral("inboundIds"), QJsonArray{1, 2}},
            {QStringLiteral("enable"), true},
            {QStringLiteral("totalGB"), qint64(5 * 1024 * 1024 * 1024LL)},
            {QStringLiteral("expiryTime"), qint64(-86400000)},
            {QStringLiteral("subId"), QStringLiteral("existing-sub-id")},
            {QStringLiteral("flow"), QStringLiteral("xtls-rprx-vision")},
            {QStringLiteral("security"), QStringLiteral("auto")},
            {QStringLiteral("limitHwid"), 7},
            {QStringLiteral("allowedIPs"), QString()},
            {QStringLiteral("reverse"), QJsonValue::Null},
            {QStringLiteral("limitIp"), 3},
            {QStringLiteral("comment"), QStringLiteral("old comment")},
            {QStringLiteral("opaque"), QJsonObject{{QStringLiteral("tier"), 3}}},
            {QStringLiteral("traffic"),
             QJsonObject{{QStringLiteral("up"), 100}, {QStringLiteral("down"), 200}}}};
}

Client selectedModern() {
    return ThreeXUiApi::parseInventory(QJsonArray{inbound(1), inbound(2)},
                                       QJsonArray{modernRecord()}, QStringLiteral("server-one"),
                                       true)
        .value.clients.first();
}

QJsonObject freshResponse(QJsonObject record = modernRecord()) {
    return {{QStringLiteral("client"), record},
            {QStringLiteral("inboundIds"), record.value(QStringLiteral("inboundIds"))},
            {QStringLiteral("usedTraffic"), 300}};
}

QJsonObject legacyRecord() {
    return {{QStringLiteral("id"), QStringLiteral("legacy+uuid/identifier")},
            {QStringLiteral("email"), QStringLiteral("plus+label@example.com")},
            {QStringLiteral("subId"), QStringLiteral("legacy-sub")},
            {QStringLiteral("enable"), true},
            {QStringLiteral("totalGB"), qint64(1024)},
            {QStringLiteral("expiryTime"), qint64(0)},
            {QStringLiteral("flow"), QStringLiteral("xtls-rprx-vision")},
            {QStringLiteral("customField"), QJsonObject{{QStringLiteral("keep"), true}}}};
}

QJsonObject legacyInbound(QJsonObject client = legacyRecord()) {
    QJsonObject result = inbound();
    result.insert(QStringLiteral("settings"),
                  QString::fromUtf8(
                      QJsonDocument(QJsonObject{{QStringLiteral("clients"), QJsonArray{client}}})
                          .toJson(QJsonDocument::Compact)));
    result.insert(
        QStringLiteral("clientStats"),
        QJsonArray{QJsonObject{{QStringLiteral("email"), client.value(QStringLiteral("email"))},
                               {QStringLiteral("up"), 11},
                               {QStringLiteral("down"), 22}}});
    return result;
}

QJsonObject formClient(const QByteArray& body) {
    const QUrlQuery query(QString::fromUtf8(body));
    const QByteArray settings =
        query.queryItemValue(QStringLiteral("settings"), QUrl::FullyDecoded).toUtf8();
    return QJsonDocument::fromJson(settings)
        .object()
        .value(QStringLiteral("clients"))
        .toArray()
        .first()
        .toObject();
}

ClientDraft sharedDraft() {
    const auto raw = modernRecord();
    ClientDraft draft;
    draft.inboundIds = {1, 2};
    draft.email = raw.value(QStringLiteral("email")).toString();
    draft.clientId = raw.value(QStringLiteral("uuid")).toString();
    draft.password = raw.value(QStringLiteral("password")).toString();
    draft.subId = raw.value(QStringLiteral("subId")).toString();
    draft.totalBytes = raw.value(QStringLiteral("totalGB")).toInteger();
    draft.expiryTime = raw.value(QStringLiteral("expiryTime")).toInteger();
    draft.enable = true;
    draft.flow = raw.value(QStringLiteral("flow")).toString();
    return draft;
}

QByteArray sqliteFixture() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("panel.db"));
    const QString connection = QStringLiteral("backup-fixture-") + QUuid::createUuid().toString();
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Cannot create SQLite backup fixture");
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE inbounds (id INTEGER PRIMARY KEY, protocol TEXT, settings TEXT)")) ||
            !query.exec(
                QStringLiteral("CREATE TABLE clients (id INTEGER PRIMARY KEY, email TEXT)")) ||
            !query.exec(QStringLiteral(
                "CREATE TABLE settings (id INTEGER PRIMARY KEY, key TEXT, value TEXT)")) ||
            !query.exec(QStringLiteral(
                "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT, password TEXT)")))
            qFatal("Cannot initialize SQLite backup fixture");
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read SQLite backup fixture");
    return file.readAll();
}
} // namespace

class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void statusMissingAndZeroFields() {
        const auto missing = ThreeXUiApi::parseStatus({}, QStringLiteral("s"));
        QVERIFY(missing.ok);
        QVERIFY(!missing.value.cpu);
        QVERIFY(!missing.value.memoryPercent);
        QVERIFY(!missing.value.diskPercent);
        QVERIFY(!missing.value.rxBps);
        QVERIFY(!missing.value.receivedBytes);
        QVERIFY(!missing.value.online);
        QCOMPARE(missing.value.health, Health::Unknown);
        const QJsonObject value{
            {QStringLiteral("cpu"), 0},
            {QStringLiteral("mem"),
             QJsonObject{{QStringLiteral("current"), 0}, {QStringLiteral("total"), 100}}},
            {QStringLiteral("disk"),
             QJsonObject{{QStringLiteral("current"), 0}, {QStringLiteral("total"), 0}}},
            {QStringLiteral("netIO"),
             QJsonObject{{QStringLiteral("up"), 0}, {QStringLiteral("down"), 0}}},
            {QStringLiteral("netTraffic"),
             QJsonObject{{QStringLiteral("sent"), 0}, {QStringLiteral("recv"), 0}}},
            {QStringLiteral("xray"),
             QJsonObject{{QStringLiteral("state"), QStringLiteral("running")},
                         {QStringLiteral("version"), QStringLiteral("25.3.6")}}},
            {QStringLiteral("uptime"), 5},
            {QStringLiteral("errorMsg"), QStringLiteral("do-not-log-secret")}};
        const auto result = ThreeXUiApi::parseStatus(value, QStringLiteral("s"));
        QVERIFY(result.ok);
        QCOMPARE(result.value.serverId, QStringLiteral("s"));
        QCOMPARE(result.value.health, Health::Online);
        QVERIFY(result.value.cpu);
        QCOMPARE(*result.value.cpu, 0.0);
        QVERIFY(result.value.memoryPercent);
        QCOMPARE(*result.value.memoryPercent, 0.0);
        QVERIFY(!result.value.diskPercent);
        QVERIFY(result.value.rxBps);
        QCOMPARE(*result.value.rxBps, 0.0);
        QVERIFY(result.value.receivedBytes);
        QCOMPARE(*result.value.receivedBytes, qint64(0));
        QCOMPARE(result.value.uptimeSeconds, qint64(5));
        QVERIFY(result.value.error.isEmpty());
        const auto rejected =
            ThreeXUiApi::parseStatus({{QStringLiteral("success"), false},
                                      {QStringLiteral("msg"), QStringLiteral("do-not-log-secret")}},
                                     QStringLiteral("s"));
        QVERIFY(!rejected.ok);
        QVERIFY(!rejected.error.contains(QStringLiteral("do-not-log-secret")));
        const auto invalid = ThreeXUiApi::parseStatus(
            {{QStringLiteral("cpu"), -1},
             {QStringLiteral("netIO"), QJsonObject{{QStringLiteral("down"), -2}}},
             {QStringLiteral("xray"),
              QJsonObject{{QStringLiteral("state"), QStringLiteral("error")}}}},
            QStringLiteral("s"));
        QVERIFY(!invalid.value.cpu);
        QVERIFY(!invalid.value.receivedBytes);
        QCOMPARE(invalid.value.health, Health::Warning);
    }

    void urlValidationAndPath() {
        ServerConfig config;
        config.panelUrl = QUrl(QStringLiteral("https://host.test:8443/secret%20path/"));
        auto valid = ThreeXUiApi::validatePanelUrl(config);
        QVERIFY(valid.ok);
        QCOMPARE(valid.value.path(QUrl::FullyEncoded), QStringLiteral("/secret%20path/"));
        config.panelUrl = QUrl(QStringLiteral("https://host.test/base"));
        QCOMPARE(ThreeXUiApi::validatePanelUrl(config).value.path(), QStringLiteral("/base/"));
        config.panelUrl = QUrl(QStringLiteral("http://127.0.0.1:8090/base/"));
        QVERIFY(!ThreeXUiApi::validatePanelUrl(config).ok);
        config.allowHttp = true;
        QVERIFY(ThreeXUiApi::validatePanelUrl(config).ok);
        for (const QString& invalid :
             {QStringLiteral("https://user:password@host.test/"),
              QStringLiteral("https://host.test/?token=x"),
              QStringLiteral("https://host.test/#frag"), QStringLiteral("file:///local/file"),
              QStringLiteral("https:///missing-host")}) {
            config.panelUrl = QUrl(invalid);
            QVERIFY2(!ThreeXUiApi::validatePanelUrl(config).ok, qPrintable(invalid));
        }
    }

    void legacyInventoryPreservesFields() {
        const auto parsed = ThreeXUiApi::parseInventory(QJsonArray{legacyInbound()}, {},
                                                        QStringLiteral("s"), false);
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.value.clients.size(), 1);
        const auto& client = parsed.value.clients.first();
        QCOMPARE(client.id, QStringLiteral("legacy+uuid/identifier"));
        QCOMPARE(client.usedBytes, qint64(33));
        QCOMPARE(client.totalBytes, qint64(1024));
        QCOMPARE(client.inboundIds, QList<int>{1});
        QVERIFY(client.raw.value(QStringLiteral("customField"))
                    .toObject()
                    .value(QStringLiteral("keep"))
                    .toBool());
        auto bad = inbound();
        bad.insert(QStringLiteral("settings"), QStringLiteral("{invalid"));
        QVERIFY(!ThreeXUiApi::parseInventory(QJsonArray{bad}, {}, QStringLiteral("s"), false).ok);
        QJsonObject trojan = legacyRecord();
        trojan.remove(QStringLiteral("id"));
        trojan.insert(QStringLiteral("password"), QStringLiteral("trojan-test-password"));
        auto trojanInbound = legacyInbound(trojan);
        trojanInbound.insert(QStringLiteral("protocol"), QStringLiteral("trojan"));
        const auto parsedTrojan =
            ThreeXUiApi::parseInventory(QJsonArray{trojanInbound}, {}, QStringLiteral("s"), false);
        QVERIFY(parsedTrojan.ok);
        QCOMPARE(parsedTrojan.value.clients.first().id, QStringLiteral("trojan-test-password"));
    }

    void modernNumericIdIsNeverAccessId() {
        const auto parsed =
            ThreeXUiApi::parseInventory(QJsonArray{inbound(1), inbound(2)},
                                        QJsonArray{modernRecord()}, QStringLiteral("s"), true);
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.value.clients.size(), 1);
        const auto& client = parsed.value.clients.first();
        QCOMPARE(client.id, modernRecord().value(QStringLiteral("uuid")).toString());
        QCOMPARE(client.raw.value(QStringLiteral("id")).toInt(), 42);
        QCOMPARE(client.inboundIds, (QList<int>{1, 2}));
        QCOMPARE(client.totalBytes, qint64(5 * 1024 * 1024 * 1024LL));
        QCOMPARE(client.expiryTime, qint64(-86400000));
        QCOMPARE(client.usedBytes, qint64(300));
        auto numericOnly = modernRecord();
        numericOnly.remove(QStringLiteral("uuid"));
        numericOnly.remove(QStringLiteral("password"));
        QVERIFY(!ThreeXUiApi::parseInventory(QJsonArray{inbound()}, QJsonArray{numericOnly},
                                             QStringLiteral("s"), true)
                     .ok);
    }

    void modernTokenCrudPreservesFreshOpaqueFields() {
        MockPanel mock;
        auto config = configuration(mock);
        ThreeXUiApi api(config);
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{modernRecord()}));
        Outcome<Inventory> inventory;
        bool done = false;
        api.fetchInventory([&](Outcome<Inventory> result) {
            inventory = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(inventory.ok, qPrintable(inventory.error));
        const Client client = inventory.value.clients.first();
        auto fresh = modernRecord();
        fresh.insert(QStringLiteral("comment"), QStringLiteral("changed on remote"));
        mock.add(200, envelope(freshResponse(fresh)));
        mock.add(200, envelope());
        ClientPatch patch;
        patch.totalBytes = qint64(8192);
        patch.enable = false;
        Outcome<bool> outcome;
        done = false;
        api.updateClient(client, patch, [&](Outcome<bool> result) {
            outcome = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(mock.requests.size(), 4);
        const auto& update = mock.requests.at(3);
        QCOMPARE(update.target,
                 QByteArray("/secret/base/panel/api/clients/update/alice%2Btag%40example.com"));
        QCOMPARE(update.headers.value("authorization"), QByteArray("Bearer test-token"));
        QCOMPARE(update.headers.value("x-requested-with"), QByteArray("XMLHttpRequest"));
        const auto body = QJsonDocument::fromJson(update.body).object();
        QCOMPARE(body.value(QStringLiteral("id")).toString(), client.id);
        QCOMPARE(body.value(QStringLiteral("uuid")), fresh.value(QStringLiteral("uuid")));
        QCOMPARE(body.value(QStringLiteral("password")), fresh.value(QStringLiteral("password")));
        QCOMPARE(body.value(QStringLiteral("subId")), fresh.value(QStringLiteral("subId")));
        QCOMPARE(body.value(QStringLiteral("flow")), fresh.value(QStringLiteral("flow")));
        QCOMPARE(body.value(QStringLiteral("security")), fresh.value(QStringLiteral("security")));
        QCOMPARE(body.value(QStringLiteral("limitHwid")).toInt(), 7);
        QVERIFY(body.value(QStringLiteral("allowedIPs")).isArray());
        QVERIFY(body.value(QStringLiteral("allowedIPs")).toArray().isEmpty());
        QVERIFY(body.value(QStringLiteral("reverse")).isNull());
        QCOMPARE(body.value(QStringLiteral("opaque")), fresh.value(QStringLiteral("opaque")));
        QCOMPARE(body.value(QStringLiteral("comment")).toString(),
                 QStringLiteral("changed on remote"));
        QCOMPARE(body.value(QStringLiteral("totalGB")).toInteger(), qint64(8192));
        QVERIFY(!body.value(QStringLiteral("enable")).toBool());
        QVERIFY(!body.contains(QStringLiteral("inboundIds")));
        for (int operation = 0; operation < 2; ++operation) {
            mock.add(200, envelope(freshResponse()));
            mock.add(200, envelope());
            done = false;
            auto callback = [&](Outcome<bool> result) {
                outcome = std::move(result);
                done = true;
            };
            if (operation == 0)
                api.deleteClient(client, callback);
            else
                api.resetClientTraffic(client, callback);
            QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
            QVERIFY2(outcome.ok, qPrintable(outcome.error));
        }
        QCOMPARE(mock.requests.size(), 8);
        QCOMPARE(mock.requests.at(5).target,
                 QByteArray("/secret/base/panel/api/clients/del/alice%2Btag%40example.com"));
        QCOMPARE(
            mock.requests.at(7).target,
            QByteArray("/secret/base/panel/api/clients/resetTraffic/alice%2Btag%40example.com"));
        QVERIFY(mock.responses.isEmpty());
    }

    void modernStaleIdentityBlocksMutation_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<int>("operation");
        QTest::newRow("uuid-update") << QStringLiteral("uuid") << 0;
        QTest::newRow("password-delete") << QStringLiteral("password") << 1;
        QTest::newRow("inbounds-reset") << QStringLiteral("inboundIds") << 2;
        QTest::newRow("email-update") << QStringLiteral("email") << 0;
    }

    void modernModelConvertsCsvAndSerializedReverse() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        auto fresh = modernRecord();
        fresh.insert(QStringLiteral("allowedIPs"), QStringLiteral("10.0.0.1, 2001:db8::1,, "));
        const QJsonObject reverse{{QStringLiteral("tag"), QStringLiteral("reverse-test")},
                                  {QStringLiteral("domain"), QStringLiteral("example.test")}};
        fresh.insert(QStringLiteral("reverse"),
                     QString::fromUtf8(QJsonDocument(reverse).toJson(QJsonDocument::Compact)));
        mock.add(200, envelope(freshResponse(fresh)));
        mock.add(200, envelope());
        bool done = false;
        Outcome<bool> result;
        ClientPatch patch;
        patch.enable = false;
        api.updateClient(selectedModern(), patch, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        const auto body = QJsonDocument::fromJson(mock.requests.last().body).object();
        QCOMPARE(body.value(QStringLiteral("allowedIPs")).toArray(),
                 (QJsonArray{QStringLiteral("10.0.0.1"), QStringLiteral("2001:db8::1")}));
        QCOMPARE(body.value(QStringLiteral("reverse")).toObject(), reverse);
        fresh.insert(QStringLiteral("reverse"), QStringLiteral("{invalid-json-secret"));
        mock.add(200, envelope(freshResponse(fresh)));
        done = false;
        api.updateClient(selectedModern(), patch, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.contains(QStringLiteral("invalid-json-secret")));
        QCOMPARE(mock.requests.size(), 3); // No POST with malformed schema.
    }

    void modernStaleIdentityBlocksMutation() {
        QFETCH(QString, field);
        QFETCH(int, operation);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        QJsonObject changed = modernRecord();
        if (field == QStringLiteral("inboundIds"))
            changed.insert(field, QJsonArray{1});
        else
            changed.insert(field, QStringLiteral("changed-value"));
        mock.add(200, envelope(freshResponse(changed)));
        bool done = false;
        Outcome<bool> outcome;
        auto callback = [&](Outcome<bool> result) {
            outcome = std::move(result);
            done = true;
        };
        if (operation == 0) {
            ClientPatch patch;
            patch.enable = false;
            api.updateClient(selectedModern(), patch, callback);
        } else if (operation == 1)
            api.deleteClient(selectedModern(), callback);
        else
            api.resetClientTraffic(selectedModern(), callback);
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!outcome.ok);
        QCOMPARE(mock.requests.size(), 1);
        QCOMPARE(mock.requests.first().method, QByteArray("GET"));
    }

    void mutationsRequireSuccessAndNeverRetry() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(freshResponse()));
        mock.add(200, {{QStringLiteral("success"), false},
                       {QStringLiteral("msg"), QStringLiteral("secret-must-not-escape")}});
        bool done = false;
        Outcome<bool> outcome;
        ClientPatch patch;
        patch.enable = false;
        api.updateClient(selectedModern(), patch, [&](Outcome<bool> result) {
            outcome = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!outcome.ok);
        QVERIFY(!outcome.error.contains(QStringLiteral("secret-must-not-escape")));
        QTest::qWait(30);
        QCOMPARE(mock.requests.size(), 2);
        mock.add(200, {});
        done = false;
        api.serverAction(ServerAction::RestartXray, {}, [&](Outcome<bool> result) {
            outcome = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!outcome.ok);
        QCOMPARE(mock.requests.size(), 3);
    }

    void discoveryFallsBackOnlyOn404Or405_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("fallback");
        QTest::newRow("404") << 404 << true;
        QTest::newRow("405") << 405 << true;
        QTest::newRow("401") << 401 << false;
        QTest::newRow("503") << 503 << false;
        QTest::newRow("success-false") << 200 << false;
    }

    void discoveryFallsBackOnlyOn404Or405() {
        QFETCH(int, status);
        QFETCH(bool, fallback);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        mock.add(status, {{QStringLiteral("success"), false},
                          {QStringLiteral("msg"), QStringLiteral("secret")}});
        if (fallback)
            mock.add(200, envelope(QJsonArray{}));
        bool done = false;
        Outcome<Inventory> result;
        api.fetchInventory([&](Outcome<Inventory> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QCOMPARE(result.ok, fallback);
        QCOMPARE(api.detectedFlavor(), fallback ? ApiFlavor::LegacyV2 : ApiFlavor::Auto);
        QCOMPARE(mock.requests.size(), fallback ? 2 : 1);
    }

    void autoModernDiscoveryIsCached() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        mock.add(200, envelope(QJsonArray{modernRecord()}));
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{modernRecord()}));
        bool done = false;
        Outcome<Inventory> inventory;
        api.fetchInventory([&](Outcome<Inventory> result) {
            inventory = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(inventory.ok);
        QCOMPARE(api.detectedFlavor(), ApiFlavor::ModernV3);
        mock.add(200, envelope(QJsonObject{{QStringLiteral("cpu"), 5}}));
        mock.add(200, envelope(QJsonArray{QStringLiteral("one"), QStringLiteral("two")}));
        Outcome<Snapshot> status;
        done = false;
        api.fetchStatus([&](Outcome<Snapshot> result) {
            status = std::move(result);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(status.ok);
        QVERIFY(status.value.online);
        QCOMPARE(*status.value.online, 2);
        QCOMPARE(mock.requests.size(), 5);
        QCOMPARE(mock.requests.at(4).target, QByteArray("/secret/base/panel/api/clients/onlines"));
    }

    void legacyCsrfLoginCookieCreateAndUpdate() {
        MockPanel mock;
        auto config = configuration(mock, ApiFlavor::LegacyV2);
        config.auth = AuthKind::Password;
        config.username = QStringLiteral("test-user");
        config.password = QStringLiteral("test-password");
        config.twoFactorCode = QStringLiteral("123456");
        ThreeXUiApi api(config);
        mock.add(200, envelope(QStringLiteral("before-login")));
        mock.add(200, envelope(), {{"Set-Cookie", "session=test-session; Path=/; HttpOnly"}});
        mock.add(200, envelope(QStringLiteral("after-login")));
        mock.add(200, envelope(QJsonArray{legacyInbound()}));
        mock.add(200, envelope());
        ClientDraft draft;
        draft.inboundIds = {1};
        draft.email = QStringLiteral("new+client@example.com");
        draft.totalBytes = 1234;
        draft.flow = QStringLiteral("xtls-rprx-vision");
        Outcome<bool> result;
        bool done = false;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 5);
        QCOMPARE(mock.requests.at(0).target, QByteArray("/secret/base/csrf-token"));
        const auto login = QJsonDocument::fromJson(mock.requests.at(1).body).object();
        QCOMPARE(login.value(QStringLiteral("twoFactorCode")).toString(), QStringLiteral("123456"));
        QCOMPARE(mock.requests.at(1).headers.value("x-csrf-token"), QByteArray("before-login"));
        QVERIFY(mock.requests.at(2).headers.value("cookie").contains("session=test-session"));
        QVERIFY(api.config().twoFactorCode.isEmpty());
        const auto& add = mock.requests.at(4);
        QCOMPARE(add.target, QByteArray("/secret/base/panel/api/inbounds/addClient"));
        QCOMPARE(add.headers.value("x-csrf-token"), QByteArray("after-login"));
        QVERIFY(add.headers.value("cookie").contains("session=test-session"));
        QVERIFY(!add.headers.contains("authorization"));
        const auto created = formClient(add.body);
        QCOMPARE(created.value(QStringLiteral("email")).toString(), draft.email);
        QCOMPARE(created.value(QStringLiteral("totalGB")).toInteger(), qint64(1234));
        QCOMPARE(created.value(QStringLiteral("flow")).toString(), draft.flow);
        QCOMPARE(created.value(QStringLiteral("subId")).toString().size(), 24);
        QCOMPARE(created.value(QStringLiteral("id")).toString().size(), 36);
        auto fresh = legacyRecord();
        fresh.insert(QStringLiteral("comment"), QStringLiteral("fresh remote value"));
        mock.add(200, envelope(QJsonArray{legacyInbound(fresh)}));
        mock.add(200, envelope());
        const Client selected =
            ThreeXUiApi::parseInventory(QJsonArray{legacyInbound()}, {}, config.id, false)
                .value.clients.first();
        ClientPatch patch;
        patch.enable = false;
        patch.totalBytes = 9876;
        done = false;
        api.updateClient(selected, patch, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 7);
        QCOMPARE(
            mock.requests.at(6).target,
            QByteArray("/secret/base/panel/api/inbounds/updateClient/legacy%2Buuid%2Fidentifier"));
        const auto updated = formClient(mock.requests.at(6).body);
        QCOMPARE(updated.value(QStringLiteral("email")), fresh.value(QStringLiteral("email")));
        QCOMPARE(updated.value(QStringLiteral("subId")), fresh.value(QStringLiteral("subId")));
        QCOMPARE(updated.value(QStringLiteral("customField")),
                 fresh.value(QStringLiteral("customField")));
        QCOMPARE(updated.value(QStringLiteral("comment")).toString(),
                 QStringLiteral("fresh remote value"));
        QCOMPARE(updated.value(QStringLiteral("totalGB")).toInteger(), qint64(9876));
        QVERIFY(!updated.value(QStringLiteral("enable")).toBool());
    }

    void csrf404FallbackOnly_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("old-v2-404") << 404 << true;
        QTest::newRow("405-is-not-old-v2") << 405 << false;
        QTest::newRow("503-is-not-old-v2") << 503 << false;
    }

    void csrf404FallbackOnly() {
        QFETCH(int, status);
        QFETCH(bool, allowed);
        MockPanel mock;
        auto config = configuration(mock, ApiFlavor::LegacyV2);
        config.auth = AuthKind::Password;
        config.username = QStringLiteral("user");
        config.password = QStringLiteral("password");
        ThreeXUiApi api(config);
        mock.add(status, {});
        if (allowed) {
            mock.add(200, envelope());
            mock.add(404, {});
            mock.add(200, envelope(QJsonArray{}));
        }
        bool done = false;
        Outcome<Inventory> result;
        api.fetchInventory([&](Outcome<Inventory> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QCOMPARE(result.ok, allowed);
        QCOMPARE(mock.requests.size(), allowed ? 4 : 1);
        if (allowed)
            QVERIFY(!mock.requests.at(1).headers.contains("x-csrf-token"));
    }

    void legacyStatusFallbackAndOnlineFailureKeepMetrics() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::LegacyV2));
        mock.add(404, {});
        mock.add(200,
                 envelope(QJsonObject{
                     {QStringLiteral("cpu"), 0},
                     {QStringLiteral("netTraffic"),
                      QJsonObject{{QStringLiteral("recv"), 123}, {QStringLiteral("sent"), 456}}},
                     {QStringLiteral("xray"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("running")}}}}));
        mock.add(404, {});
        bool done = false;
        Outcome<Snapshot> result;
        api.fetchStatus([&](Outcome<Snapshot> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.value.cpu);
        QCOMPARE(*result.value.cpu, 0.0);
        QVERIFY(result.value.rxBps);
        QCOMPARE(*result.value.rxBps, 123.0);
        QVERIFY(!result.value.online);
        QVERIFY(result.value.latencyMs);
        QCOMPARE(result.value.health, Health::Online);
        QCOMPARE(mock.requests.size(), 3);
        QCOMPARE(mock.requests.at(1).target, QByteArray("/secret/base/panel/server/status"));
        QCOMPARE(mock.requests.at(2).target, QByteArray("/secret/base/panel/api/inbounds/onlines"));
    }

    void monitorOnlyTokenRetainsMetricsAfterDiscovery403() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        mock.add(200, envelope(QJsonObject{
                          {QStringLiteral("cpu"), 12},
                          {QStringLiteral("xray"),
                           QJsonObject{{QStringLiteral("state"), QStringLiteral("running")}}}}));
        mock.add(403, {{QStringLiteral("success"), false},
                       {QStringLiteral("msg"), QStringLiteral("secret")}});
        bool done = false;
        Outcome<Snapshot> result;
        api.fetchStatus([&](Outcome<Snapshot> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.value.cpu);
        QCOMPARE(*result.value.cpu, 12.0);
        QCOMPARE(result.value.health, Health::Online);
        QVERIFY(!result.value.online);
        QCOMPARE(mock.requests.size(), 2);
        QCOMPARE(mock.requests.first().target, QByteArray("/secret/base/panel/api/server/status"));
        QCOMPARE(mock.requests.last().target, QByteArray("/secret/base/panel/api/clients/list"));
    }

    void loginRequiresExplicitSuccess() {
        MockPanel mock;
        auto config = configuration(mock, ApiFlavor::LegacyV2);
        config.auth = AuthKind::Password;
        config.username = QStringLiteral("user");
        config.password = QStringLiteral("password");
        ThreeXUiApi api(config);
        mock.add(200, envelope(QStringLiteral("csrf")));
        mock.add(200, {});
        bool done = false;
        Outcome<Inventory> result;
        api.fetchInventory([&](Outcome<Inventory> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 2);
    }

    void redirectsAreNeverFollowed() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(302, {}, {{"Location", mock.url().toEncoded() + "redirected"}});
        bool done = false;
        Outcome<Snapshot> result;
        api.fetchStatus([&](Outcome<Snapshot> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 1);
    }

    void actionVersionValidationAndEndpoints() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        bool done = false;
        Outcome<bool> result;
        api.serverAction(ServerAction::InstallXray, QStringLiteral("1.2.3;cmd"),
                         [&](Outcome<bool> value) {
                             result = std::move(value);
                             done = true;
                         });
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(mock.requests.isEmpty());
        mock.add(200, envelope());
        done = false;
        api.serverAction(ServerAction::InstallXray, QStringLiteral("v25.3.6"),
                         [&](Outcome<bool> value) {
                             result = std::move(value);
                             done = true;
                         });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(result.ok);
        QCOMPARE(mock.requests.first().target,
                 QByteArray("/secret/base/panel/api/server/installXray/v25.3.6"));
        mock.add(200, envelope());
        done = false;
        api.serverAction(ServerAction::UpdateGeofiles, {}, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(result.ok);
        QCOMPARE(mock.requests.last().target,
                 QByteArray("/secret/base/panel/api/server/updateGeofile"));
    }

    void unsupportedOrDisabledCreationDoesNotMutate() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(QJsonArray{inbound(1, QStringLiteral("shadowsocks"))}));
        ClientDraft draft;
        draft.inboundIds = {1};
        draft.email = QStringLiteral("new-client");
        bool done = false;
        Outcome<bool> result;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 1);
        auto disabled = inbound();
        disabled.insert(QStringLiteral("enable"), false);
        mock.add(200, envelope(QJsonArray{disabled}));
        done = false;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 2);
    }

    void modernCreateUsesProtocolCredentialsAndByteLimits_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("vless") << QStringLiteral("vless");
        QTest::newRow("vmess") << QStringLiteral("vmess");
        QTest::newRow("trojan") << QStringLiteral("trojan");
    }

    void modernCreateUsesProtocolCredentialsAndByteLimits() {
        QFETCH(QString, protocol);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(QJsonArray{inbound(3, protocol)}));
        mock.add(200, envelope(QJsonArray{}));
        mock.add(200, envelope());
        mock.reflectCreatedClient(true);
        mock.add(200, envelope());
        mock.reflectCreatedClient(false);
        ClientDraft draft;
        draft.inboundIds = {3};
        draft.email = QStringLiteral("created+client@example.com");
        draft.totalBytes = qint64(9 * 1024 * 1024 * 1024LL);
        draft.expiryTime = -86400000;
        draft.enable = false;
        if (protocol == QStringLiteral("vless"))
            draft.flow = QStringLiteral("xtls-rprx-vision");
        bool done = false;
        Outcome<bool> result;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 6);
        const auto& request = mock.requests.at(2);
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.target, QByteArray("/secret/base/panel/api/clients/add"));
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer test-token"));
        const auto body = QJsonDocument::fromJson(request.body).object();
        QCOMPARE(body.value(QStringLiteral("inboundIds")).toArray(), (QJsonArray{3}));
        const auto client = body.value(QStringLiteral("client")).toObject();
        QCOMPARE(client.value(QStringLiteral("email")).toString(), draft.email);
        QCOMPARE(client.value(QStringLiteral("totalGB")).toInteger(), draft.totalBytes);
        QCOMPARE(client.value(QStringLiteral("expiryTime")).toInteger(), draft.expiryTime);
        QCOMPARE(client.value(QStringLiteral("flow")).toString(), draft.flow);
        QVERIFY(!client.value(QStringLiteral("enable")).toBool());
        QCOMPARE(client.value(QStringLiteral("subId")).toString().size(), 24);
        QVERIFY(!QUuid(client.value(QStringLiteral("id")).toString()).isNull());
        if (protocol == QStringLiteral("trojan"))
            QCOMPARE(client.value(QStringLiteral("password")).toString().size(), 32);
        else
            QVERIFY(!client.contains(QStringLiteral("password")));
        if (protocol == QStringLiteral("vmess"))
            QCOMPARE(client.value(QStringLiteral("security")).toString(), QStringLiteral("auto"));
        QCOMPARE(client.value(QStringLiteral("limitIp")).toInt(), 0);
        QCOMPARE(client.value(QStringLiteral("limitHwid")).toInt(), 0);
    }

    void modernOrphanBindingsAreValidAndMalformedBindingsFail_data() {
        QTest::addColumn<QJsonValue>("bindings");
        QTest::addColumn<bool>("valid");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined) << true;
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << true;
        QTest::newRow("empty") << QJsonValue(QJsonArray{}) << true;
        QTest::newRow("string") << QJsonValue(QStringLiteral("1")) << false;
        QTest::newRow("null-entry") << QJsonValue(QJsonArray{QJsonValue::Null}) << false;
        QTest::newRow("zero") << QJsonValue(QJsonArray{0}) << false;
        QTest::newRow("negative") << QJsonValue(QJsonArray{-1}) << false;
        QTest::newRow("fraction") << QJsonValue(QJsonArray{1.5}) << false;
        QTest::newRow("malformed-after-valid")
            << QJsonValue(QJsonArray{1, QStringLiteral("2")}) << false;
    }

    void modernOrphanBindingsAreValidAndMalformedBindingsFail() {
        QFETCH(QJsonValue, bindings);
        QFETCH(bool, valid);
        auto raw = modernRecord();
        if (bindings.isUndefined())
            raw.remove(QStringLiteral("inboundIds"));
        else
            raw.insert(QStringLiteral("inboundIds"), bindings);
        const auto result =
            ThreeXUiApi::parseInventory({}, QJsonArray{raw}, QStringLiteral("server-one"), true);
        QCOMPARE(result.ok, valid);
        if (valid) {
            QCOMPARE(result.value.clients.size(), 1);
            QVERIFY(result.value.clients.first().inboundIds.isEmpty());
        }
    }

    void orphanDeleteRequiresFreshEmptyBindings() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        auto raw = modernRecord();
        raw.insert(QStringLiteral("inboundIds"), QJsonValue::Null);
        const Client orphan =
            ThreeXUiApi::parseInventory({}, QJsonArray{raw}, QStringLiteral("server-one"), true)
                .value.clients.first();
        mock.add(200, envelope(freshResponse(raw)));
        mock.add(200, envelope());
        bool done = false;
        Outcome<bool> result;
        api.deleteClient(orphan, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 2);
        QVERIFY(mock.requests.last().target.contains("/clients/del/"));
        raw.insert(QStringLiteral("inboundIds"), QJsonArray{1});
        mock.add(200, envelope(freshResponse(raw)));
        done = false;
        api.resetClientTraffic(orphan, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 3);
    }

    void mixedInboundsUseOneSuppliedIdentity() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2, QStringLiteral("vmess")),
                                          inbound(3, QStringLiteral("trojan"))}));
        mock.add(200, envelope(QJsonArray{}));
        mock.add(200, envelope());
        mock.reflectCreatedClient(true);
        auto draft = sharedDraft();
        draft.inboundIds = {1, 2, 3};
        draft.flow.clear();
        bool done = false;
        Outcome<bool> result;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 4);
        const auto body = QJsonDocument::fromJson(mock.requests.at(2).body).object();
        QCOMPARE(body.value(QStringLiteral("inboundIds")).toArray(), (QJsonArray{1, 2, 3}));
        const auto client = body.value(QStringLiteral("client")).toObject();
        QCOMPARE(client.value(QStringLiteral("id")).toString(), draft.clientId);
        QCOMPARE(client.value(QStringLiteral("password")).toString(), draft.password);
        QCOMPARE(client.value(QStringLiteral("subId")).toString(), draft.subId);
        QCOMPARE(client.value(QStringLiteral("security")).toString(), QStringLiteral("auto"));
    }

    void legacyRejectsMultipleInboundsBeforeWriting() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::LegacyV2));
        bool done = false;
        Outcome<bool> result;
        api.createClient(sharedDraft(), [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(mock.requests.isEmpty());
    }

    void exactSyncedIdentityIsIdempotentAndOnlyMissingBindingsAttach_data() {
        QTest::addColumn<bool>("missing");
        QTest::newRow("already-synced") << false;
        QTest::newRow("attach-one-missing") << true;
    }

    void exactSyncedIdentityIsIdempotentAndOnlyMissingBindingsAttach() {
        QFETCH(bool, missing);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        auto raw = modernRecord();
        if (missing)
            raw.insert(QStringLiteral("inboundIds"), QJsonArray{1});
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{raw}));
        mock.add(200, envelope(freshResponse(raw)));
        if (missing) {
            mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
            mock.add(200, envelope());
            mock.add(200, envelope(freshResponse()));
        }
        bool done = false;
        Outcome<bool> result;
        api.createClient(sharedDraft(), [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), missing ? 6 : 3);
        int writes = 0;
        for (const auto& request : mock.requests)
            if (request.method == "POST") {
                ++writes;
                QVERIFY(request.target.endsWith("/attach"));
                QCOMPARE(QJsonDocument::fromJson(request.body)
                             .object()
                             .value(QStringLiteral("inboundIds"))
                             .toArray(),
                         (QJsonArray{2}));
            }
        QCOMPARE(writes, missing ? 1 : 0);
    }

    void collisionsAndChangedFieldsNeverWrite_data() {
        QTest::addColumn<QString>("field");
        for (const auto& field : {"uuid", "password", "subId", "totalGB", "expiryTime", "enable",
                                  "flow", "inboundIds", "case"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }

    void collisionsAndChangedFieldsNeverWrite() {
        QFETCH(QString, field);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        auto raw = modernRecord();
        if (field == QStringLiteral("totalGB") || field == QStringLiteral("expiryTime"))
            raw.insert(field, 1);
        else if (field == QStringLiteral("enable"))
            raw.insert(field, false);
        else if (field == QStringLiteral("inboundIds"))
            raw.insert(field, QJsonArray{1, 2, 3});
        else if (field == QStringLiteral("case"))
            raw.insert(QStringLiteral("email"),
                       raw.value(QStringLiteral("email")).toString().toUpper());
        else
            raw.insert(field, QStringLiteral("different"));
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{raw}));
        if (field != QStringLiteral("case"))
            mock.add(200, envelope(freshResponse(raw)));
        bool done = false;
        Outcome<bool> result;
        api.createClient(sharedDraft(), [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        for (const auto& request : mock.requests)
            QCOMPARE(request.method, QByteArray("GET"));
    }

    void createdClientMismatchIsPartialFailureWithoutRollback() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{}));
        mock.add(200, envelope());
        auto changed = modernRecord();
        changed.insert(QStringLiteral("uuid"), QStringLiteral("foreign-identity"));
        mock.add(200, envelope(freshResponse(changed)));
        bool done = false;
        Outcome<bool> result;
        api.createClient(sharedDraft(), [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("Клиент создан")));
        QCOMPARE(mock.requests.size(), 4);
        QCOMPARE(mock.requests.at(2).method, QByteArray("POST"));
        QCOMPARE(mock.requests.last().method, QByteArray("GET"));
    }

    void disableFailureReportsCreatedStateWithoutRollback() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2)}));
        mock.add(200, envelope(QJsonArray{}));
        mock.add(200, envelope());
        mock.reflectCreatedClient(true);
        mock.add(200, {{QStringLiteral("success"), false},
                       {QStringLiteral("msg"), QStringLiteral("secret")}});
        auto draft = sharedDraft();
        draft.enable = false;
        bool done = false;
        Outcome<bool> result;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("Клиент создан")));
        QVERIFY(!result.error.contains(QStringLiteral("secret")));
        QCOMPARE(mock.requests.size(), 5);
        QVERIFY(mock.requests.last().target.contains("/clients/update/"));
    }

    void invalidPrecisionAndVisionMixedTargetsRejectWithoutWrite() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        auto draft = sharedDraft();
        draft.totalBytes = 9007199254740992LL;
        bool done = false;
        Outcome<bool> result;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(mock.requests.isEmpty());
        draft = sharedDraft();
        mock.add(200, envelope(QJsonArray{inbound(1), inbound(2, QStringLiteral("vmess"))}));
        done = false;
        api.createClient(draft, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QCOMPARE(mock.requests.size(), 1);
    }

    void downloadReturnsVerifiedBinaryAndUsesOwnPermission() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        const auto data = sqliteFixture();
        QVERIFY(inspectDatabaseBackup(data).ok);
        mock.responses.append({200, data, {{"Content-Type", "application/octet-stream"}}});
        bool done = false;
        Outcome<QByteArray> result;
        api.downloadDatabase([&](Outcome<QByteArray> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.value, data);
        QCOMPARE(mock.requests.size(), 1);
        QCOMPARE(mock.requests.first().target, QByteArray("/secret/base/panel/api/server/getDb"));
        QCOMPARE(mock.requests.first().headers.value("authorization"),
                 QByteArray("Bearer test-token"));
        QCOMPARE(api.detectedFlavor(), ApiFlavor::Auto);
    }

    void downloadRejectsHtmlJsonAndOversizeHeaders_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("oversized");
        QTest::newRow("html-login")
            << QByteArray("<!DOCTYPE html><html>secret-password</html>") << false;
        QTest::newRow("json-error")
            << QByteArray("{\"success\":false,\"msg\":\"secret-token\"}") << false;
        QTest::newRow("oversized") << QByteArray() << true;
    }

    void downloadRejectsHtmlJsonAndOversizeHeaders() {
        QFETCH(QByteArray, body);
        QFETCH(bool, oversized);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        QMap<QByteArray, QByteArray> headers;
        if (oversized)
            headers.insert("Content-Length", QByteArray::number(MaximumBackupBytes + 1));
        mock.responses.append({200, body, headers});
        bool done = false;
        Outcome<QByteArray> result;
        api.downloadDatabase([&](Outcome<QByteArray> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.contains(QStringLiteral("secret")));
        QCOMPARE(mock.requests.size(), 1);
        if (oversized)
            QVERIFY(result.error.contains(QStringLiteral("128")));
    }

    void restoreMultipartCarriesFileAndHostPolicy_data() {
        QTest::addColumn<bool>("keep");
        QTest::newRow("safe-host-settings") << true;
        QTest::newRow("full-clone") << false;
    }

    void restoreMultipartCarriesFileAndHostPolicy() {
        QFETCH(bool, keep);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        mock.add(200, envelope());
        const auto data = sqliteFixture();
        bool done = false;
        Outcome<bool> result;
        api.restoreDatabase(data, keep, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(mock.requests.size(), 1);
        const auto& request = mock.requests.first();
        QCOMPARE(request.target, QByteArray("/secret/base/panel/api/server/importDB"));
        QVERIFY(request.headers.value("content-type").startsWith("multipart/form-data; boundary="));
        QVERIFY(request.body.contains("name=\"db\"; filename=\"fleet.db\""));
        QVERIFY(request.body.contains(data));
        QVERIFY(request.body.contains("name=\"keepHostSettings\""));
        QVERIFY(request.body.contains(keep ? "\r\n\r\ntrue\r\n" : "\r\n\r\nfalse\r\n"));
    }

    void restoreRejectsUnconfirmedResponsesAndNeverRetries_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("success-false")
            << 200 << QByteArray("{\"success\":false,\"msg\":\"secret-token\"}");
        QTest::newRow("missing-success") << 200 << QByteArray("{}");
        QTest::newRow("invalid-json") << 200 << QByteArray("<html>secret-token</html>");
        QTest::newRow("forbidden") << 403 << QByteArray("{}");
        QTest::newRow("connection-drop") << -1 << QByteArray();
        QTest::newRow("redirect") << 302 << QByteArray("{}");
    }

    void restoreRejectsUnconfirmedResponsesAndNeverRetries() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.responses.append(
            {status, body, {{"Location", mock.url().toEncoded() + "redirected"}}});
        bool done = false;
        Outcome<bool> result;
        api.restoreDatabase(sqliteFixture(), true, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.contains(QStringLiteral("secret-token")));
        QTest::qWait(30);
        QCOMPARE(mock.requests.size(), 1);
        if (status == -1)
            QVERIFY(result.error.contains(QStringLiteral("неизвестен")));
    }

    void jsonMutationDropIsNotReplayedByTransport() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.responses.append({-1, {}, {}});
        bool done = false;
        Outcome<bool> result;
        api.serverAction(ServerAction::RestartXray, {}, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("неизвестен")));
        QTest::qWait(30);
        QCOMPARE(mock.requests.size(), 1);
        QCOMPARE(mock.requests.first().target,
                 QByteArray("/secret/base/panel/api/server/restartXrayService"));
    }

    void invalidRestoreDoesNotSendAndPendingRestoreCanBeDestroyed() {
        MockPanel mock;
        auto* api = new ThreeXUiApi(configuration(mock));
        bool done = false;
        Outcome<bool> result;
        api->restoreDatabase(QByteArray("not-a-database"), true, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(mock.requests.isEmpty());
        mock.hold();
        bool pendingCallback = false;
        api->restoreDatabase(sqliteFixture(), true, [&](Outcome<bool>) { pendingCallback = true; });
        QTRY_COMPARE_WITH_TIMEOUT(mock.requests.size(), 1, 3000);
        delete api;
        QTest::qWait(30);
        QVERIFY(!pendingCallback);
    }

    void backupCookieCsrfAndCallbackDestruction() {
        MockPanel mock;
        auto config = configuration(mock, ApiFlavor::Auto);
        config.auth = AuthKind::Password;
        config.username = QStringLiteral("user");
        config.password = QStringLiteral("password");
        auto* api = new ThreeXUiApi(config);
        mock.add(200, envelope(QStringLiteral("pre")));
        mock.add(200, envelope(), {{"Set-Cookie", "session=backup-session; Path=/; HttpOnly"}});
        mock.add(200, envelope(QStringLiteral("post")));
        mock.add(200, envelope());
        bool done = false;
        bool queued = false;
        api->restoreDatabase(sqliteFixture(), true, [&](Outcome<bool> value) {
            done = value.ok;
            delete api;
        });
        api->downloadDatabase([&](Outcome<QByteArray>) { queued = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QTest::qWait(30);
        QVERIFY(!queued);
        QCOMPARE(mock.requests.size(), 4);
        QCOMPARE(mock.requests.last().headers.value("x-csrf-token"), QByteArray("post"));
        QVERIFY(mock.requests.last().headers.value("cookie").contains("session=backup-session"));
    }

    void serverActionsUseOwnEndpointPermissions() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock, ApiFlavor::Auto));
        mock.add(200, envelope());
        bool done = false;
        Outcome<bool> result;
        api.serverAction(ServerAction::RestartXray, {}, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(result.ok);
        QCOMPARE(mock.requests.size(), 1);
        QCOMPARE(api.detectedFlavor(), ApiFlavor::Auto);
        QCOMPARE(mock.requests.first().target,
                 QByteArray("/secret/base/panel/api/server/restartXrayService"));
        mock.add(403, {{QStringLiteral("success"), false},
                       {QStringLiteral("msg"), QStringLiteral("secret-admin-token")}});
        done = false;
        api.serverAction(ServerAction::UpdateGeofiles, {}, [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.contains(QStringLiteral("secret-admin-token")));
        QCOMPARE(mock.requests.size(), 2); // Forbidden action has no alternate endpoint or retry.
    }

    void operationQueueAndCallbackDestruction() {
        MockPanel mock;
        auto* api = new ThreeXUiApi(configuration(mock));
        mock.add(200, envelope(QJsonObject{{QStringLiteral("cpu"), 1}}));
        mock.add(200, envelope(QJsonArray{}));
        bool first = false;
        bool second = false;
        api->fetchStatus([&](Outcome<Snapshot> value) {
            first = value.ok;
            delete api;
        });
        api->fetchInventory([&](Outcome<Inventory>) { second = true; });
        QTRY_VERIFY_WITH_TIMEOUT(first, 3000);
        QTest::qWait(30);
        QVERIFY(!second);
        QCOMPARE(mock.requests.size(), 2);
    }

    void destroyedPendingOperationHasNoCallback() {
        MockPanel mock;
        auto* api = new ThreeXUiApi(configuration(mock));
        mock.hold();
        bool callback = false;
        api->fetchStatus([&](Outcome<Snapshot>) { callback = true; });
        QTRY_COMPARE_WITH_TIMEOUT(mock.requests.size(), 1, 3000);
        delete api;
        QTest::qWait(30);
        QVERIFY(!callback);
    }

    void mutationTimeoutIsUnknownAndNeverRetried() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.add(200, envelope(freshResponse()));
        mock.hold();
        bool done = false;
        Outcome<bool> result;
        api.deleteClient(selectedModern(), [&](Outcome<bool> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 11000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("результат операции неизвестен")));
        QCOMPARE(mock.requests.size(), 2);
        QTest::qWait(30);
        QCOMPARE(mock.requests.size(), 2);
    }

    void oversizedReplyIsRejected() {
        MockPanel mock;
        ThreeXUiApi api(configuration(mock));
        mock.responses.append({200, QByteArray(16 * 1024 * 1024 + 1024, 'x'), {}});
        bool done = false;
        Outcome<Snapshot> result;
        api.fetchStatus([&](Outcome<Snapshot> value) {
            result = std::move(value);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("16 МБ")));
        QCOMPARE(mock.requests.size(), 1);
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
