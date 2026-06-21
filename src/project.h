#pragma once
#include <QString>
#include <QVector>

class QJsonObject;

// Token-expansion context for command templates.
//   {root}    = the opened project folder (where build.sh/app.elf live)
//   {repo}    = repo root above it (holds the ./x launcher + tools/)
//   {file}    = current source path     {dir} = current file's dir
//   {base}    = file name without extension     {exe} = {dir}/{base}
//   {example} = the examples/<name> subdir holding the current file (./x arg)
struct CmdContext {
    QString root;
    QString repo;
    QString file;
    QString example;
    QString profile;     // selected Ada runtime profile -> {profile} (./x --profile)
};

// One processor target: the configurable commands the editor runs for it
// (build / flash / run / monitor) plus an on-chip debug configuration. Command
// templates are token-expanded (see CmdContext) and run at the project root.
struct TargetProfile {
    QString name;
    QString description;

    // Action commands (default to the ./x launcher).
    QString buildCommand;
    QString flashCommand;
    QString runCommand;
    QString monitorCommand;

    // Debug: the editor starts debugServerCommand (e.g. OpenOCD on :3333),
    // then launches gdbCommand in MI mode on gdbProgram, connects to gdbRemote,
    // and stops at initBreakpoint.
    QString debugServerCommand;
    QString gdbCommand;
    QString gdbProgram;       // .elf to debug (token-expanded)
    QString gdbRemote;        // e.g. localhost:3333
    QString initBreakpoint;   // e.g. app_main
    bool debugSmp = false;    // dual-core: launch server with ESP_RTOS=hwthread

    static QString expand(const QString &cmd, const CmdContext &ctx);
};

// A project rooted at an opened folder, holding target profiles.
// Serialised to <folder>/.adaproj (JSON).
class Project {
public:
    QString name = "Untitled";
    QString rootPath;                 // the opened folder
    QString filePath;                 // .adaproj path; empty if never saved
    QVector<TargetProfile> targets;
    int activeIndex = 0;
    bool dirty = false;

    static Project makeDefault();     // seeds ./x-based ESP32-S3 targets

    const TargetProfile *active() const;
    void setActive(int index);

    bool load(const QString &path, QString *error);
    bool save(const QString &path, QString *error);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);
};
