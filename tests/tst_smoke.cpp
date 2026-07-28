#include <QtTest>

class SmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void testArithmetic()
    {
        QVERIFY(1 + 1 == 2);
    }
};

QTEST_MAIN(SmokeTest)
#include "tst_smoke.moc"
