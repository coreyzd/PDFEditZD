// SPDX-License-Identifier: GPL-3.0-only
// Renders pages on a worker thread and keeps what it rendered: tiles of a page at the view's
// zoom, and one small preview per page (shown while tiles arrive, and as the thumbnails).
#pragma once

#include "core/document.hpp"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// One tile: page `page` rendered at `scaleKey` / 1000 device pixels per point, the square of
// kTileSize pixels at column `tx`, row `ty`.
struct TileKey {
  int page = 0;
  int scaleKey = 0;
  int tx = 0;
  int ty = 0;
  double scale() const { return scaleKey / 1000.0; }
  friend bool operator==(const TileKey&, const TileKey&) = default;
};
inline size_t qHash(const TileKey& k, size_t seed = 0) noexcept {
  return qHashMulti(seed, k.page, k.scaleKey, k.tx, k.ty);
}
Q_DECLARE_METATYPE(TileKey)

class PageRenderer : public QObject {
  Q_OBJECT
public:
  static constexpr int kTileSize = 512;     // device pixels
  static constexpr int kPreviewBox = 320;   // previews fit in this many device pixels square

  explicit PageRenderer(QObject* parent = nullptr);
  ~PageRenderer() override;

  // Drops every cached image and pending job; nullptr for no document.
  void setDocument(std::shared_ptr<pdfz::Document> doc);
  const std::shared_ptr<pdfz::Document>& document() const { return doc_; }
  // The pages changed (deleted, moved, rotated...): forget everything rendered so far.
  void invalidate();

  // Rounds a scale to one tiles are made at, so nearby zooms share them.
  static int scaleKeyFor(double devicePixelsPerPoint);

  // Cached images, or null ones.
  QImage tile(const TileKey& key) const;
  QImage preview(int page) const;

  // What the view needs now, most important first. Replaces what the view asked for before,
  // so work for places scrolled past is dropped.
  void requestView(const std::vector<int>& previews, const std::vector<TileKey>& tiles);
  // A preview for the thumbnail list: done after the view's work, newest request first.
  void requestPreview(int page);

signals:
  void tileReady(const TileKey& key);
  void previewReady(int page);

private:
  struct Job {
    bool isTile = false;
    int page = 0;
    TileKey key;
    friend bool operator==(const Job&, const Job&) = default;
  };

  void run();
  void deliver(quint64 serial, bool isTile, int page, TileKey key, QImage image);
  void clearQueues();  // caller holds mutex_

  std::shared_ptr<pdfz::Document> doc_;
  quint64 serial_ = 0;  // bumped on every setDocument()/invalidate(); stale results are dropped

  QCache<TileKey, QImage> tiles_;
  QCache<int, QImage> previews_;

  // Shared with the worker thread, under mutex_.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::shared_ptr<pdfz::Document> jobDoc_;
  quint64 jobSerial_ = 0;
  std::deque<Job> viewJobs_;
  std::deque<Job> lowJobs_;
  std::optional<Job> current_;
  bool stop_ = false;

  std::thread worker_;
};
