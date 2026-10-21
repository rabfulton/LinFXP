#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

struct Profile {
    QString id, name, host, user, key, secret, path = ".";
    int port = 22;
    bool password = true;
    bool remember = false;
    bool local = false;
    static Profile thisComputer();
    QJsonObject json() const;
    static Profile fromJson(const QJsonObject &o);
};
struct Entry {
    QString name;
    quint64 size;
    quint32 mtime;
    int type;
};
using Trust = std::function<bool(const QString &, const QString &)>;
class Connection {
  public:
    Connection(const Profile &p, const Trust &trust);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    QVector<Entry> list(const QString &path);
    QString canonical(const QString &path);
    QString knownHost();
    QJsonObject manifest(const QString &root, const QVector<Entry> &selected);
    QString uploadWorker(const QByteArray &data);
    QByteArray execute(const QString &command, const QByteArray &input = {});
    QJsonObject status(const QString &id, const QString &workerPath);
    void cancel(const QString &id);
    void skipFiles(const QString &id, const QJsonArray &indices);

  private:
    friend class TransferEndpoint;
    bool local = false;
    ssh_session ssh = nullptr;
    sftp_session sftp = nullptr;
    QString home;
    void scan(const QString &path, const QString &relative, QJsonArray &files, QJsonArray &dirs,
              int &skipped);
    void writeFile(const QString &path, const QByteArray &data, int mode);
};
QString shellQuote(QString value);
QString remoteJoin(const QString &root, const QString &name);
QString loadSecret(const QString &id);
void saveSecret(const QString &id, const QString &value);
