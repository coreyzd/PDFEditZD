// SPDX-License-Identifier: GPL-3.0-only
#include "RichText.hpp"

#include "Markup.hpp"

#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace {

enum Property {
  kFamily = QTextFormat::UserProperty + 1,
  kBold,
  kItalic,
  kSize,
  kRise,
};

void appendRun(RichText& out, const RichRun& run) {
  if (run.text.isEmpty()) return;
  if (!out.empty() && out.back().sameStyle(run)) out.back().text += run.text;
  else out.push_back(run);
}

RichText merged(const RichText& t) {
  RichText out;
  for (const RichRun& r : t) appendRun(out, r);
  return out;
}

}  // namespace

bool RichRun::sameStyle(const RichRun& o) const {
  return font == o.font && std::abs(size - o.size) < 0.05 && color.rgb() == o.color.rgb() && std::abs(rise - o.rise) < 0.05;
}

RichText richFromBlock(const pdfz::ContentBlock& b) {
  RichText out;
  for (const pdfz::TextRun& r : b.runs) {
    RichRun run;
    run.text = QString::fromStdString(r.text);
    run.font = editFontFor(r);
    run.size = r.size > 0 ? r.size : 12;
    run.color = toQColor(r.color);
    run.rise = r.rise;
    appendRun(out, run);
  }
  if (out.empty() && !b.text.empty()) {
    RichRun run;
    run.text = QString::fromStdString(b.text);
    run.font = editFontFor(b);
    run.size = b.fontSize > 0 ? b.fontSize : 12;
    run.color = toQColor(b.color);
    out.push_back(run);
  }
  return out;
}

QString plainText(const RichText& text) {
  QString s;
  for (const RichRun& r : text) s += r.text;
  return s;
}

bool sameText(const RichText& a, const RichText& b) {
  const RichText x = merged(a), y = merged(b);
  if (x.size() != y.size()) return false;
  for (std::size_t i = 0; i < x.size(); ++i)
    if (x[i].text != y[i].text || !x[i].sameStyle(y[i])) return false;
  return true;
}

RichRun mainStyle(const RichText& text) {
  RichRun best;
  qsizetype most = -1;
  for (const RichRun& r : text) {
    if (std::abs(r.rise) > 0.05 && most >= 0) continue;
    if (r.text.size() > most) {
      most = r.text.size();
      best = r;
    }
  }
  best.text.clear();
  return best;
}

void applyStyle(RichRun& run, const TextStyleChange& c, double relativeTo) {
  if (c.family) run.font.family = *c.family;
  if (c.bold) run.font.bold = *c.bold;
  if (c.italic) run.font.italic = *c.italic;
  if (c.color && c.color->isValid()) run.color = *c.color;
  if (c.size && *c.size > 0) {
    const double ratio = relativeTo > 0 ? *c.size / relativeTo : *c.size / run.size;
    run.size *= ratio;
    run.rise *= ratio;
  }
}

QTextCharFormat charFormat(const RichRun& run, double pxPerPt, double dpiY) {
  QTextCharFormat f;
  QFont q = qtFont(run.font, 10);
  q.setPointSizeF(std::max(0.5, run.size * pxPerPt * 72.0 / dpiY));
  f.setFont(q);
  f.setForeground(run.color);
  f.setProperty(kFamily, run.font.family);
  f.setProperty(kBold, run.font.bold);
  f.setProperty(kItalic, run.font.italic);
  f.setProperty(kSize, run.size);
  f.setProperty(kRise, run.rise);
  if (std::abs(run.rise) > 0.01) {
    const double height = QFontMetricsF(q).height();
    if (height > 0) f.setBaselineOffset(run.rise * pxPerPt / height * 100);
  }
  return f;
}

RichRun runOf(const QTextCharFormat& f) {
  RichRun r;
  const QFont q = f.font();
  r.font.family = f.hasProperty(kFamily) ? f.property(kFamily).toString() : q.family();
  r.font.bold = f.hasProperty(kBold) ? f.boolProperty(kBold) : q.bold();
  r.font.italic = f.hasProperty(kItalic) ? f.boolProperty(kItalic) : q.italic();
  r.size = f.hasProperty(kSize) ? f.doubleProperty(kSize) : 12;
  r.rise = f.hasProperty(kRise) ? f.doubleProperty(kRise) : 0;
  r.color = f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color() : QColor(Qt::black);
  return r;
}

void fillDocument(QTextDocument* doc, const RichText& text, double pxPerPt, double dpiY) {
  doc->clear();
  QTextCursor c(doc);
  bool first = true;
  for (const RichRun& run : text) {
    const QTextCharFormat fmt = charFormat(run, pxPerPt, dpiY);
    if (first) {
      c.setBlockCharFormat(fmt);
      first = false;
    }
    const QStringList parts = run.text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
      if (i > 0) c.insertBlock(c.blockFormat(), fmt);
      if (!parts[i].isEmpty()) c.insertText(parts[i], fmt);
    }
  }
}

RichText richFromDocument(const QTextDocument* doc) {
  RichText out;
  for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
    for (auto it = b.begin(); !it.atEnd(); ++it) {
      const QTextFragment frag = it.fragment();
      if (!frag.isValid()) continue;
      RichRun r = runOf(frag.charFormat());
      r.text = frag.text();
      r.text.replace(QChar::LineSeparator, QLatin1Char('\n'));
      r.text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
      appendRun(out, r);
    }
    if (b.next().isValid()) {
      RichRun br = out.empty() ? runOf(b.charFormat()) : out.back();
      br.text = QStringLiteral("\n");
      appendRun(out, br);
    }
  }
  return out;
}

namespace {

// The fragments of `doc` within [from, to): their ranges and formats.
std::vector<std::tuple<int, int, QTextCharFormat>> fragments(const QTextDocument* doc, int from, int to) {
  std::vector<std::tuple<int, int, QTextCharFormat>> out;
  for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
    for (auto it = b.begin(); !it.atEnd(); ++it) {
      const QTextFragment frag = it.fragment();
      if (!frag.isValid()) continue;
      const int s = std::max(from, frag.position()), e = std::min(to, frag.position() + frag.length());
      if (s < e) out.emplace_back(s, e, frag.charFormat());
    }
  return out;
}

}  // namespace

void rescaleDocument(QTextDocument* doc, double pxPerPt, double dpiY) {
  QTextCursor c(doc);
  for (const auto& [s, e, fmt] : fragments(doc, 0, INT32_MAX)) {
    c.setPosition(s);
    c.setPosition(e, QTextCursor::KeepAnchor);
    c.setCharFormat(charFormat(runOf(fmt), pxPerPt, dpiY));
  }
  // Empty paragraphs' formats, which new typing takes.
  for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
    if (b.length() > 1) continue;
    c.setPosition(b.position());
    c.setBlockCharFormat(charFormat(runOf(b.charFormat()), pxPerPt, dpiY));
  }
}

void restyleSelection(QTextCursor& cursor, const TextStyleChange& change, double pxPerPt, double dpiY) {
  if (!cursor.hasSelection()) return;
  QTextDocument* doc = cursor.document();
  QTextCursor c(doc);
  c.beginEditBlock();
  for (const auto& [s, e, fmt] : fragments(doc, cursor.selectionStart(), cursor.selectionEnd())) {
    RichRun run = runOf(fmt);
    applyStyle(run, change);
    c.setPosition(s);
    c.setPosition(e, QTextCursor::KeepAnchor);
    c.setCharFormat(charFormat(run, pxPerPt, dpiY));
  }
  c.endEditBlock();
}

void setDocumentAlignment(QTextDocument* doc, pdfz::TextAlign align) {
  QTextCursor c(doc);
  c.select(QTextCursor::Document);
  QTextBlockFormat f;
  switch (align) {
    case pdfz::TextAlign::Center: f.setAlignment(Qt::AlignHCenter); break;
    case pdfz::TextAlign::Right: f.setAlignment(Qt::AlignRight); break;
    case pdfz::TextAlign::Justify: f.setAlignment(Qt::AlignJustify); break;
    default: f.setAlignment(Qt::AlignLeft);
  }
  c.mergeBlockFormat(f);
}

void setTabStops(QTextOption& option, const std::vector<double>& stops, double pxPerPt) {
  QList<QTextOption::Tab> tabs;
  for (double t : stops)
    if (t > 0) tabs.append(QTextOption::Tab(t * pxPerPt, QTextOption::LeftTab));
  option.setTabs(tabs);
  option.setTabStopDistance(36 * pxPerPt);
}

double lineAdvance(double lineSpacing, double baseSize, double largestSize) {
  if (baseSize > 0 && largestSize > baseSize * 1.05) return std::max(lineSpacing, lineSpacing * largestSize / baseSize);
  return lineSpacing;
}

pdfz::PlacedText layoutRich(const RichText& input, pdfz::TextAlign align, double width, double lineSpacing,
                            double baseSize, pdfz::PointF origin, const std::vector<double>& tabs, bool* complete) {
  if (complete) *complete = true;
  pdfz::PlacedText out;
  out.origin = origin;
  const RichText text = merged(input);

  // The fonts: one for each style of font, chosen for all the text it shows.
  std::vector<EditFont> fonts;
  std::vector<QString> fontText;
  std::vector<int> runFont(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    auto it = std::find(fonts.begin(), fonts.end(), text[i].font);
    if (it == fonts.end()) {
      fonts.push_back(text[i].font);
      fontText.push_back({});
      it = fonts.end() - 1;
    }
    const auto k = std::size_t(it - fonts.begin());
    runFont[i] = int(k);
    fontText[k] += text[i].text;
  }
  for (std::size_t k = 0; k < fonts.size(); ++k) {
    bool whole = true;
    pdfz::TextFont tf = pdfFont(fonts[k], fontText[k], &whole);
    if (tf.standard.empty() && tf.file.empty()) {
      out.fonts.clear();
      return out;
    }
    if (!whole && complete) *complete = false;
    out.fonts.push_back(std::move(tf));
  }
  if (out.fonts.empty()) {  // no text: still a font, for an empty result
    pdfz::TextFont tf;
    tf.standard = "Helvetica";
    out.fonts.push_back(tf);
  }

  // Each paragraph laid out at 100 pixels a point, for precision, and measured back.
  constexpr double K = 100;
  const RichRun base = mainStyle(text);
  const bool wrap = width > 0;
  QTextOption opt;
  opt.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
  opt.setUseDesignMetrics(true);
  Qt::Alignment qa = Qt::AlignLeft;
  if (wrap) {
    if (align == pdfz::TextAlign::Center) qa = Qt::AlignHCenter;
    else if (align == pdfz::TextAlign::Right) qa = Qt::AlignRight;
    else if (align == pdfz::TextAlign::Justify) qa = Qt::AlignJustify;
  }
  opt.setAlignment(qa);
  setTabStops(opt, tabs, K);

  // The runs cut at line breaks into paragraphs: (run index, text) pieces.
  struct Piece {
    std::size_t run;
    QString text;
  };
  std::vector<std::vector<Piece>> paras(1);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const QStringList parts = text[i].text.split(QLatin1Char('\n'));
    for (qsizetype p = 0; p < parts.size(); ++p) {
      if (p > 0) paras.emplace_back();
      if (!parts[p].isEmpty()) paras.back().push_back({i, parts[p]});
    }
  }

  bool firstLine = true;
  for (const auto& para : paras) {
    QString s;
    QList<QTextLayout::FormatRange> ranges;
    std::vector<std::pair<int, std::size_t>> starts;  // where each piece starts, and its run
    for (const Piece& p : para) {
      QTextLayout::FormatRange fr;
      fr.start = int(s.size());
      fr.length = int(p.text.size());
      QFont q = qtFont(text[p.run].font, text[p.run].size * K);
      fr.format.setFont(q);
      ranges.push_back(fr);
      starts.emplace_back(int(s.size()), p.run);
      s += p.text;
    }
    QTextLayout layout(s, qtFont(base.font, std::max(1.0, base.size) * K));
    layout.setFormats(ranges);
    layout.setTextOption(opt);
    layout.beginLayout();
    for (;;) {
      QTextLine line = layout.createLine();
      if (!line.isValid()) break;
      line.setLineWidth(wrap ? width * K : 1e9);
    }
    layout.endLayout();
    const int lineCount = std::max(1, layout.lineCount());
    for (int li = 0; li < lineCount; ++li) {
      pdfz::PlacedLine placed;
      double largest = 0;
      if (li < layout.lineCount()) {
        const QTextLine line = layout.lineAt(li);
        const int ls = line.textStart();
        int le = ls + line.textLength();
        while (le > ls && s.at(le - 1).isSpace()) --le;  // the space it wrapped at
        const bool justified = qa == Qt::AlignJustify && li + 1 < layout.lineCount();
        for (std::size_t k = 0; k < starts.size(); ++k) {
          const int rs = starts[k].first;
          const int re = k + 1 < starts.size() ? starts[k + 1].first : int(s.size());
          const int a = std::max(rs, ls), b = std::min(re, le);
          if (a >= b) continue;
          const RichRun& run = text[starts[k].second];
          largest = std::max(largest, run.size);
          auto addSpan = [&](int from, int to) {
            pdfz::TextSpan sp;
            sp.text = s.mid(from, to - from).toStdString();
            sp.x = line.cursorToX(from) / K;
            sp.font = runFont[starts[k].second];
            sp.size = run.size;
            sp.color = fromQColor(run.color);
            sp.rise = run.rise;
            placed.spans.push_back(std::move(sp));
          };
          if (!justified) {  // in pieces between tabs, each where its tab stop puts it
            int from = a;
            for (int k2 = a; k2 <= b; ++k2) {
              if (k2 < b && s.at(k2) != QLatin1Char('\t')) continue;
              if (k2 > from) addSpan(from, k2);
              from = k2 + 1;
            }
            continue;
          }
          // Justified: word by word, where the stretched spaces put them.
          int w = a;
          while (w < b) {
            while (w < b && s.at(w).isSpace()) ++w;
            int we = w;
            while (we < b && !s.at(we).isSpace()) ++we;
            if (we > w) addSpan(w, we);
            w = we;
          }
        }
      }
      if (largest <= 0) largest = baseSize;
      placed.advance = firstLine ? 0 : lineAdvance(lineSpacing, baseSize, largest);
      firstLine = false;
      out.lines.push_back(std::move(placed));
    }
  }
  return out;
}
