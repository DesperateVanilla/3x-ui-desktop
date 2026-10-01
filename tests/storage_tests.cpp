#include "local_store.h"

#include <QFile>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>
#include <limits>
#include <memory>

using namespace fleet;

namespace {
class DatabaseProbe {
  public:
    explicit DatabaseProbe(const QString& path)
        : name_(QStringLiteral("storage-test-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces)),
          database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_)) {
        database.setDatabaseName(path);
        opened = database.open();
    }
    ~DatabaseProbe() {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }
    DatabaseProbe(const DatabaseProbe&) = delete;
    DatabaseProbe& operator=(const DatabaseProbe&) = delete;

  private:
    QString name_;

  public:
    QSqlDatabase database;
    bool opened = false;
};

ServerConfig server(const QString& id = QStringLiteral("server-a")) {
    ServerConfig result;
    result.id = id;
    result.name = QStringLiteral("Test fleet ") + id;
    result.location = QStringLiteral("Test location");
    result.panelUrl = QUrl(QStringLiteral("https://panel.example.invalid:8443/private/base/"));
    result.subscriptionUrl = QUrl(QStringLiteral("https://subscription.example.invalid/sub/"));
    result.auth = AuthKind::Password;
    result.api = ApiFlavor::ModernV3;
    result.username = QStringLiteral("Synthetic-user-9fd96a88");
    result.password = QStringLiteral("Synthetic-password-22f417ff");
    result.token = QStringLiteral("Synthetic-token-bc931df3");
    result.twoFactorCode = QStringLiteral("Synthetic-onetime-4fafbdb3");
    return result;
}

QDateTime epoch(qint64 milliseconds = 1'800'000'000'000) {
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::UTC);
}

Snapshot sample(const QString& id, const QDateTime& at) {
    Snapshot result;
    result.serverId = id;
    result.at = at;
    result.health = Health::Online;
    return result;
}

QStringList addedNames(const QStringList& baseline) {
    QStringList result = QSqlDatabase::connectionNames();
    for (const auto& name : baseline)
        result.removeAll(name);
    return result;
}
} // namespace

class StorageTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
#ifndef Q_OS_WIN
        QSKIP("The production credential store requires Windows DPAPI.");
#endif
        QVERIFY(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")));
    }

    void dpapiEnvelopeAndCorruption() {
        const QByteArray plaintext("binary\0credential\xff", 18);
        const auto protectedData = LocalStore::protect(plaintext);
        QVERIFY2(protectedData.ok, qPrintable(protectedData.error));
        QVERIFY(protectedData.value.startsWith(QByteArray("3XDP\x01", 5)));
        QVERIFY(!protectedData.value.contains(plaintext));
        const auto decrypted = LocalStore::unprotect(protectedData.value);
        QVERIFY2(decrypted.ok, qPrintable(decrypted.error));
        QCOMPARE(decrypted.value, plaintext);

        QByteArray corrupt = protectedData.value;
        corrupt[corrupt.size() - 1] = char(static_cast<unsigned char>(corrupt.back()) ^ 0x80);
        const auto damaged = LocalStore::unprotect(corrupt);
        QVERIFY(!damaged.ok);
        QVERIFY(!damaged.error.isEmpty());
        QByteArray future = protectedData.value;
        future[4] = char(2);
        QVERIFY(!LocalStore::unprotect(future).ok);
        QVERIFY(!LocalStore::unprotect(QByteArrayLiteral("{\"password\":\"plaintext\"}")).ok);
        QVERIFY(!LocalStore::unprotect(QByteArray()).ok);

        const auto empty = LocalStore::protect(QByteArray());
        QVERIFY2(empty.ok, qPrintable(empty.error));
        const auto emptyPlaintext = LocalStore::unprotect(empty.value);
        QVERIFY2(emptyPlaintext.ok, qPrintable(emptyPlaintext.error));
        QVERIFY(emptyPlaintext.value.isEmpty());
    }

    void encryptedConfigurationAndReopen() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("subdirectory/fleet.db"));
        const ServerConfig config = server();
        {
            LocalStore store;
            const auto opened = store.open(path);
            QVERIFY2(opened.ok, qPrintable(opened.error));
            const auto saved = store.saveServer(config);
            QVERIFY2(saved.ok, qPrintable(saved.error));
            const auto loaded = store.servers();
            QVERIFY2(loaded.ok, qPrintable(loaded.error));
            QCOMPARE(loaded.value.size(), 1);
            const auto& restored = loaded.value.first();
            QCOMPARE(restored.id, config.id);
            QCOMPARE(restored.name, config.name);
            QCOMPARE(restored.location, config.location);
            QCOMPARE(restored.panelUrl, config.panelUrl);
            QCOMPARE(restored.subscriptionUrl, config.subscriptionUrl);
            QCOMPARE(restored.auth, config.auth);
            QCOMPARE(restored.api, config.api);
            QCOMPARE(restored.username, config.username);
            QCOMPARE(restored.password, config.password);
            QCOMPARE(restored.token, config.token);
            QCOMPARE(restored.allowHttp, config.allowHttp);
            QVERIFY(restored.twoFactorCode.isEmpty());

            DatabaseProbe probe(path);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY2(query.exec(QStringLiteral("SELECT * FROM servers")),
                     qPrintable(query.lastError().text()));
            QVERIFY(query.next());
            const QSqlRecord record = query.record();
            QVERIFY(record.indexOf(QStringLiteral("username")) == -1);
            QVERIFY(record.indexOf(QStringLiteral("password")) == -1);
            QVERIFY(record.indexOf(QStringLiteral("token")) == -1);
            QVERIFY(record.indexOf(QStringLiteral("two_factor_code")) == -1);
            const QByteArray ciphertext =
                query.value(record.indexOf(QStringLiteral("credentials"))).toByteArray();
            QVERIFY(ciphertext.startsWith(QByteArray("3XDP\x01", 5)));
            for (const QString& secret :
                 {config.username, config.password, config.token, config.twoFactorCode})
                QVERIFY(!ciphertext.contains(secret.toUtf8()));
            const auto decrypted = LocalStore::unprotect(ciphertext);
            QVERIFY(decrypted.ok);
            const auto credentials = QJsonDocument::fromJson(decrypted.value).object();
            QCOMPARE(credentials.size(), 3);
            QVERIFY(!credentials.contains(QStringLiteral("twoFactorCode")));
            QVERIFY(!decrypted.value.contains(config.twoFactorCode.toUtf8()));
            query.finish();
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
            query.finish();
            QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("wal"));
        }
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray bytes = raw.readAll();
        for (const QString& secret :
             {config.username, config.password, config.token, config.twoFactorCode})
            QVERIFY(!bytes.contains(secret.toUtf8()));
        raw.close();
        LocalStore reopened;
        const auto opened = reopened.open(path);
        QVERIFY2(opened.ok, qPrintable(opened.error));
        const auto loaded = reopened.servers();
        QVERIFY(loaded.ok);
        QCOMPARE(loaded.value.size(), 1);
        QCOMPARE(loaded.value.first().password, config.password);
        QVERIFY(loaded.value.first().twoFactorCode.isEmpty());
    }

    void updateAndMissingRows() {
        QTemporaryDir temporary;
        LocalStore store;
        const QString path = temporary.filePath(QStringLiteral("fleet.db"));
        QVERIFY(store.open(path).ok);
        ServerConfig config = server(QStringLiteral("id'; DROP TABLE servers; --"));
        config.name = QStringLiteral("O'Brien\nlocal name");
        config.location = QString();
        config.subscriptionUrl = QUrl();
        QVERIFY(store.saveServer(config).ok);
        config.name = QStringLiteral("Updated");
        config.token = QStringLiteral("Synthetic-updated-token-2d478c87");
        config.panelUrl = QUrl(QStringLiteral("http://localhost:2053/base/"));
        config.allowHttp = true;
        config.auth = AuthKind::Token;
        config.api = ApiFlavor::Auto;
        QVERIFY(store.saveServer(config).ok);
        const auto updated = store.servers();
        QVERIFY(updated.ok);
        QCOMPARE(updated.value.size(), 1);
        QCOMPARE(updated.value.first().id, config.id);
        QCOMPARE(updated.value.first().name, config.name);
        QCOMPARE(updated.value.first().token, config.token);
        QVERIFY(updated.value.first().allowHttp);
        QVERIFY(updated.value.first().location.isEmpty());
        QVERIFY(updated.value.first().subscriptionUrl.isEmpty());
        const auto missing = store.removeServer(QStringLiteral("missing"));
        QVERIFY(!missing.ok);
        QVERIFY(!missing.error.isEmpty());
        const auto missingSnapshot =
            store.appendSnapshot(sample(QStringLiteral("missing"), epoch()));
        QVERIFY(!missingSnapshot.ok);
        QVERIFY(!missingSnapshot.error.isEmpty());
        QVERIFY(store.removeServer(config.id).ok);
        QVERIFY(store.servers().value.isEmpty());
        QVERIFY(!store.removeServer(config.id).ok);
        QVERIFY(!store.removeServer({}).ok);
    }

    void corruptedCredentialRowsFailWithoutFallback() {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("fleet.db"));
        LocalStore store;
        QVERIFY(store.open(path).ok);
        const auto config = server();
        QVERIFY(store.saveServer(config).ok);
        {
            DatabaseProbe probe(path);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.prepare(
                QStringLiteral("UPDATE servers SET credentials = :blob WHERE id = :id")));
            query.bindValue(QStringLiteral(":id"), config.id);
            query.bindValue(
                QStringLiteral(":blob"),
                QByteArrayLiteral(
                    "{\"username\":\"fallback\",\"password\":\"unsafe\",\"token\":\"raw\"}"));
            QVERIFY(query.exec());
        }
        const auto corrupted = store.servers();
        QVERIFY(!corrupted.ok);
        QVERIFY(!corrupted.error.isEmpty());
        QVERIFY(corrupted.value.isEmpty());
        QVERIFY(store.saveServer(config).ok);
        {
            DatabaseProbe probe(path);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.prepare(
                QStringLiteral("UPDATE servers SET credentials = :blob WHERE id = :id")));
            query.bindValue(QStringLiteral(":id"), config.id);
            const auto encryptedInvalidJson = LocalStore::protect(QByteArrayLiteral("not-json"));
            QVERIFY(encryptedInvalidJson.ok);
            query.bindValue(QStringLiteral(":blob"), encryptedInvalidJson.value);
            QVERIFY(query.exec());
        }
        QVERIFY(!store.servers().ok);
        LocalStore reopened;
        QVERIFY(reopened.open(path).ok);
        QVERIFY(!reopened.servers().ok);
    }

    void nullableMetricsAndChronologicalHistory() {
        QTemporaryDir temporary;
        LocalStore store;
        QVERIFY(store.open(temporary.filePath(QStringLiteral("fleet.db"))).ok);
        QVERIFY(store.saveServer(server()).ok);
        QVERIFY(store.saveServer(server(QStringLiteral("server-b"))).ok);
        Snapshot zero = sample(QStringLiteral("server-a"), epoch());
        zero.cpu = zero.memoryPercent = zero.diskPercent = zero.rxBps = zero.txBps = 0.0;
        zero.receivedBytes = zero.sentBytes = qint64(0);
        zero.online = zero.latencyMs = 0;
        zero.panelVersion = QStringLiteral("3.7.0");
        zero.xrayVersion = QStringLiteral("25.8.31");
        zero.uptimeSeconds = 987654;
        zero.error = QStringLiteral("Synthetic status message");
        const Snapshot unknown = sample(QStringLiteral("server-a"), epoch().addSecs(30));
        QVERIFY(store.appendSnapshot(unknown).ok);
        QVERIFY(store.appendSnapshot(zero).ok);
        QVERIFY(store.appendSnapshot(sample(QStringLiteral("server-b"), epoch().addSecs(60))).ok);
        QVERIFY(store.appendSnapshot(sample(QStringLiteral("server-a"), epoch().addSecs(-30))).ok);
        const auto history = store.history(QStringLiteral("server-a"), epoch());
        QVERIFY2(history.ok, qPrintable(history.error));
        QCOMPARE(history.value.size(), 2);
        QCOMPARE(history.value.at(0).at, zero.at);
        QCOMPARE(history.value.at(1).at, unknown.at);
        const auto& restoredZero = history.value.at(0);
        QVERIFY(restoredZero.cpu.has_value());
        QCOMPARE(*restoredZero.cpu, 0.0);
        QVERIFY(restoredZero.memoryPercent.has_value());
        QCOMPARE(*restoredZero.memoryPercent, 0.0);
        QVERIFY(restoredZero.diskPercent.has_value());
        QCOMPARE(*restoredZero.diskPercent, 0.0);
        QVERIFY(restoredZero.rxBps.has_value());
        QCOMPARE(*restoredZero.rxBps, 0.0);
        QVERIFY(restoredZero.txBps.has_value());
        QCOMPARE(*restoredZero.txBps, 0.0);
        QVERIFY(restoredZero.receivedBytes.has_value());
        QCOMPARE(*restoredZero.receivedBytes, qint64(0));
        QVERIFY(restoredZero.sentBytes.has_value());
        QCOMPARE(*restoredZero.sentBytes, qint64(0));
        QVERIFY(restoredZero.online.has_value());
        QCOMPARE(*restoredZero.online, 0);
        QVERIFY(restoredZero.latencyMs.has_value());
        QCOMPARE(*restoredZero.latencyMs, 0);
        QCOMPARE(restoredZero.panelVersion, zero.panelVersion);
        QCOMPARE(restoredZero.xrayVersion, zero.xrayVersion);
        QCOMPARE(restoredZero.uptimeSeconds, zero.uptimeSeconds);
        QCOMPARE(restoredZero.error, zero.error);
        const auto& restoredUnknown = history.value.at(1);
        QVERIFY(!restoredUnknown.cpu);
        QVERIFY(!restoredUnknown.memoryPercent);
        QVERIFY(!restoredUnknown.diskPercent);
        QVERIFY(!restoredUnknown.rxBps);
        QVERIFY(!restoredUnknown.txBps);
        QVERIFY(!restoredUnknown.receivedBytes);
        QVERIFY(!restoredUnknown.sentBytes);
        QVERIFY(!restoredUnknown.online);
        QVERIFY(!restoredUnknown.latencyMs);
        const auto all = store.history({}, epoch().addSecs(-30));
        QVERIFY(all.ok);
        QCOMPARE(all.value.size(), 4);
        for (qsizetype i = 1; i < all.value.size(); ++i)
            QVERIFY(all.value.at(i - 1).at <= all.value.at(i).at);
        QVERIFY(store.history(QStringLiteral("missing"), epoch()).value.isEmpty());
        QVERIFY(store.history({}, epoch().addDays(1)).value.isEmpty());
    }

    void duplicateWindowAndForeignKeyCascade() {
        const QStringList baseline = QSqlDatabase::connectionNames();
        QTemporaryDir temporary;
        LocalStore store;
        const QString path = temporary.filePath(QStringLiteral("fleet.db"));
        QVERIFY(store.open(path).ok);
        const auto config = server();
        QVERIFY(store.saveServer(config).ok);
        const auto first = store.appendSnapshot(sample(config.id, epoch()));
        QVERIFY(first.ok && first.value);
        const auto duplicate = store.appendSnapshot(sample(config.id, epoch().addMSecs(19'999)));
        QVERIFY(duplicate.ok && !duplicate.value);
        const auto earlierDuplicate =
            store.appendSnapshot(sample(config.id, epoch().addMSecs(-19'999)));
        QVERIFY(earlierDuplicate.ok && !earlierDuplicate.value);
        const auto boundary = store.appendSnapshot(sample(config.id, epoch().addSecs(20)));
        QVERIFY(boundary.ok && boundary.value);
        const auto earlierBoundary = store.appendSnapshot(sample(config.id, epoch().addSecs(-20)));
        QVERIFY(earlierBoundary.ok && earlierBoundary.value);
        QCOMPARE(store.history(config.id, epoch().addSecs(-20)).value.size(), 3);

        const QStringList connections = addedNames(baseline);
        QCOMPARE(connections.size(), 1);
        {
            QSqlDatabase borrowed = QSqlDatabase::database(connections.first(), false);
            QSqlQuery query(borrowed);
            QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_keys")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
        }
        {
            DatabaseProbe probe(path);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
            QVERIFY(query.prepare(QStringLiteral(
                "INSERT INTO snapshots(server_id,at,health) VALUES (:server,:at,0)")));
            query.bindValue(QStringLiteral(":server"), QStringLiteral("missing"));
            query.bindValue(QStringLiteral(":at"), epoch().toMSecsSinceEpoch());
            QVERIFY(!query.exec());
        }
        QVERIFY(
            store.appendEvent({epoch(), config.id, config.name, QStringLiteral("Added"), true, {}})
                .ok);
        QVERIFY(store.removeServer(config.id).ok);
        const auto history = store.history({}, epoch().addDays(-1));
        QVERIFY(history.ok);
        QVERIFY(history.value.isEmpty());
        const auto events = store.events();
        QVERIFY(events.ok);
        QCOMPARE(events.value.size(), 1);
        QCOMPARE(events.value.first().serverId, config.id);
        QCOMPARE(events.value.first().serverName, config.name);
    }

    void eventOrderLimitsAndPruning() {
        QTemporaryDir temporary;
        LocalStore store;
        QVERIFY(store.open(temporary.filePath(QStringLiteral("fleet.db"))).ok);
        const auto config = server();
        QVERIFY(store.saveServer(config).ok);
        const QDateTime before = epoch().addDays(-30);
        QVERIFY(store.appendSnapshot(sample(config.id, before.addSecs(-30))).ok);
        QVERIFY(store.appendSnapshot(sample(config.id, before)).ok);
        QVERIFY(store.appendSnapshot(sample(config.id, epoch())).ok);
        const ActivityEvent old{before.addSecs(-1),    config.id, config.name,
                                QStringLiteral("Old"), true,      {}};
        const ActivityEvent exact{before, config.id, config.name, QStringLiteral("Boundary"),
                                  true,   {}};
        const ActivityEvent recent{epoch(),     config.id,
                                   config.name, QStringLiteral("Newest"),
                                   false,       QStringLiteral("Synthetic error")};
        QVERIFY(store.appendEvent(recent).ok);
        QVERIFY(store.appendEvent(old).ok);
        QVERIFY(store.appendEvent(exact).ok);
        const auto latestTwo = store.events(2);
        QVERIFY(latestTwo.ok);
        QCOMPARE(latestTwo.value.size(), 2);
        QCOMPARE(latestTwo.value.at(0).at, recent.at);
        QCOMPARE(latestTwo.value.at(1).at, exact.at);
        QVERIFY(!latestTwo.value.at(0).success);
        QCOMPARE(latestTwo.value.at(0).detail, recent.detail);
        const auto none = store.events(0);
        QVERIFY(none.ok && none.value.isEmpty());
        QVERIFY(!store.events(-1).ok);
        QVERIFY(store.prune(before).ok);
        const auto history = store.history({}, before.addDays(-1));
        QVERIFY(history.ok);
        QCOMPARE(history.value.size(), 2);
        QCOMPARE(history.value.first().at, before);
        const auto retainedEvents = store.events();
        QVERIFY(retainedEvents.ok);
        QCOMPARE(retainedEvents.value.size(), 2);
        QCOMPARE(retainedEvents.value.last().at, before);
        QCOMPARE(store.servers().value.size(), 1);
    }

    void schemaVersionAndIndependentConnections() {
        const QStringList baseline = QSqlDatabase::connectionNames();
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("fleet.db"));
        const QString futurePath = temporary.filePath(QStringLiteral("future.db"));
        {
            DatabaseProbe probe(futurePath);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 999")));
        }
        {
            auto first = std::make_unique<LocalStore>();
            auto second = std::make_unique<LocalStore>();
            QVERIFY(first->open(path).ok);
            QVERIFY(first->saveServer(server()).ok);
            QVERIFY(second->open(path).ok);
            QCOMPARE(addedNames(baseline).size(), 2);
            QCOMPARE(second->servers().value.size(), 1);
            const auto future = first->open(futurePath);
            QVERIFY(!future.ok);
            QVERIFY(!future.error.isEmpty());
            QCOMPARE(addedNames(baseline).size(), 2);
            QCOMPARE(first->servers().value.size(), 1);
            QVERIFY(first->open(path).ok);
            QCOMPARE(addedNames(baseline).size(), 2);
            first.reset();
            QCOMPARE(addedNames(baseline).size(), 1);
            QVERIFY(second->saveServer(server(QStringLiteral("server-b"))).ok);
            QCOMPARE(second->servers().value.size(), 2);
            second.reset();
            QVERIFY(addedNames(baseline).isEmpty());
        }
        const QString incompatiblePath = temporary.filePath(QStringLiteral("incompatible.db"));
        {
            DatabaseProbe probe(incompatiblePath);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE servers (id TEXT PRIMARY KEY)")));
        }
        LocalStore incompatible;
        const auto failed = incompatible.open(incompatiblePath);
        QVERIFY(!failed.ok);
        QVERIFY(addedNames(baseline).isEmpty());
        {
            DatabaseProbe probe(incompatiblePath);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec(
                QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name = 'snapshots'")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
        }
    }

    void downsampledHistoryPreservesRangeLastSampleAndWorstHealth() {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("fleet.db"));
        LocalStore store;
        QVERIFY(store.open(path).ok);
        const auto config = server();
        QVERIFY(store.saveServer(config).ok);
        {
            DatabaseProbe probe(path);
            QVERIFY(probe.opened);
            QSqlQuery query(probe.database);
            // One SQL statement creates a small synthetic oversize history without 100001 API
            // calls.
            QVERIFY(query.prepare(QStringLiteral("WITH RECURSIVE n(value) AS (VALUES(0) UNION ALL "
                                                 "SELECT value+1 FROM n WHERE value<100000) "
                                                 "INSERT INTO snapshots(server_id,at,health,cpu) "
                                                 "SELECT :server,:base+value*1000,CASE WHEN "
                                                 "value=10 THEN 3 ELSE 1 END,value FROM n")));
            query.bindValue(QStringLiteral(":server"), config.id);
            query.bindValue(QStringLiteral(":base"), epoch().toMSecsSinceEpoch());
            QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
        }
        const auto history = store.history({}, epoch());
        QVERIFY2(history.ok, qPrintable(history.error));
        QVERIFY(history.value.size() <= 100000);
        QVERIFY(history.value.size() > 1000);
        QVERIFY(history.value.first().at < epoch().addSecs(60));
        QCOMPARE(history.value.first().health, Health::Offline);
        QCOMPARE(history.value.last().at, epoch().addSecs(100000));
        QVERIFY(history.value.last().cpu.has_value());
        QCOMPARE(*history.value.last().cpu, 100000.0);
        for (qsizetype i = 1; i < history.value.size(); ++i)
            QVERIFY(history.value.at(i - 1).at < history.value.at(i).at);
    }

    void closedStoreInvalidInputAndThreadAffinity() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        LocalStore closed;
        QVERIFY(!closed.servers().ok);
        QVERIFY(!closed.saveServer(server()).ok);
        QVERIFY(!closed.removeServer(QStringLiteral("server-a")).ok);
        QVERIFY(!closed.appendSnapshot(sample(QStringLiteral("server-a"), epoch())).ok);
        QVERIFY(!closed.history({}, epoch()).ok);
        QVERIFY(!closed.appendEvent({epoch(), {}, {}, {}, true, {}}).ok);
        QVERIFY(!closed.events().ok);
        QVERIFY(!closed.prune(epoch()).ok);
        QVERIFY(!closed.open({}).ok);

        QVERIFY(closed.open(temporary.filePath(QStringLiteral("fleet.db"))).ok);
        QVERIFY(closed.saveServer(server()).ok);
        auto invalidConfig = server();
        invalidConfig.id.clear();
        QVERIFY(!closed.saveServer(invalidConfig).ok);
        invalidConfig = server();
        invalidConfig.panelUrl =
            QUrl(QStringLiteral("https://plain-user:plain-password@panel.example.invalid/"));
        QVERIFY(!closed.saveServer(invalidConfig).ok);
        invalidConfig = server();
        invalidConfig.panelUrl = QUrl(QStringLiteral("http://panel.example.invalid/"));
        QVERIFY(!closed.saveServer(invalidConfig).ok);
        auto invalidSnapshot = sample(QStringLiteral("server-a"), epoch());
        invalidSnapshot.cpu = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!closed.appendSnapshot(invalidSnapshot).ok);
        invalidSnapshot.cpu.reset();
        invalidSnapshot.at = {};
        QVERIFY(!closed.appendSnapshot(invalidSnapshot).ok);
        QVERIFY(!closed.history({}, {}).ok);
        QVERIFY(!closed.prune({}).ok);
        QVERIFY(!closed.appendEvent({{}, {}, {}, {}, true, {}}).ok);

        Outcome<QList<ServerConfig>> crossThread;
        OperationResult crossThreadWrite;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            crossThread = closed.servers();
            crossThreadWrite = closed.saveServer(server(QStringLiteral("other-thread")));
        }));
        thread->start();
        QVERIFY(thread->wait(5000));
        QVERIFY(!crossThread.ok);
        QVERIFY(!crossThread.error.isEmpty());
        QVERIFY(!crossThreadWrite.ok);
        QCOMPARE(closed.servers().value.size(), 1);
    }
};

QTEST_GUILESS_MAIN(StorageTests)
#include "storage_tests.moc"
