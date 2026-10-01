#pragma once
#include "domain.h"
#include <QSqlDatabase>

class QThread;

namespace fleet {
class LocalStore {
  public:
    LocalStore();
    ~LocalStore();
    LocalStore(const LocalStore&) = delete;
    LocalStore& operator=(const LocalStore&) = delete;
    OperationResult open(const QString& filePath);
    Outcome<QList<ServerConfig>> servers() const;
    OperationResult saveServer(const ServerConfig& config);
    OperationResult removeServer(const QString& serverId);
    OperationResult appendSnapshot(const Snapshot& snapshot);
    Outcome<QList<Snapshot>> history(const QString& serverId, const QDateTime& since) const;
    OperationResult appendEvent(const ActivityEvent& event);
    Outcome<QList<ActivityEvent>> events(int limit = 100) const;
    OperationResult prune(const QDateTime& before);
    static Outcome<QByteArray> protect(const QByteArray& plaintext);
    static Outcome<QByteArray> unprotect(const QByteArray& ciphertext);

  private:
    QString accessError() const;
    void closeDatabase();
    QString connectionName_;
    QSqlDatabase db_;
    QThread* ownerThread_ = nullptr;
};
} // namespace fleet
