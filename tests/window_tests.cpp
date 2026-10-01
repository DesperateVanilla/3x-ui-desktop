#include "main_window.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
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
