#include "subscription_model.h"
#include <QCryptographicHash>
#include <QSet>
#include <algorithm>
#include <limits>

namespace fleet {
namespace {
constexpr qint64 week = 7LL * 24 * 3600000;
QString identityKey(const Client& client, const QString& serverId, int index) {
    const QStringList parts =
        !client.subId.isEmpty() ? QStringList{"sub", client.subId, client.email}
        : !client.id.isEmpty()
            ? QStringList{"credential", client.id, client.email}
            : QStringList{"local", serverId, client.email, QString::number(index)};
    QByteArray data;
    for (const auto& part : parts) {
        const auto encoded = part.toUtf8();
        data += QByteArray::number(encoded.size()) + ':' + encoded;
    }
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool expiring(qint64 expiry, qint64 now) {
    // Subtraction is only safe after bounding nonnegative timestamps.
    return now >= 0 && expiry > now && expiry - now <= week;
}
} // namespace

const Client& SubscriptionGroup::primary() const {
    Q_ASSERT(!members.isEmpty() && primaryIndex >= 0 && primaryIndex < members.size());
    return members.at(primaryIndex);
}
SubscriptionState SubscriptionGroup::state(qint64 now) const {
    if (settingsDiffer)
        return SubscriptionState::Mixed;
    const auto& client = primary();
    if (client.inboundIds.isEmpty())
        return SubscriptionState::Unbound;
    if (!client.enable)
        return SubscriptionState::Disabled;
    if (client.expiryTime > 0 && client.expiryTime <= now)
        return SubscriptionState::Expired;
    if (client.totalBytes > 0 && client.usedBytes >= client.totalBytes)
        return SubscriptionState::QuotaExceeded;
    return SubscriptionState::Active;
}
QList<SubscriptionGroup> groupSubscriptions(const QList<ServerConfig>& servers,
                                            const QHash<QString, Inventory>& inventories,
                                            const QString& masterServerId) {
    QList<SubscriptionGroup> groups;
    QHash<QString, int> positions;
    for (const auto& server : servers) {
        const auto inventory = inventories.constFind(server.id);
        if (inventory == inventories.cend())
            continue;
        for (int i = 0; i < inventory->clients.size(); ++i) {
            Client client = inventory->clients.at(i);
            client.serverId = server.id;
            const auto key = identityKey(client, server.id, i);
            int position = positions.value(key, -1);
            if (position < 0) {
                position = static_cast<int>(groups.size());
                SubscriptionGroup group;
                group.key = key;
                group.email = client.email;
                groups.append(group);
                positions.insert(key, position);
            }
            auto& group = groups[position];
            const int member = static_cast<int>(group.members.size());
            group.members.append(client);
            if (server.id == masterServerId &&
                group.members.at(group.primaryIndex).serverId != masterServerId)
                group.primaryIndex = member;
            if (!group.serverIds.contains(server.id))
                group.serverIds.append(server.id);
            bool foundProtocol = false;
            for (const auto& inbound : inventory->inbounds) {
                if (!client.inboundIds.contains(inbound.id) || inbound.protocol.isEmpty())
                    continue;
                const auto protocol = inbound.protocol.toLower();
                if (!group.protocols.contains(protocol))
                    group.protocols.append(protocol);
                foundProtocol = true;
            }
            if (!foundProtocol && !client.protocol.isEmpty() &&
                !group.protocols.contains(client.protocol.toLower()))
                group.protocols.append(client.protocol.toLower());
        }
    }
    for (auto& group : groups) {
        bool anyOnline = false, allKnown = true;
        const auto& main = group.primary();
        for (const auto& member : group.members) {
            group.settingsDiffer |= member.enable != main.enable ||
                                    member.totalBytes != main.totalBytes ||
                                    member.expiryTime != main.expiryTime;
            anyOnline |= member.online.value_or(false);
            allKnown &= member.online.has_value();
        }
        if (anyOnline)
            group.online = true;
        else if (allKnown)
            group.online = false;
        group.protocols.sort();
    }
    return groups;
}
bool matchesSubscription(const SubscriptionGroup& group, const SubscriptionFilter& filter,
                         const QHash<QString, QString>& serverNames, qint64 now) {
    if (!filter.serverId.isEmpty() && !group.serverIds.contains(filter.serverId))
        return false;
    if (!filter.protocol.isEmpty() && !group.protocols.contains(filter.protocol.toLower()))
        return false;
    if (filter.state == 6) {
        if (!group.online.value_or(false))
            return false;
    } else if (filter.state >= 0 && int(group.state(now)) != filter.state)
        return false;
    const qint64 expiry = group.primary().expiryTime;
    if ((filter.expiry == 1 && expiry != 0) || (filter.expiry == 2 && !expiring(expiry, now)) ||
        (filter.expiry == 3 && !(expiry > 0 && expiry <= now)))
        return false;
    const QString query = filter.text.trimmed();
    if (query.isEmpty() || group.email.contains(query, Qt::CaseInsensitive))
        return true;
    for (const auto& id : group.serverIds)
        if (serverNames.value(id).contains(query, Qt::CaseInsensitive))
            return true;
    return false;
}
SubscriptionStats subscriptionStats(const QList<SubscriptionGroup>& groups, qint64 now) {
    SubscriptionStats result;
    result.total = static_cast<int>(groups.size());
    for (const auto& group : groups) {
        const auto state = group.state(now);
        result.active += state == SubscriptionState::Active;
        result.disabled += state == SubscriptionState::Disabled;
        result.expired += state == SubscriptionState::Expired;
        result.quotaExceeded += state == SubscriptionState::QuotaExceeded;
        result.mixed += state == SubscriptionState::Mixed;
        result.expiring +=
            state == SubscriptionState::Active && expiring(group.primary().expiryTime, now);
        result.online += group.online.value_or(false);
        result.onlineKnown += group.online.has_value();
        const qint64 used = std::max(qint64(0), group.primary().usedBytes);
        result.usedBytes = used > std::numeric_limits<qint64>::max() - result.usedBytes
                               ? std::numeric_limits<qint64>::max()
                               : result.usedBytes + used;
    }
    return result;
}
} // namespace fleet
