#pragma once
#include "domain.h"
#include "local_store.h"
#include "subscription_model.h"
#include <QHash>
#include <QMainWindow>
#include <QQueue>
#include <QTimer>

class QLabel;
class QPushButton;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QStackedWidget;
class QVBoxLayout;
class QSpinBox;
class QFrame;
class QSplitter;

namespace fleet {
class ThreeXUiApi;
class ChartWidget;
class ClientProvisioner;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QString dataDirectory, bool demo = false);
    ~MainWindow() override;
    void showPage(int page) { navigate(qBound(0, page, 4)); }

  private:
    QString dataDirectory_;
    LocalStore store_;
    QList<ServerConfig> servers_;
    QHash<QString, ThreeXUiApi*> apis_;
    QHash<QString, Snapshot> latest_;
    QList<Snapshot> history_;
    QList<ActivityEvent> events_;
    QHash<QString, Inventory> inventories_;
    QHash<QString, QString> inventoryErrors_;
    QList<SubscriptionGroup> subscriptionGroups_;
    QList<SubscriptionGroup> visibleSubscriptions_;
    QStringList visibleServers_;
    QQueue<QString> pollQueue_;
    QQueue<QString> inventoryQueue_;
    QTimer pollTimer_;
    QDate lastPruned_;
    int activePolls_ = 0;
    int inventoryPending_ = 0;
    int activeInventories_ = 0;
    int pollSeconds_ = 30;
    int rangeHours_ = 8;
    QString masterServerId_;
    bool hasMasterPreference_ = false;
    bool demo_ = false;
    bool polling_ = false;
    bool mutating_ = false;
    bool storageReady_ = false;
    bool demoSaved_ = false;
    QList<ServerConfig> realServers_;
    QHash<QString, Inventory> realInventories_;
    QHash<QString, QString> realInventoryErrors_;
    QStackedWidget* pages_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* networkState_ = nullptr;
    QLabel* lastCheck_ = nullptr;
    QLabel* notice_ = nullptr;
    QLabel* clientStatus_ = nullptr;
    QFrame* demoBanner_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* demoButton_ = nullptr;
    QComboBox* rangeCombo_ = nullptr;
    QComboBox* serverFilter_ = nullptr;
    QComboBox* clientStateFilter_ = nullptr;
    QComboBox* clientProtocolFilter_ = nullptr;
    QComboBox* clientExpiryFilter_ = nullptr;
    QComboBox* clientSort_ = nullptr;
    QComboBox* clientActionServer_ = nullptr;
    QComboBox* serverHealthFilter_ = nullptr;
    QComboBox* serverLocationFilter_ = nullptr;
    QComboBox* serverSort_ = nullptr;
    QComboBox* overviewServerFilter_ = nullptr;
    QComboBox* masterCombo_ = nullptr;
    QLabel* masterStatus_ = nullptr;
    QLineEdit* serverSearch_ = nullptr;
    QLineEdit* clientSearch_ = nullptr;
    QTableWidget* serverTable_ = nullptr;
    QTableWidget* clientTable_ = nullptr;
    QTableWidget* clientNodesTable_ = nullptr;
    QTableWidget* eventTable_ = nullptr;
    QVBoxLayout* healthRows_ = nullptr;
    QList<QLabel*> metricValues_;
    QList<QLabel*> metricDetails_;
    QList<QLabel*> subscriptionValues_;
    QList<QSplitter*> splitters_;
    QLabel* resourceStats_ = nullptr;
    QLabel* serverStatus_ = nullptr;
    QLabel* clientDetail_ = nullptr;
    QLabel* onlineChartScope_ = nullptr;
    QList<QPushButton*> navigation_;
    QList<QPushButton*> writeButtons_;
    ChartWidget* onlineChart_ = nullptr;
    ChartWidget* trafficChart_ = nullptr;
    ChartWidget* resourceChart_ = nullptr;
    ChartWidget* latencyChart_ = nullptr;
    QSpinBox* intervalBox_ = nullptr;
    ClientProvisioner* provisioner_ = nullptr;
    void buildUi();
    QWidget* overviewPage();
    QWidget* serversPage();
    QWidget* clientsPage();
    QWidget* eventsPage();
    QWidget* settingsPage();
    void navigate(int page);
    void reloadData();
    void refreshUi();
    void refreshServersTable();
    void refreshClientsTable();
    void refreshClientDetails();
    void configureSplitter(QSplitter* splitter, const QString& name, const QList<int>& sizes);
    void refreshEventsTable();
    void refreshCharts();
    void refreshHealth();
    void setBusyUi();
    void poll();
    void pollNext();
    void fetchInventories();
    void loadInventories(const QString& serverId = {});
    void inventoryNext();
    void addServer();
    void editServer();
    void removeServer();
    void editClient(bool create);
    void toggleClient();
    void renewClient();
    void deleteClient();
    void resetClient();
    void backupServer();
    void restoreServer();
    void refreshMasterSelector();
    QString activeMasterServerId() const;
    void copySubscriptionLink();
    void runServerAction(ServerAction action);
    void completeMutation(const QString& serverId, const QString& action, OperationResult result);
    void recordEvent(const QString& serverId, const QString& action, bool success,
                     const QString& detail);
    void report(const QString& text, bool error = false);
    bool writable(bool allowPolling = false);
    ServerConfig* selectedServer();
    std::optional<Client> selectedClient();
    QString serverName(const QString& id) const;
    void setDemo(bool enabled);
    void generateDemo();
};
QString appStyle();
} // namespace fleet
