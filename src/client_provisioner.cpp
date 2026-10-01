#include "client_provisioner.h"
#include "three_x_ui_api.h"

#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace fleet {
namespace {
constexpr qint64 MaximumExactJsonInteger = 9'007'199'254'740'991;

bool validEmail(const QString& email) {
    if (email.isEmpty() || email.size() > 128 || email != email.trimmed())
        return false;
    for (const QChar character : email) {
        if (character.isSpace() || character.category() == QChar::Other_Control ||
            character == QLatin1Char('\\') || character == QLatin1Char('/') ||
            character == QLatin1Char('?') || character == QLatin1Char('#'))
            return false;
    }
    return true;
}

QString randomSecret(int length) {
    static const QString alphabet =
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    QString value;
    value.reserve(length);
    for (int i = 0; i < length; ++i)
        value.append(alphabet.at(static_cast<int>(QRandomGenerator::system()->generate() & 63U)));
    return value;
}

bool validPassword(const QString& password) {
    if (password.isEmpty())
        return true;
    if (password.size() > 256)
        return false;
    for (const QChar character : password)
        if (character.category() == QChar::Other_Control)
            return false;
    return true;
}

QString safeText(QString text, const ServerConfig& config, const ClientDraft& draft) {
    for (const auto& secret : {config.username, config.password, config.token, config.twoFactorCode,
                               draft.password, draft.clientId, draft.subId}) {
        if (!secret.isEmpty())
            text.replace(secret, QStringLiteral("[скрыто]"));
    }
    for (qsizetype i = 0; i < text.size(); ++i)
        if (text.at(i).category() == QChar::Other_Control)
            text[i] = QLatin1Char(' ');
    return text.left(1024);
}

QString serverPrefix(const ServerConfig& config, const ClientDraft& draft) {
    const QString name =
        safeText(config.name.isEmpty() ? config.id : config.name, config, draft).left(160);
    return QStringLiteral("Сервер «%1»: ").arg(name);
}

QString validateInventory(const Inventory& inventory, const ClientTarget& target,
                          const ClientDraft& draft, ApiFlavor flavor) {
    if (flavor != ApiFlavor::LegacyV2 && flavor != ApiFlavor::ModernV3)
        return QStringLiteral("Не удалось определить возможности API панели. Выдача не начата.");
    if (flavor == ApiFlavor::LegacyV2 && target.inboundIds.size() != 1)
        return QStringLiteral(
            "Панель v2 поддерживает выдачу только в один inbound на сервере. Выдача не начата.");
    QSet<QString> protocols;
    for (const int id : target.inboundIds) {
        const Inbound* selected = nullptr;
        for (const auto& inbound : inventory.inbounds) {
            if (inbound.id != id)
                continue;
            if (selected)
                return QStringLiteral(
                    "Панель вернула неоднозначный список inbound. Выдача не начата.");
            selected = &inbound;
        }
        if (!selected || !selected->enable)
            return QStringLiteral("Выбранный inbound удалён или отключён. Выдача не начата.");
        if (selected->protocol != QStringLiteral("vless") &&
            selected->protocol != QStringLiteral("vmess") &&
            selected->protocol != QStringLiteral("trojan"))
            return QStringLiteral(
                "Выбранный протокол не поддерживает выдачу клиентов. Выдача не начата.");
        if (!draft.flow.isEmpty() && selected->protocol != QStringLiteral("vless"))
            return QStringLiteral("Flow Vision доступен только для VLESS на всех выбранных "
                                  "inbound. Выдача не начата.");
        protocols.insert(selected->protocol);
    }
    const Client* existing = nullptr;
    for (const auto& client : inventory.clients) {
        if (client.email != draft.email) {
            const QString uuid = client.raw.value(QStringLiteral("uuid")).toString();
            const QString id = client.raw.value(QStringLiteral("id")).toString();
            if (client.email.compare(draft.email, Qt::CaseInsensitive) == 0 ||
                client.id == draft.clientId || uuid == draft.clientId || id == draft.clientId ||
                client.subId == draft.subId ||
                (protocols.contains(QStringLiteral("trojan")) &&
                 (client.id == draft.password ||
                  client.raw.value(QStringLiteral("password")).toString() == draft.password)))
                return QStringLiteral(
                    "Email, UUID, пароль или subId уже заняты другим клиентом. Выдача не начата.");
            continue;
        }
        if (existing)
            return QStringLiteral(
                "На панели несколько клиентов с выбранным email. Выдача не начата.");
        existing = &client;
    }
    if (!existing)
        return {};
    const QString expectedId =
        flavor == ApiFlavor::LegacyV2 && protocols.contains(QStringLiteral("trojan"))
            ? draft.password
            : draft.clientId;
    if (existing->id != expectedId || existing->subId != draft.subId ||
        existing->totalBytes != draft.totalBytes || existing->expiryTime != draft.expiryTime ||
        existing->enable != draft.enable ||
        existing->raw.value(QStringLiteral("flow")).toString() != draft.flow ||
        (protocols.contains(QStringLiteral("trojan")) &&
         existing->raw.value(QStringLiteral("password")).toString() != draft.password) ||
        (protocols.contains(QStringLiteral("vmess")) &&
         existing->raw.value(QStringLiteral("security")).toString() != QStringLiteral("auto")))
        return QStringLiteral("Email уже занят клиентом с другими параметрами или идентификатором. "
                              "Выдача не начата.");
    for (const int id : existing->inboundIds) {
        if (!target.inboundIds.contains(id))
            return QStringLiteral(
                "Существующий клиент привязан к другим inbound. Выдача не начата.");
    }
    return {};
}
} // namespace

struct ClientProvisioner::Run {
    QList<ClientTarget> targets;
    ClientDraft draft;
    ProvisionSummary summary;
    Callback<ProvisionSummary> callback;
    QList<qsizetype> writeOrder;
    qsizetype preflightIndex = 0;
    qsizetype writeIndex = 0;
    quint64 generation = 0;
    bool finished = false;
    QMetaObject::Connection pendingDestroyed;
};

ClientProvisioner::ClientProvisioner(const QHash<QString, ThreeXUiApi*>& apis, QObject* parent)
    : QObject(parent) {
    for (auto iterator = apis.cbegin(); iterator != apis.cend(); ++iterator)
        apis_.insert(iterator.key(), QPointer<ThreeXUiApi>(iterator.value()));
}

void ClientProvisioner::provision(const QList<ClientTarget>& targets, const ClientDraft& draft,
                                  const QString& masterServerId,
                                  Callback<ProvisionSummary> callback) {
    auto reject = [&callback](const QString& error) {
        if (callback)
            callback(Outcome<ProvisionSummary>::failure(error));
    };
    if (QThread::currentThread() != thread()) {
        reject(QStringLiteral("Выдачу нужно запустить в потоке координатора."));
        return;
    }
    if (busy_) {
        reject(QStringLiteral("Другая выдача клиентов ещё не завершена."));
        return;
    }
    static const QRegularExpression uuidPattern(
        QStringLiteral("^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$"));
    static const QRegularExpression subIdPattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (targets.isEmpty() || !validEmail(draft.email) || draft.totalBytes < 0 ||
        draft.totalBytes > MaximumExactJsonInteger || draft.expiryTime < -MaximumExactJsonInteger ||
        draft.expiryTime > MaximumExactJsonInteger ||
        (draft.expiryTime > 0 &&
         !QDateTime::fromMSecsSinceEpoch(draft.expiryTime, QTimeZone::UTC).isValid()) ||
        (!draft.flow.isEmpty() && draft.flow != QStringLiteral("xtls-rprx-vision")) ||
        (!draft.clientId.isEmpty() &&
         (!uuidPattern.match(draft.clientId).hasMatch() || QUuid(draft.clientId).isNull())) ||
        !validPassword(draft.password) ||
        (!draft.subId.isEmpty() && !subIdPattern.match(draft.subId).hasMatch())) {
        reject(QStringLiteral("Неверные цели, email, квота, срок или общие параметры клиента."));
        return;
    }
    QSet<QString> serverIds;
    for (const auto& target : targets) {
        if (target.serverId.trimmed().isEmpty() || target.serverId != target.serverId.trimmed() ||
            serverIds.contains(target.serverId) || target.inboundIds.isEmpty()) {
            reject(QStringLiteral(
                "Список целей содержит пустой сервер, повтор или пустой список inbound."));
            return;
        }
        serverIds.insert(target.serverId);
        QSet<int> inboundIds;
        for (const int id : target.inboundIds) {
            if (id <= 0 || inboundIds.contains(id)) {
                reject(QStringLiteral("Список целей содержит неверный или повторяющийся inbound."));
                return;
            }
            inboundIds.insert(id);
        }
        const QPointer<ThreeXUiApi> api = apis_.value(target.serverId);
        if (!api || api->thread() != thread() || api->config().id != target.serverId) {
            reject(QStringLiteral("Одно из выбранных подключений недоступно. Выдача не начата."));
            return;
        }
    }
    auto run = std::make_shared<Run>();
    run->targets = targets;
    run->draft = draft;
    run->draft.inboundIds.clear();
    if (run->draft.clientId.isEmpty())
        run->draft.clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (run->draft.password.isEmpty())
        run->draft.password = randomSecret(32);
    if (run->draft.subId.isEmpty())
        run->draft.subId = randomSecret(24);
    run->summary.clientId = run->draft.clientId;
    run->summary.subId = run->draft.subId;
    run->callback = std::move(callback);
    qsizetype masterIndex = -1;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        run->summary.nodes.append({targets.at(i).serverId, targets.at(i).inboundIds, false, {}});
        if (!masterServerId.isEmpty() && targets.at(i).serverId == masterServerId)
            masterIndex = i;
        else
            run->writeOrder.append(i);
    }
    if (masterIndex >= 0)
        run->writeOrder.append(masterIndex);
    busy_ = true;
    run_ = run;
    preflightNext(run);
}

void ClientProvisioner::finish(const std::shared_ptr<Run>& run, Outcome<ProvisionSummary> result) {
    if (!run || run->finished || run_ != run)
        return;
    run->finished = true;
    ++run->generation;
    QObject::disconnect(run->pendingDestroyed);
    run->pendingDestroyed = {};
    busy_ = false;
    run_.reset();
    auto callback = std::move(run->callback);
    if (callback)
        callback(std::move(result)); // Callback may destroy the coordinator or start a new run.
}

void ClientProvisioner::preflightNext(const std::shared_ptr<Run>& run) {
    if (run->finished || run_ != run)
        return;
    if (run->preflightIndex == run->targets.size()) {
        createNext(run);
        return;
    }
    const auto target = run->targets.at(run->preflightIndex);
    const QPointer<ThreeXUiApi> api = apis_.value(target.serverId);
    if (!api) {
        finish(run, Outcome<ProvisionSummary>::failure(QStringLiteral(
                        "Подключение закрыто во время проверки. Выдача не начата.")));
        return;
    }
    const ServerConfig config = api->config();
    const QString prefix = serverPrefix(config, run->draft);
    const quint64 generation = ++run->generation;
    run->pendingDestroyed =
        connect(api, &QObject::destroyed, this, [this, run, generation, prefix] {
            if (run->finished || run_ != run || run->generation != generation)
                return;
            finish(run,
                   Outcome<ProvisionSummary>::failure(
                       prefix +
                       QStringLiteral("Подключение закрыто во время проверки. Выдача не начата.")));
        });
    QPointer<ClientProvisioner> self(this);
    api->fetchInventory([self, api, run, target, generation, config,
                         prefix](Outcome<Inventory> inventory) {
        if (!self || run->finished || self->run_ != run || run->generation != generation)
            return;
        QObject::disconnect(run->pendingDestroyed);
        run->pendingDestroyed = {};
        ++run->generation;
        if (!inventory.ok || !api) {
            self->finish(
                run,
                Outcome<ProvisionSummary>::failure(
                    prefix +
                    QStringLiteral("Не удалось проверить выбранную панель. Выдача не начата.") +
                    (inventory.error.isEmpty()
                         ? QString()
                         : QStringLiteral(" ") + safeText(inventory.error, config, run->draft))));
            return;
        }
        const QString error =
            validateInventory(inventory.value, target, run->draft, api->detectedFlavor());
        if (!error.isEmpty()) {
            self->finish(run, Outcome<ProvisionSummary>::failure(prefix + error));
            return;
        }
        ++run->preflightIndex;
        QTimer::singleShot(0, self, [self, run] {
            if (self)
                self->preflightNext(run);
        });
    });
}

void ClientProvisioner::createNext(const std::shared_ptr<Run>& run) {
    if (run->finished || run_ != run)
        return;
    if (run->writeIndex == run->writeOrder.size()) {
        finish(run, Outcome<ProvisionSummary>::success(std::move(run->summary)));
        return;
    }
    const qsizetype index = run->writeOrder.at(run->writeIndex);
    const auto target = run->targets.at(index);
    const QPointer<ThreeXUiApi> api = apis_.value(target.serverId);
    QPointer<ClientProvisioner> self(this);
    auto advance = [self, run] {
        if (!self || run->finished || self->run_ != run)
            return;
        ++run->writeIndex;
        QTimer::singleShot(0, self, [self, run] {
            if (self)
                self->createNext(run);
        });
    };
    if (!api) {
        run->summary.nodes[index].error =
            QStringLiteral("Подключение закрыто до выдачи на этом сервере.");
        advance();
        return;
    }
    const quint64 generation = ++run->generation;
    run->pendingDestroyed =
        connect(api, &QObject::destroyed, this, [this, run, index, generation, advance] {
            if (run->finished || run_ != run || run->generation != generation)
                return;
            ++run->generation;
            QObject::disconnect(run->pendingDestroyed);
            run->pendingDestroyed = {};
            run->summary.nodes[index].error =
                QStringLiteral("Подключение закрыто во время выдачи; результат операции "
                               "неизвестен. Обновите список.");
            advance();
        });
    ClientDraft nodeDraft = run->draft;
    nodeDraft.inboundIds = target.inboundIds;
    const ServerConfig config = api->config();
    api->createClient(
        nodeDraft, [self, run, index, generation, advance, config](OperationResult result) {
            if (!self || run->finished || self->run_ != run || run->generation != generation)
                return;
            QObject::disconnect(run->pendingDestroyed);
            run->pendingDestroyed = {};
            ++run->generation;
            auto& node = run->summary.nodes[index];
            node.ok = result.ok && result.value;
            if (!node.ok)
                node.error = result.error.isEmpty()
                                 ? QStringLiteral("Панель не подтвердила выдачу клиента.")
                                 : safeText(result.error, config, run->draft);
            advance();
        });
}
} // namespace fleet
