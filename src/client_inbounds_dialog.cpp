#include "client_inbounds_dialog.h"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace fleet {
namespace {
constexpr int boundRole = Qt::UserRole + 1;
constexpr int eligibleRole = Qt::UserRole + 2;
} // namespace
ClientInboundsDialog::ClientInboundsDialog(const ServerConfig& server, const Inventory& inventory,
                                           const Client& client, bool canAttach, QWidget* parent)
    : QDialog(parent), canAttach_(canAttach), attachedCount_(0) {
    setWindowTitle(tr("Инбаунды клиента"));
    resize(850, 560);
    setMinimumSize(600, 410);
    auto* layout = new QVBoxLayout(this);
    auto* caption = new QLabel(tr("%1 · панель: %2").arg(client.email, server.name), this);
    caption->setTextFormat(Qt::PlainText);
    caption->setWordWrap(true);
    layout->addWidget(caption);
    auto* hint = new QLabel(
        canAttach ? tr("Подключённые инбаунды уже отмечены. Выберите новые для этой подписки.")
                  : tr("Просмотр текущих привязок. Добавление доступно для подключённой панели "
                       "3x-ui 3.x."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName("clientInboundsTree");
    tree_->setHeaderLabels({tr("Инбаунд / протокол / порт"), tr("Состояние")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    QSet<int> shown;
    for (const auto& inbound : inventory.inbounds) {
        if (shown.contains(inbound.id))
            continue;
        shown.insert(inbound.id);
        const bool bound = client.inboundIds.contains(inbound.id);
        const auto protocol = inbound.protocol.toLower();
        const bool supported = protocol == "vless" || protocol == "vmess" || protocol == "trojan" ||
                               protocol == "hysteria";
        const bool eligible = !bound && canAttach && inbound.enable && supported;
        auto* row = new QTreeWidgetItem(tree_);
        row->setText(
            0, tr("№%1 · %2 · %3 · порт %4")
                   .arg(inbound.id)
                   .arg(inbound.remark, protocol == "hysteria" ? "HYSTERIA 2" : protocol.toUpper())
                   .arg(inbound.port));
        row->setText(1, bound ? (inbound.enable ? tr("Подключён") : tr("Подключён · выключен"))
                        : !inbound.enable ? tr("Инбаунд выключен")
                        : !supported      ? tr("Протокол не поддерживается")
                        : canAttach       ? tr("Можно подключить")
                                          : tr("Только просмотр"));
        row->setData(0, Qt::UserRole, inbound.id);
        row->setData(0, boundRole, bound);
        row->setData(0, eligibleRole, eligible);
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                      (eligible ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
        row->setCheckState(0, bound ? Qt::Checked : Qt::Unchecked);
        attachedCount_ += bound;
    }
    for (const int id : client.inboundIds) {
        if (shown.contains(id))
            continue;
        shown.insert(id);
        auto* row = new QTreeWidgetItem(tree_);
        row->setText(0, tr("Инбаунд №%1").arg(id));
        row->setText(1, tr("Привязан · отсутствует в списке панели"));
        row->setData(0, Qt::UserRole, id);
        row->setData(0, boundRole, true);
        row->setData(0, eligibleRole, false);
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        row->setCheckState(0, Qt::Checked);
        ++attachedCount_;
    }
    layout->addWidget(tree_, 1);
    auto* select = new QHBoxLayout;
    auto* all = new QPushButton(tr("Выбрать доступные"), this);
    auto* clear = new QPushButton(tr("Снять новые"), this);
    all->setObjectName("selectAvailableInbounds");
    clear->setObjectName("clearNewInbounds");
    all->setEnabled(canAttach);
    clear->setEnabled(canAttach);
    select->addWidget(all);
    select->addWidget(clear);
    select->addStretch();
    layout->addLayout(select);
    count_ = new QLabel(this);
    layout->addWidget(count_);
    auto* footer = new QLabel(tr("Текущие привязки, subId и существующие ключи сохраняются. "
                                 "После подключения подписка получит новые конфигурации."),
                              this);
    footer->setWordWrap(true);
    layout->addWidget(footer);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    attach_ = buttons->button(QDialogButtonBox::Ok);
    attach_->setObjectName("attachInboundsButton");
    attach_->setText(tr("Подключить выбранные"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Закрыть"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ClientInboundsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    const auto setNew = [this](Qt::CheckState state) {
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* row = tree_->topLevelItem(i);
            if (row->data(0, eligibleRole).toBool())
                row->setCheckState(0, state);
        }
    };
    connect(all, &QPushButton::clicked, this, [setNew] { setNew(Qt::Checked); });
    connect(clear, &QPushButton::clicked, this, [setNew] { setNew(Qt::Unchecked); });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (item->data(0, boundRole).toBool() && item->checkState(0) != Qt::Checked) {
            const QSignalBlocker block(tree_);
            item->setCheckState(0, Qt::Checked);
        }
        refreshSelection();
    });
    refreshSelection();
}
QList<int> ClientInboundsDialog::addedInboundIds() const {
    QList<int> result;
    if (!canAttach_)
        return result;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const auto* item = tree_->topLevelItem(i);
        if (item->data(0, eligibleRole).toBool() && item->checkState(0) == Qt::Checked)
            result.append(item->data(0, Qt::UserRole).toInt());
    }
    return result;
}
void ClientInboundsDialog::refreshSelection() {
    const auto ids = addedInboundIds();
    count_->setText(tr("Подключено: %1 · выбрано новых: %2").arg(attachedCount_).arg(ids.size()));
    attach_->setEnabled(!ids.isEmpty());
}
void ClientInboundsDialog::accept() {
    if (!addedInboundIds().isEmpty())
        QDialog::accept();
}
} // namespace fleet
