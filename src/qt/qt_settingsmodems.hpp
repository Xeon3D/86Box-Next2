/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Settings > Modems: the modems on the COM ports (the same choice as
 *             Ports > Serial ports, kept in step with it), the internal modem
 *             card, what each is set to, and -- while the machine runs -- what
 *             every modem is doing now.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_SETTINGSMODEMS_HPP
#define QT_SETTINGSMODEMS_HPP

#include <QWidget>

#define MODEMS_COM_MAX 4

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

class SettingsModems : public QWidget {
    Q_OBJECT

public:
    /* internalRow: Other peripherals' internal modem row, shown here. */
    explicit SettingsModems(QWidget *internalRow, QWidget *parent = nullptr);

    void refresh();   /* the choices as the pages have them now */

    /* What a modem's settings say (its line, number, speed, speaker), from its
       configuration section: inst the COM port's instance, 0 a device's own.
       Ports shows it under a COM port holding a modem. */
    static QString summary(const struct _device_ *dev, int inst);

    /* Ports' Configure on a COM port with a modem: this page's Configure. */
    void configureCom(int i);

private:
    void comChoice(int i);
    void refreshLive();

    QComboBox   *comBox[MODEMS_COM_MAX] {};
    QPushButton *comConf[MODEMS_COM_MAX] {};
    QLabel      *live    = nullptr;
    QTimer      *timer   = nullptr;
    bool         filling = false;
};

#endif
