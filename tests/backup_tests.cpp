#include "backup_file.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace fleet;

namespace {
Outcome<QByteArray> sqliteFixture(bool modern, const QStringList& extra = {}, bool genuine = true) {
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return Outcome<QByteArray>::failure(QStringLiteral("Temporary fixture directory failed"));
    const QString path = temporary.filePath(QStringLiteral("panel.db"));
    const QString connection =
        QStringLiteral("backup-fixture-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString failure;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            failure = database.lastError().text();
        {
            QSqlQuery query(database);
            QStringList statements;
            if (genuine) {
                statements = {
                    QStringLiteral(
                        "CREATE TABLE settings (id INTEGER PRIMARY KEY, key TEXT, value TEXT)"),
                    QStringLiteral("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT, "
                                   "password TEXT)"),
                    QStringLiteral("CREATE TABLE inbounds (id INTEGER PRIMARY KEY, protocol TEXT, "
                                   "settings TEXT)"),
                    QStringLiteral("INSERT INTO settings VALUES(1,'webPort','2053')"),
                    QStringLiteral("INSERT INTO users VALUES(1,'synthetic-user','synthetic-hash')"),
                    QStringLiteral("INSERT INTO inbounds "
                                   "VALUES(1,'vless','{\"clients\":[{\"email\":\"one\"},{\"email\":"
                                   "\"two\"}]}')"),
                    QStringLiteral("INSERT INTO inbounds "
                                   "VALUES(2,'vmess','{\"clients\":[{\"email\":\"three\"}]}')")};
                if (modern)
                    statements.append(
                        {QStringLiteral(
                             "CREATE TABLE clients (id INTEGER PRIMARY KEY, email TEXT)"),
                         QStringLiteral("INSERT INTO clients VALUES(1,'one'),(2,'two')")});
            } else {
                statements = {
                    QStringLiteral("CREATE TABLE servers(id TEXT PRIMARY KEY, credentials BLOB)"),
                    QStringLiteral("CREATE TABLE snapshots(id INTEGER PRIMARY KEY, server_id TEXT, "
                                   "at INTEGER)"),
                    QStringLiteral("CREATE TABLE events(id INTEGER PRIMARY KEY, at INTEGER)")};
            }
            statements.append(extra);
            for (const auto& statement : statements) {
                if (!failure.isEmpty())
                    break;
                if (!query.exec(statement))
                    failure = query.lastError().text();
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!failure.isEmpty())
        return Outcome<QByteArray>::failure(failure);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Outcome<QByteArray>::failure(QStringLiteral("Fixture file failed"));
    return Outcome<QByteArray>::success(file.readAll());
}

void appendPgInt(QByteArray& data, qint32 value) {
    data.append(value < 0 ? char(1) : char(0));
    quint32 magnitude = value < 0 ? quint32(-qint64(value)) : quint32(value);
    for (int i = 0; i < 4; ++i) {
        data.append(char(magnitude & 0xff));
        magnitude >>= 8;
    }
}

void appendPgString(QByteArray& data, const QByteArray& text) {
    appendPgInt(data, static_cast<qint32>(text.size()));
    data.append(text);
}

QByteArray postgresFixture(int minor = 15) {
    QByteArray data("PGDMP", 5);
    for (const int byte : {1, minor, 0, 4, 8, 1})
        data.append(char(byte));
    if (minor >= 15)
        data.append(char(0));
    else
        appendPgInt(data, 0);
    for (const int value : {0, 0, 0, 1, 0, 126, 0})
        appendPgInt(data, value);
    appendPgString(data, QByteArrayLiteral("xui"));
    appendPgString(data, QByteArrayLiteral("16.0"));
    appendPgString(data, QByteArrayLiteral("16.0"));
    appendPgInt(data, 0); // Empty TOC. This fixture exercises header recognition only.
    return data;
}
} // namespace

class BackupTests : public QObject {
    Q_OBJECT
  private slots:
    void genuineSQLiteModernAndLegacyCounts() {
        for (const bool modern : {false, true}) {
            const auto fixture = sqliteFixture(modern);
            QVERIFY2(fixture.ok, qPrintable(fixture.error));
            const auto inspected = inspectDatabaseBackup(fixture.value);
            QVERIFY2(inspected.ok, qPrintable(inspected.error));
            QCOMPARE(inspected.value.kind, BackupKind::SQLite);
            QCOMPARE(inspected.value.size, fixture.value.size());
            QCOMPARE(inspected.value.inboundCount, qint64(2));
            QCOMPARE(inspected.value.clientCount, modern ? qint64(2) : qint64(3));
            QCOMPARE(inspected.value.extension(), QStringLiteral(".db"));
        }
    }

    void invalidBinaryAndFleetDatabaseAreRejected() {
        const auto good = sqliteFixture(true);
        QVERIFY(good.ok);
        QByteArray corrupt = good.value;
        corrupt[100] = char(-1); // Invalid root b-tree page, valid outer database header.
        const auto fleet = sqliteFixture(false, {}, false);
        QVERIFY(fleet.ok);
        const QList<QByteArray> invalid = {
            {},
            QByteArrayLiteral("<html>synthetic-secret-login-form</html>"),
            QByteArrayLiteral("CREATE TABLE inbounds(id INTEGER); INSERT INTO inbounds VALUES(1);"),
            QByteArrayLiteral("PGDMP"),
            QByteArray("SQLite format 3\0", 16),
            good.value.left(good.value.size() - 1),
            corrupt,
            fleet.value};
        for (const auto& bytes : invalid) {
            const auto rejected = inspectDatabaseBackup(bytes);
            QVERIFY(!rejected.ok);
            QVERIFY(!rejected.error.isEmpty());
            QVERIFY(!rejected.error.contains(QStringLiteral("synthetic-secret-login-form")));
        }
    }

    void superficialTablesAndViewsAreRejected() {
        const auto wrongColumns = sqliteFixture(
            false, {QStringLiteral("ALTER TABLE users RENAME COLUMN username TO unrelated")});
        QVERIFY(wrongColumns.ok);
        QVERIFY(!inspectDatabaseBackup(wrongColumns.value).ok);
        const auto view = sqliteFixture(
            false,
            {QStringLiteral("ALTER TABLE settings RENAME TO actual_settings"),
             QStringLiteral("CREATE VIEW settings AS SELECT key,value FROM actual_settings")});
        QVERIFY(view.ok);
        QVERIFY(!inspectDatabaseBackup(view.value).ok);
        const auto optionalUnknown = sqliteFixture(
            false, {QStringLiteral("UPDATE inbounds SET settings='invalid-json' WHERE id=1")});
        QVERIFY(optionalUnknown.ok);
        const auto inspected = inspectDatabaseBackup(optionalUnknown.value);
        QVERIFY(inspected.ok);
        QCOMPARE(inspected.value.clientCount, qint64(-1));
    }

    void postgresIsHeaderRecognitionWithUnknownCounts() {
        for (const int minor : {14, 15, 16}) {
            const QByteArray data = postgresFixture(minor);
            const auto info = inspectDatabaseBackup(data);
            QVERIFY2(info.ok, qPrintable(info.error));
            QCOMPARE(info.value.kind, BackupKind::PostgreSQL);
            QCOMPARE(info.value.inboundCount, qint64(-1));
            QCOMPARE(info.value.clientCount, qint64(-1));
            QCOMPARE(info.value.extension(), QStringLiteral(".dump"));
        }
        QByteArray corrupt = postgresFixture();
        corrupt[8] = char(0);
        QVERIFY(!inspectDatabaseBackup(corrupt).ok);
        corrupt = postgresFixture();
        corrupt[10] = char(2);
        QVERIFY(!inspectDatabaseBackup(corrupt).ok);
        QVERIFY(!inspectDatabaseBackup(postgresFixture().left(12)).ok);
        // Content is deliberately opaque: recognizing the header never promises archive integrity.
        QByteArray opaque = postgresFixture();
        opaque[opaque.size() - 1] = char(0x7f);
        QVERIFY(inspectDatabaseBackup(opaque).ok);
    }

    void sizeLimitAndCappedRead() {
        {
            const QByteArray huge(static_cast<qsizetype>(MaximumBackupBytes + 1), 'x');
            const auto rejected = inspectDatabaseBackup(huge);
            QVERIFY(!rejected.ok);
            QVERIFY(rejected.error.contains(QStringLiteral("128")));
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile file(temporary.filePath(QStringLiteral("oversized.db")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(MaximumBackupBytes + 1));
        file.close();
        const auto rejected = readDatabaseBackup(file.fileName());
        QVERIFY(!rejected.ok);
        QVERIFY(rejected.value.isEmpty());
        QVERIFY(rejected.error.contains(QStringLiteral("128")));
    }

    void atomicSaveAndReadRejectBadReplacement() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto fixture = sqliteFixture(true);
        QVERIFY(fixture.ok);
        const QString path = temporary.filePath(QStringLiteral("panel.db"));
        const auto saved = saveDatabaseBackup(path, fixture.value);
        QVERIFY2(saved.ok, qPrintable(saved.error));
        const auto loaded = readDatabaseBackup(path);
        QVERIFY2(loaded.ok, qPrintable(loaded.error));
        QCOMPARE(loaded.value, fixture.value);
        QVERIFY(!saveDatabaseBackup(path, QByteArrayLiteral("<html>login failure</html>")).ok);
        QCOMPARE(readDatabaseBackup(path).value, fixture.value);
        QVERIFY(saveDatabaseBackup(path, postgresFixture()).ok);
        QCOMPARE(readDatabaseBackup(path).value, postgresFixture());
        QVERIFY(!saveDatabaseBackup(temporary.path(), fixture.value).ok);
        QVERIFY(!saveDatabaseBackup({}, fixture.value).ok);
        QVERIFY(!saveDatabaseBackup(temporary.filePath(QStringLiteral("absent/panel.db")),
                                    fixture.value)
                     .ok);
        QVERIFY(!readDatabaseBackup(temporary.filePath(QStringLiteral("missing.db"))).ok);

        const QString invalidPath = temporary.filePath(QStringLiteral("html.db"));
        QFile invalid(invalidPath);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QVERIFY(invalid.write("<html>synthetic-secret</html>") > 0);
        invalid.close();
        const auto rejected = readDatabaseBackup(invalidPath);
        QVERIFY(!rejected.ok);
        QVERIFY(!rejected.error.contains(QStringLiteral("synthetic-secret")));
    }

    void inspectionReleasesNamedConnectionsOnSuccessAndFailure() {
        const QStringList baseline = QSqlDatabase::connectionNames();
        const auto fixture = sqliteFixture(true);
        QVERIFY(fixture.ok);
        QByteArray corrupt = fixture.value;
        corrupt[100] = char(-1);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(inspectDatabaseBackup(fixture.value).ok);
            QVERIFY(!inspectDatabaseBackup(corrupt).ok);
            auto names = QSqlDatabase::connectionNames();
            names.sort();
            auto expected = baseline;
            expected.sort();
            QCOMPARE(names, expected);
        }
    }
};

QTEST_GUILESS_MAIN(BackupTests)
#include "backup_tests.moc"
