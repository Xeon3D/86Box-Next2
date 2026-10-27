/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Serial/Parallel ports configuration UI module.
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *          Cacodemon345
 *
 *          Copyright 2022 Cacodemon345
 *          Copyright 2022 Jasmine Iwanek
 *          Copyright 2021 Joakim L. Gilje
 */
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/lpt.h>
#include <86box/scsi_device.h>
#include <86box/scsi_tape.h>
#include <86box/serial.h>
#include <86box/char.h>
}

#include "qt_deviceconfig.hpp"
#include "qt_models_common.hpp"

#include "qt_defs.hpp"

#include "qt_settings_completer.hpp"
#include "qt_settings_bus_tracking.hpp"
#include "qt_harddrive_common.hpp"

#include "qt_settingsports.hpp"
#include "ui_qt_settingsports.h"
#include "qt_settings.hpp"
#include "qt_usb_manager.hpp"
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <QStandardItem>

SettingsPorts::SettingsPorts(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsPorts)
{
    ui->setupUi(this);

    for (int i = 0; i < PARALLEL_MAX; i++) {
        scLpt[i]                  = new SettingsCompleter(findChild<QComboBox *>(QString("comboBoxLpt%1").arg(i + 1)), nullptr);
        lpt_device_cfg_changed[i] = 0;
    }

    for (int i = 0; i < SERIAL_MAX_UI; i++) {
        scCom[i]                  = new SettingsCompleter(findChild<QComboBox *>(QString("comboBoxCom%1").arg(i + 1)), nullptr);
        com_device_cfg_changed[i] = 0;
    }

    buildUsbTab();
    onCurrentMachineChanged(machine);
}

SettingsPorts::~SettingsPorts()
{
    for (int i = 0; i < PARALLEL_MAX; i++)
        delete scLpt[i];
    for (int i = 0; i < SERIAL_MAX_UI; i++)
        delete scCom[i];

    delete ui;
}

void
SettingsPorts::updateLptPortTracking(int i)
{
    auto *comboBox = findChild<QComboBox *>(QString("comboBoxLpt%1").arg(i + 1));
    if (comboBox == NULL)
        return;

    int  device   = comboBox->currentData().toInt();
    bool occupied = (lpt_ports[i].enabled > 0) && (device > 0) &&
                    (char_get_device(device) != &lpt_ditto_device);

    Harddrives::busTrackClass->device_track(occupied ? 1 : 0, DEV_LPT, TAPE_BUS_LPT, i);
}

int
SettingsPorts::changed()
{
    int has_changed  = 0;
    int soft_changed = 0;

    has_changed |= (jumpered_internal_ecp_dma != ui->comboBoxLptECPDMA->currentData().toInt());

    for (int i = 0; i < PARALLEL_MAX; i++) {
        auto *checkBox = findChild<QCheckBox *>(QString("checkBoxParallel%1").arg(i + 1));
        if (checkBox != NULL)
            has_changed |= (lpt_ports[i].enabled != (checkBox->isChecked() ? 1 : 0));

        int device_changed = lpt_device_cfg_changed[i];
        auto *comboBox = findChild<QComboBox *>(QString("comboBoxLpt%1").arg(i + 1));
        if (comboBox != NULL) {
            int device = comboBox->currentData().toInt();
            device_changed |= (lpt_ports[i].device != device);
            if ((lpt_ports[i].hotunplug >= CHAR_PORT_NOHOTUNPLUG) || !(char_get_device(device)->flags & DEVICE_HOTPLUG_IN))
                has_changed |= device_changed;
            else
                soft_changed |= device_changed;
        } else {
            has_changed |= device_changed;
        }
    }

    for (int i = 0; i < SERIAL_MAX_UI; i++) {
        auto *checkBox = findChild<QCheckBox *>(QString("checkBoxSerial%1").arg(i + 1));
        if (checkBox != NULL)
            has_changed |= (com_ports[i].enabled != (checkBox->isChecked() ? 1 : 0));

        int device_changed = com_device_cfg_changed[i];
    	auto *comboBox = findChild<QComboBox *>(QString("comboBoxCom%1").arg(i + 1));
        if (comboBox != NULL) {
            int device = comboBox->currentData().toInt();
            device_changed |= (com_ports[i].device != device);
            if ((com_ports[i].hotunplug >= CHAR_PORT_NOHOTUNPLUG) || !(char_get_device(device)->flags & DEVICE_HOTPLUG_IN))
                has_changed |= device_changed;
            else
                soft_changed |= device_changed;
        } else {
            has_changed |= device_changed;
        }
    }

    /* What is on the USB ports: plugged in at the hard reset. */
    for (int p = 0; p < USBN_PORTS; p++)
        has_changed |= (usbCfg[p] != QString(usbn_port_cfg[p]));

    return has_changed ? (SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET) :
                         (soft_changed ? SETTINGS_CHANGED : 0);
}

void
SettingsPorts::restore()
{
}

void
SettingsPorts::save(int soft)
{
    for (int i = 0; i < PARALLEL_MAX; i++) {
        auto *cbox     = findChild<QComboBox *>(QString("comboBoxLpt%1").arg(i + 1));
        auto *checkBox = findChild<QCheckBox *>(QString("checkBoxParallel%1").arg(i + 1));
        if (cbox != NULL)
            lpt_ports[i].device = cbox->currentData().toInt();
        if (checkBox != NULL)
            lpt_ports[i].enabled = checkBox->isChecked() ? 1 : 0;
    }

    for (int i = 0; i < SERIAL_MAX_UI; i++) {
        auto *cbox     = findChild<QComboBox *>(QString("comboBoxCom%1").arg(i + 1));
        auto *checkBox = findChild<QCheckBox *>(QString("checkBoxSerial%1").arg(i + 1));
        if (cbox != NULL)
            com_ports[i].device = cbox->currentData().toInt();
        if (checkBox != NULL)
            com_ports[i].enabled = checkBox->isChecked() ? 1 : 0;
    }

    for (int p = 0; p < USBN_PORTS; p++)
        snprintf(usbn_port_cfg[p], sizeof(usbn_port_cfg[p]), "%s", usbCfg[p].toUtf8().constData());
    usbn_layout = 2;   /* assignments by port name from now on */

    if (soft)
        return;

    jumpered_internal_ecp_dma = ui->comboBoxLptECPDMA->currentData().toInt();
}

/* ------------------------------------------------------------------ USB --- */

/* 86Box-Next: the USB tab -- a row for every port of the USB controllers the
   machine will have (the motherboard's, the card's) and of the hub, each with
   what is plugged into it: the USB keyboard, the USB mouse, the hub (on a
   controller's port), or a host device connected from the USB menu. */
void
SettingsPorts::buildUsbTab()
{
    auto *tab    = new QWidget();
    auto *layout = new QVBoxLayout(tab);
    auto *intro  = new QLabel(tr("What is plugged into each USB port when the machine starts. The USB keyboard and mouse are chosen on the Input devices page, the USB card and hub on Other peripherals; host devices are connected from the USB menu while the machine runs."));
    auto *grid   = new QGridLayout();

    intro->setWordWrap(true);
    layout->addWidget(intro);
    usbNone = new QLabel(tr("This machine has no USB controller: choose a motherboard with USB, or fit a USB card on Other peripherals."));
    usbNone->setWordWrap(true);
    layout->addWidget(usbNone);
    for (int p = 0; p < USBN_PORTS; p++) {
        usbCfg[p]   = QString(usbn_port_cfg[p]);
        usbLabel[p] = new QLabel(UsbManager::portLabel(p) + ":");
        usbCombo[p] = new QComboBox();
        usbCombo[p]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        grid->addWidget(usbLabel[p], p, 0);
        grid->addWidget(usbCombo[p], p, 1);
        connect(usbCombo[p], qOverload<int>(&QComboBox::currentIndexChanged), this, [this, p](int) { usbChoiceChanged(p); });
    }
    layout->addLayout(grid);
    layout->addStretch(1);
    ui->tabWidgetPorts->addTab(tab, tr("USB"));
}

QString
SettingsPorts::usbAssignment(int slot) const
{
    return ((slot >= 0) && (slot < USBN_PORTS)) ? usbCfg[slot] : QString();
}

/* The rows the machine has now, and on each the choices it can take. */
void
SettingsPorts::refreshUsb()
{
    Settings *s = Settings::settings;

    if (usbCombo[0] == nullptr)
        return;

    const int  card     = s ? s->currentUsbCard() : usb_card_type;
    const int  hubType  = s ? s->currentUsbHub() : usbn_hub_type;
    const bool keyboard = s ? s->currentUsbKeyboard() : false;
    const bool mouse    = s ? s->currentUsbMouse() : false;
    bool       any      = false;

    /* A hub just chosen goes on the first free controller port. */
    if (hubType > 0) {
        bool placed = false;
        for (int p = 0; p < USBN_HUB_BASE; p++)
            placed |= UsbManager::portExistsFor(machineId, card, hubType, p) && (usbCfg[p] == "hub");
        for (int p = 0; (p < USBN_HUB_BASE) && !placed; p++) {
            if (UsbManager::portExistsFor(machineId, card, hubType, p) && usbCfg[p].isEmpty()) {
                usbCfg[p] = "hub";
                placed    = true;
            }
        }
    }

    usbFilling = true;
    for (int p = 0; p < USBN_PORTS; p++) {
        const bool exists = UsbManager::portExistsFor(machineId, card, hubType, p);

        usbLabel[p]->setVisible(exists);
        usbCombo[p]->setVisible(exists);
        if (!exists)
            continue;
        any = true;

        QComboBox *c = usbCombo[p];
        c->clear();
        c->addItem(tr("None"), QString());
        if (keyboard)
            c->addItem(tr("USB keyboard"), QString("keyboard"));
        if (mouse)
            c->addItem(tr("USB mouse"), QString("mouse"));
        if ((hubType > 0) && (usbn_port_role(p) >= 0))
            c->addItem(tr("USB hub"), QString("hub"));
        unsigned vid, pid;
        if (sscanf(usbCfg[p].toUtf8().constData(), "%x:%x", &vid, &pid) == 2)
            c->addItem(tr("Host device %1 (from the USB menu)").arg(usbCfg[p]), usbCfg[p]);
        const int i = c->findData(usbCfg[p]);
        c->setCurrentIndex((i >= 0) ? i : 0);
        if (i < 0)
            usbCfg[p].clear();   /* no longer possible here (that device is not chosen) */
    }
    usbNone->setVisible(!any);
    usbFilling = false;
}

/* One port's choice: the keyboard, mouse and hub are on one port each, so
   choosing one here takes it off any other. */
void
SettingsPorts::usbChoiceChanged(int slot)
{
    if (usbFilling)
        return;

    const QString what = usbCombo[slot]->currentData().toString();

    usbCfg[slot] = what;
    if ((what == "keyboard") || (what == "mouse") || (what == "hub")) {
        for (int p = 0; p < USBN_PORTS; p++) {
            if ((p != slot) && (usbCfg[p] == what))
                usbCfg[p].clear();
        }
    }
    if (Settings::settings)
        Settings::settings->usbStateChanged();
    else
        refreshUsb();
}

/* The character devices, for the parallel and serial port lists alike. */
static const QVector<Models::Device> &
charDevices()
{
    return Models::Devices(
        char_get_device,
        [](int c) -> const char * {
            const device_t *dev = char_get_device(c);
            return (dev != nullptr) ? dev->internal_name : nullptr;
        },
        [](int c) -> int {
            const device_t *dev = char_get_device(c);
            return (dev != nullptr) && device_available(dev);
        },
        -1);
}

void
SettingsPorts::onCurrentMachineChanged(int machineId)
{
    this->machineId = machineId;

    int c = 0;

    auto *lptEcpDmaModel   = ui->comboBoxLptECPDMA->model();
    auto  removeRowsEcpDma = lptEcpDmaModel->rowCount();

    int has_jumpers = !!machine_has_jumpered_ecp_dma(machineId, DMA_ANY);

    int selectedRow = -2;
    int first       = -2;

    Models::Batch ecpDmaRows(lptEcpDmaModel);
    for (int i = 0; i < 9; ++i) {
        int j = machine_map_jumpered_ecp_dma(i);

        if ((has_jumpers && ((j == DMA_NONE) || !machine_has_jumpered_ecp_dma(machineId, j))) || (!has_jumpers && (j != DMA_NONE)))
            continue;

        if (first == -2)
            first = j;

        QString name = tr(machine_get_jumpered_ecp_dma_name(i));
        int     row  = ecpDmaRows.add(name, j);

        if (j == jumpered_internal_ecp_dma)
            selectedRow = row - removeRowsEcpDma;

        c++;
    }

    ecpDmaRows.commit();

    if (selectedRow == -2)
        selectedRow = first;

    lptEcpDmaModel->removeRows(0, removeRowsEcpDma);
    ui->comboBoxLptECPDMA->setCurrentIndex(-1);
    ui->comboBoxLptECPDMA->setCurrentIndex(selectedRow);

    if ((c == 1) || !has_jumpers)
        ui->comboBoxLptECPDMA->setEnabled(false);
    else
        ui->comboBoxLptECPDMA->setEnabled(true);

    c = 0;

    // LPT Device
#define CBOX_MAX MAX(PARALLEL_MAX, SERIAL_MAX_UI)
    QComboBox          *cbox[CBOX_MAX]         = { 0 };
    QAbstractItemModel *models[CBOX_MAX]       = { 0 };
    int                 removeRows_[CBOX_MAX]  = { 0 };
    int                 selectedRows[CBOX_MAX] = { 0 };

    for (uint8_t i = 0; i < PARALLEL_MAX; ++i) {
        scLpt[i]->removeRows();
        cbox[i]        = findChild<QComboBox *>(QString("comboBoxLpt%1").arg(i + 1));
        models[i]      = cbox[i]->model();
        removeRows_[i] = models[i]->rowCount();
    }

    const auto &chars = charDevices();

    std::vector<Models::Batch> lptRows(models, models + PARALLEL_MAX);
    for (const auto &device : chars) {
        if ((device.dev->flags & DEVICE_LPT) && device.available && device_is_valid(device.dev, machineId)) {
            for (uint8_t i = 0; i < PARALLEL_MAX; ++i) {
                int row = lptRows[i].add(device.name, device.id);
                scLpt[i]->addDevice(nullptr, device.name);

                if (device.id == lpt_ports[i].device)
                    selectedRows[i] = row - removeRows_[i];
            }
        }
    }

    for (uint8_t i = 0; i < PARALLEL_MAX; ++i) {
        lptRows[i].commit();
        models[i]->removeRows(0, removeRows_[i]);
        cbox[i]->setEnabled(models[i]->rowCount() > 1);
        cbox[i]->setCurrentIndex(-1);
        cbox[i]->setCurrentIndex(selectedRows[i]);

        auto *checkBox  = findChild<QCheckBox *>(QString("checkBoxParallel%1").arg(i + 1));
        auto *buttonCfg = findChild<QPushButton *>(QString("pushButtonConfigureLpt%1").arg(i + 1));
        if (checkBox != NULL)
            checkBox->setChecked(lpt_ports[i].enabled > 0);
        if (cbox[i] != NULL) {
            cbox[i]->setEnabled(lpt_ports[i].enabled > 0);
            if (buttonCfg != NULL) {
                int lptDevice = cbox[i]->currentData().toInt();
                buttonCfg->setEnabled(device_has_config(char_get_device(lptDevice)) && (lpt_ports[i].enabled > 0));
            }
        }

        updateLptPortTracking(i);
    }

    c = 0;

    // COM Device
    for (uint8_t i = 0; i < SERIAL_MAX_UI; ++i) {
        scCom[i]->removeRows();
        cbox[i]        = findChild<QComboBox *>(QString("comboBoxCom%1").arg(i + 1));
        models[i]      = cbox[i]->model();
        removeRows_[i] = models[i]->rowCount();
        selectedRows[i] = 0;
    }

    std::vector<Models::Batch> comRows(models, models + SERIAL_MAX_UI);
    for (const auto &device : chars) {
        if ((device.dev->flags & DEVICE_COM) && device.available && device_is_valid(device.dev, machineId)) {
            for (uint8_t i = 0; i < SERIAL_MAX_UI; ++i) {
                int row = comRows[i].add(device.name, device.id);
                scCom[i]->addDevice(nullptr, device.name);

                if (device.id == com_ports[i].device)
                    selectedRows[i] = row - removeRows_[i];
            }
        }
    }

    for (int i = 0; i < SERIAL_MAX_UI; i++) {
        comRows[i].commit();
        models[i]->removeRows(0, removeRows_[i]);
        cbox[i]->setEnabled(models[i]->rowCount() > 1);
        cbox[i]->setCurrentIndex(-1);
        cbox[i]->setCurrentIndex(selectedRows[i]);

        auto *checkBox  = findChild<QCheckBox *>(QString("checkBoxSerial%1").arg(i + 1));
        auto *buttonCfg = findChild<QPushButton *>(QString("pushButtonConfigureCom%1").arg(i + 1));
        if (checkBox != NULL)
            checkBox->setChecked(com_ports[i].enabled > 0);
        if (cbox[i] != NULL) {
            cbox[i]->setEnabled(com_ports[i].enabled > 0);
            if (buttonCfg != NULL) {
                int comDevice = cbox[i]->currentData().toInt();
                buttonCfg->setEnabled(device_has_config(char_get_device(comDevice)) && (com_ports[i].enabled > 0));
            }
        }
    }

    refreshUsb();   /* the USB ports this machine has */
}

void
SettingsPorts::on_comboBoxLpt1_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int lptDevice = ui->comboBoxLpt1->currentData().toInt();

    updateLptPortTracking(0);
    ui->pushButtonConfigureLpt1->setEnabled(ui->comboBoxLpt1->isEnabled() && device_has_config(char_get_device(lptDevice)));
}

void
SettingsPorts::on_pushButtonConfigureLpt1_clicked()
{
    int   lptDevice = ui->comboBoxLpt1->currentData().toInt();
    auto *device    = char_get_device(lptDevice);

    lpt_device_cfg_changed[0] = DeviceConfig::ConfigureDevice(device, 1);
}

void
SettingsPorts::on_comboBoxLpt2_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int lptDevice = ui->comboBoxLpt2->currentData().toInt();

    updateLptPortTracking(1);
    ui->pushButtonConfigureLpt2->setEnabled(ui->comboBoxLpt2->isEnabled() && device_has_config(char_get_device(lptDevice)));
}

void
SettingsPorts::on_pushButtonConfigureLpt2_clicked()
{
    int   lptDevice = ui->comboBoxLpt2->currentData().toInt();
    auto *device    = char_get_device(lptDevice);

    lpt_device_cfg_changed[1] = DeviceConfig::ConfigureDevice(device, 2);
}

void
SettingsPorts::on_comboBoxLpt3_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int lptDevice = ui->comboBoxLpt3->currentData().toInt();

    updateLptPortTracking(2);
    ui->pushButtonConfigureLpt3->setEnabled(ui->comboBoxLpt3->isEnabled() && device_has_config(char_get_device(lptDevice)));
}

void
SettingsPorts::on_pushButtonConfigureLpt3_clicked()
{
    int   lptDevice = ui->comboBoxLpt3->currentData().toInt();
    auto *device    = char_get_device(lptDevice);

    lpt_device_cfg_changed[2] = DeviceConfig::ConfigureDevice(device, 3);
}

void
SettingsPorts::on_comboBoxLpt4_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int lptDevice = ui->comboBoxLpt4->currentData().toInt();

    updateLptPortTracking(3);
    ui->pushButtonConfigureLpt4->setEnabled(ui->comboBoxLpt4->isEnabled() && device_has_config(char_get_device(lptDevice)));
}

void
SettingsPorts::on_pushButtonConfigureLpt4_clicked()
{
    int   lptDevice = ui->comboBoxLpt4->currentData().toInt();
    auto *device    = char_get_device(lptDevice);

    lpt_device_cfg_changed[3] = DeviceConfig::ConfigureDevice(device, 4);
}

void
SettingsPorts::on_checkBoxParallel1_stateChanged(int state)
{
    ui->comboBoxLpt1->setEnabled(state == Qt::Checked);
    on_comboBoxLpt1_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxParallel2_stateChanged(int state)
{
    ui->comboBoxLpt2->setEnabled(state == Qt::Checked);
    on_comboBoxLpt2_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxParallel3_stateChanged(int state)
{
    ui->comboBoxLpt3->setEnabled(state == Qt::Checked);
    on_comboBoxLpt3_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxParallel4_stateChanged(int state)
{
    ui->comboBoxLpt4->setEnabled(state == Qt::Checked);
    on_comboBoxLpt4_currentIndexChanged(0);
}

void
SettingsPorts::on_comboBoxCom1_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int comDevice = ui->comboBoxCom1->currentData().toInt();

    ui->pushButtonConfigureCom1->setEnabled(ui->comboBoxCom1->isEnabled() && device_has_config(char_get_device(comDevice)));
}

void
SettingsPorts::on_pushButtonConfigureCom1_clicked()
{
    int   comDevice = ui->comboBoxCom1->currentData().toInt();
    auto *device    = char_get_device(comDevice);

    com_device_cfg_changed[0] = DeviceConfig::ConfigureDevice(device, 1);
}

void
SettingsPorts::on_comboBoxCom2_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int comDevice = ui->comboBoxCom2->currentData().toInt();

    ui->pushButtonConfigureCom2->setEnabled(ui->comboBoxCom2->isEnabled() && device_has_config(char_get_device(comDevice)));
}

void
SettingsPorts::on_pushButtonConfigureCom2_clicked()
{
    int   comDevice = ui->comboBoxCom2->currentData().toInt();
    auto *device    = char_get_device(comDevice);

    com_device_cfg_changed[1] = DeviceConfig::ConfigureDevice(device, 2);
}

void
SettingsPorts::on_comboBoxCom3_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int comDevice = ui->comboBoxCom3->currentData().toInt();

    ui->pushButtonConfigureCom3->setEnabled(ui->comboBoxCom3->isEnabled() && device_has_config(char_get_device(comDevice)));
}

void
SettingsPorts::on_pushButtonConfigureCom3_clicked()
{
    int   comDevice = ui->comboBoxCom3->currentData().toInt();
    auto *device    = char_get_device(comDevice);

    com_device_cfg_changed[2] = DeviceConfig::ConfigureDevice(device, 3);
}

void
SettingsPorts::on_comboBoxCom4_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    int comDevice = ui->comboBoxCom4->currentData().toInt();

    ui->pushButtonConfigureCom4->setEnabled(ui->comboBoxCom4->isEnabled() && device_has_config(char_get_device(comDevice)));
}

void
SettingsPorts::on_pushButtonConfigureCom4_clicked()
{
    int   comDevice = ui->comboBoxCom4->currentData().toInt();
    auto *device    = char_get_device(comDevice);

    com_device_cfg_changed[3] = DeviceConfig::ConfigureDevice(device, 4);
}

void
SettingsPorts::on_checkBoxSerial1_stateChanged(int state)
{
    ui->comboBoxCom1->setEnabled(state == Qt::Checked);
    on_comboBoxCom1_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxSerial2_stateChanged(int state)
{
    ui->comboBoxCom2->setEnabled(state == Qt::Checked);
    on_comboBoxCom2_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxSerial3_stateChanged(int state)
{
    ui->comboBoxCom3->setEnabled(state == Qt::Checked);
    on_comboBoxCom3_currentIndexChanged(0);
}

void
SettingsPorts::on_checkBoxSerial4_stateChanged(int state)
{
    ui->comboBoxCom4->setEnabled(state == Qt::Checked);
    on_comboBoxCom4_currentIndexChanged(0);
}
