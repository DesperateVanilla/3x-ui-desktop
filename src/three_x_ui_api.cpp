#include "three_x_ui_api.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace fleet {
namespace {
constexpr qint64 kResponseLimit = 16 * 1024 * 1024;
constexpr int kRequestTimeoutMs = 8000;

std::optional<double> nonnegativeNumber(const QJsonValue& value) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0)
        return std::nullopt;
    return number;
}

std::optional<qint64> integer(const QJsonValue& value, bool negativeAllowed = false) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    // The upper endpoint cannot be represented by double. QJsonValue::toInteger
    // preserves native qint64 values, but rejects a double outside its range.
    constexpr qint64 invalid = std::numeric_limits<qint64>::min();
    const qint64 result = value.toInteger(invalid);
    if (!std::isfinite(number) || std::floor(number) != number || result == invalid ||
        (!negativeAllowed && result < 0))
        return std::nullopt;
    return result;
}

std::optional<double> percent(const QJsonObject& object) {
    const auto current = nonnegativeNumber(object.value(QStringLiteral("current")));
    const auto total = nonnegativeNumber(object.value(QStringLiteral("total")));
    if (!current || !total || *total <= 0)
        return std::nullopt;
    return std::clamp(100.0 * *current / *total, 0.0, 100.0);
}

qint64 trafficBytes(const QJsonObject& object) {
    const auto direct = integer(object.value(QStringLiteral("usedTraffic")));
    if (direct)
        return *direct;
    const QJsonObject traffic = object.value(QStringLiteral("traffic")).isObject()
                                    ? object.value(QStringLiteral("traffic")).toObject()
                                    : object;
    const auto up = integer(traffic.value(QStringLiteral("up")));
    const auto down = integer(traffic.value(QStringLiteral("down")));
    const qint64 sent = up.value_or(0);
    const qint64 received = down.value_or(0);
    if (sent > std::numeric_limits<qint64>::max() - received)
        return std::numeric_limits<qint64>::max();
    return sent + received;
}

QString identifier(const QJsonObject& raw, const QString& protocol = {}) {
    // V3 ClientRecord.id is a database key. It must never become model.Client.ID.
    const QString uuid = raw.value(QStringLiteral("uuid")).toString();
    if (!uuid.isEmpty())
        return uuid;
    if (protocol == QStringLiteral("trojan")) {
        const QString password = raw.value(QStringLiteral("password")).toString();
        if (!password.isEmpty())
            return password;
    }
    const QString id = raw.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        return id;
    return raw.value(QStringLiteral("password")).toString();
}

QList<int> inboundIds(const QJsonValue& value) {
    QList<int> ids;
    if (!value.isArray())
        return ids;
    for (const auto& entry : value.toArray()) {
        const auto id = integer(entry);
        if (!id || *id <= 0 || *id > std::numeric_limits<int>::max())
            return {};
        if (!ids.contains(static_cast<int>(*id)))
            ids.append(static_cast<int>(*id));
    }
    return ids;
}

bool sameInboundIds(QList<int> left, QList<int> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

bool sameIdentity(const Client& selected, const QJsonObject& fresh, const QList<int>& ids) {
    if (selected.id.isEmpty() || selected.email.isEmpty() || selected.inboundIds.isEmpty() ||
        fresh.value(QStringLiteral("email")).toString() != selected.email ||
        identifier(fresh, selected.protocol) != selected.id ||
        !sameInboundIds(selected.inboundIds, ids))
        return false;
    for (const auto* key : {"uuid", "password"}) {
        const QString field = QString::fromLatin1(key);
        if (selected.raw.contains(field) && selected.raw.value(field) != fresh.value(field))
            return false;
    }
    return true;
}

QString encodedSegment(const QString& text) {
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

QString randomSecret(int length) {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    QString result;
    result.reserve(length);
    // Qt's system generator uses the operating system cryptographic entropy
    // source. Rejection sampling avoids a bias in the alphabet distribution.
    while (result.size() < length) {
        quint64 entropy = QRandomGenerator::system()->generate64();
        for (int i = 0; i < 8 && result.size() < length; ++i) {
            const unsigned value = static_cast<unsigned>(entropy & 0xff);
            entropy >>= 8;
            if (value < 248)
                result.append(QLatin1Char(alphabet[value % 62]));
        }
    }
    return result;
}

bool validEmail(const QString& email) {
    if (email.isEmpty() || email.size() > 128 || email != email.trimmed())
        return false;
    for (const QChar character : email) {
        if (character.category() == QChar::Other_Control || character == QLatin1Char('/') ||
            character == QLatin1Char('?') || character == QLatin1Char('#'))
            return false;
    }
    return true;
}

QString identityError() {
    return QStringLiteral(
        "Клиент изменился на сервере. Обновите список перед повторением операции.");
}

Outcome<QJsonObject> modernModel(QJsonObject record, const QString& protocol) {
    record.insert(QStringLiteral("id"), identifier(record, protocol));
    record.remove(QStringLiteral("inboundIds"));
    // ClientRecord is a database DTO rather than model.Client. Besides its
    // numeric primary key, it serializes these model fields as text.
    const QJsonValue allowed = record.value(QStringLiteral("allowedIPs"));
    if (allowed.isString()) {
        QJsonArray addresses;
        for (const QString& entry :
             allowed.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString address = entry.trimmed();
            if (!address.isEmpty())
                addresses.append(address);
        }
        record.insert(QStringLiteral("allowedIPs"), addresses);
    } else if (!allowed.isUndefined() && !allowed.isNull() && !allowed.isArray()) {
        return Outcome<QJsonObject>::failure(
            QStringLiteral("Панель вернула некорректные allowedIPs клиента."));
    }
    if (allowed.isArray()) {
        for (const auto& entry : allowed.toArray()) {
            if (!entry.isString())
                return Outcome<QJsonObject>::failure(
                    QStringLiteral("Панель вернула некорректные allowedIPs клиента."));
        }
    }
    const QJsonValue reverse = record.value(QStringLiteral("reverse"));
    if (reverse.isString()) {
        if (reverse.toString().trimmed().isEmpty())
            record.insert(QStringLiteral("reverse"), QJsonValue::Null);
        else {
            QJsonParseError error;
            const QJsonDocument parsed =
                QJsonDocument::fromJson(reverse.toString().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !parsed.isObject())
                return Outcome<QJsonObject>::failure(
                    QStringLiteral("Панель вернула некорректные reverse настройки клиента."));
            record.insert(QStringLiteral("reverse"), parsed.object());
        }
    } else if (!reverse.isUndefined() && !reverse.isNull() && !reverse.isObject()) {
        return Outcome<QJsonObject>::failure(
            QStringLiteral("Панель вернула некорректные reverse настройки клиента."));
    }
    return Outcome<QJsonObject>::success(record);
}
} // namespace

ThreeXUiApi::ThreeXUiApi(ServerConfig config, QObject* parent,
                         QNetworkAccessManager* injectedManager)
    : QObject(parent), config_(std::move(config)),
      network_(injectedManager ? injectedManager : new QNetworkAccessManager(this)),
      detected_(config_.api) {
    if (injectedManager) {
        connect(injectedManager, &QObject::destroyed, this, [this] { network_ = nullptr; });
    }
}

Outcome<QUrl> ThreeXUiApi::validatePanelUrl(const ServerConfig& config) {
    QUrl url = config.panelUrl;
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() ||
        (scheme != QStringLiteral("https") &&
         !(scheme == QStringLiteral("http") && config.allowHttp))) {
        return Outcome<QUrl>::failure(
            QStringLiteral("Укажите HTTPS URL панели. HTTP требует явного разрешения."));
    }
    if (url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || url.hasQuery() ||
        url.hasFragment()) {
        return Outcome<QUrl>::failure(
            QStringLiteral("URL панели не должен содержать логин, пароль, query или fragment."));
    }
    url.setScheme(scheme);
    QString path = url.path(QUrl::FullyEncoded);
    if (!path.endsWith(QLatin1Char('/')))
        path.append(QLatin1Char('/'));
    url.setPath(path, QUrl::StrictMode);
    return Outcome<QUrl>::success(url);
}

Outcome<Snapshot> ThreeXUiApi::parseStatus(const QJsonObject& input, const QString& serverId) {
    if (input.contains(QStringLiteral("success")) &&
        !input.value(QStringLiteral("success")).toBool()) {
        return Outcome<Snapshot>::failure(QStringLiteral("Панель отклонила запрос статуса."));
    }
    const QJsonObject object = input.value(QStringLiteral("obj")).isObject()
                                   ? input.value(QStringLiteral("obj")).toObject()
                                   : input;
    Snapshot snapshot;
    snapshot.serverId = serverId;
    snapshot.at = QDateTime::currentDateTimeUtc();
    snapshot.cpu = nonnegativeNumber(object.value(QStringLiteral("cpu")));
    if (snapshot.cpu && *snapshot.cpu > 100)
        snapshot.cpu.reset();
    snapshot.memoryPercent = percent(object.value(QStringLiteral("mem")).toObject());
    snapshot.diskPercent = percent(object.value(QStringLiteral("disk")).toObject());
    const auto traffic = object.value(QStringLiteral("netTraffic")).toObject();
    snapshot.rxBps = nonnegativeNumber(traffic.value(QStringLiteral("recv")));
    snapshot.txBps = nonnegativeNumber(traffic.value(QStringLiteral("sent")));
    const auto netIO = object.value(QStringLiteral("netIO")).toObject();
    snapshot.receivedBytes = integer(netIO.value(QStringLiteral("down")));
    snapshot.sentBytes = integer(netIO.value(QStringLiteral("up")));
    snapshot.uptimeSeconds = integer(object.value(QStringLiteral("uptime"))).value_or(0);
    snapshot.panelVersion = object.value(QStringLiteral("panelVersion")).toString();
    const auto xray = object.value(QStringLiteral("xray")).toObject();
    snapshot.xrayVersion = xray.value(QStringLiteral("version")).toString();
    const QString state = xray.value(QStringLiteral("state")).toString().toLower();
    if (state == QStringLiteral("running"))
        snapshot.health = Health::Online;
    else if (state == QStringLiteral("stop") || state == QStringLiteral("stopped") ||
             state == QStringLiteral("error"))
        snapshot.health = Health::Warning;
    return Outcome<Snapshot>::success(snapshot);
}

Outcome<Inventory> ThreeXUiApi::parseInventory(const QJsonArray& inbounds,
                                               const QJsonArray& modernClients,
                                               const QString& serverId, bool modern) {
    Inventory inventory;
    QHash<int, QString> protocols;
    for (const auto& value : inbounds) {
        if (!value.isObject())
            return Outcome<Inventory>::failure(
                QStringLiteral("Панель вернула некорректный inbound."));
        const QJsonObject raw = value.toObject();
        const auto id = integer(raw.value(QStringLiteral("id")));
        if (!id || *id <= 0 || *id > std::numeric_limits<int>::max() ||
            protocols.contains(static_cast<int>(*id)))
            return Outcome<Inventory>::failure(
                QStringLiteral("Панель вернула некорректный ID inbound."));
        Inbound inbound;
        inbound.serverId = serverId;
        inbound.id = static_cast<int>(*id);
        inbound.remark = raw.value(QStringLiteral("remark")).toString();
        inbound.protocol = raw.value(QStringLiteral("protocol")).toString().toLower();
        inbound.port = raw.value(QStringLiteral("port")).toInt();
        inbound.enable = raw.value(QStringLiteral("enable")).toBool(true);
        inventory.inbounds.append(inbound);
        protocols.insert(inbound.id, inbound.protocol);
        if (modern)
            continue;

        QJsonObject settings;
        const QJsonValue encoded = raw.value(QStringLiteral("settings"));
        if (encoded.isString() && !encoded.toString().isEmpty()) {
            QJsonParseError error;
            const QJsonDocument document =
                QJsonDocument::fromJson(encoded.toString().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Панель вернула некорректные настройки inbound."));
            settings = document.object();
        } else if (encoded.isObject())
            settings = encoded.toObject();
        else if (!encoded.isUndefined() && !encoded.isNull() && !encoded.isString())
            return Outcome<Inventory>::failure(
                QStringLiteral("Панель вернула некорректные настройки inbound."));
        const QJsonValue clientsValue = settings.value(QStringLiteral("clients"));
        if (!clientsValue.isUndefined() && !clientsValue.isArray())
            return Outcome<Inventory>::failure(
                QStringLiteral("Панель вернула некорректный список клиентов."));
        QHash<QString, QJsonObject> statistics;
        for (const auto& stat : raw.value(QStringLiteral("clientStats")).toArray()) {
            const auto object = stat.toObject();
            statistics.insert(object.value(QStringLiteral("email")).toString(), object);
        }
        for (const auto& clientValue : clientsValue.toArray()) {
            if (!clientValue.isObject())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Панель вернула некорректного клиента."));
            Client client;
            client.serverId = serverId;
            client.inboundIds = {inbound.id};
            client.protocol = inbound.protocol;
            client.raw = clientValue.toObject();
            client.id = identifier(client.raw, client.protocol);
            client.email = client.raw.value(QStringLiteral("email")).toString();
            if (client.id.isEmpty())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Клиент не содержит идентификатора доступа."));
            client.subId = client.raw.value(QStringLiteral("subId")).toString();
            client.enable = client.raw.value(QStringLiteral("enable")).toBool(true);
            client.totalBytes = integer(client.raw.value(QStringLiteral("totalGB"))).value_or(0);
            client.expiryTime =
                integer(client.raw.value(QStringLiteral("expiryTime")), true).value_or(0);
            client.usedBytes = trafficBytes(statistics.value(client.email));
            if (client.raw.value(QStringLiteral("online")).isBool())
                client.online = client.raw.value(QStringLiteral("online")).toBool();
            inventory.clients.append(client);
        }
    }
    if (modern) {
        for (const auto& value : modernClients) {
            if (!value.isObject())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Панель вернула некорректного клиента."));
            Client client;
            client.serverId = serverId;
            client.raw = value.toObject();
            client.inboundIds = inboundIds(client.raw.value(QStringLiteral("inboundIds")));
            if (client.raw.contains(QStringLiteral("inboundIds")) &&
                !client.raw.value(QStringLiteral("inboundIds")).isArray())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Панель вернула некорректные привязки клиента."));
            for (const int id : client.inboundIds) {
                if (client.protocol.isEmpty())
                    client.protocol = protocols.value(id);
            }
            client.id = identifier(client.raw, client.protocol);
            client.email = client.raw.value(QStringLiteral("email")).toString();
            if (client.id.isEmpty())
                return Outcome<Inventory>::failure(
                    QStringLiteral("Клиент не содержит идентификатора доступа."));
            client.subId = client.raw.value(QStringLiteral("subId")).toString();
            client.enable = client.raw.value(QStringLiteral("enable")).toBool(true);
            client.totalBytes = integer(client.raw.value(QStringLiteral("totalGB"))).value_or(0);
            client.expiryTime =
                integer(client.raw.value(QStringLiteral("expiryTime")), true).value_or(0);
            client.usedBytes = trafficBytes(client.raw);
            if (client.raw.value(QStringLiteral("online")).isBool())
                client.online = client.raw.value(QStringLiteral("online")).toBool();
            inventory.clients.append(client);
        }
    }
    return Outcome<Inventory>::success(inventory);
}

void ThreeXUiApi::enqueue(std::function<void()> operation) {
    operations_.enqueue(std::move(operation));
    if (busy_)
        return;
    busy_ = true;
    auto next = operations_.dequeue();
    next();
}

void ThreeXUiApi::complete() {
    // Keep the queue busy until the next event-loop turn so a callback can
    // enqueue more work without overtaking an operation already waiting.
    QTimer::singleShot(0, this, [this] {
        if (operations_.isEmpty()) {
            busy_ = false;
            return;
        }
        auto next = operations_.dequeue();
        next();
    });
}

void ThreeXUiApi::request(const QByteArray& method, const QString& path, const QJsonObject& body,
                          JsonCallback callback, bool form, bool mutation) {
    const bool confirmationRequired = mutation || path == QStringLiteral("/login");
    lastHttpStatus_ = 0;
    const auto validated = validatePanelUrl(config_);
    if (!validated.ok || !network_) {
        callback(Outcome<QJsonValue>::failure(
            validated.ok ? QStringLiteral("Сетевой менеджер недоступен.") : validated.error));
        return;
    }
    QUrl url = validated.value;
    url.setPath(url.path(QUrl::FullyEncoded) +
                    (path.startsWith(QLatin1Char('/')) ? path.mid(1) : path),
                QUrl::StrictMode);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(kRequestTimeoutMs);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    if (config_.auth == AuthKind::Token)
        request.setRawHeader("Authorization", "Bearer " + config_.token.toUtf8());
    if (!csrf_.isEmpty() && method != "GET")
        request.setRawHeader("X-CSRF-Token", csrf_.toUtf8());
    QByteArray data;
    if (method != "GET") {
        if (form) {
            QUrlQuery query;
            for (auto it = body.constBegin(); it != body.constEnd(); ++it)
                query.addQueryItem(it.key(), it.value().toVariant().toString());
            data = query.toString(QUrl::FullyEncoded).toUtf8();
            // QUrlQuery deliberately leaves '+' literal, but form decoders
            // treat it as a space. Encode it for credentials and settings.
            data.replace("+", "%2B");
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/x-www-form-urlencoded"));
        } else {
            data = QJsonDocument(body).toJson(QJsonDocument::Compact);
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
        }
    }
    QNetworkReply* reply = method == "GET" ? network_->get(request) : network_->post(request, data);
    reply->setReadBufferSize(kResponseLimit + 1);
    struct ResponseState {
        QByteArray bytes;
        bool timedOut = false;
        bool tooLarge = false;
        QElapsedTimer clock;
    };
    auto state = std::make_shared<ResponseState>();
    state->clock.start();
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply, state] {
        state->timedOut = true;
        reply->abort();
    });
    timeout->start(kRequestTimeoutMs);
    const auto read = [reply, state] {
        const qint64 remaining = kResponseLimit + 1 - state->bytes.size();
        if (remaining > 0)
            state->bytes.append(reply->read(remaining));
        if (state->bytes.size() > kResponseLimit || reply->bytesAvailable() > 0) {
            state->tooLarge = true;
            reply->abort();
        }
    };
    connect(reply, &QIODevice::readyRead, this, read);
    connect(this, &QObject::destroyed, reply, [reply] {
        // QObject emits destroyed after the derived class's members have
        // gone. Abort can synchronously emit finished, so first sever every
        // reply signal connection that might capture those members.
        reply->disconnect();
        reply->abort();
        reply->deleteLater();
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, timeout, state, read, callback = std::move(callback), mutation,
         confirmationRequired]() mutable {
            timeout->stop();
            if (reply->isOpen())
                read();
            lastHttpStatus_ = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            lastLatencyMs_ = static_cast<int>(
                std::min<qint64>(state->clock.elapsed(), std::numeric_limits<int>::max()));
            const int status = lastHttpStatus_;
            const auto networkError = reply->error();
            reply->deleteLater();
            if (status == 401 || status == 403)
                loggedIn_ = false;
            QString error;
            if (state->tooLarge)
                error = QStringLiteral("Ответ панели превышает 16 МБ.");
            else if (state->timedOut || networkError == QNetworkReply::TimeoutError)
                error = mutation ? QStringLiteral("Время ожидания истекло: результат операции "
                                                  "неизвестен. Обновите список перед повторением.")
                                 : QStringLiteral("Время ожидания ответа панели истекло.");
            else if (status >= 300 && status < 400)
                error =
                    QStringLiteral("Панель перенаправляет запрос. Укажите конечный URL панели.");
            else if (status == 401 || status == 403)
                error = QStringLiteral("Панель отклонила авторизацию. Проверьте подключение.");
            else if (status >= 400)
                error = QStringLiteral("Панель вернула HTTP %1.").arg(status);
            else if (networkError != QNetworkReply::NoError || status < 200 || status >= 300)
                error = mutation ? QStringLiteral("Ошибка соединения: результат операции "
                                                  "неизвестен. Обновите список перед повторением.")
                                 : QStringLiteral("Не удалось соединиться с панелью.");
            if (!error.isEmpty()) {
                callback(Outcome<QJsonValue>::failure(error));
                return;
            }
            QJsonParseError jsonError;
            const QJsonDocument document = QJsonDocument::fromJson(state->bytes, &jsonError);
            if (jsonError.error != QJsonParseError::NoError ||
                (!document.isObject() && !document.isArray())) {
                callback(Outcome<QJsonValue>::failure(
                    QStringLiteral("Панель вернула некорректный JSON.")));
                return;
            }
            if (document.isArray()) {
                if (confirmationRequired)
                    callback(Outcome<QJsonValue>::failure(
                        QStringLiteral("Панель не подтвердила выполнение операции.")));
                else
                    callback(Outcome<QJsonValue>::success(document.array()));
                return;
            }
            const QJsonObject object = document.object();
            const auto success = object.value(QStringLiteral("success"));
            if ((object.contains(QStringLiteral("success")) &&
                 (!success.isBool() || !success.toBool())) ||
                (confirmationRequired && (!success.isBool() || !success.toBool()))) {
                // The remote msg/errorMsg may contain a URL, token or password.
                // It is deliberately never propagated to UI or persistent events.
                if (config_.auth == AuthKind::Password)
                    loggedIn_ = false;
                callback(Outcome<QJsonValue>::failure(QStringLiteral("Панель отклонила запрос.")));
                return;
            }
            callback(Outcome<QJsonValue>::success(object.contains(QStringLiteral("obj"))
                                                      ? object.value(QStringLiteral("obj"))
                                                      : QJsonValue(object)));
        });
}

void ThreeXUiApi::authenticate(Callback<bool> callback) {
    const auto validated = validatePanelUrl(config_);
    if (!validated.ok) {
        callback(Outcome<bool>::failure(validated.error));
        return;
    }
    if (config_.auth == AuthKind::Token) {
        if (config_.token.trimmed().isEmpty() || config_.token.contains(QLatin1Char('\r')) ||
            config_.token.contains(QLatin1Char('\n'))) {
            callback(Outcome<bool>::failure(QStringLiteral("Укажите корректный API токен.")));
        } else
            callback(Outcome<bool>::success(true));
        return;
    }
    if (loggedIn_) {
        callback(Outcome<bool>::success(true));
        return;
    }
    if (config_.username.isEmpty() || config_.password.isEmpty()) {
        callback(Outcome<bool>::failure(QStringLiteral("Укажите логин и пароль панели.")));
        return;
    }
    request(
        "GET", QStringLiteral("/csrf-token"), {},
        [this, callback = std::move(callback)](Outcome<QJsonValue> token) mutable {
            if (!token.ok && lastHttpStatus_ != 404) {
                callback(Outcome<bool>::failure(token.error));
                return;
            }
            csrf_ = token.ok ? token.value.toString() : QString();
            if (token.ok && csrf_.isEmpty()) {
                callback(Outcome<bool>::failure(QStringLiteral("Панель не выдала CSRF токен.")));
                return;
            }
            const QJsonObject login{{QStringLiteral("username"), config_.username},
                                    {QStringLiteral("password"), config_.password},
                                    {QStringLiteral("twoFactorCode"), config_.twoFactorCode}};
            request("POST", QStringLiteral("/login"), login,
                    [this, callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                        if (!result.ok) {
                            loggedIn_ = false;
                            callback(Outcome<bool>::failure(result.error));
                            return;
                        }
                        config_.twoFactorCode.clear();
                        request("GET", QStringLiteral("/csrf-token"), {},
                                [this, callback = std::move(callback)](
                                    Outcome<QJsonValue> refreshed) mutable {
                                    if (!refreshed.ok && lastHttpStatus_ != 404) {
                                        loggedIn_ = false;
                                        callback(Outcome<bool>::failure(refreshed.error));
                                        return;
                                    }
                                    csrf_ = refreshed.ok ? refreshed.value.toString() : QString();
                                    if (refreshed.ok && csrf_.isEmpty()) {
                                        loggedIn_ = false;
                                        callback(Outcome<bool>::failure(QStringLiteral(
                                            "Панель не выдала CSRF токен после входа.")));
                                        return;
                                    }
                                    loggedIn_ = true;
                                    callback(Outcome<bool>::success(true));
                                });
                    });
        });
}

void ThreeXUiApi::discover(Callback<bool> callback) {
    if (detected_ != ApiFlavor::Auto) {
        callback(Outcome<bool>::success(true));
        return;
    }
    request("GET", QStringLiteral("/panel/api/clients/list"), {},
            [this, callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                if (result.ok && result.value.isArray()) {
                    detected_ = ApiFlavor::ModernV3;
                    callback(Outcome<bool>::success(true));
                } else if (!result.ok && (lastHttpStatus_ == 404 || lastHttpStatus_ == 405)) {
                    detected_ = ApiFlavor::LegacyV2;
                    callback(Outcome<bool>::success(true));
                } else
                    callback(Outcome<bool>::failure(
                        result.ok ? QStringLiteral("Панель вернула некорректный список клиентов.")
                                  : result.error));
            });
}

void ThreeXUiApi::prepare(Callback<bool> callback) {
    authenticate([this, callback = std::move(callback)](Outcome<bool> result) mutable {
        if (!result.ok) {
            callback(std::move(result));
            return;
        }
        discover(std::move(callback));
    });
}

void ThreeXUiApi::inventory(JsonCallback callback) {
    prepare([this, callback = std::move(callback)](Outcome<bool> prepared) mutable {
        if (!prepared.ok) {
            callback(Outcome<QJsonValue>::failure(prepared.error));
            return;
        }
        request(
            "GET", QStringLiteral("/panel/api/inbounds/list"), {},
            [this, callback = std::move(callback)](Outcome<QJsonValue> inbounds) mutable {
                if (!inbounds.ok || !inbounds.value.isArray()) {
                    callback(Outcome<QJsonValue>::failure(
                        inbounds.ok ? QStringLiteral("Панель вернула некорректный список inbound.")
                                    : inbounds.error));
                    return;
                }
                if (detected_ == ApiFlavor::LegacyV2) {
                    callback(Outcome<QJsonValue>::success(
                        QJsonObject{{QStringLiteral("inbounds"), inbounds.value}}));
                    return;
                }
                request("GET", QStringLiteral("/panel/api/clients/list"), {},
                        [callback = std::move(callback),
                         inbounds = inbounds.value](Outcome<QJsonValue> clients) mutable {
                            if (!clients.ok || !clients.value.isArray()) {
                                callback(Outcome<QJsonValue>::failure(
                                    clients.ok ? QStringLiteral(
                                                     "Панель вернула некорректный список клиентов.")
                                               : clients.error));
                                return;
                            }
                            callback(Outcome<QJsonValue>::success(
                                QJsonObject{{QStringLiteral("inbounds"), inbounds},
                                            {QStringLiteral("clients"), clients.value}}));
                        });
            });
    });
}

void ThreeXUiApi::fetchInventory(Callback<Inventory> callback) {
    enqueue([this, callback = std::move(callback)]() mutable {
        inventory([this, callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
            Outcome<Inventory> parsed =
                result.ok ? parseInventory(
                                result.value.toObject().value(QStringLiteral("inbounds")).toArray(),
                                result.value.toObject().value(QStringLiteral("clients")).toArray(),
                                config_.id, detected_ == ApiFlavor::ModernV3)
                          : Outcome<Inventory>::failure(result.error);
            complete();
            if (callback)
                callback(std::move(parsed));
        });
    });
}

void ThreeXUiApi::fetchStatus(Callback<Snapshot> callback) {
    enqueue([this, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<Snapshot> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        authenticate([this, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(Outcome<Snapshot>::failure(prepared.error));
                return;
            }
            auto statusResult = [this, finish](Outcome<QJsonValue> result) mutable {
                if (!result.ok || !result.value.isObject()) {
                    finish(Outcome<Snapshot>::failure(
                        result.ok ? QStringLiteral("Панель вернула некорректный статус.")
                                  : result.error));
                    return;
                }
                auto parsed = parseStatus(result.value.toObject(), config_.id);
                if (!parsed.ok) {
                    finish(std::move(parsed));
                    return;
                }
                parsed.value.latencyMs = lastLatencyMs_;
                discover(
                    [this, finish, parsed = std::move(parsed)](Outcome<bool> discovered) mutable {
                        // A monitoring token may read status without permission
                        // to list clients. Those metrics remain valid.
                        if (!discovered.ok) {
                            finish(std::move(parsed));
                            return;
                        }
                        const QString path = detected_ == ApiFlavor::ModernV3
                                                 ? QStringLiteral("/panel/api/clients/onlines")
                                                 : QStringLiteral("/panel/api/inbounds/onlines");
                        request("POST", path, {},
                                [finish,
                                 parsed = std::move(parsed)](Outcome<QJsonValue> online) mutable {
                                    if (online.ok && online.value.isArray())
                                        parsed.value.online = online.value.toArray().size();
                                    finish(std::move(parsed));
                                });
                    });
            };
            request("GET", QStringLiteral("/panel/api/server/status"), {},
                    [this, statusResult](Outcome<QJsonValue> result) mutable {
                        if (!result.ok && lastHttpStatus_ == 404 &&
                            detected_ != ApiFlavor::ModernV3)
                            request("GET", QStringLiteral("/panel/server/status"), {},
                                    std::move(statusResult));
                        else
                            statusResult(std::move(result));
                    });
        });
    });
}

void ThreeXUiApi::readClient(const Client& client, Callback<QJsonObject> callback) {
    if (client.serverId != config_.id || !validEmail(client.email) || client.id.isEmpty() ||
        client.inboundIds.isEmpty()) {
        callback(Outcome<QJsonObject>::failure(identityError()));
        return;
    }
    if (detected_ == ApiFlavor::ModernV3) {
        request("GET", QStringLiteral("/panel/api/clients/get/") + encodedSegment(client.email), {},
                [client, callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                    if (!result.ok) {
                        callback(Outcome<QJsonObject>::failure(result.error));
                        return;
                    }
                    const QJsonObject response = result.value.toObject();
                    QJsonObject fresh = response.value(QStringLiteral("client")).toObject();
                    const auto ids = inboundIds(response.value(QStringLiteral("inboundIds")));
                    if (fresh.isEmpty() || !sameIdentity(client, fresh, ids)) {
                        callback(Outcome<QJsonObject>::failure(identityError()));
                        return;
                    }
                    callback(Outcome<QJsonObject>::success(fresh));
                });
        return;
    }
    if (client.inboundIds.size() != 1) {
        callback(Outcome<QJsonObject>::failure(identityError()));
        return;
    }
    request("GET", QStringLiteral("/panel/api/inbounds/list"), {},
            [this, client, callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                if (!result.ok || !result.value.isArray()) {
                    callback(
                        Outcome<QJsonObject>::failure(result.ok ? identityError() : result.error));
                    return;
                }
                const auto parsed = parseInventory(result.value.toArray(), {}, config_.id, false);
                if (!parsed.ok) {
                    callback(Outcome<QJsonObject>::failure(parsed.error));
                    return;
                }
                const Client* match = nullptr;
                for (const auto& candidate : parsed.value.clients) {
                    if (candidate.inboundIds == client.inboundIds && candidate.id == client.id &&
                        candidate.email == client.email) {
                        if (match) {
                            callback(Outcome<QJsonObject>::failure(identityError()));
                            return;
                        }
                        match = &candidate;
                    }
                }
                if (!match || !sameIdentity(client, match->raw, match->inboundIds) ||
                    match->protocol != client.protocol) {
                    callback(Outcome<QJsonObject>::failure(identityError()));
                    return;
                }
                callback(Outcome<QJsonObject>::success(match->raw));
            });
}

void ThreeXUiApi::createClient(const ClientDraft& draft, Callback<bool> callback) {
    enqueue([this, draft, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<bool> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        if (!validEmail(draft.email) || draft.inboundId <= 0 || draft.totalBytes < 0 ||
            (!draft.flow.isEmpty() && draft.flow != QStringLiteral("xtls-rprx-vision"))) {
            finish(Outcome<bool>::failure(QStringLiteral("Некорректные параметры клиента.")));
            return;
        }
        prepare([this, draft, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(std::move(prepared));
                return;
            }
            request(
                "GET", QStringLiteral("/panel/api/inbounds/list"), {},
                [this, draft, finish](Outcome<QJsonValue> result) mutable {
                    if (!result.ok || !result.value.isArray()) {
                        finish(Outcome<bool>::failure(
                            result.ok
                                ? QStringLiteral("Панель вернула некорректный список inbound.")
                                : result.error));
                        return;
                    }
                    const auto parsed =
                        parseInventory(result.value.toArray(), {}, config_.id, true);
                    if (!parsed.ok) {
                        finish(Outcome<bool>::failure(parsed.error));
                        return;
                    }
                    auto selected = std::find_if(
                        parsed.value.inbounds.cbegin(), parsed.value.inbounds.cend(),
                        [draft](const Inbound& inbound) { return inbound.id == draft.inboundId; });
                    if (selected == parsed.value.inbounds.cend() || !selected->enable) {
                        finish(Outcome<bool>::failure(
                            QStringLiteral("Inbound удалён или отключён. Обновите список.")));
                        return;
                    }
                    const QString protocol = selected->protocol;
                    if (protocol != QStringLiteral("vless") &&
                        protocol != QStringLiteral("vmess") &&
                        protocol != QStringLiteral("trojan")) {
                        finish(Outcome<bool>::failure(QStringLiteral(
                            "Создание клиента для этого протокола пока не поддерживается.")));
                        return;
                    }
                    if (!draft.flow.isEmpty() && protocol != QStringLiteral("vless")) {
                        finish(Outcome<bool>::failure(
                            QStringLiteral("Flow xtls-rprx-vision доступен только для VLESS.")));
                        return;
                    }
                    QJsonObject client{{QStringLiteral("email"), draft.email},
                                       {QStringLiteral("totalGB"), draft.totalBytes},
                                       {QStringLiteral("expiryTime"), draft.expiryTime},
                                       {QStringLiteral("enable"), draft.enable},
                                       {QStringLiteral("subId"), randomSecret(24)},
                                       {QStringLiteral("flow"), draft.flow},
                                       {QStringLiteral("limitIp"), 0},
                                       {QStringLiteral("limitHwid"), 0},
                                       {QStringLiteral("tgId"), 0},
                                       {QStringLiteral("comment"), QString()},
                                       {QStringLiteral("reset"), 0}};
                    if (protocol == QStringLiteral("trojan"))
                        client.insert(QStringLiteral("password"), randomSecret(32));
                    if (protocol != QStringLiteral("trojan") || detected_ == ApiFlavor::ModernV3)
                        client.insert(QStringLiteral("id"),
                                      QUuid::createUuid().toString(QUuid::WithoutBraces));
                    if (protocol == QStringLiteral("vmess"))
                        client.insert(QStringLiteral("security"), QStringLiteral("auto"));
                    auto done = [finish](Outcome<QJsonValue> result) mutable {
                        finish(result.ok ? Outcome<bool>::success(true)
                                         : Outcome<bool>::failure(result.error));
                    };
                    if (detected_ == ApiFlavor::ModernV3) {
                        request("POST", QStringLiteral("/panel/api/clients/add"),
                                {{QStringLiteral("client"), client},
                                 {QStringLiteral("inboundIds"), QJsonArray{draft.inboundId}}},
                                done, false, true);
                    } else {
                        const QString settings =
                            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("clients"),
                                                                         QJsonArray{client}}})
                                                  .toJson(QJsonDocument::Compact));
                        request("POST", QStringLiteral("/panel/api/inbounds/addClient"),
                                {{QStringLiteral("id"), draft.inboundId},
                                 {QStringLiteral("settings"), settings}},
                                done, true, true);
                    }
                });
        });
    });
}

void ThreeXUiApi::mutate(const Client& client, const ClientPatch& patch, Mutation mutation,
                         Callback<bool> callback) {
    auto finish = [this, callback = std::move(callback)](Outcome<bool> result) mutable {
        complete();
        if (callback)
            callback(std::move(result));
    };
    if (patch.totalBytes && *patch.totalBytes < 0) {
        finish(
            Outcome<bool>::failure(QStringLiteral("Лимит трафика не может быть отрицательным.")));
        return;
    }
    prepare([this, client, patch, mutation, finish](Outcome<bool> prepared) mutable {
        if (!prepared.ok) {
            finish(std::move(prepared));
            return;
        }
        readClient(client, [this, client, patch, mutation,
                            finish](Outcome<QJsonObject> fresh) mutable {
            if (!fresh.ok) {
                finish(Outcome<bool>::failure(fresh.error));
                return;
            }
            auto done = [finish](Outcome<QJsonValue> result) mutable {
                finish(result.ok ? Outcome<bool>::success(true)
                                 : Outcome<bool>::failure(result.error));
            };
            if (mutation == Mutation::Update) {
                QJsonObject updated = fresh.value;
                if (patch.totalBytes)
                    updated.insert(QStringLiteral("totalGB"), *patch.totalBytes);
                if (patch.expiryTime)
                    updated.insert(QStringLiteral("expiryTime"), *patch.expiryTime);
                if (patch.enable)
                    updated.insert(QStringLiteral("enable"), *patch.enable);
                if (detected_ == ApiFlavor::ModernV3) {
                    // A ClientRecord contains associations; model.Client does
                    // not. Global v3 updates must not filter to one inbound.
                    auto model = modernModel(std::move(updated), client.protocol);
                    if (!model.ok) {
                        finish(Outcome<bool>::failure(model.error));
                        return;
                    }
                    request("POST",
                            QStringLiteral("/panel/api/clients/update/") +
                                encodedSegment(client.email),
                            model.value, done, false, true);
                } else {
                    const QString settings = QString::fromUtf8(
                        QJsonDocument(QJsonObject{{QStringLiteral("clients"), QJsonArray{updated}}})
                            .toJson(QJsonDocument::Compact));
                    request("POST",
                            QStringLiteral("/panel/api/inbounds/updateClient/") +
                                encodedSegment(client.id),
                            {{QStringLiteral("id"), client.inboundIds.first()},
                             {QStringLiteral("settings"), settings}},
                            done, true, true);
                }
                return;
            }
            QString path;
            if (detected_ == ApiFlavor::ModernV3) {
                path = (mutation == Mutation::Delete
                            ? QStringLiteral("/panel/api/clients/del/")
                            : QStringLiteral("/panel/api/clients/resetTraffic/")) +
                       encodedSegment(client.email);
            } else if (mutation == Mutation::Delete) {
                path = QStringLiteral("/panel/api/inbounds/%1/delClient/")
                           .arg(client.inboundIds.first()) +
                       encodedSegment(client.id);
            } else {
                path = QStringLiteral("/panel/api/inbounds/%1/resetClientTraffic/")
                           .arg(client.inboundIds.first()) +
                       encodedSegment(client.email);
            }
            request("POST", path, {}, done, false, true);
        });
    });
}

void ThreeXUiApi::updateClient(const Client& client, const ClientPatch& patch,
                               Callback<bool> callback) {
    enqueue([this, client, patch, callback = std::move(callback)]() mutable {
        mutate(client, patch, Mutation::Update, std::move(callback));
    });
}

void ThreeXUiApi::deleteClient(const Client& client, Callback<bool> callback) {
    enqueue([this, client, callback = std::move(callback)]() mutable {
        mutate(client, {}, Mutation::Delete, std::move(callback));
    });
}

void ThreeXUiApi::resetClientTraffic(const Client& client, Callback<bool> callback) {
    enqueue([this, client, callback = std::move(callback)]() mutable {
        mutate(client, {}, Mutation::Reset, std::move(callback));
    });
}

void ThreeXUiApi::serverAction(ServerAction action, const QString& version,
                               Callback<bool> callback) {
    enqueue([this, action, version, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<bool> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        QString path;
        switch (action) {
        case ServerAction::RestartXray:
            path = QStringLiteral("/panel/api/server/restartXrayService");
            break;
        case ServerAction::UpdateGeofiles:
            path = QStringLiteral("/panel/api/server/updateGeofile");
            break;
        case ServerAction::InstallXray:
            if (!QRegularExpression(QStringLiteral("^v?[0-9]+(\\.[0-9]+){1,3}$"))
                     .match(version)
                     .hasMatch()) {
                finish(Outcome<bool>::failure(
                    QStringLiteral("Укажите версию Xray вида 25.3.6 или v25.3.6.")));
                return;
            }
            path = QStringLiteral("/panel/api/server/installXray/") + version;
            break;
        }
        authenticate([this, path, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(std::move(prepared));
                return;
            }
            request(
                "POST", path, {},
                [finish](Outcome<QJsonValue> result) mutable {
                    finish(result.ok ? Outcome<bool>::success(true)
                                     : Outcome<bool>::failure(result.error));
                },
                false, true);
        });
    });
}
} // namespace fleet
