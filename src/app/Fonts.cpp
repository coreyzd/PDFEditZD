// SPDX-License-Identifier: GPL-3.0-only
#include "Fonts.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QProcess>
#include <QRawFont>
#include <QSettings>
#include <QTextLayout>

namespace {

enum class Standard { None, Helvetica, Times, Courier };

Standard standardOf(const QString& family) {
  const QString f = family.toLower();
  if (f.contains("helvetica") || f == "helv" || f == "arial" || f.startsWith("arial ") || f == "arialmt") return Standard::Helvetica;
  if (f.contains("times")) return Standard::Times;
  if (f.contains("courier")) return Standard::Courier;
  return Standard::None;
}

QString standardName(Standard s, bool bold, bool italic) {
  switch (s) {
    case Standard::Helvetica: return bold && italic ? "Helvetica-BoldOblique" : bold ? "Helvetica-Bold" : italic ? "Helvetica-Oblique" : "Helvetica";
    case Standard::Times: return bold && italic ? "Times-BoldItalic" : bold ? "Times-Bold" : italic ? "Times-Italic" : "Times-Roman";
    case Standard::Courier: return bold && italic ? "Courier-BoldOblique" : bold ? "Courier-Bold" : italic ? "Courier-Oblique" : "Courier";
    default: return {};
  }
}

// The computer's font file for a family and style, or empty.
QString fontFile(const QString& family, bool bold, bool italic) {
  static QHash<QString, QString> cache;
  const QString key = family + (bold ? "/b" : "/") + (italic ? "i" : "");
  if (auto it = cache.find(key); it != cache.end()) return *it;
  QString found;
#ifdef Q_OS_WIN
  // The registry lists installed fonts as "Calibri Bold Italic (TrueType)" = "calibriz.ttf"
  // (the Windows font folder) or a full path (fonts installed for one user).
  const QString style = QString(bold ? " Bold" : "") + (italic ? " Italic" : "");
  const QString winFonts = QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", "C:\\Windows")) + "/Fonts/";
  for (const char* hive : {"HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER"}) {
    QSettings reg(QString(hive) + "\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", QSettings::NativeFormat);
    for (const QString& name : reg.allKeys()) {
      const QString base = name.section(" (", 0, 0);
      if (base.compare(family + style, Qt::CaseInsensitive) != 0) continue;
      QString file = reg.value(name).toString();
      if (QFileInfo(file).isRelative()) file = winFonts + file;
      if (file.endsWith(".ttf", Qt::CaseInsensitive) || file.endsWith(".otf", Qt::CaseInsensitive)) found = file;
    }
    if (!found.isEmpty()) break;
  }
#else
  QProcess fc;
  fc.start("fc-match", {"--format=%{file}", family + (bold ? ":bold" : "") + (italic ? ":italic" : "")});
  if (fc.waitForFinished(3000)) {
    const QString file = QString::fromLocal8Bit(fc.readAllStandardOutput()).trimmed();
    // fc-match always answers with something: only take it if it's the family asked for.
    QProcess fam;
    fam.start("fc-query", {"--format=%{family}", file});
    if (fam.waitForFinished(3000) &&
        QString::fromLocal8Bit(fam.readAllStandardOutput()).split(',').contains(family, Qt::CaseInsensitive) &&
        (file.endsWith(".ttf", Qt::CaseInsensitive) || file.endsWith(".otf", Qt::CaseInsensitive)))
      found = file;
  }
#endif
  cache.insert(key, found);
  return found;
}

}  // namespace

EditFont editFontFor(const pdfz::ContentBlock& b) {
  EditFont f;
  f.bold = b.bold;
  f.italic = b.italic;
  const QString family = QString::fromStdString(b.family).trimmed();
  switch (standardOf(family.isEmpty() ? QString::fromStdString(b.fontName) : family)) {
    case Standard::Helvetica: f.family = family.contains("Arial", Qt::CaseInsensitive) ? "Arial" : "Helvetica"; break;
    case Standard::Times: f.family = family.contains("New", Qt::CaseInsensitive) ? "Times New Roman" : "Times"; break;
    case Standard::Courier: f.family = family.contains("New", Qt::CaseInsensitive) ? "Courier New" : "Courier"; break;
    default: f.family = QFontDatabase::families().contains(family, Qt::CaseInsensitive) ? family : QStringLiteral("Helvetica");
  }
  return f;
}

QFont qtFont(const EditFont& f, double pixelSize) {
  QFont q;
  switch (standardOf(f.family)) {
    // Metric-compatible stand-ins for the standard fonts, so text measures as it will print.
    case Standard::Helvetica: q.setFamilies({"Arial", "Liberation Sans", "Helvetica", "Nimbus Sans"}); q.setStyleHint(QFont::SansSerif); break;
    case Standard::Times: q.setFamilies({"Times New Roman", "Liberation Serif", "Times", "Nimbus Roman"}); q.setStyleHint(QFont::Serif); break;
    case Standard::Courier: q.setFamilies({"Courier New", "Liberation Mono", "Courier", "Nimbus Mono PS"}); q.setStyleHint(QFont::TypeWriter); break;
    default: q.setFamily(f.family);
  }
  q.setBold(f.bold);
  q.setItalic(f.italic);
  q.setPixelSize(std::max(1, int(std::lround(pixelSize))));
  q.setHintingPreference(QFont::PreferNoHinting);
  return q;
}

namespace {

// How many of the text's characters the font file has no glyph for.
int missingGlyphs(const QByteArray& data, const QString& text) {
  const QRawFont raw(data, 12);
  if (!raw.isValid()) return int(text.size()) + 1;
  int missing = 0;
  for (char32_t c : text.toUcs4())
    if (!QChar::isSpace(c) && QChar::category(c) != QChar::Other_Control && !raw.supportsCharacter(c)) ++missing;
  return missing;
}

}  // namespace

pdfz::TextFont pdfFont(const EditFont& f, const QString& text, bool* complete) {
  if (complete) *complete = true;
  pdfz::TextFont out;
  const Standard s = standardOf(f.family);
  const bool latin = pdfz::standardFontCanShow(text.toStdString());
  if (s != Standard::None && latin) {
    out.standard = standardName(s, f.bold, f.italic).toStdString();
    return out;
  }
  QString family = f.family;
  if (s == Standard::Helvetica) family = "Arial";
  else if (s == Standard::Times) family = "Times New Roman";
  else if (s == Standard::Courier) family = "Courier New";
  // The font asked for, then others that may have the characters it lacks (Greek, Cyrillic,
  // other scripts): the first that has them all, else the one missing fewest.
  const QStringList candidates = {family, "Arial", "Segoe UI", "Arial Unicode MS", "Nirmala UI", "Malgun Gothic",
                                  "Liberation Sans", "DejaVu Sans", "Noto Sans", "Noto Sans CJK SC", "Droid Sans Fallback"};
  QByteArray best;
  QString bestFile;
  int bestMissing = -1;
  for (int i = 0; i < candidates.size() && bestMissing != 0; ++i) {
    QString file = fontFile(candidates[i], f.bold, f.italic);
    if (file.isEmpty()) file = fontFile(candidates[i], false, false);
    QFile in(file);
    if (file.isEmpty() || file == bestFile || !in.open(QIODevice::ReadOnly)) continue;
    const QByteArray data = in.readAll();
    const int missing = missingGlyphs(data, text);
    if (bestMissing < 0 || missing < bestMissing) {
      best = data;
      bestFile = file;
      bestMissing = missing;
    }
  }
  if (!best.isEmpty()) {
    out.file.assign(best.begin(), best.end());
    out.key = bestFile.toStdString();
    if (complete) *complete = bestMissing == 0;
    return out;
  }
  if (latin) out.standard = standardName(s == Standard::None ? Standard::Helvetica : s, f.bold, f.italic).toStdString();
  return out;
}

QStringList editFontFamilies() {
  QStringList out = {"Helvetica", "Times", "Courier"};
  for (const QString& f : QFontDatabase::families())
    if (!QFontDatabase::isPrivateFamily(f) && !f.startsWith('@') && !out.contains(f, Qt::CaseInsensitive)) out << f;
  return out;
}

QStringList wrapText(const QString& text, const EditFont& font, double sizePt, double width) {
  // Measured at a large size and scaled, for precision.
  constexpr double kPx = 1000;
  const QFont q = qtFont(font, kPx);
  const double k = sizePt / kPx;
  QStringList out;
  for (const QString& para : text.split('\n')) {
    if (width <= 0 || para.isEmpty()) {
      out << para;
      continue;
    }
    QTextLayout layout(para, q);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) line.setLineWidth(width / k);
    layout.endLayout();
    for (int i = 0; i < layout.lineCount(); ++i) {
      const QTextLine line = layout.lineAt(i);
      QString t = para.mid(line.textStart(), line.textLength());
      while (t.endsWith(' ')) t.chop(1);
      out << t;
    }
  }
  return out;
}
