// SPDX-License-Identifier: GPL-3.0-only
// File > Print: the system print dialog, then each page rendered at the printer's resolution,
// shrunk to fit the printable area and turned to match the paper.
#pragma once

#include <QString>

#include <functional>

class QWidget;
class QPrinter;
namespace pdfz { class Document; }

// Shows the print dialog and prints. Returns false if the user canceled or printing failed.
bool printDocument(QWidget* parent, const pdfz::Document& doc, const QString& title, int currentPage);

// Prints pages [from, to] (0-based, inclusive) on an already set-up printer, with no dialog.
// `progress` is called before each page (and between the strips of a big one) with how many
// pages are done; returning false cancels.
bool printPages(QPrinter& printer, const pdfz::Document& doc, int from, int to,
                const std::function<bool(int done, int total)>& progress = {});
