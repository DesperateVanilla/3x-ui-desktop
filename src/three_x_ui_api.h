#pragma once
#include "domain.h"
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>

namespace fleet {
class ThreeXUiApi : public QObject {
    Q_OBJECT
  public:
    explicit ThreeXUiApi(ServerConfig config, QObject* parent = nullptr,
                         QNetworkAccessManager* injectedManager = nullptr);
    const ServerConfig& config() const { return config_; }
    ApiFlavor detectedFlavor() const { return detected_; }
    void fetchStatus(Callback<Snapshot> callback);
    void fetchInventory(Callback<Inventory> callback);
    void createClient(const ClientDraft& draft, Callback<bool> callback);
    void updateClient(const Client& client, const ClientPatch& patch, Callback<bool> callback);
    void deleteClient(const Client& client, Callback<bool> callback);
    void resetClientTraffic(const Client& client, Callback<bool> callback);
    void serverAction(ServerAction action, const QString& version, Callback<bool> callback);
    static Outcome<QUrl> validatePanelUrl(const ServerConfig& config);
    static Outcome<Snapshot> parseStatus(const QJsonObject& object, const QString& serverId);
    static Outcome<Inventory> parseInventory(const QJsonArray& inbounds,
                                             const QJsonArray& modernClients,
                                             const QString& serverId, bool modern);

  private:
    using JsonCallback = Callback<QJsonValue>;
    ServerConfig config_;
    QNetworkAccessManager* network_ = nullptr;
    ApiFlavor detected_ = ApiFlavor::Auto;
    bool loggedIn_ = false;
    bool busy_ = false;
    QString csrf_;
    QQueue<std::function<void()>> operations_;
    int lastHttpStatus_ = 0;
    int lastLatencyMs_ = 0;
    enum class Mutation { Update, Delete, Reset };
    void enqueue(std::function<void()> operation);
    void complete();
    void authenticate(Callback<bool> callback);
    void discover(Callback<bool> callback);
    void request(const QByteArray& method, const QString& path, const QJsonObject& body,
                 JsonCallback callback, bool form = false, bool mutation = false);
    void inventory(JsonCallback callback);
    void readClient(const Client& client, Callback<QJsonObject> callback);
    void prepare(Callback<bool> callback);
    void mutate(const Client& client, const ClientPatch& patch, Mutation mutation,
                Callback<bool> callback);
};
} // namespace fleet
