// SPDX-License-Identifier: GPL-3.0-only
// View > Comments: every comment and markup in the document, by page. Listed a few pages per
// event-loop turn, so a long document opens without waiting for its comments.
#pragma once

#include <QTimer>
#include <QWidget>

#include <memory>

namespace pdfz { class Document; }
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

class CommentsPanel : public QWidget {
  Q_OBJECT
public:
  explicit CommentsPanel(QWidget* parent = nullptr);

  void setDocument(std::shared_ptr<pdfz::Document> doc);  // lists it from scratch
  void refreshPage(int page);                              // after a comment on it changed
  // Marks the entry for that annotation, without signalling.
  void showAnnotation(int page, int index);

  QSize sizeHint() const override { return {300, 400}; }

signals:
  void activated(int page, int index);
  void editRequested(int page, int index);
  void deleteRequested(int page, int index);

private:
  void scanSome();
  void fillPage(int page);
  QTreeWidgetItem* pageItem(int page, bool create);
  void updateSummary();
  bool itemRef(QTreeWidgetItem* item, int& page, int& index) const;

  std::shared_ptr<pdfz::Document> doc_;
  QTreeWidget* tree_;
  QLabel* summary_;
  QTimer scan_;
  int nextPage_ = 0;
  bool syncing_ = false;
};
