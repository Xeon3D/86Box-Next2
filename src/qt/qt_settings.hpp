#ifndef QT_SETTINGS_HPP
#define QT_SETTINGS_HPP

#include "qt_pageddialog.hpp"

class SettingsMachine;
class SettingsDisplay;
class SettingsInput;
class SettingsSound;
class SettingsNetwork;
class SettingsPorts;
class SettingsStorageControllers;
class SettingsHarddisks;
class SettingsFloppyCDROM;
class SettingsOtherRemovable;
class SettingsOtherPeripherals;

class Settings : public PagedDialog {
    Q_OBJECT

public:
    explicit Settings(QWidget *parent = nullptr);
    ~Settings();
    void save(int soft);

    /* What the dialog holds now, or what is saved for a page not built. */
    int currentMachine() const;
    int currentHdc(int i) const;
    int currentSoundCard(int i) const;
    int currentScsiCard(int i) const;
    int currentPcmciaCard(int s) const;
    int currentUsbCard(int i = 0) const;
    void currentUsbCards(int *cards) const;   /* USBN_CARDS of them */
    /* 86Box-Next: the USB state as the pages have it now (Ports > USB). */
    int     currentUsbHub() const;
    bool    currentUsbKeyboard() const;
    bool    currentUsbMouse() const;
    int     currentUsbAssigned(const char *what) const;   /* the slot, -1 */
    QString currentUsbAssignment(int slot) const;
    bool    currentUsbPortExists(int slot) const;
    void    usbStateChanged();   /* a page changed it: the others follow */
    void    modemsChanged();     /* a COM port or the internal modem changed */
    void    showModemsPage(int com);   /* Ports' Configure on a modem: Modems, and its Configure */
    class SettingsPorts            *portsPage() const { return ports; }
    class SettingsOtherPeripherals *otherPage() const { return otherPeripherals; }

    static Settings *settings;
protected slots:
    void accept() override;
    void reject() override;

private:
    /* The pages in the order of the list beside them. */
    enum {
        PAGE_MACHINE = 0,
        PAGE_DISPLAY,
        PAGE_INPUT,
        PAGE_SOUND,
        PAGE_NETWORK,
        PAGE_PORTS,
        PAGE_STORAGE,
        PAGE_HARDDISKS,
        PAGE_FLOPPYCDROM,
        PAGE_REMOVABLE,
        PAGE_OTHER,
        PAGE_MODEMS,
        PAGE_INTEGRATION,
        PAGE_COUNT
    };

    void aboutToShowPage(int index) override;
    void placePage(int index, QWidget *page);
    void ensurePage(int index);
    void ensureAllPages();

    SettingsMachine            *machine;
    SettingsDisplay            *display;
    SettingsInput              *input;
    SettingsSound              *sound;
    SettingsNetwork            *network;
    SettingsPorts              *ports;
    SettingsStorageControllers *storageControllers;
    SettingsHarddisks          *harddisks;
    SettingsFloppyCDROM        *floppyCdrom;
    SettingsOtherRemovable     *otherRemovable;
    SettingsOtherPeripherals   *otherPeripherals;
    class SettingsModems *modems = nullptr;

    friend class SettingsMachine;
    friend class SettingsDisplay;
    friend class SettingsInput;
    friend class SettingsSound;
    friend class SettingsNetwork;
    friend class SettingsPorts;
    friend class SettingsStorageControllers;
    friend class SettingsHarddisks;
    friend class SettingsFloppyCDROM;
    friend class SettingsOtherRemovable;
    friend class SettingsOtherPeripherals;
    friend class DeviceConfig;
};

#endif // QT_SETTINGS_HPP
