// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.hpp"

#include "core/engine.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

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
  args.addPositionalArgument("file", "The PDF to open");
  args.process(app);

  // PDFium runs for as long as the app; the window (and every document) closes before it stops.
  pdfz::Engine engine;
  int rc = 0;
  {
    MainWindow window;
    window.show();
    if (!args.positionalArguments().isEmpty()) window.openFile(args.positionalArguments().first());
    rc = QApplication::exec();
  }
  return rc;
}
