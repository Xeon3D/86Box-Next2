/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Settings > Modems.  See qt_settingsmodems.hpp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <QComboBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/fifo8.h>
#include <86box/serial.h>
#include <86box/char.h>
#include <86box/char_modem.h>
#include <86box/modem_card.h>
}

#include "qt_deviceconfig.hpp"
#include "qt_settings.hpp"
#include "qt_settingsmodems.hpp"
#include "qt_settingsotherperipherals.hpp"
#include "qt_settingsports.hpp"


/* The modems a COM port can have, as char device numbers. */
static bool
is_modem(int chardev)
{
    const device_t *dev = char_get_device(chardev);

    return dev && dev->internal_name && !strncmp(dev->internal_name, "modem_", 6);
}

/* What a modem's settings say, from its configuration section (inst: the COM
   port's instance, 0 for a device's own). */
QString
SettingsModems::summary(const device_t *dev, int inst)
{
    QStringList parts;

    if (inst > 0)
        device_context_inst(dev, inst);
    else
        device_context(dev);

    const int   line  = device_get_config_int("line");
    const char *num   = device_get_config_string("phone_number");
    const char *exch  = device_get_config_string("exchange");
    const char *host  = device_get_config_string("host");
    const int   port  = device_get_config_int("host_port");
    const int   rate  = device_get_config_int("connect_rate");
    const int   spkr  = device_get_config_int("speaker");

    switch (line) {
        case CHAR_MODEM_LINE_TCP:
            if (host && host[0])
                parts << QObject::tr("Line: dials out to %1:%2").arg(host).arg(port);
            else
                parts << QObject::tr("Line: dials out to the TCP/IP host the guest dials as its number");
            break;
        case CHAR_MODEM_LINE_PHONE:
            parts << QObject::tr("Line: telephone network (isp-server) at %1").arg((exch && exch[0]) ? exch : CHAR_MODEM_EXCHANGE);
            parts << ((num && num[0]) ? QObject::tr("number %1").arg(num) : QObject::tr("the exchange gives the number"));
            break;
        default:
            parts << QObject::tr("Line: not connected");
            break;
    }
    if (rate > 0)
        parts << QObject::tr("reports %1 bps").arg(rate);
    parts << (spkr ? QObject::tr("speaker on") : QObject::tr("speaker off"));
    device_context_restore();
    return parts.join(QStringLiteral(", "));
}

SettingsModems::SettingsModems(QWidget *internalRow, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    /* The COM ports: each one's modem and its Configure. */
    auto *comGroup = new QGroupBox(tr("Modems on COM ports (external modems)"));
    auto *grid     = new QGridLayout(comGroup);
    for (int i = 0; i < MODEMS_COM_MAX; i++) {
        comBox[i]  = new QComboBox();
        comConf[i] = new QPushButton(tr("Configure"));
        comBox[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        grid->addWidget(new QLabel(tr("COM%1:").arg(i + 1)), i, 0);
        grid->addWidget(comBox[i], i, 1);
        grid->addWidget(comConf[i], i, 2);
        connect(comBox[i], qOverload<int>(&QComboBox::currentIndexChanged), this, [this, i](int) { comChoice(i); });
        connect(comConf[i], &QPushButton::clicked, this, [this, i] { configureCom(i); });
    }
    layout->addWidget(comGroup);

    /* The internal modem card (Other peripherals' row and Configure, saved
       with it). */
    auto *internalGroup  = new QGroupBox(tr("Internal modem (ISA card)"));
    auto *internalLayout = new QVBoxLayout(internalGroup);
    if (internalRow)
        internalLayout->addWidget(internalRow);
    layout->addWidget(internalGroup);

    /* What the running machine's modems do now. */
    auto *liveGroup  = new QGroupBox(tr("Now"));
    auto *liveLayout = new QVBoxLayout(liveGroup);
    live             = new QLabel();
    live->setWordWrap(true);
    live->setTextFormat(Qt::RichText);
    liveLayout->addWidget(live);
    layout->addWidget(liveGroup);
    layout->addStretch(1);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &SettingsModems::refreshLive);
    timer->start(1000);

    refresh();
}

/* The COM choices as Ports has them. */
void
SettingsModems::refresh()
{
    Settings      *s     = Settings::settings;
    SettingsPorts *ports = s ? s->portsPage() : nullptr;

    filling = true;
    for (int i = 0; i < MODEMS_COM_MAX; i++) {
        const int  cur     = ports ? ports->comDevice(i) : com_ports[i].device;
        const bool enabled = ports ? ports->comEnabled(i) : !!com_ports[i].enabled;

        comBox[i]->clear();
        comBox[i]->addItem(enabled ? tr("None") : tr("None (COM%1 is turned off in Ports)").arg(i + 1), 0);
        if ((cur != 0) && !is_modem(cur))
            comBox[i]->addItem(tr("%1 (another device, chosen in Ports)").arg(char_get_device(cur)->name), cur);
        for (int d = 1; char_get_device(d) != nullptr; d++) {
            if (is_modem(d))
                comBox[i]->addItem(char_get_device(d)->name, d);
        }
        const int row = comBox[i]->findData(cur);
        comBox[i]->setCurrentIndex((row >= 0) ? row : 0);
        comBox[i]->setEnabled(enabled);
        comConf[i]->setEnabled(enabled && is_modem(cur));
    }
    filling = false;
    refreshLive();
}

/* A COM port's modem settings: the device's Configure dialog, as Ports had
   it, the change marked for Ports (which owns the port). */
void
SettingsModems::configureCom(int i)
{
    SettingsPorts *ports = Settings::settings ? Settings::settings->portsPage() : nullptr;
    const int      dev   = comBox[i]->currentData().toInt();

    if (!is_modem(dev) || !comConf[i]->isEnabled())
        return;
    if (DeviceConfig::ConfigureDevice(char_get_device(dev), i + 1) && ports)
        ports->comConfigured(i);
}

/* Every modem the running machine has, and what it is doing. */
void
SettingsModems::refreshLive()
{
    QStringList rows;

    if (!isVisible())
        return;
    for (int com = 0; com < char_modem_slots(); com++) {
        if (!char_modem_present(com))
            continue;

        char    label[64], host[256], want[64], exch[128], number[64];
        int     port  = 0;
        QString state;

        char_modem_slot_label(com, label, sizeof(label));
        switch (char_modem_get_state(com)) {
            case CHAR_MODEM_IDLE:
                state = tr("on hook");
                break;
            case CHAR_MODEM_CALLING:
                state = tr("dialling, ringing or training");
                break;
            case CHAR_MODEM_ONLINE:
                state = tr("online (carrier up)");
                break;
            case CHAR_MODEM_VOICE:
                state = tr("in a voice call");
                break;
            case CHAR_MODEM_HANDSET:
                state = tr("the handset's call");
                break;
            default:
                state = tr("unknown");
                break;
        }

        QString line;
        switch (char_modem_get_line(com, host, sizeof(host), &port)) {
            case CHAR_MODEM_LINE_TCP:
                line = host[0] ? tr("line to %1:%2").arg(host).arg(port) : tr("line to any TCP/IP host dialled");
                break;
            case CHAR_MODEM_LINE_PHONE: {
                const int reg = char_modem_get_phone(com, want, sizeof(want), exch, sizeof(exch), number, sizeof(number));
                line          = (reg > 0) ? tr("telephone network, number %1").arg(number)
                                          : tr("telephone network, not registered (is isp-server running at %1?)").arg(exch);
                break;
            }
            default:
                line = tr("no line");
                break;
        }
        rows << QString("<b>%1</b> (%2): %3; %4").arg(QString(label).toHtmlEscaped(), QString(char_modem_name(com)).toHtmlEscaped(),
                                                        state.toHtmlEscaped(), line.toHtmlEscaped());
    }
    live->setText(rows.isEmpty() ? tr("No modem is running (the machine is off, or has none).") : rows.join("<br>"));
}

/* A modem chosen here goes to the COM port in Ports, as if chosen there. */
void
SettingsModems::comChoice(int i)
{
    SettingsPorts *ports = Settings::settings ? Settings::settings->portsPage() : nullptr;

    if (filling || !ports)
        return;
    ports->setComDevice(i, comBox[i]->currentData().toInt());
    QTimer::singleShot(0, this, [this] { refresh(); });
}
