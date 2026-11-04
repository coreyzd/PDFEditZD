// SPDX-License-Identifier: GPL-3.0-only
// Undo and redo for document edits, as whole-document snapshots.
//
// Before an edit, the document is snapshotted (the bytes of a PDF: a fresh save, or the bytes it
// was opened from while still unedited). Undo opens the previous snapshot as a new Document.
// Whole snapshots rather than inverse operations, because PDFium can't put a deleted page back:
// it could only import a copy, and bookmarks, links and form fields that pointed at the original
// page would silently break. A snapshot brings everything back exactly as it was.
#pragma once

#include "core/document.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace pdfz {

class EditHistory {
public:
  // Snapshots are dropped, oldest first, while they add up to more than this (the newest undo
  // step is always kept).
  explicit EditHistory(std::size_t maxBytes = std::size_t(768) * 1024 * 1024);

  // What an edit needs to be undoable: the document as it was just before.
  struct Snapshot {
    Bytes bytes;
    int page = 0;  // the page to show when coming back to this state
  };
  // Takes the snapshot; call it before changing the document. Null bytes if saving failed.
  static Snapshot capture(const Document& doc, int page);
  // The edit happened: `before` (from capture()) becomes the undo step called `label`
  // ("Delete Pages"). Anything that could be redone is gone.
  void push(Snapshot before, std::string label);

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  const std::string& undoLabel() const;  // empty if none
  const std::string& redoLabel() const;

  struct Restored {
    std::unique_ptr<Document> document;  // null if the snapshot couldn't be opened
    int page = 0;
  };
  // `current` is the document as it is now, which becomes the redo (or undo) step; `page` is
  // the page showing now. Returns the document to switch to. On failure nothing changes.
  Restored undo(const Document& current, int page);
  Restored redo(const Document& current, int page);

  void clear();
  // The document now matches the file on disk (just opened, or saved).
  void markClean() { cleanPosition_ = position_; }
  // True while the document differs from the file on disk, through any undo and redo.
  bool isModified() const { return position_ != cleanPosition_; }

  std::size_t bytesUsed() const;
  std::size_t undoCount() const { return undo_.size(); }
  std::size_t redoCount() const { return redo_.size(); }

private:
  struct Step {
    Snapshot state;
    std::string label;
  };
  Restored move(std::deque<Step>& from, std::deque<Step>& to, const Document& current, int page, int direction);
  void trim();

  std::deque<Step> undo_;  // back = most recent
  std::deque<Step> redo_;  // back = next to redo
  std::size_t maxBytes_;
  // Edits applied since the history began (undo -1, redo +1), and that count when the document
  // last matched its file; -1 once that state can no longer be reached.
  long long position_ = 0;
  long long cleanPosition_ = 0;
};

}  // namespace pdfz
