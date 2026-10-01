#pragma once
#include "domain.h"
#include <QDialog>
class QTreeWidget;
class QLabel;
class QPushButton;
namespace fleet {
class ClientInboundsDialog : public QDialog {
    Q_OBJECT
  public:
    ClientInboundsDialog(const ServerConfig& server, const Inventory& inventory,
                         const Client& client, bool canAttach, QWidget* parent = nullptr);
    QList<int> addedInboundIds() const;
  public slots:
    void accept() override;

  private:
    bool canAttach_;
    int attachedCount_;
    QTreeWidget* tree_;
    QLabel* count_;
    QPushButton* attach_;
    void refreshSelection();
};
} // namespace fleet
