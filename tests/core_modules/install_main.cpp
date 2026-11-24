#include "Support.hpp"
#include "support/SettingsIsolation.hpp"
int main(int argc, char **argv) {
 QCoreApplication app(argc, argv);
 int moduleStatus = 0;
 app.setOrganizationName("WTFML"); app.setApplicationName("WTFML");
 QTemporaryDir fixture; if (!fixture.isValid()) return 2;
 QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,fixture.path());
 QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,fixture.filePath("system-settings"));
 requireIsolatedSettings(fixture.path(), "verify core-install");
 const QString sandbox=fixture.filePath(".minecraft"); QDir().mkpath(sandbox); AppPaths::setMinecraftDir(sandbox);
 app.setProperty("wtfml.testDefaultMinecraftDir", sandbox);
 const QString metaDir=QStringLiteral(REPO_ROOT_PATH)+"/tests/meta";
    // vm-batch / vm-fetch-fail remain explicit offline diagnostic modes of verify;
    // splitting tests must not silently add their timeout probes to routine CI.
    runLoaderInheritanceTests(sandbox);
    runLoaderInstallTests();
 out()<<"install: "<<g_checks-g_failures<<"/"<<g_checks<<"\n"; return g_failures==0 && moduleStatus==0 ? 0 : 1;
}

