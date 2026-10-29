/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             TouchMaster key generator: programs the Midway Dolphin I/O
 *             board's U9 chip and Abyss key as the game's own factory flow
 *             (diag init, then ProgramAbyss) would, for a serial number,
 *             country and key version, so the machine boots straight to its
 *             menus without the factory screens.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_dolphin_keydialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

extern "C" {
#include <86box/86box.h>
#include <86box/dolphin_keygen.h>
#include <86box/dolphin_io.h>
}

DolphinKeyDialog::DolphinKeyDialog(QWidget *parent)
    : QDialog(parent)
{
    uint64_t serial  = 0;
    unsigned country = 1;
    bool     current = (dolphin_io_read_keys(&serial, &country) == 0);

    setWindowTitle(tr("TouchMaster Key Generator"));

    auto *layout = new QVBoxLayout(this);
    auto *intro  = new QLabel(tr("Programs the Dolphin board's U9 chip and the Abyss key as the game's own factory "
                                 "setup would, so it accepts them and boots straight to its menus."),
                              this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    currentLabel = new QLabel(this);
    if (current) {
        const char *name = dolphin_country_name((int) country);
        currentLabel->setText(tr("Fitted now: serial %1, %2 (%3).")
                                  .arg(QString::number(serial), name ? QString::fromUtf8(name) : tr("unknown country"))
                                  .arg(country));
    } else
        currentLabel->setText(tr("Fitted now: blank chips (the game would run its factory setup)."));
    currentLabel->setWordWrap(true);
    layout->addWidget(currentLabel);

    auto *form = new QFormLayout();

    /* Serial number: digits, at most the 14 the chip stores. */
    auto *serialRow = new QHBoxLayout();
    serialEdit      = new QLineEdit(this);
    serialEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,14}")), serialEdit));
    auto *generate = new QPushButton(tr("&Generate"), this);
    serialRow->addWidget(serialEdit, 1);
    serialRow->addWidget(generate);
    form->addRow(tr("&Serial number:"), serialRow);
    serialHint = new QLabel(this);
    serialHint->setWordWrap(true);
    form->addRow(QString(), serialHint);

    /* Country: the game's own list, by name. */
    countryBox = new QComboBox(this);
    std::vector<const dolphin_country_t *> list;
    for (const dolphin_country_t *c = dolphin_countries; c->name; c++)
        list.push_back(c);
    std::sort(list.begin(), list.end(),
              [](const dolphin_country_t *a, const dolphin_country_t *b) { return QString::fromUtf8(a->name) < QString::fromUtf8(b->name); });
    for (const dolphin_country_t *c : list)
        countryBox->addItem(QStringLiteral("%1 (%2)").arg(QString::fromUtf8(c->name)).arg(c->code), c->code);
    countryBox->setCurrentIndex(std::max(0, countryBox->findData((int) country)));
    form->addRow(tr("&Country:"), countryBox);

    /* Key version: 2.03 writes 1.0 and ignores it, 3.0x writes 3.0. */
    versionBox = new QComboBox(this);
    versionBox->addItem(tr("3.0 (TouchMaster 3.0x; accepted by 2.03 too)"), 0x30000);
    versionBox->addItem(tr("1.0 (as TouchMaster 2.03 writes it)"), 0x10000);
    form->addRow(tr("&Key version:"), versionBox);
    layout->addLayout(form);

    auto *note = new QLabel(tr("The game reads the chips when it starts. A country other than the one in the machine's "
                               "settings makes it restore its factory settings (operator settings are lost)."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *write   = buttons->addButton(tr("&Write Chips"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    connect(generate, &QPushButton::clicked, this, &DolphinKeyDialog::generateSerial);
    connect(serialEdit, &QLineEdit::textChanged, this, &DolphinKeyDialog::checkSerial);
    connect(write, &QPushButton::clicked, this, &DolphinKeyDialog::writeChips);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (current)
        serialEdit->setText(QString::number(serial));
    else
        generateSerial();
    checkSerial();
    setMinimumWidth(460);
}

/* As the factory keypad makes them: 6 (or 9), then 00, then 8 digits. */
void
DolphinKeyDialog::generateSerial()
{
    QString digits;

    for (int i = 0; i < 8; i++)
        digits += QChar('0' + QRandomGenerator::global()->bounded(10));
    serialEdit->setText(QStringLiteral("600") + digits);
}

void
DolphinKeyDialog::checkSerial()
{
    const QString s = serialEdit->text();

    if (s.isEmpty())
        serialHint->setText(tr("Enter a serial number."));
    else if (((s[0] == '6') || (s[0] == '9')) && (s.length() == 11))
        serialHint->setText(tr("A factory-style serial."));
    else
        serialHint->setText(tr("The game takes it, but TouchMaster 3.0x's factory keypad only makes 11-digit serials "
                               "starting with 6 or 9."));
}

void
DolphinKeyDialog::writeChips()
{
    const QString s       = serialEdit->text();
    const int     version = versionBox->currentData().toInt();
    int           ret;

    if (s.isEmpty())
        return;
    ret = dolphin_io_write_keys(s.toULongLong(), (unsigned) countryBox->currentData().toInt(), version >> 16, version & 0xffff);
    if (ret == -2) {
        QMessageBox::warning(this, windowTitle(), tr("There is no Midway Dolphin I/O board in this machine."));
        return;
    } else if (ret) {
        QMessageBox::warning(this, windowTitle(), tr("The chips could not be written (%1).").arg(ret));
        return;
    }
    if (QMessageBox::question(this, windowTitle(),
                              tr("The chips are written and saved. The game reads them when it starts: hard reset the "
                                 "machine now?"))
        == QMessageBox::Yes)
        pc_reset_hard();
    accept();
}
