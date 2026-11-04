// SPDX-License-Identifier: GPL-3.0-only
// Page text for the UI: a cache of extracted pages (selection, hit-testing, highlights) and the
// document-wide search behind the find bar.
#pragma once

#include "core/text.hpp"

#include <QCache>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace pdfz { class Document; }

// Extracted text of recently used pages. Entries are shared, so a page's text stays valid for
// as long as the caller holds it even if the cache drops it meanwhile.
class TextCache {
public:
  void setDocument(std::shared_ptr<pdfz::Document> doc);
  // The page's text, extracted on first use; null for a page that doesn't exist.
  std::shared_ptr<const pdfz::PageText> get(int page);
  void invalidate(int page) { cache_.remove(page); }  // its content was edited

private:
  std::shared_ptr<pdfz::Document> doc_;
  QCache<int, std::shared_ptr<const pdfz::PageText>> cache_{64};
};

// A page's text as a QString for matching. PDFium's "\r\n" line breaks become one space, so a
// phrase is found even where it wraps onto the next line.
struct SearchableText {
  QString text;
  std::vector<int> unitToChar;  // character index of each UTF-16 unit of `text`
};
SearchableText searchableText(const pdfz::PageText& t);

struct SearchMatch {
  int page = 0;
  int start = 0;  // character range [start, end) of the page's PageText
  int end = 0;
};

// Finds every match in the document on a thread of its own, so the window stays responsive
// however long the document is. Matches arrive in page order, in batches a few times a second;
// starting a new search (or clearing) drops what the old one was still going to send.
class TextSearch : public QObject {
  Q_OBJECT
public:
  explicit TextSearch(QObject* parent = nullptr);
  ~TextSearch() override;

  void start(std::shared_ptr<pdfz::Document> doc, const QString& query, bool matchCase, bool wholeWords);
  void clear();

  const QString& query() const { return query_; }
  bool running() const { return running_; }
  const std::vector<SearchMatch>& matches() const { return matches_; }
  // The matches on one page, as an index range into matches().
  std::pair<int, int> matchesOnPage(int page) const;

  int current() const { return current_; }
  void setCurrent(int index) { current_ = index; }

signals:
  void matchesChanged();  // more were found, or the search was cleared
  void finished();

private:
  struct Job {
    quint64 generation = 0;
    std::shared_ptr<pdfz::Document> doc;
    QString query;
    Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    bool wholeWords = false;
  };
  void run();                     // the worker thread
  void search(const Job& job);    // on the worker
  void deliver(quint64 generation, std::vector<SearchMatch> found, bool done);  // on the GUI thread

  QString query_;
  std::vector<SearchMatch> matches_;
  int current_ = -1;
  bool running_ = false;

  std::atomic<quint64> generation_{0};  // bumped by start() and clear(): the worker stops early
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<Job> pending_;
  bool stop_ = false;
  std::thread worker_;
};
