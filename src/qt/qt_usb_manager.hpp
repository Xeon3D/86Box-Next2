/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Host USB devices and the virtual machine, the way VMware and
 *             VirtualBox do it: new devices plugged into the host are noticed,
 *             and the user is asked whether to connect each one to the VM or
 *             leave it with the host (optionally remembered per device).  A
 *             USB menu lists every host device and connects or disconnects it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_USB_MANAGER_HPP
#define QT_USB_MANAGER_HPP

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

extern "C" {
#include <86box/usb_next.h>
}

class QAction;
class QMenu;
class QMenuBar;
class QTimer;
class QWidget;

class UsbManager : public QObject {
    Q_OBJECT

public:
    explicit UsbManager(QWidget *parent);

    /* The machine may have gained or lost its controller. */
    void refresh();

    /* A controller has the USB ports: a USB card's, or the motherboard's. */
    static bool fitted();
    /* A board whose southbridge has a USB controller (PIIX3/4, VIA, ALi,
       SiS, SMSC, STPC) with USB connectors -- for Settings, before the
       machine is built. */
    static bool machineHasOnboardUsb(int machineId);
    /* A bus slot's name: "Motherboard port 1", "USB card port 2", "Hub port 3"... */
    static QString portLabel(int slot);
    /* For Settings, before the machine is built: the ports a controller role
       (USBN_ROLE_*) has on this machine with this card, and whether a slot
       exists with this card and hub. */
    static int  machinePorts(int machineId, const int *cards, int role);   /* cards: USBN_CARDS of them */
    static bool portExistsFor(int machineId, const int *cards, int hub, int slot);

    /* The USB menu, behind the USB icon in the status bar. */
    QMenu  *usbMenu() const { return menu; }
    QString toolTip() const;

private:
    QWidget                   *parentWidget;
    QMenu                     *menu;
    QTimer                    *timer;
    QVector<usbn_host_info_t>  devices;
    QSet<QString>              known;
    bool                       firstPoll = true;
    bool                       prompting = false;

    void poll();
    void buildMenu();
    void ask(const usbn_host_info_t &d);
    bool connectToVm(const usbn_host_info_t &d, bool quiet = false);
    void disconnectFromVm(uint16_t vid, uint16_t pid);
    int  portOf(uint16_t vid, uint16_t pid) const;

    static QString key(const usbn_host_info_t &d);
    static QString label(const usbn_host_info_t &d);
    static QString rememberKey(uint16_t vid, uint16_t pid);
};

#endif
