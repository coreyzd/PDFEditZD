// SPDX-License-Identifier: GPL-3.0-only
#include "FindBar.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
  auto* lay = new QHBoxLayout(this);
  lay->setContentsMargins(6, 3, 6, 3);
  lay->setSpacing(6);

  edit_ = new QLineEdit(this);
  edit_->setPlaceholderText(tr("Find in document"));
  edit_->setClearButtonEnabled(true);
  edit_->setMinimumWidth(240);
  edit_->installEventFilter(this);

  prev_ = new QToolButton(this);
  prev_->setIcon(QIcon::fromTheme("go-up", style()->standardIcon(QStyle::SP_ArrowUp)));
  prev_->setToolTip(tr("Previous match (Shift+Enter, Shift+F3)"));
  next_ = new QToolButton(this);
  next_->setIcon(QIcon::fromTheme("go-down", style()->standardIcon(QStyle::SP_ArrowDown)));
  next_->setToolTip(tr("Next match (Enter, F3)"));
  case_ = new QCheckBox(tr("Match case"), this);
  words_ = new QCheckBox(tr("Whole words"), this);
  status_ = new QLabel(this);
  status_->setMinimumWidth(110);
  auto* close = new QToolButton(this);
  close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
  close->setAutoRaise(true);
  close->setToolTip(tr("Close (Esc)"));

  lay->addWidget(new QLabel(tr("Find:"), this));
  lay->addWidget(edit_);
  lay->addWidget(prev_);
  lay->addWidget(next_);
  lay->addWidget(case_);
  lay->addWidget(words_);
  lay->addWidget(status_);
  lay->addStretch(1);
  lay->addWidget(close);

  debounce_.setSingleShot(true);
  debounce_.setInterval(250);
  connect(&debounce_, &QTimer::timeout, this, &FindBar::searchChanged);
  connect(edit_, &QLineEdit::textEdited, this, [this] { debounce_.start(); });
  connect(edit_, &QLineEdit::textChanged, this, [this](const QString& t) {
    if (t.isEmpty()) {  // the clear button: no need to wait
      debounce_.stop();
      emit searchChanged();
    }
  });
  connect(case_, &QCheckBox::toggled, this, &FindBar::searchChanged);
  connect(words_, &QCheckBox::toggled, this, &FindBar::searchChanged);
  connect(prev_, &QToolButton::clicked, this, &FindBar::findPrevious);
  connect(next_, &QToolButton::clicked, this, &FindBar::findNext);
  connect(close, &QToolButton::clicked, this, [this] {
    hide();
    emit closed();
  });
}

void FindBar::activate(const QString& text) {
  show();
  if (!text.isEmpty() && text != edit_->text()) {
    edit_->setText(text);
    debounce_.stop();
    emit searchChanged();
  }
  edit_->setFocus();
  edit_->selectAll();
}

QString FindBar::text() const { return edit_->text(); }
bool FindBar::matchCase() const { return case_->isChecked(); }
bool FindBar::wholeWords() const { return words_->isChecked(); }

void FindBar::setStatus(const QString& status, bool notFound) {
  status_->setText(status);
  // A red-tinted box when there's nothing to find, as browsers do.
  edit_->setStyleSheet(notFound ? QStringLiteral("QLineEdit { background: #fbe3e4; color: black; }") : QString());
}

bool FindBar::eventFilter(QObject* obj, QEvent* e) {
  if (obj == edit_ && e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
      if (debounce_.isActive()) {  // Enter right after typing: search now, then go
        debounce_.stop();
        emit searchChanged();
      }
      if (k->modifiers() & Qt::ShiftModifier) emit findPrevious();
      else emit findNext();
      return true;
    }
    if (k->key() == Qt::Key_Escape) {
      hide();
      emit closed();
      return true;
    }
  }
  return QWidget::eventFilter(obj, e);
}
