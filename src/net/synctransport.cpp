#include "synctransport.h"

// This file exists solely so that the MOC for synctransport.h (which contains
// Q_OBJECT) gets compiled and linked. Without it, the linker cannot find
// SyncTransport::staticMetaObject, qt_metacall, or typeinfo.