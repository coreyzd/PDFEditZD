// SPDX-License-Identifier: GPL-3.0-only
#include "CommentDialog.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

CommentDialog::CommentDialog(const QString& heading, const QString& byline, const QString& text, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(heading);
  auto* lay = new QVBoxLayout(this);
  if (!byline.isEmpty()) {
    auto* by = new QLabel(byline, this);
    by->setEnabled(false);  // the muted text color
    lay->addWidget(by);
  }
  edit_ = new QPlainTextEdit(text, this);
  edit_->setPlaceholderText(tr("Type your comment"));
  edit_->setTabChangesFocus(true);
  lay->addWidget(edit_, 1);

  auto* bottom = new QHBoxLayout;
  sizeLabel_ = new QLabel(tr("Font size:"), this);
  size_ = new QDoubleSpinBox(this);
  size_->setRange(4, 144);
  size_->setDecimals(1);
  size_->setSuffix(tr(" pt"));
  sizeLabel_->hide();
  size_->hide();
  bottom->addWidget(sizeLabel_);
  bottom->addWidget(size_);
  bottom->addStretch(1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setToolTip(tr("Ctrl+Enter"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  bottom->addWidget(buttons);
  lay->addLayout(bottom);

  // Ctrl+Enter accepts from inside the text box (Enter makes a new line).
  auto* ok = new QAction(this);
  ok->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)});
  connect(ok, &QAction::triggered, this, &QDialog::accept);
  addAction(ok);

  resize(380, 220);
  edit_->setFocus();
  edit_->moveCursor(QTextCursor::End);
}

QString CommentDialog::text() const { return edit_->toPlainText(); }

void CommentDialog::showFontSize(double size) {
  size_->setValue(size);
  size_->show();
  sizeLabel_->show();
}

double CommentDialog::fontSize() const { return size_->value(); }
