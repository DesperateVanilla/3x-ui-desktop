#pragma once
#include "domain.h"
#include <QDialog>
#include <QHash>

namespace fleet {
class ServerDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ServerDialog(const std::optional<ServerConfig>& existing = {},
                          QWidget* parent = nullptr);
    ServerConfig result() const;

  private:
    ServerConfig result_;
};

class ClientDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ClientDialog(const QList<ServerConfig>& servers,
                          const QHash<QString, Inventory>& inventories,
                          const std::optional<Client>& existing = {}, QWidget* parent = nullptr);
    QString selectedServerId() const;
    ClientDraft draft() const;
    ClientPatch patch() const;

  private:
    QString serverId_;
    ClientDraft draft_;
    ClientPatch patch_;
};
} // namespace fleet
