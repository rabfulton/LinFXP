#include "transfer.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
[[noreturn]] void error(const QString &text) {
    throw std::runtime_error(text.toStdString());
}
QString jobPath(const QString &id) {
    if (!QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch())
        error("Invalid desktop transfer ID");
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
           "/desktop-jobs/" + id;
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly))
        error("Cannot read transfer state: " + file.errorString());
    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        error("Invalid desktop transfer state");
    return doc.object();
}
void writeJson(const QString &path, const QJsonObject &obj) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        error("Cannot write transfer state: " + file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    auto data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit())
        error("Cannot save desktop transfer state");
}
struct Metadata {
    quint64 size = 0;
    quint32 mtime = 0;
    unsigned mode = 0;
    bool exists = false;
};
Metadata fromStat(const struct stat &s) {
    return {quint64(s.st_size), quint32(s.st_mtime), unsigned(s.st_mode), true};
}
Metadata fromSftp(sftp_attributes a) {
    if (!a)
        error("Cannot read remote file attributes");
    Metadata m{a->size, a->mtime, a->permissions, true};
    sftp_attributes_free(a);
    return m;
}
struct Async {
    sftp_aio handle = nullptr;
    qsizetype count = 0;
    qsizetype offset = 0;
    ~Async() {
        if (handle)
            sftp_aio_free(handle);
    }
};
class File {
  public:
    int fd = -1;
    sftp_file remote = nullptr;
    sftp_session session = nullptr;
    ~File() {
        if (remote)
            sftp_close(remote);
        if (fd >= 0)
            ::close(fd);
    }
    void finish() {
        if (remote) {
            auto file = remote;
            remote = nullptr;
            if (sftp_close(file) != SSH_OK)
                error("Cannot close remote destination");
        }
        if (fd >= 0) {
            int file = fd;
            fd = -1;
            if (::close(file) != 0)
                error("Cannot close local destination");
        }
    }
    Metadata metadata() {
        if (remote)
            return fromSftp(sftp_fstat(remote));
        struct stat s{};
        if (fstat(fd, &s))
            error("Cannot stat local file");
        return fromStat(s);
    }
    QByteArray read(quint64 offset, quint64 remaining) {
        constexpr qsizetype window = 2 * 1024 * 1024;
        if (!remote) {
            QByteArray data(qMin<quint64>(window, remaining), Qt::Uninitialized);
            ssize_t n = ::pread(fd, data.data(), data.size(), offset);
            if (n <= 0)
                error("Local file ended before expected size");
            data.resize(n);
            return data;
        }
        if (sftp_seek64(remote, offset) != SSH_OK)
            error("Cannot seek remote source");
        std::vector<std::unique_ptr<Async>> pending;
        qsizetype total = 0;
        for (int i = 0; i < 32 && quint64(total) < remaining; ++i) {
            auto a = std::make_unique<Async>();
            a->offset = total;
            a->count =
                sftp_aio_begin_read(remote, qMin<quint64>(65536, remaining - total), &a->handle);
            if (a->count <= 0)
                error("Cannot request remote read");
            total += a->count;
            pending.push_back(std::move(a));
        }
        QByteArray data(total, Qt::Uninitialized);
        for (auto &a : pending) {
            auto n = sftp_aio_wait_read(&a->handle, data.data() + a->offset, a->count);
            if (n <= 0)
                error("Remote source ended before expected size");
            // Short SFTP reads are legal; fill each gap before checkpointing.
            while (n < a->count) {
                if (sftp_seek64(remote, offset + a->offset + n) != SSH_OK)
                    error("Remote seek failed");
                auto got = sftp_read(remote, data.data() + a->offset + n, a->count - n);
                if (got <= 0)
                    error("Remote source read failed");
                n += got;
            }
        }
        return data;
    }
    void write(quint64 offset, const QByteArray &data) {
        if (!remote) {
            qsizetype done = 0;
            while (done < data.size()) {
                auto n = ::pwrite(fd, data.constData() + done, data.size() - done, offset + done);
                if (n <= 0)
                    error("Local destination write failed: " +
                          QString::fromLocal8Bit(strerror(errno)));
                done += n;
            }
            if (::fsync(fd))
                error("Cannot flush local destination");
            return;
        }
        if (sftp_seek64(remote, offset) != SSH_OK)
            error("Cannot seek remote destination");
        std::vector<std::unique_ptr<Async>> pending;
        qsizetype done = 0;
        while (done < data.size()) {
            auto a = std::make_unique<Async>();
            a->count = sftp_aio_begin_write(remote, data.constData() + done,
                                            qMin<qsizetype>(32768, data.size() - done), &a->handle);
            if (a->count <= 0)
                error("Cannot request remote write");
            done += a->count;
            pending.push_back(std::move(a));
        }
        for (auto &a : pending)
            if (sftp_aio_wait_write(&a->handle) != a->count)
                error("Remote destination write failed");
        if (sftp_extension_supported(session, "fsync@openssh.com", "1") &&
            sftp_fsync(remote) != SSH_OK)
            error("Cannot flush remote destination");
    }
};
} // namespace

class TransferEndpoint {
    Connection connection;

  public:
    TransferEndpoint(const Profile &p, const Trust &trust) : connection(p, trust) {}
    QString canonical(const QString &path) {
        return connection.canonical(path);
    }
    Metadata stat(const QString &path) {
        if (connection.local) {
            struct stat s{};
            if (::lstat(QFile::encodeName(path).constData(), &s)) {
                if (errno == ENOENT)
                    return {};
                error("Cannot stat local path: " + path);
            }
            return fromStat(s);
        }
        auto a = sftp_lstat(connection.sftp, path.toUtf8().constData());
        if (!a) {
            if (sftp_get_error(connection.sftp) == SSH_FX_NO_SUCH_FILE)
                return {};
            error("Cannot stat remote path: " + path);
        }
        return fromSftp(a);
    }
    QString checked(const QString &root, const QString &relative, bool directories = false) {
        if (relative.isEmpty() || relative.startsWith('/') || relative.split('/').contains("..") ||
            relative.split('/').contains(".") || relative.split('/').contains(QString{}))
            error("Unsafe destination path");
        auto r = stat(root);
        if (!r.exists || !S_ISDIR(r.mode) || S_ISLNK(r.mode))
            error("Destination root is not a real directory");
        QString current = root;
        auto parts = relative.split('/');
        for (int i = 0; i < parts.size(); ++i) {
            current = remoteJoin(current, parts[i]);
            auto m = stat(current);
            bool dir = directories || i + 1 < parts.size();
            if (m.exists && (S_ISLNK(m.mode) || (dir && !S_ISDIR(m.mode))))
                error("Destination contains a symlink or non-directory: " + current);
            if (dir && !m.exists) {
                int rc = connection.local
                             ? ::mkdir(QFile::encodeName(current).constData(), 0777)
                             : sftp_mkdir(connection.sftp, current.toUtf8().constData(), 0777);
                if (rc != 0)
                    error("Cannot create destination directory: " + current);
            }
        }
        return current;
    }
    std::unique_ptr<File> open(const QString &path, bool write, bool create = false) {
        auto f = std::make_unique<File>();
        int flags = write ? O_WRONLY : O_RDONLY;
        if (create)
            flags |= O_CREAT | O_EXCL;
        if (connection.local)
            f->fd = ::open(QFile::encodeName(path).constData(), flags | O_NOFOLLOW, 0666);
        else {
            f->remote = sftp_open(connection.sftp, path.toUtf8().constData(), flags, 0666);
            f->session = connection.sftp;
        }
        if (f->fd < 0 && !f->remote)
            error("Cannot open transfer file: " + path);
        if (!S_ISREG(f->metadata().mode))
            error("Transfer path is not a regular file: " + path);
        return f;
    }
    void resize(const QString &path, File &file, quint64 size) {
        if (connection.local) {
            if (::ftruncate(file.fd, size))
                error("Cannot truncate partial download");
        } else {
            struct sftp_attributes_struct a{};
            a.flags = SSH_FILEXFER_ATTR_SIZE;
            a.size = size;
            if (sftp_setstat(connection.sftp, path.toUtf8().constData(), &a) != SSH_OK)
                error("Cannot truncate remote partial upload");
        }
    }
    void chmod(const QString &path, unsigned mode) {
        int rc = connection.local ? ::chmod(QFile::encodeName(path).constData(), mode)
                                  : sftp_chmod(connection.sftp, path.toUtf8().constData(), mode);
        if (rc != 0)
            error("Cannot set transfer permissions: " + path);
    }
    void publish(const QString &temporary, const QString &target, quint32 mtime, unsigned mode) {
        // Set metadata on the partial first, so a published file is immediately usable.
        if (connection.local) {
            struct timespec t[2]{{time_t(mtime), 0}, {time_t(mtime), 0}};
            if (::utimensat(AT_FDCWD, QFile::encodeName(temporary).constData(), t,
                            AT_SYMLINK_NOFOLLOW))
                error("Cannot set local modification time");
        } else {
            struct sftp_attributes_struct a{};
            a.flags = SSH_FILEXFER_ATTR_ACMODTIME;
            a.atime = mtime;
            a.mtime = mtime;
            if (sftp_setstat(connection.sftp, temporary.toUtf8().constData(), &a) != SSH_OK)
                error("Cannot set remote modification time");
        }
        chmod(temporary, mode & 0777);
        if (stat(target).exists) {
            chmod(temporary, 0600);
            error("Destination already exists: " + target);
        }
        if (connection.local) {
            if (::link(QFile::encodeName(temporary).constData(),
                       QFile::encodeName(target).constData())) {
                chmod(temporary, 0600);
                error("Cannot publish destination without overwriting: " + target);
            }
            if (::unlink(QFile::encodeName(temporary).constData()))
                error("Cannot remove completed partial file");
        } else if (sftp_rename(connection.sftp, temporary.toUtf8().constData(),
                               target.toUtf8().constData()) != SSH_OK) {
            chmod(temporary, 0600);
            error("Cannot publish remote destination without overwriting: " + target);
        }
    }
};

QJsonObject DesktopTransfer::status(const QString &id) {
    QString path = jobPath(id);
    auto result = readJson(path + "/status.json");
    if (result.isEmpty())
        return {{"state", "Unknown"}};
    if (result["state"].toString() == "Running") {
        QLockFile lock(path + "/lock");
        lock.setStaleLockTime(0);
        if (lock.tryLock(0)) {
            result["state"] = "Failed";
            result["error"] = "Desktop transfer was interrupted. Retry / resume it.";
        }
    }
    return result;
}
void DesktopTransfer::cancel(const QString &id) {
    writeJson(jobPath(id) + "/cancel.json", {{"cancel", true}});
}
void DesktopTransfer::skip(const QString &id, const QSet<int> &indices) {
    QString path = jobPath(id) + "/skip.json";
    auto o = readJson(path);
    QSet<int> all = indices;
    for (auto value : o["indices"].toArray())
        all.insert(value.toInt());
    QJsonArray list;
    for (int i : all)
        list.append(i);
    writeJson(path, {{"indices", list}});
}
QJsonObject DesktopTransfer::run(const QJsonObject &job, const Profile &source,
                                 const Profile &destination, const Trust &trust,
                                 const std::function<void(const QJsonObject &)> &progress) {
    QString path = jobPath(job["id"].toString());
    if (!QDir().mkpath(path))
        error("Cannot create desktop transfer state directory");
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QLockFile lock(path + "/lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        error("This transfer is already running in another window");
    QFile::remove(path + "/cancel.json");
    auto s = readJson(path + "/status.json");
    auto offsets = s["offsets"].toObject(), modes = s["output_modes"].toObject();
    auto completed = s["completed"].toArray(), skipped = s["skipped"].toArray();
    s["state"] = "Running";
    s["error"] = "";
    auto files = job["files"].toArray();
    double total = 0;
    for (auto value : files)
        total += value.toObject()["size"].toDouble();
    s["total"] = total;
    QElapsedTimer throttle;
    throttle.start();
    auto save = [&](bool notify = false) {
        s["offsets"] = offsets;
        s["completed"] = completed;
        s["skipped"] = skipped;
        s["output_modes"] = modes;
        double bytes = 0;
        for (int i = 0; i < files.size(); ++i)
            bytes += completed.contains(QString::number(i))
                         ? files[i].toObject()["size"].toDouble()
                         : offsets[QString::number(i)].toDouble();
        s["bytes"] = bytes;
        writeJson(path + "/status.json", s);
        if (notify || throttle.elapsed() >= 150) {
            progress(s);
            throttle.restart();
        }
    };
    auto cancelled = [&] { return QFileInfo::exists(path + "/cancel.json"); };
    auto removed = [&](int i) {
        return job["removed_files"].toArray().contains(i) ||
               readJson(path + "/skip.json")["indices"].toArray().contains(i);
    };
    try {
        save(true);
        TransferEndpoint from(source, trust), to(destination, trust);
        QString root = to.canonical(job["destination"].toString());
        for (auto relative : job["directories"].toArray()) {
            if (cancelled())
                throw std::runtime_error("Cancelled");
            to.checked(root, relative.toString(), true);
        }
        for (int i = 0; i < files.size(); ++i) {
            if (cancelled())
                throw std::runtime_error("Cancelled");
            QString number = QString::number(i);
            auto entry = files[i].toObject();
            if (removed(i) || skipped.contains(number)) {
                if (!skipped.contains(number))
                    skipped.append(number);
                save();
                continue;
            }
            QString target = to.checked(root, entry["relative"].toString());
            auto size = quint64(entry["size"].toDouble());
            auto mtime = quint32(entry["mtime"].toDouble());
            if (completed.contains(number)) {
                auto m = to.stat(target);
                if (!m.exists || !S_ISREG(m.mode) || m.size != size)
                    error("Previously completed file changed: " + target);
                continue;
            }
            if (to.stat(target).exists)
                error("Destination already exists: " + target);
            auto input = from.open(entry["source"].toString(), false);
            auto meta = input->metadata();
            if (meta.size != size || meta.mtime != mtime)
                error("Source changed since queueing: " + entry["source"].toString());
            QString partial = remoteJoin(QFileInfo(target).path(),
                                         "." + QFileInfo(target).fileName() + ".linfxp-" +
                                             job["id"].toString() + ".part");
            auto partMeta = to.stat(partial);
            if (partMeta.exists && !S_ISREG(partMeta.mode))
                error("Partial file is not regular: " + partial);
            auto output = to.open(partial, true, !partMeta.exists);
            if (!modes.contains(number))
                modes[number] = int(output->metadata().mode & 0777);
            to.chmod(partial, 0600);
            quint64 offset =
                qMin<quint64>(quint64(offsets[number].toDouble()), output->metadata().size);
            to.resize(partial, *output, offset);
            s["current"] = entry["relative"];
            s["current_index"] = i;
            save(true);
            while (offset < size && !cancelled() && !removed(i)) {
                auto data = input->read(offset, size - offset);
                output->write(offset, data);
                offset += data.size();
                offsets[number] = double(offset);
                save();
            }
            if (!cancelled() && !removed(i)) {
                auto after = input->metadata();
                if (after.size != size || after.mtime != mtime)
                    error("Source changed during transfer: " + entry["source"].toString());
            }
            output->finish();
            output.reset();
            input.reset();
            if (cancelled())
                throw std::runtime_error("Cancelled");
            if (removed(i)) {
                skipped.append(number);
                save();
                continue;
            }
            to.publish(partial, target, mtime, unsigned(modes[number].toInt()));
            completed.append(number);
            offsets.remove(number);
            save();
        }
        s["state"] = "Completed";
    } catch (const std::exception &e) {
        s["state"] = cancelled() ? "Cancelled" : "Failed";
        s["error"] = QString::fromUtf8(e.what());
    }
    save(true);
    return s;
}
