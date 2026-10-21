#include "window.h"
#include "directoryitem.h"
#include "transfer.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QStyledItemDelegate>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
class QueueProgressDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(2, 2, -2, -2);
        bar.state = option.state;
        bar.palette = option.palette;
        bar.minimum = 0;
        bar.maximum = 1000;
        bar.progress = index.data(Qt::UserRole).toInt();
        bar.text = index.data(Qt::DisplayRole).toString();
        bar.textVisible = true;
        bar.textAlignment = Qt::AlignCenter;
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter);
    }
};
QString uid() {
    return QUuid::createUuid().toString(QUuid::Id128);
}
QString bytes(double size) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (size >= 1024 && unit < 4) {
        size /= 1024;
        ++unit;
    }
    return QString::number(size, 'f', unit ? 1 : 0) + " " + units[unit];
}
QPushButton *button(const QString &text, QBoxLayout *layout, std::function<void()> action) {
    auto b = new QPushButton(text);
    layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, std::move(action));
    return b;
}
bool terminal(const QString &state) {
    return state == "Completed" || state == "Failed" || state == "Cancelled";
}
void selectContextItem(QTreeWidget *tree, const QPoint &point) {
    auto item = tree->itemAt(point);
    if (!item)
        return;
    if (!item->isSelected())
        tree->clearSelection();
    tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
    item->setSelected(true);
}
} // namespace
Window::Window() {
    setWindowTitle("LinFXP — Remote file transfers");
    resize(1200, 780);
    configPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/state.json";
    QFile state(configPath);
    QJsonObject paneSelections;
    if (state.exists()) {
        if (!state.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read LinFXP state");
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(state.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error(
                "LinFXP state is invalid; preserve state.json before repairing it");
        auto obj = document.object();
        for (auto value : obj["profiles"].toArray())
            profiles.append(Profile::fromJson(value.toObject()));
        jobs = obj["jobs"].toArray();
        paneSelections = obj["panes"].toObject();
    }
    auto central = new QWidget;
    auto layout = new QVBoxLayout(central);
    layout->setContentsMargins(18, 18, 18, 12);
    auto heading = new QHBoxLayout;
    auto title = new QLabel("LinFXP");
    QFont font = title->font();
    font.setPointSize(22);
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title);
    heading->addStretch();
    auto compare = button("Compare", heading, [this] { compareDirectories(); });
    compare->setObjectName("compareDirectories");
    compare->setToolTip("Highlight names missing from the other displayed listing; case-sensitive, "
                        "without recursion or size/content checks");
    button("Connections…", heading, [this] {
        manageConnections();
    })->setObjectName("manageConnections");
    layout->addLayout(heading);
    layout->addSpacing(8);
    auto vertical = new QSplitter(Qt::Vertical);
    auto horizontal = new QSplitter(Qt::Horizontal);
    horizontal->addWidget(makePane(source, "SOURCE"));
    horizontal->addWidget(makePane(destination, "DESTINATION"));
    vertical->addWidget(horizontal);
    auto queuePanel = new QWidget;
    auto qlayout = new QVBoxLayout(queuePanel);
    qlayout->setContentsMargins(0, 8, 0, 0);
    auto actions = new QHBoxLayout;
    button("Queue selected →", actions, [this] { addToQueue(); });
    button("Start queue", actions, [this] { startQueue(); });
    actions->addStretch();
    button("Remove selected", actions, [this] {
        removeSelected();
    })->setObjectName("removeQueueItems");
    button("Retry / resume", actions, [this] {
        retrySelected();
    })->setToolTip("Retry the selected transfer batches, preserving completed and removed files");
    button("Cancel batch", actions, [this] { cancelSelected(); });
    button("Clear finished", actions, [this] { clearFinished(); });
    qlayout->addLayout(actions);
    queue = new QTreeWidget;
    queue->setHeaderLabels({"Transfer", "Destination", "Status", "Progress", "Size", "Method"});
    queue->setRootIsDecorated(false);
    queue->setAlternatingRowColors(true);
    queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    queue->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    queue->setColumnWidth(1, 180);
    queue->setColumnWidth(2, 150);
    queue->setColumnWidth(3, 210);
    queue->setColumnWidth(4, 100);
    queue->setItemDelegateForColumn(3, new QueueProgressDelegate(queue));
    connect(queue, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { showTransferDetails(); });
    queue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(queue, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        selectContextItem(queue, point);
        QMenu menu(this);
        queueActions(menu);
        menu.exec(queue->viewport()->mapToGlobal(point));
    });
    qlayout->addWidget(queue);
    summary = new QLabel;
    qlayout->addWidget(summary);
    vertical->addWidget(queuePanel);
    vertical->setSizes({440, 260});
    layout->addWidget(vertical);
    setCentralWidget(central);
    refreshProfiles();
    for (Pane *p : {&source, &destination}) {
        QString id = paneSelections[p == &source ? "source" : "destination"].toString();
        int index = p->profile->findData(id);
        if (index >= 0)
            p->profile->setCurrentIndex(index);
        if (p->profile->currentData().toString() == "__local__") {
            auto path =
                paneSelections[p == &source ? "source_local_path" : "destination_local_path"]
                    .toString();
            if (!path.isEmpty())
                p->path->setText(path);
        }
    }
    renderQueue();
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    timer.start(4000);
}
QWidget *Window::makePane(Pane &p, const QString &title) {
    auto box = new QWidget;
    auto layout = new QVBoxLayout(box);
    layout->setContentsMargins(4, 4, 4, 4);
    auto label = new QLabel(title);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    layout->addWidget(label);
    auto row = new QHBoxLayout;
    p.profile = new QComboBox;
    QString role = &p == &source ? "source" : "destination";
    p.profile->setObjectName(role + "Profiles");
    row->addWidget(p.profile, 1);
    button("Connect", row, [this, &p] { browse(p); });
    layout->addLayout(row);
    auto pathrow = new QHBoxLayout;
    button("↑", pathrow, [this, &p] {
        if (p.loadedPath.isEmpty())
            return;
        QString path = p.loadedPath;
        int pos = path.lastIndexOf('/');
        browse(p, pos <= 0 ? "/" : path.left(pos));
    })->setToolTip("Parent folder");
    p.path = new QLineEdit;
    p.path->setPlaceholderText("Directory, e.g. /srv/downloads");
    pathrow->addWidget(p.path, 1);
    button("…", pathrow, [this, &p] {
        if (p.profile->currentData().toString() != "__local__")
            return;
        auto path =
            QFileDialog::getExistingDirectory(this, "Choose local directory", p.path->text());
        if (!path.isEmpty())
            browse(p, path);
    })->setToolTip("Choose a folder when This computer is selected");
    button("↻", pathrow, [this, &p] { browse(p); })->setToolTip("Refresh directory");
    layout->addLayout(pathrow);
    p.tree = new QTreeWidget;
    p.tree->setHeaderLabels({"Name", "Size", "Modified"});
    p.tree->setRootIsDecorated(false);
    p.tree->setAlternatingRowColors(true);
    p.tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    p.tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(p.tree, &QWidget::customContextMenuRequested, this, [this, &p](const QPoint &point) {
        selectContextItem(p.tree, point);
        QMenu menu(this);
        directoryActions(menu, p);
        menu.exec(p.tree->viewport()->mapToGlobal(point));
    });
    p.tree->setSortingEnabled(true);
    p.tree->sortItems(0, Qt::AscendingOrder);
    p.tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    p.tree->setColumnWidth(1, 100);
    p.tree->setColumnWidth(2, 145);
    layout->addWidget(p.tree);
    p.info = new QLabel("Disconnected");
    p.info->setWordWrap(true);
    layout->addWidget(p.info);
    connect(p.path, &QLineEdit::returnPressed, this, [this, &p] { browse(p); });
    connect(p.tree, &QTreeWidget::itemDoubleClicked, this, [this, &p](QTreeWidgetItem *item, int) {
        int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < p.entries.size() &&
            p.entries[index].type == SSH_FILEXFER_TYPE_DIRECTORY)
            browse(p, remoteJoin(p.loadedPath, p.entries[index].name));
    });
    connect(p.profile, &QComboBox::currentIndexChanged, this, [this, &p] {
        profileChanged(p);
        save();
    });
    return box;
}
void Window::profileChanged(Pane &p) {
    clearDirectoryComparison();
    if (&p == &destination)
        destinationRefreshPending = false;
    ++p.generation;
    p.loadedProfile.clear();
    p.loadedPath.clear();
    p.entries.clear();
    p.tree->clear();
    p.info->setText("Disconnected");
    try {
        p.path->setText(selectedProfile(p).path);
    } catch (...) {
        p.path->clear();
    }
}
void Window::refreshProfiles(Pane *target, const QString &selectedId, const QString &changedId) {
    for (Pane *p : {&source, &destination}) {
        QString prior = p->profile->currentData().toString();
        {
            QSignalBlocker blocker(p->profile);
            p->profile->clear();
            p->profile->addItem("Select a connection…", QString{});
            p->profile->addItem("This computer · local folders", QString("__local__"));
            for (const Profile &profile : profiles)
                p->profile->addItem(profile.name + " · " + profile.user + "@" + profile.host,
                                    profile.id);
            int idx = p->profile->findData(p == target ? selectedId : prior);
            p->profile->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        if (p->profile->currentData().toString() != prior ||
            (!changedId.isEmpty() && prior == changedId))
            profileChanged(*p);
    }
}
Profile Window::selectedProfile(const Pane &p) {
    QString id = p.profile->currentData().toString();
    if (id == "__local__")
        return Profile::thisComputer();
    for (const Profile &profile : profiles)
        if (profile.id == id)
            return profile;
    throw std::runtime_error("Add and select a connection first.");
}
Profile Window::credentials(Profile p) {
    if (p.local)
        return p;
    for (const Profile &saved : profiles)
        if (saved.id == p.id) {
            p.secret = saved.secret;
            p.remember = saved.remember;
            p.password = saved.password;
            p.key = saved.key;
            break;
        }
    if (p.secret.isEmpty() && p.remember)
        p.secret = loadSecret(p.id);
    return p;
}
void Window::manageConnections() {
    QDialog dialog(this);
    dialog.setObjectName("connectionsDialog");
    dialog.setWindowTitle("Connections");
    dialog.resize(620, 380);
    auto layout = new QVBoxLayout(&dialog);
    auto notice = new QLabel("Create or edit saved connections, then choose where to use them. "
                             "Editing updates the saved connection in every pane using it.");
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto list = new QListWidget;
    list->setObjectName("savedConnections");
    layout->addWidget(list);
    auto actions = new QHBoxLayout;
    auto reload = [this, list](const QString &selected) {
        list->clear();
        for (const auto &profile : profiles) {
            auto item =
                new QListWidgetItem(profile.name + " · " + profile.user + "@" + profile.host, list);
            item->setData(Qt::UserRole, profile.id);
            if (profile.id == selected)
                list->setCurrentItem(item);
        }
        if (!list->currentItem() && list->count())
            list->setCurrentRow(0);
    };
    button("New…", actions, [this, list, reload] {
        QString prior =
            list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
        int count = profiles.size();
        editProfile();
        reload(profiles.size() > count ? profiles.back().id : prior);
    })->setObjectName("newConnection");
    auto edit = button("Edit…", actions, [this, list, reload] {
        if (!list->currentItem())
            return;
        QString id = list->currentItem()->data(Qt::UserRole).toString();
        editProfile(id);
        reload(id);
    });
    edit->setObjectName("editConnection");
    actions->addStretch();
    auto assign = [list](Pane &pane) {
        if (list->currentItem())
            pane.profile->setCurrentIndex(
                pane.profile->findData(list->currentItem()->data(Qt::UserRole)));
    };
    auto useSource = button("Use in source", actions, [this, assign] { assign(source); });
    useSource->setObjectName("useConnectionSource");
    auto useDestination =
        button("Use in destination", actions, [this, assign] { assign(destination); });
    useDestination->setObjectName("useConnectionDestination");
    auto update = [list, edit, useSource, useDestination] {
        bool selected = list->currentItem() != nullptr;
        edit->setEnabled(selected);
        useSource->setEnabled(selected);
        useDestination->setEnabled(selected);
    };
    connect(list, &QListWidget::itemSelectionChanged, &dialog, update);
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [edit](QListWidgetItem *) { edit->click(); });
    layout->addLayout(actions);
    auto controls = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(controls, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(controls);
    reload(source.profile->currentData().toString());
    update();
    dialog.exec();
}
void Window::editProfile(const QString &id) {
    Profile p;
    p.id = uid();
    bool existing = false;
    for (const Profile &profile : profiles)
        if (profile.id == id) {
            p = profile;
            existing = true;
        }
    try {
        if (existing)
            p = credentials(p);
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "Keyring", e.what());
    }
    QDialog dialog(this);
    dialog.setWindowTitle(existing ? "Edit saved connection" : "New saved connection");
    dialog.setMinimumWidth(540);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto name = new QLineEdit(p.name);
    name->setObjectName("connectionName");
    auto host = new QLineEdit(p.host);
    host->setObjectName("connectionHost");
    auto user = new QLineEdit(p.user);
    user->setObjectName("connectionUser");
    auto port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(p.port);
    auto path = new QLineEdit(p.path);
    auto auth = new QComboBox;
    auth->addItems({"Password", "SSH key / agent"});
    auth->setCurrentIndex(p.password ? 0 : 1);
    auto key = new QLineEdit(p.key);
    key->setPlaceholderText("Private key file (required for source key authentication)");
    auto keyrow = new QHBoxLayout;
    keyrow->addWidget(key);
    button("Browse…", keyrow, [this, key] {
        auto file = QFileDialog::getOpenFileName(this, "Select private SSH key",
                                                 QDir::homePath() + "/.ssh");
        if (!file.isEmpty())
            key->setText(file);
    });
    auto secret = new QLineEdit(p.secret);
    secret->setEchoMode(QLineEdit::Password);
    secret->setPlaceholderText("Password or private-key passphrase");
    auto remember = new QCheckBox("Remember secret in the desktop keyring");
    remember->setObjectName("rememberSecret");
    remember->setChecked(true);
    form->addRow("Name", name);
    form->addRow("Host", host);
    form->addRow("SSH port", port);
    form->addRow("Username", user);
    form->addRow("Initial directory", path);
    form->addRow("Authentication", auth);
    form->addRow("Private key", keyrow);
    form->addRow("Secret", secret);
    layout->addLayout(form);
    layout->addWidget(remember);
    auto explanation =
        new QLabel("Connection settings are saved locally. For direct downloads, the source secret "
                   "or selected key is sent over SSH to the destination worker. Passwords stay in "
                   "memory; temporary key files are removed when the job finishes. An SSH agent "
                   "can authenticate the destination connection.");
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto controls = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(controls);
    connect(controls, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&, this] {
        if (name->text().trimmed().isEmpty() ||
            !QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.:-]*$")
                 .match(host->text().trimmed())
                 .hasMatch() ||
            !QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.@-]*$")
                 .match(user->text().trimmed())
                 .hasMatch()) {
            QMessageBox::warning(&dialog, "Connection details",
                                 "Enter a name, hostname or IP address, and username.");
            return;
        }
        try {
            if (remember->isChecked() && !secret->text().isEmpty())
                saveSecret(p.id, secret->text());
            else if (p.remember)
                saveSecret(p.id, {});
        } catch (const std::exception &e) {
            QMessageBox::warning(
                &dialog, "Cannot save credential",
                QString::fromUtf8(e.what()) +
                    "\n\nYou can uncheck Remember to keep the secret only for this session.");
            return;
        }
        p.name = name->text().trimmed();
        p.host = host->text().trimmed();
        p.user = user->text().trimmed();
        p.port = port->value();
        p.path = path->text().isEmpty() ? "." : path->text();
        p.key = key->text();
        p.password = auth->currentIndex() == 0;
        p.secret = secret->text();
        p.remember = remember->isChecked() && !p.secret.isEmpty();
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    bool replaced = false;
    for (Profile &profile : profiles)
        if (profile.id == p.id) {
            profile = p;
            replaced = true;
        }
    if (!replaced)
        profiles.append(p);
    refreshProfiles(nullptr, {}, replaced ? p.id : QString{});
    save();
}
Trust Window::trust() {
    return [this](const QString &host, const QString &fingerprint) {
        bool accepted = false;
        QMetaObject::invokeMethod(
            this,
            [&, this] {
                accepted =
                    QMessageBox::question(
                        this, "Verify SSH host key",
                        "First connection to " + host + "\n\nSHA256 fingerprint:\n" + fingerprint +
                            "\n\nCompare this with the server's host key. Trust and save this key?",
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
            },
            Qt::BlockingQueuedConnection);
        return accepted;
    };
}
void Window::task(const QString &label, std::function<QJsonObject()> work,
                  std::function<void(const QJsonObject &, const QString &)> done) {
    ++tasks;
    statusBar()->showMessage(label);
    auto watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, done] {
        auto result = watcher->result();
        --tasks;
        watcher->deleteLater();
        QString error = result.take("__error").toString();
        done(result, error);
        if (tasks == 0)
            statusBar()->showMessage(error.isEmpty() ? "Ready" : error, 10000);
    });
    watcher->setFuture(QtConcurrent::run([work] {
        try {
            return work();
        } catch (const std::exception &e) {
            return QJsonObject{{"__error", QString::fromUtf8(e.what())}};
        }
    }));
}
void Window::browse(Pane &p, const QString &requested) {
    if (p.busy)
        return;
    try {
        Profile profile = credentials(selectedProfile(p));
        QString path = requested.isEmpty() ? p.path->text() : requested;
        auto verify = trust();
        clearDirectoryComparison();
        p.busy = true;
        p.info->setText("Connecting and reading directory…");
        int generation = p.generation;
        task(
            "Reading " + profile.name + "…",
            [profile, path, verify] {
                Connection connection(profile, verify);
                QString canonical = connection.canonical(path.isEmpty() ? "." : path);
                QJsonArray entries;
                for (const Entry &entry : connection.list(canonical))
                    entries.append(QJsonObject{{"name", entry.name},
                                               {"size", double(entry.size)},
                                               {"mtime", double(entry.mtime)},
                                               {"type", entry.type}});
                return QJsonObject{{"path", canonical}, {"entries", entries}};
            },
            [this, &p, profile, generation](const QJsonObject &result, const QString &error) {
                p.busy = false;
                if (generation != p.generation) {
                    refreshDestinationIfReady();
                    return;
                }
                if (!error.isEmpty()) {
                    p.info->setText(error);
                    refreshDestinationIfReady();
                    return;
                }
                p.loadedProfile = profile.id;
                p.loadedPath = result["path"].toString();
                p.path->setText(p.loadedPath);
                QVector<Entry> entries;
                for (auto value : result["entries"].toArray()) {
                    auto e = value.toObject();
                    Entry entry{e["name"].toString(), quint64(e["size"].toDouble()),
                                quint32(e["mtime"].toDouble()), e["type"].toInt()};
                    entries.append(entry);
                }
                displayDirectory(p, entries);
                p.info->setText(
                    QString::number(p.entries.size()) + " items · " +
                    (profile.local ? QString("This computer") : profile.user + "@" + profile.host));
                if (profile.local)
                    save();
                refreshDestinationIfReady();
            });
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "Connection", e.what());
    }
}
void Window::displayDirectory(Pane &p, const QVector<Entry> &entries) {
    clearDirectoryComparison();
    int column = p.tree->header()->sortIndicatorSection();
    auto order = p.tree->header()->sortIndicatorOrder();
    p.tree->setSortingEnabled(false);
    p.tree->clear();
    p.entries = entries;
    for (int index = 0; index < entries.size(); ++index) {
        const Entry &entry = entries[index];
        auto item = new DirectoryItem(p.tree);
        item->setText(0, entry.name);
        item->setText(1, entry.type == SSH_FILEXFER_TYPE_DIRECTORY ? "Folder" : bytes(entry.size));
        item->setText(2, QDateTime::fromSecsSinceEpoch(entry.mtime).toString("yyyy-MM-dd HH:mm"));
        item->setData(0, Qt::UserRole, index);
        item->setData(1, Qt::UserRole,
                      QVariant::fromValue<qulonglong>(
                          entry.type == SSH_FILEXFER_TYPE_DIRECTORY ? 0 : entry.size));
        item->setData(2, Qt::UserRole, QVariant::fromValue<qulonglong>(entry.mtime));
        item->setIcon(0, style()->standardIcon(entry.type == SSH_FILEXFER_TYPE_DIRECTORY
                                                   ? QStyle::SP_DirIcon
                                                   : QStyle::SP_FileIcon));
        if (entry.type == SSH_FILEXFER_TYPE_SYMLINK)
            item->setToolTip(0, "Symbolic link — excluded from transfers");
    }
    p.tree->setSortingEnabled(true);
    p.tree->sortItems(column, order);
}
void Window::clearDirectoryComparison() {
    if (!comparisonActive)
        return;
    for (Pane *pane : {&source, &destination}) {
        for (int row = 0; row < pane->tree->topLevelItemCount(); ++row) {
            auto item = pane->tree->topLevelItem(row);
            for (int column = 0; column < pane->tree->columnCount(); ++column) {
                item->setData(column, Qt::BackgroundRole, QVariant{});
                item->setData(column, Qt::ForegroundRole, QVariant{});
            }
        }
    }
    comparisonActive = false;
    statusBar()->showMessage("Comparison cleared — click Compare after opening both directories.");
}
void Window::compareDirectories() {
    clearDirectoryComparison();
    if (source.busy || destination.busy || source.loadedPath.isEmpty() ||
        destination.loadedPath.isEmpty()) {
        statusBar()->showMessage(
            "Open both directories and wait for browsing to finish before comparing.");
        return;
    }
    auto names = [](const Pane &pane) {
        QSet<QString> result;
        for (int row = 0; row < pane.tree->topLevelItemCount(); ++row)
            result.insert(pane.tree->topLevelItem(row)->text(0));
        return result;
    };
    const auto sourceNames = names(source), destinationNames = names(destination);
    auto highlight = [](Pane &pane, const QSet<QString> &other) {
        int missing = 0;
        for (int row = 0; row < pane.tree->topLevelItemCount(); ++row) {
            auto item = pane.tree->topLevelItem(row);
            if (other.contains(item->text(0)))
                continue;
            ++missing;
            for (int column = 0; column < pane.tree->columnCount(); ++column) {
                item->setBackground(column, QColor(255, 226, 153));
                item->setForeground(column, QColor(Qt::black));
            }
        }
        return missing;
    };
    int sourceOnly = highlight(source, destinationNames);
    int destinationOnly = highlight(destination, sourceNames);
    comparisonActive = true;
    statusBar()->showMessage(
        QString("Name comparison: %1 source-only, %2 destination-only (highlighted). "
                "Case-sensitive names only; folder contents, types, sizes and dates ignored.")
            .arg(sourceOnly)
            .arg(destinationOnly));
}
void Window::directoryActions(QMenu &menu, Pane &pane) {
    bool selected = !pane.tree->selectedItems().isEmpty();
    bool ready = !pane.busy && !pane.loadedPath.isEmpty() &&
                 pane.loadedProfile == pane.profile->currentData().toString() &&
                 pane.path->text() == pane.loadedPath;
    if (&pane == &source) {
        auto enqueue = menu.addAction("Queue selected →", this, [this] { addToQueue(); });
        enqueue->setObjectName("contextQueue");
        enqueue->setEnabled(
            selected && ready && !destination.busy && !destination.loadedPath.isEmpty() &&
            destination.loadedProfile == destination.profile->currentData().toString() &&
            destination.path->text() == destination.loadedPath);
        auto dequeue =
            menu.addAction("Dequeue selected", this, [this] { dequeueSourceSelection(); });
        dequeue->setObjectName("contextDequeueSource");
        dequeue->setEnabled(ready && !queuedSourceItems().isEmpty());
        dequeue->setToolTip("Remove matching entries from all destination batches; "
                            "transferred files are never deleted");
        menu.addSeparator();
    }
    auto item = pane.tree->currentItem();
    int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
    auto open = menu.addAction("Open folder", this, [this, &pane] {
        auto current = pane.tree->currentItem();
        int index = current ? current->data(0, Qt::UserRole).toInt() : -1;
        if (!pane.busy && index >= 0 && index < pane.entries.size() &&
            pane.entries[index].type == SSH_FILEXFER_TYPE_DIRECTORY)
            browse(pane, remoteJoin(pane.loadedPath, pane.entries[index].name));
    });
    open->setEnabled(ready && selected && index >= 0 && index < pane.entries.size() &&
                     pane.entries[index].type == SSH_FILEXFER_TYPE_DIRECTORY);
    menu.addAction("Refresh", this, [this, &pane] { browse(pane); })
        ->setEnabled(!pane.busy && !pane.profile->currentData().toString().isEmpty());
    menu.addAction("Compare listings", this, [this] { compareDirectories(); })
        ->setEnabled(!source.busy && !destination.busy && !source.loadedPath.isEmpty() &&
                     !destination.loadedPath.isEmpty());
    menu.addSeparator();
    auto copy = menu.addAction("Copy full path(s)", this, [&pane] {
        QStringList paths;
        for (auto item : pane.tree->selectedItems())
            paths.append(remoteJoin(pane.loadedPath, item->text(0)));
        if (!paths.isEmpty())
            QApplication::clipboard()->setText(paths.join('\n'));
    });
    copy->setObjectName("contextCopyPaths");
    copy->setEnabled(selected && ready);
    menu.addAction("Select all", pane.tree, &QTreeWidget::selectAll)
        ->setEnabled(pane.tree->topLevelItemCount() > 0);
}
QList<QTreeWidgetItem *> Window::queuedSourceItems() const {
    QList<QTreeWidgetItem *> result;
    if (source.busy || source.loadedPath.isEmpty() ||
        source.loadedProfile != source.profile->currentData().toString() ||
        source.path->text() != source.loadedPath)
        return result;
    QVector<QPair<QString, bool>> paths;
    for (auto item : source.tree->selectedItems()) {
        int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < source.entries.size())
            paths.append({remoteJoin(source.loadedPath, source.entries[index].name),
                          source.entries[index].type == SSH_FILEXFER_TYPE_DIRECTORY});
    }
    QHash<QString, QJsonObject> batches;
    for (auto value : jobs) {
        auto job = value.toObject();
        if (job["source_profile"].toObject()["id"].toString() == source.loadedProfile)
            batches.insert(job["id"].toString(), job);
    }
    for (int row = 0; row < queue->topLevelItemCount(); ++row) {
        auto item = queue->topLevelItem(row);
        auto job = batches.value(item->data(0, Qt::UserRole).toString());
        if (job.isEmpty())
            continue;
        auto files = job["files"].toArray();
        int index = item->data(0, Qt::UserRole + 1).toInt();
        QStringList candidates;
        if (index >= 0 && index < files.size())
            candidates.append(files[index].toObject()["source"].toString());
        else if (files.isEmpty() && !job["source_directory"].toString().isEmpty())
            for (auto directory : job["directories"].toArray())
                candidates.append(
                    remoteJoin(job["source_directory"].toString(), directory.toString()));
        bool matches = false;
        for (const auto &path : paths)
            for (const auto &candidate : candidates)
                if (candidate == path.first ||
                    (path.second && candidate.startsWith(path.first + '/')))
                    matches = true;
        if (matches)
            result.append(item);
    }
    return result;
}
void Window::dequeueSourceSelection() {
    auto matching = queuedSourceItems();
    if (matching.isEmpty())
        return;
    queue->clearSelection();
    for (auto item : matching)
        item->setSelected(true);
    removeSelected();
}
void Window::queueActions(QMenu &menu) {
    bool selected = !queue->selectedItems().isEmpty();
    auto start = menu.addAction("Start queue", this, [this] { startQueue(); });
    start->setEnabled(!jobs.isEmpty() && !dispatching);
    auto dequeue = menu.addAction("Dequeue selected", this, [this] { removeSelected(); });
    dequeue->setObjectName("contextDequeue");
    dequeue->setEnabled(selected);
    dequeue->setToolTip("Remove or skip selected entries without deleting transferred files");
    menu.addAction("Retry / resume selected", this, [this] { retrySelected(); })
        ->setEnabled(selected);
    menu.addAction("Cancel selected batch(es)", this, [this] { cancelSelected(); })
        ->setEnabled(selected);
    menu.addSeparator();
    menu.addAction("Transfer details…", this, [this] { showTransferDetails(); })
        ->setEnabled(selected);
    auto copy = menu.addAction("Copy destination path(s)", this, [this] {
        QStringList paths;
        for (auto item : queue->selectedItems()) {
            int index = jobIndex(item->data(0, Qt::UserRole).toString());
            if (index < 0)
                continue;
            auto job = jobs[index].toObject();
            auto files = job["files"].toArray();
            int file = item->data(0, Qt::UserRole + 1).toInt();
            paths.append(file >= 0 && file < files.size()
                             ? remoteJoin(job["destination"].toString(),
                                          files[file].toObject()["relative"].toString())
                             : job["destination"].toString());
        }
        if (!paths.isEmpty())
            QApplication::clipboard()->setText(paths.join('\n'));
    });
    copy->setObjectName("contextCopyDestinations");
    copy->setEnabled(selected);
    menu.addSeparator();
    menu.addAction("Select all", queue, &QTreeWidget::selectAll)
        ->setEnabled(queue->topLevelItemCount() > 0);
    menu.addAction("Clear finished", this, [this] { clearFinished(); })
        ->setEnabled(!jobs.isEmpty());
}
void Window::showTransferDetails() {
    auto selected = queue->selectedItems();
    if (selected.isEmpty())
        return;
    auto item = queue->currentItem();
    if (!item || !item->isSelected())
        item = selected.first();
    int index = jobIndex(item->data(0, Qt::UserRole).toString());
    if (index < 0)
        return;
    auto job = jobs[index].toObject();
    auto status = job["status"].toObject();
    auto files = job["files"].toArray();
    int fileIndex = item->data(0, Qt::UserRole + 1).toInt();
    QString file = fileIndex >= 0 && fileIndex < files.size()
                       ? files[fileIndex].toObject()["relative"].toString()
                       : QString{};
    QMessageBox::information(this, "Transfer details",
                             (file.isEmpty() ? job["label"].toString() : file) +
                                 "\n\nDestination: " + job["destination"].toString() + "\nState: " +
                                 item->text(2) + "\nTransfer batch: " + job["label"].toString() +
                                 "\nCurrent file: " + status["current"].toString() + "\n\n" +
                                 job["error"].toString() + "\n" + status["error"].toString());
}
void Window::clearFinished() {
    QJsonArray keep;
    for (auto job : jobs)
        if (job.toObject()["state"].toString() != "Completed")
            keep.append(job);
    jobs = keep;
    save();
    renderQueue();
}
void Window::queueManifest(const QJsonObject &manifest, const Profile &from, const Profile &to,
                           const QString &target, const QString &label, const QString &sourceRoot) {
    QJsonObject job = manifest;
    job["id"] = uid();
    job["label"] = label;
    job["destination"] = target;
    job["source_profile"] = from.json();
    job["source_directory"] = sourceRoot;
    job["destination_profile"] = to.json();
    job["state"] = "Queued";
    job["engine"] = from.local || to.local ? "desktop" : "remote";
    job["created"] = QDateTime::currentMSecsSinceEpoch();
    jobs.append(job);
    save();
    renderQueue();
}
void Window::addToQueue() {
    try {
        if (source.busy || destination.busy)
            throw std::runtime_error("Wait for directory browsing to finish.");
        Profile from = credentials(selectedProfile(source)), to = selectedProfile(destination);
        if (source.loadedProfile != from.id || destination.loadedProfile != to.id)
            throw std::runtime_error("Connect both panes before queueing a transfer.");
        if (source.path->text() != source.loadedPath ||
            destination.path->text() != destination.loadedPath)
            throw std::runtime_error("Open the edited directory path before queueing.");
        if (!from.local && !to.local && !from.password && from.key.isEmpty())
            throw std::runtime_error("Select a private key file for the source connection. Direct "
                                     "downloads cannot use your local SSH agent.");
        QVector<Entry> selected;
        for (auto item : source.tree->selectedItems())
            selected.append(source.entries[item->data(0, Qt::UserRole).toInt()]);
        if (selected.isEmpty())
            throw std::runtime_error("Select files or folders in the source pane.");
        if (from.local && to.local)
            for (const auto &entry : selected)
                if (entry.type == SSH_FILEXFER_TYPE_DIRECTORY) {
                    QString folder = remoteJoin(source.loadedPath, entry.name);
                    if (destination.loadedPath == folder ||
                        destination.loadedPath.startsWith(folder + "/"))
                        throw std::runtime_error(
                            "Choose a destination outside the selected source folder.");
                }
        QString path = source.loadedPath, target = destination.loadedPath;
        auto verify = trust();
        task(
            "Scanning selected folders…",
            [from, path, selected, verify] {
                Connection c(from, verify);
                return c.manifest(path, selected);
            },
            [this, from, to, path, target, selected](const QJsonObject &manifest,
                                                     const QString &error) {
                if (!error.isEmpty()) {
                    QMessageBox::warning(this, "Cannot queue transfer", error);
                    return;
                }
                if (manifest["files"].toArray().isEmpty() &&
                    manifest["directories"].toArray().isEmpty()) {
                    QMessageBox::information(this, "No transferable items",
                                             "Symbolic links and special files are excluded.");
                    return;
                }
                QString label = selected.size() == 1
                                    ? selected.front().name
                                    : QString::number(selected.size()) + " selected items";
                queueManifest(manifest, from, to, target, label, path);
                if (manifest["skipped"].toInt() > 0)
                    QMessageBox::information(this, "Items excluded",
                                             QString::number(manifest["skipped"].toInt()) +
                                                 " symbolic links or special files were excluded.");
            });
    } catch (const std::exception &e) {
        QMessageBox::information(this, "Queue transfer", e.what());
    }
}
int Window::jobIndex(const QString &id) const {
    for (int i = 0; i < jobs.size(); ++i)
        if (jobs[i].toObject()["id"].toString() == id)
            return i;
    return -1;
}
void Window::startQueue() {
    if (dispatching)
        return;
    QJsonObject job;
    for (auto value : jobs)
        if (value.toObject()["state"].toString() == "Queued") {
            job = value.toObject();
            break;
        }
    if (job.isEmpty())
        return;
    try {
        auto from = credentials(Profile::fromJson(job["source_profile"].toObject()));
        auto to = credentials(Profile::fromJson(job["destination_profile"].toObject()));
        if (from.local || to.local) {
            startDesktop(job, from, to);
            return;
        }
        QJsonObject sourceAuth{{"host", from.host},
                               {"port", from.port},
                               {"user", from.user},
                               {"secret", from.secret},
                               {"known_host", job["known_host"]}};
        if (!from.password) {
            QFile key(from.key);
            if (!key.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read source private key");
            sourceAuth["key_data"] = QString::fromLatin1(key.readAll().toBase64());
        }
        if (from.password && from.secret.isEmpty())
            throw std::runtime_error(
                "Source password is missing. Edit its connection and enter the password.");
        QFile worker(":/worker/worker.py");
        if (!worker.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read embedded worker");
        QByteArray script = worker.readAll();
        auto verify = trust();
        QString id = job["id"].toString();
        job["state"] = "Dispatching";
        job["submitted"] = true;
        job["queue_version"] = 2;
        jobs[jobIndex(id)] = job;
        save();
        renderQueue();
        dispatching = true;
        task(
            "Starting direct download on " + to.name + "…",
            [to, sourceAuth, job, script, verify] {
                Connection c(to, verify);
                QString workerPath = c.uploadWorker(script);
                QJsonObject remote = job;
                remote.remove("source_profile");
                remote.remove("destination_profile");
                remote.remove("known_host");
                remote.remove("status");
                QByteArray payload =
                    QJsonDocument(QJsonObject{{"job", remote}, {"source", sourceAuth}})
                        .toJson(QJsonDocument::Compact);
                QByteArray result = c.execute("python3 " + shellQuote(workerPath), payload);
                auto doc = QJsonDocument::fromJson(result);
                if (!doc.object()["started"].toBool())
                    throw std::runtime_error("Remote worker did not acknowledge the job");
                auto reply = doc.object();
                reply["worker_path"] = workerPath;
                return reply;
            },
            [this, id](const QJsonObject &reply, const QString &error) {
                dispatching = false;
                int index = jobIndex(id);
                if (index >= 0) {
                    auto j = jobs[index].toObject();
                    j["state"] = error.isEmpty() ? "Starting" : "Uncertain";
                    j["error"] = error;
                    if (reply.contains("worker_path"))
                        j["worker_path"] = reply["worker_path"];
                    jobs[index] = j;
                    save();
                    renderQueue();
                }
                startQueue();
            });
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "Cannot start queue", e.what());
    }
}
void Window::startDesktop(const QJsonObject &queued, const Profile &from, const Profile &to) {
    QString id = queued["id"].toString();
    auto job = queued;
    job["engine"] = "desktop";
    job["state"] = "Dispatching";
    job["submitted"] = true;
    job["queue_version"] = 2;
    jobs[jobIndex(id)] = job;
    dispatching = true;
    activeDesktop = id;
    save();
    renderQueue();
    auto verify = trust();
    task(
        from.local ? (to.local ? "Copying local files…" : "Uploading files…")
                   : "Downloading files…",
        [this, job, from, to, verify, id] {
            return DesktopTransfer::run(
                job, from, to, verify, [this, id](const QJsonObject &status) {
                    QMetaObject::invokeMethod(
                        this, [this, id, status] { applyStatuses({{id, status}}); },
                        Qt::QueuedConnection);
                });
        },
        [this, id](const QJsonObject &result, const QString &error) {
            activeDesktop.clear();
            dispatching = false;
            auto status = result;
            if (!error.isEmpty())
                status = {{"state", "Failed"}, {"error", error}};
            applyStatuses({{id, status}});
            startQueue();
        });
}
void Window::poll() {
    if (polling)
        return;
    QJsonArray pending;
    QMap<QString, Profile> connections;
    try {
        for (auto value : jobs) {
            auto job = value.toObject();
            QString state = job["state"].toString();
            if (state == "Queued" || (state == "Dispatching" && dispatching) || terminal(state) ||
                job["id"].toString() == activeDesktop)
                continue;
            if (job["engine"].toString() == "desktop") {
                pending.append(job);
                continue;
            }
            Profile p = credentials(Profile::fromJson(job["destination_profile"].toObject()));
            connections[p.id] = p;
            pending.append(job);
        }
    } catch (const std::exception &e) {
        statusBar()->showMessage(e.what());
        return;
    }
    if (pending.isEmpty())
        return;
    polling = true;
    auto verify = trust();
    task(
        "Checking remote transfers…",
        [pending, connections, verify] {
            QJsonObject results;
            for (auto value : pending) {
                auto j = value.toObject();
                if (j["engine"].toString() == "desktop") {
                    try {
                        results[j["id"].toString()] = DesktopTransfer::status(j["id"].toString());
                    } catch (const std::exception &e) {
                        results[j["id"].toString()] =
                            QJsonObject{{"monitor_error", QString::fromUtf8(e.what())}};
                    }
                }
            }
            for (auto it = connections.begin(); it != connections.end(); ++it) {
                try {
                    Connection c(it.value(), verify);
                    for (auto value : pending) {
                        auto j = value.toObject();
                        if (j["engine"].toString() != "desktop" &&
                            j["destination_profile"].toObject()["id"].toString() == it.key())
                            results[j["id"].toString()] =
                                c.status(j["id"].toString(), j["worker_path"].toString());
                    }
                } catch (const std::exception &e) {
                    for (auto value : pending) {
                        auto j = value.toObject();
                        if (j["engine"].toString() != "desktop" &&
                            j["destination_profile"].toObject()["id"].toString() == it.key())
                            results[j["id"].toString()] =
                                QJsonObject{{"monitor_error", QString::fromUtf8(e.what())}};
                    }
                }
            }
            return results;
        },
        [this](const QJsonObject &results, const QString &) {
            polling = false;
            applyStatuses(results);
        });
}
bool Window::matchesDestination(const QJsonObject &job) const {
    auto endpoint = job["destination_profile"].toObject();
    QString id = destination.profile->currentData().toString();
    if (id == "__local__")
        return endpoint["local"].toBool() && endpoint["id"].toString() == id;
    for (const Profile &p : profiles)
        if (p.id == id)
            return endpoint["id"].toString() == id && endpoint["host"].toString() == p.host &&
                   endpoint["user"].toString() == p.user && endpoint["port"].toInt(22) == p.port;
    return false;
}
bool Window::destinationRefreshReady() const {
    if (!destinationRefreshPending || destination.busy)
        return false;
    for (auto value : jobs) {
        auto job = value.toObject();
        auto state = job["state"].toString();
        if (matchesDestination(job) && state != "Queued" && !terminal(state))
            return false;
    }
    return true;
}
void Window::refreshDestinationIfReady() {
    if (!destinationRefreshReady())
        return;
    destinationRefreshPending = false;
    browse(destination);
}
void Window::applyStatuses(const QJsonObject &results) {
    for (auto it = results.begin(); it != results.end(); ++it) {
        int index = jobIndex(it.key());
        if (index < 0)
            continue;
        auto job = jobs[index].toObject();
        QString priorState = job["state"].toString();
        auto status = it.value().toObject();
        if (status.contains("monitor_error")) {
            job["error"] = "Monitoring disconnected: " + status["monitor_error"].toString();
        } else {
            job["status"] = status;
            job["state"] = status["state"];
            job["error"] = status["error"].toString();
            if (!terminal(priorState) && terminal(status["state"].toString()) &&
                matchesDestination(job))
                destinationRefreshPending = true;
        }
        jobs[index] = job;
    }
    save();
    renderQueue();
    refreshDestinationIfReady();
}
void Window::cancelSelected() {
    QSet<QString> handled;
    for (auto item : queue->selectedItems()) {
        QString id = item->data(0, Qt::UserRole).toString();
        if (handled.contains(id))
            continue;
        handled.insert(id);
        int index = jobIndex(id);
        if (index < 0)
            continue;
        auto job = jobs[index].toObject();
        QString state = job["state"].toString();
        if (terminal(state))
            continue;
        if (state == "Queued") {
            job["state"] = "Cancelled";
            jobs[index] = job;
            continue;
        }
        if (state == "Dispatching")
            continue;
        if (job["engine"].toString() == "desktop") {
            try {
                DesktopTransfer::cancel(id);
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "Cancel", e.what());
            }
            continue;
        }
        try {
            Profile p = credentials(Profile::fromJson(job["destination_profile"].toObject()));
            auto verify = trust();
            task(
                "Requesting cancellation…",
                [p, id, verify] {
                    Connection c(p, verify);
                    c.cancel(id);
                    return QJsonObject{};
                },
                [this](const QJsonObject &, const QString &error) {
                    if (!error.isEmpty())
                        QMessageBox::warning(this, "Cancel", error);
                    else
                        poll();
                });
        } catch (const std::exception &e) {
            QMessageBox::warning(this, "Cancel", e.what());
        }
    }
    save();
    renderQueue();
}
void Window::removeSelected() {
    QMap<QString, QSet<int>> selections;
    for (auto item : queue->selectedItems())
        selections[item->data(0, Qt::UserRole).toString()].insert(
            item->data(0, Qt::UserRole + 1).toInt());
    queue->clearSelection();
    for (auto it = selections.begin(); it != selections.end(); ++it) {
        QString id = it.key();
        int index = jobIndex(id);
        if (index < 0)
            continue;
        auto job = jobs[index].toObject();
        auto state = job["state"].toString();
        auto files = job["files"].toArray();
        if (state == "Dispatching") {
            statusBar()->showMessage("Wait for dispatch to finish before removing these files.",
                                     10000);
            continue;
        }
        bool local = state == "Queued" && !job["submitted"].toBool() &&
                     !job.contains("worker_path") && !job.contains("status");
        if (local) {
            if (files.isEmpty()) {
                jobs.removeAt(index);
                continue;
            }
            QJsonArray keep;
            for (int i = 0; i < files.size(); ++i)
                if (!it.value().contains(i))
                    keep.append(files[i]);
            if (keep.isEmpty())
                jobs.removeAt(index);
            else {
                job["files"] = keep;
                jobs[index] = job;
            }
        } else if (terminal(state)) {
            QSet<int> removed;
            for (auto value : job["removed_files"].toArray())
                removed.insert(value.toInt());
            removed.unite(it.value());
            QJsonArray array;
            for (int i : removed)
                array.append(i);
            if (files.isEmpty() || removed.size() >= files.size())
                jobs.removeAt(index);
            else {
                job["removed_files"] = array;
                jobs[index] = job;
            }
        } else {
            if (job["engine"].toString() == "desktop") {
                try {
                    DesktopTransfer::skip(id, it.value());
                    QSet<int> removed = it.value();
                    for (auto value : job["removed_files"].toArray())
                        removed.insert(value.toInt());
                    QJsonArray array;
                    for (int i : removed)
                        array.append(i);
                    job["removed_files"] = array;
                    jobs[index] = job;
                } catch (const std::exception &e) {
                    QMessageBox::warning(this, "Remove", e.what());
                }
                continue;
            }
            if (files.isEmpty() || job["queue_version"].toInt() < 2) {
                statusBar()->showMessage("Cancel this batch before removing its entries; it uses "
                                         "an earlier worker or creates empty folders.",
                                         10000);
                continue;
            }
            try {
                Profile p = credentials(Profile::fromJson(job["destination_profile"].toObject()));
                QJsonArray indices;
                for (int i : it.value())
                    indices.append(QString::number(i));
                auto verify = trust();
                auto selected = it.value();
                task(
                    "Removing files from the remote queue…",
                    [p, id, indices, verify] {
                        Connection c(p, verify);
                        c.skipFiles(id, indices);
                        return QJsonObject{};
                    },
                    [this, id, selected](const QJsonObject &, const QString &error) {
                        if (!error.isEmpty()) {
                            QMessageBox::warning(this, "Cannot remove queued files", error);
                            return;
                        }
                        int index = jobIndex(id);
                        if (index < 0)
                            return;
                        auto job = jobs[index].toObject();
                        QSet<int> removed = selected;
                        for (auto value : job["removed_files"].toArray())
                            removed.insert(value.toInt());
                        QJsonArray array;
                        for (int i : removed)
                            array.append(i);
                        job["removed_files"] = array;
                        jobs[index] = job;
                        save();
                        renderQueue();
                        poll();
                    });
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "Remove", e.what());
            }
        }
    }
    save();
    renderQueue();
}
void Window::retrySelected() {
    for (auto item : queue->selectedItems()) {
        int index = jobIndex(item->data(0, Qt::UserRole).toString());
        if (index < 0)
            continue;
        auto j = jobs[index].toObject();
        QString state = j["state"].toString();
        if (state == "Failed" || state == "Cancelled" || state == "Unknown" ||
            state == "Uncertain") {
            j["state"] = "Queued";
            j["error"] = "";
            jobs[index] = j;
        }
    }
    save();
    renderQueue();
    startQueue();
}
void Window::renderQueue() {
    QSet<QString> selected;
    for (auto item : queue->selectedItems())
        selected.insert(item->data(0, Qt::UserRole + 2).toString());
    queue->clear();
    double total = 0;
    int complete = 0, count = 0;
    for (auto value : jobs) {
        auto j = value.toObject();
        auto s = j["status"].toObject();
        auto files = j["files"].toArray();
        QSet<int> removed;
        for (auto index : j["removed_files"].toArray())
            removed.insert(index.toInt());
        QSet<QString> completed, skipped;
        for (auto index : s["completed"].toArray())
            completed.insert(index.toString());
        for (auto index : s["skipped"].toArray())
            skipped.insert(index.toString());
        auto offsets = s["offsets"].toObject();
        QString batchState = j["state"].toString();
        for (int fileIndex = 0; fileIndex < qMax(1, int(files.size())); ++fileIndex) {
            if (removed.contains(fileIndex))
                continue;
            auto file = files.isEmpty() ? QJsonObject{} : files[fileIndex].toObject();
            QString number = QString::number(fileIndex), state = batchState;
            bool done = completed.contains(number);
            if (skipped.contains(number))
                state = "Skipped";
            else if (done || batchState == "Completed")
                state = "Completed";
            else if (batchState == "Running" &&
                     s["current"].toString() != file["relative"].toString())
                state = j["engine"].toString() == "desktop" ? "Waiting" : "Remote queue";
            double size = file["size"].toDouble();
            total += size;
            ++count;
            auto item = new QTreeWidgetItem(queue);
            if (state == "Completed")
                ++complete;
            item->setText(0, files.isEmpty() ? j["label"].toString() + " (folders)"
                                             : file["relative"].toString());
            item->setText(1, j["destination_profile"].toObject()["name"].toString());
            item->setText(2, state);
            item->setText(4, bytes(size));
            bool fromLocal = j["source_profile"].toObject()["local"].toBool(),
                 toLocal = j["destination_profile"].toObject()["local"].toBool();
            item->setText(5, fromLocal ? (toLocal ? "Local copy" : "Upload")
                                       : (toLocal ? "Download" : "Server → server"));
            item->setData(0, Qt::UserRole, j["id"].toString());
            item->setData(0, Qt::UserRole + 1, files.isEmpty() ? -1 : fileIndex);
            QString selectionKey =
                j["id"].toString() + ":" + QString::number(files.isEmpty() ? -1 : fileIndex);
            item->setData(0, Qt::UserRole + 2, selectionKey);
            item->setSelected(selected.contains(selectionKey));
            QString detail = j["error"].toString();
            if (detail.isEmpty())
                detail = s["current"].toString();
            item->setToolTip(0, detail);
            item->setToolTip(2, detail);
            double transferred = state == "Completed" ? size : offsets[number].toDouble();
            item->setData(3, Qt::UserRole,
                          size > 0 ? qBound(0, int(1000 * transferred / size), 1000)
                                   : (state == "Completed" ? 1000 : 0));
            item->setText(3, bytes(transferred) + " / " + bytes(size));
        }
    }
    summary->setText(QString::number(count) + " queue items · " + bytes(total) + " · " +
                     QString::number(complete) + " completed");
}
void Window::save() {
    QDir().mkpath(QFileInfo(configPath).absolutePath());
    QJsonArray connections;
    for (const Profile &p : profiles)
        connections.append(p.json());
    // Keep a recoverable snapshot before changing saved connection definitions.
    // Queue updates and pane selections alone do not create extra backups.
    QFile prior(configPath);
    if (prior.exists() && prior.open(QIODevice::ReadOnly)) {
        QByteArray previous = prior.readAll();
        if (QJsonDocument::fromJson(previous).object()["profiles"].toArray() != connections) {
            QString backupPath = QFileInfo(configPath).absolutePath() + "/connections-" +
                                 QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") +
                                 "-" + uid() + ".json.bak";
            QSaveFile backup(backupPath);
            if (!backup.open(QIODevice::WriteOnly)) {
                statusBar()->showMessage("Cannot back up connection settings: " +
                                         backup.errorString());
                return;
            }
            backup.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (backup.write(previous) != previous.size() || !backup.commit()) {
                statusBar()->showMessage("Cannot back up connection settings");
                return;
            }
        }
    }
    QSaveFile file(configPath);
    if (!file.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage("Cannot save application state: " + file.errorString());
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject panes{{"source", source.profile->currentData().toString()},
                      {"destination", destination.profile->currentData().toString()}};
    if (source.profile->currentData().toString() == "__local__")
        panes["source_local_path"] = source.path->text();
    if (destination.profile->currentData().toString() == "__local__")
        panes["destination_local_path"] = destination.path->text();
    file.write(
        QJsonDocument(QJsonObject{{"profiles", connections}, {"jobs", jobs}, {"panes", panes}})
            .toJson());
    if (!file.commit())
        statusBar()->showMessage("Cannot save queue: " + file.errorString());
}
void Window::closeEvent(QCloseEvent *event) {
    if (tasks) {
        QMessageBox::information(
            this, "Operation in progress",
            "Finish or cancel the current local transfer or connection operation before closing. "
            "Server-side transfers keep running independently.");
        event->ignore();
        return;
    }
    save();
    event->accept();
}
void Window::demo() {
    timer.stop();
    profiles.clear();
    jobs = QJsonArray{};
    Profile vps;
    vps.id = "demo-vps";
    vps.name = "Source server";
    vps.host = "vps.example.net";
    vps.user = "user";
    vps.path = "/srv/files";
    Profile cabin;
    cabin.id = "demo-cabin";
    cabin.name = "Backup server";
    cabin.host = "cabin.example.net";
    cabin.user = "user";
    cabin.path = "/srv/downloads";
    profiles = {vps, cabin};
    refreshProfiles();
    {
        QSignalBlocker sourceBlocker(source.profile), destinationBlocker(destination.profile);
        source.profile->setCurrentIndex(source.profile->findData(vps.id));
        destination.profile->setCurrentIndex(destination.profile->findData(cabin.id));
    }
    for (Pane *p : {&source, &destination}) {
        p->path->setText(p == &source ? vps.path : cabin.path);
        p->info->setText("Preview — no live connection");
    }
    for (const QString &name :
         {QString("Backups"), QString("Photos"), QString("Projects"), QString("archive.tar.zst")}) {
        auto item = new QTreeWidgetItem(
            source.tree, {name, name.contains('.') ? "2.4 GiB" : "Folder", "2026-10-13 16:30"});
        item->setIcon(0, style()->standardIcon(name.contains('.') ? QStyle::SP_FileIcon
                                                                  : QStyle::SP_DirIcon));
    }
    new QTreeWidgetItem(destination.tree, {"Downloads", "Folder", "2026-10-13 15:00"});
    auto j = QJsonObject{
        {"id", "preview"},
        {"label", "Photos"},
        {"source_profile", vps.json()},
        {"destination", cabin.path},
        {"destination_profile", cabin.json()},
        {"state", "Running"},
        {"files", QJsonArray{QJsonObject{{"relative", "Photos/2024.zip"}, {"size", 1073741824.0}},
                             QJsonObject{{"relative", "Photos/2025.zip"}, {"size", 1073741824.0}},
                             QJsonObject{{"relative", "Photos/2026.zip"}, {"size", 1073741824.0}}}},
        {"status", QJsonObject{{"bytes", 1450000000.0},
                               {"current", "Photos/2025.zip"},
                               {"completed", QJsonArray{"0"}},
                               {"offsets", QJsonObject{{"1", 376258176.0}}}}}};
    jobs.append(j);
    renderQueue();
    statusBar()->showMessage("Interface preview");
}
