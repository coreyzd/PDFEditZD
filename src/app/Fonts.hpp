// SPDX-License-Identifier: GPL-3.0-only
// Fonts for Edit PDF: a PDF font name matched to a font on this computer, for typing in and
// laying out the text, and the font the edited text is written with. PDF's standard fonts
// (Helvetica, Times, Courier) need nothing embedded; anything else is embedded from the
// computer's font file.
#pragma once

#include "core/content.hpp"

#include <QFont>
#include <QString>

struct EditFont {
  QString family;   // as the computer calls it ("Arial", "Calibri") or a standard family
  bool bold = false;
  bool italic = false;
  bool operator==(const EditFont&) const = default;
};

// What a block's font is best edited as.
EditFont editFontFor(const pdfz::ContentBlock& block);  // its own (most of its text's) font
EditFont editFontFor(const pdfz::TextRun& run);
EditFont editFontFor(const std::string& pdfFamily, const std::string& fontName, bool bold, bool italic);
// The Qt font to type and measure with, at `pixelSize`.
QFont qtFont(const EditFont& f, double pixelSize);
// The font to write `text` with: a standard font if it is one and can show the text, else the
// computer's font file (Arial if the family isn't on this computer). Empty `file` and
// `standard` if no font file could be found for text a standard font can't show. If the
// family's file lacks some of the characters, another font that has them is used instead;
// `complete` is false if no font had them all (those characters won't show).
pdfz::TextFont pdfFont(const EditFont& f, const QString& text, bool* complete = nullptr);
// The standard families first, then the computer's.
QStringList editFontFamilies();

