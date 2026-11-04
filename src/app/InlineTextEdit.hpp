// SPDX-License-Identifier: GPL-3.0-only
// The editor laid over a text box while it's typed in, on the page itself (as in Acrobat). It
// shows only the text: the page view draws the box's fill and border under it. Esc, or the
// keyboard going elsewhere (a click on the page, another panel), ends the editing; popups and
// the style controls, which don't take the keyboard, don't.
#pragma once

#include <QTextEdit>

class InlineTextEdit : public QTextEdit {
  Q_OBJECT
public:
  explicit InlineTextEdit(QWidget* parent);

  // The look: the font at its on-screen pixel size, the text color, alignment (0 left, 1
  // center, 2 right) and the space between the box's edge and the text, in pixels.
  void setLook(const QFont& font, const QColor& textColor, int align, double padding);
  // The height the text needs, in pixels (padding included).
  double contentHeight() const;

signals:
  void finished();       // Esc, or the keyboard went elsewhere
  void heightNeeded();   // the text grew or shrank

protected:
  void keyPressEvent(QKeyEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;
};
