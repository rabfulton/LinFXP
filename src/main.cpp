#include "transfer.h"
#include "window.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QPixmap>
#include <QTimer>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
        std::puts("LinFXP " LINFXP_VERSION);
        return 0;
    }
    QApplication app(argc, argv);
    app.setOrganizationName("LinFXP");
    app.setApplicationName("LinFXP");
    app.setApplicationVersion(LINFXP_VERSION);
    // Integration diagnostics use stdin for credentials, never command arguments.
    if (app.arguments().contains("--probe")) {
        try {
            QFile input;
            if (!input.open(stdin, QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read probe input");
            QJsonObject json = QJsonDocument::fromJson(input.readAll()).object();
            if (json["action"].toString() == "desktop") {
                auto from = Profile::fromJson(json["source"].toObject()),
                     to = Profile::fromJson(json["target"].toObject());
                from.secret = json["source"].toObject()["secret"].toString();
                to.secret = json["target"].toObject()["secret"].toString();
                int checkpoint = 0;
                auto result = DesktopTransfer::run(
                    json["job"].toObject(), from, to,
                    [](const QString &, const QString &) { return false; },
                    [&](const QJsonObject &status) {
                        if (status["state"].toString() != "Running")
                            return;
                        if (json["cancel_after"].toDouble(-1) >= 0 &&
                            status["bytes"].toDouble() >= json["cancel_after"].toDouble() &&
                            checkpoint++ == 0)
                            DesktopTransfer::cancel(json["job"].toObject()["id"].toString());
                        if (json.contains("remove_index") && checkpoint++ == 0)
                            DesktopTransfer::skip(json["job"].toObject()["id"].toString(),
                                                  {json["remove_index"].toInt()});
                    });
                std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
                return 0;
            }
            Profile p = Profile::fromJson(json);
            p.secret = json["secret"].toString();
            Connection connection(p, [](const QString &, const QString &) { return false; });
            if (json["action"].toString() == "skip") {
                connection.skipFiles(json["job_id"].toString(), json["indices"].toArray());
                std::puts("{}");
                return 0;
            }
            if (json["action"].toString() == "launch") {
                QFile worker(":/worker/worker.py");
                if (!worker.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Cannot load worker");
                QString path = connection.uploadWorker(worker.readAll());
                QByteArray output =
                    connection.execute("python3 " + shellQuote(path),
                                       QJsonDocument(json["payload"].toObject()).toJson());
                std::puts(output.constData());
                return 0;
            }
            if (json["action"].toString() == "manifest") {
                auto selected = connection.list(connection.canonical(p.path));
                auto result = connection.manifest(p.path, selected);
                std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
                return 0;
            }
            QJsonArray entries;
            for (const Entry &entry : connection.list(connection.canonical(p.path)))
                entries.append(QJsonObject{
                    {"name", entry.name}, {"size", double(entry.size)}, {"type", entry.type}});
            std::puts(QJsonDocument(entries).toJson(QJsonDocument::Compact).constData());
            return 0;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    try {
        Window window;
        window.show();
        int screenshot = app.arguments().indexOf("--screenshot");
        if (screenshot >= 0 && screenshot + 1 < app.arguments().size()) {
            window.demo();
            QString path = app.arguments()[screenshot + 1];
            QTimer::singleShot(200, &window, [&window, path, &app] {
                window.grab().save(path);
                app.quit();
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
