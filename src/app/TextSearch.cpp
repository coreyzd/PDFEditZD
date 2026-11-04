// SPDX-License-Identifier: GPL-3.0-only
#include "TextSearch.hpp"

#include "core/document.hpp"

#include <QElapsedTimer>
#include <QMetaObject>

#include <algorithm>

void TextCache::setDocument(std::shared_ptr<pdfz::Document> doc) {
  doc_ = std::move(doc);
  cache_.clear();
}

std::shared_ptr<const pdfz::PageText> TextCache::get(int page) {
  if (!doc_ || page < 0 || page >= doc_->pageCount()) return nullptr;
  if (auto* hit = cache_.object(page)) return *hit;
  auto text = std::make_shared<const pdfz::PageText>(doc_->extractText(page));
  cache_.insert(page, new std::shared_ptr<const pdfz::PageText>(text));
  return text;
}

SearchableText searchableText(const pdfz::PageText& t) {
  SearchableText out;
  out.text.reserve(t.size());
  out.unitToChar.reserve(std::size_t(t.size()));
  for (int i = 0; i < t.size(); ++i) {
    const char32_t c = t.chars[i];
    if (c == U'\r') continue;
    if (c == U'\n') {
      out.text += QLatin1Char(' ');
      out.unitToChar.push_back(i);
    } else if (c > 0xFFFF) {
      out.text += QChar(QChar::highSurrogate(c));
      out.text += QChar(QChar::lowSurrogate(c));
      out.unitToChar.push_back(i);
      out.unitToChar.push_back(i);
    } else {
      out.text += QChar(char16_t(c));  // including halves of a pair PDFium gave separately
      out.unitToChar.push_back(i);
    }
  }
  return out;
}

TextSearch::TextSearch(QObject* parent) : QObject(parent) {
  worker_ = std::thread([this] { run(); });
}

TextSearch::~TextSearch() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    pending_.reset();
  }
  ++generation_;  // a search in progress gives up after the page it's on
  wake_.notify_one();
  worker_.join();
}

void TextSearch::start(std::shared_ptr<pdfz::Document> doc, const QString& query, bool matchCase, bool wholeWords) {
  clear();
  if (!doc || query.isEmpty()) return;
  query_ = query;
  running_ = true;
  {
    std::lock_guard lock(mutex_);
    pending_ = Job{generation_.load(), std::move(doc), query, matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive,
                   wholeWords};
  }
  wake_.notify_one();
}

void TextSearch::clear() {
  ++generation_;
  {
    std::lock_guard lock(mutex_);
    pending_.reset();
  }
  query_.clear();
  matches_.clear();
  current_ = -1;
  running_ = false;
  emit matchesChanged();
}

std::pair<int, int> TextSearch::matchesOnPage(int page) const {
  auto lo = std::lower_bound(matches_.begin(), matches_.end(), page,
                             [](const SearchMatch& m, int p) { return m.page < p; });
  auto hi = std::upper_bound(lo, matches_.end(), page, [](int p, const SearchMatch& m) { return p < m.page; });
  return {int(lo - matches_.begin()), int(hi - matches_.begin())};
}

void TextSearch::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    search(job);
  }
}

void TextSearch::search(const Job& job) {
  std::vector<SearchMatch> batch;
  QElapsedTimer sinceSent;
  sinceSent.start();
  bool sentAny = false;
  const int pages = job.doc->pageCount();
  for (int page = 0; page < pages; ++page) {
    if (generation_.load() != job.generation) return;  // superseded: nothing more to send
    const pdfz::PageText text = job.doc->extractText(page);
    if (!text.empty()) {
      const SearchableText s = searchableText(text);
      for (qsizetype at = s.text.indexOf(job.query, 0, job.cs); at >= 0; at = s.text.indexOf(job.query, at + 1, job.cs)) {
        const qsizetype endUnit = at + job.query.size();
        if (job.wholeWords) {
          auto isWord = [&](qsizetype u) { return u >= 0 && u < s.text.size() && s.text[u].isLetterOrNumber(); };
          if (isWord(at - 1) || isWord(endUnit)) continue;
        }
        batch.push_back({page, s.unitToChar[std::size_t(at)], s.unitToChar[std::size_t(endUnit - 1)] + 1});
      }
    }
    // The first matches as soon as there are some, then a few batches a second.
    if (!batch.empty() && (!sentAny || sinceSent.elapsed() >= 100)) {
      QMetaObject::invokeMethod(this, [this, g = job.generation, b = std::move(batch)]() mutable {
        deliver(g, std::move(b), false);
      }, Qt::QueuedConnection);
      batch.clear();
      sinceSent.restart();
      sentAny = true;
    }
  }
  QMetaObject::invokeMethod(this, [this, g = job.generation, b = std::move(batch)]() mutable {
    deliver(g, std::move(b), true);
  }, Qt::QueuedConnection);
}

void TextSearch::deliver(quint64 generation, std::vector<SearchMatch> found, bool done) {
  if (generation != generation_.load()) return;  // from a search since replaced or cleared
  matches_.insert(matches_.end(), found.begin(), found.end());
  if (!found.empty() || done) {
    if (done) running_ = false;
    emit matchesChanged();
  }
  if (done) emit finished();
}
