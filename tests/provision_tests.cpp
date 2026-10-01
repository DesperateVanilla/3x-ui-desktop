#include "client_provisioner.h"
#include "three_x_ui_api.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUuid>
#include <memory>

using namespace fleet;

namespace {
class Panel : public QObject {
  public:
    QTcpServer listener;
    QString name;
    QStringList* trace;
    QJsonArray inbounds;
    QJsonArray clients;
    QList<QJsonObject> writes;
    QList<QByteArray> requests;
    bool rejectAdd = false;
    bool holdReads = false;
    bool holdAdd = false;
    bool legacy = false;
    explicit Panel(QString id, QStringList* log) : name(std::move(id)), trace(log) {
        inbounds = {inbound(1, "vless"), inbound(2, "vless")};
        if (!listener.listen(QHostAddress::LocalHost, 0))
            qFatal("Cannot open loopback test panel");
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (listener.hasPendingConnections()) {
                auto* socket = listener.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, handled] {
                    bytes->append(socket->readAll());
                    if (*handled)
                        return;
                    const qsizetype end = bytes->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const auto lines = bytes->left(end).split('\n');
                    const auto parts = lines.first().trimmed().split(' ');
                    if (parts.size() < 2)
                        return;
                    qsizetype length = 0;
                    for (const auto& line : lines)
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    if (bytes->size() < end + 4 + length)
                        return;
                    *handled = true;
                    const auto method = parts[0];
                    const auto path = parts[1];
                    const auto payload =
                        QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                    requests.append(method + " " + path);
                    trace->append(name + ":" + QString::fromUtf8(method + " " + path));
                    const bool adding = path.endsWith("/clients/add");
                    if (adding)
                        writes.append(payload);
                    if ((method == "GET" && holdReads) || (adding && holdAdd))
                        return;
                    QJsonValue object;
                    bool success = true;
                    if (path.endsWith("/inbounds/list"))
                        object = inbounds;
                    else if (path.endsWith("/clients/list"))
                        object = clients;
                    else if (adding) {
                        success = !rejectAdd;
                        if (success) {
                            auto record = payload.value("client").toObject();
                            record.insert("uuid", record.value("id"));
                            record.insert("id", 99);
                            record.insert("inboundIds", payload.value("inboundIds"));
                            clients.append(record);
                        }
                    } else if (path.contains("/clients/get/")) {
                        if (clients.isEmpty())
                            success = false;
                        else
                            object = QJsonObject{
                                {"client", clients.first()},
                                {"inboundIds", clients.first().toObject().value("inboundIds")}};
                    } else if (path.endsWith("/attach")) {
                        auto record = clients.first().toObject();
                        auto bindings = record.value("inboundIds").toArray();
                        for (const auto& id : payload.value("inboundIds").toArray())
                            bindings.append(id);
                        record.insert("inboundIds", bindings);
                        clients[0] = record;
                    } else
                        success = false;
                    const QByteArray body =
                        QJsonDocument(
                            QJsonObject{{"success", success},
                                        {"obj", object},
                                        {"msg", success ? "" : "synthetic-secret-never-log"}})
                            .toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "
                                  "application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    static QJsonObject inbound(int id, const QString& protocol) {
        return {{"id", id}, {"protocol", protocol}, {"enable", true}, {"port", 443}};
    }
    ServerConfig config() const {
        ServerConfig result;
        result.id = name;
        result.name = name;
        result.panelUrl = QUrl(QString("http://127.0.0.1:%1/base/").arg(listener.serverPort()));
        result.allowHttp = true;
        result.token = "synthetic-token";
        result.api = legacy ? ApiFlavor::LegacyV2 : ApiFlavor::ModernV3;
        return result;
    }
};
ClientDraft draft() {
    ClientDraft value;
    value.email = "common-client";
    value.totalBytes = 5LL * 1024 * 1024 * 1024;
    value.expiryTime = 1900000000000LL;
    return value;
}
QJsonObject existing(const ClientDraft& value, QString email = {}) {
    return {{"id", 12},
            {"uuid", value.clientId},
            {"password", value.password},
            {"email", email.isEmpty() ? value.email : email},
            {"subId", value.subId},
            {"totalGB", value.totalBytes},
            {"expiryTime", value.expiryTime},
            {"enable", value.enable},
            {"flow", value.flow},
            {"inboundIds", QJsonArray{1, 2}}};
}
} // namespace

class ProvisionTests : public QObject {
    Q_OBJECT
  private slots:
    void verifiesEveryServerBeforeWritingAndSharesIdentity() {
        QStringList trace;
        Panel master("master", &trace), child("child", &trace);
        child.inbounds = {Panel::inbound(1, "vmess"), Panel::inbound(2, "trojan")};
        ThreeXUiApi masterApi(master.config()), childApi(child.config());
        ClientProvisioner coordinator({{"master", &masterApi}, {"child", &childApi}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"master", {1, 2}}, {"child", {1, 2}}}, draft(), "master",
                              [&](auto outcome) {
                                  result = std::move(outcome);
                                  done = true;
                              });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.value.nodes.size(), 2);
        QVERIFY(result.value.nodes[0].ok && result.value.nodes[1].ok);
        QCOMPARE(master.writes.size(), 1);
        QCOMPARE(child.writes.size(), 1);
        const auto a = master.writes.first().value("client").toObject();
        const auto b = child.writes.first().value("client").toObject();
        for (const auto* key : {"id", "password", "subId", "email", "totalGB", "expiryTime"})
            QCOMPARE(a.value(key), b.value(key));
        QVERIFY(!QUuid(a.value("id").toString()).isNull());
        QCOMPARE(a.value("password").toString().size(), 32);
        QCOMPARE(a.value("subId").toString().size(), 24);
        QCOMPARE(result.value.clientId, a.value("id").toString());
        QCOMPARE(result.value.subId, a.value("subId").toString());
        QCOMPARE(b.value("security").toString(), QString("auto"));
        const int firstWrite = trace.indexOf("child:POST /base/panel/api/clients/add");
        QVERIFY(firstWrite >= 0);
        QVERIFY(trace.indexOf("master:GET /base/panel/api/clients/list") < firstWrite);
        QVERIFY(trace.indexOf("child:GET /base/panel/api/clients/list") < firstWrite);
        QVERIFY(firstWrite < trace.indexOf("master:POST /base/panel/api/clients/add"));
    }
    void hysteriaSharesAuthAcrossServersAndAllowsMixedInbounds() {
        QStringList trace;
        Panel master("master", &trace), child("child", &trace);
        master.inbounds = {Panel::inbound(1, "vless"), Panel::inbound(2, "hysteria")};
        child.inbounds = {Panel::inbound(1, "trojan"), Panel::inbound(2, "hysteria")};
        ThreeXUiApi a(master.config()), b(child.config());
        ClientProvisioner coordinator({{"master", &a}, {"child", &b}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"master", {1, 2}}, {"child", {1, 2}}}, draft(), "master",
                              [&](auto value) {
                                  result = std::move(value);
                                  done = true;
                              });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.value.nodes[0].ok && result.value.nodes[1].ok);
        const auto left = master.writes.first().value("client").toObject(),
                   right = child.writes.first().value("client").toObject();
        QCOMPARE(left.value("auth"), right.value("auth"));
        QCOMPARE(left.value("auth").toString().size(), 32);
        QCOMPARE(left.value("id"), right.value("id"));
        QCOMPARE(left.value("subId"), right.value("subId"));
    }
    void rejectedSecondServerLeavesBothUnchanged_data() {
        QTest::addColumn<QString>("problem");
        for (const auto* item :
             {"disabled", "missing", "legacy", "vision", "email", "case", "uuid", "subid"})
            QTest::newRow(item) << QString(item);
    }
    void rejectedSecondServerLeavesBothUnchanged() {
        QFETCH(QString, problem);
        QStringList trace;
        Panel a("a", &trace), b("b", &trace);
        auto plan = draft();
        plan.clientId = "11111111-2222-4333-8444-555555555555";
        plan.password = "shared-synthetic-password";
        plan.subId = "shared-synthetic-sub";
        if (problem == "disabled") {
            auto item = Panel::inbound(2, "vless");
            item.insert("enable", false);
            b.inbounds = {item};
        }
        if (problem == "missing")
            b.inbounds = {Panel::inbound(99, "vless")};
        if (problem == "legacy")
            b.legacy = true;
        if (problem == "vision") {
            b.inbounds = {Panel::inbound(1, "trojan"), Panel::inbound(2, "vless")};
            plan.flow = "xtls-rprx-vision";
        }
        if (problem == "email") {
            auto raw = existing(plan);
            raw.insert("uuid", "foreign-identity");
            b.clients = {raw};
        }
        if (problem == "case")
            b.clients = {existing(plan, "COMMON-CLIENT")};
        if (problem == "uuid") {
            auto raw = existing(plan, "foreign-email");
            raw.insert("subId", "different-sub");
            b.clients = {raw};
        }
        if (problem == "subid") {
            auto raw = existing(plan, "foreign-email");
            raw.insert("uuid", "foreign-identity");
            b.clients = {raw};
        }
        ThreeXUiApi apiA(a.config()), apiB(b.config());
        ClientProvisioner coordinator({{"a", &apiA}, {"b", &apiB}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"a", {1, 2}}, {"b", {1, 2}}}, plan, {}, [&](auto outcome) {
            result = std::move(outcome);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("b"));
        QVERIFY(!result.error.contains(plan.password));
        QVERIFY(!result.error.contains(plan.subId));
        QVERIFY(!result.error.contains(plan.clientId));
        QVERIFY(a.writes.isEmpty() && b.writes.isEmpty());
        for (const auto& entry : trace)
            QVERIFY(!entry.contains(":POST "));
    }
    void partialResultKeepsSuccessWithoutRetryOrDelete() {
        QStringList trace;
        Panel a("a", &trace), b("b", &trace);
        b.rejectAdd = true;
        ThreeXUiApi apiA(a.config()), apiB(b.config());
        ClientProvisioner coordinator({{"a", &apiA}, {"b", &apiB}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"a", {1}}, {"b", {2}}}, draft(), {}, [&](auto outcome) {
            result = std::move(outcome);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(result.ok);
        QVERIFY(result.value.nodes[0].ok);
        QVERIFY(!result.value.nodes[1].ok);
        QVERIFY(!result.value.nodes[1].error.contains("synthetic-secret-never-log"));
        QCOMPARE(a.writes.size(), 1);
        QCOMPARE(b.writes.size(), 1);
        QCOMPARE(a.clients.size(), 1);
        for (const auto& entry : trace)
            QVERIFY(!entry.contains("/del/"));
    }
    void invalidPlansNeverReachNetwork_data() {
        QTest::addColumn<QString>("problem");
        for (const auto* item :
             {"empty", "repeat-server", "repeat-inbound", "zero", "empty-inbounds", "email-space",
              "email-slash", "quota", "uuid", "flow"})
            QTest::newRow(item) << QString(item);
    }
    void invalidPlansNeverReachNetwork() {
        QFETCH(QString, problem);
        QStringList trace;
        Panel a("a", &trace);
        ThreeXUiApi api(a.config());
        ClientProvisioner coordinator({{"a", &api}});
        QList<ClientTarget> targets{{"a", {1}}};
        auto plan = draft();
        if (problem == "empty")
            targets.clear();
        if (problem == "repeat-server")
            targets.append({"a", {2}});
        if (problem == "repeat-inbound")
            targets[0].inboundIds = {1, 1};
        if (problem == "zero")
            targets[0].inboundIds = {0};
        if (problem == "empty-inbounds")
            targets[0].inboundIds.clear();
        if (problem == "email-space")
            plan.email = "invalid name";
        if (problem == "email-slash")
            plan.email = "invalid\\name";
        if (problem == "quota")
            plan.totalBytes = 9007199254740992LL;
        if (problem == "uuid")
            plan.clientId = "bad-uuid";
        if (problem == "flow")
            plan.flow = "unsupported-flow";
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision(targets, plan, {}, [&](auto outcome) {
            result = std::move(outcome);
            done = true;
        });
        QVERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(trace.isEmpty());
    }
    void apiDestructionDuringPreflightCompletesAndBusyRejects() {
        QStringList trace;
        Panel a("a", &trace);
        a.holdReads = true;
        auto* api = new ThreeXUiApi(a.config());
        ClientProvisioner coordinator({{"a", api}});
        int callbacks = 0;
        bool firstOk = true, secondOk = true;
        coordinator.provision({{"a", {1}}}, draft(), {}, [&](auto result) {
            firstOk = result.ok;
            ++callbacks;
        });
        coordinator.provision({{"a", {1}}}, draft(), {}, [&](auto result) {
            secondOk = result.ok;
            ++callbacks;
        });
        QCOMPARE(callbacks, 1);
        QVERIFY(!secondOk);
        QTRY_VERIFY_WITH_TIMEOUT(!a.requests.isEmpty(), 2000);
        delete api;
        QCOMPARE(callbacks, 2);
        QVERIFY(!firstOk);
        bool thirdDone = false;
        coordinator.provision({{"a", {1}}}, draft(), {}, [&](auto result) {
            QVERIFY(!result.ok);
            thirdDone = true;
        });
        QVERIFY(thirdDone);
    }
    void apiDestructionDuringWriteReportsUnknownAndContinues() {
        QStringList trace;
        Panel a("a", &trace), b("b", &trace);
        a.holdAdd = true;
        auto* apiA = new ThreeXUiApi(a.config());
        ThreeXUiApi apiB(b.config());
        ClientProvisioner coordinator({{"a", apiA}, {"b", &apiB}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"a", {1}}, {"b", {1}}}, draft(), {}, [&](auto outcome) {
            result = std::move(outcome);
            done = true;
        });
        QTRY_COMPARE_WITH_TIMEOUT(a.writes.size(), 1, 2000);
        delete apiA;
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(result.ok);
        QVERIFY(!result.value.nodes[0].ok);
        QVERIFY(result.value.nodes[0].error.contains(QString::fromUtf8("неизвестен")));
        QVERIFY(result.value.nodes[1].ok);
        QCOMPARE(b.writes.size(), 1);
    }
    void destroyingCoordinatorSuppressesLateCallbacks() {
        QStringList trace;
        Panel a("a", &trace);
        a.holdReads = true;
        auto* api = new ThreeXUiApi(a.config());
        auto* coordinator = new ClientProvisioner({{"a", api}});
        int calls = 0;
        coordinator->provision({{"a", {1}}}, draft(), {}, [&](auto) { ++calls; });
        QTRY_VERIFY_WITH_TIMEOUT(!a.requests.isEmpty(), 2000);
        delete coordinator;
        delete api;
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
    }
    void exactExistingIdentityDoesNotAddAgain() {
        QStringList trace;
        Panel a("a", &trace);
        auto plan = draft();
        plan.clientId = "11111111-2222-4333-8444-555555555555";
        plan.password = "shared-password";
        plan.subId = "shared-sub";
        a.clients = {existing(plan)};
        ThreeXUiApi api(a.config());
        ClientProvisioner coordinator({{"a", &api}});
        bool done = false;
        Outcome<ProvisionSummary> result;
        coordinator.provision({{"a", {1, 2}}}, plan, "not-a-target", [&](auto outcome) {
            result = std::move(outcome);
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.value.nodes.first().ok);
        QCOMPARE(result.value.clientId, plan.clientId);
        QVERIFY(a.writes.isEmpty());
    }
};
QTEST_GUILESS_MAIN(ProvisionTests)
#include "provision_tests.moc"
