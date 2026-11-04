// SPDX-License-Identifier: GPL-3.0-only
#include "InlineTextEdit.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTextDocument>
#include <QTextOption>

InlineTextEdit::InlineTextEdit(QWidget* parent) : QTextEdit(parent) {
  setAcceptRichText(false);  // pasting brings the text, not its formatting
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  setTabChangesFocus(false);
  // See-through: the page, and the box the view draws, show under the text.
  setAttribute(Qt::WA_TranslucentBackground);
  viewport()->setAutoFillBackground(false);
  setAutoFillBackground(false);
  QPalette pal = palette();
  pal.setColor(QPalette::Base, Qt::transparent);
  setPalette(pal);
  viewport()->setCursor(Qt::IBeamCursor);
  connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
          [this] { emit heightNeeded(); });
}

void InlineTextEdit::setLook(const QFont& font, const QColor& textColor, int align, double padding) {
  QPalette pal = palette();
  pal.setColor(QPalette::Base, Qt::transparent);
  pal.setColor(QPalette::Text, textColor);
  setPalette(pal);
  document()->setDefaultFont(font);
  setFont(font);
  document()->setDocumentMargin(padding);
  QTextOption opt = document()->defaultTextOption();
  opt.setAlignment(align == 1 ? Qt::AlignHCenter : align == 2 ? Qt::AlignRight : Qt::AlignLeft);
  opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  document()->setDefaultTextOption(opt);
}

double InlineTextEdit::contentHeight() const { return document()->size().height(); }

void InlineTextEdit::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) {
    e->accept();
    emit finished();
    return;
  }
  QTextEdit::keyPressEvent(e);
}

bool focusLeftForGood(QFocusEvent* e) {
  if (e->reason() == Qt::PopupFocusReason || e->reason() == Qt::ActiveWindowFocusReason) return false;
  if (QApplication::activePopupWidget()) return false;
  if (QWidget* now = QApplication::focusWidget(); now && (now->window()->windowFlags() & Qt::Popup) == Qt::Popup)
    return false;
  return true;
}

namespace {
// Tab or Shift+Tab, which widgets otherwise hand to the focus chain before seeing them.
bool isTab(QEvent* e, bool& backwards) {
  if (e->type() != QEvent::KeyPress) return false;
  auto* k = static_cast<QKeyEvent*>(e);
  if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return false;
  backwards = k->key() == Qt::Key_Backtab || (k->key() == Qt::Key_Tab && (k->modifiers() & Qt::ShiftModifier));
  return k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab;
}
}  // namespace

void InlineTextEdit::setFormField(const QColor& background) {
  formField_ = true;
  setAttribute(Qt::WA_TranslucentBackground, false);
  viewport()->setAutoFillBackground(true);
  QPalette pal = palette();
  pal.setColor(QPalette::Base, background);
  setPalette(pal);
}

bool InlineTextEdit::event(QEvent* e) {
  bool back = false;
  if (formField_ && isTab(e, back)) {
    emit tabbed(back);
    return true;
  }
  return QTextEdit::event(e);
}

void InlineTextEdit::focusOutEvent(QFocusEvent* e) {
  QTextEdit::focusOutEvent(e);
  if (focusLeftForGood(e)) emit finished();
}

FieldLineEdit::FieldLineEdit(QWidget* parent) : QLineEdit(parent) {
  setFrame(false);
  setTextMargins(0, 0, 0, 0);
}

bool FieldLineEdit::event(QEvent* e) {
  bool back = false;
  if (isTab(e, back)) {
    emit tabbed(back);
    return true;
  }
  return QLineEdit::event(e);
}

void FieldLineEdit::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
    e->accept();
    emit finished();
    return;
  }
  QLineEdit::keyPressEvent(e);
}

void FieldLineEdit::focusOutEvent(QFocusEvent* e) {
  QLineEdit::focusOutEvent(e);
  if (focusLeftForGood(e)) emit finished();
}
