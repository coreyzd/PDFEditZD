// SPDX-License-Identifier: GPL-3.0-only
#include "SingleInstance.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int kWaitMs = 10'000;  // for the first copy to start listening (a slow PC, many files at once)

// The message: the files, then a reply that they were taken.
QByteArray message(const QStringList& files) {
  QByteArray out;
  QDataStream s(&out, QIODevice::WriteOnly);
  s.setVersion(QDataStream::Qt_6_0);
  s << QStringLiteral("PDFEditZD-open") << files;
  return out;
}

bool sendTo(const QString& name, const QStringList& files) {
  QLocalSocket socket;
  socket.connectToServer(name);
  if (!socket.waitForConnected(500)) return false;
#ifdef Q_OS_WIN
  // Windows only lets the program the user just started bring a window to the front: it lets
  // the running copy bring its window forward.
  AllowSetForegroundWindow(ASFW_ANY);
#endif
  const QByteArray data = message(files);
  QByteArray framed;
  QDataStream f(&framed, QIODevice::WriteOnly);
  f << data;
  socket.write(framed);
  if (!socket.waitForBytesWritten(3000)) return false;
  // Taken: the running copy says so (and a copy that's busy, a dialog open say, still has
  // them queued: they open when it's free).
  if (!socket.waitForReadyRead(5000)) return socket.bytesToWrite() == 0;
  return socket.readAll().startsWith("ok");
}

}  // namespace

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {
  // Per user (named pipes on Windows are seen by every session).
  const QByteArray who = QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
  name_ = QStringLiteral("PDFEditZD-") + QString::fromLatin1(who);
}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::handOff(const QStringList& files) {
  // Whoever holds the lock is the running copy (or about to be: until it listens, the others
  // keep trying). A copy that crashed leaves a lock its process no longer holds, which goes.
  lock_ = std::make_unique<QLockFile>(QDir::temp().filePath(name_ + ".lock"));
  lock_->setStaleLockTime(0);
  QElapsedTimer waited;
  waited.start();
  while (waited.elapsed() < kWaitMs) {
    if (sendTo(name_, files)) return true;
    if (lock_->tryLock(0)) {
      listen();
      return false;
    }
    QThread::msleep(50);
  }
  return false;  // nobody answers: this copy runs on its own
}

void SingleInstance::listen() {
  QLocalServer::removeServer(name_);  // a socket file left behind (not on Windows)
  server_ = new QLocalServer(this);
  server_->setSocketOptions(QLocalServer::UserAccessOption);
  if (!server_->listen(name_)) return;
  connect(server_, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket* s = server_->nextPendingConnection()) {
      connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
      connect(s, &QLocalSocket::readyRead, this, [this, s] {
        QDataStream in(s);
        in.startTransaction();
        QByteArray data;
        in >> data;
        if (!in.commitTransaction()) return;  // not all of it yet
        QDataStream msg(data);
        msg.setVersion(QDataStream::Qt_6_0);
        QString tag;
        QStringList files;
        msg >> tag >> files;
        if (tag != QLatin1String("PDFEditZD-open")) {
          s->abort();
          return;
        }
        s->write("ok");
        s->flush();
        s->disconnectFromServer();
        emit filesReceived(files);
      });
    }
  });
}
