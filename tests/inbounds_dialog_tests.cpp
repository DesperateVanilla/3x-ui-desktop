#include "client_inbounds_dialog.h"
#include <QPushButton>
#include <QTest>
#include <QTreeWidget>
using namespace fleet;
namespace {
Inventory inventory() {
    Inventory result;
    result.inbounds = {{"server", 1, "Reality", "vless", 443, true},
                       {"server", 2, "New Hysteria", "hysteria", 8443, true},
                       {"server", 3, "Disabled", "vless", 9443, false},
                       {"server", 4, "HTTP", "http", 8080, true},
                       {"server", 5, "Trojan", "trojan", 443, true}};
    return result;
}
Client client() {
    Client value;
    value.serverId = "server";
    value.email = "synthetic-client";
    value.inboundIds = {1, 9};
    value.raw = {{"flow", "xtls-rprx-vision"}};
    return value;
}
ServerConfig server() {
    ServerConfig value;
    value.id = "server";
    value.name = "Synthetic panel";
    return value;
}
QTreeWidgetItem* item(ClientInboundsDialog& dialog, int id) {
    auto* tree = dialog.findChild<QTreeWidget*>("clientInboundsTree");
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toInt() == id)
            return tree->topLevelItem(i);
    qFatal("Missing inbound");
    return nullptr;
}
} // namespace
class InboundsTests : public QObject {
    Q_OBJECT
  private slots:
    void existingAndMissingBindingsAreVisibleAndLocked() {
        ClientInboundsDialog dialog(server(), inventory(), client(), true);
        QCOMPARE(item(dialog, 1)->checkState(0), Qt::Checked);
        QVERIFY(!(item(dialog, 1)->flags() & Qt::ItemIsUserCheckable));
        QCOMPARE(item(dialog, 9)->checkState(0), Qt::Checked);
        QVERIFY(item(dialog, 9)->text(1).contains("отсутствует"));
        item(dialog, 1)->setCheckState(0, Qt::Unchecked);
        QCOMPARE(item(dialog, 1)->checkState(0), Qt::Checked);
        QVERIFY(dialog.addedInboundIds().isEmpty());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    void selectAndClearPreserveExistingBindings() {
        ClientInboundsDialog dialog(server(), inventory(), client(), true);
        dialog.findChild<QPushButton*>("selectAvailableInbounds")->click();
        QCOMPARE(dialog.addedInboundIds(), QList<int>({2, 5}));
        QVERIFY(dialog.findChild<QPushButton*>("attachInboundsButton")->isEnabled());
        QCOMPARE(item(dialog, 3)->checkState(0), Qt::Unchecked);
        QCOMPARE(item(dialog, 4)->checkState(0), Qt::Unchecked);
        dialog.findChild<QPushButton*>("clearNewInbounds")->click();
        QVERIFY(dialog.addedInboundIds().isEmpty());
        QCOMPARE(item(dialog, 1)->checkState(0), Qt::Checked);
        QCOMPARE(item(dialog, 9)->checkState(0), Qt::Checked);
        QVERIFY(!dialog.findChild<QPushButton*>("attachInboundsButton")->isEnabled());
    }
    void onlyEligibleAdditionsCanBeAccepted() {
        ClientInboundsDialog dialog(server(), inventory(), client(), true);
        item(dialog, 3)->setCheckState(0, Qt::Checked);
        item(dialog, 4)->setCheckState(0, Qt::Checked);
        QVERIFY(dialog.addedInboundIds().isEmpty());
        item(dialog, 2)->setCheckState(0, Qt::Checked);
        QCOMPARE(dialog.addedInboundIds(), QList<int>({2}));
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void legacyAndDemoViewCannotAttach() {
        ClientInboundsDialog dialog(server(), inventory(), client(), false);
        QVERIFY(!dialog.findChild<QPushButton*>("selectAvailableInbounds")->isEnabled());
        item(dialog, 2)->setCheckState(0, Qt::Checked);
        QVERIFY(dialog.addedInboundIds().isEmpty());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
};
QTEST_MAIN(InboundsTests)
#include "inbounds_dialog_tests.moc"
