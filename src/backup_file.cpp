#include "backup_file.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QUuid>
#include <limits>

namespace fleet {
namespace {
quint32 bigEndian32(const QByteArray& data, qsizetype offset) {
    quint32 value = 0;
    for (qsizetype i = 0; i < 4; ++i)
        value = (value << 8) | static_cast<unsigned char>(data.at(offset + i));
    return value;
}

bool validSQLiteHeader(const QByteArray& data) {
    if (data.size() < 100 || !data.startsWith(QByteArray("SQLite format 3\0", 16)))
        return false;
    int pageSize =
        (static_cast<unsigned char>(data.at(16)) << 8) | static_cast<unsigned char>(data.at(17));
    if (pageSize == 1)
        pageSize = 65536;
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) != 0 ||
        data.size() % pageSize != 0)
        return false;
    const auto readVersion = static_cast<unsigned char>(data.at(19));
    const auto writeVersion = static_cast<unsigned char>(data.at(18));
    if ((readVersion != 1 && readVersion != 2) || (writeVersion != 1 && writeVersion != 2) ||
        pageSize - static_cast<unsigned char>(data.at(20)) < 480 || data.at(21) != char(64) ||
        data.at(22) != char(32) || data.at(23) != char(32))
        return false;
    const quint32 declaredPages = bigEndian32(data, 28);
    // Old SQLite writers can leave a stale page count. The counter pair says whether it is
    // authoritative.
    if (declaredPages != 0 && bigEndian32(data, 24) == bigEndian32(data, 92) &&
        qint64(declaredPages) * pageSize != data.size())
        return false;
    return true;
}

class ReadOnlyDatabase {
  public:
    explicit ReadOnlyDatabase(const QString& path)
        : name_(QStringLiteral("fleet-backup-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces)),
          database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_)) {
        database.setDatabaseName(path);
        database.setConnectOptions(
            QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000"));
        opened = database.open();
    }
    ~ReadOnlyDatabase() {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }
    ReadOnlyDatabase(const ReadOnlyDatabase&) = delete;
    ReadOnlyDatabase& operator=(const ReadOnlyDatabase&) = delete;

  private:
    QString name_;

  public:
    QSqlDatabase database;
    bool opened = false;
};

bool columnsPresent(QSqlDatabase& database, const QString& table, const QSet<QString>& required) {
    // The identifier is a hardcoded table name, never SQL read from the backup.
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(\"") + table + QStringLiteral("\")")))
        return false;
    QSet<QString> columns;
    while (query.next())
        columns.insert(query.value(1).toString().toLower());
    if (query.lastError().isValid())
        return false;
    for (const auto& column : required)
        if (!columns.contains(column))
            return false;
    return true;
}

Outcome<BackupInfo> inspectSQLite(const QString& path, qint64 size) {
    ReadOnlyDatabase connection(path);
    if (!connection.opened)
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось открыть резервную копию SQLite только для чтения."));
    QSqlQuery query(connection.database);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("PRAGMA query_only = ON")) ||
        !query.exec(QStringLiteral("PRAGMA trusted_schema = OFF")) ||
        !query.exec(QStringLiteral("PRAGMA query_only")) || !query.next() ||
        query.value(0).toInt() != 1)
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось включить безопасное чтение резервной копии SQLite."));
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA trusted_schema")) || !query.next() ||
        query.value(0).toInt() != 0)
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось отключить доверие к схеме резервной копии SQLite."));
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA quick_check(1)")) || !query.next() ||
        query.value(0).toString() != QStringLiteral("ok"))
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Резервная копия SQLite повреждена или не прошла quick_check."));
    if (query.next() || query.lastError().isValid())
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Резервная копия SQLite не прошла quick_check."));
    query.finish();
    if (!query.prepare(
            QStringLiteral("SELECT lower(name),type,sql FROM sqlite_schema "
                           "WHERE lower(name) IN (:settings,:inbounds,:users,:clients)")))
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось прочитать структуру резервной копии."));
    query.bindValue(QStringLiteral(":settings"), QStringLiteral("settings"));
    query.bindValue(QStringLiteral(":inbounds"), QStringLiteral("inbounds"));
    query.bindValue(QStringLiteral(":users"), QStringLiteral("users"));
    query.bindValue(QStringLiteral(":clients"), QStringLiteral("clients"));
    if (!query.exec())
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось прочитать структуру резервной копии."));
    QSet<QString> tables;
    while (query.next()) {
        const QString declaration = query.value(2).toString().trimmed();
        if (query.value(1).toString() != QStringLiteral("table") ||
            !declaration.startsWith(QStringLiteral("CREATE TABLE"), Qt::CaseInsensitive))
            return Outcome<BackupInfo>::failure(QStringLiteral(
                "Резервная копия содержит неподдерживаемые таблицы или представления."));
        tables.insert(query.value(0).toString());
    }
    if (query.lastError().isValid())
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось прочитать структуру резервной копии."));
    query.finish();
    if (!tables.contains(QStringLiteral("settings")) ||
        !tables.contains(QStringLiteral("inbounds")) || !tables.contains(QStringLiteral("users")) ||
        !columnsPresent(connection.database, QStringLiteral("settings"),
                        {QStringLiteral("key"), QStringLiteral("value")}) ||
        !columnsPresent(
            connection.database, QStringLiteral("users"),
            {QStringLiteral("id"), QStringLiteral("username"), QStringLiteral("password")}) ||
        !columnsPresent(
            connection.database, QStringLiteral("inbounds"),
            {QStringLiteral("id"), QStringLiteral("protocol"), QStringLiteral("settings")}) ||
        (tables.contains(QStringLiteral("clients")) &&
         !columnsPresent(connection.database, QStringLiteral("clients"),
                         {QStringLiteral("id"), QStringLiteral("email")})))
        return Outcome<BackupInfo>::failure(QStringLiteral(
            "Файл SQLite не похож на базу панели 3x-ui. Локальная база приложения не подходит."));
    BackupInfo info;
    info.kind = BackupKind::SQLite;
    info.size = size;
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM inbounds")) || !query.next())
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось прочитать число inbound из резервной копии."));
    bool validCount = false;
    info.inboundCount = query.value(0).toLongLong(&validCount);
    query.finish();
    if (!validCount || info.inboundCount < 0)
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Резервная копия содержит некорректное число inbound."));
    if (tables.contains(QStringLiteral("clients"))) {
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM clients")) || !query.next())
            return Outcome<BackupInfo>::failure(
                QStringLiteral("Не удалось прочитать число клиентов из резервной копии."));
        info.clientCount = query.value(0).toLongLong(&validCount);
        if (!validCount || info.clientCount < 0)
            return Outcome<BackupInfo>::failure(
                QStringLiteral("Резервная копия содержит некорректное число клиентов."));
    } else {
        // V2 stores clients as data in inbounds.settings. Invalid optional JSON leaves the count
        // unknown.
        if (!query.exec(QStringLiteral("SELECT settings FROM inbounds")))
            return Outcome<BackupInfo>::failure(
                QStringLiteral("Не удалось прочитать настройки inbound из резервной копии."));
        info.clientCount = 0;
        bool known = true;
        while (query.next()) {
            QJsonParseError error;
            const auto document =
                QJsonDocument::fromJson(query.value(0).toString().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                known = false;
                continue;
            }
            const auto clients = document.object().value(QStringLiteral("clients"));
            if (clients.isUndefined())
                continue;
            if (!clients.isArray()) {
                known = false;
                continue;
            }
            const qsizetype count = clients.toArray().size();
            if (count > std::numeric_limits<qint64>::max() - info.clientCount) {
                known = false;
                continue;
            }
            info.clientCount += count;
        }
        if (query.lastError().isValid())
            return Outcome<BackupInfo>::failure(
                QStringLiteral("Не удалось прочитать настройки inbound из резервной копии."));
        if (!known)
            info.clientCount = -1;
    }
    return Outcome<BackupInfo>::success(info);
}

bool pgInteger(const QByteArray& data, qsizetype& offset, int integerSize, qint64& value) {
    if (offset < 0 || data.size() - offset < integerSize + 1)
        return false;
    const unsigned char sign = static_cast<unsigned char>(data.at(offset++));
    if (sign > 1)
        return false;
    quint64 magnitude = 0;
    for (int i = 0; i < integerSize; ++i)
        magnitude |= quint64(static_cast<unsigned char>(data.at(offset++))) << (i * 8);
    if (magnitude > quint64(std::numeric_limits<qint64>::max()))
        return false;
    value = sign ? -static_cast<qint64>(magnitude) : static_cast<qint64>(magnitude);
    return true;
}

bool pgString(const QByteArray& data, qsizetype& offset, int integerSize) {
    qint64 size = 0;
    if (!pgInteger(data, offset, integerSize, size) || size < -1 || size > 1024 * 1024 ||
        size > data.size() - offset)
        return false;
    if (size >= 0)
        offset += size;
    return true;
}

bool validPostgreSQLHeader(const QByteArray& data) {
    if (data.size() < 12 || !data.startsWith(QByteArrayLiteral("PGDMP")))
        return false;
    const unsigned char major = static_cast<unsigned char>(data.at(5));
    const unsigned char minor = static_cast<unsigned char>(data.at(6));
    // 3x-ui's custom dumps use versions with an offset-size field. No pg_restore is executed here.
    if (major != 1 || minor < 7 || minor > 16)
        return false;
    const int integerSize = static_cast<unsigned char>(data.at(8));
    const int offsetSize = static_cast<unsigned char>(data.at(9));
    if (integerSize < 1 || integerSize > 8 || offsetSize < 1 || offsetSize > 8 ||
        data.at(10) != char(1))
        return false;
    qsizetype offset = 11;
    if (minor >= 15) {
        const auto compression = static_cast<unsigned char>(data.at(offset++));
        if (compression > 3)
            return false;
    } else {
        qint64 compression = 0;
        if (!pgInteger(data, offset, integerSize, compression) || compression < -1 ||
            compression > 9)
            return false;
    }
    qint64 stamp[7]{};
    for (auto& field : stamp)
        if (!pgInteger(data, offset, integerSize, field))
            return false;
    if (stamp[0] < 0 || stamp[0] > 60 || stamp[1] < 0 || stamp[1] > 59 || stamp[2] < 0 ||
        stamp[2] > 23 || stamp[3] < 1 || stamp[3] > 31 || stamp[4] < 0 || stamp[4] > 11 ||
        stamp[5] < 0 || stamp[5] > 10000 || stamp[6] < -1 || stamp[6] > 1)
        return false;
    if (!pgString(data, offset, integerSize))
        return false;
    if (minor >= 10 &&
        (!pgString(data, offset, integerSize) || !pgString(data, offset, integerSize)))
        return false;
    return offset < data.size(); // There must be archive content after its header; it is not
                                 // integrity-checked.
}
} // namespace

Outcome<BackupInfo> inspectDatabaseBackup(const QByteArray& data) {
    if (data.isEmpty())
        return Outcome<BackupInfo>::failure(QStringLiteral("Резервная копия пуста."));
    if (data.size() > MaximumBackupBytes)
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Резервная копия превышает лимит 128 МиБ."));
    if (data.startsWith(QByteArrayLiteral("PGDMP"))) {
        if (!validPostgreSQLHeader(data))
            return Outcome<BackupInfo>::failure(QStringLiteral(
                "Заголовок резервной копии PostgreSQL PGDMP повреждён или не поддерживается."));
        BackupInfo info;
        info.kind = BackupKind::PostgreSQL;
        info.size = data.size();
        return Outcome<BackupInfo>::success(info);
    }
    if (!validSQLiteHeader(data))
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Файл не является целой бинарной резервной копией SQLite или PGDMP."));
    QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/3x-control-backup-XXXXXX.db"));
    if (!temporary.open() || temporary.write(data) != data.size() || !temporary.flush())
        return Outcome<BackupInfo>::failure(
            QStringLiteral("Не удалось создать временный файл для проверки резервной копии."));
    const QString path = temporary.fileName();
    temporary.close();
    return inspectSQLite(path, data.size());
}

Outcome<QByteArray> readDatabaseBackup(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return Outcome<QByteArray>::failure(
            QStringLiteral("Не удалось открыть файл резервной копии для чтения."));
    const qint64 expectedSize = file.size();
    if (expectedSize <= 0)
        return Outcome<QByteArray>::failure(QStringLiteral("Файл резервной копии пуст."));
    if (expectedSize > MaximumBackupBytes)
        return Outcome<QByteArray>::failure(
            QStringLiteral("Файл резервной копии превышает лимит 128 МиБ."));
    QByteArray data = file.read(MaximumBackupBytes + 1);
    if (file.error() != QFileDevice::NoError || data.size() != expectedSize ||
        data.size() > MaximumBackupBytes || !file.atEnd())
        return Outcome<QByteArray>::failure(
            QStringLiteral("Файл резервной копии изменился или не был прочитан полностью."));
    const auto inspected = inspectDatabaseBackup(data);
    if (!inspected.ok)
        return Outcome<QByteArray>::failure(inspected.error);
    return Outcome<QByteArray>::success(std::move(data));
}

OperationResult saveDatabaseBackup(const QString& path, const QByteArray& data) {
    if (path.trimmed().isEmpty())
        return OperationResult::failure(
            QStringLiteral("Не указан путь сохранения резервной копии."));
    const auto inspected = inspectDatabaseBackup(data);
    if (!inspected.ok)
        return OperationResult::failure(inspected.error);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return OperationResult::failure(
            QStringLiteral("Не удалось открыть файл для атомарного сохранения резервной копии."));
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return OperationResult::failure(
            QStringLiteral("Не удалось полностью записать резервную копию."));
    }
    if (!file.commit())
        return OperationResult::failure(
            QStringLiteral("Не удалось завершить атомарное сохранение резервной копии."));
    return OperationResult::success(true);
}
} // namespace fleet
