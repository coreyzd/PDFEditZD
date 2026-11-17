// SPDX-License-Identifier: GPL-3.0-only
// The window's document tabs, whose tabs can be dragged off the bar (as in Explorer, Acrobat
// and web browsers): out of the window into a window of their own, or into another window.
#pragma once

#include <QTabBar>
#include <QTabWidget>

class DocumentTabBar : public QTabBar {
  Q_OBJECT
public:
  explicit DocumentTabBar(QWidget* parent = nullptr);

signals:
  // Tab `index` was dragged away from the bar: the window takes it from here (a drag of its
  // own, which ends in another window or in a new one).
  void tabDraggedOff(int index);

protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;

private:
  int pressed_ = -1;  // the tab the left button went down on
};

class DocumentTabWidget : public QTabWidget {
  Q_OBJECT
public:
  explicit DocumentTabWidget(QWidget* parent = nullptr);
  DocumentTabBar* bar() const { return bar_; }

private:
  DocumentTabBar* bar_;
};

// What a dragged tab carries: which tab, in this program only.
inline const char* const kTabMime = "application/x-pdfeditzd-tab";
