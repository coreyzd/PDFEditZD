// SPDX-License-Identifier: GPL-3.0-only
// One copy of the program at a time (per user): started again with files (a PDF
// double-clicked in Explorer, or several opened together, which Windows starts the program
// for once each, at the same moment), it hands them to the copy already running, which opens
// them in tabs, and quits.
#pragma once

#include <QObject>
#include <QStringList>

#include <memory>

class QLocalServer;
class QLockFile;

class SingleInstance : public QObject {
  Q_OBJECT
public:
  explicit SingleInstance(QObject* parent = nullptr);
  ~SingleInstance() override;

  // True if a running copy took `files` (absolute paths; may be none: it just comes to the
  // front): this one should quit. False if this is the copy that runs (it listens from now
  // on), or if the running one can't be reached (this one runs too, on its own).
  bool handOff(const QStringList& files);

signals:
  void filesReceived(const QStringList& files);

private:
  void listen();

  QString name_;
  std::unique_ptr<QLockFile> lock_;
  QLocalServer* server_ = nullptr;
};
