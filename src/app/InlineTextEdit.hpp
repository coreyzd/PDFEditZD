// SPDX-License-Identifier: GPL-3.0-only
// The editor laid over a text box while it's typed in, on the page itself (as in Acrobat). It
// shows only the text: the page view draws the box's fill and border under it. Esc, or the
// keyboard going elsewhere (a click on the page, another panel), ends the editing; popups and
// the style controls, which don't take the keyboard, don't.
#pragma once

#include <QLineEdit>
#include <QTextEdit>

class QFocusEvent;

// Whether the keyboard leaving an editor means the editing is over: not when a menu or a list
// opens (a combo box's), nor when the window itself loses the focus (another program, a dialog).
bool focusLeftForGood(QFocusEvent* e);

class InlineTextEdit : public QTextEdit {
  Q_OBJECT
public:
  explicit InlineTextEdit(QWidget* parent);

  // The look: the font at its on-screen pixel size, the text color, alignment (0 left, 1
  // center, 2 right) and the space between the box's edge and the text, in pixels.
  void setLook(const QFont& font, const QColor& textColor, int align, double padding);
  // The height the text needs, in pixels (padding included).
  double contentHeight() const;
  // Text typed on a form (Fill & Sign): no wrapping, so the box can grow to fit the longest
  // line; textWidth() is that line's width in pixels (padding not included).
  void setAutoWidth(bool on);
  // Lines exactly this far apart (pixels), as the page's text was (Edit PDF).
  void setLineHeight(double px);
  double textWidth() const;
  // A form field: Tab and Shift+Tab move to the next field (tabbed()) instead of typing a tab,
  // and the editor is filled with `background` so the field's old look doesn't show through.
  void setFormField(const QColor& background);

signals:
  void finished();       // Esc, or the keyboard went elsewhere
  void heightNeeded();   // the text grew or shrank
  void tabbed(bool backwards);

protected:
  bool event(QEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;

private:
  bool formField_ = false;
  double lineHeight_ = 0;
};

// A one-line form field being typed in: Enter or Esc ends it, Tab moves on.
class FieldLineEdit : public QLineEdit {
  Q_OBJECT
public:
  explicit FieldLineEdit(QWidget* parent);

signals:
  void finished();
  void tabbed(bool backwards);

protected:
  bool event(QEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;
};
