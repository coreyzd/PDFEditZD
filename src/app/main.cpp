// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.hpp"
#include "SingleInstance.hpp"

#include "core/engine.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QSvgRenderer>
#include <QPainter>

#include <cstdio>
#include <mutex>

namespace {

// The app has no console on Windows, so Qt's warnings (printing problems among them) go to
// pdfeditzd.log next to the settings: %APPDATA%\PDFEditZD\pdfeditzd.log. Started fresh each run.
QFile* g_log = nullptr;
std::mutex g_logMutex;

void logMessage(QtMsgType type, const QMessageLogContext&, const QString& msg) {
  static const char* const kinds[] = {"debug", "warning", "critical", "fatal", "info"};
  const QByteArray line = QStringLiteral("%1 [%2] %3\n")
                              .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                   QLatin1String(type <= QtInfoMsg ? kinds[type] : "?"), msg)
                              .toUtf8();
  std::lock_guard lock(g_logMutex);
  if (g_log) {
    g_log->write(line);
    g_log->flush();
  }
  std::fputs(line.constData(), stderr);
}

void startLog() {
  const QString dir = QFileInfo(QSettings().fileName()).absolutePath();  // beside PDFEditZD.ini
  QDir().mkpath(dir);
  g_log = new QFile(dir + "/pdfeditzd.log");
  if (!g_log->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    delete g_log;
    g_log = nullptr;
  }
  qInstallMessageHandler(logMessage);
}

// The program's icon, for its windows and the taskbar (the .exe has it too, for Explorer:
// pdfeditzd.rc): each size drawn from its own SVG, made for that size.
QIcon appIcon() {
  QIcon icon;
  for (int size : {16, 24, 32, 48, 64, 128, 256}) {
    const int from = size <= 16 ? 16 : size <= 24 ? 24 : size <= 32 ? 32 : size <= 48 ? 48 : 64;
    QSvgRenderer svg(QStringLiteral(":/icon/PDFEditZD-%1.svg").arg(from));
    if (!svg.isValid()) continue;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    svg.render(&p);
    p.end();
    icon.addPixmap(pm);
  }
  return icon;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("PDFEditZD");
  QApplication::setOrganizationName("PDFEditZD");
  QApplication::setApplicationVersion(PDFZ_VERSION);
  // Settings in a plain file (%APPDATA%\PDFEditZD\PDFEditZD.ini, ~/.config/PDFEditZD/...),
  // not the registry.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  startLog();

  QCommandLineParser args;
  args.setApplicationDescription("A lightweight PDF viewer and editor");
  args.addHelpOption();
  args.addVersionOption();
  args.addPositionalArgument("files", "PDFs to open", "[files...]");
  args.process(app);
  QStringList files;
  for (const QString& f : args.positionalArguments()) files << QFileInfo(f).absoluteFilePath();

  // Running already: it opens them (in tabs), and this one is done.
  SingleInstance single;
  if (single.handOff(files)) return 0;

  QApplication::setWindowIcon(appIcon());
  // PDFium runs for as long as the app; the windows (and every document) close before it stops.
  pdfz::Engine engine;
  int rc = 0;
  {
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    MainWindow::openFromOutside(files);
    QObject::connect(&single, &SingleInstance::filesReceived, &app, &MainWindow::openFromOutside, Qt::QueuedConnection);
    rc = QApplication::exec();
    // Windows closed but not yet deleted (they go once the event loop gets to them), then any
    // still open: before PDFium stops.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (QWidget* w : QApplication::topLevelWidgets())
      if (qobject_cast<MainWindow*>(w)) delete w;
  }
  return rc;
}
