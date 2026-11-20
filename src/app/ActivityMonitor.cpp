// SPDX-License-Identifier: GPL-3.0-only
#include "ActivityMonitor.hpp"

#include <QApplication>
#include <QEvent>
#include <QMetaEnum>
#include <QWidget>

#include <algorithm>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX  // std::min and std::max, not windows.h's macros
#endif
#include <windows.h>
#else
#include <ctime>
#endif

std::atomic<long> ActivityMonitor::tilesRendered{0};
std::atomic<long> ActivityMonitor::previewsRendered{0};
std::atomic<long> ActivityMonitor::renderMicroseconds{0};

namespace {

constexpr int kWindowSeconds = 10;    // what a report covers
constexpr int kIdleMs = 12'000;       // no clicks, keys or wheel for this long: nobody's using it
constexpr double kBusyShare = 0.25;   // a quarter of a processor, on average, is working hard
constexpr int kReportGapSeconds = 300;
constexpr int kChild = 0x100000;      // in an event key: to a plain widget inside this class

#ifdef Q_OS_WIN
double seconds(const FILETIME& kernel, const FILETIME& user) {
  auto v = [](const FILETIME& f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7; };
  return v(kernel) + v(user);
}
double processCpu() {
  FILETIME c, e, k, u;
  return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? seconds(k, u) : 0;
}
double threadCpu() {  // the calling thread's: the timer runs on the window thread
  FILETIME c, e, k, u;
  return GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? seconds(k, u) : 0;
}
#else
double clockSeconds(clockid_t id) {
  timespec ts{};
  return clock_gettime(id, &ts) == 0 ? double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9 : 0;
}
double processCpu() { return clockSeconds(CLOCK_PROCESS_CPUTIME_ID); }
double threadCpu() { return clockSeconds(CLOCK_THREAD_CPUTIME_ID); }
#endif

bool isInput(QEvent::Type t) {
  switch (t) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
      return true;
    default:
      return false;
  }
}

}  // namespace

ActivityMonitor::ActivityMonitor(QObject* parent) : QObject(parent) {
  qApp->installEventFilter(this);
  sinceInput_.start();
  lastProcess_ = processCpu();
  lastUi_ = threadCpu();
  timer_.setInterval(1000);
  timer_.setTimerType(Qt::VeryCoarseTimer);
  connect(&timer_, &QTimer::timeout, this, &ActivityMonitor::tick);
  timer_.start();
}

bool ActivityMonitor::eventFilter(QObject* watched, QEvent* e) {
  const QEvent::Type t = e->type();
  if (isInput(t)) sinceInput_.restart();
  // The monitor's own second ticking isn't counted. A plain QWidget (a scroll area's
  // viewport, say) is told by what it's in: its type is marked kChild.
  if (watched == &timer_) return false;
  const char* cls = watched->metaObject()->className();
  int key = int(t);
  if (watched->metaObject() == &QWidget::staticMetaObject && watched->parent()) {
    cls = watched->parent()->metaObject()->className();
    key |= kChild;
  }
  ++counting_[{key, cls}];
  return false;
}

void ActivityMonitor::tick() {
  Second s;
  const double process = processCpu(), ui = threadCpu();
  s.processCpu = std::max(0.0, process - lastProcess_);
  s.uiCpu = std::max(0.0, ui - lastUi_);
  lastProcess_ = process;
  lastUi_ = ui;
  const long tiles = tilesRendered, previews = previewsRendered, us = renderMicroseconds;
  s.tiles = tiles - lastTiles_;
  s.previews = previews - lastPreviews_;
  s.renderUs = us - lastRenderUs_;
  lastTiles_ = tiles;
  lastPreviews_ = previews;
  lastRenderUs_ = us;
  s.events.swap(counting_);
  window_.push_back(std::move(s));
  while (int(window_.size()) > kWindowSeconds) window_.pop_front();
  if (quietReports_ > 0) --quietReports_;

  double cpu = 0, uiCpu = 0;
  for (const Second& w : window_) {
    cpu += w.processCpu;
    uiCpu += w.uiCpu;
  }
  const double share = cpu / double(window_.size()), uiShare = uiCpu / double(window_.size());
  const bool idle = sinceInput_.elapsed() >= kIdleMs;

  if (busy_ && share < kBusyShare / 4) {  // it settled down
    qInfo("Busy spell over (%.0f%% of a processor now)", share * 100);
    busy_ = false;
    quietReports_ = std::min(quietReports_, 60);
  }
  if (!idle || int(window_.size()) < kWindowSeconds || share < kBusyShare || quietReports_ > 0) return;

  // What it was doing.
  long nTiles = 0, nPreviews = 0, renderUs = 0;
  QHash<std::pair<int, const char*>, int> events;
  for (const Second& w : window_) {
    nTiles += w.tiles;
    nPreviews += w.previews;
    renderUs += w.renderUs;
    for (auto it = w.events.begin(); it != w.events.end(); ++it) events[it.key()] += it.value();
  }
  std::vector<std::pair<int, std::pair<int, const char*>>> top;
  for (auto it = events.begin(); it != events.end(); ++it) top.push_back({it.value(), it.key()});
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  const QMetaEnum types = QMetaEnum::fromType<QEvent::Type>();
  QStringList most;
  for (std::size_t i = 0; i < top.size() && i < 14; ++i) {
    const int type = top[i].second.first & ~kChild;
    const char* name = types.valueToKey(type);
    const QString to = QString::fromLatin1(top[i].second.second);
    most << QStringLiteral("%1/s %2 -> %3")
                .arg(top[i].first / double(kWindowSeconds), 0, 'f', 1)
                .arg(name ? QString::fromLatin1(name) : QString::number(type),
                     (top[i].second.first & kChild) ? QStringLiteral("a widget in ") + to : to);
  }
  QWidget* focus = QApplication::focusWidget();
  QWidget* active = QApplication::activeWindow();
  qInfo("Busy while idle for %d s: %.0f%% of a processor (window thread %.0f%%); rendered %ld tiles and %ld previews "
        "(%.1f s of rendering); app %s, focus on %s; events: %s",
        kWindowSeconds, share * 100, uiShare * 100, nTiles, nPreviews, double(renderUs) / 1e6,
        active ? (active->isMinimized() ? "minimized" : "active") : "in the background",
        focus ? focus->metaObject()->className() : "nothing", qUtf8Printable(most.join(QStringLiteral(", "))));
  busy_ = true;
  quietReports_ = kReportGapSeconds;
}
