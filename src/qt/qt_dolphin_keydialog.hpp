/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             TouchMaster key generator: programs the Midway Dolphin I/O
 *             board's U9 chip and Abyss key for a serial number, country
 *             and key version.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_DOLPHIN_KEYDIALOG_HPP
#define QT_DOLPHIN_KEYDIALOG_HPP

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;

class DolphinKeyDialog : public QDialog {
    Q_OBJECT

public:
    explicit DolphinKeyDialog(QWidget *parent = nullptr);

private:
    QLineEdit *serialEdit;
    QComboBox *countryBox;
    QComboBox *versionBox;
    QLabel    *serialHint;
    QLabel    *currentLabel;

    void generateSerial();
    void checkSerial();
    void writeChips();
};

#endif
