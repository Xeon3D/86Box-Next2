#ifndef QT_SETTINGSOTHERPERIPHERALS_HPP
#define QT_SETTINGSOTHERPERIPHERALS_HPP

#include <QWidget>

extern "C" {
#include <86box/usb_next.h>
}
#ifdef USE_VFIO
#    include <tuple>

#    include <QHash>
#    include <QList>
#    include <QString>
#endif

class SettingsPcmcia;

namespace Ui {
class SettingsOtherPeripherals;
}

#ifdef USE_VFIO
class QProcess;
class VFIODeviceModel;
#endif

class SettingsOtherPeripherals : public QWidget {
    Q_OBJECT

    QWidget *modems      = nullptr;
    QWidget *integration = nullptr;
    class QComboBox *usbMore[USBN_CARDS] {};       /* cards 2 to 4 (index 1-3) */
    class QLabel    *usbMoreLabel[USBN_CARDS] {};
    class QComboBox *usbBox(int i) const;

public:
    explicit SettingsOtherPeripherals(QWidget *parent = nullptr);
    ~SettingsOtherPeripherals();

    int  changed();
    int  pcmciaCard(int s) const;   /* the PC Card chosen for socket s now */
    int  usbCard(int i = 0) const;  /* USB controller card i (0-3) chosen now */
    int  modemCard() const;         /* the internal modem chosen now */
    /* 86Box-Next: the Modems and Integration pages, built from this page's
       rows (their settings are saved with it). */
    QWidget *modemsPage() const { return modems; }
    QWidget *integrationPage() const { return integration; }

    void restore();
    void save(int soft);

public slots:
    void onCurrentMachineChanged(int machineId);

signals:
    void usbCardChanged();

private slots:
    void on_comboBoxUSB_currentIndexChanged(int index);
    void on_comboBoxRTC_currentIndexChanged(int index);
    void on_pushButtonConfigureRTC_clicked();
    void on_comboBoxIOBoard_currentIndexChanged(int index);
    void on_pushButtonConfigureIOBoard_clicked();
    void on_comboBoxModemCard_currentIndexChanged(int index);
    void on_pushButtonConfigureModemCard_clicked();
    void updateIOBoardHint();
    QString ioBoardDescription(int board);
    void updateUSBHint();

    void on_comboBoxMemExpCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard1_clicked();
    void on_comboBoxMemExpCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard2_clicked();
    void on_comboBoxMemExpCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard3_clicked();
    void on_comboBoxMemExpCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard4_clicked();

    void on_comboBoxIsaRomCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard1_clicked();
    void on_comboBoxIsaRomCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard2_clicked();
    void on_comboBoxIsaRomCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard3_clicked();
    void on_comboBoxIsaRomCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard4_clicked();

    void on_checkBoxUnitTester_stateChanged(int arg1);
    void on_pushButtonConfigureUT_clicked();
    void on_checkBoxGuestTools_stateChanged(int arg1);
    void on_pushButtonConfigureGuestTools_clicked();

    void on_checkBoxKeyCard_stateChanged(int arg1);
    void on_pushButtonConfigureKeyCard_clicked();

    void on_checkBoxSoftPower_stateChanged(int arg1);
    void on_pushButtonConfigureSoftPower_clicked();

private:
    Ui::SettingsOtherPeripherals *ui;
    int                           machineId { 0 };

    int                           memexp_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isarom_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isartc_cfg_changed         = 0;
    int                           unittester_cfg_changed     = 0;
    int                           guest_tools_cfg_changed    = 0;
    int                           novell_keycard_cfg_changed = 0;
    int                           softpower_cfg_changed      = 0;
    bool                          softpower_card_enabled     = false;

    SettingsCompleter            *scRTC;
    SettingsPcmcia               *pcmcia;   /* the PCMCIA tab */
    int                           io_board_cfg_changed = 0;
    int                           modem_card_cfg_changed = 0;

    SettingsCompleter            *scMemExpCard[4];
    SettingsCompleter            *scIsaRomCard[4];

#ifdef USE_VFIO
    /* Location, custom ROM enabled and custom ROM file of each device to pass through. */
    using VFIODeviceList = QList<std::tuple<QString, bool, QString>>;

    void           vfioEnumerate();
    void           vfioRestore();
    void           vfioRequestNames();
    void           vfioSetNames(const QHash<QString, QString> &names);
    void           vfioUpdateControls();
    VFIODeviceList vfioSelectedDevices() const;
    int            vfioChanged() const;
    void           vfioSave();

    VFIODeviceModel              *vfioModel;
    QProcess                     *vfioLspci          = nullptr;
    bool                          vfioUpdating       = false;
    bool                          vfioNamesRequested = false;
#endif
};

#endif // QT_SETTINGSOTHERPERIPHERALS_HPP
