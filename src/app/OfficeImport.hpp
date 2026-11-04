// SPDX-License-Identifier: GPL-3.0-only
// Turning other files into PDF pages for Insert Pages from File: Word documents (through the
// installed Microsoft Word, in the background, or else LibreOffice) and pictures.
#pragma once

#include "core/document.hpp"

#include <QString>
#include <QStringList>

class QWidget;

// File dialog filter patterns.
QStringList wordPatterns();   // *.docx, *.doc, *.rtf...
QStringList imagePatterns();  // *.png, *.jpg...
bool isWordFile(const QString& path);
bool isImageFile(const QString& path);

// Converts a Word document to a PDF in a temporary folder, showing progress (it can take a few
// seconds while Word starts). Returns the PDF's path, which the caller deletes; empty if it
// failed (`error` says why) or was canceled (`error` empty).
QString convertToPdf(QWidget* parent, const QString& path, QString* error);

// A picture as a page's image: JPEG files are kept as they are (unless they need turning
// upright), anything else as pixels, scaled down if very large. False if it can't be read.
bool loadPageImage(const QString& path, pdfz::Document::PageImage& out, QString* error);
