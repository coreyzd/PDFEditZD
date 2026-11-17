// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentTabs.hpp"

#include <QApplication>
#include <QMouseEvent>

DocumentTabBar::DocumentTabBar(QWidget* parent) : QTabBar(parent) {}

void DocumentTabBar::mousePressEvent(QMouseEvent* e) {
  pressed_ = e->button() == Qt::LeftButton ? tabAt(e->position().toPoint()) : -1;
  QTabBar::mousePressEvent(e);
}

void DocumentTabBar::mouseMoveEvent(QMouseEvent* e) {
  // Dragged well above or below the bar: off it. (Along it, the bar moves the tab among the
  // others, as before.)
  const QPoint pos = e->position().toPoint();
  const int off = std::max(QApplication::startDragDistance() * 3, height() / 2);
  if (pressed_ >= 0 && (e->buttons() & Qt::LeftButton) && (pos.y() < -off || pos.y() > height() + off)) {
    pressed_ = -1;
    // The bar's own moving of the tab ends first (as if the button went up where it is), so
    // the tab is back in its place.
    QMouseEvent up(QEvent::MouseButtonRelease, e->position(), e->scenePosition(), e->globalPosition(), Qt::LeftButton,
                   Qt::NoButton, e->modifiers());
    QTabBar::mouseReleaseEvent(&up);
    emit tabDraggedOff(currentIndex());  // (moved along the bar first, it was made current)
    return;
  }
  QTabBar::mouseMoveEvent(e);
}

void DocumentTabBar::mouseReleaseEvent(QMouseEvent* e) {
  pressed_ = -1;
  QTabBar::mouseReleaseEvent(e);
}

DocumentTabWidget::DocumentTabWidget(QWidget* parent) : QTabWidget(parent), bar_(new DocumentTabBar(this)) {
  setTabBar(bar_);
}
