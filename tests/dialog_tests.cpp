#include "editor_dialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>
#include <QTreeWidget>
#include <limits>

using namespace fleet;

namespace {
constexpr qint64 gb = 1024LL * 1024LL * 1024LL;

ServerConfig serverConfig() {
    ServerConfig server;
    server.id = QStringLiteral("server-one");
    server.name = QStringLiteral("Test server");
    server.panelUrl = QUrl(QStringLiteral("https://example.invalid:2053/hidden/base/"));
    server.auth = AuthKind::Token;
    server.username = QStringLiteral("test-user");
    server.password = QStringLiteral("saved-test-password");
    server.token = QStringLiteral("saved-test-token");
    server.twoFactorCode = QStringLiteral("stale-test-code");
    return server;
}

QHash<QString, Inventory> inventories() {
    Inventory inventory;
    inventory.inbounds = {{QStringLiteral("server-one"), 1, QStringLiteral("VLESS"),
                           QStringLiteral("vless"), 443, true},
                          {QStringLiteral("server-one"), 2, QStringLiteral("VMess"),
                           QStringLiteral("vmess"), 8443, true},
                          {QStringLiteral("server-one"), 3, QStringLiteral("Trojan"),
                           QStringLiteral("trojan"), 9443, true},
                          {QStringLiteral("server-one"), 4, QStringLiteral("Disabled"),
                           QStringLiteral("vless"), 443, false},
                          {QStringLiteral("server-one"), 5, QStringLiteral("Unsupported"),
                           QStringLiteral("shadowsocks"), 443, true}};
    return {{QStringLiteral("server-one"), inventory}};
}

QList<ServerConfig> multipleServers() {
    auto second = serverConfig();
    second.id = QStringLiteral("server-two");
    second.name = QStringLiteral("Second server");
    auto missing = serverConfig();
    missing.id = QStringLiteral("server-three");
    missing.name = QStringLiteral("Unloaded server");
    return {serverConfig(), second, missing};
}

QHash<QString, Inventory> multipleInventories() {
    auto result = inventories();
    Inventory second;
    second.inbounds = {{QStringLiteral("server-two"), 11, QStringLiteral("Second VLESS"),
                        QStringLiteral("vless"), 443, true},
                       {QStringLiteral("server-two"), 12, QStringLiteral("Another VLESS"),
                        QStringLiteral("vless"), 8443, true},
                       {QStringLiteral("server-two"), 13, QStringLiteral("Disabled VMess"),
                        QStringLiteral("vmess"), 9443, false}};
    result.insert(QStringLiteral("server-two"), second);
    return result;
}

Client client(qint64 totalBytes, qint64 expiryTime) {
    Client result;
    result.serverId = QStringLiteral("server-one");
    result.inboundIds = {1};
    result.id = QStringLiteral("11111111-2222-4333-8444-555555555555");
    result.email = QStringLiteral("test-phone");
    result.protocol = QStringLiteral("vless");
    result.totalBytes = totalBytes;
    result.expiryTime = expiryTime;
    result.enable = true;
    return result;
}

template <class T> T* field(QObject& dialog, const char* name) {
    auto* result = dialog.findChild<T*>(QString::fromLatin1(name));
    if (!result)
        qFatal("Missing dialog control: %s", name);
    return result;
}

QTreeWidgetItem* serverNode(ClientDialog& dialog, const QString& serverId) {
    auto* tree = field<QTreeWidget>(dialog, "targetTree");
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* item = tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == serverId)
            return item;
    }
    qFatal("Missing target server: %s", qPrintable(serverId));
    return nullptr;
}

QTreeWidgetItem* inboundNode(ClientDialog& dialog, const QString& serverId, int inboundId) {
    auto* server = serverNode(dialog, serverId);
    for (int i = 0; i < server->childCount(); ++i) {
        auto* item = server->child(i);
        if (item->data(0, Qt::UserRole + 1).toInt() == inboundId)
            return item;
    }
    qFatal("Missing target inbound: %d", inboundId);
    return nullptr;
}

void selectTarget(ClientDialog& dialog, const QString& serverId, int inboundId,
                  bool selected = true) {
    inboundNode(dialog, serverId, inboundId)
        ->setCheckState(0, selected ? Qt::Checked : Qt::Unchecked);
}

QPushButton* saveButton(QDialog& dialog) {
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    if (!buttons || !buttons->button(QDialogButtonBox::Ok))
        qFatal("Missing dialog save button");
    return buttons->button(QDialogButtonBox::Ok);
}

void show(QDialog& dialog) {
    dialog.show();
    QApplication::processEvents();
}

void fillNewServer(ServerDialog& dialog) {
    field<QLineEdit>(dialog, "serverName")->setText(QStringLiteral("New test server"));
    field<QLineEdit>(dialog, "panelUrl")
        ->setText(QStringLiteral("https://example.invalid/hidden/base/"));
    field<QLineEdit>(dialog, "token")->setText(QStringLiteral("new-test-token"));
}
} // namespace

class DialogTests : public QObject {
    Q_OBJECT
  private slots:
    void unchangedClientPreservesOriginalValues_data() {
        QTest::addColumn<qint64>("totalBytes");
        QTest::addColumn<qint64>("expiryTime");
        QTest::newRow("unlimited-with-precise-quota") << 3 * gb + 12345 << qint64(0);
        QTest::newRow("relative-expiry") << gb + 999999 << qint64(-30LL * 86400000LL);
        QTest::newRow("absolute-with-milliseconds") << 2 * gb + 7 << qint64(1900000000123LL);
        QTest::newRow("quota-above-editor-range") << 1000000LL * gb + 5123 << qint64(0);
        QTest::newRow("integer-extremes")
            << std::numeric_limits<qint64>::max() << std::numeric_limits<qint64>::min();
    }

    void unchangedClientPreservesOriginalValues() {
        QFETCH(qint64, totalBytes);
        QFETCH(qint64, expiryTime);
        const Client original = client(totalBytes, expiryTime);
        ClientDialog dialog({serverConfig()}, inventories(), original);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QVERIFY(!dialog.patch().totalBytes.has_value());
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
        QCOMPARE(dialog.draft().totalBytes, original.totalBytes);
        QCOMPARE(dialog.draft().expiryTime, original.expiryTime);
        QCOMPARE(dialog.draft().email, original.email);
        QCOMPARE(dialog.selectedServerId(), original.serverId);
    }

    void enabledChangeDoesNotTouchQuotaOrRelativeExpiry() {
        const Client original = client(21 * gb + 777, -7LL * 86400000LL);
        ClientDialog dialog({serverConfig()}, inventories(), original);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        QVERIFY(!field<QComboBox>(dialog, "server")->isEnabled());
        QVERIFY(!field<QComboBox>(dialog, "inbound")->isEnabled());
        QVERIFY(field<QLineEdit>(dialog, "email")->isReadOnly());
        QVERIFY(!field<QCheckBox>(dialog, "unlimitedExpiry")->isEnabled());
        QVERIFY(!field<QDateTimeEdit>(dialog, "expiryTime")->isEnabled());
        field<QCheckBox>(dialog, "enabled")->click();
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QVERIFY(!dialog.patch().totalBytes.has_value());
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QVERIFY(dialog.patch().enable.has_value());
        QCOMPARE(*dialog.patch().enable, false);
        QCOMPARE(dialog.draft().totalBytes, original.totalBytes);
        QCOMPARE(dialog.draft().expiryTime, original.expiryTime);
    }

    void quotaChangeOnlyPatchesQuota() {
        const Client original = client(gb + 123, -86400000LL);
        ClientDialog dialog({serverConfig()}, inventories(), original);
        show(dialog);
        field<QDoubleSpinBox>(dialog, "quotaGB")->setValue(12.34);
        saveButton(dialog)->click();
        QVERIFY(dialog.patch().totalBytes.has_value());
        QCOMPARE(*dialog.patch().totalBytes, qint64(13249974108LL));
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
        QCOMPARE(dialog.draft().expiryTime, original.expiryTime);
    }

    void explicitlyRetypingDisplayedQuotaChangesPreciseOriginal() {
        ClientDialog dialog({serverConfig()}, inventories(), client(gb + 123, 0));
        show(dialog);
        auto* quota = field<QDoubleSpinBox>(dialog, "quotaGB");
        auto* input = quota->findChild<QLineEdit*>();
        QVERIFY(input);
        input->setFocus();
        input->selectAll();
        QTest::keyClicks(input, "1.00");
        saveButton(dialog)->click();
        QVERIFY(dialog.patch().totalBytes.has_value());
        QCOMPARE(*dialog.patch().totalBytes, gb);
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
    }

    void relativeExpiryRequiresExplicitOverride() {
        const Client original = client(2 * gb + 123, -30LL * 86400000LL);
        ClientDialog dialog({serverConfig()}, inventories(), original);
        show(dialog);
        field<QCheckBox>(dialog, "overrideRelativeExpiry")->click();
        QVERIFY(field<QCheckBox>(dialog, "unlimitedExpiry")->isEnabled());
        field<QCheckBox>(dialog, "unlimitedExpiry")->click();
        saveButton(dialog)->click();
        QVERIFY(dialog.patch().expiryTime.has_value());
        QCOMPARE(*dialog.patch().expiryTime, qint64(0));
        QVERIFY(!dialog.patch().totalBytes.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
        QCOMPARE(dialog.draft().totalBytes, original.totalBytes);
    }

    void cancellingRelativeOverridePreservesOriginal() {
        const Client original = client(2 * gb + 123, -30LL * 86400000LL);
        ClientDialog dialog({serverConfig()}, inventories(), original);
        show(dialog);
        auto* override = field<QCheckBox>(dialog, "overrideRelativeExpiry");
        override->click();
        field<QCheckBox>(dialog, "unlimitedExpiry")->click();
        override->click();
        saveButton(dialog)->click();
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QCOMPARE(dialog.draft().expiryTime, original.expiryTime);
    }

    void absoluteExpiryUsesUtcAndOnlyPatchesExpiry() {
        ClientDialog dialog({serverConfig()}, inventories(), client(2 * gb + 123, 0));
        show(dialog);
        auto* expiry = field<QDateTimeEdit>(dialog, "expiryTime");
        QCOMPARE(expiry->timeZone(), QTimeZone(QTimeZone::UTC));
        field<QCheckBox>(dialog, "unlimitedExpiry")->click();
        expiry->setDateTime(QDateTime::fromMSecsSinceEpoch(1900000000000LL, QTimeZone::UTC));
        saveButton(dialog)->click();
        QVERIFY(dialog.patch().expiryTime.has_value());
        QCOMPARE(*dialog.patch().expiryTime, qint64(1900000000000LL));
        QVERIFY(!dialog.patch().totalBytes.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
    }

    void blankServerSecretsPreservedWithoutStaleTwoFactor_data() {
        QTest::addColumn<bool>("passwordAuth");
        QTest::newRow("token") << false;
        QTest::newRow("password") << true;
    }

    void blankServerSecretsPreservedWithoutStaleTwoFactor() {
        QFETCH(bool, passwordAuth);
        auto original = serverConfig();
        original.auth = passwordAuth ? AuthKind::Password : AuthKind::Token;
        ServerDialog dialog(original);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        QCOMPARE(field<QLineEdit>(dialog, "password")->echoMode(), QLineEdit::Password);
        QCOMPARE(field<QLineEdit>(dialog, "token")->echoMode(), QLineEdit::Password);
        QVERIFY(field<QLineEdit>(dialog, "password")->text().isEmpty());
        QVERIFY(field<QLineEdit>(dialog, "token")->text().isEmpty());
        QVERIFY(field<QLineEdit>(dialog, "twoFactorCode")->text().isEmpty());
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.result().password, original.password);
        QCOMPARE(dialog.result().token, original.token);
        QCOMPARE(dialog.result().username, original.username);
        QVERIFY(dialog.result().twoFactorCode.isEmpty());
        QCOMPARE(dialog.result().id, original.id);
        QCOMPARE(dialog.result().panelUrl.path(), QStringLiteral("/hidden/base/"));
    }

    void serverValidationKeepsDialogOpen_data() {
        QTest::addColumn<QString>("fieldName");
        QTest::addColumn<QString>("invalidValue");
        QTest::addColumn<QString>("correctedValue");
        QTest::newRow("name-required")
            << QStringLiteral("serverName") << QString() << QStringLiteral("Test server");
        QTest::newRow("token-required")
            << QStringLiteral("token") << QString() << QStringLiteral("test-token");
        QTest::newRow("http-requires-explicit-choice")
            << QStringLiteral("panelUrl") << QStringLiteral("http://example.invalid/base/")
            << QStringLiteral("https://example.invalid/base/");
        QTest::newRow("panel-userinfo") << QStringLiteral("panelUrl")
                                        << QStringLiteral("https://user:pass@example.invalid/base/")
                                        << QStringLiteral("https://example.invalid/base/");
        QTest::newRow("panel-query") << QStringLiteral("panelUrl")
                                     << QStringLiteral("https://example.invalid/base/?query=yes")
                                     << QStringLiteral("https://example.invalid/base/");
        QTest::newRow("panel-fragment") << QStringLiteral("panelUrl")
                                        << QStringLiteral("https://example.invalid/base/#fragment")
                                        << QStringLiteral("https://example.invalid/base/");
        QTest::newRow("subscription-userinfo")
            << QStringLiteral("subscriptionUrl")
            << QStringLiteral("https://user@example.invalid/sub/")
            << QStringLiteral("https://example.invalid/sub/");
        QTest::newRow("subscription-query")
            << QStringLiteral("subscriptionUrl")
            << QStringLiteral("https://example.invalid/sub/?query=yes")
            << QStringLiteral("https://example.invalid/sub/");
        QTest::newRow("subscription-fragment")
            << QStringLiteral("subscriptionUrl")
            << QStringLiteral("https://example.invalid/sub/#fragment")
            << QStringLiteral("https://example.invalid/sub/");
        QTest::newRow("subscription-scheme")
            << QStringLiteral("subscriptionUrl") << QStringLiteral("ftp://example.invalid/sub/")
            << QStringLiteral("https://example.invalid/sub/");
    }

    void serverValidationKeepsDialogOpen() {
        QFETCH(QString, fieldName);
        QFETCH(QString, invalidValue);
        QFETCH(QString, correctedValue);
        ServerDialog dialog;
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        fillNewServer(dialog);
        auto* input = field<QLineEdit>(dialog, fieldName.toLatin1().constData());
        input->setText(invalidValue);
        show(dialog);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(rejected.count(), 0);
        QVERIFY(dialog.isVisible());
        auto* error = field<QLabel>(dialog, "validationError");
        QVERIFY(error->isVisible());
        QVERIFY(!error->text().isEmpty());
        input->setText(correctedValue);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QVERIFY(!dialog.result().id.isEmpty());
    }

    void clientValidationKeepsDialogOpen_data() {
        QTest::addColumn<QString>("email");
        QTest::newRow("empty") << QString();
        QTest::newRow("slash") << QStringLiteral("bad/name");
        QTest::newRow("query") << QStringLiteral("bad?name");
        QTest::newRow("fragment") << QStringLiteral("bad#name");
        QTest::newRow("backslash") << QStringLiteral("bad\\name");
        QTest::newRow("space") << QStringLiteral("bad name");
        QTest::newRow("unicode-space")
            << (QStringLiteral("bad") + QChar(0x00a0) + QStringLiteral("name"));
        QTest::newRow("control") << (QStringLiteral("bad") + QChar(1) + QStringLiteral("name"));
    }

    void clientValidationKeepsDialogOpen() {
        QFETCH(QString, email);
        ClientDialog dialog({serverConfig()}, inventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        selectTarget(dialog, QStringLiteral("server-one"), 1);
        field<QLineEdit>(dialog, "email")->setText(email);
        show(dialog);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(field<QLabel>(dialog, "validationError")->isVisible());
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("valid-phone"));
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.draft().email, QStringLiteral("valid-phone"));
    }

    void supportedInboundsAndFlowFollowSelection() {
        ClientDialog dialog({serverConfig()}, inventories());
        show(dialog);
        auto* server = serverNode(dialog, QStringLiteral("server-one"));
        auto* flow = field<QComboBox>(dialog, "flow");
        QCOMPARE(server->childCount(), 5);
        auto* disabled = inboundNode(dialog, QStringLiteral("server-one"), 4);
        auto* unsupported = inboundNode(dialog, QStringLiteral("server-one"), 5);
        QVERIFY(!(disabled->flags() & Qt::ItemIsEnabled));
        QVERIFY(!(unsupported->flags() & Qt::ItemIsEnabled));
        QVERIFY(!disabled->text(1).isEmpty());
        QVERIFY(!unsupported->text(1).isEmpty());
        selectTarget(dialog, QStringLiteral("server-one"), 1);
        QVERIFY(flow->isVisible());
        QVERIFY(flow->isEnabled());
        flow->setCurrentIndex(1);
        QCOMPARE(flow->currentData().toString(), QStringLiteral("xtls-rprx-vision"));
        selectTarget(dialog, QStringLiteral("server-one"), 2);
        QVERIFY(!flow->isEnabled());
        QCOMPARE(flow->currentIndex(), 0);
        field<QLineEdit>(dialog, "email")->setText(QString(129, QLatin1Char('x')));
        QCOMPARE(field<QLineEdit>(dialog, "email")->text().size(), qsizetype(128));
        saveButton(dialog)->click();
        QCOMPARE(dialog.draft().inboundIds, QList<int>({1, 2}));
        QVERIFY(dialog.draft().flow.isEmpty());
        QCOMPARE(dialog.draft().email.size(), qsizetype(128));
    }

    void selectAllGroupsTargetsAcrossServers() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        field<QPushButton>(dialog, "selectAllTargets")->click();
        const auto* count = field<QLabel>(dialog, "selectionCount");
        QVERIFY(count->text().contains(QStringLiteral("серверов: 2")));
        QVERIFY(count->text().contains(QStringLiteral("inbound: 5")));
        QCOMPARE(serverNode(dialog, QStringLiteral("server-one"))->checkState(0), Qt::Checked);
        QCOMPARE(serverNode(dialog, QStringLiteral("server-two"))->checkState(0), Qt::Checked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 4)->checkState(0),
                 Qt::Unchecked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 5)->checkState(0),
                 Qt::Unchecked);
        auto* missing = serverNode(dialog, QStringLiteral("server-three"));
        QVERIFY(!(missing->flags() & Qt::ItemIsEnabled));
        QVERIFY(!missing->text(1).isEmpty());
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        const auto targets = dialog.targets();
        QCOMPARE(targets.size(), qsizetype(2));
        QCOMPARE(targets[0].serverId, QStringLiteral("server-one"));
        QCOMPARE(targets[0].inboundIds, QList<int>({1, 2, 3}));
        QCOMPARE(targets[1].serverId, QStringLiteral("server-two"));
        QCOMPARE(targets[1].inboundIds, QList<int>({11, 12}));
        QCOMPARE(dialog.selectedServerId(), targets.first().serverId);
        QCOMPARE(dialog.draft().inboundIds, targets.first().inboundIds);
        QVERIFY(dialog.draft().clientId.isEmpty());
        QVERIFY(dialog.draft().password.isEmpty());
        QVERIFY(dialog.draft().subId.isEmpty());
    }

    void longUnicodeEmailRemainsSupportedAndFooterOutsideScroll() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        auto* scroll = field<QScrollArea>(dialog, "clientFormScroll");
        QVERIFY(scroll->widgetResizable());
        QVERIFY(!scroll->isAncestorOf(saveButton(dialog)));
        selectTarget(dialog, QStringLiteral("server-one"), 1);
        const QString unicodeEmail(128, QChar(0x044f));
        field<QLineEdit>(dialog, "email")->setText(unicodeEmail);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.draft().email, unicodeEmail);
    }

    void deselectionUpdatesParentsAndClearRemovesAllTargets() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        field<QPushButton>(dialog, "selectAllTargets")->click();
        selectTarget(dialog, QStringLiteral("server-one"), 1, false);
        QCOMPARE(serverNode(dialog, QStringLiteral("server-one"))->checkState(0),
                 Qt::PartiallyChecked);
        serverNode(dialog, QStringLiteral("server-two"))->setCheckState(0, Qt::Unchecked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-two"), 11)->checkState(0),
                 Qt::Unchecked);
        QVERIFY(
            field<QLabel>(dialog, "selectionCount")->text().contains(QStringLiteral("inbound: 2")));
        field<QPushButton>(dialog, "clearTargets")->click();
        QCOMPARE(serverNode(dialog, QStringLiteral("server-one"))->checkState(0), Qt::Unchecked);
        QVERIFY(
            field<QLabel>(dialog, "selectionCount")->text().contains(QStringLiteral("inbound: 0")));
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(field<QLabel>(dialog, "validationError")->isVisible());
    }

    void serverCheckboxSelectsOnlySupportedEnabledChildren() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        show(dialog);
        serverNode(dialog, QStringLiteral("server-one"))->setCheckState(0, Qt::Checked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 1)->checkState(0), Qt::Checked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 2)->checkState(0), Qt::Checked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 3)->checkState(0), Qt::Checked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 4)->checkState(0),
                 Qt::Unchecked);
        QCOMPARE(inboundNode(dialog, QStringLiteral("server-one"), 5)->checkState(0),
                 Qt::Unchecked);
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(dialog.targets().size(), qsizetype(1));
        QCOMPARE(dialog.targets().first().inboundIds, QList<int>({1, 2, 3}));
    }

    void unsupportedCheckedTargetCannotBeAccepted() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        selectTarget(dialog, QStringLiteral("server-one"), 1);
        // Programmatic check exercises validation even though this row is disabled in the UI.
        selectTarget(dialog, QStringLiteral("server-one"), 5);
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(field<QLabel>(dialog, "validationError")->isVisible());
        field<QPushButton>(dialog, "clearTargets")->click();
        selectTarget(dialog, QStringLiteral("server-two"), 11);
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.targets().first().serverId, QStringLiteral("server-two"));
        QCOMPARE(dialog.targets().first().inboundIds, QList<int>({11}));
    }

    void visionCanSpanServersButMixedSelectionClearsIt() {
        ClientDialog dialog(multipleServers(), multipleInventories());
        show(dialog);
        selectTarget(dialog, QStringLiteral("server-one"), 1);
        selectTarget(dialog, QStringLiteral("server-two"), 11);
        auto* flow = field<QComboBox>(dialog, "flow");
        QVERIFY(flow->isEnabled());
        flow->setCurrentIndex(1);
        selectTarget(dialog, QStringLiteral("server-one"), 3);
        QVERIFY(!flow->isEnabled());
        QCOMPARE(flow->currentIndex(), 0);
        // A stale flow value must also be ignored at accept time for a mixed set.
        flow->setCurrentIndex(1);
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(dialog.targets().size(), qsizetype(2));
        QVERIFY(dialog.draft().flow.isEmpty());
    }

    void masterDefaultSelection_data() {
        QTest::addColumn<QString>("masterId");
        QTest::addColumn<QString>("expectedServer");
        QTest::addColumn<QList<int>>("expectedInbounds");
        QTest::newRow("first-loaded-master")
            << QStringLiteral("server-one") << QStringLiteral("server-one")
            << QList<int>({1, 2, 3});
        QTest::newRow("second-loaded-master")
            << QStringLiteral("server-two") << QStringLiteral("server-two") << QList<int>({11, 12});
        QTest::newRow("unloaded-master")
            << QStringLiteral("server-three") << QString() << QList<int>();
        QTest::newRow("unknown-master") << QStringLiteral("unknown") << QString() << QList<int>();
        QTest::newRow("no-master") << QString() << QString() << QList<int>();
    }

    void masterDefaultSelection() {
        QFETCH(QString, masterId);
        QFETCH(QString, expectedServer);
        QFETCH(QList<int>, expectedInbounds);
        ClientDialog dialog(multipleServers(), multipleInventories(), {}, nullptr, masterId);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        const auto* shared = field<QLabel>(dialog, "sharedIdentifiers");
        QVERIFY(shared->text().contains(QStringLiteral("UUID")));
        QVERIFY(shared->text().contains(QStringLiteral("subId")));
        QVERIFY(!field<QLabel>(dialog, "masterSubscriptionHelp")->text().isEmpty());
        saveButton(dialog)->click();
        if (expectedInbounds.isEmpty()) {
            QCOMPARE(accepted.count(), 0);
            QVERIFY(dialog.isVisible());
        } else {
            QCOMPARE(accepted.count(), 1);
            QCOMPARE(dialog.targets().size(), qsizetype(1));
            QCOMPARE(dialog.targets().first().serverId, expectedServer);
            QCOMPARE(dialog.targets().first().inboundIds, expectedInbounds);
        }
    }

    void masterCanBeDeselected() {
        ClientDialog dialog(multipleServers(), multipleInventories(), {}, nullptr,
                            QStringLiteral("server-one"));
        show(dialog);
        field<QPushButton>(dialog, "clearTargets")->click();
        selectTarget(dialog, QStringLiteral("server-two"), 11);
        auto* flow = field<QComboBox>(dialog, "flow");
        QVERIFY(flow->isEnabled());
        flow->setCurrentIndex(1);
        field<QLineEdit>(dialog, "email")->setText(QStringLiteral("common-phone"));
        saveButton(dialog)->click();
        QCOMPARE(dialog.targets().size(), qsizetype(1));
        QCOMPARE(dialog.targets().first().serverId, QStringLiteral("server-two"));
        QCOMPARE(dialog.draft().flow, QStringLiteral("xtls-rprx-vision"));
    }

    void editPreservesAllBindingsWithDifferentMaster() {
        auto original = client(17 * gb + 123, -30LL * 86400000LL);
        original.inboundIds = {1, 2, 99};
        ClientDialog dialog(multipleServers(), multipleInventories(), original, nullptr,
                            QStringLiteral("server-two"));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        QVERIFY(!dialog.findChild<QTreeWidget*>(QStringLiteral("targetTree")));
        QVERIFY(!field<QComboBox>(dialog, "server")->isEnabled());
        QVERIFY(!field<QComboBox>(dialog, "inbound")->isEnabled());
        QVERIFY(field<QLineEdit>(dialog, "email")->isReadOnly());
        saveButton(dialog)->click();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.targets().size(), qsizetype(1));
        QCOMPARE(dialog.targets().first().serverId, original.serverId);
        QCOMPARE(dialog.targets().first().inboundIds, original.inboundIds);
        QCOMPARE(dialog.draft().inboundIds, original.inboundIds);
        QCOMPARE(dialog.draft().totalBytes, original.totalBytes);
        QCOMPARE(dialog.draft().expiryTime, original.expiryTime);
        QVERIFY(!dialog.patch().totalBytes.has_value());
        QVERIFY(!dialog.patch().expiryTime.has_value());
        QVERIFY(!dialog.patch().enable.has_value());
    }
};

int main(int argc, char* argv[]) {
    QLocale::setDefault(QLocale::c());
    QApplication application(argc, argv);
    DialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "dialog_tests.moc"
