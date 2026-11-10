#pragma once
#include <QObject>
namespace app {
class BackgroundSync : public QObject {
    Q_OBJECT
public:
    explicit BackgroundSync(QObject* parent = nullptr);
    bool start();
};
} // namespace app
