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
#include <QRegularExpression>

#ifdef PDFZ_HAVE_HB_SUBSET
#include <hb-subset.h>
#endif
#include <QSet>
#include <QSettings>

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>

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

// An installed font file and what its name table says it is.
struct FontFace {
  QString file;
  QStringList families;  // its typographic family (name 16) and its family (name 1)
  QString postscript;    // name 6: "RobotoMono-Regular", "RomanD"
  int weight = 400;
  bool italic = false;
};

quint16 be16(const char* p) { return quint16((quint8(p[0]) << 8) | quint8(p[1])); }
quint32 be32(const char* p) { return (quint32(be16(p)) << 16) | be16(p + 2); }

// Reads only the table directory, the name table and the style bits: a few kilobytes a font.
// TrueType and OpenType files only (a collection, .ttc, can't be embedded as it is).
std::optional<FontFace> readFace(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
  const QByteArray head = f.read(12);
  if (head.size() < 12) return std::nullopt;
  const quint32 tag = be32(head.constData());
  if (tag != 0x00010000 && tag != 0x4F54544F /* OTTO */ && tag != 0x74727565 /* true */) return std::nullopt;
  const int numTables = be16(head.constData() + 4);
  if (numTables <= 0 || numTables > 200) return std::nullopt;
  const QByteArray dir = f.read(16 * numTables);
  if (dir.size() < 16 * numTables) return std::nullopt;
  auto table = [&](const char* name, qint64 maxLen) -> QByteArray {
    for (int t = 0; t < numTables; ++t) {
      const char* rec = dir.constData() + 16 * t;
      if (std::memcmp(rec, name, 4) != 0) continue;
      const quint32 off = be32(rec + 8), len = be32(rec + 12);
      if (!f.seek(off)) return {};
      return f.read(std::min<qint64>(len, maxLen));
    }
    return {};
  };
  FontFace face;
  face.file = path;
  const QByteArray name = table("name", 1 << 20);
  if (name.size() < 6) return std::nullopt;
  const int count = be16(name.constData() + 2), strings = be16(name.constData() + 4);
  // For each name ID, the best string: Windows Unicode US English, else Windows Unicode, else Mac.
  QHash<int, std::pair<int, QString>> best;
  for (int r = 0; r < count && 6 + 12 * (r + 1) <= name.size(); ++r) {
    const char* rec = name.constData() + 6 + 12 * r;
    const int platform = be16(rec), encoding = be16(rec + 2), language = be16(rec + 4), id = be16(rec + 6);
    const int len = be16(rec + 8), off = be16(rec + 10);
    if (id != 1 && id != 2 && id != 6 && id != 16 && id != 17) continue;
    if (strings + off + len > name.size()) continue;
    const char* str = name.constData() + strings + off;
    QString text;
    int rank;
    if (platform == 3 && (encoding == 1 || encoding == 10 || encoding == 0)) {
      for (int k = 0; k + 1 < len; k += 2) text += QChar(be16(str + k));
      rank = language == 0x409 ? 3 : 2;
    } else if (platform == 1 && encoding == 0) {
      text = QString::fromLatin1(str, len);
      rank = language == 0 ? 1 : 0;
    } else {
      continue;
    }
    text = text.trimmed();
    if (text.isEmpty()) continue;
    if (!best.contains(id) || best[id].first < rank) best[id] = {rank, text};
  }
  for (int id : {16, 1})
    if (best.contains(id) && !face.families.contains(best[id].second, Qt::CaseInsensitive)) face.families << best[id].second;
  if (face.families.isEmpty()) return std::nullopt;
  face.postscript = best.value(6).second;
  const QString sub = (best.contains(17) ? best[17] : best.value(2)).second.toLower();
  const QByteArray os2 = table("OS/2", 64);
  if (os2.size() >= 64) {
    face.weight = be16(os2.constData() + 4);
    const int sel = be16(os2.constData() + 62);
    face.italic = (sel & 1) != 0;
    if ((sel & 0x20) && face.weight < 600) face.weight = 700;
  } else {
    if (sub.contains("bold") || sub.contains("black") || sub.contains("heavy")) face.weight = 700;
  }
  face.italic = face.italic || sub.contains("italic") || sub.contains("oblique");
  return face;
}

// The font files installed on this computer, read once.
const std::vector<FontFace>& installedFaces() {
  static const std::vector<FontFace> faces = [] {
    QStringList files;
#ifdef Q_OS_WIN
    // Installed fonts are listed in the registry, as a file in the Windows font folder or, for
    // fonts installed for one user, a full path.
    const QString winFonts = QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", "C:\\Windows")) + "/Fonts/";
    for (const char* hive : {"HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER"}) {
      QSettings reg(QString(hive) + "\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", QSettings::NativeFormat);
      for (const QString& key : reg.allKeys()) {
        QString file = QDir::fromNativeSeparators(reg.value(key).toString());
        if (QFileInfo(file).isRelative()) file = winFonts + file;
        files << file;
      }
    }
    const QString userFonts = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA")) + "/Microsoft/Windows/Fonts";
    for (const QString& f : QDir(userFonts).entryList({"*.ttf", "*.otf"}, QDir::Files)) files << userFonts + "/" + f;
#else
    QProcess fc;
    fc.start("fc-list", {"--format=%{file}\n"});
    if (fc.waitForFinished(5000)) files = QString::fromLocal8Bit(fc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
#endif
    std::vector<FontFace> out;
    QSet<QString> seen;
    for (const QString& file : files) {
      const QString key = file.toLower();
      if (seen.contains(key) || !(key.endsWith(".ttf") || key.endsWith(".otf"))) continue;
      seen.insert(key);
      if (auto face = readFace(file)) out.push_back(std::move(*face));
    }
    return out;
  }();
  return faces;
}

// "Roboto Mono" and "RobotoMono" alike.
QString squeezed(const QString& s) {
  QString out;
  for (QChar c : s)
    if (c.isLetterOrNumber()) out += c.toLower();
  return out;
}

// The computer's font file for a family and style, or empty: the face of that family whose
// boldness and slant come closest.
QString fontFile(const QString& family, bool bold, bool italic) {
  static QHash<QString, QString> cache;
  const QString key = family + (bold ? "/b" : "/") + (italic ? "i" : "");
  if (auto it = cache.find(key); it != cache.end()) return *it;
  const QString want = squeezed(family);
  QString found;
  int bestScore = INT_MAX;
  for (const FontFace& face : installedFaces()) {
    if (std::none_of(face.families.begin(), face.families.end(), [&](const QString& f) { return squeezed(f) == want; }))
      continue;
    const int score = ((face.weight >= 600) != bold ? 1000 : 0) + (face.italic != italic ? 2000 : 0) +
                      std::abs(face.weight - (bold ? 700 : 400));
    if (score < bestScore) {
      bestScore = score;
      found = face.file;
    }
  }
  cache.insert(key, found);
  return found;
}

// The installed family a PDF's font is, by its family or font name ("RomanD", "Roboto Mono",
// "RobotoMono-Regular", "Arial-BoldMT"), or empty.
QString installedFamily(const QString& pdfFamily, const QString& fontName) {
  const QStringList families = QFontDatabase::families();
  for (const QString& f : families)
    if (f.compare(pdfFamily, Qt::CaseInsensitive) == 0) return f;
  QString base = fontName.section(QRegularExpression("[-,]"), 0, 0);
  for (const char* suffix : {"PSMT", "PS", "MT"})
    if (base.endsWith(QLatin1String(suffix)) && base.size() > int(std::strlen(suffix))) {
      base.chop(int(std::strlen(suffix)));
      break;
    }
  for (const QString& name : {pdfFamily, base, fontName}) {
    const QString want = squeezed(name);
    if (want.isEmpty()) continue;
    for (const QString& f : families)
      if (squeezed(f) == want) return f;
  }
  // By the font's PostScript name, as the font file gives it.
  for (const FontFace& face : installedFaces())
    if (!fontName.isEmpty() && face.postscript.compare(fontName, Qt::CaseInsensitive) == 0)
      for (const QString& fam : face.families)
        for (const QString& f : families)
          if (f.compare(fam, Qt::CaseInsensitive) == 0) return f;
  return {};
}

}  // namespace

EditFont editFontFor(const pdfz::ContentBlock& b) { return editFontFor(b.family, b.fontName, b.bold, b.italic); }

EditFont editFontFor(const pdfz::TextRun& r) { return editFontFor(r.family, r.fontName, r.bold, r.italic); }

EditFont editFontFor(const std::string& pdfFamily, const std::string& fontName, bool bold, bool italic) {
  EditFont f;
  f.bold = bold;
  f.italic = italic;
  const QString family = QString::fromStdString(pdfFamily).trimmed();
  switch (standardOf(family.isEmpty() ? QString::fromStdString(fontName) : family)) {
    case Standard::Helvetica: f.family = family.contains("Arial", Qt::CaseInsensitive) ? "Arial" : "Helvetica"; break;
    case Standard::Times: f.family = family.contains("New", Qt::CaseInsensitive) ? "Times New Roman" : "Times"; break;
    case Standard::Courier: f.family = family.contains("New", Qt::CaseInsensitive) ? "Courier New" : "Courier"; break;
    default: {
      const QString installed = installedFamily(family, QString::fromStdString(fontName));
      f.family = installed.isEmpty() ? QStringLiteral("Helvetica") : installed;
    }
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

// Only the glyphs for `text` (and the space), without hinting: a few kilobytes instead of the
// whole file. The names stay, so the font reads back as itself. The file as it is if it can't.
QByteArray subsetFor(const QByteArray& file, const QString& text) {
#ifdef PDFZ_HAVE_HB_SUBSET
  hb_blob_t* blob = hb_blob_create(file.constData(), static_cast<unsigned>(file.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  hb_face_t* face = hb_face_create(blob, 0);
  hb_blob_destroy(blob);
  QByteArray out;
  if (hb_subset_input_t* input = hb_subset_input_create_or_fail()) {
    hb_set_t* unicodes = hb_subset_input_unicode_set(input);
    hb_set_add(unicodes, ' ');
    for (char32_t c : text.toUcs4()) hb_set_add(unicodes, c);
    hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_NO_HINTING);
    if (hb_face_t* sub = hb_subset_or_fail(face, input)) {
      hb_blob_t* result = hb_face_reference_blob(sub);
      unsigned len = 0;
      const char* data = hb_blob_get_data(result, &len);
      if (data && len > 12) out = QByteArray(data, qsizetype(len));
      hb_blob_destroy(result);
      hb_face_destroy(sub);
    }
    hb_subset_input_destroy(input);
  }
  hb_face_destroy(face);
  if (!out.isEmpty()) return out;
#else
  Q_UNUSED(text);
#endif
  return file;
}

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
  // Helvetica, Times and Courier themselves are PDF's standard fonts (nothing embedded). Arial,
  // Times New Roman and Courier New look the same but are themselves: picked, they're embedded,
  // so the text stays in the font it was given.
  const bool standardPick = f.family == "Helvetica" || f.family == "Times" || f.family == "Courier";
  if (standardPick && latin) {
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
    const QByteArray sub = subsetFor(best, text);
    out.file.assign(sub.begin(), sub.end());
    // The same characters of the same file share one embedded font in a document.
    QList<uint> chars = text.toUcs4();
    std::sort(chars.begin(), chars.end());
    chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
    const QString set = sub.size() == best.size() ? QString() : QString::fromUcs4(reinterpret_cast<const char32_t*>(chars.constData()), chars.size());
    out.key = (bestFile + QLatin1Char('#') + QString::number(qHash(set), 16)).toStdString();
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
