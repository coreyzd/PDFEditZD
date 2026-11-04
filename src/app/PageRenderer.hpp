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
  // One page's content changed (a comment added, say): what was rendered of it is out of date.
  // It stays cached, and keeps being shown, until the new rendering replaces it, so the page
  // never goes blank in between.
  void invalidatePage(int page);

  // Rounds a scale to one tiles are made at, so nearby zooms share them.
  static int scaleKeyFor(double devicePixelsPerPoint);

  // Cached images, or null ones. `fresh` says whether it shows the page as it is now (false
  // after invalidatePage(), until it's rendered again).
  QImage tile(const TileKey& key, bool* fresh = nullptr) const;
  QImage preview(int page, bool* fresh = nullptr) const;
  // The tiles of a page cached at other zooms than `scaleKey`: drawn, scaled, while the ones at
  // the new zoom are on their way.
  std::vector<std::pair<TileKey, QImage>> otherTiles(int page, int scaleKey) const;

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
    quint64 epoch = 0;  // the page's epoch when asked for; results from older ones are dropped
    friend bool operator==(const Job&, const Job&) = default;
  };

  void run();
  void deliver(quint64 serial, const Job& job, QImage image);
  quint64 epochOf(int page) const { return pageEpochs_.value(page, 0); }
  void clearQueues();  // caller holds mutex_

  std::shared_ptr<pdfz::Document> doc_;
  quint64 serial_ = 0;  // bumped on every setDocument()/invalidate(); stale results are dropped

  struct Cached {
    QImage image;
    quint64 epoch = 0;  // the page's epoch it was rendered at
  };
  QCache<TileKey, Cached> tiles_;
  QCache<int, Cached> previews_;
  QHash<int, quint64> pageEpochs_;  // bumped by invalidatePage(); UI thread only

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
