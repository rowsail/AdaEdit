#include "project.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QString TargetProfile::expand(const QString &cmd, const CmdContext &ctx)
{
    QString out = cmd;
    out.replace("{root}", ctx.root);
    out.replace("{repo}", ctx.repo.isEmpty() ? ctx.root : ctx.repo);
    out.replace("{example}", ctx.example);
    out.replace("{profile}", ctx.profile.isEmpty() ? QStringLiteral("auto") : ctx.profile);
    if (!ctx.file.isEmpty()) {
        QFileInfo fi(ctx.file);
        const QString dir  = fi.absolutePath();
        const QString base = fi.completeBaseName();
        out.replace("{file}", fi.absoluteFilePath());
        out.replace("{dir}",  dir);
        out.replace("{base}", base);
        out.replace("{exe}",  dir + "/" + base);
    }
    return out;
}

Project Project::makeDefault()
{
    Project p;
    p.name = "Untitled";
    TargetProfile esp;
    esp.name               = "ESP32-S3";
    esp.description        = "Ada bare-metal ESP32-S3 (opened folder)";
    // Drive the repo's ./x launcher; {profile} comes from the toolbar selector
    // (auto = the example's own profile). ./x maps it to ESP32S3_RTS_PROFILE.
    esp.buildCommand       = "bash {repo}/x build {example} --profile {profile}";
    esp.flashCommand       = "bash {repo}/x build {example} --profile {profile} && "
                             "bash {repo}/x flash {example}";
    esp.runCommand         = "bash {repo}/x run {example} --profile {profile}";
    esp.monitorCommand     = "bash {repo}/x monitor";
    esp.debugServerCommand = "bash {repo}/tools/openocd.sh";  // OpenOCD -> gdb server :3333
    // Repo-fetched gdb (not on PATH); same one ./x uses.
    esp.gdbCommand         = "{repo}/tools/gdb/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb";
    esp.gdbProgram         = "{root}/app.elf";
    esp.gdbRemote          = "localhost:3333";
    esp.initBreakpoint     = "app_main";

    // This editor targets the ESP32-S3 only; a single fixed profile.
    p.targets = { esp };
    p.activeIndex = 0;
    p.dirty = false;
    return p;
}

const TargetProfile *Project::active() const
{
    if (activeIndex < 0 || activeIndex >= targets.size())
        return nullptr;
    return &targets[activeIndex];
}

void Project::setActive(int index)
{
    if (index >= 0 && index < targets.size() && index != activeIndex) {
        activeIndex = index;
        dirty = true;
    }
}

static QJsonObject targetToJson(const TargetProfile &t)
{
    QJsonObject o;
    o["name"]               = t.name;
    o["description"]        = t.description;
    o["buildCommand"]       = t.buildCommand;
    o["flashCommand"]       = t.flashCommand;
    o["runCommand"]         = t.runCommand;
    o["monitorCommand"]     = t.monitorCommand;
    o["debugServerCommand"] = t.debugServerCommand;
    o["gdbCommand"]         = t.gdbCommand;
    o["gdbProgram"]         = t.gdbProgram;
    o["gdbRemote"]          = t.gdbRemote;
    o["initBreakpoint"]     = t.initBreakpoint;
    o["debugSmp"]           = t.debugSmp;
    return o;
}

static TargetProfile targetFromJson(const QJsonObject &o)
{
    TargetProfile t;
    t.name               = o.value("name").toString();
    t.description        = o.value("description").toString();
    t.buildCommand       = o.value("buildCommand").toString();
    t.flashCommand       = o.value("flashCommand").toString();
    t.runCommand         = o.value("runCommand").toString();
    t.monitorCommand     = o.value("monitorCommand").toString();
    t.debugServerCommand = o.value("debugServerCommand").toString();
    t.gdbCommand         = o.value("gdbCommand").toString();
    t.gdbProgram         = o.value("gdbProgram").toString();
    t.gdbRemote          = o.value("gdbRemote").toString();
    t.initBreakpoint     = o.value("initBreakpoint").toString();
    t.debugSmp           = o.value("debugSmp").toBool(false);
    return t;
}

QJsonObject Project::toJson() const
{
    QJsonArray arr;
    for (const auto &t : targets)
        arr.append(targetToJson(t));
    QJsonObject root;
    root["formatVersion"] = 2;
    root["name"]          = name;
    root["activeIndex"]   = activeIndex;
    root["targets"]       = arr;
    return root;
}

void Project::fromJson(const QJsonObject &obj)
{
    name = obj.value("name").toString("Untitled");
    targets.clear();
    for (const auto &v : obj.value("targets").toArray())
        targets.append(targetFromJson(v.toObject()));
    activeIndex = obj.value("activeIndex").toInt(0);
    if (activeIndex >= targets.size()) activeIndex = 0;
}

bool Project::load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = pe.errorString();
        return false;
    }
    fromJson(doc.object());
    filePath = path;
    dirty = false;
    return true;
}

bool Project::save(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    filePath = path;
    dirty = false;
    return true;
}
