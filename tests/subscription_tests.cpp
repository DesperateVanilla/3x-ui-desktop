#include "subscription_model.h"
#include <QTest>
#include <limits>
using namespace fleet;
namespace {
constexpr qint64 now = 1900000000000LL;
constexpr qint64 day = 86400000;
Client client(QString server = "master", QString email = "alice", QString sub = "common-sub") {
    Client value;
    value.serverId = server;
    value.email = email;
    value.subId = sub;
    value.id = "shared-id";
    value.inboundIds = {1};
    value.protocol = "vless";
    value.usedBytes = 100;
    value.totalBytes = 1000;
    return value;
}
QList<ServerConfig> servers() {
    QList<ServerConfig> list;
    for (const auto& id : {"child", "master", "other"}) {
        ServerConfig config;
        config.id = id;
        config.name = id;
        list.append(config);
    }
    return list;
}
SubscriptionGroup group(Client member = client()) {
    SubscriptionGroup result;
    result.email = member.email;
    result.members = {member};
    result.serverIds = {member.serverId};
    result.protocols = {"vless", "hysteria"};
    return result;
}
} // namespace
class SubscriptionTests : public QObject {
    Q_OBJECT
  private slots:
    void fiftyCopiesAreTenSubscriptionsAndUseMasterTraffic() {
        QList<ServerConfig> configs;
        QHash<QString, Inventory> data;
        for (int n = 0; n < 5; ++n) {
            ServerConfig server;
            server.id = QString::number(n);
            configs.append(server);
            for (int c = 0; c < 10; ++c) {
                auto value =
                    client(server.id, "user-" + QString::number(c), "sub-" + QString::number(c));
                value.usedBytes = n == 3 ? 100 : 999;
                data[server.id].clients.append(value);
            }
        }
        const auto result = groupSubscriptions(configs, data, "3");
        QCOMPARE(result.size(), 10);
        for (const auto& item : result) {
            QCOMPARE(item.members.size(), 5);
            QCOMPARE(item.serverIds.size(), 5);
            QCOMPARE(item.primary().serverId, QString("3"));
            QCOMPARE(item.primary().usedBytes, 100);
            QCOMPARE(item.key.size(), 64);
            QVERIFY(!item.settingsDiffer);
        }
        const auto stats = subscriptionStats(result, now);
        QCOMPARE(stats.total, 10);
        QCOMPARE(stats.active, 10);
        QCOMPARE(stats.usedBytes, 1000);
    }
    void identityPreventsNameCollisionsAndSecretsAreHashed() {
        QHash<QString, Inventory> data;
        data["child"].clients = {client("child")};
        auto other = client("master");
        other.subId = "different-sub";
        data["master"].clients = {other};
        auto result = groupSubscriptions(servers(), data, "master");
        QCOMPARE(result.size(), 2);
        for (const auto& item : result)
            QVERIFY(!item.key.contains("shared-id") && !item.key.contains("common-sub"));
        data["master"].clients[0].subId = "common-sub";
        QCOMPARE(groupSubscriptions(servers(), data, "master").size(), 1);
        data["master"].clients[0].email = "different-email";
        QCOMPARE(groupSubscriptions(servers(), data, "master").size(), 2);
    }
    void fallsBackToCredentialButEmptyIdentifiersStaySeparate() {
        QHash<QString, Inventory> data;
        auto a = client("child"), b = client("master");
        a.subId.clear();
        b.subId.clear();
        data["child"].clients = {a};
        data["master"].clients = {b};
        QCOMPARE(groupSubscriptions(servers(), data, "master").size(), 1);
        data["master"].clients[0].id = "different";
        QCOMPARE(groupSubscriptions(servers(), data, "master").size(), 2);
        data["child"].clients[0].id.clear();
        data["master"].clients[0].id.clear();
        data["child"].clients.append(data["child"].clients.first());
        QCOMPARE(groupSubscriptions(servers(), data, "master").size(), 3);
    }
    void partialInventoryHasDeterministicPrimaryAndProtocolUnion() {
        QHash<QString, Inventory> data;
        data["child"].clients = {client("child")};
        data["other"].clients = {client("other")};
        data["child"].inbounds = {{"child", 1, "", "vless", 443, true}};
        data["other"].inbounds = {{"other", 1, "", "hysteria", 8443, true}};
        const auto result = groupSubscriptions(servers(), data, "master");
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().primary().serverId, QString("child"));
        QCOMPARE(result.first().protocols, (QStringList{"hysteria", "vless"}));
        QCOMPARE(result.first().serverIds.size(), 2);
        const auto stable = result.first().key;
        data["master"].clients = {client("master")};
        const auto complete = groupSubscriptions(servers(), data, "master");
        QCOMPARE(complete.first().key, stable);
        QCOMPARE(complete.first().primary().serverId, QString("master"));
    }
    void unknownOnlineIsPreserved_data() {
        QTest::addColumn<int>("a");
        QTest::addColumn<int>("b");
        QTest::addColumn<int>("expected");
        QTest::newRow("all-unknown") << -1 << -1 << -1;
        QTest::newRow("partial-offline") << 0 << -1 << -1;
        QTest::newRow("all-offline") << 0 << 0 << 0;
        QTest::newRow("known-online") << -1 << 1 << 1;
        QTest::newRow("both-online") << 1 << 1 << 1;
    }
    void unknownOnlineIsPreserved() {
        QFETCH(int, a);
        QFETCH(int, b);
        QFETCH(int, expected);
        auto left = client("child"), right = client("master");
        if (a >= 0)
            left.online = bool(a);
        if (b >= 0)
            right.online = bool(b);
        QHash<QString, Inventory> data;
        data["child"].clients = {left};
        data["master"].clients = {right};
        const auto result = groupSubscriptions(servers(), data, "master").first();
        QCOMPARE(result.online.has_value(), expected >= 0);
        if (expected >= 0)
            QCOMPARE(result.online.value(), bool(expected));
        const auto stats = subscriptionStats({result}, now);
        QCOMPARE(stats.online, expected == 1 ? 1 : 0);
        QCOMPARE(stats.onlineKnown, expected >= 0 ? 1 : 0);
    }
    void statesAndExpiryBoundaries() {
        auto item = group();
        QCOMPARE(item.state(now), SubscriptionState::Active);
        item.members[0].expiryTime = now;
        QCOMPARE(item.state(now), SubscriptionState::Expired);
        item.members[0].expiryTime = -day;
        QCOMPARE(item.state(now), SubscriptionState::Active);
        item.members[0].usedBytes = 1000;
        QCOMPARE(item.state(now), SubscriptionState::QuotaExceeded);
        item.members[0].enable = false;
        QCOMPARE(item.state(now), SubscriptionState::Disabled);
        item.members[0].inboundIds.clear();
        QCOMPARE(item.state(now), SubscriptionState::Unbound);
    }
    void reportsConflictingSettingsButDoesNotCompareNodeTraffic() {
        QHash<QString, Inventory> data;
        data["child"].clients = {client("child")};
        data["master"].clients = {client("master")};
        data["child"].clients[0].usedBytes = 777;
        QVERIFY(!groupSubscriptions(servers(), data, "master").first().settingsDiffer);
        data["child"].clients[0].totalBytes = 2000;
        auto result = groupSubscriptions(servers(), data, "master").first();
        QCOMPARE(result.state(now), SubscriptionState::Mixed);
        QCOMPARE(subscriptionStats({result}, now).mixed, 1);
        data["child"].clients[0].totalBytes = 1000;
        data["child"].clients[0].expiryTime = now + day;
        QVERIFY(groupSubscriptions(servers(), data, "master").first().settingsDiffer);
        data["child"].clients[0].expiryTime = 0;
        data["child"].clients[0].enable = false;
        QVERIFY(groupSubscriptions(servers(), data, "master").first().settingsDiffer);
    }
    void filtersWorkOnGroupsAndServerNames() {
        auto item = group();
        item.serverIds.append("other");
        item.online = true;
        QHash<QString, QString> names{{"master", "Москва"}, {"other", "Netherlands"}};
        SubscriptionFilter filter;
        filter.serverId = "other";
        QVERIFY(matchesSubscription(item, filter, names, now));
        filter.text = "nETHER";
        QVERIFY(matchesSubscription(item, filter, names, now));
        filter.protocol = "HYSTERIA";
        QVERIFY(matchesSubscription(item, filter, names, now));
        filter.state = 6;
        QVERIFY(matchesSubscription(item, filter, names, now));
        filter.expiry = 1;
        QVERIFY(matchesSubscription(item, filter, names, now));
        filter.serverId = "missing";
        QVERIFY(!matchesSubscription(item, filter, names, now));
        filter.serverId.clear();
        filter.text = "missing";
        QVERIFY(!matchesSubscription(item, filter, names, now));
        filter.text.clear();
        filter.expiry = 2;
        item.members[0].expiryTime = now + 7 * day;
        QVERIFY(matchesSubscription(item, filter, names, now));
        QCOMPARE(subscriptionStats({item}, now).expiring, 1);
        ++item.members[0].expiryTime;
        QVERIFY(!matchesSubscription(item, filter, names, now));
        item.members[0].expiryTime = now;
        filter.expiry = 3;
        QVERIFY(matchesSubscription(item, filter, names, now));
        item.members[0].expiryTime = -day;
        QVERIFY(!matchesSubscription(item, filter, names, now));
    }
    void summationSaturatesAndInactiveClientsDoNotCountAsExpiring() {
        auto a = group(), b = group(client("other", "bob", "different"));
        a.members[0].usedBytes = std::numeric_limits<qint64>::max();
        a.members[0].totalBytes = 0;
        b.members[0].usedBytes = 100;
        b.members[0].expiryTime = now + day;
        b.members[0].enable = false;
        const auto stats = subscriptionStats({a, b}, now);
        QCOMPARE(stats.usedBytes, std::numeric_limits<qint64>::max());
        QCOMPARE(stats.expiring, 0);
    }
};
QTEST_GUILESS_MAIN(SubscriptionTests)
#include "subscription_tests.moc"
