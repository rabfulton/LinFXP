#include "ssh.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <fcntl.h>
#include <libsecret/secret.h>
#include <stdexcept>

namespace {
[[noreturn]] void fail(const QString &s) {
    throw std::runtime_error(s.toStdString());
}
const SecretSchema schema = {
    "org.linfxp.Connection",
    SECRET_SCHEMA_NONE,
    {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr};
} // namespace
QString loadSecret(const QString &id) {
    GError *error = nullptr;
    gchar *password = secret_password_lookup_sync(&schema, nullptr, &error, "id",
                                                  id.toUtf8().constData(), nullptr);
    if (error) {
        QString message = QString::fromUtf8(error->message);
        g_error_free(error);
        fail("Desktop keyring: " + message);
    }
    QString result = QString::fromUtf8(password ? password : "");
    secret_password_free(password);
    return result;
}
void saveSecret(const QString &id, const QString &value) {
    GError *error = nullptr;
    bool ok =
        value.isEmpty()
            ? secret_password_clear_sync(&schema, nullptr, &error, "id", id.toUtf8().constData(),
                                         nullptr)
            : secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "LinFXP connection",
                                         value.toUtf8().constData(), nullptr, &error, "id",
                                         id.toUtf8().constData(), nullptr);
    if (error) {
        QString message = QString::fromUtf8(error->message);
        g_error_free(error);
        fail("Desktop keyring: " + message);
    }
    if (!ok && !value.isEmpty())
        fail("The desktop keyring could not save this credential.");
}
QJsonObject Profile::json() const {
    return {{"id", id},      {"name", name},         {"host", host},
            {"user", user},  {"key", key},           {"path", path},
            {"port", port},  {"password", password}, {"remember", remember},
            {"local", local}};
}
Profile Profile::thisComputer() {
    Profile p;
    p.id = "__local__";
    p.name = "This computer";
    p.local = true;
    p.path = QDir::homePath();
    return p;
}
Profile Profile::fromJson(const QJsonObject &o) {
    Profile p;
    p.id = o["id"].toString();
    p.name = o["name"].toString();
    p.host = o["host"].toString();
    p.user = o["user"].toString();
    p.key = o["key"].toString();
    p.path = o["path"].toString(".");
    p.port = o["port"].toInt(22);
    p.password = o["password"].toBool(true);
    p.remember = o["remember"].toBool(false);
    p.local = o["local"].toBool(false);
    return p;
}
QString remoteJoin(const QString &root, const QString &name) {
    return (root == "/" ? root : root + "/") + name;
}
QString shellQuote(QString value) {
    value.replace("'", "'\\''");
    return "'" + value + "'";
}
Connection::Connection(const Profile &p, const Trust &trust) {
    local = p.local;
    if (local) {
        home = QDir::homePath();
        return;
    }
    ssh = ssh_new();
    if (!ssh)
        fail("Could not allocate SSH session");
    try {
        long timeout = 20;
        bool config = false;
        unsigned port = p.port;
        ssh_options_set(ssh, SSH_OPTIONS_PROCESS_CONFIG, &config);
        ssh_options_set(ssh, SSH_OPTIONS_HOST, p.host.toUtf8().constData());
        ssh_options_set(ssh, SSH_OPTIONS_USER, p.user.toUtf8().constData());
        ssh_options_set(ssh, SSH_OPTIONS_PORT, &port);
        ssh_options_set(ssh, SSH_OPTIONS_TIMEOUT, &timeout);
        // Use the application's HOME explicitly, including in isolated tests.
        QByteArray knownHosts = (QDir::homePath() + "/.ssh/known_hosts").toUtf8();
        ssh_options_set(ssh, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());
        if (ssh_connect(ssh) != SSH_OK)
            fail(QString::fromUtf8(ssh_get_error(ssh)));
        auto state = ssh_session_is_known_server(ssh);
        if (state == SSH_KNOWN_HOSTS_CHANGED || state == SSH_KNOWN_HOSTS_OTHER)
            fail("SSH host key changed for " + p.host +
                 ". Verify the server and update your known_hosts file before reconnecting.");
        if (state == SSH_KNOWN_HOSTS_ERROR)
            fail(QString::fromUtf8(ssh_get_error(ssh)));
        if (state != SSH_KNOWN_HOSTS_OK) {
            ssh_key key = nullptr;
            unsigned char *hash = nullptr;
            size_t length = 0;
            if (ssh_get_server_publickey(ssh, &key) != SSH_OK)
                fail("Cannot read host key");
            int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length);
            ssh_key_free(key);
            if (rc != SSH_OK)
                fail("Cannot fingerprint host key");
            char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
            QString fp = QString::fromUtf8(fingerprint);
            ssh_string_free_char(fingerprint);
            ssh_clean_pubkey_hash(&hash);
            if (!trust(p.host + ":" + QString::number(p.port), fp))
                fail("Host key was not accepted");
            if (!QDir().mkpath(QDir::homePath() + "/.ssh"))
                fail("Could not create ~/.ssh");
            if (ssh_session_update_known_hosts(ssh) != SSH_OK)
                fail("Could not save host key to ~/.ssh/known_hosts");
        }
        int auth = SSH_AUTH_DENIED;
        if (p.password) {
            auth = ssh_userauth_password(ssh, nullptr, p.secret.toUtf8().constData());
        } else if (!p.key.isEmpty()) {
            ssh_key key = nullptr;
            if (ssh_pki_import_privkey_file(p.key.toUtf8().constData(),
                                            p.secret.toUtf8().constData(), nullptr, nullptr,
                                            &key) != SSH_OK)
                fail("Cannot read private key. Check its path and passphrase.");
            auth = ssh_userauth_publickey(ssh, nullptr, key);
            ssh_key_free(key);
        } else {
            auth = ssh_userauth_publickey_auto(ssh, nullptr, p.secret.toUtf8().constData());
        }
        if (auth != SSH_AUTH_SUCCESS)
            fail("Authentication failed for " + p.user + "@" + p.host);
        sftp = sftp_new(ssh);
        if (!sftp || sftp_init(sftp) != SSH_OK)
            fail("Cannot start SFTP: " + QString::fromUtf8(ssh_get_error(ssh)));
        home = canonical(".");
    } catch (...) {
        if (sftp)
            sftp_free(sftp);
        ssh_disconnect(ssh);
        ssh_free(ssh);
        throw;
    }
}
Connection::~Connection() {
    if (local)
        return;
    sftp_free(sftp);
    ssh_disconnect(ssh);
    ssh_free(ssh);
}
QString Connection::canonical(const QString &path) {
    if (local) {
        QFileInfo info(path);
        if (!info.isDir())
            fail("Cannot open local directory: " + path);
        return info.canonicalFilePath();
    }
    char *result = sftp_canonicalize_path(sftp, path.toUtf8().constData());
    if (!result)
        fail("Cannot open directory: " + path);
    QString value = QString::fromUtf8(result);
    ssh_string_free_char(result);
    return value;
}
QVector<Entry> Connection::list(const QString &path) {
    if (local) {
        QFileInfo info(path);
        if (!info.isDir() || !info.isReadable())
            fail("Cannot list local directory: " + path);
        QVector<Entry> entries;
        for (const auto &file : QDir(path).entryInfoList(
                 QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                 QDir::DirsFirst | QDir::Name | QDir::IgnoreCase))
            entries.append({file.fileName(), quint64(file.size()),
                            quint32(file.lastModified().toSecsSinceEpoch()),
                            file.isSymbolicLink() ? SSH_FILEXFER_TYPE_SYMLINK
                            : file.isDir()        ? SSH_FILEXFER_TYPE_DIRECTORY
                            : file.isFile()       ? SSH_FILEXFER_TYPE_REGULAR
                                                  : SSH_FILEXFER_TYPE_UNKNOWN});
        return entries;
    }
    sftp_dir directory = sftp_opendir(sftp, path.toUtf8().constData());
    if (!directory)
        fail("Cannot list directory: " + path);
    QVector<Entry> result;
    while (auto attr = sftp_readdir(sftp, directory)) {
        QString name = QString::fromUtf8(attr->name);
        if (name != "." && name != "..")
            result.push_back({name, attr->size, attr->mtime, attr->type});
        sftp_attributes_free(attr);
    }
    bool eof = sftp_dir_eof(directory);
    sftp_closedir(directory);
    if (!eof)
        fail("Directory listing interrupted: " + path);
    std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
        bool ad = a.type == SSH_FILEXFER_TYPE_DIRECTORY, bd = b.type == SSH_FILEXFER_TYPE_DIRECTORY;
        return ad != bd ? ad : QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    return result;
}
QString Connection::knownHost() {
    if (local)
        return {};
    char *entry = nullptr;
    if (ssh_session_export_known_hosts_entry(ssh, &entry) != SSH_OK)
        fail("Cannot export source host key");
    QString result = QString::fromUtf8(entry);
    ssh_string_free_char(entry);
    return result;
}
void Connection::scan(const QString &path, const QString &relative, QJsonArray &files,
                      QJsonArray &dirs, int &skipped) {
    if (relative.count('/') > 128)
        fail("Directory nesting exceeds 128 levels");
    for (const Entry &e : list(path)) {
        QString rel = remoteJoin(relative, e.name), source = remoteJoin(path, e.name);
        if (e.type == SSH_FILEXFER_TYPE_DIRECTORY) {
            dirs.append(rel);
            scan(source, rel, files, dirs, skipped);
        } else if (e.type == SSH_FILEXFER_TYPE_REGULAR)
            files.append(QJsonObject{{"source", source},
                                     {"relative", rel},
                                     {"size", double(e.size)},
                                     {"mtime", double(e.mtime)}});
        else
            ++skipped;
    }
}
QJsonObject Connection::manifest(const QString &root, const QVector<Entry> &selected) {
    QJsonArray files, dirs;
    int skipped = 0;
    QString base = canonical(root);
    for (const Entry &e : selected) {
        if (e.type == SSH_FILEXFER_TYPE_DIRECTORY) {
            dirs.append(e.name);
            scan(remoteJoin(base, e.name), e.name, files, dirs, skipped);
        } else if (e.type == SSH_FILEXFER_TYPE_REGULAR)
            files.append(QJsonObject{{"source", remoteJoin(base, e.name)},
                                     {"relative", e.name},
                                     {"size", double(e.size)},
                                     {"mtime", double(e.mtime)}});
        else
            ++skipped;
    }
    return {
        {"files", files}, {"directories", dirs}, {"known_host", knownHost()}, {"skipped", skipped}};
}
void Connection::writeFile(const QString &path, const QByteArray &data, int mode) {
    if (local)
        fail("This operation requires a remote connection");
    auto file = sftp_open(sftp, path.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (!file)
        fail("Cannot write remote file: " + path);
    qsizetype done = 0;
    while (done < data.size()) {
        int size = sftp_write(file, data.constData() + done,
                              std::min<qsizetype>(32768, data.size() - done));
        if (size <= 0) {
            sftp_close(file);
            fail("Remote write failed: " + path);
        }
        done += size;
    }
    if (sftp_close(file) != SSH_OK)
        fail("Remote file close failed");
}
QString Connection::uploadWorker(const QByteArray &data) {
    if (local)
        fail("This operation requires a remote connection");
    QString path = home;
    for (const QString &part : {QString(".local"), QString("share"), QString("linfxp")}) {
        path = remoteJoin(path, part);
        auto attr = sftp_lstat(sftp, path.toUtf8().constData());
        if (attr) {
            bool directory = attr->type == SSH_FILEXFER_TYPE_DIRECTORY;
            sftp_attributes_free(attr);
            if (!directory)
                fail("Worker path is not a real directory: " + path);
        } else if (sftp_mkdir(sftp, path.toUtf8().constData(), 0700) != SSH_OK)
            fail("Cannot create worker directory");
    }
    // Versioned uploads never replace the script an existing worker is running.
    QString target = remoteJoin(
        path, "worker-" +
                  QString::fromLatin1(
                      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) +
                  ".py");
    auto attr = sftp_lstat(sftp, target.toUtf8().constData());
    if (attr) {
        bool regular = attr->type == SSH_FILEXFER_TYPE_REGULAR;
        sftp_attributes_free(attr);
        if (!regular)
            fail("Worker script path is not a regular file");
        return target;
    }
    QString temporary = target + "." + QUuid::createUuid().toString(QUuid::Id128) + ".tmp";
    writeFile(temporary, data, 0600);
    if (sftp_rename(sftp, temporary.toUtf8().constData(), target.toUtf8().constData()) != SSH_OK) {
        sftp_unlink(sftp, temporary.toUtf8().constData());
        fail("Could not publish remote worker script");
    }
    return target;
}
QByteArray Connection::execute(const QString &command, const QByteArray &input) {
    if (local)
        fail("This operation requires a remote connection");
    ssh_channel channel = ssh_channel_new(ssh);
    if (!channel)
        fail("Cannot create SSH command channel");
    auto cleanup = [&] {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    };
    if (ssh_channel_open_session(channel) != SSH_OK ||
        ssh_channel_request_exec(channel, command.toUtf8().constData()) != SSH_OK) {
        cleanup();
        fail("Remote machine does not allow SSH command execution");
    }
    qsizetype offset = 0;
    while (offset < input.size()) {
        int written = ssh_channel_write(channel, input.constData() + offset,
                                        std::min<qsizetype>(32768, input.size() - offset));
        if (written <= 0) {
            cleanup();
            fail("Could not send remote job");
        }
        offset += written;
    }
    ssh_channel_send_eof(channel);
    QByteArray output, errors;
    QElapsedTimer deadline;
    deadline.start();
    char buffer[8192];
    while (!ssh_channel_is_eof(channel)) {
        if (deadline.elapsed() > 60000) {
            cleanup();
            fail("Remote command timed out; check the queue before retrying.");
        }
        for (int stream = 0; stream <= 1; ++stream) {
            int n = ssh_channel_read_timeout(channel, buffer, sizeof(buffer), stream, 1000);
            if (n < 0) {
                cleanup();
                fail("Remote command connection failed");
            }
            if (n > 0)
                (stream ? errors : output).append(buffer, n);
        }
    }
    uint32_t code = 0;
    int exitState = ssh_channel_get_exit_state(channel, &code, nullptr, nullptr);
    cleanup();
    if (exitState != SSH_OK)
        fail("Remote command ended without an exit status");
    if (code != 0)
        fail("Remote command failed (Python 3 and OpenSSH are required): " +
             QString::fromUtf8(errors));
    return output;
}
QJsonObject Connection::status(const QString &id, const QString &workerPath) {
    if (local)
        fail("This operation requires a remote connection");
    QString path = home + "/.local/share/linfxp/jobs/" + id + "/status.json";
    auto file = sftp_open(sftp, path.toUtf8().constData(), O_RDONLY, 0);
    if (!file)
        return {{"state", "Unknown"}};
    QByteArray data;
    char buffer[16384];
    int n;
    while ((n = sftp_read(file, buffer, sizeof(buffer))) > 0) {
        data.append(buffer, n);
        if (data.size() > 32 * 1024 * 1024) {
            sftp_close(file);
            fail("Job status exceeds size limit");
        }
    }
    sftp_close(file);
    if (n < 0)
        fail("Cannot read remote job status");
    auto result = QJsonDocument::fromJson(data);
    if (!result.isObject())
        fail("Invalid remote job status");
    auto status = result.object();
    QString state = status["state"].toString();
    if (!workerPath.isEmpty() &&
        (state == "Running" || state == "Starting" || state == "Remote queue")) {
        // Check the worker's lock, rather than relying on a PID that may be
        // reused after a reboot. Interrupted jobs can then be resumed safely.
        auto response =
            execute("python3 " + shellQuote(workerPath) + " --status",
                    QJsonDocument(QJsonObject{{"id", id}}).toJson(QJsonDocument::Compact));
        auto checked = QJsonDocument::fromJson(response);
        if (!checked.isObject())
            fail("Invalid remote worker status");
        status = checked.object();
    }
    return status;
}
void Connection::cancel(const QString &id) {
    writeFile(home + "/.local/share/linfxp/jobs/" + id + "/cancel", "cancel\n", 0600);
}
void Connection::skipFiles(const QString &id, const QJsonArray &indices) {
    const QString code =
        "import json,sys,os,fcntl; from pathlib import Path; "
        "p=json.load(sys.stdin); i=p['id']; "
        "assert len(i)==32 and all(c in '0123456789abcdef' for c in i); "
        "d=Path.home()/'.local/share/linfxp/jobs'/i; "
        "lock=open(d/'skip.lock','a'); fcntl.flock(lock,fcntl.LOCK_EX); "
        "f=d/'skip.json'; old=json.loads(f.read_text()) if f.exists() else []; "
        "items=sorted(set(old)|set(p['indices'])); "
        "t=d/'skip.new'; t.write_text(json.dumps(items)); t.chmod(0o600); os.replace(t,f)";
    execute("python3 -c " + shellQuote(code),
            QJsonDocument(QJsonObject{{"id", id}, {"indices", indices}})
                .toJson(QJsonDocument::Compact));
}
