// SPDX-License-Identifier: GPL-3.0-only
// Notices the program working hard while nobody is using it (no clicks, keys or wheel for a
// while) and writes what it was doing to pdfeditzd.log: how much of a processor it took (all
// of it, and the window thread's share), how many pages and tiles were rendered, and the
// events handled most, by kind and by the kind of widget or object they went to. Cheap enough
// to run all the time: it counts events, and looks at the numbers once a second.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <deque>
#include <utility>

class ActivityMonitor : public QObject {
  Q_OBJECT
public:
  explicit ActivityMonitor(QObject* parent = nullptr);  // watches the whole application

  // Rendering done on PageRenderer's thread (counted there).
  static std::atomic<long> tilesRendered;
  static std::atomic<long> previewsRendered;
  static std::atomic<long> renderMicroseconds;

protected:
  bool eventFilter(QObject* watched, QEvent* e) override;

private:
  void tick();

  struct Second {
    double processCpu = 0;  // seconds of processor time in this second, every thread
    double uiCpu = 0;       // the window thread's
    long tiles = 0, previews = 0, renderUs = 0;
    QHash<std::pair<int, const char*>, int> events;  // (event type, receiver class) -> count
  };

  QTimer timer_;
  QElapsedTimer sinceInput_;  // since the last click, key or wheel
  QHash<std::pair<int, const char*>, int> counting_;
  std::deque<Second> window_;  // the last seconds
  double lastProcess_ = 0, lastUi_ = 0;
  long lastTiles_ = 0, lastPreviews_ = 0, lastRenderUs_ = 0;
  int quietReports_ = 0;  // seconds before another report may be written
  bool busy_ = false;
};
