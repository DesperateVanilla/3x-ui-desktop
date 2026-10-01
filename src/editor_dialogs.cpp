#include "editor_dialogs.h"
#include "three_x_ui_api.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>

namespace fleet {
namespace {
constexpr qint64 bytesPerGb = 1024LL * 1024LL * 1024LL;
constexpr double maximumQuotaGb = 1000000.0;
constexpr int protocolRole = Qt::UserRole + 1;

QLabel* helperLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setProperty("secondary", true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel* errorLabel(QWidget* parent) {
    auto* label = helperLabel({}, parent);
    label->setObjectName(QStringLiteral("validationError"));
    label->setAccessibleName(QStringLiteral("Ошибка проверки"));
    label->setStyleSheet(QStringLiteral("color: #e87575;"));
    label->hide();
    return label;
}

void reportError(QLabel* label, const QString& message, QWidget* focus) {
    label->setText(message);
    label->show();
    if (focus)
        focus->setFocus(Qt::OtherFocusReason);
}

void setFieldVisible(QFormLayout* form, QWidget* field, bool visible) {
    field->setVisible(visible);
    if (auto* label = form->labelForField(field))
        label->setVisible(visible);
}

bool supportedProtocol(const QString& protocol) {
    const QString normalized = protocol.toLower();
    return normalized == QStringLiteral("vless") || normalized == QStringLiteral("vmess") ||
           normalized == QStringLiteral("trojan");
}

QString inboundTitle(const Inbound& inbound) {
    const QString title =
        inbound.remark.isEmpty() ? QStringLiteral("Inbound №%1").arg(inbound.id) : inbound.remark;
    return QStringLiteral("%1 · %2 · порт %3")
        .arg(title, inbound.protocol.toUpper())
        .arg(inbound.port);
}

bool validEmail(const QString& email) {
    if (email.isEmpty() || email.size() > 128)
        return false;
    for (const QChar character : email) {
        if (character == QLatin1Char('/') || character == QLatin1Char('?') ||
            character == QLatin1Char('#') || character.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

QUrl subscriptionUrl(const QString& text) {
    return QUrl(text, QUrl::StrictMode);
}

bool validSubscriptionUrl(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty() &&
           (scheme == QStringLiteral("https") || scheme == QStringLiteral("http")) &&
           url.userInfo().isEmpty() &&
           !url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) && !url.hasQuery() &&
           !url.hasFragment();
}
} // namespace

ServerDialog::ServerDialog(const std::optional<ServerConfig>& existing, QWidget* parent)
    : QDialog(parent), result_(existing.value_or(ServerConfig{})) {
    const bool editing = existing.has_value();
    result_.twoFactorCode.clear();
    setWindowTitle(editing ? tr("Редактировать сервер") : tr("Добавить сервер"));
    setMinimumWidth(540);
    resize(560, 660);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addWidget(helperLabel(tr("Подключение к панели 3x-ui. Укажите полный адрес, "
                                     "включая скрытый путь панели."),
                                  this));
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setSpacing(10);
    layout->addLayout(form);

    auto* name = new QLineEdit(this);
    name->setObjectName(QStringLiteral("serverName"));
    name->setPlaceholderText(tr("Например, Москва — основной"));
    name->setText(result_.name);
    form->addRow(tr("&Название:"), name);

    auto* location = new QLineEdit(this);
    location->setObjectName(QStringLiteral("serverLocation"));
    location->setPlaceholderText(tr("Город или дата-центр, необязательно"));
    location->setText(result_.location);
    form->addRow(tr("&Расположение:"), location);

    auto* panel = new QLineEdit(this);
    panel->setObjectName(QStringLiteral("panelUrl"));
    panel->setPlaceholderText(QStringLiteral("https://host:2053/secret-base-path/"));
    panel->setText(result_.panelUrl.toString(QUrl::FullyEncoded));
    form->addRow(tr("Адрес &панели:"), panel);

    auto* subscription = new QLineEdit(this);
    subscription->setObjectName(QStringLiteral("subscriptionUrl"));
    subscription->setPlaceholderText(tr("Базовый адрес подписок, необязательно"));
    subscription->setText(result_.subscriptionUrl.toString(QUrl::FullyEncoded));
    form->addRow(tr("Адрес подпи&сок:"), subscription);
    form->addRow(helperLabel(tr("Для подписок нужен базовый HTTP(S)-адрес без логина, "
                                "параметров запроса и фрагмента."),
                             this));

    auto* allowHttp = new QCheckBox(tr("Разрешить HTTP для панели (без шифрования)"), this);
    allowHttp->setObjectName(QStringLiteral("allowHttp"));
    allowHttp->setChecked(result_.allowHttp);
    form->addRow(allowHttp);

    auto* api = new QComboBox(this);
    api->setObjectName(QStringLiteral("apiFlavor"));
    api->addItem(tr("Автоматически"), static_cast<int>(ApiFlavor::Auto));
    api->addItem(tr("3x-ui 3.x"), static_cast<int>(ApiFlavor::ModernV3));
    api->addItem(tr("3x-ui 2.x"), static_cast<int>(ApiFlavor::LegacyV2));
    api->setCurrentIndex(api->findData(static_cast<int>(result_.api)));
    form->addRow(tr("Версия &API:"), api);

    auto* auth = new QComboBox(this);
    auth->setObjectName(QStringLiteral("authKind"));
    auth->addItem(tr("API-токен"), static_cast<int>(AuthKind::Token));
    auth->addItem(tr("Логин и пароль"), static_cast<int>(AuthKind::Password));
    auth->setCurrentIndex(auth->findData(static_cast<int>(result_.auth)));
    form->addRow(tr("&Авторизация:"), auth);

    auto* token = new QLineEdit(this);
    token->setObjectName(QStringLiteral("token"));
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(editing && !result_.token.isEmpty()
                                  ? tr("Оставьте пустым, чтобы сохранить токен")
                                  : tr("API-токен панели"));
    form->addRow(tr("&Токен:"), token);

    auto* username = new QLineEdit(this);
    username->setObjectName(QStringLiteral("username"));
    username->setText(result_.username);
    form->addRow(tr("&Логин:"), username);

    auto* password = new QLineEdit(this);
    password->setObjectName(QStringLiteral("password"));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(editing && !result_.password.isEmpty()
                                     ? tr("Оставьте пустым, чтобы сохранить пароль")
                                     : tr("Пароль панели"));
    form->addRow(tr("Па&роль:"), password);

    auto* twoFactor = new QLineEdit(this);
    twoFactor->setObjectName(QStringLiteral("twoFactorCode"));
    twoFactor->setEchoMode(QLineEdit::Password);
    twoFactor->setPlaceholderText(tr("Одноразовый код, если включена 2FA"));
    form->addRow(tr("Код &2FA:"), twoFactor);
    auto* twoFactorHelp = helperLabel(tr("Код 2FA используется только для входа "
                                         "и не сохраняется."),
                                      this);
    form->addRow(twoFactorHelp);

    const auto updateAuthentication = [=] {
        const bool passwordAuth =
            auth->currentData().toInt() == static_cast<int>(AuthKind::Password);
        setFieldVisible(form, token, !passwordAuth);
        setFieldVisible(form, username, passwordAuth);
        setFieldVisible(form, password, passwordAuth);
        setFieldVisible(form, twoFactor, passwordAuth);
        twoFactorHelp->setVisible(passwordAuth);
    };
    connect(auth, &QComboBox::currentIndexChanged, this,
            [updateAuthentication](int) { updateAuthentication(); });
    updateAuthentication();

    auto* error = errorLabel(this);
    layout->addWidget(error);
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? tr("Сохранить") : tr("Добавить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [=] {
        ServerConfig candidate = existing.value_or(ServerConfig{});
        candidate.twoFactorCode.clear();
        candidate.name = name->text().trimmed();
        candidate.location = location->text().trimmed();
        if (candidate.name.isEmpty()) {
            reportError(error, tr("Введите название сервера."), name);
            return;
        }

        candidate.panelUrl = QUrl(panel->text().trimmed(), QUrl::StrictMode);
        candidate.allowHttp = allowHttp->isChecked();
        const auto validatedUrl = ThreeXUiApi::validatePanelUrl(candidate);
        if (!validatedUrl.ok) {
            reportError(error, validatedUrl.error, panel);
            return;
        }
        candidate.panelUrl = validatedUrl.value;

        const QString subscriptionText = subscription->text().trimmed();
        candidate.subscriptionUrl =
            subscriptionText.isEmpty() ? QUrl() : subscriptionUrl(subscriptionText);
        if (!subscriptionText.isEmpty() && !validSubscriptionUrl(candidate.subscriptionUrl)) {
            reportError(error,
                        tr("Адрес подписок должен быть полным HTTP(S)-адресом "
                           "без логина, параметров запроса и фрагмента."),
                        subscription);
            return;
        }

        candidate.auth = static_cast<AuthKind>(auth->currentData().toInt());
        candidate.api = static_cast<ApiFlavor>(api->currentData().toInt());
        candidate.username = username->text().trimmed();
        if (!token->text().trimmed().isEmpty())
            candidate.token = token->text().trimmed();
        if (!password->text().isEmpty())
            candidate.password = password->text();
        if (candidate.auth == AuthKind::Token) {
            if (candidate.token.isEmpty()) {
                reportError(error, tr("Введите API-токен панели."), token);
                return;
            }
        } else {
            if (candidate.username.isEmpty()) {
                reportError(error, tr("Введите логин панели."), username);
                return;
            }
            if (candidate.password.isEmpty()) {
                reportError(error, tr("Введите пароль панели."), password);
                return;
            }
            candidate.twoFactorCode = twoFactor->text().trimmed();
        }
        if (candidate.id.isEmpty())
            candidate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        result_ = std::move(candidate);
        accept();
    });
    name->setFocus(Qt::OtherFocusReason);
}

ServerConfig ServerDialog::result() const {
    return result_;
}

ClientDialog::ClientDialog(const QList<ServerConfig>& servers,
                           const QHash<QString, Inventory>& inventories,
                           const std::optional<Client>& existing, QWidget* parent)
    : QDialog(parent) {
    const bool editing = existing.has_value();
    setWindowTitle(editing ? tr("Редактировать клиента") : tr("Добавить клиента"));
    setMinimumWidth(540);
    resize(560, 540);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addWidget(
        helperLabel(editing ? tr("Измените лимит трафика, срок действия или состояние клиента.")
                            : tr("Выберите сервер и активный inbound VLESS, VMess или Trojan."),
                    this));
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setSpacing(10);
    layout->addLayout(form);

    auto* server = new QComboBox(this);
    server->setObjectName(QStringLiteral("server"));
    server->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    server->setMinimumContentsLength(24);
    for (const auto& config : servers) {
        QString title = config.name.isEmpty() ? config.panelUrl.host() : config.name;
        if (!config.location.isEmpty())
            title += QStringLiteral(" · ") + config.location;
        server->addItem(title, config.id);
    }
    if (editing) {
        int selected = server->findData(existing->serverId);
        if (selected < 0) {
            server->addItem(tr("Сервер недоступен"), existing->serverId);
            selected = server->count() - 1;
        }
        server->setCurrentIndex(selected);
        serverId_ = existing->serverId;
    } else if (server->count() == 0) {
        server->addItem(tr("Нет добавленных серверов"));
        server->setEnabled(false);
    }
    server->setEnabled(!editing && !servers.isEmpty());
    form->addRow(tr("&Сервер:"), server);

    auto* inbound = new QComboBox(this);
    inbound->setObjectName(QStringLiteral("inbound"));
    inbound->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    inbound->setMinimumContentsLength(24);
    form->addRow(tr("&Inbound:"), inbound);
    auto* inboundHelp = helperLabel({}, this);
    form->addRow(inboundHelp);

    auto* email = new QLineEdit(this);
    email->setObjectName(QStringLiteral("email"));
    email->setPlaceholderText(tr("Например, alex-phone"));
    if (editing) {
        email->setText(existing->email);
        email->setReadOnly(true);
    } else {
        email->setMaxLength(128);
    }
    form->addRow(tr("&Email / имя:"), email);
    if (!editing) {
        form->addRow(helperLabel(tr("До 128 символов; без /, ?, # и управляющих символов."), this));
    }

    auto* quota = new QDoubleSpinBox(this);
    quota->setObjectName(QStringLiteral("quotaGB"));
    quota->setRange(0.0, maximumQuotaGb);
    quota->setDecimals(2);
    quota->setSingleStep(1.0);
    quota->setSuffix(tr(" ГБ"));
    quota->setKeyboardTracking(false);
    quota->setValue(editing ? static_cast<double>(existing->totalBytes) / bytesPerGb : 0.0);
    form->addRow(tr("Лимит &трафика:"), quota);
    QString quotaHelpText = tr("0 ГБ — без ограничения. 1 ГБ = 1024³ байт.");
    if (editing && static_cast<double>(existing->totalBytes) / bytesPerGb > maximumQuotaGb) {
        quotaHelpText += tr(" Текущий лимит: %1 байт. Он превышает диапазон поля "
                            "и сохранится, пока вы не измените поле.")
                             .arg(existing->totalBytes);
    } else if (editing && existing->totalBytes != qRound64(quota->value() * bytesPerGb)) {
        quotaHelpText += tr(" Точное исходное значение сохранится, пока вы не измените поле.");
    }
    form->addRow(helperLabel(quotaHelpText, this));
    auto quotaEdited = std::make_shared<bool>(false);
    connect(quota, &QDoubleSpinBox::valueChanged, this,
            [quotaEdited](double) { *quotaEdited = true; });
    if (auto* input = quota->findChild<QLineEdit*>()) {
        connect(input, &QLineEdit::textEdited, this,
                [quotaEdited](const QString&) { *quotaEdited = true; });
    }

    const bool relativeExpiry = editing && existing->expiryTime < 0;
    auto* overrideRelativeExpiry = new QCheckBox(tr("Задать новый срок действия"), this);
    overrideRelativeExpiry->setObjectName(QStringLiteral("overrideRelativeExpiry"));
    if (relativeExpiry) {
        const double days = -static_cast<double>(existing->expiryTime) / 86400000.0;
        form->addRow(helperLabel(tr("Текущий срок: %1 дней после первого использования. "
                                    "Он сохранится, пока вы не зададите новый срок.")
                                     .arg(QString::number(days, 'g', 10)),
                                 this));
        form->addRow(overrideRelativeExpiry);
    } else {
        overrideRelativeExpiry->hide();
    }

    auto* unlimited = new QCheckBox(tr("Без срока действия"), this);
    unlimited->setObjectName(QStringLiteral("unlimitedExpiry"));
    unlimited->setChecked(!editing || existing->expiryTime == 0);
    form->addRow(unlimited);

    auto* expiry = new QDateTimeEdit(this);
    expiry->setObjectName(QStringLiteral("expiryTime"));
    expiry->setTimeZone(QTimeZone::UTC);
    expiry->setCalendarPopup(true);
    expiry->setKeyboardTracking(false);
    expiry->setDisplayFormat(QStringLiteral("dd.MM.yyyy HH:mm:ss 'UTC'"));
    expiry->setMinimumDate(QDate(1970, 1, 1));
    const QDateTime originalDate =
        editing && existing->expiryTime > 0
            ? QDateTime::fromMSecsSinceEpoch(existing->expiryTime, QTimeZone::UTC)
            : QDateTime::currentDateTimeUtc().addMonths(1);
    expiry->setDateTime(originalDate);
    form->addRow(tr("&Действует до:"), expiry);
    form->addRow(helperLabel(tr("Дата и время указаны в UTC."), this));
    if (editing && existing->expiryTime > 0 && expiry->dateTime() != originalDate) {
        form->addRow(helperLabel(tr("Исходный срок выходит за диапазон календаря "
                                    "и сохранится, пока вы его не измените."),
                                 this));
    }
    auto expiryEdited = std::make_shared<bool>(false);
    connect(expiry, &QDateTimeEdit::dateTimeChanged, this,
            [expiryEdited](const QDateTime&) { *expiryEdited = true; });
    if (auto* input = expiry->findChild<QLineEdit*>()) {
        connect(input, &QLineEdit::textEdited, this,
                [expiryEdited](const QString&) { *expiryEdited = true; });
    }
    const auto updateExpiryControls = [=] {
        const bool allowChange = !relativeExpiry || overrideRelativeExpiry->isChecked();
        unlimited->setEnabled(allowChange);
        expiry->setEnabled(allowChange && !unlimited->isChecked());
    };
    connect(unlimited, &QCheckBox::toggled, this, [=](bool) {
        *expiryEdited = true;
        updateExpiryControls();
    });
    connect(overrideRelativeExpiry, &QCheckBox::toggled, this,
            [updateExpiryControls](bool) { updateExpiryControls(); });
    updateExpiryControls();

    auto* enabled = new QCheckBox(tr("Клиент включён"), this);
    enabled->setObjectName(QStringLiteral("enabled"));
    enabled->setChecked(!editing || existing->enable);
    form->addRow(enabled);

    auto* flow = new QComboBox(this);
    flow->setObjectName(QStringLiteral("flow"));
    flow->addItem(tr("Без flow"), QString());
    flow->addItem(QStringLiteral("xtls-rprx-vision"), QStringLiteral("xtls-rprx-vision"));
    form->addRow(tr("&Flow (VLESS):"), flow);
    auto* flowHelp =
        helperLabel(tr("Vision должен поддерживаться настройками выбранного inbound."), this);
    form->addRow(flowHelp);

    auto* error = errorLabel(this);
    layout->addWidget(error);
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* save = buttons->button(QDialogButtonBox::Ok);
    save->setText(editing ? tr("Сохранить") : tr("Добавить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const auto updateFlow = [=] {
        const bool vless =
            !editing && inbound->currentData(protocolRole).toString() == QStringLiteral("vless");
        if (!vless) {
            const QSignalBlocker blocker(flow);
            flow->setCurrentIndex(0);
        }
        setFieldVisible(form, flow, vless);
        flowHelp->setVisible(vless);
        flow->setEnabled(vless);
    };

    if (editing) {
        const Inventory inventory = inventories.value(existing->serverId);
        QStringList titles;
        for (int id : existing->inboundIds) {
            QString title = tr("Inbound №%1").arg(id);
            for (const auto& item : inventory.inbounds) {
                if (item.id == id) {
                    title = inboundTitle(item);
                    break;
                }
            }
            titles.append(title);
        }
        inbound->addItem(titles.isEmpty() ? tr("Inbound недоступен")
                                          : titles.join(QStringLiteral("; ")),
                         existing->inboundIds.isEmpty() ? 0 : existing->inboundIds.first());
        inbound->setEnabled(false);
        inboundHelp->setText(
            existing->inboundIds.size() > 1
                ? tr("Изменения применятся к клиенту на всех его inbound на этом сервере.")
                : tr("Сервер, inbound и имя клиента сохраняются."));
        updateFlow();
    } else {
        const auto populateInbounds = [=] {
            const QSignalBlocker blocker(inbound);
            inbound->clear();
            const auto inventory = inventories.constFind(server->currentData().toString());
            if (inventory != inventories.constEnd()) {
                for (const auto& item : inventory->inbounds) {
                    if (!item.enable || item.id <= 0 || !supportedProtocol(item.protocol))
                        continue;
                    inbound->addItem(inboundTitle(item), item.id);
                    inbound->setItemData(inbound->count() - 1, item.protocol.toLower(),
                                         protocolRole);
                }
            }
            const bool available = inbound->count() > 0;
            if (!available)
                inbound->addItem(tr("Нет доступных inbound"));
            inbound->setEnabled(available);
            save->setEnabled(available && !server->currentData().toString().isEmpty());
            inboundHelp->setText(
                available
                    ? tr("Показаны только включённые VLESS, VMess и Trojan inbound.")
                    : tr("Обновите список сервера или включите совместимый inbound в панели."));
            updateFlow();
        };
        connect(server, &QComboBox::currentIndexChanged, this,
                [populateInbounds](int) { populateInbounds(); });
        connect(inbound, &QComboBox::currentIndexChanged, this,
                [updateFlow](int) { updateFlow(); });
        populateInbounds();
    }

    connect(buttons, &QDialogButtonBox::accepted, this, [=] {
        quota->interpretText();
        if (!editing || *expiryEdited || (relativeExpiry && overrideRelativeExpiry->isChecked()))
            expiry->interpretText();
        const QString chosenServer =
            editing ? existing->serverId : server->currentData().toString();
        const int chosenInbound =
            editing ? (existing->inboundIds.isEmpty() ? 0 : existing->inboundIds.first())
                    : inbound->currentData().toInt();
        const QString chosenEmail = editing ? existing->email : email->text().trimmed();
        if (!editing && (chosenServer.isEmpty() || chosenInbound <= 0 ||
                         !supportedProtocol(inbound->currentData(protocolRole).toString()))) {
            reportError(error, tr("Выберите сервер и активный совместимый inbound."), inbound);
            return;
        }
        if (!editing && !validEmail(chosenEmail)) {
            reportError(error,
                        tr("Введите имя длиной до 128 символов без /, ?, # "
                           "и управляющих символов."),
                        email);
            return;
        }
        const bool changeExpiry =
            !editing || (relativeExpiry ? overrideRelativeExpiry->isChecked() : *expiryEdited);
        const qint64 desiredExpiry =
            unlimited->isChecked() ? 0 : expiry->dateTime().toUTC().toMSecsSinceEpoch();
        if (changeExpiry && !unlimited->isChecked() &&
            (!expiry->dateTime().isValid() || desiredExpiry <= 0)) {
            reportError(error,
                        tr("Укажите корректную дату UTC после 1 января 1970 года "
                           "или выберите срок без ограничения."),
                        expiry);
            return;
        }
        const qint64 desiredQuota = qRound64(quota->value() * bytesPerGb);
        serverId_ = chosenServer;
        draft_.inboundId = chosenInbound;
        draft_.email = chosenEmail;
        draft_.totalBytes = editing && !*quotaEdited ? existing->totalBytes : desiredQuota;
        draft_.expiryTime = editing && !changeExpiry ? existing->expiryTime : desiredExpiry;
        draft_.enable = enabled->isChecked();
        draft_.flow =
            editing ? existing->raw.value(QStringLiteral("flow")).toString()
                    : (inbound->currentData(protocolRole).toString() == QStringLiteral("vless")
                           ? flow->currentData().toString()
                           : QString());
        patch_ = {};
        if (editing) {
            if (*quotaEdited && desiredQuota != existing->totalBytes)
                patch_.totalBytes = desiredQuota;
            if (changeExpiry && desiredExpiry != existing->expiryTime)
                patch_.expiryTime = desiredExpiry;
            if (enabled->isChecked() != existing->enable)
                patch_.enable = enabled->isChecked();
        }
        accept();
    });
    if (editing)
        quota->setFocus(Qt::OtherFocusReason);
    else
        email->setFocus(Qt::OtherFocusReason);
}

QString ClientDialog::selectedServerId() const {
    return serverId_;
}

ClientDraft ClientDialog::draft() const {
    return draft_;
}

ClientPatch ClientDialog::patch() const {
    return patch_;
}
} // namespace fleet
