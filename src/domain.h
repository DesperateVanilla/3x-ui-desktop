#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <functional>
#include <optional>

namespace fleet {
enum class Health { Unknown, Online, Warning, Offline };
enum class AuthKind { Token, Password };
enum class ApiFlavor { Auto, LegacyV2, ModernV3 };
enum class ServerAction { RestartXray, UpdateGeofiles, InstallXray };

template <class T> struct Outcome {
    bool ok = false;
    T value{};
    QString error;
    static Outcome success(T value) { return {true, std::move(value), {}}; }
    static Outcome failure(QString error) { return {false, {}, std::move(error)}; }
};
using OperationResult = Outcome<bool>;
template <class T> using Callback = std::function<void(Outcome<T>)>;

struct ServerConfig {
    QString id;
    QString name;
    QString location;
    QUrl panelUrl;
    QUrl subscriptionUrl;
    AuthKind auth = AuthKind::Token;
    ApiFlavor api = ApiFlavor::Auto;
    QString username;
    QString password;
    QString token;
    QString twoFactorCode; // One-time input; never persist.
    bool allowHttp = false;
};

struct Snapshot {
    QString serverId;
    QDateTime at;
    Health health = Health::Unknown;
    std::optional<double> cpu;
    std::optional<double> memoryPercent;
    std::optional<double> diskPercent;
    std::optional<double> rxBps;
    std::optional<double> txBps;
    std::optional<qint64> receivedBytes;
    std::optional<qint64> sentBytes;
    std::optional<int> online;
    std::optional<int> latencyMs;
    QString panelVersion;
    QString xrayVersion;
    qint64 uptimeSeconds = 0;
    QString error;
};

struct Inbound {
    QString serverId;
    int id = 0;
    QString remark;
    QString protocol;
    int port = 0;
    bool enable = true;
};

struct Client {
    QString serverId;
    QList<int> inboundIds;
    QString id; // UUID/password identifier, never the V3 database numeric id.
    QString email;
    QString subId;
    QString protocol;
    bool enable = true;
    std::optional<bool> online;
    qint64 usedBytes = 0;
    qint64 totalBytes = 0;
    qint64 expiryTime = 0; // ms UTC. 0 unlimited; negative = first-use relative expiry.
    QJsonObject raw;
};

struct Inventory {
    QList<Inbound> inbounds;
    QList<Client> clients;
};
struct ClientDraft {
    QList<int> inboundIds;
    QString email;
    qint64 totalBytes = 0;
    qint64 expiryTime = 0;
    bool enable = true;
    QString flow;
    QString clientId;
    QString password;
    QString subId;
};
struct ClientTarget {
    QString serverId;
    QList<int> inboundIds;
};
struct ProvisionNodeResult {
    QString serverId;
    QList<int> inboundIds;
    bool ok = false;
    QString error;
};
struct ProvisionSummary {
    QList<ProvisionNodeResult> nodes;
    QString clientId;
    QString subId;
};
struct ClientPatch {
    std::optional<qint64> totalBytes;
    std::optional<qint64> expiryTime;
    std::optional<bool> enable;
};
struct ActivityEvent {
    QDateTime at;
    QString serverId;
    QString serverName;
    QString action;
    bool success = true;
    QString detail;
};
} // namespace fleet
