#pragma once
#include "ssh.h"
#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QTimer>
#include <QTreeWidget>

class Window : public QMainWindow {
  public:
    Window();
    void demo();

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    friend class ConnectionProfileTests;
    struct Pane {
        QComboBox *profile;
        QLineEdit *path;
        QTreeWidget *tree;
        QLabel *info;
        QString loadedProfile, loadedPath;
        QVector<Entry> entries;
        bool busy = false;
        int generation = 0;
    } source, destination;
    QVector<Profile> profiles;
    QJsonArray jobs;
    QTreeWidget *queue;
    QLabel *summary;
    QTimer timer;
    QString configPath;
    int tasks = 0;
    bool polling = false, dispatching = false;
    bool destinationRefreshPending = false;
    QString activeDesktop;
    QWidget *makePane(Pane &p, const QString &title);
    void editProfile(const QString &id = {}, Pane *target = nullptr);
    void refreshProfiles(Pane *target = nullptr, const QString &selectedId = {},
                         const QString &changedId = {});
    void profileChanged(Pane &p);
    Profile selectedProfile(const Pane &p);
    Profile credentials(Profile p);
    Trust trust();
    void browse(Pane &p, const QString &path = {});
    void displayDirectory(Pane &p, const QVector<Entry> &entries);
    void addToQueue();
    void queueManifest(const QJsonObject &manifest, const Profile &from, const Profile &to,
                       const QString &target, const QString &label);
    void startQueue();
    void startDesktop(const QJsonObject &job, const Profile &from, const Profile &to);
    void poll();
    void applyStatuses(const QJsonObject &results);
    bool matchesDestination(const QJsonObject &job) const;
    bool destinationRefreshReady() const;
    void refreshDestinationIfReady();
    void removeSelected();
    void cancelSelected();
    void retrySelected();
    void renderQueue();
    void save();
    void task(const QString &label, std::function<QJsonObject()> work,
              std::function<void(const QJsonObject &, const QString &)> done);
    int jobIndex(const QString &id) const;
};
