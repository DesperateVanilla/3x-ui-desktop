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
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>
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
        QTest::newRow("control") << (QStringLiteral("bad") + QChar(1) + QStringLiteral("name"));
    }

    void clientValidationKeepsDialogOpen() {
        QFETCH(QString, email);
        ClientDialog dialog({serverConfig()}, inventories());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
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
        auto* inbound = field<QComboBox>(dialog, "inbound");
        auto* flow = field<QComboBox>(dialog, "flow");
        QCOMPARE(inbound->count(), 3);
        QVERIFY(inbound->findData(4) < 0);
        QVERIFY(inbound->findData(5) < 0);
        QVERIFY(flow->isVisible());
        flow->setCurrentIndex(1);
        QCOMPARE(flow->currentData().toString(), QStringLiteral("xtls-rprx-vision"));
        inbound->setCurrentIndex(inbound->findData(2));
        QVERIFY(!flow->isVisible());
        QCOMPARE(flow->currentIndex(), 0);
        field<QLineEdit>(dialog, "email")->setText(QString(129, QLatin1Char('x')));
        QCOMPARE(field<QLineEdit>(dialog, "email")->text().size(), qsizetype(128));
        saveButton(dialog)->click();
        QCOMPARE(dialog.draft().inboundId, 2);
        QVERIFY(dialog.draft().flow.isEmpty());
        QCOMPARE(dialog.draft().email.size(), qsizetype(128));
    }
};

int main(int argc, char* argv[]) {
    QLocale::setDefault(QLocale::c());
    QApplication application(argc, argv);
    DialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "dialog_tests.moc"
