#include "editor_dialogs.h"
#include "main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QPainter>
#include <QPalette>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("3X Control");
    app.setOrganizationName("3XControl");
    app.setApplicationVersion("0.1.0");
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
    parser.addOption({"capture-dialog",
                      "При --capture показать форму server или client на примерах данных.",
                      "kind"});
    parser.process(app);
    QString dataDir = parser.value("data-dir");
    if (dataDir.isEmpty())
        dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    fleet::MainWindow window(dataDir, parser.isSet("demo"));
    window.show();
    window.showPage(parser.value("page").toInt());
    QWidget* captureTarget = &window;
    std::unique_ptr<QDialog> captureDialog;
    if (parser.isSet("capture") && parser.isSet("capture-dialog")) {
        if (parser.value("capture-dialog") == "server")
            captureDialog = std::make_unique<fleet::ServerDialog>(std::nullopt, &window);
        else if (parser.value("capture-dialog") == "client") {
            fleet::ServerConfig server;
            server.id = "preview";
            server.name = "Frankfurt · DE-01";
            fleet::Inventory inventory;
            inventory.inbounds.append({server.id, 1, "VLESS Reality", "vless", 443, true});
            captureDialog = std::make_unique<fleet::ClientDialog>(
                QList<fleet::ServerConfig>{server},
                QHash<QString, fleet::Inventory>{{server.id, inventory}}, std::nullopt, &window);
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
