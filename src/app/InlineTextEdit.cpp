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

void InlineTextEdit::focusOutEvent(QFocusEvent* e) {
  QTextEdit::focusOutEvent(e);
  // A menu or a list opening (a combo box's), or the window losing the focus (another program,
  // a color dialog), isn't the end: the keyboard comes back here afterwards.
  if (e->reason() == Qt::PopupFocusReason || e->reason() == Qt::ActiveWindowFocusReason) return;
  if (QApplication::activePopupWidget()) return;
  if (QWidget* now = QApplication::focusWidget(); now && (now->window()->windowFlags() & Qt::Popup) == Qt::Popup) return;
  emit finished();
}
