#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QQmlEngine>
#include <QtQuickTest>
#include "app/RowsModel.h"

class AppRowsFixture : public rostrum::app::RowsModel
{
    Q_OBJECT
public:
    using RowsModel::RowsModel;
    Q_INVOKABLE void replaceRows(const QVariantList &rows) { setRows(rows); }
};

class Setup : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        qmlRegisterType<AppRowsFixture>("RostrumTest", 1, 0, "AppRowsFixture");
        KLocalizedString::setApplicationDomain("rostrum");
        KLocalization::setupLocalizedContext(engine);
    }
};
QUICK_TEST_MAIN_WITH_SETUP(rostrum_quick, Setup)
#include "tst_quick.moc"
