#include "main_window.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
using namespace fleet;
namespace {
template <class T> T* control(MainWindow& window, const char* name) {
    auto* result = window.findChild<T*>(QString::fromLatin1(name));
    if (!result)
        qFatal("Missing control: %s", name);
    return result;
}
void show(MainWindow& window, int page) {
    window.showPage(page);
    window.show();
    QApplication::processEvents();
}
} // namespace
class WindowTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QApplication::setStyle("Fusion");
        qApp->setStyleSheet(appStyle());
    }
    void subscriptionsGroupCopiesAndKeepAllServersInDetails() {
        QTemporaryDir directory;
        MainWindow window(directory.path(), true);
        show(window, 2);
        auto* table = control<QTableWidget>(window, "subscriptionsTable");
        QCOMPARE(table->rowCount(), 10);
        QVERIFY(control<QLabel>(window, "subscriptionCount")->text().contains("60 записей"));
        table->selectRow(0);
        auto* nodes = control<QTableWidget>(window, "subscriptionNodesTable");
        QCOMPARE(nodes->rowCount(), 6);
        QCOMPARE(nodes->columnCount(), 6);
        QVERIFY(nodes->item(0, 5)->text().contains("VLESS Reality"));
        QVERIFY(control<QLineEdit>(window, "subscriptionLink")->text().contains("demo-sub-"));
        QVERIFY(control<QPushButton>(window, "viewClientInbounds"));
        auto* target = control<QComboBox>(window, "clientActionServer");
        QCOMPARE(target->currentData().toString(), QString("demo-0"));
        auto* server = control<QComboBox>(window, "subscriptionServerFilter");
        server->setCurrentIndex(server->findData("demo-3"));
        QCOMPARE(table->rowCount(), 10);
        QCOMPARE(nodes->rowCount(), 6);
        target->setCurrentIndex(target->findData("demo-3"));
        control<QComboBox>(window, "subscriptionSort")->setCurrentIndex(1);
        QCOMPARE(target->currentData().toString(), QString("demo-3"));
    }
    void combinedFiltersAndClearWorkOnUniqueRows() {
        QTemporaryDir directory;
        MainWindow window(directory.path(), true);
        show(window, 2);
        auto* table = control<QTableWidget>(window, "subscriptionsTable");
        auto* search = control<QLineEdit>(window, "subscriptionSearch");
        search->setText("client-2");
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("client-2"));
        auto* protocol = control<QComboBox>(window, "subscriptionProtocolFilter");
        protocol->setCurrentIndex(protocol->findData("hysteria"));
        QCOMPARE(table->rowCount(), 1);
        control<QComboBox>(window, "subscriptionExpiryFilter")->setCurrentIndex(2);
        QCOMPARE(table->rowCount(), 1);
        auto* state = control<QComboBox>(window, "subscriptionStateFilter");
        state->setCurrentIndex(state->findData(int(SubscriptionState::Disabled)));
        QCOMPARE(table->rowCount(), 0);
        control<QPushButton>(window, "clearSubscriptionFilters")->click();
        QCOMPARE(table->rowCount(), 10);
        QCOMPARE(search->text(), QString());
        state->setCurrentIndex(state->findData(int(SubscriptionState::Mixed)));
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("client-8"));
    }
    void copyDiscoversAndRefreshesAddressFromMasterPanel() {
        QTcpServer panel;
        QVERIFY(panel.listen(QHostAddress::LocalHost, 0));
        QString address = "https://proxy.example.invalid/initial/sub/";
        int settingsRequests = 0;
        connect(&panel, &QTcpServer::newConnection, this, [&] {
            while (panel.hasPendingConnections()) {
                auto* socket = panel.nextPendingConnection();
                auto data = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, data, handled] {
                    data->append(socket->readAll());
                    const int end = data->indexOf("\r\n\r\n");
                    if (*handled || end < 0)
                        return;
                    int length = 0;
                    for (const auto& line : data->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    if (data->size() < end + 4 + length)
                        return;
                    *handled = true;
                    const auto target = data->split('\n').first().split(' ').value(1);
                    QJsonValue result = QJsonObject{};
                    if (target.endsWith("/setting/defaultSettings")) {
                        ++settingsRequests;
                        result = QJsonObject{{"subEnable", true}, {"subURI", address}};
                    } else if (target.endsWith("/inbounds/list")) {
                        result = QJsonArray{QJsonObject{{"id", 1},
                                                        {"remark", "Synthetic Reality"},
                                                        {"protocol", "vless"},
                                                        {"port", 443},
                                                        {"enable", true}}};
                    } else if (target.endsWith("/clients/list")) {
                        result =
                            QJsonArray{QJsonObject{{"id", 42},
                                                   {"uuid", "11111111-2222-4333-8444-555555555555"},
                                                   {"email", "synthetic-client"},
                                                   {"subId", "synthetic-sub"},
                                                   {"inboundIds", QJsonArray{1}},
                                                   {"enable", true},
                                                   {"totalGB", 0},
                                                   {"expiryTime", 0}}};
                    } else if (target.endsWith("/onlines"))
                        result = QJsonArray{};
                    const auto body = QJsonDocument(QJsonObject{{"success", true}, {"obj", result}})
                                          .toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                  "application/json\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        QTemporaryDir directory;
        {
            LocalStore store;
            QVERIFY(store.open(directory.filePath("fleet.db")).ok);
            ServerConfig config;
            config.id = "master";
            config.name = "Synthetic master";
            config.panelUrl = QUrl(QString("http://127.0.0.1:%1/base/").arg(panel.serverPort()));
            config.allowHttp = true;
            config.api = ApiFlavor::ModernV3;
            config.auth = AuthKind::Token;
            config.token = "synthetic-token";
            QVERIFY(store.saveServer(config).ok);
        }
        {
            QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
            settings.setValue("masterServerId", "master");
        }
        MainWindow window(directory.path());
        show(window, 2);
        auto* link = control<QLineEdit>(window, "subscriptionLink");
        QTRY_COMPARE(link->text(), address + "synthetic-sub");
        const int previous = settingsRequests;
        address = "https://proxy.example.invalid/changed/sub/";
        control<QPushButton>(window, "copySubscriptionLink")->click();
        QTRY_COMPARE(QApplication::clipboard()->text(), address + "synthetic-sub");
        QCOMPARE(link->text(), address + "synthetic-sub");
        QCOMPARE(settingsRequests, previous + 1);
    }
    void serverFiltersCombineLocationHealthAndSearch() {
        QTemporaryDir directory;
        MainWindow window(directory.path(), true);
        show(window, 1);
        auto* table = control<QTableWidget>(window, "serversTable");
        QCOMPARE(table->rowCount(), 6);
        auto* location = control<QComboBox>(window, "serverLocationFilter");
        location->setCurrentIndex(location->findData("Нидерланды"));
        QCOMPARE(table->rowCount(), 1);
        auto* health = control<QComboBox>(window, "serverHealthFilter");
        health->setCurrentIndex(health->findData(int(Health::Offline)));
        QCOMPARE(table->rowCount(), 0);
        health->setCurrentIndex(0);
        location->setCurrentIndex(0);
        control<QLineEdit>(window, "serverSearch")->setText("Helsinki");
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(table->item(0, 7));
        QVERIFY(table->item(0, 8));
        QVERIFY(table->item(0, 9));
    }
    void splittersResizeAndPersistOutsideDemo() {
        QTemporaryDir directory;
        QByteArray saved;
        {
            MainWindow window(directory.path());
            show(window, 0);
            auto* splitter = control<QSplitter>(window, "overviewSections");
            QVERIFY(!splitter->childrenCollapsible());
            QCOMPARE(splitter->count(), 2);
            splitter->setSizes({190, 500});
            QApplication::processEvents();
            auto sizes = splitter->sizes();
            QVERIFY(sizes[1] > sizes[0]);
            splitter->setSizes({420, 270});
            QApplication::processEvents();
            QVERIFY(splitter->sizes()[0] > sizes[0]);
            saved = splitter->saveState();
            auto* tabs = control<QTabWidget>(window, "statisticsTabs");
            QCOMPARE(tabs->count(), 2);
            tabs->setCurrentIndex(1);
        }
        QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
        QCOMPARE(settings.value("layout/overviewSections").toByteArray(), saved);
        MainWindow restored(directory.path());
        show(restored, 0);
        QCOMPARE(control<QSplitter>(restored, "overviewSections")->saveState(), saved);
    }
};
QTEST_MAIN(WindowTests)
#include "window_tests.moc"
