// SPDX-License-Identifier: GPL-3.0-only
// Edit > Find: the bar above the pages. Typing searches as you go (after a short pause).
#pragma once

#include <QTimer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;

class FindBar : public QWidget {
  Q_OBJECT
public:
  explicit FindBar(QWidget* parent = nullptr);

  // Shows the bar with the cursor in the box; `text`, if given, replaces what's there.
  void activate(const QString& text = {});
  QString text() const;
  bool matchCase() const;
  bool wholeWords() const;
  // "3 of 12", "No matches"...
  void setStatus(const QString& status, bool notFound = false);

signals:
  void searchChanged();  // the text or an option changed
  void findNext();
  void findPrevious();
  void closed();

protected:
  bool eventFilter(QObject* obj, QEvent* e) override;

private:
  QLineEdit* edit_;
  QToolButton* prev_;
  QToolButton* next_;
  QCheckBox* case_;
  QCheckBox* words_;
  QLabel* status_;
  QTimer debounce_;
};
