#include "window.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QPushButton>
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

    void enterProfile(Window &window, const QString &button, const QString &name,
                      const QString &host) {
        QTimer::singleShot(0, &window, [name, host] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            Q_ASSERT(dialog);
            dialog->findChild<QLineEdit *>("connectionName")->setText(name);
            dialog->findChild<QLineEdit *>("connectionHost")->setText(host);
            dialog->findChild<QLineEdit *>("connectionUser")->setText("test");
            dialog->findChild<QCheckBox *>("rememberSecret")->setChecked(false);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        auto control = window.findChild<QPushButton *>(button);
        Q_ASSERT(control);
        control->click();
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
        enterProfile(window, "sourceEditConnection", "VPS", "vps.example.net");
        QString sourceId = window.selectedProfile(window.source).id;
        QCOMPARE(window.destination.profile->currentData().toString(), QString{});

        // Reproduce the original workflow: enter details through destination Edit.
        window.source.loadedProfile = sourceId;
        window.source.loadedPath = "/srv/files";
        window.source.path->setText("/srv/files");
        new QTreeWidgetItem(window.source.tree, {"Keep this listing"});
        int generation = window.source.generation;
        enterProfile(window, "destinationEditConnection", "Cabin", "cabin.example.net");

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

    void editingSharedConnectionCreatesIndependentCopy() {
        Window window;
        window.timer.stop();
        enterProfile(window, "sourceNewConnection", "VPS", "vps.example.net");
        QString id = window.selectedProfile(window.source).id;
        window.destination.profile->setCurrentIndex(window.destination.profile->findData(id));
        enterProfile(window, "destinationEditConnection", "Cabin", "cabin.example.net");
        QCOMPARE(window.profiles.size(), 2);
        QCOMPARE(window.selectedProfile(window.source).id, id);
        QCOMPARE(window.selectedProfile(window.source).host, "vps.example.net");
        QVERIFY(window.selectedProfile(window.destination).id != id);
        QCOMPARE(window.selectedProfile(window.destination).host, "cabin.example.net");
    }

    void selectionsSurviveRestartWithoutRewritingLegacyProfiles() {
        QString sourceId, destinationId;
        {
            Window window;
            window.timer.stop();
            enterProfile(window, "destinationNewConnection", "Cabin", "cabin.example.net");
            QCOMPARE(window.source.profile->currentData().toString(), QString{});
            enterProfile(window, "sourceNewConnection", "VPS", "vps.example.net");
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
        enterProfile(window, "sourceNewConnection", "VPS", "vps.example.net");
        enterProfile(window, "sourceEditConnection", "Updated VPS", "new-vps.example.net");
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
        enterProfile(window, "sourceNewConnection", "Real VPS", "real.example.net");
        QFile state(window.configPath);
        QVERIFY(state.open(QIODevice::ReadOnly));
        QByteArray before = state.readAll();
        state.close();
        window.demo();
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), before);
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
        window.addToQueue();
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
