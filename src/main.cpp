#include "client_inbounds_dialog.h"
#include "editor_dialogs.h"
#include "main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QPainter>
#include <QPalette>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <memory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("3X Control");
    app.setOrganizationName("3XControl");
    app.setApplicationVersion("0.4.0");
    app.setStyle("Fusion");
    app.setFont(QFont("Segoe UI", 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#0d121a"));
    palette.setColor(QPalette::WindowText, QColor("#e4edf8"));
    palette.setColor(QPalette::Base, QColor("#111923"));
    palette.setColor(QPalette::AlternateBase, QColor("#161e29"));
    palette.setColor(QPalette::Text, QColor("#e4edf8"));
    palette.setColor(QPalette::Button, QColor("#1c2735"));
    palette.setColor(QPalette::ButtonText, QColor("#e4edf8"));
    palette.setColor(QPalette::Highlight, QColor("#24466c"));
    palette.setColor(QPalette::HighlightedText, QColor("#f0f6ff"));
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        palette.setColor(QPalette::Disabled, role, QColor("#67758a"));
    app.setPalette(palette);
    app.setStyleSheet(fleet::appStyle());
#ifdef Q_OS_WIN
    if (QSslSocket::availableBackends().contains("schannel"))
        QSslSocket::setActiveBackend("schannel");
#endif
    QPixmap icon(64, 64);
    icon.fill(Qt::transparent);
    {
        QPainter p(&icon);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor("#4c9dff"));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(2, 2, 60, 60, 15, 15);
        p.setPen(QColor("#0d1520"));
        p.setFont(QFont("Segoe UI", 20, QFont::Bold));
        p.drawText(icon.rect(), Qt::AlignCenter, "3X");
    }
    app.setWindowIcon(QIcon(icon));
    QCommandLineParser parser;
    parser.setApplicationDescription("Нативная панель управления серверами 3x-ui");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"demo", "Показать демонстрационные данные без подключения к серверам."});
    parser.addOption({"data-dir", "Каталог локальной базы и настроек.", "directory"});
    parser.addOption(
        {"capture", "Сохранить изображение интерфейса и завершить приложение.", "png"});
    parser.addOption({"smoke", "Запустить интерфейс и автоматически завершить приложение."});
    parser.addOption({"page",
                      "Показать раздел: 0 обзор, 1 серверы, 2 подписки, 3 журнал, 4 настройки.",
                      "number", "0"});
    parser.addOption(
        {"statistics-tab", "Вкладка статистики: 0 сеть, 1 CPU/RAM и отклик.", "number", "0"});
    parser.addOption({"size", "Размер окна для проверки интерфейса, например 1020x720.", "pixels"});
    parser.addOption(
        {"capture-dialog",
         "При --capture показать форму server, client или inbounds на примерах данных.", "kind"});
    parser.process(app);
    QString dataDir = parser.value("data-dir");
    if (dataDir.isEmpty())
        dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    fleet::MainWindow window(dataDir, parser.isSet("demo"));
    if (parser.isSet("size")) {
        const auto dimensions = parser.value("size").split('x');
        bool widthOk = false, heightOk = false;
        const int width = dimensions.value(0).toInt(&widthOk);
        const int height = dimensions.value(1).toInt(&heightOk);
        if (dimensions.size() != 2 || !widthOk || !heightOk || width <= 0 || height <= 0)
            return 2;
        window.resize(width, height);
    }
    window.show();
    window.showPage(parser.value("page").toInt());
    if (auto* tabs = window.findChild<QTabWidget*>("statisticsTabs"))
        tabs->setCurrentIndex(qBound(0, parser.value("statistics-tab").toInt(), 1));
    QWidget* captureTarget = &window;
    std::unique_ptr<QDialog> captureDialog;
    if (parser.isSet("capture") && parser.isSet("capture-dialog")) {
        if (parser.value("capture-dialog") == "server")
            captureDialog = std::make_unique<fleet::ServerDialog>(std::nullopt, &window);
        else if (parser.value("capture-dialog") == "inbounds") {
            fleet::ServerConfig server;
            server.id = "preview-master";
            server.name = "Москва · мастер-нода";
            fleet::Inventory inventory;
            inventory.inbounds = {
                {server.id, 1, "VLESS Reality", "vless", 443, true},
                {server.id, 2, "VLESS WebSocket", "vless", 8443, true},
                {server.id, 3, "Hysteria 2", "hysteria", 11891, true},
                {server.id, 4, "Trojan TLS", "trojan", 9443, true},
                {server.id, 5, "Пример выключенного inbound", "vless", 54300, false}};
            fleet::Client client;
            client.serverId = server.id;
            client.email = "synthetic-client";
            client.inboundIds = {1, 2};
            captureDialog = std::make_unique<fleet::ClientInboundsDialog>(server, inventory, client,
                                                                          true, &window);
        } else if (parser.value("capture-dialog") == "client") {
            QList<fleet::ServerConfig> servers;
            QHash<QString, fleet::Inventory> inventories;
            const QStringList names = {"Москва · мастер-нода", "Нидерланды", "Германия"};
            for (int n = 0; n < names.size(); ++n) {
                fleet::ServerConfig server;
                server.id = "preview-" + QString::number(n);
                server.name = names[n];
                fleet::Inventory inventory;
                inventory.inbounds.append({server.id, 1, "VLESS Reality", "vless", 443, true});
                inventory.inbounds.append({server.id, 2, "VLESS WebSocket", "vless", 8443, true});
                if (n == 1)
                    inventory.inbounds.append({server.id, 3, "Trojan TLS", "trojan", 9443, true});
                inventory.inbounds.append({server.id, 4, "Hysteria 2", "hysteria", 11891, true});
                servers.append(server);
                inventories.insert(server.id, inventory);
            }
            captureDialog = std::make_unique<fleet::ClientDialog>(
                servers, inventories, std::nullopt, &window, "preview-0");
        } else
            return 2;
        captureDialog->show();
        captureTarget = captureDialog.get();
    }
    if (parser.isSet("capture"))
        QTimer::singleShot(1100, &app, [&] {
            app.exit(captureTarget->grab().save(parser.value("capture")) ? 0 : 2);
        });
    else if (parser.isSet("smoke"))
        QTimer::singleShot(700, &app, &QCoreApplication::quit);
    return app.exec();
}
