#pragma once
#include "domain.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <memory>

namespace fleet {
class ThreeXUiApi;
class ClientProvisioner : public QObject {
    Q_OBJECT
  public:
    explicit ClientProvisioner(const QHash<QString, ThreeXUiApi*>& apis, QObject* parent = nullptr);
    void provision(const QList<ClientTarget>& targets, const ClientDraft& draft,
                   const QString& masterServerId, Callback<ProvisionSummary> callback);

  private:
    struct Run;
    void preflightNext(const std::shared_ptr<Run>& run);
    void createNext(const std::shared_ptr<Run>& run);
    void finish(const std::shared_ptr<Run>& run, Outcome<ProvisionSummary> result);
    QHash<QString, QPointer<ThreeXUiApi>> apis_;
    bool busy_ = false;
    std::shared_ptr<Run> run_;
};
} // namespace fleet
