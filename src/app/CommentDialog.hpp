// SPDX-License-Identifier: GPL-3.0-only
// Writing or editing a comment's text: a sticky note, a text box, or the comment attached to a
// highlight or a drawing.
#pragma once

#include <QDialog>

class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;

class CommentDialog : public QDialog {
  Q_OBJECT
public:
  // `heading`: "Sticky Note", "Highlight"...; `byline`: "Corey · Nov 4, 2026 9:30 AM".
  CommentDialog(const QString& heading, const QString& byline, const QString& text, QWidget* parent = nullptr);

  QString text() const;
  // Text boxes: the font size control.
  void showFontSize(double size);
  double fontSize() const;

private:
  QPlainTextEdit* edit_;
  QDoubleSpinBox* size_ = nullptr;
  QLabel* sizeLabel_ = nullptr;
};
