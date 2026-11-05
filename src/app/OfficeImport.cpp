// SPDX-License-Identifier: GPL-3.0-only
#include "OfficeImport.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QPainter>

#include <cstring>

#include <memory>

QStringList wordPatterns() {
  return {"*.docx", "*.doc", "*.docm", "*.dotx", "*.dot", "*.rtf", "*.odt"};
}

QStringList imagePatterns() {
  return {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.tif", "*.tiff", "*.webp"};
}

namespace {
bool matches(const QString& path, const QStringList& patterns) {
  const QString suffix = "*." + QFileInfo(path).suffix().toLower();
  return patterns.contains(suffix);
}

// Word, driven through its COM interface from PowerShell: opened read-only and invisible, saved
// as PDF, closed. A password-protected document fails instead of asking (a wrong password is
// given), so nothing waits on a hidden prompt. Word's Open, Close and Quit take their arguments
// by reference, which PowerShell only passes as [ref]. Closing and quitting can't fail the
// conversion or hide its error.
const char* const kWordScript = R"PS(
param([string]$In, [string]$Out)
$ErrorActionPreference = 'Stop'
$word = $null
$doc = $null
$ok = $false
try {
  $word = New-Object -ComObject Word.Application
  $word.Visible = $false
  $word.DisplayAlerts = 0
  $file = $In
  $confirm = $false
  $readOnly = $true
  $recent = $false
  $password = 'pdfz-no-password'
  $doc = $word.Documents.Open([ref]$file, [ref]$confirm, [ref]$readOnly, [ref]$recent, [ref]$password)
  $doc.ExportAsFixedFormat($Out, 17)
  $ok = $true
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
} finally {
  try { if ($doc) { $dontSave = 0; $doc.Close([ref]$dontSave) } } catch {}
  try { if ($word) { $word.Quit() } } catch {}
  try { if ($word) { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) } } catch {}
}
if ($ok) { exit 0 } else { exit 1 }
)PS";

QString findLibreOffice() {
  QString exe = QStandardPaths::findExecutable("soffice");
  if (exe.isEmpty()) exe = QStandardPaths::findExecutable("libreoffice");
#ifdef Q_OS_WIN
  for (const char* dir : {"C:/Program Files/LibreOffice/program", "C:/Program Files (x86)/LibreOffice/program"})
    if (exe.isEmpty() && QFileInfo::exists(QString(dir) + "/soffice.exe")) exe = QString(dir) + "/soffice.exe";
#endif
  return exe;
}

// Runs a converter, keeping the window alive and offering Cancel. Returns its exit code, or -1
// if it couldn't start, timed out or was canceled (`canceled` says which).
int runWithProgress(QWidget* parent, const QString& program, const QStringList& args, const QString& what,
                    bool& canceled, QString& output) {
  canceled = false;
  QProcess proc;
  proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
  // No console window flashing up for PowerShell (CREATE_NO_WINDOW).
  proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; });
#endif
  proc.start(program, args);
  if (!proc.waitForStarted(10000)) return -1;
  QProgressDialog progress(what, QObject::tr("Cancel"), 0, 0, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(400);
  QElapsedTimer clock;
  clock.start();
  while (!proc.waitForFinished(50)) {
    QApplication::processEvents();
    if (progress.wasCanceled() || clock.elapsed() > 180000) {
      canceled = progress.wasCanceled();
      proc.kill();
      proc.waitForFinished(3000);
      return -1;
    }
  }
  output = QString::fromLocal8Bit(proc.readAll()).trimmed();
  return proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
}

}  // namespace

bool isWordFile(const QString& path) { return matches(path, wordPatterns()); }
bool isImageFile(const QString& path) { return matches(path, imagePatterns()); }

QString convertToPdf(QWidget* parent, const QString& path, QString* error) {
  if (error) error->clear();
  // A folder of its own, kept until the caller has read the PDF (it deletes the file; the
  // folder goes with the next temporary-folder cleanup, or now if it's empty).
  QTemporaryDir dir(QDir::tempPath() + "/pdfeditzd-import-XXXXXX");
  dir.setAutoRemove(false);
  if (!dir.isValid()) {
    if (error) *error = QObject::tr("Couldn't make a temporary folder.");
    return {};
  }
  const QString in = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
  const QString out = QDir::toNativeSeparators(dir.filePath(QFileInfo(path).completeBaseName() + ".pdf"));
  const QString what = QObject::tr("Converting %1 to PDF…").arg(QFileInfo(path).fileName());
  QString log;
  bool canceled = false;

#ifdef Q_OS_WIN
  {
    const QString script = dir.filePath("convert.ps1");
    QFile f(script);
    if (f.open(QIODevice::WriteOnly)) {
      f.write(kWordScript);
      f.close();
      const int code = runWithProgress(parent, "powershell.exe",
                                       {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                                        QDir::toNativeSeparators(script), "-In", in, "-Out", out},
                                       what, canceled, log);
      QFile::remove(script);
      if (canceled) return {};
      if (code == 0 && QFileInfo::exists(out)) return out;
    }
  }
#endif
  // LibreOffice, if Word isn't there (or couldn't do it).
  const QString soffice = findLibreOffice();
  if (!soffice.isEmpty()) {
    QString loLog;
    const int code = runWithProgress(parent, soffice,
                                     {"--headless", "--norestore", "--convert-to", "pdf", "--outdir",
                                      QDir::toNativeSeparators(dir.path()), in},
                                     what, canceled, loLog);
    if (canceled) return {};
    if (code == 0 && QFileInfo::exists(out)) return out;
    if (!loLog.isEmpty()) log = loLog;
  }
  if (error) {
#ifdef Q_OS_WIN
    *error = QObject::tr("Microsoft Word couldn't convert %1.").arg(QFileInfo(path).fileName());
#else
    *error = soffice.isEmpty() ? QObject::tr("Converting Word documents needs LibreOffice, which isn't installed.")
                               : QObject::tr("LibreOffice couldn't convert %1.").arg(QFileInfo(path).fileName());
#endif
    if (!log.isEmpty()) *error += "\n\n" + log.left(400);
  }
  QDir(dir.path()).removeRecursively();
  return {};
}

bool loadPageImage(const QString& path, pdfz::Document::PageImage& out, QString* error) {
  out = {};
  QImageReader reader(path);
  reader.setAutoTransform(true);  // a phone photo's orientation
  const QString format = QString::fromLatin1(reader.format()).toLower();
  const QSize size = reader.size();
  // A JPEG that's already upright goes in as it is: smaller, and not recompressed.
  if (format == "jpeg" && size.isValid() && reader.transformation() == QImageIOHandler::TransformationNone) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      const QByteArray bytes = f.readAll();
      out.jpeg.assign(bytes.begin(), bytes.end());
      out.width = size.width();
      out.height = size.height();
      return true;
    }
  }
  QImage img = reader.read();
  if (img.isNull()) {
    if (error) *error = QObject::tr("%1 couldn't be read as a picture.").arg(QFileInfo(path).fileName());
    return false;
  }
  out = pageImageFrom(img);
  return true;
}

pdfz::Document::PageImage pageImageFrom(QImage img) {
  pdfz::Document::PageImage out;
  if (img.isNull()) return out;
  // On white, as paper; and no bigger than a page needs (300 DPI across a large sheet).
  if (img.width() > 4000 || img.height() > 4000) img = img.scaled(4000, 4000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QImage opaque(img.size(), QImage::Format_ARGB32);
  opaque.fill(Qt::white);
  {
    QPainter p(&opaque);
    p.drawImage(0, 0, img);
  }
  out.width = opaque.width();
  out.height = opaque.height();
  out.bgra.resize(std::size_t(out.width) * std::size_t(out.height) * 4);
  for (int y = 0; y < opaque.height(); ++y)
    std::memcpy(out.bgra.data() + std::size_t(y) * std::size_t(out.width) * 4, opaque.constScanLine(y),
                std::size_t(out.width) * 4);  // ARGB32 is B, G, R, A in memory on Windows and Linux PCs
  return out;
}
