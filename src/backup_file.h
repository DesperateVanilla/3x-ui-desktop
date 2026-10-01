#pragma once
#include "domain.h"

namespace fleet {
constexpr qint64 MaximumBackupBytes = 128LL * 1024 * 1024;
enum class BackupKind { SQLite, PostgreSQL };
struct BackupInfo {
    BackupKind kind = BackupKind::SQLite;
    qint64 size = 0;
    qint64 inboundCount = -1;
    qint64 clientCount = -1;
    QString extension() const { return kind == BackupKind::SQLite ? ".db" : ".dump"; }
};
Outcome<BackupInfo> inspectDatabaseBackup(const QByteArray& data);
Outcome<QByteArray> readDatabaseBackup(const QString& path);
OperationResult saveDatabaseBackup(const QString& path, const QByteArray& data);
} // namespace fleet
