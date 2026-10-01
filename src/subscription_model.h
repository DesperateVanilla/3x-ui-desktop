#pragma once
#include "domain.h"
#include <QHash>
#include <QStringList>

namespace fleet {
enum class SubscriptionState { Active, Disabled, Expired, QuotaExceeded, Unbound, Mixed };
struct SubscriptionGroup {
    QString key;
    QString email;
    QList<Client> members;
    QStringList serverIds;
    QStringList protocols;
    int primaryIndex = 0;
    bool settingsDiffer = false;
    std::optional<bool> online;
    const Client& primary() const;
    SubscriptionState state(qint64 now) const;
};
struct SubscriptionFilter {
    QString text;
    QString serverId;
    QString protocol;
    int state = -1; // SubscriptionState; 6 means online.
    int expiry = 0; // Any, unlimited, next seven days, expired.
};
struct SubscriptionStats {
    int total = 0, active = 0, online = 0, onlineKnown = 0, expiring = 0;
    int disabled = 0, expired = 0, quotaExceeded = 0, mixed = 0;
    qint64 usedBytes = 0;
};
QList<SubscriptionGroup> groupSubscriptions(const QList<ServerConfig>& servers,
                                            const QHash<QString, Inventory>& inventories,
                                            const QString& masterServerId);
bool matchesSubscription(const SubscriptionGroup& group, const SubscriptionFilter& filter,
                         const QHash<QString, QString>& serverNames, qint64 now);
SubscriptionStats subscriptionStats(const QList<SubscriptionGroup>& groups, qint64 now);
} // namespace fleet
