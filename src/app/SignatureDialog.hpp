// SPDX-License-Identifier: GPL-3.0-only
// Making a signature (or initials): draw it with the mouse or a pen, type it in a handwriting
// font, or use a picture of it.
#pragma once

#include "Signatures.hpp"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QTabWidget;
class SignaturePad;

class SignatureDialog : public QDialog {
  Q_OBJECT
public:
  explicit SignatureDialog(bool initials, QWidget* parent = nullptr);
  Signature signature() const;
  bool saveForLater() const;

private:
  void updateOk();
  void updateTyped();
  void chooseImage();
  void updateImage();

  bool initials_;
  QTabWidget* tabs_;
  SignaturePad* pad_;
  QLineEdit* typed_;
  QListWidget* fonts_;
  QLabel* imagePreview_;
  QSlider* threshold_;
  QImage sourceImage_;
  QImage cleanImage_;
  QButtonGroup* colors_;
  QCheckBox* save_;
  QPushButton* ok_;
};
