#ifndef QT_SETTINGSPORTS_HPP
#define QT_SETTINGSPORTS_HPP

#include <QWidget>

extern "C" {
#include <86box/usb_next.h>
}

class QComboBox;
class QLabel;

#define SERIAL_MAX_UI 4

namespace Ui {
class SettingsPorts;
}

class SettingsPorts : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPorts(QWidget *parent = nullptr);
    ~SettingsPorts();

    int  changed();

    void restore();
    void save(int soft);

    /* 86Box-Next: Ports > USB, what is on each USB port, and the hub (1 the
       USB 1.1 one, 2 the USB 2.0 one, 0 none) chosen on one of them. */
    QString usbAssignment(int slot) const;

    /* 86Box-Next: the COM ports, for the Modems page. */
    int  comDevice(int i) const;
    bool comEnabled(int i) const;
    void setComDevice(int i, int device);
    void comConfigured(int i);
    int     usbHub() const;
    void    refreshUsb();

public slots:
    void onCurrentMachineChanged(int machineId);

private slots:
    void on_comboBoxLpt1_currentIndexChanged(int index);
    void on_pushButtonConfigureLpt1_clicked();
    void on_comboBoxLpt2_currentIndexChanged(int index);
    void on_pushButtonConfigureLpt2_clicked();
    void on_comboBoxLpt3_currentIndexChanged(int index);
    void on_pushButtonConfigureLpt3_clicked();
    void on_comboBoxLpt4_currentIndexChanged(int index);
    void on_pushButtonConfigureLpt4_clicked();

    void on_checkBoxParallel1_stateChanged(int state);
    void on_checkBoxParallel2_stateChanged(int state);
    void on_checkBoxParallel3_stateChanged(int state);
    void on_checkBoxParallel4_stateChanged(int state);

    void on_comboBoxCom1_currentIndexChanged(int index);
    void on_pushButtonConfigureCom1_clicked();
    void on_comboBoxCom2_currentIndexChanged(int index);
    void on_pushButtonConfigureCom2_clicked();
    void on_comboBoxCom3_currentIndexChanged(int index);
    void on_pushButtonConfigureCom3_clicked();
    void on_comboBoxCom4_currentIndexChanged(int index);
    void on_pushButtonConfigureCom4_clicked();

    void on_checkBoxSerial1_stateChanged(int state);
    void on_checkBoxSerial2_stateChanged(int state);
    void on_checkBoxSerial3_stateChanged(int state);
    void on_checkBoxSerial4_stateChanged(int state);

private:
    Ui::SettingsPorts *ui;
    int                machineId = 0;

    void updateLptPortTracking(int i);
    void buildUsbTab();
    void comChanged(int i);
    QLabel *comNote[SERIAL_MAX_UI] {};
    void usbChoiceChanged(int slot, const QString &what);

    void usbRow(class QVBoxLayout *layout, int slot, int indent, bool controllerPort);

    QWidget     *usbTab  = nullptr;
    QWidget     *usbTree = nullptr;   /* rebuilt by refreshUsb() */
    QString      usbCfg[USBN_PORTS];  /* the choices, kept for ports not shown now */
    int          usbHubType = 0;
    bool         usbFilling = false;

    int                lpt_device_cfg_changed[4] = { 0, 0, 0, 0 };
    int                com_device_cfg_changed[SERIAL_MAX_UI] = { 0 };

    SettingsCompleter *scLpt[4];
    SettingsCompleter *scCom[SERIAL_MAX_UI];
};

#endif // QT_SETTINGSPORTS_HPP
