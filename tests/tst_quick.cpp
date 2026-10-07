#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QQmlEngine>
#include <QtQuickTest>

class Setup : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        KLocalizedString::setApplicationDomain("rostrum");
        KLocalization::setupLocalizedContext(engine);
    }
};
QUICK_TEST_MAIN_WITH_SETUP(rostrum_quick, Setup)
#include "tst_quick.moc"
