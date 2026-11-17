// SPDX-License-Identifier: GPL-3.0-only
// The side panel's other views, beside the page thumbnails (as Acrobat has them): the
// document's bookmarks, its attached files and its layers. One of each per window, showing the
// current tab's document.
#pragma once

#include <QWidget>

#include <memory>

namespace pdfz { class Document; }
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class BookmarksPanel : public QWidget {
  Q_OBJECT
public:
  explicit BookmarksPanel(QWidget* parent = nullptr);
  // Read again (a different document, or its pages changed).
  void setDocument(std::shared_ptr<pdfz::Document> doc);

signals:
  void goTo(int page, double top);  // top: display points from the page's top; -1 for its top

private:
  QTreeWidget* tree_;
  QLabel* none_;
};

class AttachmentsPanel : public QWidget {
  Q_OBJECT
public:
  explicit AttachmentsPanel(QWidget* parent = nullptr);
  void setDocument(std::shared_ptr<pdfz::Document> doc);

signals:
  // Adding and removing are edits: the window does them on the tab, so they can be undone.
  void addRequested();
  void removeRequested(int index);

private:
  void openSelected();
  void saveSelected();
  int selected() const;  // the selected attachment's index, or -1
  void updateButtons();

  std::shared_ptr<pdfz::Document> doc_;
  QTreeWidget* tree_;
  QToolButton* openButton_;
  QToolButton* saveButton_;
  QToolButton* addButton_;
  QToolButton* removeButton_;
};

class LayersPanel : public QWidget {
  Q_OBJECT
public:
  explicit LayersPanel(QWidget* parent = nullptr);
  void setDocument(std::shared_ptr<pdfz::Document> doc);

signals:
  void layersChanged();  // one was turned on or off: the pages look different

private:
  std::shared_ptr<pdfz::Document> doc_;
  QTreeWidget* tree_;
  QLabel* none_;
  bool filling_ = false;
};
