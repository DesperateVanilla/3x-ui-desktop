#include "main_window.h"
#include "backup_file.h"
#include "chart_widget.h"
#include "client_provisioner.h"
#include "editor_dialogs.h"
#include "three_x_ui_api.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fleet {
namespace {
QString bytes(double value, bool rate = false) {
    const QString suffix = rate ? "/с" : "";
    const QStringList units = {"Б", "КБ", "МБ", "ГБ", "ТБ"};
    int n = 0;
    while (value >= 1024 && n < 4) {
        value /= 1024;
        ++n;
    }
    return QString::number(value, 'f', value < 10 && n > 0 ? 1 : 0) + " " + units[n] + suffix;
}
template <class T> QString percent(const std::optional<T>& v) {
    return v ? QString::number(*v, 'f', 1) + " %" : "—";
}
QString healthName(Health h) {
    switch (h) {
    case Health::Online:
        return "Работает";
    case Health::Warning:
        return "Внимание";
    case Health::Offline:
        return "Недоступен";
    default:
        return "Нет данных";
    }
}
QColor healthColor(Health h) {
    switch (h) {
    case Health::Online:
        return QColor("#46d8a9");
    case Health::Warning:
        return QColor("#e6b950");
    case Health::Offline:
        return QColor("#ef7488");
    default:
        return QColor("#8694a8");
    }
}
QFrame* card(QWidget* parent = nullptr) {
    auto* f = new QFrame(parent);
    f->setObjectName("card");
    return f;
}
QLabel* label(QString text, QString type = {}, QWidget* parent = nullptr) {
    auto* l = new QLabel(std::move(text), parent);
    l->setTextFormat(Qt::PlainText);
    if (!type.isEmpty())
        l->setObjectName(type);
    return l;
}
QPushButton* button(QString text, QString role = {}) {
    auto* b = new QPushButton(std::move(text));
    if (!role.isEmpty())
        b->setProperty("role", role);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
QTableWidget* table(QStringList headers) {
    auto* t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setAlternatingRowColors(false);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setShowGrid(false);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(54);
    t->horizontalHeader()->setHighlightSections(false);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->setFocusPolicy(Qt::StrongFocus);
    return t;
}
void cell(QTableWidget* t, int r, int c, const QString& text, QColor color = QColor("#e4edf8"),
          const QString& tooltip = {}) {
    auto* item = new QTableWidgetItem(text);
    item->setForeground(color);
    item->setToolTip(tooltip);
    t->setItem(r, c, item);
}
QScrollArea* scrollArea(QWidget* content) {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}
void clearLayout(QLayout* layout) {
    while (auto* item = layout->takeAt(0)) {
        if (item->widget())
            delete item->widget();
        if (item->layout()) {
            clearLayout(item->layout());
            delete item->layout();
        }
        delete item;
    }
}
bool confirm(QWidget* parent, const QString& title, const QString& message) {
    QMessageBox box(QMessageBox::Question, title, message, QMessageBox::NoButton, parent);
    auto* yes = box.addButton("Подтвердить", QMessageBox::AcceptRole);
    box.addButton("Отмена", QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton*>(box.buttons().last()));
    box.exec();
    return box.clickedButton() == yes;
}
QString backupName(const QString& server, const QString& extension, bool beforeRestore = false) {
    QString name = server;
    name.replace(QRegularExpression("[^\\p{L}\\p{N}._-]"), "_");
    name = name.left(60);
    return "3x-ui-" + name + (beforeRestore ? "-before-restore-" : "-") +
           QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" +
           QUuid::createUuid().toString(QUuid::Id128).left(6) + extension;
}
} // namespace

QString appStyle() {
    return R"(
    QWidget { background: #0d121a; color: #e4edf8; font-family: 'Segoe UI'; font-size: 14px; }
    QMainWindow { background: #0d121a; }
    QLabel { background: transparent; }
    QLabel#muted { color: #8b99ac; font-size: 13px; }
    QLabel#section { font-size: 16px; font-weight: 600; }
    QLabel#eyebrow { color: #8291a6; font-size: 12px; font-weight: 600; }
    QLabel#value { font-size: 29px; font-weight: 600; }
    QFrame#card { background: #161e29; border: 1px solid #293341; border-radius: 10px; }
    QFrame#sidebar { background: #111822; border-right: 1px solid #26303f; }
    QFrame#topbar { border-bottom: 1px solid #26303f; }
    QFrame#demoBanner { background: #302818; border: 1px solid #64512a; border-radius: 7px; }
    QFrame#demoBanner QLabel { color: #e6c374; }
    QPushButton { background: #1c2735; border: 1px solid #344255; border-radius: 6px; padding: 8px 13px; min-height: 20px; }
    QPushButton:hover { background: #27364a; border-color: #51647c; }
    QPushButton:pressed { background: #304359; }
    QPushButton:disabled { color: #67758a; background: #17202d; border-color: #293344; }
    QPushButton[role='primary'] { background: #4c9dff; border-color: #4c9dff; color: #091728; font-weight: 600; }
    QPushButton[role='primary']:hover { background: #71b2ff; }
    QPushButton[role='primary']:disabled { background: #233c5b; border-color: #2a4462; color: #8e9db0; }
    QPushButton[role='danger'] { color: #f28798; border-color: #5a3543; }
    QPushButton[role='nav'] { background: transparent; border: 0; text-align: left; padding: 13px 16px; color: #99a7ba; font-size: 15px; }
    QPushButton[role='nav']:hover { background: #192434; }
    QPushButton[role='nav'][active='true'] { background: #203651; color: #83bdff; font-weight: 600; }
    QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateTimeEdit { background: #111923; border: 1px solid #344255; border-radius: 6px; padding: 7px 10px; min-height: 20px; selection-background-color: #355d89; }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateTimeEdit:focus { border: 1px solid #4c9dff; }
    QComboBox::drop-down { border: 0; width: 24px; }
    QComboBox QAbstractItemView { background: #1c2735; border: 1px solid #344255; selection-background-color: #24466c; }
    QTableWidget { background: #161e29; border: 1px solid #293341; border-radius: 8px; selection-background-color: #244365; selection-color: #f0f6ff; }
    QTableWidget::item { padding: 8px 12px; border-bottom: 1px solid #242f3e; }
    QTableWidget::item:selected { background: #213e5d; }
    QHeaderView::section { background: #1a2431; color: #93a4bb; font-size: 13px; border: 0; border-bottom: 1px solid #303d4f; padding: 12px; text-align: left; }
    QScrollArea { background: transparent; border: 0; }
    QScrollBar:vertical { background: #101722; width: 8px; margin: 0; }
    QScrollBar::handle:vertical { background: #354358; min-height: 30px; border-radius: 4px; }
    QScrollBar:horizontal { background: #101722; height: 8px; }
    QScrollBar::handle:horizontal { background: #354358; min-width: 30px; border-radius: 4px; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    QDialog { background: #151d29; }
    QCheckBox { background: transparent; spacing: 8px; }
    QCheckBox::indicator { width: 17px; height: 17px; border: 1px solid #53647d; border-radius: 3px; background: #111923; }
    QCheckBox::indicator:checked { background: #4c9dff; border: 1px solid #4c9dff; }
    QToolTip { background: #243247; color: #e4edf8; border: 1px solid #4b5d77; padding: 6px; }
    )";
}

MainWindow::MainWindow(QString dataDirectory, bool demo)
    : dataDirectory_(std::move(dataDirectory)) {
    setWindowTitle("3X Control — управление серверами");
    resize(1400, 970);
    setMinimumSize(1020, 720);
    const auto opened = store_.open(QDir(dataDirectory_).filePath("fleet.db"));
    storageReady_ = opened.ok;
    QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
    pollSeconds_ = qBound(10, settings.value("pollSeconds", 30).toInt(), 600);
    hasMasterPreference_ = settings.contains("masterServerId");
    masterServerId_ = settings.value("masterServerId").toString();
    buildUi();
    if (storageReady_) {
        const auto configs = store_.servers();
        if (configs.ok)
            servers_ = configs.value;
        else
            report(configs.error, true);
        for (const auto& s : servers_)
            apis_.insert(s.id, new ThreeXUiApi(s, this));
        reloadData();
    } else
        report(opened.error, true);
    navigate(0);
    pollTimer_.setInterval(pollSeconds_ * 1000);
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (!demo_)
            poll();
    });
    pollTimer_.start();
    if (demo)
        setDemo(true);
    else {
        refreshUi();
        QTimer::singleShot(500, this, [this] {
            poll();
            fetchInventories();
        });
    }
}
MainWindow::~MainWindow() {
    pollTimer_.stop();
    delete provisioner_;
    provisioner_ = nullptr;
    qDeleteAll(apis_);
    apis_.clear();
}

void MainWindow::buildUi() {
    auto* root = new QWidget;
    auto* frame = new QHBoxLayout(root);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);
    setCentralWidget(root);
    auto* sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(205);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(18, 28, 18, 18);
    side->setSpacing(7);
    auto* brand = label("3X  <span style='color:#8fa5c0'>CONTROL</span>");
    brand->setTextFormat(Qt::RichText);
    brand->setStyleSheet("font-size:22px;font-weight:700;color:#7bb8ff;");
    side->addWidget(brand);
    side->addSpacing(4);
    side->addWidget(label("СЕРВЕРЫ ПОД КОНТРОЛЕМ", "eyebrow"));
    side->addSpacing(32);
    const QStringList nav = {"Обзор сети", "Серверы", "Подписки", "Журнал действий", "Настройки"};
    for (int i = 0; i < nav.size(); ++i) {
        auto* b = button(nav[i], "nav");
        navigation_.append(b);
        side->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] { navigate(i); });
    }
    side->addStretch();
    demoButton_ = button("Посмотреть демо");
    side->addWidget(demoButton_);
    connect(demoButton_, &QPushButton::clicked, this, [this] { setDemo(!demo_); });
    side->addSpacing(16);
    side->addWidget(label("C++ / Qt  ·  Windows", "muted"));
    side->addWidget(label("Локальная история · v0.2", "muted"));
    frame->addWidget(sidebar);
    auto* main = new QWidget;
    auto* layout = new QVBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    frame->addWidget(main, 1);
    auto* top = new QFrame;
    top->setObjectName("topbar");
    auto* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(28, 18, 28, 18);
    title_ = label("Обзор сети");
    title_->setStyleSheet("font-size:23px;font-weight:600;");
    topLayout->addWidget(title_);
    topLayout->addStretch();
    networkState_ = label("Сеть не подключена", "muted");
    topLayout->addWidget(networkState_);
    topLayout->addSpacing(16);
    refreshButton_ = button("Обновить");
    topLayout->addWidget(refreshButton_);
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        if (demo_) {
            generateDemo();
            refreshUi();
            return;
        }
        poll();
        if (pages_->currentIndex() == 2)
            fetchInventories();
    });
    auto* add = button("+ Добавить сервер", "primary");
    writeButtons_.append(add);
    topLayout->addWidget(add);
    connect(add, &QPushButton::clicked, this, &MainWindow::addServer);
    layout->addWidget(top);
    demoBanner_ = new QFrame;
    demoBanner_->setObjectName("demoBanner");
    auto* db = new QHBoxLayout(demoBanner_);
    db->setContentsMargins(16, 9, 16, 9);
    db->addWidget(
        label("ДЕМОРЕЖИМ · Примеры данных. Серверы не подключены, удалённые действия отключены."));
    db->addStretch();
    auto* leave = button("Выйти из демо");
    db->addWidget(leave);
    connect(leave, &QPushButton::clicked, this, [this] { setDemo(false); });
    demoBanner_->hide();
    layout->addWidget(demoBanner_);
    pages_ = new QStackedWidget;
    pages_->addWidget(overviewPage());
    pages_->addWidget(serversPage());
    pages_->addWidget(clientsPage());
    pages_->addWidget(eventsPage());
    pages_->addWidget(settingsPage());
    layout->addWidget(pages_, 1);
    auto* footer = new QWidget;
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(28, 9, 28, 9);
    notice_ = label("Подключите первую панель 3x-ui, чтобы начать мониторинг.", "muted");
    notice_->setWordWrap(true);
    fl->addWidget(notice_, 1);
    lastCheck_ = label("Обновление каждые " + QString::number(pollSeconds_) + " с", "muted");
    fl->addWidget(lastCheck_);
    layout->addWidget(footer);
}

QWidget* MainWindow::overviewPage() {
    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);
    auto* meta = new QHBoxLayout;
    meta->addWidget(label("СОСТОЯНИЕ ВАШЕЙ СЕТИ", "eyebrow"));
    meta->addStretch();
    meta->addWidget(label("Период", "muted"));
    rangeCombo_ = new QComboBox;
    rangeCombo_->addItem("8 часов", 8);
    rangeCombo_->addItem("24 часа", 24);
    rangeCombo_->addItem("7 дней", 168);
    meta->addWidget(rangeCombo_);
    connect(rangeCombo_, &QComboBox::currentIndexChanged, this, [this] {
        rangeHours_ = rangeCombo_->currentData().toInt();
        if (demo_)
            generateDemo();
        else
            reloadData();
        refreshUi();
    });
    root->addLayout(meta);
    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(14);
    const QStringList names = {"СЕРВЕРЫ", "КЛИЕНТОВ ОНЛАЙН", "ВХОДЯЩИЙ ТРАФИК",
                               "СРЕДНЯЯ НАГРУЗКА CPU"};
    for (const auto& name : names) {
        auto* f = card();
        auto* l = new QVBoxLayout(f);
        l->setContentsMargins(20, 16, 20, 16);
        l->setSpacing(8);
        l->addWidget(label(name, "eyebrow"));
        auto* value = label("—", "value");
        metricValues_.append(value);
        l->addWidget(value);
        auto* detail = label("Нет измерений", "muted");
        metricDetails_.append(detail);
        l->addWidget(detail);
        metrics->addWidget(f, 1);
    }
    root->addLayout(metrics);
    auto* health = card();
    auto* hl = new QVBoxLayout(health);
    hl->setContentsMargins(20, 17, 20, 15);
    hl->setSpacing(10);
    auto* head = new QHBoxLayout;
    head->addWidget(label("Доступность серверов", "section"));
    head->addStretch();
    head->addWidget(
        label("Зелёный — работает   ·   Жёлтый — Xray   ·   Красный — нет связи", "muted"));
    hl->addLayout(head);
    auto* healthContent = new QWidget;
    healthRows_ = new QVBoxLayout(healthContent);
    healthRows_->setContentsMargins(0, 0, 0, 0);
    healthRows_->setSpacing(6);
    auto* area = scrollArea(healthContent);
    area->setMinimumHeight(100);
    area->setMaximumHeight(320);
    hl->addWidget(area);
    auto* timeline = new QHBoxLayout;
    timeline->addWidget(label("Серые интервалы: измерений нет", "muted"));
    timeline->addStretch();
    timeline->addWidget(
        label("Начало периода                                            Сейчас", "muted"));
    hl->addLayout(timeline);
    root->addWidget(health);
    auto* charts = new QHBoxLayout;
    charts->setSpacing(16);
    for (int i = 0; i < 2; ++i) {
        auto* f = card();
        auto* l = new QVBoxLayout(f);
        l->setContentsMargins(17, 17, 17, 12);
        l->setSpacing(7);
        l->addWidget(label(i == 0 ? "Клиенты онлайн" : "Сетевой трафик", "section"));
        l->addWidget(label(i == 0 ? "По доступным измерениям всех серверов"
                                  : "Приём  ·  синий     Передача  ·  зелёный",
                           "muted"));
        auto* chart =
            new ChartWidget(i == 0 ? ChartWidget::Kind::Online : ChartWidget::Kind::Bandwidth);
        if (i == 0)
            onlineChart_ = chart;
        else
            trafficChart_ = chart;
        l->addWidget(chart, 1);
        charts->addWidget(f, 1);
    }
    root->addLayout(charts);
    auto* actions = card();
    auto* al = new QHBoxLayout(actions);
    al->setContentsMargins(20, 14, 20, 14);
    auto* texts = new QVBoxLayout;
    texts->addWidget(label("Все подписки в одном месте", "section"));
    texts->addWidget(label("Создание, срок действия, лимиты трафика и состояние клиента", "muted"));
    al->addLayout(texts, 1);
    auto* go = button("Открыть подписки");
    al->addWidget(go);
    connect(go, &QPushButton::clicked, this, [this] { navigate(2); });
    root->addWidget(actions);
    root->addStretch();
    return scrollArea(content);
}

QWidget* MainWindow::serversPage() {
    auto* page = new QWidget;
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(16);
    auto* toolbar = new QHBoxLayout;
    serverSearch_ = new QLineEdit;
    serverSearch_->setPlaceholderText("Поиск по имени, адресу или расположению");
    toolbar->addWidget(serverSearch_, 1);
    connect(serverSearch_, &QLineEdit::textChanged, this, &MainWindow::refreshServersTable);
    auto* edit = button("Изменить");
    auto* remove = button("Удалить подключение", "danger");
    for (auto* b : {edit, remove}) {
        writeButtons_.append(b);
        toolbar->addWidget(b);
    }
    connect(edit, &QPushButton::clicked, this, &MainWindow::editServer);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeServer);
    l->addLayout(toolbar);
    serverTable_ = table({"СЕРВЕР / РАСПОЛОЖЕНИЕ", "СТАТУС", "CPU", "RAM", "ДИСК", "ОНЛАЙН",
                          "ОТКЛИК", "ВЕРСИЯ 3X-UI"});
    serverTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(serverTable_, &QTableWidget::itemDoubleClicked, this, [this] { editServer(); });
    l->addWidget(serverTable_, 1);
    auto* operations = new QHBoxLayout;
    auto* open = button("Открыть 3x-ui");
    operations->addWidget(open);
    connect(open, &QPushButton::clicked, this, [this] {
        if (auto* s = selectedServer())
            QDesktopServices::openUrl(s->panelUrl);
    });
    operations->addStretch();
    auto* restart = button("Перезапустить Xray");
    auto* geo = button("Обновить Geo-файлы");
    auto* update = button("Обновить Xray");
    for (auto* b : {restart, geo, update}) {
        b->setProperty("allowPolling", true);
        writeButtons_.append(b);
        operations->addWidget(b);
    }
    connect(restart, &QPushButton::clicked, this,
            [this] { runServerAction(ServerAction::RestartXray); });
    connect(geo, &QPushButton::clicked, this,
            [this] { runServerAction(ServerAction::UpdateGeofiles); });
    connect(update, &QPushButton::clicked, this,
            [this] { runServerAction(ServerAction::InstallXray); });
    l->addLayout(operations);
    auto* backups = new QHBoxLayout;
    auto* backup = button("Бэкап 3x-ui");
    auto* restore = button("Восстановить из бэкапа", "danger");
    for (auto* b : {backup, restore}) {
        writeButtons_.append(b);
        backups->addWidget(b);
    }
    backups->addStretch();
    connect(backup, &QPushButton::clicked, this, &MainWindow::backupServer);
    connect(restore, &QPushButton::clicked, this, &MainWindow::restoreServer);
    l->addLayout(backups);
    l->addWidget(
        label("Выберите сервер для действий. Удаление подключения сохраняет клиентов на сервере.",
              "muted"));
    return page;
}

QWidget* MainWindow::clientsPage() {
    auto* page = new QWidget;
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(14);
    auto* filters = new QHBoxLayout;
    clientSearch_ = new QLineEdit;
    clientSearch_->setPlaceholderText("Найти подписку по имени клиента или серверу");
    filters->addWidget(clientSearch_, 1);
    serverFilter_ = new QComboBox;
    serverFilter_->setMinimumWidth(180);
    serverFilter_->addItem("Все серверы", "");
    filters->addWidget(serverFilter_);
    auto* load = button("Загрузить клиентов");
    filters->addWidget(load);
    connect(load, &QPushButton::clicked, this, &MainWindow::fetchInventories);
    connect(serverFilter_, &QComboBox::currentIndexChanged, this, &MainWindow::refreshClientsTable);
    connect(clientSearch_, &QLineEdit::textChanged, this, &MainWindow::refreshClientsTable);
    l->addLayout(filters);
    auto* toolbar = new QHBoxLayout;
    auto* create = button("+ Создать подписку", "primary");
    auto* edit = button("Изменить");
    auto* renew = button("Продлить на 30 дней");
    auto* toggle = button("Включить / выключить");
    auto* reset = button("Сбросить трафик");
    auto* remove = button("Удалить", "danger");
    for (auto* b : {create, edit, renew, toggle, reset, remove}) {
        b->setProperty("allowPolling", true);
        toolbar->addWidget(b);
        writeButtons_.append(b);
    }
    toolbar->addStretch();
    l->addLayout(toolbar);
    connect(create, &QPushButton::clicked, this, [this] { editClient(true); });
    connect(edit, &QPushButton::clicked, this, [this] { editClient(false); });
    connect(renew, &QPushButton::clicked, this, &MainWindow::renewClient);
    connect(toggle, &QPushButton::clicked, this, &MainWindow::toggleClient);
    connect(reset, &QPushButton::clicked, this, &MainWindow::resetClient);
    connect(remove, &QPushButton::clicked, this, &MainWindow::deleteClient);
    masterStatus_ = label("", "muted");
    masterStatus_->setWordWrap(true);
    l->addWidget(masterStatus_);
    clientTable_ = table({"КЛИЕНТ", "СЕРВЕР", "ИНБАУНДЫ", "ПРОТОКОЛ", "СОСТОЯНИЕ", "ТРАФИК / ЛИМИТ",
                          "ДЕЙСТВУЕТ ДО"});
    clientTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    l->addWidget(clientTable_, 1);
    connect(clientTable_, &QTableWidget::itemDoubleClicked, this, [this] { editClient(false); });
    auto* footer = new QHBoxLayout;
    clientStatus_ = label("Клиенты ещё не загружены", "muted");
    clientStatus_->setWordWrap(true);
    footer->addWidget(clientStatus_, 1);
    auto* copy = button("Копировать ссылку подписки");
    footer->addWidget(copy);
    connect(copy, &QPushButton::clicked, this, &MainWindow::copySubscriptionLink);
    l->addLayout(footer);
    return page;
}

QWidget* MainWindow::eventsPage() {
    auto* page = new QWidget;
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(16);
    l->addWidget(label("Изменения подписок, действия на серверах и ошибки подключения", "muted"));
    eventTable_ = table({"ВРЕМЯ", "СЕРВЕР", "ДЕЙСТВИЕ", "РЕЗУЛЬТАТ", "ПОДРОБНОСТИ"});
    eventTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    l->addWidget(eventTable_, 1);
    return page;
}
QWidget* MainWindow::settingsPage() {
    auto* page = new QWidget;
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(20);
    auto* monitoring = card();
    auto* ml = new QVBoxLayout(monitoring);
    ml->setContentsMargins(24, 22, 24, 22);
    ml->addWidget(label("Мониторинг", "section"));
    auto* row = new QHBoxLayout;
    row->addWidget(label("Проверять серверы каждые"));
    intervalBox_ = new QSpinBox;
    intervalBox_->setRange(10, 600);
    intervalBox_->setValue(pollSeconds_);
    intervalBox_->setSuffix(" с");
    row->addWidget(intervalBox_);
    row->addStretch();
    ml->addLayout(row);
    ml->addWidget(label("Измерения собираются, пока приложение запущено. История хранится 30 дней.",
                        "muted"));
    connect(intervalBox_, &QSpinBox::valueChanged, this, [this](int value) {
        pollSeconds_ = value;
        pollTimer_.setInterval(value * 1000);
        QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("pollSeconds", value);
        report("Интервал проверки сохранён.");
    });
    l->addWidget(monitoring);
    auto* subscription = card();
    auto* subs = new QVBoxLayout(subscription);
    subs->setContentsMargins(24, 22, 24, 22);
    subs->setSpacing(12);
    subs->addWidget(label("Мастер-нода подписок", "section"));
    auto* masterHelp = label("Выберите Москву или другой сервер, который уже объединяет ваши "
                             "подписки в 3x-ui. При копировании общей ссылки приложение использует "
                             "его адрес выдачи подписок.");
    masterHelp->setWordWrap(true);
    subs->addWidget(masterHelp);
    masterCombo_ = new QComboBox;
    masterCombo_->setObjectName("masterServer");
    masterCombo_->addItem("Без мастер-ноды — ссылка выбранного сервера", "");
    subs->addWidget(masterCombo_);
    auto* topology = label("Выбор мастер-ноды здесь не меняет связи между серверами в 3x-ui. "
                           "Для общей ссылки клиент должен быть создан и на мастер-ноде.",
                           "muted");
    topology->setWordWrap(true);
    subs->addWidget(topology);
    connect(masterCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (demo_)
            return;
        masterServerId_ = masterCombo_->currentData().toString();
        hasMasterPreference_ = true;
        QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("masterServerId", masterServerId_);
        settings.sync();
        refreshMasterSelector();
        report(settings.status() == QSettings::NoError ? "Мастер-нода сохранена."
                                                       : "Не удалось сохранить мастер-ноду.",
               settings.status() != QSettings::NoError);
    });
    l->addWidget(subscription);
    auto* storage = card();
    auto* sl = new QVBoxLayout(storage);
    sl->setContentsMargins(24, 22, 24, 22);
    sl->setSpacing(12);
    sl->addWidget(label("Локальные данные", "section"));
    auto* info = label("Пароли и токены защищены Windows и доступны только вашей учётной записи. "
                       "При переносе базы на другой ПК учётные данные нужно ввести заново.");
    info->setWordWrap(true);
    sl->addWidget(info);
    auto* path = label(dataDirectory_, "muted");
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    sl->addWidget(path);
    auto* folder = button("Открыть папку данных");
    sl->addWidget(folder, 0, Qt::AlignLeft);
    connect(folder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirectory_)); });
    l->addWidget(storage);
    auto* help = card();
    auto* hl = new QVBoxLayout(help);
    hl->setContentsMargins(24, 22, 24, 22);
    hl->addWidget(label("Подключение 3x-ui", "section"));
    auto* notes =
        label("Добавьте полный адрес панели, включая порт и её скрытый путь. Для актуальных 3x-ui "
              "удобно использовать API-токен с правами администратора. Для старых панелей доступен "
              "вход по логину и паролю. Адрес выдачи подписок задаётся отдельно.");
    notes->setWordWrap(true);
    hl->addWidget(notes);
    hl->addWidget(label("Сертификаты HTTPS проверяются. Самоподписанный сертификат нужно добавить "
                        "в доверенные сертификаты Windows.",
                        "muted"));
    l->addWidget(help);
    l->addStretch();
    return scrollArea(page);
}

void MainWindow::navigate(int page) {
    pages_->setCurrentIndex(page);
    const QStringList titles = {"Обзор сети", "Серверы", "Подписки", "Журнал действий",
                                "Настройки"};
    title_->setText(titles.value(page));
    for (int i = 0; i < navigation_.size(); ++i) {
        navigation_[i]->setProperty("active", i == page);
        navigation_[i]->style()->unpolish(navigation_[i]);
        navigation_[i]->style()->polish(navigation_[i]);
    }
    if (page == 2 && !demo_ && inventories_.isEmpty())
        fetchInventories();
}
QString MainWindow::serverName(const QString& id) const {
    for (const auto& s : servers_)
        if (s.id == id)
            return s.name;
    return "Удалённый сервер";
}
QString MainWindow::activeMasterServerId() const {
    return demo_ ? "demo-0" : masterServerId_;
}
void MainWindow::refreshMasterSelector() {
    if (!demo_ && !hasMasterPreference_) {
        QStringList matches;
        for (const auto& server : servers_) {
            const QString text = server.name + " " + server.location;
            if (text.contains("Москва", Qt::CaseInsensitive) ||
                text.contains("Moscow", Qt::CaseInsensitive))
                matches.append(server.id);
        }
        if (matches.size() == 1) {
            masterServerId_ = matches.first();
            hasMasterPreference_ = true;
            QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
            settings.setValue("masterServerId", masterServerId_);
        }
    }
    const QString id = activeMasterServerId();
    masterCombo_->blockSignals(true);
    masterCombo_->clear();
    masterCombo_->addItem("Без мастер-ноды — ссылка выбранного сервера", "");
    for (const auto& server : servers_)
        masterCombo_->addItem(server.name, server.id);
    masterCombo_->setCurrentIndex(qMax(0, masterCombo_->findData(id)));
    masterCombo_->blockSignals(false);
    masterStatus_->setText(id.isEmpty()
                               ? "Создание доступно на нескольких серверах. Мастер-ноду для общей "
                                 "ссылки выберите в Настройках."
                               : "Мастер-нода: " + serverName(id) +
                                     " · общая ссылка использует её адрес выдачи подписок.");
}
void MainWindow::copySubscriptionLink() {
    auto client = selectedClient();
    if (!client)
        return;
    const QString master = activeMasterServerId();
    const QString id = master.isEmpty() ? client->serverId : master;
    if (!master.isEmpty() && client->serverId != master) {
        if (!inventories_.contains(master)) {
            report("Сначала загрузите клиентов мастер-ноды, чтобы проверить общую подписку.", true);
            return;
        }
        bool matched = false;
        for (const auto& candidate : inventories_.value(master).clients)
            if (candidate.email == client->email && !candidate.subId.isEmpty() &&
                candidate.subId == client->subId) {
                matched = true;
                break;
            }
        if (!matched) {
            report("На мастер-ноде нет клиента с этим именем и subId. "
                   "При создании общей подписки выберите также её инбаунды.",
                   true);
            return;
        }
    }
    const auto server =
        std::find_if(servers_.cbegin(), servers_.cend(), [&](const auto& s) { return s.id == id; });
    if (server == servers_.cend() || server->subscriptionUrl.isEmpty() || client->subId.isEmpty()) {
        report("Укажите адрес выдачи подписок в настройках " +
                   QString(master.isEmpty() ? "выбранного сервера" : "мастер-ноды") +
                   ". Например: https://vpn.example.com:2096/sub/",
               true);
        return;
    }
    QUrl link = server->subscriptionUrl;
    QString path = link.path(QUrl::FullyEncoded);
    if (!path.endsWith('/'))
        path += '/';
    path += QString::fromLatin1(QUrl::toPercentEncoding(client->subId));
    link.setPath(path, QUrl::StrictMode);
    QApplication::clipboard()->setText(link.toString(QUrl::FullyEncoded));
    report("Ссылка подписки скопирована: " + server->name + ".");
}
void MainWindow::report(const QString& text, bool error) {
    notice_->setText(text);
    notice_->setStyleSheet(error ? "color:#ef8d9d;" : "color:#8b99ac;");
}
bool MainWindow::writable(bool allowPolling) {
    if (demo_) {
        report("Удалённые действия недоступны в деморежиме.");
        return false;
    }
    if (!storageReady_) {
        report("Локальная база недоступна.", true);
        return false;
    }
    if (mutating_ || inventoryPending_ || (polling_ && !allowPolling)) {
        report("Дождитесь завершения текущей операции.");
        return false;
    }
    return true;
}
void MainWindow::setBusyUi() {
    refreshButton_->setEnabled(!polling_);
    refreshButton_->setText(polling_ ? "Проверка…" : "Обновить");
    for (auto* b : writeButtons_)
        b->setEnabled(storageReady_ && !demo_ && !mutating_ && !inventoryPending_ &&
                      (!polling_ || b->property("allowPolling").toBool()));
    demoButton_->setEnabled(!polling_ && !mutating_ && !inventoryPending_);
    masterCombo_->setEnabled(storageReady_ && !demo_ && !mutating_);
}
void MainWindow::reloadData() {
    if (demo_ || !storageReady_)
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    if (lastPruned_ != now.date()) {
        const auto pruned = store_.prune(now.addDays(-30));
        if (pruned.ok)
            lastPruned_ = now.date();
        else
            report(pruned.error, true);
    }
    const auto h = store_.history({}, now.addSecs(-rangeHours_ * 3600));
    if (h.ok) {
        history_ = h.value;
        for (const auto& v : history_)
            if (!latest_.contains(v.serverId) || v.at > latest_[v.serverId].at)
                latest_[v.serverId] = v;
    } else
        report(h.error, true);
    const auto e = store_.events(150);
    if (e.ok)
        events_ = e.value;
    else
        report(e.error, true);
}

void MainWindow::refreshUi() {
    int onlineServers = 0, warnings = 0;
    int clientsOnline = 0, cpuCount = 0, onlineCount = 0, rxCount = 0;
    double cpu = 0, rx = 0, tx = 0;
    QDateTime recent;
    for (const auto& s : servers_) {
        auto snap = latest_.value(s.id);
        if (snap.at.isValid() &&
            snap.at.secsTo(QDateTime::currentDateTimeUtc()) > qMax(90, pollSeconds_ * 3))
            snap.health = Health::Unknown;
        if (snap.health == Health::Online)
            ++onlineServers;
        else if (snap.health == Health::Warning || snap.health == Health::Offline)
            ++warnings;
        if (snap.health == Health::Online || snap.health == Health::Warning) {
            if (snap.online) {
                clientsOnline += *snap.online;
                ++onlineCount;
            }
            if (snap.cpu) {
                cpu += *snap.cpu;
                ++cpuCount;
            }
            if (snap.rxBps) {
                rx += *snap.rxBps;
                ++rxCount;
            }
            if (snap.txBps)
                tx += *snap.txBps;
        }
        if (snap.at > recent)
            recent = snap.at;
    }
    metricValues_[0]->setText(QString::number(onlineServers) + " / " +
                              QString::number(servers_.size()));
    metricDetails_[0]->setText(servers_.isEmpty()
                                   ? "Подключите первый сервер"
                                   : QString::number(warnings) + " требуют внимания");
    metricValues_[1]->setText(onlineCount ? QString::number(clientsOnline) : "—");
    metricDetails_[1]->setText(onlineCount ? "Данные " + QString::number(onlineCount) + " из " +
                                                 QString::number(servers_.size()) + " серверов"
                                           : "Нет измерений онлайна");
    metricValues_[2]->setText(rxCount ? bytes(rx, true) : "—");
    metricDetails_[2]->setText(rxCount ? "Передача: " + bytes(tx, true) : "Нет измерений трафика");
    metricValues_[3]->setText(cpuCount ? QString::number(cpu / cpuCount, 'f', 1) + " %" : "—");
    metricDetails_[3]->setText(cpuCount ? "По " + QString::number(cpuCount) + " серверам"
                                        : "Нет измерений нагрузки");
    QString state = servers_.isEmpty() ? "Сеть не подключена"
                    : warnings         ? QString::number(warnings) + " требуют внимания"
                    : onlineServers == servers_.size() ? "Все серверы работают"
                                                       : "Ожидаем измерения";
    networkState_->setText(state);
    networkState_->setStyleSheet(QString("color:%1;")
                                     .arg(warnings            ? "#e6b950"
                                          : onlineServers > 0 ? "#46d8a9"
                                                              : "#8b99ac"));
    lastCheck_->setText(demo_ ? "Демонстрационные данные"
                        : recent.isValid()
                            ? "Последняя проверка " + recent.toLocalTime().toString("HH:mm:ss")
                            : "Проверка каждые " + QString::number(pollSeconds_) + " с");
    const QString oldFilter = serverFilter_->currentData().toString();
    serverFilter_->blockSignals(true);
    serverFilter_->clear();
    serverFilter_->addItem("Все серверы", "");
    for (const auto& s : servers_)
        serverFilter_->addItem(s.name, s.id);
    int idx = serverFilter_->findData(oldFilter);
    serverFilter_->setCurrentIndex(qMax(0, idx));
    serverFilter_->blockSignals(false);
    refreshMasterSelector();
    refreshServersTable();
    refreshClientsTable();
    refreshEventsTable();
    refreshCharts();
    refreshHealth();
    setBusyUi();
}
void MainWindow::refreshServersTable() {
    const QString search = serverSearch_->text().trimmed();
    QString selected;
    int oldRow = serverTable_->currentRow();
    if (oldRow >= 0 && oldRow < visibleServers_.size())
        selected = visibleServers_[oldRow];
    visibleServers_.clear();
    serverTable_->setRowCount(0);
    for (const auto& s : servers_) {
        if (!search.isEmpty() && !QString(s.name + " " + s.location + " " + s.panelUrl.host())
                                      .contains(search, Qt::CaseInsensitive))
            continue;
        int row = serverTable_->rowCount();
        serverTable_->insertRow(row);
        visibleServers_.append(s.id);
        const auto v = latest_.value(s.id);
        Health h = v.health;
        if (!v.at.isValid() ||
            v.at.secsTo(QDateTime::currentDateTimeUtc()) > qMax(90, pollSeconds_ * 3))
            h = Health::Unknown;
        cell(serverTable_, row, 0, {}, QColor("#e4edf8"),
             s.name + "\n" + s.location + " · " + s.panelUrl.host());
        auto* info = new QWidget;
        info->setAttribute(Qt::WA_TransparentForMouseEvents);
        info->setStyleSheet("background:transparent;");
        auto* infoLayout = new QVBoxLayout(info);
        infoLayout->setContentsMargins(12, 5, 12, 5);
        infoLayout->setSpacing(2);
        infoLayout->addWidget(label(s.name));
        infoLayout->addWidget(label(s.location + " · " + s.panelUrl.host(), "muted"));
        info->setToolTip(s.name + "\n" + s.location + " · " + s.panelUrl.host());
        serverTable_->setCellWidget(row, 0, info);
        cell(serverTable_, row, 1, healthName(h), healthColor(h), v.error);
        cell(serverTable_, row, 2, percent(v.cpu));
        cell(serverTable_, row, 3, percent(v.memoryPercent));
        cell(serverTable_, row, 4, percent(v.diskPercent));
        cell(serverTable_, row, 5, v.online ? QString::number(*v.online) : "—");
        cell(serverTable_, row, 6, v.latencyMs ? QString::number(*v.latencyMs) + " мс" : "—");
        cell(serverTable_, row, 7, v.panelVersion.isEmpty() ? "—" : v.panelVersion);
        if (s.id == selected)
            serverTable_->selectRow(row);
    }
}
void MainWindow::refreshClientsTable() {
    if (!clientTable_)
        return;
    const QString search = clientSearch_->text().trimmed(),
                  filter = serverFilter_->currentData().toString();
    QString selected;
    const int selectedRow = clientTable_->currentRow();
    if (selectedRow >= 0 && selectedRow < visibleClients_.size())
        selected = visibleClients_[selectedRow].serverId + "|" + visibleClients_[selectedRow].email;
    visibleClients_.clear();
    clientTable_->setRowCount(0);
    int total = 0;
    for (const auto& s : servers_)
        for (const auto& c : inventories_.value(s.id).clients) {
            ++total;
            if (!filter.isEmpty() && c.serverId != filter)
                continue;
            if (!search.isEmpty() &&
                !QString(c.email + " " + s.name).contains(search, Qt::CaseInsensitive))
                continue;
            const int row = clientTable_->rowCount();
            clientTable_->insertRow(row);
            visibleClients_.append(c);
            cell(clientTable_, row, 0, c.email);
            cell(clientTable_, row, 1, s.name);
            QStringList bindings;
            QStringList protocols;
            for (int inboundId : c.inboundIds) {
                QString description = "#" + QString::number(inboundId);
                for (const auto& inbound : inventories_.value(s.id).inbounds)
                    if (inbound.id == inboundId) {
                        description +=
                            " · " + inbound.remark + " · " + QString::number(inbound.port);
                        const QString protocol = inbound.protocol.toUpper();
                        if (!protocols.contains(protocol))
                            protocols.append(protocol);
                        break;
                    }
                bindings.append(description);
            }
            cell(clientTable_, row, 2,
                 c.inboundIds.isEmpty() ? "Нет привязок" : QString::number(c.inboundIds.size()),
                 QColor("#93a4bb"), bindings.join('\n'));
            protocols.sort();
            cell(clientTable_, row, 3,
                 protocols.isEmpty() ? (c.protocol.isEmpty() ? "—" : c.protocol.toUpper())
                                     : protocols.join(" / "));
            QString state = "Активна";
            QColor color("#46d8a9");
            if (c.inboundIds.isEmpty()) {
                state = "Нет инбаундов";
                color = QColor("#e6b950");
            } else if (!c.enable) {
                state = "Отключена";
                color = QColor("#8b99ac");
            } else if (c.expiryTime > 0 && c.expiryTime < QDateTime::currentMSecsSinceEpoch()) {
                state = "Истекла";
                color = QColor("#e6b950");
            } else if (c.totalBytes > 0 && c.usedBytes >= c.totalBytes) {
                state = "Лимит исчерпан";
                color = QColor("#e6b950");
            } else if (c.online && *c.online)
                state = "Онлайн";
            cell(clientTable_, row, 4, state, color);
            cell(clientTable_, row, 5,
                 bytes(double(c.usedBytes)) + " / " +
                     (c.totalBytes ? bytes(double(c.totalBytes)) : "∞"));
            cell(clientTable_, row, 6,
                 c.expiryTime == 0 ? "Без срока"
                 : c.expiryTime < 0
                     ? "После первого входа"
                     : QDateTime::fromMSecsSinceEpoch(c.expiryTime).toString("dd.MM.yyyy HH:mm"));
            if (c.serverId + "|" + c.email == selected)
                clientTable_->selectRow(row);
        }
    QString status = QString::number(visibleClients_.size()) + " из " + QString::number(total) +
                     " подписок · " + QString::number(inventories_.size()) + " из " +
                     QString::number(servers_.size()) + " серверов загружены";
    if (inventoryPending_)
        status = "Загрузка клиентов… " + QString::number(inventoryPending_) + " серверов";
    else if (!inventoryErrors_.isEmpty()) {
        QStringList errors;
        for (auto it = inventoryErrors_.cbegin(); it != inventoryErrors_.cend(); ++it)
            errors << serverName(it.key()) + ": " + it.value();
        status += " · Ошибки: " + errors.join("; ");
    }
    clientStatus_->setText(status);
    clientStatus_->setStyleSheet(inventoryErrors_.isEmpty() ? "color:#8b99ac;" : "color:#ef8d9d;");
}
void MainWindow::refreshEventsTable() {
    eventTable_->setRowCount(events_.size());
    for (int i = 0; i < events_.size(); ++i) {
        const auto& e = events_[i];
        cell(eventTable_, i, 0, e.at.toLocalTime().toString("dd.MM HH:mm:ss"));
        cell(eventTable_, i, 1, e.serverName);
        cell(eventTable_, i, 2, e.action);
        cell(eventTable_, i, 3, e.success ? "Выполнено" : "Ошибка",
             e.success ? QColor("#46d8a9") : QColor("#ef7488"));
        cell(eventTable_, i, 4, e.detail, QColor("#93a4bb"), e.detail);
    }
}
void MainWindow::refreshCharts() {
    const QDateTime now = QDateTime::currentDateTimeUtc(), from = now.addSecs(-rangeHours_ * 3600);
    QMap<qint64, QHash<QString, Snapshot>> buckets;
    for (const auto& s : history_) {
        if (s.at < from)
            continue;
        qint64 at = s.at.toMSecsSinceEpoch() / 60000 * 60000;
        buckets[at][s.serverId] = s;
    }
    QList<ChartPoint> online, traffic;
    for (auto it = buckets.cbegin(); it != buckets.cend(); ++it) {
        std::optional<double> users, rx, tx;
        for (const auto& s : it.value()) {
            if (s.health != Health::Online && s.health != Health::Warning)
                continue;
            if (s.online)
                users = users.value_or(0) + *s.online;
            if (s.rxBps)
                rx = rx.value_or(0) + *s.rxBps;
            if (s.txBps)
                tx = tx.value_or(0) + *s.txBps;
        }
        online.append({it.key(), users, {}});
        traffic.append({it.key(), rx, tx});
    }
    onlineChart_->setPoints(online, from, now);
    trafficChart_->setPoints(traffic, from, now);
}
void MainWindow::refreshHealth() {
    clearLayout(healthRows_);
    const QDateTime now = QDateTime::currentDateTimeUtc(), from = now.addSecs(-rangeHours_ * 3600);
    if (servers_.isEmpty()) {
        auto* empty = new QWidget;
        auto* l = new QHBoxLayout(empty);
        l->setContentsMargins(0, 20, 0, 20);
        auto* text = label("Серверы ещё не подключены. Добавьте панель 3x-ui или посмотрите демо.");
        text->setWordWrap(true);
        l->addWidget(text, 1);
        auto* add = button("+ Добавить сервер", "primary");
        add->setEnabled(storageReady_ && !demo_ && !polling_);
        connect(add, &QPushButton::clicked, this, &MainWindow::addServer);
        l->addWidget(add);
        healthRows_->addWidget(empty);
        return;
    }
    for (const auto& s : servers_) {
        auto* row = new QWidget;
        auto* l = new QHBoxLayout(row);
        l->setContentsMargins(0, 8, 0, 8);
        l->setSpacing(14);
        const auto v = latest_.value(s.id);
        Health h = v.health;
        if (!v.at.isValid() || v.at.secsTo(now) > qMax(90, pollSeconds_ * 3))
            h = Health::Unknown;
        auto* info = new QWidget;
        info->setFixedWidth(220);
        auto* il = new QVBoxLayout(info);
        il->setContentsMargins(0, 0, 0, 0);
        il->setSpacing(3);
        auto* name = label(s.name);
        name->setStyleSheet("font-weight:600;");
        il->addWidget(name);
        il->addWidget(label(s.location + " · " + s.panelUrl.host(), "muted"));
        l->addWidget(info);
        auto* status = label(healthName(h));
        status->setFixedWidth(100);
        status->setStyleSheet("color:" + healthColor(h).name() + ";");
        status->setToolTip(v.error);
        l->addWidget(status);
        auto* measured = label(v.latencyMs ? QString::number(*v.latencyMs) + " мс" : "—", "muted");
        measured->setFixedWidth(62);
        l->addWidget(measured);
        auto* bars = new HealthBar;
        QList<QPair<qint64, int>> samples;
        int healthy = 0, total = 0;
        for (const auto& snap : history_)
            if (snap.serverId == s.id && snap.at >= from) {
                samples.append({snap.at.toMSecsSinceEpoch(), int(snap.health)});
                ++total;
                if (snap.health == Health::Online)
                    ++healthy;
            }
        bars->setSamples(samples, from, now);
        l->addWidget(bars, 1);
        auto* availability =
            label(total ? QString::number(100.0 * healthy / total, 'f', 1) + " %" : "—", "muted");
        availability->setFixedWidth(65);
        availability->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        availability->setToolTip("Доля успешных измерений за выбранный период");
        l->addWidget(availability);
        healthRows_->addWidget(row);
    }
    healthRows_->addStretch();
}

void MainWindow::poll() {
    if (demo_ || polling_ || mutating_ || QApplication::activeModalWidget() || !storageReady_ ||
        servers_.isEmpty())
        return;
    polling_ = true;
    pollQueue_.clear();
    for (const auto& s : servers_)
        pollQueue_.enqueue(s.id);
    activePolls_ = 0;
    setBusyUi();
    pollNext();
}
void MainWindow::pollNext() {
    while (activePolls_ < 5 && !pollQueue_.isEmpty()) {
        const QString id = pollQueue_.dequeue();
        auto* api = apis_.value(id);
        if (!api)
            continue;
        ++activePolls_;
        api->fetchStatus([this, id](Outcome<Snapshot> result) {
            Snapshot snap;
            if (result.ok)
                snap = result.value;
            else {
                snap.serverId = id;
                snap.at = QDateTime::currentDateTimeUtc();
                snap.health = Health::Offline;
                snap.error = result.error;
            }
            const Health previous = latest_.value(id).health;
            latest_[id] = snap;
            const auto saved = store_.appendSnapshot(snap);
            if (!saved.ok)
                report(saved.error, true);
            if (snap.health != previous &&
                (snap.health == Health::Offline || snap.health == Health::Warning))
                recordEvent(id, "Мониторинг", false,
                            snap.error.isEmpty() ? "Xray требует внимания" : snap.error);
            --activePolls_;
            if (pollQueue_.isEmpty() && activePolls_ == 0) {
                polling_ = false;
                reloadData();
                refreshUi();
                if (inventoryPending_ == 0)
                    report("Проверка завершена. Данные серверов обновлены.");
            } else
                pollNext();
        });
    }
    if (pollQueue_.isEmpty() && activePolls_ == 0) {
        polling_ = false;
        setBusyUi();
    }
}
void MainWindow::fetchInventories() {
    loadInventories();
}
void MainWindow::loadInventories(const QString& serverId) {
    if (demo_) {
        refreshClientsTable();
        return;
    }
    if (inventoryPending_ || mutating_ || QApplication::activeModalWidget() || servers_.isEmpty())
        return;
    inventoryQueue_.clear();
    if (serverId.isEmpty()) {
        inventoryErrors_.clear();
        for (const auto& s : servers_)
            inventoryQueue_.enqueue(s.id);
    } else {
        if (!apis_.contains(serverId))
            return;
        inventoryErrors_.remove(serverId);
        inventoryQueue_.enqueue(serverId);
    }
    inventoryPending_ = inventoryQueue_.size();
    activeInventories_ = 0;
    setBusyUi();
    refreshClientsTable();
    inventoryNext();
}
void MainWindow::inventoryNext() {
    while (activeInventories_ < 5 && !inventoryQueue_.isEmpty()) {
        const QString id = inventoryQueue_.dequeue();
        auto* api = apis_.value(id);
        if (!api) {
            --inventoryPending_;
            continue;
        }
        ++activeInventories_;
        api->fetchInventory([this, id](Outcome<Inventory> result) {
            if (result.ok)
                inventories_[id] = result.value;
            else {
                inventories_.remove(id);
                inventoryErrors_[id] = result.error;
            }
            --inventoryPending_;
            --activeInventories_;
            refreshClientsTable();
            setBusyUi();
            if (inventoryPending_ == 0)
                report(inventoryErrors_.isEmpty()
                           ? "Список подписок обновлён."
                           : "Часть серверов недоступна. Подробности показаны под списком.",
                       !inventoryErrors_.isEmpty());
            else
                inventoryNext();
        });
    }
    if (inventoryPending_ == 0)
        setBusyUi();
}

ServerConfig* MainWindow::selectedServer() {
    const int row = serverTable_->currentRow();
    if (row < 0 || row >= visibleServers_.size()) {
        report("Сначала выберите сервер в списке.");
        return nullptr;
    }
    const QString id = visibleServers_[row];
    for (auto& s : servers_)
        if (s.id == id)
            return &s;
    return nullptr;
}
std::optional<Client> MainWindow::selectedClient() {
    const int row = clientTable_->currentRow();
    if (row < 0 || row >= visibleClients_.size()) {
        report("Сначала выберите подписку в списке.");
        return {};
    }
    return visibleClients_[row];
}
void MainWindow::addServer() {
    if (!writable())
        return;
    ServerDialog dialog({}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto config = dialog.result();
    for (const auto& s : servers_)
        if (s.panelUrl.toString().trimmed().remove(QRegularExpression("/+$")) ==
            config.panelUrl.toString().trimmed().remove(QRegularExpression("/+$"))) {
            report("Этот адрес панели уже добавлен: " + s.name, true);
            return;
        }
    const auto saved = store_.saveServer(config);
    if (!saved.ok) {
        report(saved.error, true);
        return;
    }
    servers_.append(config);
    apis_.insert(config.id, new ThreeXUiApi(config, this));
    recordEvent(config.id, "Подключение добавлено", true, "Настройки сохранены на этом компьютере");
    refreshUi();
    navigate(1);
    poll();
    fetchInventories();
}
void MainWindow::editServer() {
    if (!writable())
        return;
    auto* existing = selectedServer();
    if (!existing)
        return;
    const QString id = existing->id;
    ServerDialog dialog(*existing, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto config = dialog.result();
    const auto saved = store_.saveServer(config);
    if (!saved.ok) {
        report(saved.error, true);
        return;
    }
    for (auto& s : servers_)
        if (s.id == id)
            s = config;
    delete apis_.take(id);
    apis_[id] = new ThreeXUiApi(config, this);
    inventories_.remove(id);
    recordEvent(id, "Подключение изменено", true, "Настройки обновлены");
    refreshUi();
    poll();
    fetchInventories();
}
void MainWindow::removeServer() {
    if (!writable())
        return;
    auto* s = selectedServer();
    if (!s)
        return;
    const QString id = s->id, name = s->name;
    if (!confirm(this, "Удалить подключение?",
                 "Удалить «" + name + "» и его локальную историю? Клиенты на сервере сохранятся."))
        return;
    const auto removed = store_.removeServer(id);
    if (!removed.ok) {
        report(removed.error, true);
        return;
    }
    recordEvent(id, "Подключение удалено", true, "Удалена локальная запись сервера");
    for (int i = 0; i < servers_.size(); ++i)
        if (servers_[i].id == id) {
            servers_.removeAt(i);
            break;
        }
    delete apis_.take(id);
    inventories_.remove(id);
    inventoryErrors_.remove(id);
    latest_.remove(id);
    if (masterServerId_ == id) {
        masterServerId_.clear();
        hasMasterPreference_ = true;
        QSettings settings(QDir(dataDirectory_).filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("masterServerId", "");
    }
    reloadData();
    refreshUi();
}
void MainWindow::editClient(bool create) {
    if (!writable(true))
        return;
    std::optional<Client> existing;
    if (!create) {
        existing = selectedClient();
        if (!existing)
            return;
    }
    if (create && inventories_.isEmpty()) {
        report("Сначала загрузите клиентов и входящие подключения серверов.");
        fetchInventories();
        return;
    }
    ClientDialog dialog(servers_, inventories_, existing, this, activeMasterServerId());
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!create) {
        const auto changes = dialog.patch();
        if (!changes.totalBytes && !changes.expiryTime && !changes.enable) {
            report("Изменений нет.");
            return;
        }
    }
    if (create) {
        mutating_ = true;
        setBusyUi();
        provisioner_ = new ClientProvisioner(apis_, this);
        report("Проверка выбранных серверов перед созданием подписки…");
        provisioner_->provision(
            dialog.targets(), dialog.draft(), activeMasterServerId(),
            [this](Outcome<ProvisionSummary> result) {
                provisioner_->deleteLater();
                provisioner_ = nullptr;
                mutating_ = false;
                QString message;
                bool failed = !result.ok;
                if (!result.ok) {
                    message = result.error;
                    recordEvent({}, "Создание подписки", false, message);
                } else {
                    int completed = 0;
                    QStringList details;
                    for (const auto& node : result.value.nodes) {
                        if (node.ok)
                            ++completed;
                        else
                            failed = true;
                        const QString detail = node.ok ? QString::number(node.inboundIds.size()) +
                                                             " инбаундов: подтверждено сервером"
                                                       : node.error;
                        recordEvent(node.serverId, "Создание подписки", node.ok, detail);
                        details.append(serverName(node.serverId) + ": " + detail);
                    }
                    message = "Создано на " + QString::number(completed) + " из " +
                              QString::number(result.value.nodes.size()) + " серверов.";
                    if (failed)
                        message += "\n\n" + details.join('\n') +
                                   "\n\nОбновите список перед повторением: после сетевого сбоя "
                                   "результат на сервере может быть неизвестен.";
                }
                setBusyUi();
                if (failed) {
                    QMessageBox box(QMessageBox::Warning, "Результат создания подписки", message,
                                    QMessageBox::Ok, this);
                    box.setTextFormat(Qt::PlainText);
                    box.exec();
                }
                report(message.section('\n', 0, 0), failed);
                loadInventories();
                poll();
            });
    } else {
        const QString id = dialog.selectedServerId();
        auto* api = apis_.value(id);
        if (!api)
            return;
        mutating_ = true;
        setBusyUi();
        api->updateClient(*existing, dialog.patch(), [this, id](OperationResult r) {
            completeMutation(id, "Изменение подписки", r);
        });
    }
}
void MainWindow::toggleClient() {
    if (!writable(true))
        return;
    auto client = selectedClient();
    if (!client)
        return;
    const QString action = client->enable ? "Отключение подписки" : "Включение подписки";
    if (!confirm(this, action,
                 (client->enable ? "Отключить" : "Включить") + QString(" клиента «") +
                     client->email + "» на «" + serverName(client->serverId) + "»?"))
        return;
    auto* api = apis_.value(client->serverId);
    if (!api)
        return;
    ClientPatch patch;
    patch.enable = !client->enable;
    mutating_ = true;
    setBusyUi();
    api->updateClient(*client, patch, [this, id = client->serverId, action](OperationResult r) {
        completeMutation(id, action, r);
    });
}
void MainWindow::renewClient() {
    if (!writable(true))
        return;
    auto client = selectedClient();
    if (!client)
        return;
    const qint64 base = qMax(client->expiryTime, QDateTime::currentMSecsSinceEpoch());
    const qint64 extension = 30LL * 24 * 3600000;
    if (base > std::numeric_limits<qint64>::max() - extension) {
        report("Этот срок невозможно продлить ещё на 30 дней.", true);
        return;
    }
    const qint64 expiry = base + extension;
    if (!confirm(this, "Продлить подписку?",
                 "Установить срок клиента «" + client->email + "» до " +
                     QDateTime::fromMSecsSinceEpoch(expiry).toString("dd.MM.yyyy HH:mm") + "?"))
        return;
    auto* api = apis_.value(client->serverId);
    if (!api)
        return;
    ClientPatch patch;
    patch.expiryTime = expiry;
    mutating_ = true;
    setBusyUi();
    api->updateClient(*client, patch, [this, id = client->serverId](OperationResult r) {
        completeMutation(id, "Продление подписки", r);
    });
}
void MainWindow::deleteClient() {
    if (!writable(true))
        return;
    auto client = selectedClient();
    if (!client)
        return;
    if (!confirm(this, "Удалить подписку?",
                 "Удалить клиента «" + client->email + "» на сервере «" +
                     serverName(client->serverId) +
                     "»?\nВ 3x-ui 3.x клиент удаляется со всех его входящих подключений на этом "
                     "сервере."))
        return;
    auto* api = apis_.value(client->serverId);
    if (!api)
        return;
    mutating_ = true;
    setBusyUi();
    api->deleteClient(*client, [this, id = client->serverId](OperationResult r) {
        completeMutation(id, "Удаление подписки", r);
    });
}
void MainWindow::resetClient() {
    if (!writable(true))
        return;
    auto client = selectedClient();
    if (!client)
        return;
    if (!confirm(this, "Сбросить трафик?",
                 "Обнулить счётчик трафика клиента «" + client->email + "» на «" +
                     serverName(client->serverId) + "»?"))
        return;
    auto* api = apis_.value(client->serverId);
    if (!api)
        return;
    mutating_ = true;
    setBusyUi();
    api->resetClientTraffic(*client, [this, id = client->serverId](OperationResult r) {
        completeMutation(id, "Сброс трафика", r);
    });
}
void MainWindow::backupServer() {
    if (!writable())
        return;
    const auto* server = selectedServer();
    if (!server)
        return;
    const QString id = server->id, name = server->name;
    auto* api = apis_.value(id);
    if (!api)
        return;
    mutating_ = true;
    setBusyUi();
    report("Загрузка полной базы 3x-ui: " + name + "…");
    api->downloadDatabase([this, id, name](Outcome<QByteArray> downloaded) {
        if (!downloaded.ok) {
            completeMutation(id, "Бэкап 3x-ui", OperationResult::failure(downloaded.error));
            return;
        }
        const auto info = inspectDatabaseBackup(downloaded.value);
        if (!info.ok) {
            completeMutation(id, "Бэкап 3x-ui", OperationResult::failure(info.error));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, "Сохранить базу 3x-ui — файл содержит настройки и ключи клиентов",
            QDir(dataDirectory_).filePath(backupName(name, info.value.extension())),
            info.value.kind == BackupKind::SQLite ? "База 3x-ui (*.db);;Все файлы (*)"
                                                  : "Дамп PostgreSQL (*.dump);;Все файлы (*)");
        if (path.isEmpty()) {
            mutating_ = false;
            setBusyUi();
            report("Сохранение бэкапа отменено.");
            return;
        }
        const auto saved = saveDatabaseBackup(path, downloaded.value);
        mutating_ = false;
        setBusyUi();
        recordEvent(id, "Бэкап 3x-ui", saved.ok,
                    saved.ok ? "Полная база сохранена: " + path : saved.error);
        report(saved.ok ? "База 3x-ui сохранена: " + path : saved.error, !saved.ok);
    });
}
void MainWindow::restoreServer() {
    if (!writable())
        return;
    const auto* server = selectedServer();
    if (!server)
        return;
    const QString id = server->id, name = server->name;
    const QString path =
        QFileDialog::getOpenFileName(this, "Выбрать бэкап базы 3x-ui", dataDirectory_,
                                     "Бэкапы 3x-ui (*.db *.dump);;Все файлы (*)");
    if (path.isEmpty())
        return;
    auto backupContents = readDatabaseBackup(path);
    if (!backupContents.ok) {
        report(backupContents.error, true);
        return;
    }
    const auto info = inspectDatabaseBackup(backupContents.value);
    if (!info.ok) {
        report(info.error, true);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Восстановить базу 3x-ui");
    dialog.setMinimumWidth(570);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto* description = label("Сервер: " + name + "\nБэкап: " + QFileInfo(path).fileName() +
                              "\nРазмер: " + bytes(double(info.value.size)));
    description->setWordWrap(true);
    layout->addWidget(description);
    if (info.value.kind == BackupKind::SQLite)
        layout->addWidget(
            label("Инбаундов: " + QString::number(info.value.inboundCount) + " · клиентов: " +
                      (info.value.clientCount < 0 ? QString("старый формат в инбаундах")
                                                  : QString::number(info.value.clientCount)),
                  "muted"));
    else {
        auto* pg = label("Проверен заголовок дампа PostgreSQL. Полную проверку выполняет "
                         "целевая 3x-ui при импорте.",
                         "muted");
        pg->setWordWrap(true);
        layout->addWidget(pg);
    }
    auto* keep = new QCheckBox("Сохранить адреса, сертификаты и идентификатор целевого сервера");
    keep->setChecked(true);
    layout->addWidget(keep);
    auto* effect = label("Клиенты, инбаунды и остальные настройки базы будут заменены. "
                         "Если снять галочку, адрес и порт панели тоже могут измениться. "
                         "Внешние файлы сертификатов и Geo-файлы нужно переносить отдельно.");
    effect->setWordWrap(true);
    layout->addWidget(effect);
    auto* rollback = label("Перед заменой приложение сохранит текущую базу целевого сервера "
                           "в папку backups рядом с локальными данными. Если это не удастся, "
                           "восстановление не начнётся.",
                           "muted");
    rollback->setWordWrap(true);
    layout->addWidget(rollback);
    auto* actions = new QDialogButtonBox;
    auto* restore = actions->addButton("Заменить базу", QDialogButtonBox::AcceptRole);
    restore->setProperty("role", "danger");
    restore->setAutoDefault(false);
    auto* cancel = actions->addButton("Отмена", QDialogButtonBox::RejectRole);
    cancel->setDefault(true);
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(actions);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const bool keepHostSettings = keep->isChecked();
    if (!writable())
        return;
    const QString rollbackDirectory = QDir(dataDirectory_).filePath("backups");
    if (!QDir().mkpath(rollbackDirectory)) {
        report("Не удалось создать папку для копии текущей базы. Восстановление отменено.", true);
        return;
    }
    QPointer<ThreeXUiApi> api = apis_.value(id);
    if (!api)
        return;
    mutating_ = true;
    setBusyUi();
    report("Сохранение текущей базы «" + name + "» перед заменой…");
    api->downloadDatabase([this, id, name, api, rollbackDirectory, keepHostSettings,
                           restoreData =
                               std::move(backupContents.value)](Outcome<QByteArray> before) {
        if (!before.ok) {
            completeMutation(
                id, "Восстановление 3x-ui",
                OperationResult::failure(
                    "Не удалось получить текущую базу. Восстановление отменено. " + before.error));
            return;
        }
        const auto beforeInfo = inspectDatabaseBackup(before.value);
        if (!beforeInfo.ok) {
            completeMutation(
                id, "Восстановление 3x-ui",
                OperationResult::failure(
                    "Копия текущей базы не прошла проверку. Восстановление отменено. " +
                    beforeInfo.error));
            return;
        }
        const QString rollbackPath =
            QDir(rollbackDirectory).filePath(backupName(name, beforeInfo.value.extension(), true));
        const auto saved = saveDatabaseBackup(rollbackPath, before.value);
        if (!saved.ok) {
            completeMutation(
                id, "Восстановление 3x-ui",
                OperationResult::failure(
                    "Не удалось сохранить текущую базу. Восстановление отменено. " + saved.error));
            return;
        }
        recordEvent(id, "Бэкап перед восстановлением", true, "База сохранена: " + rollbackPath);
        if (!api) {
            completeMutation(
                id, "Восстановление 3x-ui",
                OperationResult::failure(
                    "Подключение закрыто. Текущая база сохранена, импорт не запущен."));
            return;
        }
        report("Восстановление базы 3x-ui на «" + name + "»…");
        api->restoreDatabase(
            restoreData, keepHostSettings,
            [this, id, keepHostSettings, rollbackPath](OperationResult result) {
                // Recreate the session after the API operation has unwound.
                QTimer::singleShot(0, this, [this, id, keepHostSettings, rollbackPath, result] {
                    if (!result.ok) {
                        completeMutation(
                            id, "Восстановление 3x-ui",
                            OperationResult::failure(result.error +
                                                     "\nКопия базы до импорта: " + rollbackPath));
                        return;
                    }
                    delete apis_.take(id);
                    for (const auto& config : servers_)
                        if (config.id == id)
                            apis_.insert(id, new ThreeXUiApi(config, this));
                    inventories_.remove(id);
                    inventoryErrors_.remove(id);
                    latest_.remove(id);
                    mutating_ = false;
                    recordEvent(id, "Восстановление 3x-ui", true,
                                "Импорт подтверждён. Копия предыдущей базы: " + rollbackPath);
                    refreshUi();
                    QMessageBox box(
                        QMessageBox::Information, "База восстановлена",
                        "Сервер подтвердил импорт.\n\nКопия базы до импорта:\n" + rollbackPath +
                            (keepHostSettings
                                 ? "\n\nПароль или токен панели могли измениться. При ошибке входа "
                                   "обновите подключение в разделе Серверы."
                                 : "\n\nАдрес, порт и доступы панели могли измениться. "
                                   "Обновите подключение в разделе Серверы."),
                        QMessageBox::Ok, this);
                    box.setTextFormat(Qt::PlainText);
                    box.exec();
                    report("База восстановлена. Копия до импорта: " + rollbackPath);
                    loadInventories(id);
                    poll();
                });
            });
    });
}
void MainWindow::runServerAction(ServerAction action) {
    if (!writable(true))
        return;
    auto* s = selectedServer();
    if (!s)
        return;
    const QString id = s->id;
    QString version;
    const QString name = action == ServerAction::RestartXray      ? "Перезапуск Xray"
                         : action == ServerAction::UpdateGeofiles ? "Обновление Geo-файлов"
                                                                  : "Обновление Xray";
    if (action == ServerAction::InstallXray) {
        bool ok = false;
        version = QInputDialog::getText(
            this, "Версия Xray", "Укажите точную версию, например 26.9.11:", QLineEdit::Normal, {},
            &ok);
        if (!ok || version.trimmed().isEmpty())
            return;
    }
    if (!confirm(this, name,
                 "Выполнить «" + name + "» на «" + s->name +
                     "»?\nСоединения клиентов могут прерваться."))
        return;
    auto* api = apis_.value(id);
    if (!api)
        return;
    mutating_ = true;
    setBusyUi();
    api->serverAction(action, version.trimmed(),
                      [this, id, name](OperationResult r) { completeMutation(id, name, r); });
}
void MainWindow::recordEvent(const QString& id, const QString& action, bool success,
                             const QString& detail) {
    ActivityEvent event{
        QDateTime::currentDateTimeUtc(), id, serverName(id), action, success, detail};
    if (!demo_ && storageReady_) {
        const auto saved = store_.appendEvent(event);
        if (!saved.ok)
            report(saved.error, true);
    }
    events_.prepend(event);
    refreshEventsTable();
}
void MainWindow::completeMutation(const QString& id, const QString& action,
                                  OperationResult result) {
    mutating_ = false;
    recordEvent(id, action, result.ok, result.ok ? "Сервер подтвердил выполнение" : result.error);
    report(result.ok ? action + ": выполнено." : result.error, !result.ok);
    if (!result.ok)
        QMessageBox::warning(this, "Операция не выполнена", result.error);
    setBusyUi();
    loadInventories(id);
    poll();
}

void MainWindow::setDemo(bool enabled) {
    if (enabled == demo_)
        return;
    if (polling_ || mutating_ || inventoryPending_) {
        report("Дождитесь завершения текущей операции.");
        return;
    }
    if (enabled) {
        realServers_ = servers_;
        realInventories_ = inventories_;
        realInventoryErrors_ = inventoryErrors_;
        demoSaved_ = true;
        demo_ = true;
        generateDemo();
        report("Деморежим. Данные не сохраняются и не отправляются на серверы.");
    } else {
        demo_ = false;
        if (demoSaved_) {
            servers_ = realServers_;
            inventories_ = realInventories_;
            inventoryErrors_ = realInventoryErrors_;
        }
        history_.clear();
        latest_.clear();
        events_.clear();
        reloadData();
        report(servers_.isEmpty() ? "Добавьте первую панель 3x-ui, чтобы начать мониторинг."
                                  : "Подключены ваши серверы.");
    }
    demoBanner_->setVisible(demo_);
    demoButton_->setText(demo_ ? "Выйти из демо" : "Посмотреть демо");
    refreshUi();
    if (!demo_)
        poll();
}
void MainWindow::generateDemo() {
    servers_.clear();
    inventories_.clear();
    inventoryErrors_.clear();
    latest_.clear();
    history_.clear();
    events_.clear();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QStringList names = {"Москва · мастер-нода", "Amsterdam · NL-01", "Helsinki · FI-01",
                               "Warsaw · PL-01",       "Paris · FR-01",     "Stockholm · SE-01"};
    const QStringList places = {"Россия", "Нидерланды", "Финляндия", "Польша", "Франция", "Швеция"};
    for (int n = 0; n < names.size(); ++n) {
        ServerConfig s;
        s.id = "demo-" + QString::number(n);
        s.name = names[n];
        s.location = places[n];
        s.panelUrl = QUrl("https://node-" + QString::number(n + 1) + ".example.com/panel-path/");
        s.subscriptionUrl = QUrl("https://node-" + QString::number(n + 1) + ".example.com/sub/");
        servers_.append(s);
        Inventory inv;
        inv.inbounds.append({s.id, 1, "VLESS Reality", "vless", 443, true});
        inv.inbounds.append({s.id, 2, "VLESS WebSocket", "vless", 8443, true});
        for (int c = 0; c < 5; ++c) {
            Client client;
            client.serverId = s.id;
            client.inboundIds = {1, 2};
            client.id = "example-" + QString::number(c);
            client.email = "client-" + QString::number(c + 1);
            client.subId = "demo-sub-" + QString::number(c);
            client.protocol = "vless";
            client.enable = c != 4;
            client.online = c < 3;
            client.usedBytes = qint64((c + 1) * 8.3 * 1024 * 1024 * 1024);
            client.totalBytes = 100LL * 1024 * 1024 * 1024;
            client.expiryTime = now.addDays(18 + c * 3).toMSecsSinceEpoch();
            inv.clients.append(client);
        }
        inventories_[s.id] = inv;
        for (int m = rangeHours_ * 60; m >= 0; --m) {
            Snapshot v;
            v.serverId = s.id;
            v.at = now.addSecs(-m * 60);
            v.health = Health::Online;
            v.cpu = 18 + n * 4 + 8 * std::sin(m / 17.0 + n);
            v.memoryPercent = 32 + n * 4;
            v.diskPercent = 24 + n * 5;
            v.online = int(90 + n * 15 + 25 * std::sin(m / 35.0 + n));
            v.rxBps = (7 + n * 2.0 + 2 * std::sin(m / 19.0)) * 1024 * 1024;
            v.txBps = (12 + n * 3.0 + 4 * std::sin(m / 19.0)) * 1024 * 1024;
            v.receivedBytes = 180LL * 1024 * 1024 * 1024;
            v.sentBytes = 270LL * 1024 * 1024 * 1024;
            v.latencyMs = 34 + n * 7;
            v.panelVersion = "3.7.0";
            v.xrayVersion = "26.8.12";
            v.uptimeSeconds = 1209600;
            if (n == 2 && m < 43 && m > 28) {
                v.health = Health::Offline;
                v.online.reset();
                v.cpu.reset();
                v.rxBps.reset();
                v.txBps.reset();
                v.error = "Пример: таймаут подключения";
            }
            if (n == 4 && m < 15) {
                v.health = Health::Warning;
                v.cpu = 88.5;
                v.error = "Пример: Xray требует внимания";
            }
            history_.append(v);
            if (m == 0)
                latest_[s.id] = v;
        }
    }
    std::sort(history_.begin(), history_.end(),
              [](const auto& a, const auto& b) { return a.at < b.at; });
    events_.append({now.addSecs(-120), servers_[0].id, servers_[0].name, "Создание подписки", true,
                    "Пример: client-31"});
    events_.append({now.addSecs(-850), servers_[4].id, servers_[4].name, "Мониторинг", false,
                    "Пример: Xray требует внимания"});
    events_.append({now.addSecs(-1700), servers_[2].id, servers_[2].name, "Связь восстановлена",
                    true, "Пример события"});
}
} // namespace fleet
