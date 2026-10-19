#include <QAbstractNativeEventFilter>
#include <QByteArray>

#define result_t qintptr

class CocoaEventFilter : public QAbstractNativeEventFilter {
public:
    CocoaEventFilter() {};
    ~CocoaEventFilter();
    virtual bool nativeEventFilter(const QByteArray &eventType, void *message, result_t *result) override;

private:
    bool control_click_active = false;
    bool right_button_active  = false;
};
