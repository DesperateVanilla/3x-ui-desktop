#include "local_store.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaType>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QThread>
#include <QTimeZone>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// wincrypt.h requires Windows type declarations before it is included.
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#endif

namespace fleet {
namespace {
constexpr int SchemaVersion = 1;
constexpr qint64 DuplicateWindowMs = 20'000;
constexpr qint64 HistoryLimit = 100'000;

QString sqlError(const QString& operation, const QSqlError& error) {
    // Database diagnostics can contain SQL or bound values. Only expose a numeric code.
    bool numeric = false;
    const int code = error.nativeErrorCode().toInt(&numeric);
    return numeric ? operation + QStringLiteral(" (код SQLite %1).").arg(code)
                   : operation + QStringLiteral(".");
}

QString nonNullString(const QString& value) {
    return value.isNull() ? QStringLiteral("") : value;
}

class Transaction {
  public:
    explicit Transaction(QSqlDatabase& database)
        : database_(database), active_(database_.transaction()) {}
    ~Transaction() {
        if (active_)
            database_.rollback();
    }
    bool started() const { return active_; }
    bool commit() {
        if (!active_ || !database_.commit())
            return false;
        active_ = false;
        return true;
    }

  private:
    QSqlDatabase& database_;
    bool active_ = false;
};

void releaseConnection(QSqlDatabase& database, const QString& name) {
    if (database.isValid())
        database.close();
    database = QSqlDatabase();
    if (!name.isEmpty())
        QSqlDatabase::removeDatabase(name);
}

OperationResult initializeDatabase(QSqlDatabase& database, bool inMemory) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout = 5000")))
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось настроить ожидание SQLite"), query.lastError()));
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys = ON")) ||
        !query.exec(QStringLiteral("PRAGMA foreign_keys")) || !query.next() ||
        query.value(0).toInt() != 1)
        return OperationResult::failure(
            QStringLiteral("Не удалось включить внешние ключи SQLite."));
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA journal_mode = WAL")) || !query.next())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось включить журнал WAL"), query.lastError()));
    const QString journalMode = query.value(0).toString().toLower();
    query.finish();
    if (journalMode != QStringLiteral("wal") &&
        !(inMemory && journalMode == QStringLiteral("memory")))
        return OperationResult::failure(QStringLiteral("База SQLite не поддерживает журнал WAL."));

    Transaction transaction(database);
    if (!transaction.started())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось начать обновление схемы"), database.lastError()));
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось прочитать версию схемы"), query.lastError()));
    const int version = query.value(0).toInt();
    query.finish();
    if (version < 0 || version > SchemaVersion)
        return OperationResult::failure(
            QStringLiteral("Версия локальной базы не поддерживается этим приложением."));

    if (version == 0) {
        const QStringList statements = {
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS servers ("
                "id TEXT NOT NULL PRIMARY KEY CHECK(length(id) > 0),"
                "name TEXT NOT NULL, location TEXT NOT NULL, panel_url TEXT NOT NULL,"
                "subscription_url TEXT NOT NULL, auth INTEGER NOT NULL CHECK(auth IN (0,1)),"
                "api INTEGER NOT NULL CHECK(api IN (0,1,2)),"
                "allow_http INTEGER NOT NULL CHECK(allow_http IN (0,1)),"
                "credentials BLOB NOT NULL CHECK(typeof(credentials) = 'blob'))"),
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS snapshots ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "server_id TEXT NOT NULL REFERENCES servers(id) ON DELETE CASCADE,"
                "at INTEGER NOT NULL, health INTEGER NOT NULL CHECK(health IN (0,1,2,3)),"
                "cpu REAL, memory_percent REAL, disk_percent REAL, rx_bps REAL, tx_bps REAL,"
                "received_bytes INTEGER, sent_bytes INTEGER, online INTEGER, latency_ms INTEGER,"
                "panel_version TEXT NOT NULL DEFAULT '', xray_version TEXT NOT NULL DEFAULT '',"
                "uptime_seconds INTEGER NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '')"),
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS snapshots_server_at ON snapshots(server_id, at)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS snapshots_at ON snapshots(at)"),
            // No FK here: audit history remains useful after deleting a local connection.
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS events ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL,"
                "server_id TEXT NOT NULL, server_name TEXT NOT NULL, action TEXT NOT NULL,"
                "success INTEGER NOT NULL CHECK(success IN (0,1)), detail TEXT NOT NULL)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS events_at ON events(at DESC, id DESC)")};
        for (const auto& statement : statements) {
            if (!query.exec(statement))
                return OperationResult::failure(sqlError(
                    QStringLiteral("Не удалось создать схему локальной базы"), query.lastError()));
        }
    }

    // Reject incomplete/foreign schemas instead of silently claiming a successful migration.
    const QStringList validations = {
        QStringLiteral(
            "SELECT id,name,location,panel_url,subscription_url,auth,api,allow_http,credentials "
            "FROM servers WHERE 0"),
        QStringLiteral("SELECT "
                       "id,server_id,at,health,cpu,memory_percent,disk_percent,rx_bps,tx_bps,"
                       "received_bytes,sent_bytes,online,latency_ms,panel_version,xray_version,"
                       "uptime_seconds,error FROM snapshots WHERE 0"),
        QStringLiteral(
            "SELECT id,at,server_id,server_name,action,success,detail FROM events WHERE 0")};
    for (const auto& statement : validations) {
        if (!query.exec(statement))
            return OperationResult::failure(
                sqlError(QStringLiteral("Схема локальной базы повреждена или несовместима"),
                         query.lastError()));
        query.finish();
    }
    if (!query.exec(QStringLiteral("PRAGMA foreign_key_list(snapshots)")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось проверить связи локальной базы"), query.lastError()));
    bool cascade = false;
    while (query.next()) {
        if (query.value(2).toString() == QStringLiteral("servers") &&
            query.value(3).toString() == QStringLiteral("server_id") &&
            query.value(4).toString() == QStringLiteral("id") &&
            query.value(6).toString().compare(QStringLiteral("CASCADE"), Qt::CaseInsensitive) == 0)
            cascade = true;
    }
    if (query.lastError().isValid())
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось проверить связи локальной базы"), query.lastError()));
    query.finish();
    if (!cascade)
        return OperationResult::failure(
            QStringLiteral("В локальной базе отсутствует каскадное удаление истории."));
    if (version == 0 && !query.exec(QStringLiteral("PRAGMA user_version = 1")))
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось записать версию схемы"), query.lastError()));
    query.finish();
    if (!transaction.commit())
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось завершить обновление схемы"), database.lastError()));
    return OperationResult::success(true);
}

template <class T> QVariant nullable(const std::optional<T>& value) {
    return value ? QVariant::fromValue(*value) : QVariant(QMetaType::fromType<T>());
}

template <class T> bool readOptional(const QVariant& value, std::optional<T>& destination) {
    if (value.isNull()) {
        destination.reset();
        return true;
    }
    bool valid = false;
    if constexpr (std::is_same_v<T, double>) {
        const double number = value.toDouble(&valid);
        if (!valid || !std::isfinite(number))
            return false;
        destination = number;
    } else if constexpr (std::is_same_v<T, qint64>) {
        const qint64 number = value.toLongLong(&valid);
        if (!valid)
            return false;
        destination = number;
    } else {
        const int number = value.toInt(&valid);
        if (!valid)
            return false;
        destination = number;
    }
    return true;
}

bool finite(const std::optional<double>& number) {
    return !number || std::isfinite(*number);
}

QString snapshotColumns() {
    return QStringLiteral("server_id,at,health,cpu,memory_percent,disk_percent,rx_bps,tx_bps,"
                          "received_bytes,sent_bytes,online,latency_ms,panel_version,xray_version,"
                          "uptime_seconds,error");
}
} // namespace

LocalStore::LocalStore() : ownerThread_(QThread::currentThread()) {}

LocalStore::~LocalStore() {
    closeDatabase();
}

void LocalStore::closeDatabase() {
    releaseConnection(db_, connectionName_);
    connectionName_.clear();
}

QString LocalStore::accessError() const {
    if (ownerThread_ != QThread::currentThread())
        return QStringLiteral(
            "Локальная база доступна только в потоке, в котором создано хранилище.");
    if (!db_.isValid() || !db_.isOpen())
        return QStringLiteral("Локальная база ещё не открыта.");
    return {};
}

OperationResult LocalStore::open(const QString& filePath) {
    if (ownerThread_ != QThread::currentThread())
        return OperationResult::failure(
            QStringLiteral("Локальную базу нужно открыть в потоке хранилища."));
    if (filePath.trimmed().isEmpty())
        return OperationResult::failure(QStringLiteral("Не указан файл локальной базы."));
    const bool inMemory = filePath == QStringLiteral(":memory:");
    if (!inMemory) {
        const QFileInfo file(filePath);
        if (file.isDir() || !QDir().mkpath(file.absolutePath()))
            return OperationResult::failure(
                QStringLiteral("Не удалось создать каталог локальной базы."));
    }
    const QString candidateName =
        QStringLiteral("fleet-store-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase candidate = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), candidateName);
    candidate.setDatabaseName(filePath);
    candidate.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    OperationResult result =
        candidate.open()
            ? initializeDatabase(candidate, inMemory)
            : OperationResult::failure(sqlError(QStringLiteral("Не удалось открыть локальную базу"),
                                                candidate.lastError()));
    if (!result.ok) {
        releaseConnection(candidate, candidateName);
        return result;
    }
    closeDatabase();
    connectionName_ = candidateName;
    db_ = candidate;
    candidate = QSqlDatabase();
    return OperationResult::success(true);
}

Outcome<QList<ServerConfig>> LocalStore::servers() const {
    const QString error = accessError();
    if (!error.isEmpty())
        return Outcome<QList<ServerConfig>>::failure(error);
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral(
            "SELECT id,name,location,panel_url,subscription_url,auth,api,allow_http,credentials "
            "FROM servers ORDER BY name COLLATE NOCASE,id")))
        return Outcome<QList<ServerConfig>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать подключения"), query.lastError()));
    QList<ServerConfig> result;
    while (query.next()) {
        if (query.value(8).metaType().id() != QMetaType::QByteArray)
            return Outcome<QList<ServerConfig>>::failure(
                QStringLiteral("Зашифрованные учётные данные имеют неверный формат."));
        auto decrypted = unprotect(query.value(8).toByteArray());
        if (!decrypted.ok)
            return Outcome<QList<ServerConfig>>::failure(decrypted.error);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(decrypted.value, &parseError);
        decrypted.value.fill('\0');
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return Outcome<QList<ServerConfig>>::failure(
                QStringLiteral("Расшифрованные учётные данные повреждены."));
        const QJsonObject credentials = document.object();
        if (!credentials.value(QStringLiteral("username")).isString() ||
            !credentials.value(QStringLiteral("password")).isString() ||
            !credentials.value(QStringLiteral("token")).isString())
            return Outcome<QList<ServerConfig>>::failure(
                QStringLiteral("Расшифрованные учётные данные имеют неверную структуру."));
        bool authValid = false, apiValid = false;
        const int auth = query.value(5).toInt(&authValid);
        const int api = query.value(6).toInt(&apiValid);
        if (!authValid || auth < 0 || auth > 1 || !apiValid || api < 0 || api > 2)
            return Outcome<QList<ServerConfig>>::failure(
                QStringLiteral("Настройки подключения в локальной базе повреждены."));
        ServerConfig config;
        config.id = query.value(0).toString();
        config.name = query.value(1).toString();
        config.location = query.value(2).toString();
        config.panelUrl = QUrl(query.value(3).toString());
        config.subscriptionUrl = QUrl(query.value(4).toString());
        config.auth = static_cast<AuthKind>(auth);
        config.api = static_cast<ApiFlavor>(api);
        config.allowHttp = query.value(7).toBool();
        config.username = credentials.value(QStringLiteral("username")).toString();
        config.password = credentials.value(QStringLiteral("password")).toString();
        config.token = credentials.value(QStringLiteral("token")).toString();
        result.append(std::move(config));
    }
    if (query.lastError().isValid())
        return Outcome<QList<ServerConfig>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать подключения"), query.lastError()));
    return Outcome<QList<ServerConfig>>::success(std::move(result));
}

OperationResult LocalStore::saveServer(const ServerConfig& config) {
    const QString error = accessError();
    if (!error.isEmpty())
        return OperationResult::failure(error);
    if (config.id.trimmed().isEmpty())
        return OperationResult::failure(QStringLiteral("У подключения отсутствует идентификатор."));
    const int auth = static_cast<int>(config.auth), api = static_cast<int>(config.api);
    if (auth < 0 || auth > 1 || api < 0 || api > 2)
        return OperationResult::failure(QStringLiteral("Неверный способ подключения к панели."));
    const QString scheme = config.panelUrl.scheme().toLower();
    if (!config.panelUrl.isValid() || config.panelUrl.host().isEmpty() ||
        !(scheme == QStringLiteral("https") ||
          (scheme == QStringLiteral("http") && config.allowHttp)) ||
        !config.panelUrl.userInfo().isEmpty() || config.panelUrl.hasQuery() ||
        config.panelUrl.hasFragment())
        return OperationResult::failure(
            QStringLiteral("Неверный URL панели: нужен HTTPS без встроенных учётных данных, "
                           "параметров и фрагмента."));
    const QJsonObject credentials{{QStringLiteral("username"), config.username},
                                  {QStringLiteral("password"), config.password},
                                  {QStringLiteral("token"), config.token}};
    QByteArray plaintext = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
    auto encrypted = protect(plaintext);
    plaintext.fill('\0');
    if (!encrypted.ok)
        return OperationResult::failure(encrypted.error);
    QSqlQuery query(db_);
    if (!query.prepare(QStringLiteral(
            "INSERT INTO servers "
            "(id,name,location,panel_url,subscription_url,auth,api,allow_http,credentials) "
            "VALUES (:id,:name,:location,:panel,:subscription,:auth,:api,:http,:credentials) "
            "ON CONFLICT(id) DO UPDATE SET name=excluded.name,location=excluded.location,"
            "panel_url=excluded.panel_url,subscription_url=excluded.subscription_url,"
            "auth=excluded.auth,api=excluded.api,allow_http=excluded.allow_http,credentials="
            "excluded.credentials")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось подготовить сохранение подключения"), query.lastError()));
    query.bindValue(QStringLiteral(":id"), config.id);
    query.bindValue(QStringLiteral(":name"), nonNullString(config.name));
    query.bindValue(QStringLiteral(":location"), nonNullString(config.location));
    query.bindValue(QStringLiteral(":panel"),
                    nonNullString(config.panelUrl.toString(QUrl::FullyEncoded)));
    query.bindValue(QStringLiteral(":subscription"),
                    nonNullString(config.subscriptionUrl.toString(QUrl::FullyEncoded)));
    query.bindValue(QStringLiteral(":auth"), auth);
    query.bindValue(QStringLiteral(":api"), api);
    query.bindValue(QStringLiteral(":http"), config.allowHttp ? 1 : 0);
    query.bindValue(QStringLiteral(":credentials"), encrypted.value);
    if (!query.exec())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось сохранить подключение"), query.lastError()));
    return OperationResult::success(true);
}

OperationResult LocalStore::removeServer(const QString& serverId) {
    const QString error = accessError();
    if (!error.isEmpty())
        return OperationResult::failure(error);
    if (serverId.trimmed().isEmpty())
        return OperationResult::failure(
            QStringLiteral("Не указан идентификатор удаляемого подключения."));
    Transaction transaction(db_);
    if (!transaction.started())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось начать удаление подключения"), db_.lastError()));
    QSqlQuery query(db_);
    if (!query.prepare(QStringLiteral("DELETE FROM servers WHERE id = :id")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось подготовить удаление подключения"), query.lastError()));
    query.bindValue(QStringLiteral(":id"), serverId);
    if (!query.exec())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось удалить подключение"), query.lastError()));
    if (query.numRowsAffected() == 0)
        return OperationResult::failure(QStringLiteral("Подключение для удаления не найдено."));
    query.finish();
    if (!transaction.commit())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось завершить удаление подключения"), db_.lastError()));
    return OperationResult::success(true);
}

OperationResult LocalStore::appendSnapshot(const Snapshot& snapshot) {
    const QString error = accessError();
    if (!error.isEmpty())
        return OperationResult::failure(error);
    const int health = static_cast<int>(snapshot.health);
    if (snapshot.serverId.trimmed().isEmpty() || !snapshot.at.isValid() || health < 0 || health > 3)
        return OperationResult::failure(
            QStringLiteral("Неверный идентификатор, время или статус замера."));
    if (!finite(snapshot.cpu) || !finite(snapshot.memoryPercent) || !finite(snapshot.diskPercent) ||
        !finite(snapshot.rxBps) || !finite(snapshot.txBps))
        return OperationResult::failure(
            QStringLiteral("Замер содержит некорректную числовую метрику."));
    Transaction transaction(db_);
    if (!transaction.started())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось начать сохранение замера"), db_.lastError()));
    QSqlQuery query(db_);
    if (!query.prepare(QStringLiteral("SELECT 1 FROM servers WHERE id = :id")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось проверить подключение для замера"), query.lastError()));
    query.bindValue(QStringLiteral(":id"), snapshot.serverId);
    if (!query.exec())
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось проверить подключение для замера"), query.lastError()));
    if (!query.next()) {
        if (query.lastError().isValid())
            return OperationResult::failure(sqlError(
                QStringLiteral("Не удалось проверить подключение для замера"), query.lastError()));
        return OperationResult::failure(QStringLiteral("Подключение для замера не найдено."));
    }
    query.finish();
    const qint64 at = snapshot.at.toMSecsSinceEpoch();
    const qint64 radius =
        DuplicateWindowMs - 1; // Millisecond timestamps: exactly 20s is a new sample.
    const qint64 lower = at < std::numeric_limits<qint64>::min() + radius
                             ? std::numeric_limits<qint64>::min()
                             : at - radius;
    const qint64 upper = at > std::numeric_limits<qint64>::max() - radius
                             ? std::numeric_limits<qint64>::max()
                             : at + radius;
    if (!query.prepare(QStringLiteral("SELECT 1 FROM snapshots WHERE server_id = :id AND at >= "
                                      ":lower AND at <= :upper LIMIT 1")))
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось проверить повторный замер"), query.lastError()));
    query.bindValue(QStringLiteral(":id"), snapshot.serverId);
    query.bindValue(QStringLiteral(":lower"), lower);
    query.bindValue(QStringLiteral(":upper"), upper);
    if (!query.exec())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось проверить повторный замер"), query.lastError()));
    if (query.next())
        return OperationResult::success(false);
    if (query.lastError().isValid())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось проверить повторный замер"), query.lastError()));
    query.finish();
    if (!query.prepare(
            QStringLiteral("INSERT INTO snapshots "
                           "(server_id,at,health,cpu,memory_percent,disk_percent,rx_bps,tx_bps,"
                           "received_bytes,sent_bytes,online,latency_ms,panel_version,xray_version,"
                           "uptime_seconds,error) "
                           "VALUES "
                           "(:server,:at,:health,:cpu,:memory,:disk,:rx,:tx,:received,:sent,:"
                           "online,:latency,:panel,:xray,:uptime,:error)")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось подготовить сохранение замера"), query.lastError()));
    query.bindValue(QStringLiteral(":server"), snapshot.serverId);
    query.bindValue(QStringLiteral(":at"), at);
    query.bindValue(QStringLiteral(":health"), health);
    query.bindValue(QStringLiteral(":cpu"), nullable(snapshot.cpu));
    query.bindValue(QStringLiteral(":memory"), nullable(snapshot.memoryPercent));
    query.bindValue(QStringLiteral(":disk"), nullable(snapshot.diskPercent));
    query.bindValue(QStringLiteral(":rx"), nullable(snapshot.rxBps));
    query.bindValue(QStringLiteral(":tx"), nullable(snapshot.txBps));
    query.bindValue(QStringLiteral(":received"), nullable(snapshot.receivedBytes));
    query.bindValue(QStringLiteral(":sent"), nullable(snapshot.sentBytes));
    query.bindValue(QStringLiteral(":online"), nullable(snapshot.online));
    query.bindValue(QStringLiteral(":latency"), nullable(snapshot.latencyMs));
    query.bindValue(QStringLiteral(":panel"), nonNullString(snapshot.panelVersion));
    query.bindValue(QStringLiteral(":xray"), nonNullString(snapshot.xrayVersion));
    query.bindValue(QStringLiteral(":uptime"), snapshot.uptimeSeconds);
    query.bindValue(QStringLiteral(":error"), nonNullString(snapshot.error));
    if (!query.exec())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось сохранить замер"), query.lastError()));
    query.finish();
    if (!transaction.commit())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось завершить сохранение замера"), db_.lastError()));
    return OperationResult::success(true);
}

Outcome<QList<Snapshot>> LocalStore::history(const QString& serverId,
                                             const QDateTime& since) const {
    const QString error = accessError();
    if (!error.isEmpty())
        return Outcome<QList<Snapshot>>::failure(error);
    if (!since.isValid())
        return Outcome<QList<Snapshot>>::failure(
            QStringLiteral("Неверная начальная дата истории."));
    // Metadata and points must share a read snapshot even if another connection is writing.
    QSqlDatabase database = db_;
    Transaction transaction(database);
    if (!transaction.started())
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось начать чтение истории"), database.lastError()));
    const QString where = serverId.isEmpty()
                              ? QStringLiteral("at >= :since")
                              : QStringLiteral("at >= :since AND server_id = :server");
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.prepare(
            QStringLiteral(
                "SELECT COUNT(*),MIN(at),MAX(at),COUNT(DISTINCT server_id) FROM snapshots WHERE ") +
            where))
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось подготовить чтение истории"), query.lastError()));
    query.bindValue(QStringLiteral(":since"), since.toMSecsSinceEpoch());
    if (!serverId.isEmpty())
        query.bindValue(QStringLiteral(":server"), serverId);
    if (!query.exec() || !query.next())
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать размер истории"), query.lastError()));
    const qint64 count = query.value(0).toLongLong();
    const qint64 firstAt = query.value(1).toLongLong(), lastAt = query.value(2).toLongLong();
    const qint64 serverCount = query.value(3).toLongLong();
    query.finish();
    QString statement;
    qint64 bucketMs = 0;
    if (count > HistoryLimit) {
        if (serverCount <= 0 || serverCount > HistoryLimit)
            return Outcome<QList<Snapshot>>::failure(
                QStringLiteral("Слишком много серверов для одного запроса истории."));
        const qint64 bucketCount = HistoryLimit / serverCount;
        const long double span =
            static_cast<long double>(lastAt) - static_cast<long double>(firstAt) + 1.0L;
        const long double bucket = std::ceil(span / static_cast<long double>(bucketCount));
        if (bucket >= static_cast<long double>(std::numeric_limits<qint64>::max()))
            return Outcome<QList<Snapshot>>::failure(
                QStringLiteral("Диапазон истории слишком велик."));
        bucketMs = (std::max)(qint64(60'000), static_cast<qint64>(bucket));
        const QString partition =
            QStringLiteral("server_id,CAST((at - :origin) / :bucket AS INTEGER)");
        statement =
            QStringLiteral("WITH ranked AS (SELECT id,") + snapshotColumns() +
            QStringLiteral(",ROW_NUMBER() OVER (PARTITION BY ") + partition +
            QStringLiteral(
                " ORDER BY at DESC,id DESC) AS row_number,MAX(health) OVER (PARTITION BY ") +
            partition + QStringLiteral(") AS bucket_health FROM snapshots WHERE ") + where +
            QStringLiteral(
                ") SELECT server_id,at,bucket_health,cpu,memory_percent,disk_percent,rx_bps,tx_bps,"
                "received_bytes,sent_bytes,online,latency_ms,panel_version,xray_version,uptime_"
                "seconds,error "
                "FROM ranked WHERE row_number = 1 ORDER BY at ASC,id ASC LIMIT 100000");
    } else {
        statement = QStringLiteral("SELECT ") + snapshotColumns() +
                    QStringLiteral(" FROM snapshots WHERE ") + where +
                    QStringLiteral(" ORDER BY at ASC,id ASC LIMIT 100000");
    }
    if (!query.prepare(statement))
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось подготовить точки истории"), query.lastError()));
    query.bindValue(QStringLiteral(":since"), since.toMSecsSinceEpoch());
    if (!serverId.isEmpty())
        query.bindValue(QStringLiteral(":server"), serverId);
    if (bucketMs != 0) {
        query.bindValue(QStringLiteral(":origin"), firstAt);
        query.bindValue(QStringLiteral(":bucket"), bucketMs);
    }
    if (!query.exec())
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать историю"), query.lastError()));
    QList<Snapshot> result;
    while (query.next()) {
        Snapshot snapshot;
        snapshot.serverId = query.value(0).toString();
        bool atValid = false, healthValid = false, uptimeValid = false;
        const qint64 at = query.value(1).toLongLong(&atValid);
        const int health = query.value(2).toInt(&healthValid);
        snapshot.at = QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC);
        if (!atValid || !snapshot.at.isValid() || !healthValid || health < 0 || health > 3 ||
            !readOptional(query.value(3), snapshot.cpu) ||
            !readOptional(query.value(4), snapshot.memoryPercent) ||
            !readOptional(query.value(5), snapshot.diskPercent) ||
            !readOptional(query.value(6), snapshot.rxBps) ||
            !readOptional(query.value(7), snapshot.txBps) ||
            !readOptional(query.value(8), snapshot.receivedBytes) ||
            !readOptional(query.value(9), snapshot.sentBytes) ||
            !readOptional(query.value(10), snapshot.online) ||
            !readOptional(query.value(11), snapshot.latencyMs))
            return Outcome<QList<Snapshot>>::failure(
                QStringLiteral("История содержит повреждённый замер."));
        snapshot.health = static_cast<Health>(health);
        snapshot.panelVersion = query.value(12).toString();
        snapshot.xrayVersion = query.value(13).toString();
        snapshot.uptimeSeconds = query.value(14).toLongLong(&uptimeValid);
        snapshot.error = query.value(15).toString();
        if (!uptimeValid)
            return Outcome<QList<Snapshot>>::failure(
                QStringLiteral("История содержит повреждённое время работы сервера."));
        result.append(std::move(snapshot));
    }
    if (query.lastError().isValid())
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать историю"), query.lastError()));
    query.finish();
    if (!transaction.commit())
        return Outcome<QList<Snapshot>>::failure(
            sqlError(QStringLiteral("Не удалось завершить чтение истории"), database.lastError()));
    return Outcome<QList<Snapshot>>::success(std::move(result));
}

OperationResult LocalStore::appendEvent(const ActivityEvent& event) {
    const QString error = accessError();
    if (!error.isEmpty())
        return OperationResult::failure(error);
    if (!event.at.isValid())
        return OperationResult::failure(QStringLiteral("Неверная дата события."));
    QSqlQuery query(db_);
    if (!query.prepare(
            QStringLiteral("INSERT INTO events (at,server_id,server_name,action,success,detail) "
                           "VALUES (:at,:server,:name,:action,:success,:detail)")))
        return OperationResult::failure(sqlError(
            QStringLiteral("Не удалось подготовить сохранение события"), query.lastError()));
    query.bindValue(QStringLiteral(":at"), event.at.toMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":server"), nonNullString(event.serverId));
    query.bindValue(QStringLiteral(":name"), nonNullString(event.serverName));
    query.bindValue(QStringLiteral(":action"), nonNullString(event.action));
    query.bindValue(QStringLiteral(":success"), event.success ? 1 : 0);
    query.bindValue(QStringLiteral(":detail"), nonNullString(event.detail));
    if (!query.exec())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось сохранить событие"), query.lastError()));
    return OperationResult::success(true);
}

Outcome<QList<ActivityEvent>> LocalStore::events(int limit) const {
    const QString error = accessError();
    if (!error.isEmpty())
        return Outcome<QList<ActivityEvent>>::failure(error);
    if (limit < 0)
        return Outcome<QList<ActivityEvent>>::failure(
            QStringLiteral("Лимит событий не может быть отрицательным."));
    if (limit == 0)
        return Outcome<QList<ActivityEvent>>::success({});
    QSqlQuery query(db_);
    query.setForwardOnly(true);
    if (!query.prepare(
            QStringLiteral("SELECT at,server_id,server_name,action,success,detail FROM events "
                           "ORDER BY at DESC,id DESC LIMIT :limit")))
        return Outcome<QList<ActivityEvent>>::failure(
            sqlError(QStringLiteral("Не удалось подготовить чтение событий"), query.lastError()));
    query.bindValue(QStringLiteral(":limit"), (std::min)(limit, static_cast<int>(HistoryLimit)));
    if (!query.exec())
        return Outcome<QList<ActivityEvent>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать события"), query.lastError()));
    QList<ActivityEvent> result;
    while (query.next()) {
        bool atValid = false, successValid = false;
        const qint64 at = query.value(0).toLongLong(&atValid);
        const int success = query.value(4).toInt(&successValid);
        ActivityEvent event;
        event.at = QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC);
        if (!atValid || !event.at.isValid() || !successValid || success < 0 || success > 1)
            return Outcome<QList<ActivityEvent>>::failure(
                QStringLiteral("Журнал содержит повреждённое событие."));
        event.serverId = query.value(1).toString();
        event.serverName = query.value(2).toString();
        event.action = query.value(3).toString();
        event.success = success != 0;
        event.detail = query.value(5).toString();
        result.append(std::move(event));
    }
    if (query.lastError().isValid())
        return Outcome<QList<ActivityEvent>>::failure(
            sqlError(QStringLiteral("Не удалось прочитать события"), query.lastError()));
    return Outcome<QList<ActivityEvent>>::success(std::move(result));
}

OperationResult LocalStore::prune(const QDateTime& before) {
    const QString error = accessError();
    if (!error.isEmpty())
        return OperationResult::failure(error);
    if (!before.isValid())
        return OperationResult::failure(QStringLiteral("Неверная дата очистки истории."));
    Transaction transaction(db_);
    if (!transaction.started())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось начать очистку истории"), db_.lastError()));
    QSqlQuery query(db_);
    for (const QString& table : {QStringLiteral("snapshots"), QStringLiteral("events")}) {
        if (!query.prepare(QStringLiteral("DELETE FROM ") + table +
                           QStringLiteral(" WHERE at < :before")))
            return OperationResult::failure(sqlError(
                QStringLiteral("Не удалось подготовить очистку истории"), query.lastError()));
        query.bindValue(QStringLiteral(":before"), before.toMSecsSinceEpoch());
        if (!query.exec())
            return OperationResult::failure(
                sqlError(QStringLiteral("Не удалось очистить историю"), query.lastError()));
        query.finish();
    }
    if (!transaction.commit())
        return OperationResult::failure(
            sqlError(QStringLiteral("Не удалось завершить очистку истории"), db_.lastError()));
    return OperationResult::success(true);
}

Outcome<QByteArray> LocalStore::protect(const QByteArray& plaintext) {
#ifdef Q_OS_WIN
    if (static_cast<quint64>(plaintext.size()) > std::numeric_limits<DWORD>::max())
        return Outcome<QByteArray>::failure(
            QStringLiteral("Учётные данные слишком велики для Windows DPAPI."));
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"3X Control credentials", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return Outcome<QByteArray>::failure(
            QStringLiteral("Не удалось зашифровать учётные данные Windows DPAPI (код %1).")
                .arg(static_cast<quint32>(GetLastError())));
    QByteArray ciphertext("3XDP", 4);
    ciphertext.append(char(1));
    ciphertext.append(reinterpret_cast<const char*>(output.pbData),
                      static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return Outcome<QByteArray>::success(std::move(ciphertext));
#else
    Q_UNUSED(plaintext);
    return Outcome<QByteArray>::failure(
        QStringLiteral("Шифрование учётных данных поддерживается только в Windows через DPAPI."));
#endif
}

Outcome<QByteArray> LocalStore::unprotect(const QByteArray& ciphertext) {
#ifdef Q_OS_WIN
    if (ciphertext.size() <= 5 || !ciphertext.startsWith(QByteArray("3XDP", 4)) ||
        ciphertext.at(4) != char(1))
        return Outcome<QByteArray>::failure(
            QStringLiteral("Зашифрованные учётные данные имеют неверный формат или версию."));
    const qsizetype size = ciphertext.size() - 5;
    if (static_cast<quint64>(size) > std::numeric_limits<DWORD>::max())
        return Outcome<QByteArray>::failure(
            QStringLiteral("Зашифрованные учётные данные слишком велики."));
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(size);
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(ciphertext.constData() + 5));
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output))
        return Outcome<QByteArray>::failure(
            QStringLiteral("Не удалось расшифровать учётные данные Windows DPAPI. "
                           "Они повреждены или принадлежат другому пользователю Windows."));
    QByteArray plaintext(reinterpret_cast<const char*>(output.pbData),
                         static_cast<qsizetype>(output.cbData));
    if (output.pbData) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    return Outcome<QByteArray>::success(std::move(plaintext));
#else
    Q_UNUSED(ciphertext);
    return Outcome<QByteArray>::failure(
        QStringLiteral("Расшифровка учётных данных поддерживается только в Windows через DPAPI."));
#endif
}
} // namespace fleet
