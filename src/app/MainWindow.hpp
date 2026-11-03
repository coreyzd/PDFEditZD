// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DocumentView.hpp"

#include <QMainWindow>

#include <memory>

namespace pdfz { class Document; }
class PageRenderer;
class ThumbnailPanel;
class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QSpinBox;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  bool openFile(const QString& path);

protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

private:
  void createActions();
  void createMenus();
  void createToolBar();
  void setDocument(std::shared_ptr<pdfz::Document> doc, const QString& path);
  void updateUi();
  void updateRecentMenu();
  void addRecent(const QString& path);
  // Opens a PDF, asking for its password if it needs one. Shows its own error messages.
  std::unique_ptr<pdfz::Document> loadWithPrompt(const QString& path);

  // Asks to save unsaved changes. False if the user cancelled.
  bool maybeSave();
  bool save();
  bool saveAs();
  bool saveTo(const QString& path);
  void closeDocument();
  void print();

  // After an edit to the pages: redraw everything, then show `pageToShow`.
  void pagesChanged(int pageToShow);
  void deletePages();
  void insertBlankPage();
  void insertPagesFromFile();
  void rotatePages(int degrees);

  void onCurrentPageChanged(int page);
  void onZoomChanged(double zoom, DocumentView::FitMode mode);
  void applyZoomText(const QString& text);

  PageRenderer* renderer_;
  DocumentView* view_;
  ThumbnailPanel* thumbs_;
  QDockWidget* thumbsDock_;
  std::shared_ptr<pdfz::Document> doc_;
  QString path_;

  QAction* openAct_;
  QAction* saveAct_;
  QAction* saveAsAct_;
  QAction* printAct_;
  QAction* closeAct_;
  QAction* quitAct_;
  QAction* deletePagesAct_;
  QAction* insertBlankAct_;
  QAction* insertFileAct_;
  QAction* rotateCwAct_;
  QAction* rotateCcwAct_;
  QAction* zoomInAct_;
  QAction* zoomOutAct_;
  QAction* actualSizeAct_;
  QAction* fitPageAct_;
  QAction* fitWidthAct_;
  QAction* prevPageAct_;
  QAction* nextPageAct_;
  QAction* aboutAct_;
  QMenu* recentMenu_;
  QMenu* pagesMenu_;

  QSpinBox* pageSpin_;
  QLabel* pageCountLabel_;
  QComboBox* zoomCombo_;
  QLabel* pageSizeLabel_;
};
