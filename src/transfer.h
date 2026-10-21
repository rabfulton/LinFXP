#pragma once
#include "ssh.h"
#include <QSet>

namespace DesktopTransfer {
QJsonObject run(const QJsonObject &job, const Profile &source, const Profile &destination,
                const Trust &trust, const std::function<void(const QJsonObject &)> &progress);
QJsonObject status(const QString &id);
void cancel(const QString &id);
void skip(const QString &id, const QSet<int> &indices);
} // namespace DesktopTransfer
