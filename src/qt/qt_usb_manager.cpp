/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Host USB devices and the virtual machine.  See qt_usb_manager.hpp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_usb_manager.hpp"

#include <QAction>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QWidget>

extern "C" {
#include <86box/86box.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/machine.h>
}

#define POLL_MS 2000

UsbManager::UsbManager(QWidget *parent)
    : QObject(parent)
    , parentWidget(parent)
{
    menu = new QMenu(tr("&USB"), parent);
    connect(menu, &QMenu::aboutToShow, this, &UsbManager::buildMenu);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &UsbManager::poll);
    refresh();
}

QString
UsbManager::key(const usbn_host_info_t &d)
{
    QString path;
    for (int i = 0; i < d.port_depth; i++)
        path += QString(".%1").arg(d.port_path[i]);
    return QString("%1:%2@%3%4").arg(d.vid, 4, 16, QChar('0')).arg(d.pid, 4, 16, QChar('0')).arg(d.bus).arg(path);
}

QString
UsbManager::label(const usbn_host_info_t &d)
{
    return QString("%1 (%2:%3)").arg(QString::fromUtf8(d.desc),
                                     QString("%1").arg(d.vid, 4, 16, QChar('0')).toUpper(),
                                     QString("%1").arg(d.pid, 4, 16, QChar('0')).toUpper());
}

QString
UsbManager::rememberKey(uint16_t vid, uint16_t pid)
{
    return QString("%1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
}

/* Remembered choices: [USB] remember = vvvv:pppp=vm,vvvv:pppp=host,... */
static QString
remembered(const QString &id)
{
    const QString all = QString::fromUtf8(config_get_string((char *) "USB", (char *) "remember", (char *) ""));
    for (const QString &e : all.split(',', Qt::SkipEmptyParts)) {
        if (e.section('=', 0, 0) == id)
            return e.section('=', 1);
    }
    return QString();
}

static void
rememberChoice(const QString &id, const QString &choice)
{
    QStringList out;
    const QString all = QString::fromUtf8(config_get_string((char *) "USB", (char *) "remember", (char *) ""));
    for (const QString &e : all.split(',', Qt::SkipEmptyParts))
        if (e.section('=', 0, 0) != id)
            out << e;
    out << id + "=" + choice;
    config_set_string((char *) "USB", (char *) "remember", (char *) out.join(',').toUtf8().constData());
}

int
UsbManager::portOf(uint16_t vid, uint16_t pid) const
{
    const QString id = QString("%1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
    for (int p = 0; p < USBN_PORTS; p++)
        if (QString(usbn_port_cfg[p]).compare(id, Qt::CaseInsensitive) == 0)
            return p;
    return -1;
}

/* The ports and what the VM has in them. */
QString
UsbManager::toolTip() const
{
    QString tip = tr("USB");
    for (int p = 0; p < USBN_PORTS; p++) {
        char name[128];
        if (!usbn_port_exists(p))
            continue;   /* a hub's ports, without the hub */
        usbn_port_name(p, name, sizeof(name));
        tip += "\n" + tr("%1: %2").arg(portLabel(p), name[0] ? QString::fromUtf8(name) : tr("empty"));
    }
    return tip;
}

/* The ports of each controller, as the machine will have them: the
   southbridge's two (the ALi M1543's M5237: three connectors; the VIA 686A/B:
   a second controller with two more), the card's two. */
int
UsbManager::machinePorts(int machineId, const int *cards, int role)
{
    const machine_t *m = &machines[machineId];

    switch (role) {
        case USBN_ROLE_ONBOARD:
            if (!machineHasOnboardUsb(machineId))
                return 0;
            return ((m->chipset == MACHINE_CHIPSET_ALI_ALADDIN_IV_PLUS) || (m->chipset == MACHINE_CHIPSET_ALI_ALADDIN_V) ||
                    (m->chipset == MACHINE_CHIPSET_ALI_ALADDIN_PRO_II)) ? 3 : 2;
        case USBN_ROLE_ONBOARD2:
            if (!machineHasOnboardUsb(machineId))
                return 0;
            return ((m->init == machine_at_6via90ap_init) || (m->init == machine_at_cuv4xls_init) || (m->init == machine_at_ms6318_init) ||
                    (m->init == machine_at_m790mr_init) || (m->init == machine_at_cairo5_init) || (m->init == machine_at_ficva503a_init) ||
                    (m->init == machine_at_5emapro_init)) ? 2 : 0;
        default:
            /* A card: two ports, on a machine with PCI. */
            if ((role >= USBN_ROLE_CARD) && (role < USBN_ROLES))
                return ((cards[role - USBN_ROLE_CARD] > 0) && (machine_has_bus(machineId, MACHINE_BUS_PCI) > 0)) ? 2 : 0;
            return 0;
    }
}

bool
UsbManager::portExistsFor(int machineId, const int *cards, int hub, int slot)
{
    const int role = usbn_port_role(slot);

    if (role == -1) {
        int any = 0;
        for (int r = 0; r < USBN_ROLES; r++)
            any += machinePorts(machineId, cards, r);
        return (hub > 0) && (any > 0);
    }
    return (role >= 0) && ((slot % USBN_ROOT_PORTS) < machinePorts(machineId, cards, role));
}

/* A port by the controller it is on, as Settings > Ports > USB names it. */
QString
UsbManager::portLabel(int slot)
{
    const int n    = (usbn_port_role(slot) == -1) ? (slot - USBN_HUB_BASE + 1) : ((slot % USBN_ROOT_PORTS) + 1);
    const int role = usbn_port_role(slot);

    if (role > USBN_ROLE_CARD)
        return tr("USB card %1 port %2").arg(role - USBN_ROLE_CARD + 1).arg(n);
    switch (role) {
        case USBN_ROLE_ONBOARD:
            return tr("Motherboard port %1").arg(n);
        case USBN_ROLE_ONBOARD2:
            return tr("Motherboard (second controller) port %1").arg(n);
        case USBN_ROLE_CARD:
            return tr("USB card port %1").arg(n);
        case -1:
            return tr("Hub port %1").arg(n);
        default:
            return QString();
    }
}

bool
UsbManager::fitted()
{
    for (int i = 0; i < USBN_CARDS; i++) {
        if (usbn_card_get(i) > 0)
            return true;
    }
    return usbn_present();
}

bool
UsbManager::machineHasOnboardUsb(int machineId)
{
    /* Every board with these flags has a southbridge whose USB function is
       one of ours: the PIIX3/PIIX4's UHCI (usb_uhci.c), VIA's UHCIs and the
       ALi, SiS, SMSC and STPC OHCIs (usb_chipset.c).  The machine table's
       flags follow the southbridge (upstream's 2026-10-25 audit). */
    return machine_has_flags(machineId, MACHINE_USB) && machine_has_bus(machineId, MACHINE_BUS_USB);
}

void
UsbManager::refresh()
{
    const bool on = fitted();

    menu->menuAction()->setVisible(on);
    if (on && usbn_host_available()) {
        if (!timer->isActive()) {
            firstPoll = true;
            timer->start(POLL_MS);
            poll();
        }
    } else
        timer->stop();
}

/* What is plugged into the host now; what is new since last time is offered
   to the VM, and what the VM had and is gone from the host is unplugged. */
void
UsbManager::poll()
{
    if (prompting)
        return;

    usbn_host_info_t list[64];
    int              n = usbn_host_list(list, 64);
    QSet<QString>    now;
    QVector<usbn_host_info_t> arrived;

    devices.clear();
    for (int i = 0; i < n; i++) {
        devices.append(list[i]);
        const QString k = key(list[i]);
        now.insert(k);
        if (!firstPoll && !known.contains(k))
            arrived.append(list[i]);
    }

    for (int p = 0; p < USBN_PORTS; p++) {
        unsigned vid, pid;
        if (sscanf(usbn_port_cfg[p], "%x:%x", &vid, &pid) != 2)
            continue;
        bool present = false;
        for (const auto &d : devices)
            present |= (d.vid == vid) && (d.pid == pid);
        if (!present && !firstPoll) {
            usbn_detach(p);
            usbn_port_cfg[p][0] = '\0';
            config_save();
        }
    }

    known     = now;
    firstPoll = false;

    if (!usbn_present())
        return;
    for (const auto &d : arrived) {
        const QString choice = remembered(rememberKey(d.vid, d.pid));
        if (choice == "vm")
            connectToVm(d, true);
        else if ((choice != "host") && config_get_int((char *) "USB", (char *) "ask_on_plug", 1))
            ask(d);
    }
}

void
UsbManager::ask(const usbn_host_info_t &d)
{
    prompting = true;

    QMessageBox box(QMessageBox::Question, tr("USB device connected"),
                    tr("A USB device was plugged into the host:\n\n%1\n\nConnect it to the virtual machine, or keep it on the host? "
                       "While the virtual machine has it, the host cannot use it.").arg(label(d)),
                    QMessageBox::NoButton, parentWidget);
    auto *vm   = box.addButton(tr("Connect to the &virtual machine"), QMessageBox::AcceptRole);
    auto *host = box.addButton(tr("Keep on the &host"), QMessageBox::RejectRole);
    box.setDefaultButton(host);
    auto *remember = new QCheckBox(tr("&Remember my choice for this device"));
    box.setCheckBox(remember);
    box.exec();

    const bool toVm = (box.clickedButton() == vm);
    if (remember->isChecked()) {
        rememberChoice(rememberKey(d.vid, d.pid), toVm ? "vm" : "host");
        config_save();
    }
    if (toVm)
        connectToVm(d);

    prompting = false;
}

bool
UsbManager::connectToVm(const usbn_host_info_t &d, bool quiet)
{
    int  port = -1;
    char err[512];

    if (!usbn_present())
        return false;
    /* The first port with nothing on it and nothing assigned to it (the
       keyboard, mouse and hub keep theirs, plugged in or not). */
    for (int p = 0; p < USBN_PORTS; p++) {
        if (!usbn_port_busy(p) && !usbn_port_cfg[p][0]) {
            port = p;
            break;
        }
    }
    if (port < 0) {
        if (!quiet)
            QMessageBox::warning(parentWidget, tr("USB"), tr("All USB ports of the virtual machine are in use or assigned. Disconnect a device from the USB menu first, free a port in Settings > Ports > USB, or add a USB hub in Settings > Other peripherals."));
        return false;
    }

    usbn_device_t *dev = usbn_host_open(d.vid, d.pid, err, sizeof(err));
    if (!dev) {
        QMessageBox::warning(parentWidget, tr("USB"), tr("%1 could not be connected to the virtual machine: %2.").arg(label(d), QString::fromUtf8(err)));
        return false;
    }
    if (!usbn_attach(port, dev)) {
        dev->destroy(dev);
        return false;
    }
    snprintf(usbn_port_cfg[port], sizeof(usbn_port_cfg[port]), "%04x:%04x", d.vid, d.pid);
    config_save();
    return true;
}

void
UsbManager::disconnectFromVm(uint16_t vid, uint16_t pid)
{
    const int p = portOf(vid, pid);
    if (p < 0)
        return;
    usbn_detach(p);
    usbn_port_cfg[p][0] = '\0';
    config_save();
}

void
UsbManager::buildMenu()
{
    menu->clear();

    if (!usbn_present()) {
        auto *a = menu->addAction(tr("The USB controller starts at the next hard reset"));
        a->setEnabled(false);
        return;
    }
    if (!usbn_host_available()) {
        auto *a = menu->addAction(tr("This build has no USB passthrough"));
        a->setEnabled(false);
        return;
    }

    poll();
    auto *hdr = menu->addAction(tr("Host devices (checked = connected to the virtual machine)"));
    hdr->setEnabled(false);
    if (devices.isEmpty())
        menu->addAction(tr("(none found)"))->setEnabled(false);

    for (const auto &d : devices) {
        auto *a = menu->addAction(label(d));
        a->setCheckable(true);
        a->setChecked(portOf(d.vid, d.pid) >= 0);
        if ((d.speed == USBN_SPEED_HIGH) && !usbn_bus_high_speed())
            a->setToolTip(tr("A high-speed (USB 2.0) device: on this USB 1.1 controller it runs at full speed, as it would on a real USB 1.1 port."));
        const usbn_host_info_t info = d;
        connect(a, &QAction::triggered, this, [this, info](bool on) {
            if (on)
                connectToVm(info);
            else
                disconnectFromVm(info.vid, info.pid);
        });
    }
    menu->setToolTipsVisible(true);

#ifdef _WIN32
    menu->addSeparator();
    if (usbn_host_uses_usbdk())
        menu->addAction(tr("Capturing devices through UsbDk"))->setEnabled(false);
    else {
        auto *hint = menu->addAction(tr("Only devices with the WinUSB driver can be connected. Install UsbDk to connect any device..."));
        connect(hint, &QAction::triggered, this, [this]() {
            QMessageBox::information(parentWidget, tr("USB passthrough"),
                                     tr("To connect any USB device to the virtual machine the way VirtualBox and VMware do, install UsbDk "
                                        "(a signed driver from Red Hat / Daynix) once, then restart 86Box-Next2:\n\n"
                                        "https://github.com/daynix/UsbDk/releases\n\n"
                                        "Without it, only devices given the WinUSB driver (for example with Zadig) can be connected."));
        });
    }
#endif

    menu->addSeparator();
    auto *askAct = menu->addAction(tr("&Ask when a device is plugged into the host"));
    askAct->setCheckable(true);
    askAct->setChecked(config_get_int((char *) "USB", (char *) "ask_on_plug", 1));
    connect(askAct, &QAction::toggled, this, [](bool on) {
        if (on)
            config_delete_var((char *) "USB", (char *) "ask_on_plug");
        else
            config_set_int((char *) "USB", (char *) "ask_on_plug", 0);
        config_save();
    });
    auto *trace = menu->addAction(tr("&Log USB activity (usb_trace.txt)"));
    trace->setCheckable(true);
    trace->setChecked(usbn_trace_on);
    trace->setToolTip(tr("Writes every USB request and transfer, with timings, to usb_trace.txt next to the machine's 86box.cfg."));
    connect(trace, &QAction::toggled, this, [](bool on) { usbn_trace_enable(on); });
    auto *forget = menu->addAction(tr("&Forget remembered choices"));
    forget->setEnabled(config_get_string((char *) "USB", (char *) "remember", (char *) "")[0] != '\0');
    connect(forget, &QAction::triggered, this, []() {
        config_delete_var((char *) "USB", (char *) "remember");
        config_save();
    });
}
