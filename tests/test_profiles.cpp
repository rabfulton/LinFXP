#include "window.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

class ConnectionProfileTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;

    QJsonObject manifest() {
        return {{"files", QJsonArray{QJsonObject{{"relative", "folder/a.txt"},
                                                 {"source", "/folder/a.txt"},
                                                 {"size", 10}},
                                     QJsonObject{{"relative", "folder/sub/b.txt"},
                                                 {"source", "/folder/sub/b.txt"},
                                                 {"size", 2048}},
                                     QJsonObject{{"relative", "folder/sub/c.txt"},
                                                 {"source", "/folder/sub/c.txt"},
                                                 {"size", 4096}}}},
                {"directories", QJsonArray{"folder", "folder/sub"}},
                {"known_host", "test-only"}};
    }

    Profile queueFixture(Window &window) {
        Profile from, to;
        from.id = "source";
        from.name = "VPS";
        from.host = "vps.example.net";
        from.user = "test";
        to.id = "destination";
        to.name = "Cabin";
        to.host = "cabin.example.net";
        to.user = "test";
        window.profiles = {from, to};
        window.refreshProfiles(&window.destination, to.id);
        window.queueManifest(manifest(), from, to, "/downloads", "folder");
        return to;
    }

    void scheduleProfileDetails(Window &window, const QString &name, const QString &host) {
        QTimer::singleShot(0, &window, [name, host] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            Q_ASSERT(dialog);
            dialog->findChild<QLineEdit *>("connectionName")->setText(name);
            dialog->findChild<QLineEdit *>("connectionHost")->setText(host);
            dialog->findChild<QLineEdit *>("connectionUser")->setText("test");
            dialog->findChild<QCheckBox *>("rememberSecret")->setChecked(false);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
    }

    void enterProfile(Window &window, Window::Pane &pane, bool editing, const QString &name,
                      const QString &host) {
        scheduleProfileDetails(window, name, host);
        int count = window.profiles.size();
        window.editProfile(editing ? pane.profile->currentData().toString() : QString{});
        if (window.profiles.size() > count)
            pane.profile->setCurrentIndex(pane.profile->findData(window.profiles.back().id));
    }

  private Q_SLOTS:
    void init() {
        directory = std::make_unique<QTemporaryDir>("/tmp/linfxp-profiles-XXXXXX");
        QVERIFY(directory->isValid());
        qputenv("XDG_CONFIG_HOME", directory->path().toUtf8());
        qputenv("XDG_DATA_HOME", (directory->path() + "/data").toUtf8());
    }

    void newDestinationPreservesConnectedSource() {
        Window window;
        window.timer.stop();
        QCOMPARE(window.source.profile->currentData().toString(), QString{});
        QCOMPARE(window.destination.profile->currentData().toString(), QString{});
        enterProfile(window, window.source, true, "VPS", "vps.example.net");
        QString sourceId = window.selectedProfile(window.source).id;
        QCOMPARE(window.destination.profile->currentData().toString(), QString{});

        // Creating a destination connection must preserve an opened source listing.
        window.source.loadedProfile = sourceId;
        window.source.loadedPath = "/srv/files";
        window.source.path->setText("/srv/files");
        new QTreeWidgetItem(window.source.tree, {"Keep this listing"});
        int generation = window.source.generation;
        enterProfile(window, window.destination, true, "Cabin", "cabin.example.net");

        QCOMPARE(window.profiles.size(), 2);
        QCOMPARE(window.selectedProfile(window.source).id, sourceId);
        QCOMPARE(window.selectedProfile(window.source).host, "vps.example.net");
        QCOMPARE(window.selectedProfile(window.destination).host, "cabin.example.net");
        QVERIFY(window.selectedProfile(window.destination).id != sourceId);
        QCOMPARE(window.source.loadedProfile, sourceId);
        QCOMPARE(window.source.loadedPath, "/srv/files");
        QCOMPARE(window.source.tree->topLevelItemCount(), 1);
        QCOMPARE(window.source.generation, generation);
        QCOMPARE(window.source.path->text(), "/srv/files");
    }

    void connectionsDialogCreatesEditsAndAssignsWithoutImplicitPaneChanges() {
        Window window;
        window.timer.stop();
        QVERIFY(!window.findChild<QPushButton *>("sourceNewConnection"));
        QVERIFY(!window.findChild<QPushButton *>("destinationEditConnection"));
        QTimer::singleShot(0, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            Q_ASSERT(dialog);
            auto close = qScopeGuard([dialog] { dialog->reject(); });
            QCOMPARE(dialog->objectName(), "connectionsDialog");
            auto list = dialog->findChild<QListWidget *>("savedConnections");
            auto create = dialog->findChild<QPushButton *>("newConnection");
            auto edit = dialog->findChild<QPushButton *>("editConnection");
            auto useSource = dialog->findChild<QPushButton *>("useConnectionSource");
            auto useDestination = dialog->findChild<QPushButton *>("useConnectionDestination");
            QVERIFY(list && create && edit && useSource && useDestination);
            QVERIFY(!edit->isEnabled());
            QVERIFY(!useSource->isEnabled());
            QVERIFY(!useDestination->isEnabled());
            scheduleProfileDetails(window, "VPS", "vps.example.net");
            create->click();
            QCOMPARE(list->count(), 1);
            QCOMPARE(window.profiles.size(), 1);
            QCOMPARE(window.source.profile->currentData().toString(), QString{});
            QCOMPARE(window.destination.profile->currentData().toString(), QString{});
            QVERIFY(edit->isEnabled());
            useSource->click();
            QString sourceId = window.selectedProfile(window.source).id;
            window.source.loadedPath = "/source";
            window.source.loadedProfile = sourceId;
            window.displayDirectory(window.source, {{"keep", 1, 1, SSH_FILEXFER_TYPE_REGULAR}});
            scheduleProfileDetails(window, "Cabin", "cabin.example.net");
            create->click();
            QCOMPARE(list->count(), 2);
            QCOMPARE(window.selectedProfile(window.source).id, sourceId);
            QCOMPARE(window.destination.profile->currentData().toString(), QString{});
            useDestination->click();
            QString destinationId = window.selectedProfile(window.destination).id;
            QVERIFY(destinationId != sourceId);
            scheduleProfileDetails(window, "Updated cabin", "updated.example.net");
            edit->click();
            QCOMPARE(window.profiles.size(), 2);
            QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), destinationId);
            QVERIFY(list->currentItem()->text().contains("Updated cabin"));
            QCOMPARE(window.selectedProfile(window.destination).host, "updated.example.net");
            QCOMPARE(window.selectedProfile(window.source).host, "vps.example.net");
            QCOMPARE(window.source.tree->topLevelItemCount(), 1);
            QCOMPARE(window.source.loadedPath, "/source");
            QTimer::singleShot(0, &window, [] {
                auto editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                Q_ASSERT(editor);
                editor->reject();
            });
            create->click();
            QCOMPARE(list->count(), 2);
            QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), destinationId);
        });
        window.findChild<QPushButton *>("manageConnections")->click();
    }

    void editingSharedSavedConnectionUpdatesBothPanes() {
        Window window;
        window.timer.stop();
        enterProfile(window, window.source, false, "VPS", "vps.example.net");
        QString id = window.selectedProfile(window.source).id;
        window.destination.profile->setCurrentIndex(window.destination.profile->findData(id));
        enterProfile(window, window.destination, true, "Updated VPS", "updated.example.net");
        QCOMPARE(window.profiles.size(), 1);
        QCOMPARE(window.selectedProfile(window.source).id, id);
        QCOMPARE(window.selectedProfile(window.source).host, "updated.example.net");
        QCOMPARE(window.selectedProfile(window.destination).id, id);
        QCOMPARE(window.selectedProfile(window.destination).host, "updated.example.net");
    }

    void selectionsSurviveRestartWithoutRewritingLegacyProfiles() {
        QString sourceId, destinationId;
        {
            Window window;
            window.timer.stop();
            enterProfile(window, window.destination, false, "Cabin", "cabin.example.net");
            QCOMPARE(window.source.profile->currentData().toString(), QString{});
            enterProfile(window, window.source, false, "VPS", "vps.example.net");
            sourceId = window.selectedProfile(window.source).id;
            destinationId = window.selectedProfile(window.destination).id;
            window.save();
        }
        Window reopened;
        reopened.timer.stop();
        QCOMPARE(reopened.selectedProfile(reopened.source).id, sourceId);
        QCOMPARE(reopened.selectedProfile(reopened.destination).id, destinationId);
        QCOMPARE(reopened.profiles.size(), 2);
        QVERIFY(reopened.source.profile->findData(destinationId) > 0);
        QVERIFY(reopened.destination.profile->findData(sourceId) > 0);
    }

    void changingConnectionKeepsRecoverableBackup() {
        Window window;
        window.timer.stop();
        enterProfile(window, window.source, false, "VPS", "vps.example.net");
        enterProfile(window, window.source, true, "Updated VPS", "new-vps.example.net");
        auto backups = QDir(QFileInfo(window.configPath).absolutePath())
                           .entryList({"connections-*.json.bak"}, QDir::Files);
        QVERIFY(!backups.isEmpty());
        bool foundOriginal = false;
        for (const QString &filename : backups) {
            QFile file(QFileInfo(window.configPath).absolutePath() + "/" + filename);
            QVERIFY(file.open(QIODevice::ReadOnly));
            for (auto value :
                 QJsonDocument::fromJson(file.readAll()).object()["profiles"].toArray())
                if (value.toObject()["host"].toString() == "vps.example.net")
                    foundOriginal = true;
        }
        QVERIFY(foundOriginal);
    }

    void legacySingleConnectionIsPreservedWithoutAutoselection() {
        Profile cabin;
        cabin.id = "existing-cabin";
        cabin.name = "Cabin";
        cabin.host = "cabin.example.net";
        cabin.user = "test";
        QString path = directory->path() + "/LinFXP/LinFXP/state.json";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile state(path);
        QVERIFY(state.open(QIODevice::WriteOnly));
        QByteArray original = QJsonDocument(QJsonObject{{"profiles", QJsonArray{cabin.json()}},
                                                        {"jobs", QJsonArray{}}})
                                  .toJson();
        QCOMPARE(state.write(original), original.size());
        state.close();
        Window window;
        window.timer.stop();
        QCOMPARE(window.profiles.size(), 1);
        QCOMPARE(window.profiles.front().host, "cabin.example.net");
        QCOMPARE(window.source.profile->currentData().toString(), QString{});
        QCOMPARE(window.destination.profile->currentData().toString(), QString{});
        QVERIFY(window.source.profile->findData(cabin.id) > 0);
        QVERIFY(window.destination.profile->findData(cabin.id) > 0);
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), original);
    }

    void demoDoesNotWriteProfiles() {
        Window window;
        window.timer.stop();
        enterProfile(window, window.source, false, "Real VPS", "real.example.net");
        QFile state(window.configPath);
        QVERIFY(state.open(QIODevice::ReadOnly));
        QByteArray before = state.readAll();
        state.close();
        window.demo();
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), before);
    }

    void compareListingsByExactNameOnly() {
        Window window;
        window.timer.stop();
        window.source.loadedPath = "/source";
        window.destination.loadedPath = "/destination";
        window.displayDirectory(window.source,
                                {{"shared", 10, 1, SSH_FILEXFER_TYPE_REGULAR},
                                 {"folder", 0, 1, SSH_FILEXFER_TYPE_DIRECTORY},
                                 {"Source only", 3, 1, SSH_FILEXFER_TYPE_REGULAR},
                                 {"Case", 4, 1, SSH_FILEXFER_TYPE_REGULAR},
                                 {QString::fromUtf8("café.txt"), 5, 1, SSH_FILEXFER_TYPE_REGULAR}});
        window.displayDirectory(
            window.destination,
            {{"shared", 999, 99, SSH_FILEXFER_TYPE_DIRECTORY},
             {"folder", 500, 99, SSH_FILEXFER_TYPE_REGULAR},
             {"Destination only", 3, 1, SSH_FILEXFER_TYPE_REGULAR},
             {"case", 4, 1, SSH_FILEXFER_TYPE_REGULAR},
             {QString::fromUtf8("café.txt"), 20, 99, SSH_FILEXFER_TYPE_REGULAR}});
        auto compare = window.findChild<QPushButton *>("compareDirectories");
        QVERIFY(compare);
        compare->click();
        QVERIFY(window.comparisonActive);
        QVERIFY(window.statusBar()->currentMessage().contains("2 source-only, 2 destination-only"));
        for (auto pane : {&window.source, &window.destination}) {
            pane->tree->sortItems(1, Qt::DescendingOrder);
            for (int row = 0; row < pane->tree->topLevelItemCount(); ++row) {
                auto item = pane->tree->topLevelItem(row);
                bool unique = item->text(0).contains("only") ||
                              item->text(0).compare("case", Qt::CaseInsensitive) == 0;
                for (int column = 0; column < 3; ++column)
                    QCOMPARE(item->data(column, Qt::BackgroundRole).isValid(), unique);
                int index = item->data(0, Qt::UserRole).toInt();
                QCOMPARE(pane->entries[index].name, item->text(0));
            }
        }
        compare->click();
        QVERIFY(window.statusBar()->currentMessage().contains("2 source-only, 2 destination-only"));
        window.displayDirectory(window.destination, {});
        QVERIFY(!window.comparisonActive);
        for (int row = 0; row < window.source.tree->topLevelItemCount(); ++row)
            QVERIFY(!window.source.tree->topLevelItem(row)->data(0, Qt::BackgroundRole).isValid());
        compare->click();
        QVERIFY(window.statusBar()->currentMessage().contains("5 source-only, 0 destination-only"));
        window.profileChanged(window.source);
        QVERIFY(!window.comparisonActive);
    }

    void comparisonRequiresLoadedIdleListingsAndHandlesEmptyDirectories() {
        Window window;
        window.timer.stop();
        window.compareDirectories();
        QVERIFY(!window.comparisonActive);
        window.source.loadedPath = "/empty-source";
        window.destination.loadedPath = "/empty-destination";
        window.compareDirectories();
        QVERIFY(window.comparisonActive);
        QVERIFY(window.statusBar()->currentMessage().contains("0 source-only, 0 destination-only"));
        window.source.busy = true;
        window.compareDirectories();
        QVERIFY(!window.comparisonActive);
        window.source.busy = false;
        window.destination.busy = true;
        window.compareDirectories();
        QVERIFY(!window.comparisonActive);
        window.destination.busy = false;
    }

    void directoryMetadataStaysAtRightEdgeWhileNameExpands() {
        Window window;
        window.timer.stop();
        window.show();
        QApplication::processEvents();
        for (auto pane : {&window.source, &window.destination}) {
            window.displayDirectory(
                *pane, {{QString(100, 'x'), 1024, 1700000100, SSH_FILEXFER_TYPE_REGULAR}});
            auto header = pane->tree->header();
            QVERIFY(!header->stretchLastSection());
            QCOMPARE(header->sectionResizeMode(0), QHeaderView::Stretch);
            for (int column : {1, 2}) {
                QCOMPARE(header->sectionResizeMode(column), QHeaderView::Interactive);
                QVERIFY(pane->tree->headerItem()->textAlignment(column) & Qt::AlignRight);
                QVERIFY(pane->tree->topLevelItem(0)->textAlignment(column) & Qt::AlignRight);
            }
            int nameWidth = header->sectionSize(0);
            header->resizeSection(1, header->sectionSize(1) + 20);
            QApplication::processEvents();
            QCOMPARE(header->sectionSize(0), nameWidth - 20);
        }
        int sourceNameWidth = window.source.tree->columnWidth(0);
        int destinationNameWidth = window.destination.tree->columnWidth(0);
        window.resize(window.width() + 400, window.height());
        QApplication::processEvents();
        QVERIFY(window.source.tree->columnWidth(0) > sourceNameWidth);
        QVERIFY(window.destination.tree->columnWidth(0) > destinationNameWidth);
        for (auto pane : {&window.source, &window.destination}) {
            auto header = pane->tree->header();
            QCOMPARE(header->sectionViewportPosition(2) + header->sectionSize(2),
                     pane->tree->viewport()->width());
        }
    }

    void directoryHeadersSortRawValuesAndReverse() {
        Window window;
        window.timer.stop();
        window.show();
        QVector<Entry> entries{{"zeta", 2048, 1700000300, SSH_FILEXFER_TYPE_REGULAR},
                               {"alpha", 10240, 1700000100, SSH_FILEXFER_TYPE_REGULAR},
                               {"beta", 100, 1700000200, SSH_FILEXFER_TYPE_REGULAR}};
        for (auto pane : {&window.source, &window.destination}) {
            window.displayDirectory(*pane, entries);
            QCOMPARE(pane->tree->topLevelItem(0)->text(0), "alpha");
            auto header = pane->tree->header();
            for (int column : {0, 1, 2}) {
                auto click = [&] {
                    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                                      QPoint(header->sectionViewportPosition(column) +
                                                 header->sectionSize(column) / 2,
                                             header->height() / 2));
                };
                click();
                auto firstOrder = header->sortIndicatorOrder();
                QCOMPARE(header->sortIndicatorSection(), column);
                QString first = column == 1 ? "beta" : "alpha",
                        last = column == 1 ? "alpha" : "zeta";
                QCOMPARE(pane->tree->topLevelItem(0)->text(0),
                         firstOrder == Qt::AscendingOrder ? first : last);
                click();
                QVERIFY(header->sortIndicatorOrder() != firstOrder);
                QCOMPARE(pane->tree->topLevelItem(0)->text(0),
                         firstOrder == Qt::AscendingOrder ? last : first);
                int entryIndex = pane->tree->topLevelItem(0)->data(0, Qt::UserRole).toInt();
                QCOMPARE(pane->entries[entryIndex].name, pane->tree->topLevelItem(0)->text(0));
                window.displayDirectory(*pane, entries);
                QCOMPARE(header->sortIndicatorSection(), column);
                QCOMPARE(pane->tree->topLevelItem(0)->text(0),
                         firstOrder == Qt::AscendingOrder ? last : first);
            }
        }
    }

    void foldersShowIndividualFilesAndRemoveOnlySelectedOnes() {
        Window window;
        window.timer.stop();
        queueFixture(window);
        QCOMPARE(window.jobs.size(), 1);
        QCOMPARE(window.queue->topLevelItemCount(), 3);
        QCOMPARE(window.queue->topLevelItem(1)->text(0), "folder/sub/b.txt");
        window.queue->topLevelItem(0)->setSelected(true);
        window.queue->topLevelItem(2)->setSelected(true);
        window.findChild<QPushButton *>("removeQueueItems")->click();
        QCOMPARE(window.queue->topLevelItemCount(), 1);
        QCOMPARE(window.queue->topLevelItem(0)->text(0), "folder/sub/b.txt");
        QCOMPARE(window.jobs[0].toObject()["files"].toArray().size(), 1);
        QCOMPARE(window.queue->selectedItems().size(), 0);
        {
            Window reopened;
            reopened.timer.stop();
            QCOMPARE(reopened.queue->topLevelItemCount(), 1);
            QCOMPARE(reopened.queue->topLevelItem(0)->text(0), "folder/sub/b.txt");
        }
        window.queue->topLevelItem(0)->setSelected(true);
        window.removeSelected();
        QCOMPARE(window.jobs.size(), 0);
        QCOMPARE(window.queue->topLevelItemCount(), 0);
    }

    void contextMenusAndSimplifiedLabels() {
        Window window;
        window.timer.stop();
        int connectButtons = 0;
        for (auto button : window.findChildren<QPushButton *>()) {
            QVERIFY(button->text() != "Open");
            connectButtons += button->text() == "Connect";
        }
        QCOMPARE(connectButtons, 2);
        for (auto label : window.findChildren<QLabel *>()) {
            QVERIFY(!label->text().contains("Choose This computer or a saved server"));
            QVERIFY(!label->text().contains("Add your VPS and cabin"));
        }
        QVERIFY(!window.statusBar()->currentMessage().contains("cabin"));
        QMenu emptySource, emptyQueue;
        window.directoryActions(emptySource, window.source);
        window.queueActions(emptyQueue);
        QVERIFY(!emptySource.findChild<QAction *>("contextQueue")->isEnabled());
        QVERIFY(!emptySource.findChild<QAction *>("contextDequeueSource")->isEnabled());
        QVERIFY(!emptyQueue.findChild<QAction *>("contextDequeue")->isEnabled());
        queueFixture(window);
        window.queue->topLevelItem(0)->setSelected(true);
        window.queue->topLevelItem(2)->setSelected(true);
        QMenu menu;
        window.queueActions(menu);
        menu.findChild<QAction *>("contextCopyDestinations")->trigger();
        QCOMPARE(QApplication::clipboard()->text(),
                 QString("/downloads/folder/a.txt\n/downloads/folder/sub/c.txt"));
        menu.findChild<QAction *>("contextDequeue")->trigger();
        QCOMPARE(window.queue->topLevelItemCount(), 1);
        QCOMPARE(window.queue->topLevelItem(0)->text(0), "folder/sub/b.txt");
    }

    void sourceDequeueMatchesPathsFoldersAndSourceConnection() {
        Window window;
        window.timer.stop();
        auto to = queueFixture(window);
        window.source.profile->setCurrentIndex(window.source.profile->findData("source"));
        auto from = window.selectedProfile(window.source);
        window.source.loadedProfile = from.id;
        window.source.loadedPath = "/folder/sub";
        window.source.path->setText(window.source.loadedPath);
        window.displayDirectory(window.source, {{"b.txt", 1, 1, SSH_FILEXFER_TYPE_REGULAR}});
        // Identical names on another server must never be dequeued.
        auto other = from;
        other.id = "other-source";
        other.host = "other.example.net";
        window.queueManifest(manifest(), other, to, "/other", "other source");
        window.source.tree->topLevelItem(0)->setSelected(true);
        QMenu single;
        window.directoryActions(single, window.source);
        auto dequeue = single.findChild<QAction *>("contextDequeueSource");
        QVERIFY(dequeue->isEnabled());
        single.findChild<QAction *>("contextCopyPaths")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QString("/folder/sub/b.txt"));
        dequeue->trigger();
        QCOMPARE(window.jobs.size(), 2);
        QCOMPARE(window.jobs[0].toObject()["files"].toArray().size(), 2);
        QCOMPARE(window.jobs[1].toObject()["files"].toArray().size(), 3);
        window.source.loadedPath = "/";
        window.source.path->setText("/");
        window.displayDirectory(window.source, {{"folder", 0, 1, SSH_FILEXFER_TYPE_DIRECTORY}});
        window.source.tree->topLevelItem(0)->setSelected(true);
        QCOMPARE(window.queuedSourceItems().size(), 2);
        window.source.path->setText("/unopened");
        QVERIFY(window.queuedSourceItems().isEmpty());
        window.source.path->setText("/");
        window.dequeueSourceSelection();
        QCOMPARE(window.jobs.size(), 1);
        QCOMPARE(window.jobs[0].toObject()["source_profile"].toObject()["id"].toString(), other.id);
        window.jobs = {};
        window.renderQueue();
        QJsonObject emptyFolder{{"files", QJsonArray{}}, {"directories", QJsonArray{"folder"}}};
        window.queueManifest(emptyFolder, from, to, "/downloads", "empty folder", "/");
        QCOMPARE(window.queuedSourceItems().size(), 1);
        window.dequeueSourceSelection();
        QVERIFY(window.jobs.isEmpty());
    }

    void rightClickPreservesSelectionAndTargetsUnselectedRows() {
        Window window;
        window.timer.stop();
        queueFixture(window);
        window.show();
        QApplication::processEvents();
        auto tree = window.queue;
        tree->topLevelItem(0)->setSelected(true);
        tree->topLevelItem(1)->setSelected(true);
        auto request = [&](int row, int selectedCount) {
            QTimer::singleShot(0, &window, [&, selectedCount] {
                auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                Q_ASSERT(menu);
                auto close = qScopeGuard([menu] { menu->close(); });
                QCOMPARE(tree->selectedItems().size(), selectedCount);
            });
            tree->customContextMenuRequested(
                tree->visualItemRect(tree->topLevelItem(row)).center());
        };
        request(1, 2);
        request(2, 1);
        QCOMPARE(tree->selectedItems().first(), tree->topLevelItem(2));
    }

    void individualProgressAndTerminalRemovalKeepStableIndices() {
        Window window;
        window.timer.stop();
        queueFixture(window);
        auto job = window.jobs[0].toObject();
        job["state"] = "Running";
        job["status"] = QJsonObject{{"completed", QJsonArray{"0"}},
                                    {"current", "folder/sub/b.txt"},
                                    {"offsets", QJsonObject{{"1", 1024}}}};
        window.jobs[0] = job;
        window.renderQueue();
        QCOMPARE(window.queue->topLevelItem(0)->text(2), "Completed");
        QCOMPARE(window.queue->topLevelItem(1)->text(2), "Running");
        QCOMPARE(window.queue->topLevelItem(2)->text(2), "Remote queue");
        QCOMPARE(window.queue->topLevelItem(1)->data(3, Qt::UserRole).toInt(), 500);
        job["state"] = "Failed";
        window.jobs[0] = job;
        window.renderQueue();
        window.queue->topLevelItem(1)->setSelected(true);
        window.removeSelected();
        QCOMPARE(window.queue->topLevelItemCount(), 2);
        QCOMPARE(window.jobs[0].toObject()["files"].toArray().size(), 3);
        QVERIFY(window.jobs[0].toObject()["removed_files"].toArray().contains(1));
        QCOMPARE(window.queue->topLevelItem(1)->data(0, Qt::UserRole + 1).toInt(), 2);
    }

    void destinationRefreshWaitsForAllSubmittedBatchesAndBrowsing() {
        Window window;
        window.timer.stop();
        Profile to = queueFixture(window);
        auto first = window.jobs[0].toObject();
        first["state"] = "Running";
        window.jobs[0] = first;
        auto second = first;
        second["id"] = "other-batch";
        window.jobs.append(second);
        window.destination.busy = true;
        window.applyStatuses({{first["id"].toString(), QJsonObject{{"state", "Completed"}}}});
        QVERIFY(window.destinationRefreshPending);
        QVERIFY(!window.destinationRefreshReady());
        window.destination.busy = false;
        QVERIFY(!window.destinationRefreshReady());
        window.destination.busy = true;
        window.applyStatuses({{"other-batch", QJsonObject{{"state", "Completed"}}}});
        QVERIFY(!window.destinationRefreshReady());
        window.destination.busy = false;
        QVERIFY(window.destinationRefreshReady());
        window.destination.profile->setCurrentIndex(0);
        QVERIFY(!window.destinationRefreshPending);
        QVERIFY(!window.destinationRefreshReady());
    }

    void destinationAutoRefreshLoadsLocalSftp() {
        if (!qEnvironmentVariableIsSet("LINFXP_TEST_PORT"))
            QSKIP("This integration check is invoked by the localhost SSH test suite");
        Window window;
        window.timer.stop();
        Profile to;
        to.id = "localhost";
        to.name = "Local test";
        to.host = "127.0.0.1";
        to.user = "test";
        to.secret = "test-secret";
        to.port = qEnvironmentVariableIntValue("LINFXP_TEST_PORT");
        window.profiles = {to};
        window.refreshProfiles(&window.destination, to.id);
        window.destination.loadedProfile = to.id;
        window.destination.loadedPath = "/";
        window.destination.path->setText("/");
        new QTreeWidgetItem(window.destination.tree, {"Stale listing"});
        window.queueManifest(manifest(), {}, to, "/", "folder");
        auto job = window.jobs[0].toObject();
        job["state"] = "Running";
        window.jobs[0] = job;
        window.applyStatuses({{job["id"].toString(), QJsonObject{{"state", "Completed"}}}});
        QVERIFY(window.destination.busy);
        QTRY_VERIFY_WITH_TIMEOUT(!window.destination.busy, 10000);
        QVERIFY2(
            window.destination.tree->findItems("autorefresh-proof.bin", Qt::MatchExactly).size() ==
                1,
            qPrintable(window.destination.info->text()));
        QVERIFY(window.destination.tree->findItems("Stale listing", Qt::MatchExactly).isEmpty());
        QVERIFY(!window.destinationRefreshPending);
        QCOMPARE(window.tasks, 0);
    }

    void localPaneBrowsingAndQueueUseDesktopEngine() {
        QString source = directory->path() + "/source", target = directory->path() + "/target";
        QVERIFY(QDir().mkpath(source + "/folder/empty"));
        QVERIFY(QDir().mkpath(target));
        QFile file(source + "/folder/test.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("local transfer data");
        file.close();
        Window window;
        window.timer.stop();
        window.source.profile->setCurrentIndex(window.source.profile->findData("__local__"));
        window.destination.profile->setCurrentIndex(
            window.destination.profile->findData("__local__"));
        window.browse(window.source, source);
        window.browse(window.destination, target);
        QTRY_COMPARE_WITH_TIMEOUT(window.tasks, 0, 5000);
        QCOMPARE(window.source.loadedPath, source);
        QCOMPARE(window.destination.loadedPath, target);
        QCOMPARE(window.source.tree->topLevelItemCount(), 1);
        window.source.tree->topLevelItem(0)->setSelected(true);
        QMenu sourceMenu;
        window.directoryActions(sourceMenu, window.source);
        auto enqueue = sourceMenu.findChild<QAction *>("contextQueue");
        QVERIFY(enqueue->isEnabled());
        enqueue->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.jobs.size(), 1, 5000);
        QCOMPARE(window.jobs[0].toObject()["engine"].toString(), "desktop");
        QCOMPARE(window.queue->topLevelItem(0)->text(5), "Local copy");
        window.startQueue();
        QTRY_COMPARE_WITH_TIMEOUT(window.tasks, 0, 5000);
        QCOMPARE(window.jobs[0].toObject()["state"].toString(), "Completed");
        QVERIFY(QDir(target + "/folder/empty").exists());
        QFile copy(target + "/folder/test.txt");
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("local transfer data"));
        window.dequeueSourceSelection();
        QVERIFY(window.jobs.isEmpty());
        QVERIFY(QFile::exists(target + "/folder/test.txt"));
        QVERIFY(window.destination.tree->findItems("folder", Qt::MatchExactly).size() == 1);
        {
            Window reopened;
            reopened.timer.stop();
            QCOMPARE(reopened.source.profile->currentData().toString(), "__local__");
            QCOMPARE(reopened.destination.profile->currentData().toString(), "__local__");
            QCOMPARE(reopened.source.path->text(), source);
            QCOMPARE(reopened.destination.path->text(), target);
        }
    }
};

int main(int argc, char **argv) {
    QTemporaryDir initial("/tmp/linfxp-initial-XXXXXX");
    qputenv("XDG_CONFIG_HOME", initial.path().toUtf8());
    QApplication application(argc, argv);
    application.setOrganizationName("LinFXP");
    application.setApplicationName("LinFXP");
    ConnectionProfileTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_profiles.moc"
