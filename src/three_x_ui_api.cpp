#include "three_x_ui_api.h"
#include "backup_file.h"

#include <QElapsedTimer>
#include <QIODevice>
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
#include <cstring>
#include <limits>
#include <memory>

namespace fleet {
namespace {
constexpr qint64 kResponseLimit = 16 * 1024 * 1024;
constexpr int kRequestTimeoutMs = 8000;

// Qt can rewind a buffered POST and resend it after a dropped connection.
// A mutation body must be readable once, including by Qt's HTTP transport.
class OneShotUpload final : public QIODevice {
  public:
    explicit OneShotUpload(QByteArray bytes) : bytes_(std::move(bytes)) {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    bool isSequential() const override { return true; }
    bool reset() override { return false; }
    qint64 size() const override { return bytes_.size(); }
    qint64 bytesAvailable() const override {
        return bytes_.size() - consumed_ + QIODevice::bytesAvailable();
    }

  protected:
    qint64 readData(char* output, qint64 maximum) override {
        const qint64 count = std::min(maximum, static_cast<qint64>(bytes_.size()) - consumed_);
        if (count > 0) {
            std::memcpy(output, bytes_.constData() + consumed_, static_cast<size_t>(count));
            consumed_ += count;
        }
        return count;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

  private:
    QByteArray bytes_;
    qint64 consumed_ = 0;
};

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
    if (protocol == QStringLiteral("hysteria")) {
        const QString auth = raw.value(QStringLiteral("auth")).toString();
        if (!auth.isEmpty())
            return auth;
    }
    if (protocol == QStringLiteral("trojan")) {
        const QString password = raw.value(QStringLiteral("password")).toString();
        if (!password.isEmpty())
            return password;
    }
    const QString id = raw.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        return id;
    const QString password = raw.value(QStringLiteral("password")).toString();
    return password.isEmpty() ? raw.value(QStringLiteral("auth")).toString() : password;
}

Outcome<QList<int>> inboundIds(const QJsonValue& value) {
    QList<int> ids;
    if (value.isUndefined() || value.isNull())
        return Outcome<QList<int>>::success(ids);
    if (!value.isArray())
        return Outcome<QList<int>>::failure(
            QStringLiteral("Панель вернула некорректные привязки клиента."));
    for (const auto& entry : value.toArray()) {
        const auto id = integer(entry);
        if (!id || *id <= 0 || *id > std::numeric_limits<int>::max())
            return Outcome<QList<int>>::failure(
                QStringLiteral("Панель вернула некорректные привязки клиента."));
        if (!ids.contains(static_cast<int>(*id)))
            ids.append(static_cast<int>(*id));
    }
    return Outcome<QList<int>>::success(ids);
}

bool sameInboundIds(QList<int> left, QList<int> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

bool sameIdentity(const Client& selected, const QJsonObject& fresh, const QList<int>& ids) {
    if (selected.id.isEmpty() || selected.email.isEmpty() ||
        fresh.value(QStringLiteral("email")).toString() != selected.email ||
        identifier(fresh, selected.protocol) != selected.id ||
        !sameInboundIds(selected.inboundIds, ids))
        return false;
    for (const auto* key : {"uuid", "password", "auth", "subId"}) {
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
        if (character.category() == QChar::Other_Control || character.isSpace() ||
            character == QLatin1Char('\\') || character == QLatin1Char('/') ||
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
    Q_UNUSED(protocol);
    // Password/auth identifies a client for a stale-record check, but is not
    // the UUID field of model.Client. Pure Hysteria clients may have no UUID.
    QString uuid = record.value(QStringLiteral("uuid")).toString();
    if (uuid.isEmpty())
        uuid = record.value(QStringLiteral("id")).toString();
    record.insert(QStringLiteral("id"), uuid);
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

QJsonArray jsonIds(const QList<int>& ids) {
    QJsonArray array;
    for (int id : ids)
        array.append(id);
    return array;
}

Outcome<QList<Inbound>> selectedInbounds(const QJsonArray& data, const ClientDraft& draft,
                                         const QString& serverId) {
    const auto parsed = ThreeXUiApi::parseInventory(data, {}, serverId, true);
    if (!parsed.ok)
        return Outcome<QList<Inbound>>::failure(parsed.error);
    QList<Inbound> selected;
    for (int id : draft.inboundIds) {
        auto found = std::find_if(parsed.value.inbounds.cbegin(), parsed.value.inbounds.cend(),
                                  [id](const Inbound& inbound) { return inbound.id == id; });
        if (found == parsed.value.inbounds.cend() || !found->enable)
            return Outcome<QList<Inbound>>::failure(
                QStringLiteral("Inbound удалён или отключён. Обновите список."));
        if (found->protocol != QStringLiteral("vless") &&
            found->protocol != QStringLiteral("vmess") &&
            found->protocol != QStringLiteral("trojan") &&
            found->protocol != QStringLiteral("hysteria"))
            return Outcome<QList<Inbound>>::failure(
                QStringLiteral("Создание клиента для этого протокола пока не поддерживается."));
        if (!draft.flow.isEmpty() && found->protocol != QStringLiteral("vless"))
            return Outcome<QList<Inbound>>::failure(
                QStringLiteral("Flow xtls-rprx-vision доступен только при выборе VLESS inbound."));
        selected.append(*found);
    }
    return Outcome<QList<Inbound>>::success(selected);
}

bool matchesDraft(const QJsonObject& raw, const ClientDraft& draft, bool checkEnable = true,
                  const QString& legacyProtocol = {}, bool vmess = false) {
    const QString access = identifier(raw, legacyProtocol);
    const QString expected = legacyProtocol == QStringLiteral("trojan")     ? draft.password
                             : legacyProtocol == QStringLiteral("hysteria") ? draft.hysteriaAuth
                                                                            : draft.clientId;
    const auto quota = integer(raw.value(QStringLiteral("totalGB")));
    const auto expiry = integer(raw.value(QStringLiteral("expiryTime")), true);
    return raw.value(QStringLiteral("email")).toString() == draft.email && access == expected &&
           raw.value(QStringLiteral("password")).toString() == draft.password &&
           (draft.hysteriaAuth.isEmpty() ||
            raw.value(QStringLiteral("auth")).toString() == draft.hysteriaAuth) &&
           raw.value(QStringLiteral("subId")).toString() == draft.subId && quota &&
           *quota == draft.totalBytes && expiry && *expiry == draft.expiryTime &&
           raw.value(QStringLiteral("enable")).isBool() &&
           (!checkEnable || raw.value(QStringLiteral("enable")).toBool() == draft.enable) &&
           raw.value(QStringLiteral("flow")).toString() == draft.flow &&
           (!vmess || raw.value(QStringLiteral("security")).toString() == QStringLiteral("auto"));
}

QJsonObject newClient(const ClientDraft& draft, const QList<Inbound>& inbounds, bool modern) {
    bool vmess = false;
    bool trojan = false;
    bool hysteria = false;
    for (const auto& inbound : inbounds) {
        vmess |= inbound.protocol == QStringLiteral("vmess");
        trojan |= inbound.protocol == QStringLiteral("trojan");
        hysteria |= inbound.protocol == QStringLiteral("hysteria");
    }
    QJsonObject client{{QStringLiteral("email"), draft.email},
                       {QStringLiteral("totalGB"), draft.totalBytes},
                       {QStringLiteral("expiryTime"), draft.expiryTime},
                       {QStringLiteral("enable"), draft.enable},
                       {QStringLiteral("subId"), draft.subId},
                       {QStringLiteral("flow"), draft.flow},
                       {QStringLiteral("limitIp"), 0},
                       {QStringLiteral("limitHwid"), 0},
                       {QStringLiteral("tgId"), 0},
                       {QStringLiteral("comment"), QString()},
                       {QStringLiteral("reset"), 0}};
    if (modern || (!trojan && !hysteria))
        client.insert(QStringLiteral("id"), draft.clientId);
    if (!draft.password.isEmpty())
        client.insert(QStringLiteral("password"), draft.password);
    if (!draft.hysteriaAuth.isEmpty())
        client.insert(QStringLiteral("auth"), draft.hysteriaAuth);
    if (vmess)
        client.insert(QStringLiteral("security"), QStringLiteral("auto"));
    return client;
}

QString collisionError() {
    return QStringLiteral("Email или идентификатор уже принадлежит другому клиенту. Обновите "
                          "список; чужая запись не изменена.");
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
            const auto bindings = inboundIds(client.raw.value(QStringLiteral("inboundIds")));
            if (!bindings.ok)
                return Outcome<Inventory>::failure(bindings.error);
            client.inboundIds = bindings.value;
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
    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = network_->get(request);
    } else {
        request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        request.setHeader(QNetworkRequest::ContentLengthHeader, data.size());
        auto* upload = new OneShotUpload(std::move(data));
        reply = network_->post(request, upload);
        upload->setParent(reply);
    }
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
                                    if (online.ok && online.value.isArray()) {
                                        QSet<QString> emails;
                                        bool valid = true;
                                        for (const auto& entry : online.value.toArray()) {
                                            if (!entry.isString() || entry.toString().isEmpty()) {
                                                valid = false;
                                                break;
                                            }
                                            emails.insert(entry.toString());
                                        }
                                        if (valid) {
                                            parsed.value.onlineEmails = emails.values();
                                            parsed.value.online = static_cast<int>(emails.size());
                                            parsed.value.onlineDetailsAvailable = true;
                                        }
                                    }
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

Outcome<QUrl> ThreeXUiApi::parseSubscriptionSettings(const QJsonObject& object) {
    if (!object.value(QStringLiteral("subEnable")).isBool())
        return Outcome<QUrl>::failure(QStringLiteral("Панель не вернула настройки подписок."));
    if (!object.value(QStringLiteral("subEnable")).toBool())
        return Outcome<QUrl>::failure(QStringLiteral("Выдача подписок выключена в 3x-ui."));
    const QString text = object.value(QStringLiteral("subURI")).toString().trimmed();
    const QUrl url(text, QUrl::StrictMode);
    if (text.isEmpty() || !url.isValid() || url.isRelative() || url.host().isEmpty() ||
        (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || url.port() == 0)
        return Outcome<QUrl>::failure(QStringLiteral(
            "Панель не вернула корректный адрес выдачи подписок. Проверьте её настройки "
            "подписок или задайте адрес в подключении приложения."));
    return Outcome<QUrl>::success(url);
}

void ThreeXUiApi::fetchSubscriptionUrl(Callback<QUrl> callback) {
    enqueue([this, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<QUrl> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        prepare([this, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(Outcome<QUrl>::failure(prepared.error));
                return;
            }
            auto parse = [finish](Outcome<QJsonValue> settings) mutable {
                finish(settings.ok && settings.value.isObject()
                           ? parseSubscriptionSettings(settings.value.toObject())
                           : Outcome<QUrl>::failure(
                                 settings.ok
                                     ? QStringLiteral("Панель не вернула настройки подписок.")
                                     : settings.error));
            };
            const QString legacy = QStringLiteral("/panel/setting/defaultSettings");
            if (detected_ == ApiFlavor::LegacyV2) {
                request("POST", legacy, {}, parse);
                return;
            }
            request("POST", QStringLiteral("/panel/api/setting/defaultSettings"), {},
                    [this, legacy, parse](Outcome<QJsonValue> settings) mutable {
                        // Some older 3.x builds expose this read-only route at the UI prefix.
                        // Never switch routes on an authorization failure or retry a mutation.
                        if (!settings.ok && lastHttpStatus_ == 404)
                            request("POST", legacy, {}, parse);
                        else
                            parse(std::move(settings));
                    });
        });
    });
}

void ThreeXUiApi::attachClientInbounds(const Client& client, const QList<int>& additions,
                                       Callback<bool> callback) {
    enqueue([this, client, additions, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<bool> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        QSet<int> unique;
        for (const int id : additions) {
            if (id <= 0 || unique.contains(id) || client.inboundIds.contains(id)) {
                finish(
                    Outcome<bool>::failure(QStringLiteral("Некорректный список новых инбаундов.")));
                return;
            }
            unique.insert(id);
        }
        if (additions.isEmpty()) {
            finish(Outcome<bool>::failure(QStringLiteral("Выберите новые инбаунды.")));
            return;
        }
        prepare([this, client, additions, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(Outcome<bool>::failure(prepared.error));
                return;
            }
            if (detected_ != ApiFlavor::ModernV3) {
                finish(Outcome<bool>::failure(
                    QStringLiteral("Добавление привязок требует 3x-ui 3.x.")));
                return;
            }
            readClient(client, [this, client, additions,
                                finish](Outcome<QJsonObject> fresh) mutable {
                if (!fresh.ok) {
                    finish(Outcome<bool>::failure(fresh.error));
                    return;
                }
                request(
                    "GET", QStringLiteral("/panel/api/inbounds/list"), {},
                    [this, client, additions, original = fresh.value,
                     finish](Outcome<QJsonValue> inbounds) mutable {
                        if (!inbounds.ok || !inbounds.value.isArray()) {
                            finish(Outcome<bool>::failure(inbounds.ok ? identityError()
                                                                      : inbounds.error));
                            return;
                        }
                        ClientDraft plan;
                        plan.inboundIds = additions;
                        const auto targets =
                            selectedInbounds(inbounds.value.toArray(), plan, config_.id);
                        if (!targets.ok) {
                            finish(Outcome<bool>::failure(targets.error));
                            return;
                        }
                        QJsonArray ids;
                        for (const int id : additions)
                            ids.append(id);
                        request(
                            "POST",
                            QStringLiteral("/panel/api/clients/") + encodedSegment(client.email) +
                                QStringLiteral("/attach"),
                            {{QStringLiteral("inboundIds"), ids}},
                            [this, client, additions, original,
                             finish](Outcome<QJsonValue> attached) mutable {
                                const QString partial =
                                    QStringLiteral("Привязки могли измениться. Обновите клиентов "
                                                   "перед повторением. ");
                                if (!attached.ok) {
                                    finish(Outcome<bool>::failure(partial + attached.error));
                                    return;
                                }
                                request(
                                    "GET",
                                    QStringLiteral("/panel/api/clients/get/") +
                                        encodedSegment(client.email),
                                    {},
                                    [client, additions, original, finish,
                                     partial](Outcome<QJsonValue> checked) mutable {
                                        if (!checked.ok) {
                                            finish(Outcome<bool>::failure(partial + checked.error));
                                            return;
                                        }
                                        const auto response = checked.value.toObject();
                                        const auto raw =
                                            response.value(QStringLiteral("client")).toObject();
                                        const auto actual = inboundIds(
                                            response.value(QStringLiteral("inboundIds")));
                                        QList<int> expected = client.inboundIds;
                                        expected.append(additions);
                                        bool same = raw.value(QStringLiteral("email")).toString() ==
                                                        client.email &&
                                                    raw.value(QStringLiteral("id")) ==
                                                        original.value(QStringLiteral("id"));
                                        // 3x-ui may fill missing credentials for a newly added
                                        // protocol. Every pre-existing nonempty credential must
                                        // remain unchanged.
                                        for (const auto* field :
                                             {"uuid", "password", "auth", "subId"}) {
                                            const auto key = QString::fromLatin1(field);
                                            if (!original.value(key).toString().isEmpty())
                                                same &= raw.value(key) == original.value(key);
                                        }
                                        if (!same || !actual.ok ||
                                            !sameInboundIds(actual.value, expected)) {
                                            finish(Outcome<bool>::failure(
                                                partial +
                                                QStringLiteral("Проверка идентификаторов или "
                                                               "привязок не пройдена.")));
                                            return;
                                        }
                                        finish(Outcome<bool>::success(true));
                                    });
                            },
                            false, true);
                    });
            });
        });
    });
}

void ThreeXUiApi::readClient(const Client& client, Callback<QJsonObject> callback) {
    if (client.serverId != config_.id || !validEmail(client.email) || client.id.isEmpty()) {
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
                    if (fresh.isEmpty() || !ids.ok || !sameIdentity(client, fresh, ids.value)) {
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
        QSet<int> seen;
        constexpr qint64 exactIntegerMax = 9007199254740991LL;
        bool valid = validEmail(draft.email) && !draft.inboundIds.isEmpty() &&
                     draft.totalBytes >= 0 && draft.totalBytes <= exactIntegerMax &&
                     draft.expiryTime >= -exactIntegerMax && draft.expiryTime <= exactIntegerMax &&
                     (draft.flow.isEmpty() || draft.flow == QStringLiteral("xtls-rprx-vision"));
        for (int id : draft.inboundIds) {
            if (id <= 0 || seen.contains(id))
                valid = false;
            seen.insert(id);
        }
        if (!draft.clientId.isEmpty() && QUuid(draft.clientId).isNull())
            valid = false;
        for (const auto& secret : {draft.password, draft.hysteriaAuth, draft.subId}) {
            if (secret.size() > 512)
                valid = false;
            for (const auto& character : secret) {
                if (character.category() == QChar::Other_Control)
                    valid = false;
            }
        }
        for (const auto& character : draft.subId) {
            if (character.isSpace() || character == QLatin1Char('/') ||
                character == QLatin1Char('\\'))
                valid = false;
        }
        if (!valid) {
            finish(Outcome<bool>::failure(QStringLiteral("Некорректные параметры клиента.")));
            return;
        }
        ClientDraft plan = draft;
        if (plan.clientId.isEmpty())
            plan.clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (plan.subId.isEmpty())
            plan.subId = randomSecret(24);
        prepare([this, plan, finish](Outcome<bool> prepared) mutable {
            if (!prepared.ok) {
                finish(std::move(prepared));
                return;
            }
            if (detected_ == ApiFlavor::ModernV3) {
                createModern(plan, finish);
                return;
            }
            if (plan.inboundIds.size() != 1) {
                finish(Outcome<bool>::failure(QStringLiteral(
                    "API v2 поддерживает создание только на одном inbound. Записи не изменены.")));
                return;
            }
            request(
                "GET", QStringLiteral("/panel/api/inbounds/list"), {},
                [this, plan, finish](Outcome<QJsonValue> response) mutable {
                    if (!response.ok || !response.value.isArray()) {
                        finish(Outcome<bool>::failure(
                            response.ok
                                ? QStringLiteral("Панель вернула некорректный список inbound.")
                                : response.error));
                        return;
                    }
                    const auto selected =
                        selectedInbounds(response.value.toArray(), plan, config_.id);
                    if (!selected.ok) {
                        finish(Outcome<bool>::failure(selected.error));
                        return;
                    }
                    if (plan.password.isEmpty() &&
                        selected.value.first().protocol == QStringLiteral("trojan"))
                        plan.password = randomSecret(32);
                    if (plan.hysteriaAuth.isEmpty() &&
                        selected.value.first().protocol == QStringLiteral("hysteria"))
                        plan.hysteriaAuth = randomSecret(32);
                    if (selected.value.first().protocol != QStringLiteral("hysteria"))
                        plan.hysteriaAuth.clear();
                    const auto inventory =
                        parseInventory(response.value.toArray(), {}, config_.id, false);
                    if (!inventory.ok) {
                        finish(Outcome<bool>::failure(inventory.error));
                        return;
                    }
                    const Client* existing = nullptr;
                    for (const auto& client : inventory.value.clients) {
                        if (client.email.compare(plan.email, Qt::CaseInsensitive) == 0) {
                            if (existing) {
                                finish(Outcome<bool>::failure(collisionError()));
                                return;
                            }
                            existing = &client;
                        } else if (client.subId == plan.subId || client.id == plan.clientId ||
                                   (selected.value.first().protocol == QStringLiteral("trojan") &&
                                    client.raw.value(QStringLiteral("password")).toString() ==
                                        plan.password) ||
                                   (selected.value.first().protocol == QStringLiteral("hysteria") &&
                                    client.raw.value(QStringLiteral("auth")).toString() ==
                                        plan.hysteriaAuth)) {
                            finish(Outcome<bool>::failure(collisionError()));
                            return;
                        }
                    }
                    if (existing) {
                        const bool same = existing->email == plan.email &&
                                          sameInboundIds(existing->inboundIds, plan.inboundIds) &&
                                          matchesDraft(existing->raw, plan, true,
                                                       selected.value.first().protocol);
                        finish(same ? Outcome<bool>::success(true)
                                    : Outcome<bool>::failure(collisionError()));
                        return;
                    }
                    const auto client = newClient(plan, selected.value, false);
                    const QString settings = QString::fromUtf8(
                        QJsonDocument(QJsonObject{{QStringLiteral("clients"), QJsonArray{client}}})
                            .toJson(QJsonDocument::Compact));
                    request(
                        "POST", QStringLiteral("/panel/api/inbounds/addClient"),
                        {{QStringLiteral("id"), plan.inboundIds.first()},
                         {QStringLiteral("settings"), settings}},
                        [finish](Outcome<QJsonValue> result) mutable {
                            finish(result.ok ? Outcome<bool>::success(true)
                                             : Outcome<bool>::failure(result.error));
                        },
                        true, true);
                });
        });
    });
}

void ThreeXUiApi::createModern(const ClientDraft& initialPlan, Callback<bool> callback) {
    request(
        "GET", QStringLiteral("/panel/api/inbounds/list"), {},
        [this, plan = initialPlan,
         callback = std::move(callback)](Outcome<QJsonValue> response) mutable {
            if (!response.ok || !response.value.isArray()) {
                callback(Outcome<bool>::failure(
                    response.ok ? QStringLiteral("Панель вернула некорректный список inbound.")
                                : response.error));
                return;
            }
            const auto selected = selectedInbounds(response.value.toArray(), plan, config_.id);
            if (!selected.ok) {
                callback(Outcome<bool>::failure(selected.error));
                return;
            }
            bool vmess = false;
            bool trojan = false;
            bool hysteria = false;
            for (const auto& inbound : selected.value) {
                vmess |= inbound.protocol == QStringLiteral("vmess");
                trojan |= inbound.protocol == QStringLiteral("trojan");
                hysteria |= inbound.protocol == QStringLiteral("hysteria");
                if (inbound.protocol == QStringLiteral("trojan") && plan.password.isEmpty())
                    plan.password = randomSecret(32);
                if (inbound.protocol == QStringLiteral("hysteria") && plan.hysteriaAuth.isEmpty())
                    plan.hysteriaAuth = randomSecret(32);
            }
            if (!hysteria)
                plan.hysteriaAuth.clear();
            const auto client = newClient(plan, selected.value, true);
            request(
                "GET", QStringLiteral("/panel/api/clients/list"), {},
                [this, plan, client, vmess, trojan, hysteria,
                 callback = std::move(callback)](Outcome<QJsonValue> listed) mutable {
                    if (!listed.ok || !listed.value.isArray()) {
                        callback(Outcome<bool>::failure(
                            listed.ok
                                ? QStringLiteral("Панель вернула некорректный список клиентов.")
                                : listed.error));
                        return;
                    }
                    bool exists = false;
                    for (const auto& entry : listed.value.toArray()) {
                        if (!entry.isObject()) {
                            callback(Outcome<bool>::failure(
                                QStringLiteral("Панель вернула некорректного клиента.")));
                            return;
                        }
                        const auto raw = entry.toObject();
                        if (raw.value(QStringLiteral("email"))
                                .toString()
                                .compare(plan.email, Qt::CaseInsensitive) == 0) {
                            if (exists ||
                                raw.value(QStringLiteral("email")).toString() != plan.email) {
                                callback(Outcome<bool>::failure(collisionError()));
                                return;
                            }
                            exists = true;
                        } else if (raw.value(QStringLiteral("subId")).toString() == plan.subId ||
                                   identifier(raw) == plan.clientId ||
                                   (trojan && raw.value(QStringLiteral("password")).toString() ==
                                                  plan.password) ||
                                   (hysteria && raw.value(QStringLiteral("auth")).toString() ==
                                                    plan.hysteriaAuth)) {
                            callback(Outcome<bool>::failure(collisionError()));
                            return;
                        }
                    }
                    if (!exists) {
                        request(
                            "POST", QStringLiteral("/panel/api/clients/add"),
                            {{QStringLiteral("client"), client},
                             {QStringLiteral("inboundIds"), jsonIds(plan.inboundIds)}},
                            [this, plan, vmess,
                             callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                                if (!result.ok) {
                                    callback(Outcome<bool>::failure(result.error));
                                    return;
                                }
                                verifyCreated(plan, true,
                                              QStringLiteral("Клиент создан, но его состояние не "
                                                             "подтверждено. Обновите список. "),
                                              std::move(callback), vmess);
                            },
                            false, true);
                        return;
                    }
                    request(
                        "GET",
                        QStringLiteral("/panel/api/clients/get/") + encodedSegment(plan.email), {},
                        [this, plan, vmess,
                         callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                            if (!result.ok) {
                                callback(Outcome<bool>::failure(result.error));
                                return;
                            }
                            const auto object = result.value.toObject();
                            const auto raw = object.value(QStringLiteral("client")).toObject();
                            const auto ids = inboundIds(object.value(QStringLiteral("inboundIds")));
                            if (!ids.ok || !matchesDraft(raw, plan, true, {}, vmess)) {
                                callback(Outcome<bool>::failure(collisionError()));
                                return;
                            }
                            QList<int> missing;
                            for (int id : ids.value) {
                                if (!plan.inboundIds.contains(id)) {
                                    callback(Outcome<bool>::failure(collisionError()));
                                    return;
                                }
                            }
                            for (int id : plan.inboundIds)
                                if (!ids.value.contains(id))
                                    missing.append(id);
                            if (missing.isEmpty()) {
                                callback(Outcome<bool>::success(true));
                                return;
                            }
                            // Attach is additive; never re-run add, patch another record's
                            // fields, or detach a binding to force the requested shape.
                            request(
                                "GET", QStringLiteral("/panel/api/inbounds/list"), {},
                                [this, plan, missing, vmess, callback = std::move(callback)](
                                    Outcome<QJsonValue> refreshed) mutable {
                                    if (!refreshed.ok || !refreshed.value.isArray()) {
                                        callback(Outcome<bool>::failure(
                                            refreshed.ok
                                                ? QStringLiteral(
                                                      "Панель вернула некорректный список inbound.")
                                                : refreshed.error));
                                        return;
                                    }
                                    const auto selected = selectedInbounds(
                                        refreshed.value.toArray(), plan, config_.id);
                                    if (!selected.ok) {
                                        callback(Outcome<bool>::failure(selected.error));
                                        return;
                                    }
                                    request(
                                        "POST",
                                        QStringLiteral("/panel/api/clients/") +
                                            encodedSegment(plan.email) + QStringLiteral("/attach"),
                                        {{QStringLiteral("inboundIds"), jsonIds(missing)}},
                                        [this, plan, vmess, callback = std::move(callback)](
                                            Outcome<QJsonValue> attached) mutable {
                                            if (!attached.ok) {
                                                callback(Outcome<bool>::failure(attached.error));
                                                return;
                                            }
                                            verifyCreated(
                                                plan, false,
                                                QStringLiteral(
                                                    "Привязка отправлена, но её состояние не "
                                                    "подтверждено. Обновите список. "),
                                                std::move(callback), vmess);
                                        },
                                        false, true);
                                });
                        });
                });
        });
}

void ThreeXUiApi::verifyCreated(const ClientDraft& plan, bool mayDisable,
                                const QString& partialError, Callback<bool> callback, bool vmess) {
    request("GET", QStringLiteral("/panel/api/clients/get/") + encodedSegment(plan.email), {},
            [this, plan, mayDisable, partialError, vmess,
             callback = std::move(callback)](Outcome<QJsonValue> result) mutable {
                if (!result.ok) {
                    callback(Outcome<bool>::failure(partialError + result.error));
                    return;
                }
                const auto response = result.value.toObject();
                const auto raw = response.value(QStringLiteral("client")).toObject();
                const auto ids = inboundIds(response.value(QStringLiteral("inboundIds")));
                if (!ids.ok || !sameInboundIds(ids.value, plan.inboundIds) ||
                    !matchesDraft(raw, plan, !mayDisable, {}, vmess)) {
                    callback(Outcome<bool>::failure(
                        partialError +
                        QStringLiteral("Идентификаторы, ограничения или привязки отличаются.")));
                    return;
                }
                if (mayDisable && !plan.enable && raw.value(QStringLiteral("enable")).toBool()) {
                    auto model = modernModel(raw, {});
                    if (!model.ok) {
                        callback(Outcome<bool>::failure(partialError + model.error));
                        return;
                    }
                    model.value.insert(QStringLiteral("enable"), false);
                    request(
                        "POST",
                        QStringLiteral("/panel/api/clients/update/") + encodedSegment(plan.email),
                        model.value,
                        [this, plan, vmess,
                         callback = std::move(callback)](Outcome<QJsonValue> updated) mutable {
                            const auto partial = QStringLiteral(
                                "Клиент создан, но отключение не подтверждено. Обновите список. ");
                            if (!updated.ok) {
                                callback(Outcome<bool>::failure(partial + updated.error));
                                return;
                            }
                            verifyCreated(plan, false, partial, std::move(callback), vmess);
                        },
                        false, true);
                    return;
                }
                if (raw.value(QStringLiteral("enable")).toBool() != plan.enable) {
                    callback(Outcome<bool>::failure(
                        partialError + QStringLiteral("Состояние enable отличается.")));
                    return;
                }
                callback(Outcome<bool>::success(true));
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
void ThreeXUiApi::binaryRequest(const QString& path, const QByteArray& upload,
                                const QByteArray& contentType, Callback<QByteArray> callback,
                                qint64 limit, int timeoutMs, bool mutation) {
    lastHttpStatus_ = 0;
    const auto validated = validatePanelUrl(config_);
    if (!validated.ok || !network_) {
        callback(Outcome<QByteArray>::failure(
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
    request.setTransferTimeout(timeoutMs);
    request.setRawHeader("Accept", mutation ? "application/json" : "application/octet-stream");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    if (config_.auth == AuthKind::Token)
        request.setRawHeader("Authorization", "Bearer " + config_.token.toUtf8());
    if (mutation && !csrf_.isEmpty())
        request.setRawHeader("X-CSRF-Token", csrf_.toUtf8());
    QNetworkReply* reply = nullptr;
    if (mutation) {
        request.setRawHeader("Content-Type", contentType);
        request.setHeader(QNetworkRequest::ContentLengthHeader, upload.size());
        request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        auto* device = new OneShotUpload(upload);
        reply = network_->post(request, device);
        device->setParent(reply);
    } else {
        reply = network_->get(request);
    }
    reply->setReadBufferSize(limit + 1);
    struct State {
        QByteArray bytes;
        bool tooLarge = false;
        bool timedOut = false;
        bool delivered = false;
        QElapsedTimer clock;
    };
    auto state = std::make_shared<State>();
    state->clock.start();
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [state, reply] {
        state->timedOut = true;
        reply->abort();
    });
    timer->start(timeoutMs);
    auto read = [state, reply, limit] {
        const qint64 left = limit + 1 - state->bytes.size();
        if (left > 0)
            state->bytes.append(reply->read(left));
        if (state->bytes.size() > limit || reply->bytesAvailable() > 0) {
            state->tooLarge = true;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, state, limit] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) {
            state->tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QIODevice::readyRead, this, read);
    connect(this, &QObject::destroyed, reply, [reply] {
        reply->disconnect();
        reply->abort();
        reply->deleteLater();
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, timer, state, read, callback = std::move(callback), mutation,
         limit]() mutable {
            if (state->delivered)
                return;
            state->delivered = true;
            timer->stop();
            if (reply->isOpen())
                read();
            lastHttpStatus_ = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            lastLatencyMs_ = static_cast<int>(
                std::min<qint64>(state->clock.elapsed(), std::numeric_limits<int>::max()));
            const int status = lastHttpStatus_;
            const auto error = reply->error();
            reply->deleteLater();
            if (status == 401 || status == 403)
                loggedIn_ = false;
            QString message;
            if (state->tooLarge)
                message = QStringLiteral("Ответ панели превышает лимит %1 МиБ.")
                              .arg(limit / (1024 * 1024));
            else if (state->timedOut || error == QNetworkReply::TimeoutError)
                message = mutation
                              ? QStringLiteral("Время ожидания истекло: результат восстановления "
                                               "неизвестен. Проверьте панель перед повторением.")
                              : QStringLiteral("Время ожидания загрузки базы истекло.");
            else if (status >= 300 && status < 400)
                message =
                    QStringLiteral("Панель перенаправляет запрос. Укажите конечный URL панели.");
            else if (status == 401 || status == 403)
                message = QStringLiteral("Панель отклонила авторизацию. Проверьте подключение.");
            else if (status >= 400)
                message = QStringLiteral("Панель вернула HTTP %1.").arg(status);
            else if (error != QNetworkReply::NoError || status < 200 || status >= 300)
                message = mutation
                              ? QStringLiteral("Ошибка соединения: результат восстановления "
                                               "неизвестен. Проверьте панель перед повторением.")
                              : QStringLiteral("Не удалось загрузить базу панели.");
            if (!message.isEmpty()) {
                callback(Outcome<QByteArray>::failure(message));
                return;
            }
            callback(Outcome<QByteArray>::success(std::move(state->bytes)));
        });
}

void ThreeXUiApi::downloadDatabase(Callback<QByteArray> callback) {
    enqueue([this, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<QByteArray> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        authenticate([this, finish](Outcome<bool> authenticated) mutable {
            if (!authenticated.ok) {
                finish(Outcome<QByteArray>::failure(authenticated.error));
                return;
            }
            binaryRequest(
                QStringLiteral("/panel/api/server/getDb"), {}, {},
                [finish](Outcome<QByteArray> result) mutable {
                    if (!result.ok) {
                        finish(std::move(result));
                        return;
                    }
                    const auto inspected = inspectDatabaseBackup(result.value);
                    finish(inspected.ok ? std::move(result)
                                        : Outcome<QByteArray>::failure(inspected.error));
                },
                MaximumBackupBytes, 60000, false);
        });
    });
}

void ThreeXUiApi::restoreDatabase(const QByteArray& data, bool keepHostSettings,
                                  Callback<bool> callback) {
    enqueue([this, data, keepHostSettings, callback = std::move(callback)]() mutable {
        auto finish = [this, callback = std::move(callback)](Outcome<bool> result) mutable {
            complete();
            if (callback)
                callback(std::move(result));
        };
        const auto inspected = inspectDatabaseBackup(data);
        if (!inspected.ok) {
            finish(Outcome<bool>::failure(inspected.error));
            return;
        }
        authenticate([this, data, keepHostSettings, info = inspected.value,
                      finish](Outcome<bool> authenticated) mutable {
            if (!authenticated.ok) {
                finish(std::move(authenticated));
                return;
            }
            QByteArray boundary;
            do {
                boundary = "3x-control-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
            } while (data.contains(boundary));
            const QByteArray fileHeader =
                "--" + boundary +
                "\r\nContent-Disposition: form-data; name=\"db\"; filename=\"fleet" +
                info.extension().toLatin1() +
                "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
            const QByteArray footer =
                "\r\n--" + boundary +
                "\r\nContent-Disposition: form-data; name=\"keepHostSettings\"\r\n\r\n" +
                (keepHostSettings ? "true" : "false") + "\r\n--" + boundary + "--\r\n";
            QByteArray upload;
            upload.reserve(fileHeader.size() + data.size() + footer.size());
            upload.append(fileHeader);
            upload.append(data);
            upload.append(footer);
            binaryRequest(
                QStringLiteral("/panel/api/server/importDB"), upload,
                "multipart/form-data; boundary=" + boundary,
                [this, finish](Outcome<QByteArray> result) mutable {
                    if (!result.ok) {
                        finish(Outcome<bool>::failure(result.error));
                        return;
                    }
                    QJsonParseError error;
                    const auto document = QJsonDocument::fromJson(result.value, &error);
                    const auto success = document.object().value(QStringLiteral("success"));
                    if (error.error != QJsonParseError::NoError || !document.isObject() ||
                        !success.isBool() || !success.toBool()) {
                        if (config_.auth == AuthKind::Password)
                            loggedIn_ = false;
                        finish(Outcome<bool>::failure(QStringLiteral(
                            "Панель не подтвердила восстановление базы. Проверьте её состояние.")));
                        return;
                    }
                    // Imported panel settings may invalidate every session and
                    // change API version. Keep no stale CSRF/detection state.
                    loggedIn_ = false;
                    csrf_.clear();
                    detected_ = config_.api;
                    finish(Outcome<bool>::success(true));
                },
                kResponseLimit, 120000, true);
        });
    });
}
} // namespace fleet
